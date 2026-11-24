find_package(Threads REQUIRED)
add_library(rme_md STATIC feed.cpp publisher.cpp)
target_link_libraries(rme_md PUBLIC rme_core rme_codec Threads::Threads)

add_library(rme_md_subscriber STATIC subscriber.cpp)
target_link_libraries(rme_md_subscriber PUBLIC rme_md rme_gateway rme_replica)
