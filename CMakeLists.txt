cmake_minimum_required(VERSION 4.3)
project(posix_in_cpp)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


set(CMAKE_CXX_STANDARD 23)

add_executable(prog
        main.cpp)
