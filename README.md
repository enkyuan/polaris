# Shopping Sessions

A native C++20 pipeline for the shopping-sessions-4 track. It reads the mounted
CSV logs and writes `user_daily/` and `campaign_daily/` as Parquet.

## Build and verify

The scorer builds the top-level Dockerfile for Linux/amd64 and starts the
image without arguments. The image needs no runtime network or writable root.

```sh
python3 -m pip install -r requirements-dev.txt
make fixtures
make dev                 # generate from the organizer’s generator.zip must be on PATH
make run-dev
make check-dev           # unzip dev-answer.zip into dev-answer/ first
```

The dev input is generated with seed 1, 1,000,000 events and 250,000 clicks.
The fixtures and comparison harness are from the Polars starter. `dev/`,
`dev-answer/`, and `out/` are local data directories excluded from Git and the
Docker build context.

For direct tests on Linux or macOS with CMake and a C++20 compiler:

```sh
make fixtures-native
DATA_DIR="$PWD/dev" OUTPUT_DIR="$PWD/out" NCPU=8 build/shopping-sessions
make check-dev
```

Pass `-DPOLARIS_SANITIZE=ON` to CMake for an AddressSanitizer and
UndefinedBehaviorSanitizer build.
Run `python3 scripts/check_record_layouts.py build/shopping-sessions` to check
the timestamp and category limits of the compact representation.

## Benchmark

```sh
make build
scripts/benchmark.sh /path/to/generated-data scan 3
scripts/benchmark.sh /path/to/generated-data full 3
```

The scan mode reads event, click, and link CSVs and validates their field
structure. The full mode measures the pipeline through Parquet close. Both
modes time process or container startup. Set `POLARIS_BIN` to an absolute path
to benchmark a native executable. Set `CPUS` for worker count, and record
hardware and cache conditions when reporting times. Benchmarks emit timings
on stderr.

By default the scanner dispatches to AVX-512BW or AVX2 on supported x86 CPUs,
to NEON on ARM64, and to scalar otherwise. Use `CSV_SIMD=avx2` or
`CSV_SIMD=scalar` to compare implementations. Full runs with
`POLARIS_TIMINGS=1` print the selected scanner and phase timings.

See [PERFORMANCE.md](PERFORMANCE.md) for the measurements and design decisions.

## Pipeline

- Schedule newline-aligned input ranges dynamically. Resolve connected
  components while event reads proceed, then map clicks and event winners to
  the byte-wise minimum person ID. A membership bitmap avoids exact alias-map
  probes for IDs that are definitely absent.
- Bucket events by ID. Resolve repeated fingerprints by exact key equality
  and ingest time, retaining invalid payloads until the earliest copy is known.
- Group winning events and valid clicks into per-person spans. Sort those
  spans by timestamp, sessionize in one pass, and roll up campaign assists
  from the union of each campaign’s ranges of session starts. Keep event
  timestamps and purchases compact through timeline sorting when they fit.
- Stream user rows to worker-local Parquet files and reduce campaign counters.
  The writer emits flat uncompressed Parquet with date and UTF-8 annotations.
