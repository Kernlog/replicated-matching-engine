find_package(Threads REQUIRED)
find_package(asio CONFIG REQUIRED)

add_library(rme_io STATIC atomic_file.cpp disk_storage.cpp frame.cpp fs.cpp raft_codec.cpp
                          storage_thread.cpp transport.cpp wal.cpp)
target_link_libraries(rme_io PUBLIC rme_core rme_codec rme_log rme_consensus asio::asio
                                    Threads::Threads)
target_compile_definitions(rme_io PUBLIC ASIO_NO_DEPRECATED)
