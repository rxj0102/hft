find_package(Threads REQUIRED)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE hft_core)
add_test(NAME test_core COMMAND test_core)

add_executable(test_phase1 test_phase1.cpp)
target_link_libraries(test_phase1 PRIVATE hft_core Threads::Threads)
add_test(NAME test_phase1 COMMAND test_phase1)

add_executable(test_md test_md.cpp)
target_link_libraries(test_md PRIVATE hft_core)
target_include_directories(test_md PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
add_test(NAME test_md COMMAND test_md 100000)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
  add_test(NAME test_md_gate COMMAND test_md 2000000)
endif()

add_executable(test_book test_book.cpp)
target_link_libraries(test_book PRIVATE hft_core)
target_include_directories(test_book PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
# Default run is sized for sanitizer builds; the 10M-event gate run is separate.
add_test(NAME test_book COMMAND test_book 1000000)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
  add_test(NAME test_book_gate_10m COMMAND test_book 10000000)
endif()

add_executable(test_sim test_sim.cpp)
target_link_libraries(test_sim PRIVATE hft_core)
target_include_directories(test_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
add_test(NAME test_sim COMMAND test_sim 100000)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
  add_test(NAME test_sim_gate COMMAND test_sim 300000)
endif()
