#!/usr/bin/env python3
"""Fault injection against the compose cluster while chaos_client records a history.

usage: nemesis.py --faults kill,partition,latency,pause,wipe --seconds 60 --seed 1 --out run/

Each fault holds for 1-4 s, then heals, then the cluster gets 1-3 s of calm. Every fault and heal
is written to nemesis.tsv in the output directory with its time, so a failing history can be lined
up against what the cluster went through.
"""
import argparse
import pathlib
import random
import subprocess
import sys
import time

NODES = ["node1", "node2", "node3"]


def sh(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True).stdout.strip()


def container(node):
    return sh("docker", "compose", "ps", "-q", node)


def root(node, *args):
    sh("docker", "exec", "-u", "root", container(node), *args, check=False)


def ip_of(node):
    return sh("docker", "inspect", "-f", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
              container(node))


class Nemesis:
    def __init__(self, rng, log):
        self.rng = rng
        self.log = log
        self.start = time.monotonic()

    def note(self, what):
        self.log.write(f"{time.monotonic() - self.start:.3f}\t{what}\n")
        self.log.flush()
        print(what, flush=True)

    def kill(self):
        node = self.rng.choice(NODES)
        sh("docker", "kill", "-s", "KILL", container(node), check=False)
        self.note(f"kill {node}")
        return lambda: (sh("docker", "compose", "start", node), self.note(f"start {node}"))

    def partition(self):
        alone = self.rng.choice(NODES)
        rest = [n for n in NODES if n != alone]
        ips = {n: ip_of(n) for n in NODES}
        for other in rest:
            root(alone, "iptables", "-A", "INPUT", "-s", ips[other], "-j", "DROP")
            root(alone, "iptables", "-A", "OUTPUT", "-d", ips[other], "-j", "DROP")
        self.note(f"partition {alone} | {' '.join(rest)}")

        def heal():
            root(alone, "iptables", "-F")
            self.note("heal partition")
        return heal

    def latency(self):
        node = self.rng.choice(NODES)
        delay = self.rng.choice([20, 50, 100])
        root(node, "tc", "qdisc", "replace", "dev", "eth0", "root", "netem", "delay",
             f"{delay}ms", f"{delay // 2}ms")
        self.note(f"latency {node} {delay}ms")

        def heal():
            root(node, "tc", "qdisc", "del", "dev", "eth0", "root")
            self.note(f"heal latency {node}")
        return heal

    def wipe(self):
        # Two lost nodes out of three cannot elect anyone, so one recovers before the next goes.
        recovering = [n for n in NODES if subprocess.run(
            ["docker", "compose", "exec", "-T", n, "test", "-f", "/data/LOST_STATE"],
            capture_output=True).returncode == 0]
        if recovering:
            self.note(f"wipe skipped: {' '.join(recovering)} still recovering")
            return lambda: None
        node = self.rng.choice(NODES)
        sh("docker", "kill", "-s", "KILL", container(node), check=False)
        sh("docker", "compose", "run", "--rm", "--no-deps", "--entrypoint", "sh", node, "-c",
           "rm -rf /data/* && touch /data/LOST_STATE")
        self.note(f"wipe {node}")
        return lambda: (sh("docker", "compose", "start", node), self.note(f"start {node}"))

    def pause(self):
        node = self.rng.choice(NODES)
        sh("docker", "pause", container(node), check=False)
        self.note(f"pause {node}")
        return lambda: (sh("docker", "unpause", container(node), check=False),
                        self.note(f"unpause {node}"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--faults", default="kill,partition,latency,pause")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("run"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    with open(args.out / "nemesis.tsv", "w") as log:
        nemesis = Nemesis(rng, log)
        faults = [getattr(nemesis, name) for name in args.faults.split(",")]
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            heal = rng.choice(faults)()
            time.sleep(rng.uniform(1, 4))
            heal()
            time.sleep(rng.uniform(1, 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
