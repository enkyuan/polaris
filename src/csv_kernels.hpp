#pragma once

#include "csv.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace shop {

inline Scanner::Bits scalar_markers(const char* p) {
    Scanner::Bits bits{};
    for (unsigned i = 0; i < 64; ++i) {
        if (p[i] == ',' || p[i] == '\n') bits.delimiters |= std::uint64_t{1} << i;
        if (p[i] == '"') bits.quotes |= std::uint64_t{1} << i;
    }
    return bits;
}

#if defined(__x86_64__)
__attribute__((target("avx2")))
inline Scanner::Bits avx2_markers(const char* p) {
    const auto comma = _mm256_set1_epi8(',');
    const auto lf = _mm256_set1_epi8('\n');
    const auto quote = _mm256_set1_epi8('"');
    Scanner::Bits bits{};
    for (unsigned i = 0; i < 64; i += 32) {
        const auto v = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(p + i));
        const auto delimiters = _mm256_or_si256(_mm256_cmpeq_epi8(v, comma), _mm256_cmpeq_epi8(v, lf));
        const auto d = static_cast<std::uint32_t>(_mm256_movemask_epi8(delimiters));
        const auto q = static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, quote)));
        bits.delimiters |= static_cast<std::uint64_t>(d) << i;
        bits.quotes |= static_cast<std::uint64_t>(q) << i;
    }
    return bits;
}

__attribute__((target("avx512bw")))
inline Scanner::Bits avx512_markers(const char* p) {
    const auto v = _mm512_loadu_si512(p);
    return {_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(',')) |
            _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n')),
            _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('"'))};
}
#elif defined(__aarch64__)
inline Scanner::Bits neon_markers(const char* p) {
    const auto v = vld1q_u8(reinterpret_cast<const std::uint8_t*>(p));
    const auto mask = [](uint8x16_t matches) {
        const auto nibbles = vshrn_n_u16(vreinterpretq_u16_u8(matches), 4);
        return vget_lane_u64(vreinterpret_u64_u8(nibbles), 0) & 0x1111111111111111ULL;
    };
    return {mask(vorrq_u8(vceqq_u8(v, vdupq_n_u8(',')), vceqq_u8(v, vdupq_n_u8('\n')))),
            mask(vceqq_u8(v, vdupq_n_u8('"')))};
}
#endif

template<Scanner::Markers Mark, unsigned Bytes, unsigned Shift = 0>
struct StaticScanner {
    static constexpr auto markers = Mark;
    static constexpr auto bytes = Bytes;
    static constexpr auto shift = Shift;
};

}  // namespace shop
