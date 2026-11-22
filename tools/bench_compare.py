#!/usr/bin/env python3
"""Render a markdown table comparing Google Benchmark JSON runs of a base and a head build.

usage: bench_compare.py BASE_SHA HEAD_SHA BASE_DIR HEAD_DIR
"""
import json
import pathlib
import statistics
import sys

MARKER = "<!-- rme-bench -->"
NOISE = 5.0


def medians(directory):
    samples = {}
    for path in sorted(pathlib.Path(directory).glob("*.json")):
        for b in json.loads(path.read_text())["benchmarks"]:
            if b.get("run_type") != "iteration":
                continue
            rate = b.get("items_per_second")
            value = (rate, "items/s") if rate else (b["real_time"], b["time_unit"])
            samples.setdefault(b["run_name"], []).append(value)
    return {name: (statistics.median(v for v, _ in vals), vals[0][1]) for name, vals in samples.items()}


def fmt(value, unit):
    if unit == "items/s":
        return f"{value / 1e6:.2f}M items/s"
    return f"{value:.1f} {unit}"


def main():
    base_sha, head_sha, base_dir, head_dir = sys.argv[1:5]
    base, head = medians(base_dir), medians(head_dir)
    lines = [
        MARKER,
        f"### Benchmarks: base `{base_sha[:7]}` vs head `{head_sha[:7]}`",
        "",
        "| Benchmark | Base | Head | Change |",
        "|---|---|---|---|",
    ]
    for name in sorted(base.keys() | head.keys()):
        b, h = base.get(name), head.get(name)
        if b and h:
            higher_better = h[1] == "items/s"
            change = (h[0] - b[0]) / b[0] * 100
            better = change > 0 if higher_better else change < 0
            flag = "" if abs(change) < NOISE else (" faster" if better else " **slower**")
            cells = [fmt(*b), fmt(*h), f"{change:+.1f}%{flag}"]
        else:
            cells = [fmt(*b) if b else "-", fmt(*h) if h else "-", "new" if h else "removed"]
        lines.append(f"| `{name}` | " + " | ".join(cells) + " |")
    lines += [
        "",
        f"Medians of alternated base/head runs on one pinned core of a shared GitHub runner;"
        f" changes within ±{NOISE:.0f}% are noise.",
    ]
    print("\n".join(lines))


if __name__ == "__main__":
    main()
