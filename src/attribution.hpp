#pragma once

#include "parquet.hpp"

#include <span>

namespace shop {

class Timeline {
public:
    explicit Timeline(std::size_t categories, std::size_t campaigns);
    template<typename Record>
    void run(Key person, std::span<const Record> events, std::span<const TimedClick> clicks, Time origin,
             UserWriter& output, CampaignTotals& campaigns);
private:
    struct Session {
        Time start;
        std::int64_t revenue = 0;
        std::int32_t day;
        std::uint32_t day_end = 0;
    };
    void attribute(std::span<const TimedClick> clicks, CampaignTotals& totals);
    std::vector<Session> sessions_;
    std::vector<std::int64_t> category_cents_;
    std::vector<std::uint32_t> touched_categories_;
    std::vector<std::uint32_t> covered_;
    std::vector<std::uint32_t> touched_campaigns_;
};

}  // namespace shop
