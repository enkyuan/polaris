#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace shop {

namespace fs = std::filesystem;
using Time = std::int64_t;
constexpr Time day_ms = 86'400'000;
constexpr Time week_ms = 7 * day_ms;
constexpr Time session_gap_ms = 1'800'000;
constexpr Time bad_time = INT64_MIN;

// All ASCII IDs of at most nine non-NUL bytes fit exactly in 63 bits. Other IDs
// use a tagged reference to a stable string_view. Both paths preserve raw bytes.
using Key = std::uint64_t;
constexpr Key key_ref = Key{1} << 63;
constexpr std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

class Keys {
public:
    Key pack(std::string_view value);
private:
    std::deque<std::string_view> external_;
};

std::string_view key_view(Key key, char (&buffer)[9]);

struct KeyHash {
    std::size_t operator()(Key key) const {
        if (!(key & key_ref)) return mix(key);
        char buffer[9];
        return std::hash<std::string_view>{}(key_view(key, buffer));
    }
};

struct KeyEqual {
    bool operator()(Key a, Key b) const {
        if (a == b) return true;
        if (!(a & b & key_ref)) return false;
        char left[9], right[9];
        return key_view(a, left) == key_view(b, right);
    }
};

struct KeyLess {
    bool operator()(Key a, Key b) const {
        if (!((a | b) & key_ref)) return a < b;
        char left[9], right[9];
        return key_view(a, left) < key_view(b, right);
    }
};

class KeyIndex {
public:
    explicit KeyIndex(std::size_t expected = 0, unsigned hash_shift = 0);
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] Key get(Key key, Key fallback) const {
        return get_hashed(key, fallback, KeyHash{}(key));
    }
    [[nodiscard]] Key get_hashed(Key key, Key fallback, std::size_t hash) const {
        if (slots_.empty()) return fallback;
        const auto& entry = slots_[slot(key, hash)];
        return entry.key ? entry.value : fallback;
    }
    std::pair<Key, bool> insert(Key key, Key value) {
        if (slots_.empty()) grow();
        auto i = slot(key);
        if (slots_[i].key) return {slots_[i].value, false};
        if ((size_ + 1) * 4 >= slots_.size() * 3) {
            grow();
            i = slot(key);
        }
        slots_[i] = {key, value};
        ++size_;
        return {value, true};
    }
    template<typename F>
    void each(F&& f) const {
        for (const auto& entry : slots_) if (entry.key) f(entry.key, entry.value);
    }
private:
    struct Slot { Key key{}, value{}; };
    [[nodiscard]] std::size_t slot(Key key) const { return slot(key, KeyHash{}(key)); }
    [[nodiscard]] std::size_t slot(Key key, std::size_t hash) const {
        const auto mask = slots_.size() - 1;
        auto i = (hash >> hash_shift_) & mask;
        while (slots_[i].key && !KeyEqual{}(key, slots_[i].key)) i = (i + 1) & mask;
        return i;
    }
    void grow();
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    unsigned hash_shift_;
};

template<typename F>
void parallel(std::size_t count, unsigned threads, F&& work) {
    if (count == 0) return;
    std::atomic_size_t next{0};
    std::mutex mutex;
    std::exception_ptr failure;
    const auto worker = [&](unsigned id) {
        try {
            for (;;) {
                const auto job = next.fetch_add(1, std::memory_order_relaxed);
                if (job >= count) return;
                work(id, job);
            }
        } catch (...) {
            next.store(count, std::memory_order_relaxed);
            const std::lock_guard lock(mutex);
            if (!failure) failure = std::current_exception();
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned id = 1; id < std::min<std::size_t>(threads, count); ++id)
            pool.emplace_back(worker, id);
        worker(0);
    }
    if (failure) std::rethrow_exception(failure);
}

inline Time utc_day(Time t) {
    return t / day_ms - (t % day_ms < 0);
}

}  // namespace shop
