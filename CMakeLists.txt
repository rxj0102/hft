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

# Compiles hot-path headers under -fno-exceptions -fno-rtti so a violation breaks
# the build instead of hiding in a driver that is allowed to use them.
add_library(hft_hotpath_check STATIC src/core/hotpath_check.cpp src/book/book_check.cpp
            src/md/md_check.cpp src/sim/sim_check.cpp src/oms/oms_check.cpp src/bt/bt_check.cpp)
target_link_libraries(hft_hotpath_check PUBLIC hft_core PRIVATE hft_hotpath)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)

# Fuzz targets need clang's libFuzzer runtime: use the "fuzz" preset.
option(HFT_BUILD_FUZZ "Build libFuzzer targets (requires clang)" OFF)
if(HFT_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
