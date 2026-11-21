#pragma once

#include "model.hpp"

namespace shop {

constexpr std::size_t partition_count = 256;
constexpr unsigned partition_bits = std::countr_zero(partition_count);

inline std::size_t bin_for(Key key) { return KeyHash{}(key) & (partition_count - 1); }

inline std::pair<Key, std::size_t> person_bin(const Aliases& aliases, Key user) {
    const auto hash = KeyHash{}(user);
    const auto person = aliases.resolve(user, hash);
    const auto bin = person == user ? hash & (partition_count - 1) : bin_for(person);
    return {person, bin};
}

template<typename T>
using Bins = std::array<std::vector<T>, partition_count>;

struct RawEvent {
    Key user;
    OffsetEvent value;
};

// Deduplication reads ID and version metadata. Event payloads stay in
// raw-user buckets until grouping.
struct EventVersion {
    Key id;
    std::uint32_t order, position;
};

struct VersionLocation {
    Time order;
    std::size_t position;
};

struct Input {
    static constexpr auto no_position = SIZE_MAX;

    Keys keys;
    Bins<EventVersion> versions;
    Bins<RawEvent> events;
    Bins<Click> clicks;
    std::vector<VersionLocation> wide_versions;
    std::vector<Time> wide_times;
    std::vector<Purchase> wide_purchases;

    std::size_t append(Key user, OffsetEvent value) {
        const auto bin = bin_for(user);
        auto& bucket = events[bin];
        const auto position = (bucket.size() << partition_bits) | bin;
        bucket.push_back({user, value});
        return position;
    }

    void remember(Key id, Time order, std::size_t position) {
        auto& bucket = versions[bin_for(id)];
        if (static_cast<std::uint64_t>(order) < none &&
            (position < none || position == no_position)) {
            bucket.push_back({id, static_cast<std::uint32_t>(order), static_cast<std::uint32_t>(position)});
        } else {
            const auto index = static_cast<std::uint32_t>(wide_versions.size());
            wide_versions.push_back({order, position});
            bucket.push_back({id, none, index});
        }
    }

    [[nodiscard]] VersionLocation locate(const EventVersion& version) const {
        if (version.order == none) return wide_versions[version.position];
        return {version.order, version.position == none ? no_position : version.position};
    }

    void discard(std::size_t position) {
        if (position == no_position) return;
        events[position & (partition_count - 1)][position >> partition_bits].value.time = none;
    }
};
static_assert(sizeof(EventVersion) == 16 && sizeof(RawEvent) == 16);

// Side arrays preserve the full range of times and categories.
struct EventLayout {
    Time origin;
    bool wide_time, wide_purchase;

    std::uint32_t store_time(Time time, Input& in) const {
        if (!wide_time) return static_cast<std::uint32_t>(time - origin);
        const auto index = static_cast<std::uint32_t>(in.wide_times.size());
        in.wide_times.push_back(time);
        return index;
    }

    std::uint32_t store_purchase(Purchase value, Input& in) const {
        if (!wide_purchase) return (value.category << price_bits) | value.price;
        in.wide_purchases.push_back(value);
        return static_cast<std::uint32_t>(in.wide_purchases.size());
    }

    Time read_time(std::uint32_t value, const Input& in) const {
        return wide_time ? in.wide_times[value] : origin + value;
    }

    Purchase read_purchase(std::uint32_t value, const Input& in) const {
        return !value ? Purchase{} : wide_purchase ? in.wide_purchases[value - 1]
                                                   : unpack_purchase(value);
    }
};

struct IngestKernels {
    void (*events)(Range, const Clock&, const Catalog&, const EventLayout&, Input&);
    void (*clicks)(Range, const Clock&, const Catalog&, const Aliases&, Input&);
    std::uint64_t (*structure)(Range, unsigned);
};

extern const IngestKernels scalar_ingest;
#if defined(__x86_64__)
extern const IngestKernels avx2_ingest, avx512_ingest;
#elif defined(__aarch64__)
extern const IngestKernels neon_ingest;
#endif

inline const IngestKernels& select_ingest(Scanner::Kind kind) {
#if defined(__x86_64__)
    if (kind == Scanner::Kind::avx512) return avx512_ingest;
    if (kind == Scanner::Kind::avx2) return avx2_ingest;
#elif defined(__aarch64__)
    if (kind == Scanner::Kind::neon) return neon_ingest;
#else
    (void)kind;
#endif
    return scalar_ingest;
}

}  // namespace shop
