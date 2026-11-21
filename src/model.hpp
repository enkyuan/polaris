#pragma once

#include "csv.hpp"

namespace shop {

struct Campaign {
    std::string_view name;
    std::string_view channel;
};

class Catalog {
public:
    Catalog(const fs::path& data, Scanner scanner, Keys& keys);
    std::vector<std::string_view> categories;
    std::vector<Campaign> campaigns;
    std::uint32_t unknown{};
    KeyIndex products;
    KeyIndex ads;
private:
    Mapping products_file_;
    Mapping campaigns_file_;
};

class Aliases {
public:
    explicit Aliases(KeyIndex entries) : entries_(std::move(entries)) {
        const auto bits = std::bit_ceil(std::max<std::size_t>(64, entries_.size() * 12));
        membership_.resize(bits / 64);
        entries_.each([&](Key key, Key) {
            const auto i = KeyHash{}(key) & (bits - 1);
            membership_[i / 64] |= std::uint64_t{1} << (i % 64);
        });
    }
    [[nodiscard]] Key resolve(Key key, std::size_t hash) const {
        const auto i = hash & (membership_.size() * 64 - 1);
        if (!((membership_[i / 64] >> (i % 64)) & 1)) return key;
        return entries_.get_hashed(key, key, hash);
    }
private:
    KeyIndex entries_;
    std::vector<std::uint64_t> membership_;
};
Aliases read_links(const CsvFiles& files, Scanner scanner, Keys& keys);

struct Purchase {
    std::uint32_t price, category;
};

constexpr unsigned price_bits = 24;
constexpr std::uint32_t price_mask = (1U << price_bits) - 1;
inline constexpr Purchase unpack_purchase(std::uint32_t value) {
    return {value & price_mask, value >> price_bits};
}

struct Click {
    Key person, id;
    Time time;
    std::uint32_t campaign;
};

struct TimedEvent {
    Time time;
    std::uint32_t price, category;
};

struct OffsetEvent {
    std::uint32_t time, purchase;
};

inline Time absolute_time(const TimedEvent& event, Time) { return event.time; }
inline Time absolute_time(const OffsetEvent& event, Time origin) { return origin + event.time; }
inline Purchase event_purchase(const TimedEvent& event) { return {event.price, event.category}; }
inline Purchase event_purchase(const OffsetEvent& event) { return unpack_purchase(event.purchase); }

struct TimedClick {
    Time time;
    Key id;
    std::uint32_t campaign;
};

struct UserDay {
    Key person;
    std::int32_t day;
    std::uint32_t category;
    std::int64_t sessions{}, events{}, revenue{};
};

struct CampaignDay {
    std::int64_t sessions{}, purchases{}, revenue{}, assists{};
};

inline constexpr std::uint32_t organic = 0;
inline constexpr std::uint32_t none = UINT32_MAX;

class CampaignTotals {
public:
    CampaignTotals(const Clock& clock, std::size_t campaigns);
    void merge(const CampaignTotals& other);
    CampaignDay& at(std::uint32_t campaign, std::int32_t day) {
        return values_[campaign * day_count_ + static_cast<std::size_t>(day - first_day_)];
    }
    [[nodiscard]] std::int32_t first_day() const { return first_day_; }
    [[nodiscard]] std::size_t day_count() const { return day_count_; }
    [[nodiscard]] const std::vector<CampaignDay>& values() const { return values_; }
private:
    std::int32_t first_day_;
    std::size_t day_count_;
    std::vector<CampaignDay> values_;
};

}  // namespace shop
