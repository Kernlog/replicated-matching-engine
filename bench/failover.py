#!/usr/bin/env python3
"""Client-observed failover time: SIGKILL the leader under load and measure the longest gap
between consecutive acks, over many runs.

usage: bench/failover.py [runs] ; BIN=build/release, DATA_ROOT=/dev/shm
"""
import os
import pathlib
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

BIN = pathlib.Path(os.environ.get("BIN", "build/release"))
ROOT = os.environ.get("DATA_ROOT", "/tmp")


def leader_of(logs):
    best = (-1, None)
    for node, path in logs.items():
        for line in path.read_text(errors="replace").splitlines():
            m = re.search(r"node (\d) role 3 term (\d+)", line)
            if m and int(m.group(2)) > best[0]:
                best = (int(m.group(2)), node)
    return best[1]


def one_run(seed):
    work = pathlib.Path(tempfile.mkdtemp(dir=ROOT))
    base = 30000 + random.Random(seed).randrange(20000)
    members = [f"--member=127.0.0.1:{base + i}" for i in (1, 2, 3)]
    procs, logs = {}, {}
    try:
        for i in (1, 2, 3):
            logs[i] = work / f"node{i}.log"
            procs[i] = subprocess.Popen(
                [BIN / "node/rme_node", f"--id={i}", f"--data-dir={work / str(i)}",
                 "--listen=127.0.0.1", f"--client-port={base + 10 + i}", *members],
                stdout=open(logs[i], "w"), stderr=subprocess.STDOUT)
        time.sleep(1.0)
        history = work / "history.tsv"
        client = subprocess.Popen(
            [BIN / "tools/chaos_client", "--workers=1", "--seconds=3", f"--out={history}",
             *[f"--member=127.0.0.1:{base + 10 + i}" for i in (1, 2, 3)]])
        time.sleep(1.2)
        leader = leader_of(logs)
        procs[leader].send_signal(signal.SIGKILL)
        client.wait(timeout=60)
        ends = sorted(int(line.split("\t")[4]) for line in history.read_text().splitlines()
                      if line.split("\t")[5] in ("ok", "dup"))
        return max(b - a for a, b in zip(ends, ends[1:])) / 1e6
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.kill()
            p.wait()
        shutil.rmtree(work, ignore_errors=True)


def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    gaps = [one_run(seed) for seed in range(runs)]
    gaps.sort()
    q = lambda p: gaps[min(len(gaps) - 1, int(p * len(gaps)))]
    print(f"runs={runs} failover_ms p50={q(0.5):.0f} p90={q(0.9):.0f} p99={q(0.99):.0f} "
          f"max={gaps[-1]:.0f} mean={statistics.mean(gaps):.0f}")


if __name__ == "__main__":
    main()
