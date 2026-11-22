add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE rme_log)

if(TARGET rme_testing)
  add_executable(gen_log gen_log.cpp)
  target_link_libraries(gen_log PRIVATE rme_log rme_testing)
endif()
