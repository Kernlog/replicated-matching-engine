add_library(rme_sim STATIC event_queue.cpp network.cpp)
target_link_libraries(rme_sim PUBLIC rme_consensus)
