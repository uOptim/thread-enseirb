cmake_minimum_required (VERSION 2.6)
project (thread-enserib)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wall")

include_directories (${PROJECT_SOURCE_DIR}/include)

add_subdirectory (src)
add_subdirectory (tests)

