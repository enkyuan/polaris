#include "csv_kernels.hpp"
#include "ingest.hpp"

namespace shop {
namespace {

template<typename Kernel>
void parse_events(Range range, const Clock& clock, const Catalog& catalog,
                  const EventLayout& layout, Input& input) {
    // About 68 years on either side of the manifest start fit inline.
    const auto order_origin = clock.begin / 1000 - (Time{1} << 31);
    scan_csv<7>(range, Kernel{}, [&](const auto& fields) {
        if (fields[0].empty()) return;
        const auto id = input.keys.pack(fields[0]);
        const auto received = clock.ingest_seconds(fields[3]);
        if (received == bad_time) return;
        auto position = Input::no_position;
        if (!fields[1].empty() && !fields[5].empty()) {
            const auto kind = fields[4];
            const bool purchase = kind == "purchase";
            if (purchase || kind == "view" || kind == "cart") {
                const auto time = clock.parse(fields[2]);
                if (time >= clock.begin && time < clock.end && received * 1000 - time <= day_ms) {
                    const auto price = purchase ? price_cents(fields[6]) : 0;
                    if (!purchase || price) {
                        const auto user = input.keys.pack(fields[1]);
                        OffsetEvent value{layout.store_time(time, input), 0};
                        if (purchase) {
                            const auto category = static_cast<std::uint32_t>(
                                catalog.products.get(input.keys.pack(fields[5]), catalog.unknown));
                            value.purchase = layout.store_purchase({price, category}, input);
                        }
                        position = input.append(user, value);
                    }
                }
            }
        }
        // Invalid payloads remain eligible to win the earliest-ingest comparison.
        input.remember(id, received - order_origin, position);
    });
}

template<typename Kernel>
void parse_clicks(Range range, const Clock& clock, const Catalog& catalog,
                  const Aliases& aliases, Input& input) {
    scan_csv<4>(range, Kernel{}, [&](const auto& fields) {
        if (fields[0].empty() || fields[1].empty() || fields[2].empty()) return;
        const auto campaign = catalog.ads.get(input.keys.pack(fields[2]), none);
        if (campaign == none) return;
        const auto time = clock.parse(fields[3]);
        if (time < clock.begin - week_ms || time >= clock.end) return;
        const auto user = input.keys.pack(fields[1]);
        const auto [person, bin] = person_bin(aliases, user);
        input.clicks[bin].push_back(
            {person, input.keys.pack(fields[0]), time, static_cast<std::uint32_t>(campaign)});
    });
}

template<typename Kernel>
std::uint64_t structure(Range range, unsigned columns) {
    std::uint64_t subtotal = 0;
    const auto add = [&](const auto& fields) { subtotal += fields[0].size(); };
    if (columns == 7) scan_csv<7>(range, Kernel{}, add);
    else if (columns == 4) scan_csv<4>(range, Kernel{}, add);
    else scan_csv<2>(range, Kernel{}, add);
    return subtotal;
}

template<typename Kernel>
constexpr IngestKernels kernels{parse_events<Kernel>, parse_clicks<Kernel>, structure<Kernel>};

}  // namespace

#if defined(POLARIS_AVX512)
const IngestKernels avx512_ingest = kernels<StaticScanner<avx512_markers, 64>>;
#elif defined(POLARIS_AVX2)
const IngestKernels avx2_ingest = kernels<StaticScanner<avx2_markers, 64>>;
#else
const IngestKernels scalar_ingest = kernels<StaticScanner<scalar_markers, 64>>;
#if defined(__aarch64__)
const IngestKernels neon_ingest = kernels<StaticScanner<neon_markers, 16, 2>>;
#endif
#endif

}  // namespace shop
