add_library(rme_gateway STATIC client.cpp gateway.cpp protocol.cpp)
target_link_libraries(rme_gateway PUBLIC rme_node rme_io)
