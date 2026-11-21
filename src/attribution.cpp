#include "attribution.hpp"

namespace shop {

CampaignTotals::CampaignTotals(const Clock& clock, std::size_t campaigns)
    : first_day_(static_cast<std::int32_t>(utc_day(clock.begin))),
      day_count_(static_cast<std::size_t>(utc_day(clock.end - 1) - first_day_ + 1)),
      values_(campaigns * day_count_) {}

void CampaignTotals::merge(const CampaignTotals& other) {
    for (std::size_t i = 0; i < values_.size(); ++i) {
        auto& a = values_[i];
        const auto& b = other.values_[i];
        a.sessions += b.sessions;
        a.purchases += b.purchases;
        a.revenue += b.revenue;
        a.assists += b.assists;
    }
}

Timeline::Timeline(std::size_t categories, std::size_t campaigns)
    : category_cents_(categories), covered_(campaigns) {}

template<typename Record>
void Timeline::run(Key person, std::span<const Record> events, std::span<const TimedClick> clicks, Time origin,
                   UserWriter& output, CampaignTotals& campaigns) {
    sessions_.clear();
    auto previous = absolute_time(events.front(), origin);
    UserDay day{person, static_cast<std::int32_t>(utc_day(previous)), none};
    const auto write_day = [&] {
        std::int64_t most = 0;
        for (const auto category : touched_categories_) {
            const auto cents = category_cents_[category];
            if (cents > most || (cents == most && category < day.category)) {
                most = cents;
                day.category = category;
            }
            category_cents_[category] = 0;
        }
        touched_categories_.clear();
        output.append(day);
    };

    for (const auto& event : events) {
        const auto time = absolute_time(event, origin);
        if (sessions_.empty() || time - previous > session_gap_ms) {
            const auto date = static_cast<std::int32_t>(utc_day(time));
            if (date != day.day) {
                write_day();
                day = {day.person, date, none};
            }
            ++day.sessions;
            sessions_.push_back({time, 0, day.day});
        }
        previous = time;
        ++day.events;
        const auto [price, category] = event_purchase(event);
        if (price) {
            day.revenue += price;
            sessions_.back().revenue += price;
            auto& cents = category_cents_[category];
            if (!cents) touched_categories_.push_back(category);
            cents += price;
        }
    }
    write_day();
    attribute(clicks, campaigns);
}

template void Timeline::run(Key, std::span<const TimedEvent>, std::span<const TimedClick>, Time,
                            UserWriter&, CampaignTotals&);
template void Timeline::run(Key, std::span<const OffsetEvent>, std::span<const TimedClick>, Time,
                            UserWriter&, CampaignTotals&);

void Timeline::attribute(std::span<const TimedClick> clicks, CampaignTotals& totals) {
    std::size_t next_click = 0;
    const TimedClick* latest = nullptr;
    for (const auto& session : sessions_) {
        while (next_click < clicks.size() && clicks[next_click].time <= session.start) {
            const auto* candidate = &clicks[next_click++];
            // Clicks with the same timestamp are ordered by ID.
            if (!latest || candidate->time > latest->time) latest = candidate;
        }
        const auto campaign = latest && session.start - latest->time <= week_ms ? latest->campaign : organic;
        auto& daily = totals.at(campaign, session.day);
        ++daily.sessions;
        daily.purchases += session.revenue != 0;
        daily.revenue += session.revenue;
    }
    if (clicks.empty()) return;

    // Each stored end is the first session index on the next UTC day.
    auto day_end = static_cast<std::uint32_t>(sessions_.size());
    for (auto i = day_end; i > 0; --i) {
        if (i < sessions_.size() && sessions_[i - 1].day != sessions_[i].day) day_end = i;
        sessions_[i - 1].day_end = day_end;
    }
    std::uint32_t lo = 0, hi = 0;
    for (const auto& click : clicks) {
        while (lo < sessions_.size() && sessions_[lo].start < click.time) ++lo;
        if (lo == sessions_.size()) break;
        hi = std::max(hi, lo);
        const auto end = click.time + week_ms;
        while (hi < sessions_.size() && sessions_[hi].start <= end) ++hi;
        auto& covered_end = covered_[click.campaign];
        auto start = std::max(lo, covered_end);
        if (start >= hi) continue;
        if (!covered_end) touched_campaigns_.push_back(click.campaign);
        covered_end = hi;
        while (start < hi) {
            const auto& session = sessions_[start];
            const auto next = std::min(hi, session.day_end);
            totals.at(click.campaign, session.day).assists += next - start;
            start = next;
        }
    }
    for (const auto campaign : touched_campaigns_) covered_[campaign] = 0;
    touched_campaigns_.clear();
}

}  // namespace shop
