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

struct EventCandidate {
    Key id, user;
    Time ingest;
    std::uint32_t time, purchase;
};

struct Input {
    Keys keys;
    Bins<EventCandidate> candidates;
    Bins<Click> clicks;
    std::vector<Time> wide_times;
    std::vector<Purchase> wide_purchases;
};

// Keep the common candidate at 32 bytes. Wider manifests and catalogs use
// side arrays, so the compact representation never restricts input values.
struct CandidateLayout {
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
    void (*events)(Range, const Clock&, const Catalog&, const CandidateLayout&, Input&);
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
