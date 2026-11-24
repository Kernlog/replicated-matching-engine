find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(rme_node STATIC node.cpp)
target_link_libraries(rme_node PUBLIC rme_io rme_replica rme_consensus spdlog::spdlog)

add_executable(rme_node_bin main.cpp)
set_target_properties(rme_node_bin PROPERTIES OUTPUT_NAME rme_node)
target_link_libraries(rme_node_bin PRIVATE rme_node rme_gateway rme_md CLI11::CLI11)
