# Bug log

Every bug found by fuzzing, the simulator or chaos runs, with the seed that found it and the test that keeps it fixed.

## B-001: leader log grows without bound under overload
- Found by: sim seeds 59, 161, 239, 291, 446, 463 once the swarm varied Raft knobs (1 entry per append, inflight window 1-3, latency up to 40, proposals near every tick); liveness after heal
- Symptom: no node applied past the heal-time log end within 20,000 time units; seed 161 accepted 32,360 commands while only 13,148 were applied
- Root cause: `propose` accepted every command on the leader regardless of how far commit lagged, so under sustained overload the uncommitted suffix (and its memory) grew without limit and recovery meant draining all of it at the replication rate
- Fix: the leader refuses proposals once it holds `max_uncommitted_entries` (256) uncommitted entries; clients see backpressure and retry
- Regression: `BatchingTest.LeaderRefusesProposalsPastTheUncommittedLimit`; the six seeds run in the sim suite

## B-002: leader re-sends the same entries forever to a wiped follower
- Found by: the simulator's wipe fault while lost-state recovery was being built
- Symptom: a follower that lost its disk rejected every AppendEntries; the leader never moved `next` below `match + 1`, so the same batch went out on every heartbeat
- Root cause: on a rejection the leader clamped `next` to at least `match + 1`, trusting a match the follower no longer had
- Fix: a rejection hinting at or below `match` lowers `match`, since the follower provably lost that prefix
- Regression: `RecoveryTest.WipedFollowerCatchesUpAndRejoins`

## B-003: a recovering node never finishes recovery
- Found by: simulator seeds with wipes and PreVote
- Symptom: a wiped node stayed in recovery for the rest of the run
- Root cause: its term probes were sent from the election timer, which every heartbeat from the leader reset, so it never learned every peer's term
- Fix: probes run on their own cadence, independent of the election timer
- Regression: `RecoveryTest.RecoveringNodeNeverCampaignsAndOnlyProbesTerms`

## B-004: hintless rejection underflows the follower's match
- Found by: sim seed 29 (assertion in the snapshot path)
- Symptom: `match` became `0 - 1`, and the leader tried to send a snapshot for an index below its log
- Root cause: a rejection without a conflict hint (`conflict_index == 0`) went through the hint path and computed `hint - 1`
- Fix: a rejection without a hint leaves progress alone; the next heartbeat probes again
- Regression: `RecoveryTest.HintlessRejectionLeavesProgressAlone`; seed 29 in the sim suite

## B-005: a recovered node helps elect a leader missing committed entries
- Found by: 10,000 swarm seeds with wipes: 36 leader-completeness violations
- Symptom: a new leader lacked an entry an earlier leader had committed
- Root cause: recovery ended once the node caught up to a leader's commit index, but a fresh leader's commit index can trail entries an earlier leader already committed, so the node could vote again before it held them
- Fix: recovery ends only on an AppendEntries whose commit index points at an entry of the leader's own term
- Regression: `RecoveryTest.RecoveryWaitsForALeaderCommitInItsOwnTerm`; 10,000 swarm seeds with 0 violations

## B-006: the chaos audit reported retried commands as lost acks (checker bug)
- Found by: the first Docker kill run (seed 1): 2 violations out of 33,551 acks
- Symptom: an acked (session, seq) was not in any log at the acked index
- Root cause: a resend of a command that had already executed is acked as a duplicate with the resend's log index; the audit compared that index with the original execution
- Fix: the history records duplicate acks separately; the audit matches them by events digest at any index and Porcupine leaves their position open
- Regression: every chaos run since (kill and partition seeds, 0 violations)

## B-007: a leader at its uncommitted limit dropped commands, poisoning pipelined sessions
- Found by: the open-loop load generator at 50k commands/s on three local nodes
- Symptom: 165 of 400,000 commands were never acked and 107,000 later ones came back out of order
- Root cause: when `Raft::propose` refused a command at `max_uncommitted_entries`, the node dropped it; the session's next seq then skipped one, so the replica rejected every later command of that session
- Fix: the core keeps the refused command and stops draining until Raft takes it, so the full ring pushes back on the gateway and TCP
- Regression: `Node.LeaderAtItsUncommittedLimitHoldsCommandsInsteadOfDroppingThem`
