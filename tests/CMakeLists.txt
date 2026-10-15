find_package(Threads REQUIRED)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE hft_core)
add_test(NAME test_core COMMAND test_core)

add_executable(test_phase1 test_phase1.cpp)
target_link_libraries(test_phase1 PRIVATE hft_core Threads::Threads)
add_test(NAME test_phase1 COMMAND test_phase1)
