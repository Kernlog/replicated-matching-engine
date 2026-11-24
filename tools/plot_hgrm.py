#!/usr/bin/env python3
"""Plots HdrHistogram percentile distributions (.hgrm) on the usual 1/(1-p) axis.

usage: plot_hgrm.py out.svg label=file.hgrm ...
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load(path):
    points = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 4 and not line.lstrip().startswith("#"):
            try:
                value, percentile, _, inverse = map(float, parts)
            except ValueError:
                continue
            if percentile < 1.0:
                points.append((1.0 / (1.0 - percentile), value))
    return points


def main():
    out, series = sys.argv[1], sys.argv[2:]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for item in series:
        label, path = item.split("=", 1)
        xs, ys = zip(*load(path))
        ax.plot(xs, ys, label=label)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ticks = [1, 2, 10, 100, 1000, 10000, 100000]
    ax.set_xticks(ticks)
    ax.set_xticklabels(["0%", "50%", "90%", "99%", "99.9%", "99.99%", "99.999%"])
    ax.set_xlabel("percentile")
    ax.set_ylabel("commit-to-ack latency (us)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out)


if __name__ == "__main__":
    main()
