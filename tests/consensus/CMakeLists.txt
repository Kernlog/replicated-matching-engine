rme_add_test(consensus_test
  SOURCES types_test.cpp raft_log_test.cpp election_test.cpp persist_test.cpp
          replication_test.cpp consistency_test.cpp commit_test.cpp noop_test.cpp
          figures_test.cpp prevote_test.cpp check_quorum_test.cpp batching_test.cpp pipeline_test.cpp compaction_test.cpp install_snapshot_test.cpp
  LIBS rme_consensus rme_codec rme_testing)
