#pragma once

#include "common.hpp"

#include <array>

namespace shop {

struct Range {
    const char* begin;
    const char* end;
};

class Mapping {
public:
    explicit Mapping(const fs::path& path);
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] std::string_view bytes() const { return {data_, size_}; }
private:
    const char* data_ = "";
    std::size_t size_ = 0;
};

class CsvFiles {
public:
    explicit CsvFiles(const fs::path& directory);
    std::vector<Range> ranges;
private:
    std::vector<std::unique_ptr<Mapping>> files_;
};

struct Scanner {
    enum class Kind { scalar, neon, avx2, avx512 };
    struct Bits { std::uint64_t delimiters, quotes; };
    using Markers = Bits (*)(const char*);
    Markers markers;
    unsigned bytes;
    unsigned shift;
    Kind kind;
};

Scanner select_scanner();
Range after_header(std::string_view bytes);

// Every comma and LF is structural, even in a malformed row. Quote state
// belongs to a single field, and a malformed row cannot consume the next row.
template<std::size_t N, typename Kernel, typename F>
void scan_csv(Range range, Kernel scanner, F&& emit) {
    std::array<std::string_view, N> fields;
    const char* field_start = range.begin;
    std::size_t count = 0;
    bool bad = false;

    const auto marker = [&](const char* p, char ch) {
        if (ch == '"') {
            bad = true;
            return false;
        }
        if (count < N && !bad) {
            const auto length = static_cast<std::size_t>(p - field_start);
            const bool open = length && *field_start == '"';
            const bool close = length && p[-1] == '"';
            bad = open != close || (open && length == 1);
            if (!bad) fields[count] = {field_start + open, length - 2 * open};
        }
        ++count;
        field_start = p + (p != range.end);
        if (ch != '\n') return false;
        const bool complete = count == N && !bad;
        count = 0;
        bad = false;
        return complete;
    };

    const auto delimiter = [](char ch) { return ch == ',' || ch == '\n'; };
    const auto stride = 1U << scanner.shift;
    const auto last = std::uint64_t{1} << ((scanner.bytes - 1) * stride);
    auto p = range.begin;
    while (range.end - p >= scanner.bytes) {
        const auto [delimiters, quotes] = scanner.markers(p);
        auto adjacent = (delimiters << stride) | (delimiters >> stride);
        if ((quotes & 1) && (p == range.begin || delimiter(p[-1]))) adjacent |= 1;
        if ((quotes & last) && (p + scanner.bytes == range.end || delimiter(p[scanner.bytes])))
            adjacent |= last;
        // Quotes touching a field boundary are checked when the delimiter is
        // consumed. An interior quote can invalidate the row immediately.
        auto mask = delimiters | (quotes & ~adjacent);
        while (mask) {
            const auto offset = std::countr_zero(mask) >> scanner.shift;
            if (marker(p + offset, p[offset])) emit(fields);
            mask &= mask - 1;
        }
        p += scanner.bytes;
    }
    for (; p < range.end; ++p) {
        if (delimiter(*p) || (*p == '"' && p != range.begin && p + 1 != range.end &&
                             !delimiter(p[-1]) && !delimiter(p[1]))) {
            if (marker(p, *p)) emit(fields);
        }
    }
    if ((field_start != range.end || count) && marker(range.end, '\n')) emit(fields);
}

class Clock {
public:
    explicit Clock(const fs::path& manifest);
    [[nodiscard]] Time parse(std::string_view value) const;
    [[nodiscard]] Time ingest_seconds(std::string_view value) const;
    Time begin{}, end{};
private:
    [[nodiscard]] int civil_day(const char* p) const;
    struct CivilDay {
        std::uint64_t prefix;
        std::uint16_t suffix;
        int utc_day;
    };
    std::array<CivilDay, 32> nearby_{};
};

std::uint32_t price_cents(std::string_view value);

}  // namespace shop
