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

void deduplicate(std::size_t bin, std::vector<Input>& inputs) {
    std::size_t count = 0;
    for (const auto& in : inputs) count += in.versions[bin].size();
    if (!count) return;
    const auto bits = std::bit_ceil(std::max<std::size_t>(64, count * 16));
    const auto mask = bits - 1;
    std::vector<std::uint64_t> seen(bits / 64), repeat(bits / 64);
    const auto fingerprint = [&](Key id) { return (KeyHash{}(id) >> partition_bits) & mask; };
    const auto visit = [&](auto&& f) {
        for (auto& in : inputs) {
            for (const auto& row : in.versions[bin]) f(in, row);
        }
    };
    visit([&](const Input&, const EventVersion& row) {
        const auto slot = fingerprint(row.id);
        const auto bit = std::uint64_t{1} << (slot % 64);
        repeat[slot / 64] |= seen[slot / 64] & bit;
        seen[slot / 64] |= bit;
    });
    KeyIndex ambiguous(count / 8);
    struct Winner {
        Input* in;
        VersionLocation location;
    };
    std::vector<Winner> winners;
    winners.reserve(count / 8);
    visit([&](Input& in, const EventVersion& row) {
        const auto slot = fingerprint(row.id);
        if (!((repeat[slot / 64] >> (slot % 64)) & 1)) return;
        const auto [i, inserted] = ambiguous.insert(row.id, winners.size());
        const auto location = in.locate(row);
        if (inserted) {
            winners.push_back({&in, location});
            return;
        }
        auto& winner = winners[i];
        // Only this ID partition can discard these payload locations.
        if (location.order < winner.location.order) {
            winner.in->discard(winner.location.position);
            winner = {&in, location};
        } else {
            in.discard(location.position);
        }
    });
    for (auto& in : inputs) std::vector<EventVersion>().swap(in.versions[bin]);
}

struct RawGroup {
    Key key;
    std::size_t count{}, end{};
};

class RawUsers {
public:
    explicit RawUsers(std::size_t event_count)
        : ids_(std::max<std::size_t>(32, event_count / 16), partition_bits) {
        groups.reserve(event_count / 24);
    }

    std::uint32_t add(Key key) {
        const auto [group, inserted] = ids_.insert(key, groups.size());
        if (inserted) groups.push_back({key});
        return static_cast<std::uint32_t>(group);
    }

