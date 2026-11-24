add_library(rme_io STATIC frame.cpp)
target_link_libraries(rme_io PUBLIC rme_core rme_codec)
