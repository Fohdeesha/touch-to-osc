cmake_minimum_required(VERSION 3.18)
project(touch-to-osc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TTO_WERROR "Treat warnings as errors" OFF)
option(TTO_TESTS "Build the unit tests" ON)
set(TTO_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)

set(TTO_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wformat=2 -Wimplicit-fallthrough -Wundef)
if(TTO_WERROR)
  list(APPEND TTO_WARNINGS -Werror)
endif()

if(TTO_SANITIZE)
  add_compile_options(-fsanitize=${TTO_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${TTO_SANITIZE})
endif()

configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/tto_version.h @ONLY)

add_library(tto_thirdparty INTERFACE)
target_include_directories(tto_thirdparty SYSTEM INTERFACE third_party/tomlplusplus)
target_compile_definitions(tto_thirdparty INTERFACE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=1)

file(GLOB TTO_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp src/osc/*.cpp src/config/*.cpp src/input/*.cpp src/control/*.cpp src/tools/*.cpp)
add_library(tto_core STATIC ${TTO_CORE_SOURCES})
target_include_directories(tto_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_compile_options(tto_core PRIVATE ${TTO_WARNINGS})
target_link_libraries(tto_core PUBLIC tto_thirdparty PkgConfig::LIBEVDEV Threads::Threads)

add_executable(touch-to-osc src/main.cpp)
target_compile_options(touch-to-osc PRIVATE ${TTO_WARNINGS})
target_link_libraries(touch-to-osc PRIVATE tto_core)

include(GNUInstallDirs)
install(TARGETS touch-to-osc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TTO_TESTS)
  enable_testing()
  file(GLOB TTO_TEST_SOURCES CONFIGURE_DEPENDS tests/unit/*.cpp)
  add_executable(tto_tests ${TTO_TEST_SOURCES})
  target_include_directories(tto_tests PRIVATE third_party/doctest tests)
  target_compile_options(tto_tests PRIVATE ${TTO_WARNINGS})
  target_link_libraries(tto_tests PRIVATE tto_core)
  target_compile_definitions(tto_tests PRIVATE TTO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME unit COMMAND tto_tests)
endif()
