cmake_minimum_required(VERSION 4.3)

project(server)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(server ${SOURCES})

target_include_directories(server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/")
