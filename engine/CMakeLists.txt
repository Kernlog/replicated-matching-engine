add_library(rme_engine STATIC order_book.cpp snapshot.cpp)
target_link_libraries(rme_engine PUBLIC rme_core)

add_library(rme_replica STATIC replica.cpp)
target_link_libraries(rme_replica PUBLIC rme_codec)
