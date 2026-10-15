find_package(Threads REQUIRED)

add_executable(bench_harness bench_harness.cpp)
target_link_libraries(bench_harness PRIVATE hft_core)

add_executable(bench_ring bench_ring.cpp)
target_link_libraries(bench_ring PRIVATE hft_core Threads::Threads)

add_executable(bench_book bench_book.cpp)
target_link_libraries(bench_book PRIVATE hft_core)
target_include_directories(bench_book PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)

add_executable(bench_md bench_md.cpp)
target_link_libraries(bench_md PRIVATE hft_core)
target_include_directories(bench_md PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)

add_executable(bench_match bench_match.cpp)
target_link_libraries(bench_match PRIVATE hft_core)
target_include_directories(bench_match PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)

add_executable(bench_risk bench_risk.cpp)
target_link_libraries(bench_risk PRIVATE hft_core)

add_executable(bt_report bt_report.cpp)
target_link_libraries(bt_report PRIVATE hft_core)

add_executable(bench_t2t bench_t2t.cpp)
target_link_libraries(bench_t2t PRIVATE hft_core)
