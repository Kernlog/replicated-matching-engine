add_library(rme_io STATIC atomic_file.cpp disk_storage.cpp frame.cpp fs.cpp wal.cpp)
target_link_libraries(rme_io PUBLIC rme_core rme_codec rme_log rme_consensus)
