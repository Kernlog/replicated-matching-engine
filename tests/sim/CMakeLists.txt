rme_add_test(sim_test
  SOURCES event_queue_test.cpp clock_test.cpp network_test.cpp disk_test.cpp
          determinism_test.cpp cluster_test.cpp
  LIBS rme_sim)
