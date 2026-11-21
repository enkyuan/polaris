#!/bin/sh
# Usage: scripts/benchmark.sh DATA_DIR [full|scan] [repeats]
# Local native binary: set POLARIS_BIN. Default: use the Docker harness.
set -eu
data=$(cd "${1:?provide a dataset directory}" && pwd)
mode=${2:-full}
repeats=${3:-3}
case "$mode" in
    full) set -- ;;
    scan) set -- --scan-only ;;
    *) echo "mode must be full or scan" >&2; exit 2 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
i=1
while [ "$i" -le "$repeats" ]; do
    out="$work/output-$i"
    mkdir -p "$out"
    echo "$mode run $i" >&2
    if [ -n "${POLARIS_BIN:-}" ]; then
        DATA_DIR="$data" OUTPUT_DIR="$out" NCPU="${CPUS:-8}" POLARIS_TIMINGS=1 \
            /usr/bin/time -p "$POLARIS_BIN" "$@"
    else
        POLARIS_TIMINGS=1 /usr/bin/time -p "$(dirname "$0")/docker_run.sh" "$data" "$out" "$@"
    fi
    i=$((i + 1))
done
