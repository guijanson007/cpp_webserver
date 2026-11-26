cmake_minimum_required(VERSION 4.3)

project(webserver LANGUAGES CXX)

add_compile_options(-Wall -Wextra -Wpedantic)

option(WEBSERVER_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(WEBSERVER_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

enable_testing()
add_subdirectory(tests)
