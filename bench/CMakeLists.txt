find_package(benchmark CONFIG REQUIRED)

add_executable(engine_bench engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE rme_engine rme_codec rme_log rme_testing benchmark::benchmark)

add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE rme_core benchmark::benchmark)
