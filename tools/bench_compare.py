#!/usr/bin/env python3
"""Render a markdown table comparing Google Benchmark JSON runs of a base and a head build.

usage: bench_compare.py BASE_SHA HEAD_SHA BASE_DIR HEAD_DIR

Each DIR holds benchmark JSON files and optionally sizes.json ({"name": bytes}).
"""
import json
import pathlib
import statistics
import sys

MARKER = "<!-- rme-bench -->"
NOISE = 5.0
STANDARD = {
    "name", "family_index", "per_family_instance_index", "run_name", "run_type", "repetitions",
    "repetition_index", "threads", "iterations", "real_time", "cpu_time", "time_unit", "label",
    "error_occurred", "error_message", "aggregate_name", "aggregate_unit",
}
TO_NS = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}


def metrics(directory):
    samples = {}
    for path in sorted(pathlib.Path(directory).glob("*.json")):
        if path.name == "sizes.json":
            continue
        for b in json.loads(path.read_text())["benchmarks"]:
            if b.get("run_type") != "iteration":
                continue
            name = b["run_name"].split("/")[0]
            values = {k: v for k, v in b.items() if k not in STANDARD and isinstance(v, (int, float))}
            if not values:
                values["time_ns"] = b["real_time"] * TO_NS[b["time_unit"]]
            for metric, value in values.items():
                samples.setdefault((name, metric), []).append(value)
    out = {key: statistics.median(vals) for key, vals in samples.items()}
    sizes = pathlib.Path(directory) / "sizes.json"
    if sizes.exists():
        for name, value in json.loads(sizes.read_text()).items():
            out[("binary size", name)] = value
    return out


def higher_is_better(metric):
    return metric.endswith("_per_second")


def fmt(metric, value):
    if metric == "items_per_second":
        return f"{value / 1e6:.2f}M/s"
    if metric == "bytes_per_second":
        return f"{value / 1e9:.2f} GB/s"
    if metric.endswith("_ns"):
        return f"{value:,.0f} ns"
    if metric.endswith("_mb"):
        return f"{value:,.1f} MB"
    if metric.endswith("bytes") or metric.startswith("replay"):
        return f"{value / 1024:,.1f} KiB"
    return f"{value:,.2f}"


def label(metric):
    return {"items_per_second": "throughput", "bytes_per_second": "throughput"}.get(metric, metric)


def change_cell(metric, base, head):
    if base == 0:
        return "+0.0%" if head == 0 else "new nonzero"
    change = (head - base) / base * 100
    if abs(change) < NOISE:
        return f"{change:+.1f}%"
    better = change > 0 if higher_is_better(metric) else change < 0
    word = ("faster" if better else "**slower**") if metric.endswith(("_second", "_ns")) else (
        "better" if better else "**worse**")
    return f"{change:+.1f}% {word}"


def main():
    base_sha, head_sha, base_dir, head_dir = sys.argv[1:5]
    base, head = metrics(base_dir), metrics(head_dir)
    lines = [
        MARKER,
        f"### Benchmarks: base `{base_sha[:7]}` vs head `{head_sha[:7]}`",
        "",
        "| Benchmark | Metric | Base | Head | Change |",
        "|---|---|---|---|---|",
    ]
    for name, metric in sorted(base.keys() | head.keys()):
        b, h = base.get((name, metric)), head.get((name, metric))
        if b is not None and h is not None:
            cells = [fmt(metric, b), fmt(metric, h), change_cell(metric, b, h)]
        else:
            cells = [fmt(metric, b) if b is not None else "-",
                     fmt(metric, h) if h is not None else "-", "new" if h is not None else "removed"]
        lines.append(f"| `{name}` | {label(metric)} | " + " | ".join(cells) + " |")
    lines += [
        "",
        f"Medians of alternated base/head runs on one pinned core of a shared GitHub runner;"
        f" changes within ±{NOISE:.0f}% are noise. Latency includes one `steady_clock` read per"
        f" command. Raw JSON is attached to the run as the `bench-results` artifact.",
    ]
    print("\n".join(lines))


if __name__ == "__main__":
    main()
