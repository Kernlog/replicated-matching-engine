add_library(rme_consensus STATIC raft_log.cpp raft.cpp)
target_link_libraries(rme_consensus PUBLIC rme_core)
