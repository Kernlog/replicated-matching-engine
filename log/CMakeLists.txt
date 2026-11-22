add_library(rme_log STATIC command_log.cpp)
target_link_libraries(rme_log PUBLIC rme_codec)
