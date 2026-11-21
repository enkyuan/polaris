# Shopping Sessions performance

The target is the 1.24 s practice score. Local timings use the native ARM64
release executable on an Apple M1 with eight workers. They include process
startup and closing the Parquet output. The Linux/amd64 image is checked
locally under emulation. A score or runtime on a native N2 machine requires
a run on that hardware.

## Leaderboard baseline

The reported practice score for the published candidate
`eeb4e126ec482f257ffbb58422c215caf147a2f3` is **1.47 s**. Reaching the
last reported lead of 1.24 s requires saving 0.23 s, or 15.6% of this
runtime. Taking the lead requires a score below 1.24 s.

The 1.12 s local M1 median used a generated dataset. The leaderboard score
covers the hidden dataset from container start to exit. The leaderboard
result alone cannot identify the time spent in each phase.

## Implementation decisions

| Change | Why it helps and how correctness is preserved |
| --- | --- |
| Filter alias lookups | The bitmap contains every noncanonical alias. A clear bit proves the ID is absent. Set bits always go through exact equality in the original alias table. Reuse the user hash for partitioning when their canonical ID is unchanged. |
| Select the scanner once per range | Compile the event and click readers separately for scalar, NEON, AVX2, and AVX-512BW where supported. The chosen implementation can inline delimiter masks and field decoding. The paths share the same CSV and validation logic. |
| Keep event payloads compact | A partitioned event carries an 8-byte person key and an 8-byte timestamp and purchase payload. The timeline stores just the 8-byte payload. This is down from 24 and 16 bytes respectively. The compact path uses a window span of at most `UINT32_MAX` milliseconds and at most 256 category IDs, including `unknown`. Other inputs take the wide path. |
| Decode ISO clock fields in one word | Load `HH:MM:SS` together, verify both colons and all six ASCII digits, then check hour, minute, and second ranges. Calendar validation, UTC offset conversion, and late-event rules still apply. |
| Reuse grouping buffers per worker | Reuse the group indexes, sorted records, and radix scratch across person partitions. Each worker initializes the portion used by its current partition before reading it. |

Event deduplication still compares exact IDs and the earliest ingest time
across users and files, including candidates with invalid payloads. The
per-person session and campaign-assist rules use the same timestamp
comparisons as before.

## Experiments

Seed 17 has 20 million generated events, 5 million generated clicks, and
1.920 GB of event, click, and link CSVs. The variants were interleaved after
a read-only and a full warmup. Each row shows three complete runs.

| Variant | Runs (s) | Median (s) |
| --- | --- | ---: |
| Previous workspace build | 1.08, 1.07, 1.37 | 1.08 |
| Alias filter | 1.07, 1.05, 1.14 | 1.07 |
| Alias filter and range-level scanner | 1.07, 1.01, 1.53 | 1.07 |
| Above plus compact post-dedup records | 1.08, 1.05, 0.99 | 1.05 |
| Above plus an interleaved two-bit dedup bitmap | 1.12, 1.03, 1.69 | 1.12 |
| Above plus grouped ISO clock decoding | 1.02, 1.10, 1.00 | 1.02 |

The selected build keeps the original separate seen and repeated bitmaps.
The interleaved bitmap spent a little more CPU time in the seed-17 trial
and showed no repeatable end-to-end benefit. In the earlier investigation,
separate vectors for candidate IDs and payloads also slowed the full run
despite reducing the bytes read in the first dedup pass. The selected
layout shrinks the records themselves.

The seed-41 batch ran while the host was under substantial memory pressure.
Use it for the exact-output comparison and treat its wall times as
machine-load observations. The M1 batch cannot establish the effect of the
AVX2 and AVX-512BW changes on the scorer.

## Selected build measurements

After the large-sample checks and cleanup, I reran the previous and selected
builds in alternating order on seed 17. The table reports all three
end-to-end runs per build.

| Build | Runs (s) | Median wall (s) | Median user CPU (s) |
| --- | --- | ---: | ---: |
| Previous | 1.26, 1.27, 1.34 | 1.27 | 6.89 |
| Selected | 1.10, 1.12, 1.24 | **1.12** | **6.26** |

The selected build cut the median wall time by about 12% in this batch.
In an earlier batch while the local host was under memory pressure, the
medians were 1.56 s for the previous build and 1.76 s for the selected
build. The best intermediate run was 0.99 s. It used the compact-record
variant from the experiment table.

The selected build completed the generated 60M-event sample in 13.10 s.
The local process reported 6.09 GB maximum resident size. This one run
establishes local completion and supports the row comparison below.

## Correctness checks

- The native ARM64 release, the native sanitizer build, and the
  Linux/amd64 image each passed all 36 organizer fixtures, all four new
  record-layout checks, and both public dev tables exactly. The layout
  checks exercise wide timestamps, wide catalogs, both together, and
  the compact category limit including `unknown`.
