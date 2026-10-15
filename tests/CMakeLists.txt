add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE hft_core)
# Test driver uses iostream, so it must not inherit -fno-exceptions.
target_compile_options(test_core PRIVATE -fexceptions -frtti)
add_test(NAME test_core COMMAND test_core)
