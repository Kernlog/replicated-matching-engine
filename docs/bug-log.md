# Bug log

Every bug found by fuzzing, the simulator or chaos runs, with the seed that found it and the test that keeps it fixed.

## B-001: leader log grows without bound under overload
- Found by: sim seeds 59, 161, 239, 291, 446, 463 once the swarm varied Raft knobs (1 entry per append, inflight window 1-3, latency up to 40, proposals near every tick); liveness after heal
- Symptom: no node applied past the heal-time log end within 20,000 time units; seed 161 accepted 32,360 commands while only 13,148 were applied
- Root cause: `propose` accepted every command on the leader regardless of how far commit lagged, so under sustained overload the uncommitted suffix (and its memory) grew without limit and recovery meant draining all of it at the replication rate
- Fix: the leader refuses proposals once it holds `max_uncommitted_entries` (256) uncommitted entries; clients see backpressure and retry
- Regression: `BatchingTest.LeaderRefusesProposalsPastTheUncommittedLimit`; the six seeds run in the sim suite
