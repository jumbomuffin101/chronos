"""Summarize raw Chronos benchmark samples; optional plotting requires matplotlib."""

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--plot", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    for path in args.inputs:
        with path.open(newline="", encoding="utf-8") as source:
            for name, size, latency, reset in csv.reader(source):
                groups[(name, int(size))].append((float(latency), float(reset)))

    def percentile(values, p):
        return sorted(values)[math.ceil(len(values) * p) - 1]

    rows = []
    for (name, size), values in sorted(groups.items()):
        latency = [item[0] for item in values]
        reset = [item[1] for item in values]
        rows.append([name, size, len(values), percentile(latency, 0.5) / 1000,
                     percentile(latency, 0.95) / 1000, percentile(latency, 0.99) / 1000,
                     1e9 / statistics.mean(latency), percentile(reset, 0.5) / 1000])
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["workload", "background_orders", "samples", "p50_us", "p95_us",
                         "p99_us", "commands_per_second_from_mean", "reset_verify_p50_us"])
        writer.writerows(rows)
    if args.plot:
        import matplotlib.pyplot as plt

        fig, axis = plt.subplots(figsize=(8, 4.5))
        for name in ("Noncrossing", "SingleFill", "MissingCancel", "Duplicate"):
            points = sorted((row[1], row[3]) for row in rows if row[0] == name)
            axis.plot([p[0] for p in points], [p[1] for p in points], marker="o", label=name)
        axis.set(xscale="log", yscale="log", xlabel="Background resting orders",
                 ylabel="Median command latency (µs)", title="Chronos Phase 4A: WSL2 baseline")
        axis.grid(True, which="both", alpha=0.25)
        axis.legend()
        fig.tight_layout()
        fig.savefig(args.plot, dpi=160)


if __name__ == "__main__":
    main()
