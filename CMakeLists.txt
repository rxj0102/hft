cmake_minimum_required(VERSION 3.25)
project(hft VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Interface target carrying the warning policy so every target inherits it.
add_library(hft_warnings INTERFACE)
target_compile_options(hft_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)

# Hot-path code must build without exceptions and RTTI. Test and bench
# drivers (which use iostream and the check macro) are exempt.
add_library(hft_hotpath INTERFACE)
target_compile_options(hft_hotpath INTERFACE -fno-exceptions -fno-rtti)

add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hft_core INTERFACE hft_warnings)
# Hot-path libraries (Phase 1+) link hft_hotpath explicitly; header-only core
# and the test/bench drivers do not, so they may use stdio and iostream.

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
