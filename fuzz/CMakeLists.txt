add_executable(decode_fuzz decode_fuzz.cpp)
target_link_libraries(decode_fuzz PRIVATE rme_codec)
target_link_options(decode_fuzz PRIVATE -fsanitize=fuzzer)

add_executable(frame_fuzz frame_fuzz.cpp)
target_link_libraries(frame_fuzz PRIVATE rme_io)
target_link_options(frame_fuzz PRIVATE -fsanitize=fuzzer)
