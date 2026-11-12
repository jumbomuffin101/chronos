"""Join Phase 4B summaries without hiding missing reference measurements."""

import argparse
import csv
from pathlib import Path


def load(path):
    with path.open(newline="", encoding="utf-8") as source:
        return {(row["workload"], int(row["background_orders"])): row
                for row in csv.DictReader(source)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("planned", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plot", type=Path)
    args = parser.parse_args()
    reference, planned = load(args.reference), load(args.planned)
    fields = ["workload", "background_orders", "samples", "reference_mean_us", "planned_mean_us",
              "reference_p50_us", "planned_p50_us", "reference_p95_us", "planned_p95_us",
              "reference_p99_us", "planned_p99_us", "reference_commands_per_s",
              "planned_commands_per_s", "median_speedup", "mean_speedup"]
    rows = []
    for key, new in sorted(planned.items()):
        old = reference.get(key)
        def val(row, field):
            return float(row[field]) if row else None
        old_rate = val(old, "commands_per_second_from_mean")
        new_rate = val(new, "commands_per_second_from_mean")
        old_p50, new_p50 = val(old, "p50_us"), val(new, "p50_us")
        rows.append([key[0], key[1], new["samples"], 1e6 / old_rate if old_rate else None,
                     1e6 / new_rate, old_p50, new_p50, val(old, "p95_us"), val(new, "p95_us"),
                     val(old, "p99_us"), val(new, "p99_us"), old_rate, new_rate,
                     old_p50 / new_p50 if old_p50 else None, new_rate / old_rate if old_rate else None])
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(fields)
        writer.writerows(rows)
    if args.plot:
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(figsize=(8, 4.5))
        for label, column in (("Clone reference", 5), ("Planned transaction", 6)):
            data = [(row[1], row[column]) for row in rows if row[0] == "Noncrossing" and row[column] is not None]
            ax.plot([item[0] for item in data], [item[1] for item in data], marker="o", label=label)
        ax.set(xscale="log", yscale="log", xlabel="Background resting orders",
               ylabel="Median command latency (µs)", title="Chronos Phase 4B comparison (WSL2)")
        ax.grid(True, which="both", alpha=0.25)
        ax.legend()
        fig.tight_layout()
        fig.savefig(args.plot, dpi=160)


if __name__ == "__main__":
    main()
