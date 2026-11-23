add_library(rme_sim STATIC event_queue.cpp network.cpp disk.cpp checker.cpp engine_machine.cpp cluster.cpp clients.cpp swarm.cpp)
target_link_libraries(rme_sim PUBLIC rme_consensus rme_replica rme_testing)
