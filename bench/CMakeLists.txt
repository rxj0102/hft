find_package(Threads REQUIRED)

add_executable(bench_harness bench_harness.cpp)
target_link_libraries(bench_harness PRIVATE hft_core)

add_executable(bench_ring bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE hft_core Threads::Threads)
