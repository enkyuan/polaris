#include "csv.hpp"

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csv_kernels.hpp"

namespace shop {
namespace {

const char* after_line(const char* p, const char* end) {
    const auto* lf = static_cast<const char*>(std::memchr(p, '\n', end - p));
    return lf ? lf + 1 : end;
}


constexpr bool digit(char c) { return static_cast<unsigned>(c - '0') <= 9; }
constexpr auto ascii_zero = 0x3030303030303030ULL;

constexpr bool eight_ascii_digits(std::uint64_t bytes) {
    return (((bytes + 0x4646464646464646ULL) | (bytes - ascii_zero)) & 0x8080808080808080ULL) == 0;
}

Time decimal_millis_13(const char* p) {
    std::uint64_t digits;
    std::memcpy(&digits, p, sizeof(digits));
    if (!eight_ascii_digits(digits)) return bad_time;
    digits -= ascii_zero;
    digits = (digits * 10 + (digits >> 8)) & 0x00ff00ff00ff00ffULL;
    digits = (digits * 100 + (digits >> 16)) & 0x0000ffff0000ffffULL;
    const auto prefix = static_cast<std::uint32_t>(digits * 10'000 + (digits >> 32));
    unsigned suffix = 0;
    for (int i = 8; i < 13; ++i) {
        if (!digit(p[i])) return bad_time;
        suffix = suffix * 10 + static_cast<unsigned>(p[i] - '0');
    }
    return static_cast<Time>(prefix) * 100'000 + suffix;
}

constexpr int two_digits(const char* p) {
    const auto a = static_cast<unsigned>(p[0] - '0');
    const auto b = static_cast<unsigned>(p[1] - '0');
    return a <= 9 && b <= 9 ? static_cast<int>(10 * a + b) : -1;
}

constexpr bool leap(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr int civil_to_days(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

std::array<int, 3> days_to_civil(int day) {
    const int z = day + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const int doe = z - era * 146097;
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    const int month = mp + (mp < 10 ? 3 : -9);
    return {yoe + era * 400 + (month <= 2), month, doy - (153 * mp + 2) / 5 + 1};
}

int parse_day(const char* p) {
    if (p[4] != '-' || p[7] != '-') return INT32_MIN;
    const auto century = two_digits(p), yy = two_digits(p + 2);
    const auto month = two_digits(p + 5), day = two_digits(p + 8);
    if (century < 0 || yy < 0 || month < 1 || month > 12 || day < 1) return INT32_MIN;
    constexpr std::array<int, 12> month_length{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const auto year = century * 100 + yy;
    if (day > month_length[month - 1] + (month == 2 && leap(year))) return INT32_MIN;
    return civil_to_days(year, month, day);
}

std::uint64_t date_prefix(const char* p) {
    std::uint64_t a;
    std::memcpy(&a, p, sizeof(a));
    return a;
}

std::uint16_t date_suffix(const char* p) {
    std::uint16_t b;
    std::memcpy(&b, p + 8, sizeof(b));
    return b;
}

int slot(const char* p) {
    return (static_cast<unsigned char>(p[8]) * 10 + static_cast<unsigned char>(p[9])) & 31;
}

int seconds_of_day(const char* p) {
    // The HH:MM:SS bytes fit one load. Verify the colons, substitute '0' in
    // their lanes, and validate/materialize the three digit pairs together.
    constexpr auto separators = (0xffULL << 16) | (0xffULL << 40);
    constexpr auto colons = (0x3aULL << 16) | (0x3aULL << 40);
    std::uint64_t raw;
    std::memcpy(&raw, p + 11, sizeof(raw));
    const auto ascii = (raw & ~separators) | (ascii_zero & separators);
    if (p[10] != 'T' || (raw & separators) != colons || !eight_ascii_digits(ascii)) return -1;
    const auto digits = ascii - ascii_zero;
    const auto pairs = digits * 10 + (digits >> 8);
    const auto hour = static_cast<int>(pairs & 255);
    const auto min = static_cast<int>((pairs >> 24) & 255);
    const auto sec = static_cast<int>((pairs >> 48) & 255);
    if (hour >= 24 || min >= 60 || sec >= 60) return -1;
    return hour * 3600 + min * 60 + sec;
}

std::string_view manifest_field(std::string_view json, std::string_view field) {
    const auto key = '"' + std::string(field) + '"';
    auto i = json.find(key);
    if (i != json.npos) i = json.find(':', i + key.size());
    if (i != json.npos) i = json.find('"', i + 1);
    if (i != json.npos) {
        const auto close = json.find('"', i + 1);
        if (close != json.npos) return json.substr(i + 1, close - i - 1);
    }
    throw std::runtime_error("missing manifest value " + std::string(field));
}

}  // namespace

Mapping::Mapping(const fs::path& path) {
    const auto fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), path.string());
    struct stat info{};
    if (fstat(fd, &info) < 0) {
        const auto error = errno;
        close(fd);
        throw std::system_error(error, std::generic_category(), path.string());
    }
    size_ = static_cast<std::size_t>(info.st_size);
    if (size_) {
        const auto* bytes = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        const auto error = errno;
        close(fd);
        if (bytes == MAP_FAILED) throw std::system_error(error, std::generic_category(), path.string());
        data_ = static_cast<const char*>(bytes);
        // Mapping access stays sequential within each scheduled range.
    } else {
        close(fd);
    }
}

Mapping::~Mapping() {
    if (size_) munmap(const_cast<char*>(data_), size_);
}

Range after_header(std::string_view bytes) {
    const auto* end = bytes.data() + bytes.size();
    return {after_line(bytes.data(), end), end};
}

CsvFiles::CsvFiles(const fs::path& directory) {
    if (!fs::exists(directory)) return;
    constexpr std::size_t chunk_bytes = 2 * 1024 * 1024;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".csv" || !entry.is_regular_file()) continue;
        auto file = std::make_unique<Mapping>(entry.path());
        auto [begin, end] = after_header(file->bytes());
        while (static_cast<std::size_t>(end - begin) > chunk_bytes) {
            const auto* next = after_line(begin + chunk_bytes - 1, end);
            ranges.push_back({begin, next});
            begin = next;
        }
        if (begin != end) ranges.push_back({begin, end});
        files_.push_back(std::move(file));
    }
}

Scanner select_scanner() {
    const auto* env = std::getenv("CSV_SIMD");
    const std::string_view mode = env ? env : "";
    using Kind = Scanner::Kind;
    if (mode == "scalar") return {scalar_markers, 64, 0, Kind::scalar};
#if defined(__x86_64__)
    if (mode != "avx2" && __builtin_cpu_supports("avx512bw")) return {avx512_markers, 64, 0, Kind::avx512};
    if (__builtin_cpu_supports("avx2")) return {avx2_markers, 64, 0, Kind::avx2};
#elif defined(__aarch64__)
    return {neon_markers, 16, 2, Kind::neon};
#endif
    return {scalar_markers, 64, 0, Kind::scalar};
}

Clock::Clock(const fs::path& manifest) {
    const Mapping mapping(manifest);
    const auto start_seconds = ingest_seconds(manifest_field(mapping.bytes(), "window_start"));
    const auto end_seconds = ingest_seconds(manifest_field(mapping.bytes(), "window_end"));
    if (start_seconds == bad_time || end_seconds == bad_time || end_seconds <= start_seconds)
        throw std::runtime_error("invalid time window");
    begin = start_seconds * 1000;
    end = end_seconds * 1000;
    if (utc_day(end) - utc_day(begin) <= 20) {
        for (auto day = utc_day(begin) - 8; day <= utc_day(end) + 2; ++day) {
            const auto [year, month, date] = days_to_civil(static_cast<int>(day));
            if (year < 0 || year > 9999) continue;
            const char prefix[10] = {
                char('0' + year / 1000), char('0' + year / 100 % 10),
                char('0' + year / 10 % 10), char('0' + year % 10), '-',
                char('0' + month / 10), char('0' + month % 10), '-',
                char('0' + date / 10), char('0' + date % 10)
            };
            nearby_[slot(prefix)] = {date_prefix(prefix), date_suffix(prefix), static_cast<int>(day)};
        }
    }
}

int Clock::civil_day(const char* p) const {
    const auto& cached = nearby_[slot(p)];
    return cached.prefix == date_prefix(p) && cached.suffix == date_suffix(p)
        ? cached.utc_day : parse_day(p);
}

Time Clock::ingest_seconds(std::string_view value) const {
    if (value.size() != 20 || value[4] != '-' || value[19] != 'Z') return bad_time;
    const auto* p = value.data();
    const auto secs = seconds_of_day(p);
    if (secs < 0) return bad_time;
    const auto day = civil_day(p);
    return day == INT32_MIN ? bad_time : static_cast<Time>(day) * (day_ms / 1000) + secs;
}

Time Clock::parse(std::string_view value) const {
    const auto* p = value.data();
    if (value.size() == 13) return decimal_millis_13(p);
    if ((value.size() == 20 || value.size() == 25) && p[4] == '-') {
        const auto secs = seconds_of_day(p);
        if (secs < 0) return bad_time;
        const auto day = civil_day(p);
        if (day == INT32_MIN) return bad_time;
        int offset = 0;
        if (value.size() == 20) {
            if (p[19] != 'Z') return bad_time;
        } else {
            if ((p[19] != '+' && p[19] != '-') || p[22] != ':') return bad_time;
            const auto hour = two_digits(p + 20), min = two_digits(p + 23);
            if (hour < 0 || hour >= 24 || min < 0 || min >= 60) return bad_time;
            offset = (hour * 60 + min) * (p[19] == '+' ? 60 : -60);
        }
        return static_cast<Time>(day) * day_ms + (secs - offset) * 1000;
    }
    // Strip leading zeros before bounding the length. Valid F3 values beyond
    // int64 cannot be inside the four-digit-year manifest window.
    for (; !value.empty() && value.front() == '0'; value.remove_prefix(1)) {}
    if (value.size() > 18) return bad_time;
    Time result = 0;
    for (const auto c : value) {
        if (!digit(c)) return bad_time;
        result = result * 10 + c - '0';
    }
    return p == value.data() && value.empty() ? bad_time : result;
}

std::uint32_t price_cents(std::string_view value) {
    for (; !value.empty() && value.front() == '0'; value.remove_prefix(1)) {}
    if (value.size() > 8) return 0;
    std::uint32_t result = 0;
    for (const auto c : value) {
        if (!digit(c)) return 0;
        result = result * 10 + c - '0';
    }
    return result <= 10'000'000 ? result : 0;
}

}  // namespace shop
