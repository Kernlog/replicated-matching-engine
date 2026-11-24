add_library(rme_md STATIC feed.cpp publisher.cpp)
target_link_libraries(rme_md PUBLIC rme_core rme_codec Threads::Threads)
