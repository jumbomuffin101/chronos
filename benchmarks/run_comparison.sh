#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
output="${1:-docs/assets/phase4/4b}"
mkdir -p "$output" build/bench

run_case() {
    local mode="$1" label="$2" filter="$3" stem="$output/$2"
    if [[ -e "$stem.samples.csv" || -e "$stem.json" || -e "$stem.time.txt" ]]; then
        echo "Refusing to overwrite $stem artifacts" >&2
        return 1
    fi
    echo "Running $label"
    env CHRONOS_TRANSACTION_MODE="$mode" CHRONOS_BENCH_SAMPLES="$stem.samples.csv" \
        /usr/bin/time -v -o "$stem.time.txt" ./build/bench/benchmarks/chronos_bench \
        --benchmark_filter="$filter" --benchmark_color=false \
        --benchmark_out="$stem.json" --benchmark_out_format=json > "build/bench/$label.log"
}

# Alternate mode order by size; each case itself contains three fresh repetitions.
run_case reference reference-100 'EmptyMarket|/100/'
run_case planned planned-100 'EmptyMarket|/100/'
run_case planned planned-1000 '/1000/'
run_case reference reference-1000 '/1000/'
run_case reference reference-10000 '/10000/'
run_case planned planned-10000 '/10000/'
# The reference mode's quadratic setup at 100,000 is intentionally omitted.
run_case planned planned-100000 '/100000/'
env CHRONOS_TRANSACTION_MODE=reference ./build/bench/benchmarks/chronos_allocation_probe > "$output/reference-allocations.csv"
env CHRONOS_TRANSACTION_MODE=planned ./build/bench/benchmarks/chronos_allocation_probe > "$output/planned-allocations.csv"
python3 - "$output" <<'PY'
import collections
import csv
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
expected_cases = {"reference-100": 11, "planned-100": 11, "reference-1000": 10,
                  "planned-1000": 10, "reference-10000": 10, "planned-10000": 10,
                  "planned-100000": 10}
for label, expected in expected_cases.items():
    with (root / f"{label}.json").open(encoding="utf-8") as source:
        records = json.load(source)["benchmarks"]
    errors = [r.get("error_message", r["name"]) for r in records if r.get("error_occurred")]
    if errors:
        raise SystemExit(f"{label}: benchmark errors: {errors}")
    with (root / f"{label}.samples.csv").open(newline="", encoding="utf-8") as source:
        counts = collections.Counter((r[0], r[1]) for r in csv.reader(source))
    if len(counts) != expected or any(n != 192 for n in counts.values()):
        raise SystemExit(f"{label}: unexpected sample counts: {counts}")
print("All benchmark workloads verified with expected sample counts.")
PY