    std::vector<RawGroup> groups;
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
struct PersonSlice {
    Key person;
    std::span<Record> events;
};

template<typename Record>
struct EventBatch {
    RecordBuffer<Record> records;
    // Disjoint slices stay in one allocation while their canonical
    // partitions sort and consume them independently.
    Bins<PersonSlice<Record>> destinations;
};

template<typename Record>
void group_raw_events(std::size_t bin, std::vector<Input>& input, const EventLayout& layout,
                      const Aliases& aliases, std::vector<std::uint32_t>& assignments,
                      EventBatch<Record>& batch) {
    std::size_t count = 0;
    for (const auto& source : input) count += source.events[bin].size();
    if (!count) return;

    RawUsers people(count);
    auto& groups = people.groups;
    assignments.clear();
    assignments.reserve(count);
    for (const auto& source : input) {
        for (const auto& event : source.events[bin]) {
            if (event.value.time == none) continue;
            const auto group = people.add(event.user);
            ++groups[group].count;
            assignments.push_back(group);
        }
    }
    if (assignments.empty()) {
        for (auto& source : input) std::vector<RawEvent>().swap(source.events[bin]);
        return;
    }

    auto* records = batch.records.resize_for_write(assignments.size());
    std::size_t offset = 0;
    for (auto& group : groups) {
        const auto [person, target] = person_bin(aliases, group.key);
        batch.destinations[target].push_back({person, {records + offset, group.count}});
        group.end = offset;
        offset += group.count;
    }

    std::size_t i = 0;
    for (auto& source : input) {
        for (const auto& event : source.events[bin]) {
            if (event.value.time == none) continue;
            auto& pos = groups[assignments[i++]].end;
            if constexpr (std::same_as<Record, OffsetEvent>) {
                records[pos++] = event.value;
            } else {
                const auto [price, category] = layout.read_purchase(event.value.purchase, source);
                records[pos++] = {layout.read_time(event.value.time, source), price, category};
            }
        }
        std::vector<RawEvent>().swap(source.events[bin]);
    }
}

template<typename Record>
struct TimelineGroup {
    PersonSlice<Record> first;
    std::uint32_t more = none;
    std::size_t events = 0, clicks = 0, click_end = 0;
};

template<typename Record>
struct GroupingBuffers {
    std::vector<std::uint32_t> click_groups;
    RecordBuffer<TimedClick> clicks;
    std::vector<Record> merged_events, sort_scratch;
};

template<typename Record>
void write_person_bin(std::size_t bin, std::vector<Input>& input, std::vector<EventBatch<Record>>& batches,
                      const Clock& clock, Timeline& timeline, GroupingBuffers<Record>& buffers,
                      UserWriter& writer, CampaignTotals& totals) {
    std::size_t slice_count = 0, click_count = 0;
    for (const auto& batch : batches) slice_count += batch.destinations[bin].size();
    for (const auto& source : input) click_count += source.clicks[bin].size();
    if (!slice_count) {
        for (auto& source : input) std::vector<Click>().swap(source.clicks[bin]);
        return;
    }

    struct ExtraSlice { std::span<Record> events; std::uint32_t next; };
    KeyIndex people(slice_count, partition_bits);
    std::vector<TimelineGroup<Record>> groups;
    std::vector<ExtraSlice> extra;
    groups.reserve(slice_count);
    for (const auto& batch : batches) {
        for (const auto& slice : batch.destinations[bin]) {
            const auto [index, inserted] = people.insert(slice.person, groups.size());
            if (inserted) {
                groups.push_back({slice, none, slice.events.size()});
            } else {
                auto& group = groups[index];
                extra.push_back({slice.events, group.more});
                group.more = static_cast<std::uint32_t>(extra.size() - 1);
                group.events += slice.events.size();
            }
        }
    }

    auto& click_groups = buffers.click_groups;
    click_groups.clear();
    click_groups.reserve(click_count);
    for (const auto& source : input) {
        for (const auto& click : source.clicks[bin]) {
            const auto group = static_cast<std::uint32_t>(people.get(click.person, none));
            if (group != none) ++groups[group].clicks;
            click_groups.push_back(group);
        }
    }
    std::size_t click_offset = 0;
    for (auto& group : groups) {
        group.click_end = click_offset;
        click_offset += group.clicks;
    }

    auto* click_records = buffers.clicks.resize_for_write(click_offset);
    std::size_t i = 0;
    for (auto& source : input) {
        for (const auto& click : source.clicks[bin]) {
            const auto group = click_groups[i++];
            if (group != none) click_records[groups[group].click_end++] = {click.time, click.id, click.campaign};
        }
        std::vector<Click>().swap(source.clicks[bin]);
    }

    auto& merged = buffers.merged_events;
    for (const auto& group : groups) {
        auto events = group.first.events;
        if (group.more != none) {
            merged.clear();
            merged.reserve(group.events);
            merged.insert(merged.end(), events.begin(), events.end());
            for (auto next = group.more; next != none; next = extra[next].next) {
                const auto span = extra[next].events;
                merged.insert(merged.end(), span.begin(), span.end());
            }
            events = merged;
        }
        const std::span clicks(click_records + group.click_end - group.clicks, group.clicks);
        std::sort(clicks.begin(), clicks.end(), [](const auto& a, const auto& b) {
            if (a.time != b.time) return a.time < b.time;
            return KeyLess{}(a.id, b.id);
        });
        timeline.run(group.first.person, sort_events(events, buffers.sort_scratch, clock), clicks, clock.begin,
                     writer, totals);
    }
}

template<typename Record>
void finish(std::vector<Input>& input, const Aliases& aliases, const EventLayout& layout,
            const Clock& clock, const Catalog& catalog, const fs::path& out,
            unsigned threads, Phases& phases) {
    parallel(partition_count, threads, [&](auto, auto bin) { deduplicate(bin, input); });
    phases.mark("dedup");

    std::vector<EventBatch<Record>> batches(partition_count);
    {
        std::vector<std::vector<std::uint32_t>> assignments(threads);
        parallel(partition_count, threads, [&](auto worker, auto bin) {
            group_raw_events(bin, input, layout, aliases, assignments[worker], batches[bin]);
        });
    }
    phases.mark("group and resolve event users");

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
        write_person_bin(bin, input, batches, clock, timelines[worker], buffers[worker], *writers[worker], totals[worker]);
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
    const EventLayout layout{clock.begin, clock.end - clock.begin > UINT32_MAX,
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
            for (auto& bin : worker.versions) bin.reserve(capacity);
            for (auto& bin : worker.events) bin.reserve(capacity);
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
    phases.mark("parse and partition");

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
