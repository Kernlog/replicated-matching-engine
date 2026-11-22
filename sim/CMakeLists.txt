add_library(rme_sim STATIC event_queue.cpp network.cpp disk.cpp checker.cpp engine_machine.cpp cluster.cpp swarm.cpp)
target_link_libraries(rme_sim PUBLIC rme_consensus rme_codec rme_testing)
