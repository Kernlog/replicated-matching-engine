add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE rme_log)

if(TARGET rme_testing)
  add_executable(gen_log gen_log.cpp)
  target_link_libraries(gen_log PRIVATE rme_log rme_testing)
endif()

add_executable(sim sim.cpp)
target_link_libraries(sim PRIVATE rme_sim)

if(TARGET rme_testing)
  find_package(CLI11 CONFIG REQUIRED)
  add_executable(chaos_client chaos_client.cpp)
  target_link_libraries(chaos_client PRIVATE rme_gateway rme_testing CLI11::CLI11)

  add_executable(audit audit.cpp)
  target_link_libraries(audit PRIVATE rme_io rme_replica CLI11::CLI11)
endif()
