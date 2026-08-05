cmake_minimum_required(VERSION 3.24)

project(replayarena
  VERSION 0.1.0
  DESCRIPTION "Verifiable LLM game arena on a deterministic C++20 inference gateway"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_IS_TOP_LEVEL)
  set(REPLAYARENA_SANITIZE "" CACHE STRING "Sanitizer to enable: address | thread | (empty)")
  set_property(CACHE REPLAYARENA_SANITIZE PROPERTY STRINGS "" address thread)
endif()

add_library(replayarena_options INTERFACE)
target_compile_options(replayarena_options INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

if(REPLAYARENA_SANITIZE STREQUAL "address")
  target_compile_options(replayarena_options INTERFACE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(replayarena_options INTERFACE -fsanitize=address)
elseif(REPLAYARENA_SANITIZE STREQUAL "thread")
  target_compile_options(replayarena_options INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(replayarena_options INTERFACE -fsanitize=thread)
elseif(NOT REPLAYARENA_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown REPLAYARENA_SANITIZE value: ${REPLAYARENA_SANITIZE}")
endif()

# Header-only gateway components (issue #1 onward).
add_library(replayarena_gateway INTERFACE)
target_include_directories(replayarena_gateway INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(replayarena src/main.cpp)
target_link_libraries(replayarena PRIVATE replayarena_options)

enable_testing()
add_subdirectory(tests)
