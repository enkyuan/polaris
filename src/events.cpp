#include "attribution.hpp"
#include "pipeline.hpp"
#include "ingest.hpp"

#include <chrono>
#include <concepts>
#include <future>
#include <iostream>
#include <numeric>

namespace shop {
namespace {

template<typename Record>
struct PersonEvent {
    Key person;
    Record value;
};

template<typename Record>
struct Output {
    Bins<PersonEvent<Record>> events;
};

static_assert(sizeof(PersonEvent<OffsetEvent>) == 16 && sizeof(OffsetEvent) == 8);

class Phases {
public:
    explicit Phases(bool enabled) : enabled_(enabled) {}
    void mark(std::string_view label) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration<double>(now - last_).count();
        std::cerr << label << ' ' << seconds << " s\n";
        last_ = now;
    }
private:
    bool enabled_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

template<typename Record>
void deduplicate(std::size_t bin, std::vector<Input>& inputs, Output<Record>& output,
                 const Aliases& aliases, const CandidateLayout& layout) {
    std::size_t count = 0;
    for (const auto& in : inputs) count += in.candidates[bin].size();
    if (!count) return;
    const auto bits = std::bit_ceil(std::max<std::size_t>(64, count * 16));
    const auto mask = bits - 1;
    std::vector<std::uint64_t> seen(bits / 64), repeat(bits / 64);
    const auto fingerprint = [&](Key id) { return (KeyHash{}(id) >> partition_bits) & mask; };
    const auto visit = [&](auto&& f) {
        for (const auto& in : inputs) {
            for (const auto& row : in.candidates[bin]) f(in, row);
        }
    };
    visit([&](const Input&, const EventCandidate& row) {
        const auto slot = fingerprint(row.id);
        const auto bit = std::uint64_t{1} << (slot % 64);
        repeat[slot / 64] |= seen[slot / 64] & bit;
        seen[slot / 64] |= bit;
    });
    const auto emit = [&](const Input& in, const EventCandidate& row) {
        if (row.time == none) return;
        const auto [person, target] = person_bin(aliases, row.user);
        if constexpr (std::same_as<Record, OffsetEvent>) {
            output.events[target].push_back({person, {row.time, row.purchase}});
        } else {
            const auto [price, category] = layout.read_purchase(row.purchase, in);
            output.events[target].push_back({person, {layout.read_time(row.time, in), price, category}});
        }
    };
    KeyIndex ambiguous(count / 8);
    struct Winner { const Input* in; const EventCandidate* row; };
    std::vector<Winner> winners;
    winners.reserve(count / 8);
    visit([&](const Input& in, const EventCandidate& row) {
        const auto slot = fingerprint(row.id);
        if (((repeat[slot / 64] >> (slot % 64)) & 1) == 0) {
            emit(in, row);
        } else {
            const auto [i, inserted] = ambiguous.insert(row.id, winners.size());
            if (inserted) winners.push_back({&in, &row});
            else if (row.ingest < winners[i].row->ingest) winners[i] = {&in, &row};
        }
    });
    for (const auto& [in, row] : winners) emit(*in, *row);
    for (auto& in : inputs) std::vector<EventCandidate>().swap(in.candidates[bin]);
}

struct PersonGroup {
    Key key;
    std::size_t events{}, clicks{};
    std::size_t event_end{}, click_end{};
};

class BinPeople {
public:
    explicit BinPeople(std::size_t event_count)
        : ids_(std::max<std::size_t>(32, event_count / 16), partition_bits) {
        groups.reserve(event_count / 24);
    }

    std::uint32_t find(Key key) const { return static_cast<std::uint32_t>(ids_.get(key, none)); }

    std::uint32_t add(Key key) {
        const auto [group, inserted] = ids_.insert(key, groups.size());
        if (inserted) groups.push_back({key});
        return static_cast<std::uint32_t>(group);
    }

