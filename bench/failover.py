#!/usr/bin/env python3
"""Client-observed failover time: SIGKILL the leader under load and measure the longest gap
between consecutive acks, over many runs.

usage: bench/failover.py [runs] ; BIN=build/release, DATA_ROOT=/dev/shm
"""
import os
import pathlib
import re
import shutil
import signal
import socket
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


def free_ports(count):
    sockets = [socket.socket() for _ in range(count)]
    for s in sockets:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in sockets]
    for s in sockets:
        s.close()
    return ports


def one_run():
    work = pathlib.Path(tempfile.mkdtemp(dir=ROOT))
    ports = free_ports(6)
    members = [f"--member=127.0.0.1:{ports[i - 1]}" for i in (1, 2, 3)]
    procs, logs = {}, {}
    try:
        for i in (1, 2, 3):
            logs[i] = work / f"node{i}.log"
            procs[i] = subprocess.Popen(
                [BIN / "node/rme_node", f"--id={i}", f"--data-dir={work / str(i)}",
                 "--listen=127.0.0.1", f"--client-port={ports[2 + i]}", *members],
                stdout=open(logs[i], "w"), stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while leader_of(logs) is None and time.monotonic() < deadline:
            time.sleep(0.05)
        if leader_of(logs) is None:
            raise RuntimeError("no leader elected: " + logs[1].read_text()[-500:])
        history = work / "history.tsv"
        client = subprocess.Popen(
            [BIN / "tools/chaos_client", "--workers=1", "--seconds=3", f"--out={history}",
             *[f"--member=127.0.0.1:{ports[2 + i]}" for i in (1, 2, 3)]])
        time.sleep(1.0)
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
    gaps = []
    while len(gaps) < runs:
        try:
            gaps.append(one_run())
        except RuntimeError as error:
            print(f"retrying: {error}", file=sys.stderr)
    gaps.sort()
    q = lambda p: gaps[min(len(gaps) - 1, int(p * len(gaps)))]
    print(f"runs={runs} failover_ms p50={q(0.5):.0f} p90={q(0.9):.0f} p99={q(0.99):.0f} "
          f"max={gaps[-1]:.0f} mean={statistics.mean(gaps):.0f}")


if __name__ == "__main__":
    main()
