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
