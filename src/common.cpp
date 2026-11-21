#include "common.hpp"

namespace shop {
namespace {

constexpr std::uint64_t high_bits = 0x8080808080808080ULL;
constexpr std::uint64_t low_bits = 0x0101010101010101ULL;

std::uint64_t pack_eight(std::uint64_t x) {
    x = __builtin_bswap64(x);
    const auto a = (x & 0x7f007f007f007f00ULL) >> 1;
    const auto b = x & 0x007f007f007f007fULL;
    x = a | b;
    const auto c = (x & 0x3fff00003fff0000ULL) >> 2;
    const auto d = x & 0x00003fff00003fffULL;
    x = c | d;
    return ((x & 0x0fffffff00000000ULL) >> 4) | (x & 0x000000000fffffffULL);
}

}  // namespace

Key Keys::pack(std::string_view value) {
    if (value.empty()) return 0;
    if (value.size() == 9) {
        std::uint64_t first;
        std::memcpy(&first, value.data(), sizeof(first));
        const auto last = static_cast<unsigned char>(value[8]);
        if (((first | (first - low_bits)) & high_bits) == 0 && last && last < 128)
            return (pack_eight(first) << 7) | last;
    } else if (value.size() <= 8) {
        Key result = 0;
        unsigned shift = 56;
        for (unsigned char c : value) {
            if (!c || c >= 128) {
                external_.push_back(value);
                return reinterpret_cast<Key>(&external_.back()) | key_ref;
            }
            result |= static_cast<Key>(c) << shift;
            shift -= 7;
        }
        return result;
    }
    external_.push_back(value);
    return reinterpret_cast<Key>(&external_.back()) | key_ref;
}

std::string_view key_view(Key key, char (&buffer)[9]) {
    if (key & key_ref)
        return *reinterpret_cast<const std::string_view*>(key & ~key_ref);
    std::size_t size = 0;
    for (int shift = 56; shift >= 0; shift -= 7) {
        const char c = static_cast<char>((key >> shift) & 127);
        if (!c) break;
        buffer[size++] = c;
    }
    return {buffer, size};
}

KeyIndex::KeyIndex(std::size_t expected, unsigned hash_shift) : hash_shift_(hash_shift) {
    if (expected) slots_.resize(std::bit_ceil(expected + expected / 2 + 1));
}

void KeyIndex::grow() {
    auto previous = std::move(slots_);
    slots_.resize(std::max<std::size_t>(16, 2 * previous.size()));
    for (const auto& entry : previous) if (entry.key) slots_[slot(entry.key)] = entry;
}

}  // namespace shop