- The selected build matched the previous pipeline row for row on both
  20M-event samples and on the 60M-event sample. The 60M sample matched
  6,639,553 user rows and 28,014 campaign rows, with no duplicate keys.
- The local emulators selected the scalar x86 scanner. Both x86 SIMD
  variants compiled into the image. Execution and timings for those
  variants still need native x86 hardware with the matching features.

## Reproduce on N2

Use the organizer’s generator on a Linux/amd64 N2 host. Put the dataset on
the storage path closest to the scorer’s mount and record cache conditions
and CPU affinity with the results.

```sh
generate --seed 17 --events 20000000 --clicks 5000000 --output datasets/practice-17
generate --seed 41 --events 20000000 --clicks 5000000 --output datasets/practice-41
generate --seed 29 --events 60000000 --clicks 15000000 --output datasets/final-29

make build
scripts/benchmark.sh datasets/practice-17 scan 3
scripts/benchmark.sh datasets/practice-17 full 5
CSV_SIMD=avx2 scripts/benchmark.sh datasets/practice-17 full 5
scripts/benchmark.sh datasets/practice-41 full 5
scripts/benchmark.sh datasets/final-29 full 3
```

Alternate the scanner modes when comparing small differences. Compare the
full runtime and check the scanner name in the log. Use warm and first-run
measurements as separate observations. Run the fixtures and exact public
dev check for each candidate before using its timing to choose a submission.
The Docker harness wall time includes CLI and cleanup. Use container
start and exit timestamps for a comparison at the scorer's timing boundary.

## Profile function and system costs

An Instruments Time Profiler run on the native M1 build captured 6,669
running-thread samples on the generated 20M-event seed-17 input. The input
was read immediately before the recording. Both tables matched the
previous run on the same input.

| Stage | Share of running-thread samples |
| --- | ---: |
| Event CSV ingestion | 40.1% |
| Click CSV ingestion | 12.7% |
| Linked-ID resolution | 5.3% |
| Event deduplication and person partitioning | 24.1% |
| Person grouping, sorting, aggregation, and user Parquet | 16.2% |
| Remaining samples | 1.6% |

Event ingestion included 220 samples in the copy path reached when an
event candidate bucket grows. Across event and click ingestion, 310
samples had `Clock::parse` as the executing frame. These are sampled CPU
shares. They exclude time waiting for input and cannot explain a
leaderboard wall-time difference on their own.

On native N2, build with release optimizations and debug symbols. Set
`DATA_DIR` to the generated input. Run the executable under the matching
CPU affinity and memory limits. Use a fresh writable output directory for
each run. Compare default SIMD and `CSV_SIMD=avx2` with the same input,
affinity, and cache conditions.

```sh
cmake -S . -B build-trace -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-g
cmake --build build-trace --parallel

NCPU=8 POLARIS_TIMINGS=1 OUTPUT_DIR="$(mktemp -d)" \
  perf stat -e task-clock,cycles,instructions,branch-misses,cache-misses,minor-faults,major-faults \
  -- ./build-trace/shopping-sessions

NCPU=8 POLARIS_TIMINGS=1 OUTPUT_DIR="$(mktemp -d)" \
  perf record -F 499 --call-graph dwarf -- ./build-trace/shopping-sessions
perf report --stdio
```

If the VM does not expose hardware counters, use a software event such as
`cpu-clock` for sampling and keep the fault counters that are available.
Profile the executable directly so the samples belong to the workload.

## Trace-guided ingestion changes

The delimiter loop now reports when a complete row is ready. The scanner
only calls typed decoding at that point. This lets the compiler inline
the work done for each delimiter without also inlining the row decoder
into that operation. The same quote and field-count checks still apply
to each row, including the range tail.

Pre-size the event candidate vectors from the input byte count. The
estimate budgets 64 input bytes per candidate and divides the capacity
across the workers and event-ID buckets. A vector can still grow if a
worker receives a larger share of the input. The estimate affects
allocation only.

The follow-up M1 trace used the same 20M-event input. Event ingestion
accounted for 2,232 of 6,409 running-thread samples, down from 2,671 of
6,669. The first trace showed 220 samples in candidate growth copies.
The follow-up sampled none. Both output tables matched exactly.

In a five-run interleaved comparison on the M1, the warm seed-17 median
went from 0.99 s to 0.97 s. Median user CPU time fell from 5.93 s to
5.45 s. The fastest selected run took 0.91 s and the slowest took
2.07 s. The smaller wall-time change and the spread matter when
interpreting the CPU savings.

I also tested fixed-size candidate blocks, collecting delimiters before
decoding all fields at row end, and a dedicated UTC ingest-timestamp
path. Those variants did not show a repeatable end-to-end improvement
over the selected change in the local comparisons. The Linux/amd64
image and the native M1 build passed the organizer fixtures, the four
record-layout checks, and the public dev comparison. Native N2 timing
still needs a run on that hardware.
