if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "HFT_BUILD_FUZZ needs clang (libFuzzer); use the 'fuzz' preset")
endif()

# The fuzz target links libFuzzer (provides main) plus ASan and UBSan so memory
# errors and undefined behaviour on hostile input are reported, not silent.
add_executable(fuzz_md fuzz_md.cpp)
target_link_libraries(fuzz_md PRIVATE hft_core)
target_compile_options(fuzz_md PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
target_link_options(fuzz_md PRIVATE -fsanitize=fuzzer,address,undefined)

# Seed-corpus generator: an ordinary program, no sanitizers needed.
add_executable(make_corpus make_corpus.cpp)
target_link_libraries(make_corpus PRIVATE hft_core)
target_include_directories(make_corpus PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)

# Engine vs reference matcher, run in lock step.
add_executable(fuzz_match fuzz_match.cpp)
target_link_libraries(fuzz_match PRIVATE hft_core)
target_include_directories(fuzz_match PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
target_compile_options(fuzz_match PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
target_link_options(fuzz_match PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(fuzz_oms fuzz_oms.cpp)
target_link_libraries(fuzz_oms PRIVATE hft_core)
target_compile_options(fuzz_oms PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
target_link_options(fuzz_oms PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(fuzz_codec fuzz_codec.cpp)
target_link_libraries(fuzz_codec PRIVATE hft_core)
target_compile_options(fuzz_codec PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
target_link_options(fuzz_codec PRIVATE -fsanitize=fuzzer,address,undefined)
