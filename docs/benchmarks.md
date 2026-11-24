# Benchmarks

Every number here comes from `bench/report.sh` (microbenchmarks and replicated latency),
`bench/sweep.sh` and `bench/failover.py`, run from the same commit. Raw HdrHistogram output is in
[bench/](bench/).

## Host and method

- AMD Ryzen 7 7800X3D (Zen 4, 8 cores / 16 threads, 96 MB L3), 23 GB RAM, Ubuntu 24.04 VM on
  Linux 6.18. Virtualized: no `isolcpus`, and the hypervisor schedules the vCPUs, which shows up as
  millisecond outliers in the far tail.
- Release build: clang 18, `-O3 -flto=thin -march=native` for the microbenchmarks.
- The 3-node cluster runs as three local processes. With `--cpus` each node's core thread gets its
  own physical core, its network thread another, and its storage thread the core thread's SMT
  sibling; the load generator is pinned to the last core.
- All three nodes share one virtual disk, which sustains about 290-460 `fdatasync`/s. A real
  deployment gives each node its own disk, so the disk-backed numbers below are a lower bound;
  runs with `DATA_ROOT=/dev/shm` keep the WALs in memory to measure the software path alone.
- Latency is open loop: commands go out at fixed intervals and each is timed from when it was
  scheduled to go out until its ack (commit and apply on the leader) arrives, so a stall counts
  against every command scheduled behind it. Percentiles are from HdrHistogram at 3 significant
  digits.

## Single node

| Benchmark | Result |
|---|---|
| matching engine, 1M-command stream | 44.1M commands/s, 0 allocations, 5.3 MB book |
| per-command apply latency | p50 30 ns, p99 80 ns, p99.9 160 ns |
| record decode / encode | 374M/s / 147M/s |
| CRC32C (SSE4.2) | 12.7 GiB/s |
| command log replay | 126M commands/s |
| SPSC ring, two cores | 58M items/s single, 1.39G items/s in batches of 64, 121 ns round trip |
| WAL, 256-byte entries | 286 entries/s syncing each one; 36.3k entries/s with group commit (146 syncs/s) |
| peer transport, loopback | 1.44M AppendEntries/s, 0.69 writes per 1,000 frames |
