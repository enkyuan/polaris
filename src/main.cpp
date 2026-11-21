#include "pipeline.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

const char* env(const char* name, const char* fallback) {
    if (const auto* value = std::getenv(name); value && *value) return value;
    return fallback;
}

unsigned worker_count() {
    unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
#if defined(__linux__)
    cpu_set_t affinity;
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0)
        hardware = std::max(1, CPU_COUNT(&affinity));
#endif
    const std::string_view specified = env("NCPU", "");
    auto count = 0U;
    const auto parsed = std::from_chars(specified.data(), specified.data() + specified.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != specified.data() + specified.size() || !count)
        return std::min(hardware, 8U);
    return std::min({count, hardware, 256U});
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto threads = worker_count();
        const auto* data = env("DATA_DIR", "/data");
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "--scan-only") shop::benchmark_scan(data, threads);
        else shop::run_pipeline(data, env("OUTPUT_DIR", "/output"), threads,
                                std::string_view(env("POLARIS_TIMINGS", "")) == "1");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shopping-sessions: " << error.what() << '\n';
        return 1;
    }
}
