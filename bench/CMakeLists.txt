add_executable(bench_harness bench_harness.cpp)
target_link_libraries(bench_harness PRIVATE hft_core)
target_compile_options(bench_harness PRIVATE -fexceptions -frtti)
