#pragma once

#include "common.hpp"

namespace shop {

void run_pipeline(const fs::path& data, const fs::path& out, unsigned threads, bool timings);
void benchmark_scan(const fs::path& data, unsigned threads);

}  // namespace shop