    std::vector<PersonGroup> groups;
private:
    KeyIndex ids_;
};

template<typename Record>
std::span<const Record> sort_events(std::span<Record> events,
                                   std::vector<Record>& scratch, const Clock& clock) {
    if (events.size() < 16'384) {
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        return events;
    }
    // Large timelines use integer radix passes instead of millions of comparisons.
    constexpr unsigned digit_bits = 10;
    constexpr std::size_t digits = 1 << digit_bits;
    scratch.resize(events.size());
    auto source = events;
    auto target = std::span(scratch);
    const auto width = static_cast<unsigned>(std::bit_width(static_cast<std::uint64_t>(clock.end - clock.begin)));
    for (unsigned shift = 0; shift < width; shift += digit_bits) {
        const auto digit = [&](const Record& event) {
            const auto offset = static_cast<std::uint64_t>(absolute_time(event, clock.begin) - clock.begin);
            return (offset >> shift) & (digits - 1);
        };
        std::array<std::size_t, digits> offsets{};
        for (const auto& event : source) ++offsets[digit(event)];
        std::size_t before = 0;
        for (auto& offset : offsets) {
            before += offset;
            offset = before - offset;
        }
        for (const auto& event : source) target[offsets[digit(event)]++] = event;
        std::swap(source, target);
    }
    return source;
}

template<typename T>
class RecordBuffer {
public:
    T* resize_for_write(std::size_t count) {
        if (!data_ || count > capacity_) {
            data_ = std::make_unique_for_overwrite<T[]>(count);
            capacity_ = count;
        }
        return data_.get();
    }
private:
    std::unique_ptr<T[]> data_;
    std::size_t capacity_ = 0;
};

template<typename Record>
struct GroupingBuffers {
    std::vector<std::uint32_t> event_groups, click_groups;
    RecordBuffer<Record> events;
    RecordBuffer<TimedClick> clicks;
    std::vector<Record> sort_scratch;
};

template<typename Record>
void process_bin(std::size_t bin, std::vector<Input>& input, std::vector<Output<Record>>& output,
                 const Clock& clock, Timeline& timeline, GroupingBuffers<Record>& buffers,
                 UserWriter& writer, CampaignTotals& totals) {
    std::size_t event_count = 0, click_count = 0;
    for (const auto& source : output) event_count += source.events[bin].size();
    for (const auto& source : input) click_count += source.clicks[bin].size();
    if (!event_count) {
        for (auto& source : input) std::vector<Click>().swap(source.clicks[bin]);
        return;
    }

    BinPeople people(event_count);
    auto& groups = people.groups;
    auto& event_groups = buffers.event_groups;
    auto& click_groups = buffers.click_groups;
    event_groups.clear();
    click_groups.clear();
    event_groups.reserve(event_count);
    click_groups.reserve(click_count);
    for (const auto& source : output) {
        for (const auto& event : source.events[bin]) {
            const auto group = people.add(event.person);
            ++groups[group].events;
            event_groups.push_back(group);
        }
    }
    for (const auto& source : input) {
        for (const auto& click : source.clicks[bin]) {
            const auto group = people.find(click.person);
            if (group != none) ++groups[group].clicks;
            click_groups.push_back(group);
        }
    }

    std::size_t event_offset = 0, click_offset = 0;
    for (auto& group : groups) {
        group.event_end = event_offset;
        group.click_end = click_offset;
        event_offset += group.events;
        click_offset += group.clicks;
    }

    auto* event_records = buffers.events.resize_for_write(event_offset);
    auto* click_records = buffers.clicks.resize_for_write(click_offset);
    std::size_t i = 0;
    for (auto& source : output) {
        for (const auto& event : source.events[bin]) {
            auto& pos = groups[event_groups[i++]].event_end;
            event_records[pos++] = event.value;
        }
        std::vector<PersonEvent<Record>>().swap(source.events[bin]);
    }
    i = 0;
    for (auto& source : input) {
        for (const auto& click : source.clicks[bin]) {
            const auto group = click_groups[i++];
            if (group != none) click_records[groups[group].click_end++] = {click.time, click.id, click.campaign};
        }
        std::vector<Click>().swap(source.clicks[bin]);
    }

    for (const auto& group : groups) {
        const std::span events(event_records + group.event_end - group.events, group.events);
        const std::span clicks(click_records + group.click_end - group.clicks, group.clicks);
        std::sort(clicks.begin(), clicks.end(), [](const auto& a, const auto& b) {
            if (a.time != b.time) return a.time < b.time;
            return KeyLess{}(a.id, b.id);
        });
        timeline.run(group.key, sort_events(events, buffers.sort_scratch, clock), clicks, clock.begin, writer, totals);
    }
}

template<typename Record>
void finish(std::vector<Input>& input, const Aliases& aliases, const CandidateLayout& layout,
            const Clock& clock, const Catalog& catalog, const fs::path& out,
            unsigned threads, Phases& phases) {
    std::vector<Output<Record>> output(threads);
    parallel(partition_count, threads, [&](auto worker, auto bin) {
        deduplicate(bin, input, output[worker], aliases, layout);
    });
    phases.mark("dedup and person partition");

    const auto user_dir = out / "user_daily", campaign_dir = out / "campaign_daily";
    fs::create_directories(user_dir);
    fs::create_directories(campaign_dir);
    std::vector<std::unique_ptr<UserWriter>> writers;
    std::vector<CampaignTotals> totals;
    std::vector<Timeline> timelines;
    std::vector<GroupingBuffers<Record>> buffers(threads);
    for (unsigned i = 0; i < threads; ++i) {
        writers.push_back(std::make_unique<UserWriter>(user_dir / ("part-" + std::to_string(i) + ".parquet"), catalog));
        totals.emplace_back(clock, catalog.campaigns.size());
        timelines.emplace_back(catalog.categories.size(), catalog.campaigns.size());
    }
    parallel(partition_count, threads, [&](auto worker, auto bin) {
        process_bin(bin, input, output, clock, timelines[worker], buffers[worker], *writers[worker], totals[worker]);
    });
    parallel(threads, threads, [&](auto, auto i) { writers[i]->close(i == 0); });
    phases.mark("sort, aggregate and write users");
    for (unsigned i = 1; i < threads; ++i) totals[0].merge(totals[i]);
    write_campaigns(campaign_dir / "part-0.parquet", catalog, totals[0]);
    phases.mark("write campaigns");
}

}  // namespace

void run_pipeline(const fs::path& data, const fs::path& out, unsigned threads, bool timings) {
    Phases phases(timings);
    const auto scanner = select_scanner();
    if (timings) {
        constexpr std::array names{"scalar", "neon", "avx2", "avx512bw"};
        std::cerr << "CSV scanner " << names[static_cast<unsigned>(scanner.kind)] << '\n';
    }
    const auto& ingest = select_ingest(scanner.kind);
    const Clock clock(data / "manifest.json");
    Keys lookup_keys;
    const Catalog catalog(data, scanner, lookup_keys);
    const CandidateLayout layout{clock.begin, clock.end - clock.begin > UINT32_MAX,
                                 catalog.categories.size() > 256};
    const CsvFiles link_files(data / "links");
    const CsvFiles event_files(data / "events"), click_files(data / "clicks");
    phases.mark("catalog and file ranges");

    std::vector<Input> input(threads);
    const auto event_ranges = event_files.ranges.size();
    std::size_t event_bytes = 0;
    for (const auto range : event_files.ranges) event_bytes += range.end - range.begin;
    // Budget for typical rows with headroom for uneven work assignments.
    // This only estimates allocation size. The vectors still grow as needed.
    constexpr std::size_t bytes_per_event = 64;
    const auto capacity = event_bytes / (threads * partition_count * bytes_per_event);
    if (capacity >= 128) {
        for (auto& worker : input) {
            for (auto& bin : worker.candidates) bin.reserve(capacity);
        }
    }
    Keys linked_keys;
    std::promise<Aliases> link_result;
    const auto link_ready = link_result.get_future().share();
    parallel(1 + event_ranges + click_files.ranges.size(), threads, [&](auto worker, auto job) {
        if (!job) {
            try {
                link_result.set_value(read_links(link_files, scanner, linked_keys));
            } catch (...) {
                link_result.set_exception(std::current_exception());
                throw;
            }
        } else if (--job < event_ranges) {
            ingest.events(event_files.ranges[job], clock, catalog, layout, input[worker]);
        } else {
            ingest.clicks(click_files.ranges[job - event_ranges], clock, catalog, link_ready.get(), input[worker]);
        }
    });
    phases.mark("parse and link IDs");

    const auto& aliases = link_ready.get();
    if (layout.wide_time || layout.wide_purchase)
        finish<TimedEvent>(input, aliases, layout, clock, catalog, out, threads, phases);
    else
        finish<OffsetEvent>(input, aliases, layout, clock, catalog, out, threads, phases);
}

void benchmark_scan(const fs::path& data, unsigned threads) {
    const auto scanner = select_scanner();
    const auto& ingest = select_ingest(scanner.kind);
    const CsvFiles events(data / "events"), clicks(data / "clicks"), links(data / "links");
    const auto n_events = events.ranges.size(), n_clicks = clicks.ranges.size();
    std::vector<std::uint64_t> checksums(threads);
    parallel(n_events + n_clicks + links.ranges.size(), threads, [&](auto worker, auto job) {
        if (job < n_events) checksums[worker] += ingest.structure(events.ranges[job], 7);
        else if (job < n_events + n_clicks)
            checksums[worker] += ingest.structure(clicks.ranges[job - n_events], 4);
        else checksums[worker] += ingest.structure(links.ranges[job - n_events - n_clicks], 2);
    });
    std::cerr << "scan checksum " << std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{}) << '\n';
}

}  // namespace shop
