add_executable(decode_fuzz decode_fuzz.cpp)
target_link_libraries(decode_fuzz PRIVATE rme_codec)
target_link_options(decode_fuzz PRIVATE -fsanitize=fuzzer)
