include(FetchContent)

# Dev-only dependency (not linked into the server or client).
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Clang 21+ warns inside GoogleTest 1.15.2 itself (gtest-printers.h, char8_t -> char32_t),
# because the top-level -Wall -Wextra reaches its sources. Silence only that warning, only
# on GoogleTest's own targets. Probe the positive form: GCC accepts any -Wno-<unknown>, and
# older Clang warns about an unknown -Wno- flag.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wcharacter-conversion WEBSERVER_HAS_WCHARACTER_CONVERSION)
if(WEBSERVER_HAS_WCHARACTER_CONVERSION)
    foreach(gtest_target gtest gtest_main gmock gmock_main)
        target_compile_options(${gtest_target} PRIVATE -Wno-character-conversion)
    endforeach()
endif()

include(GoogleTest)

add_executable(common_tests
    error_test.cpp
    result_test.cpp
    socketio_test.cpp
    parser_test.cpp
    response_test.cpp
    request_test.cpp
    parse_number_test.cpp
    router_test.cpp
    server_config_parser_test.cpp
    server_idle_timeout_test.cpp
    mime_types_test.cpp
    file_reader_test.cpp
    static_file_handler_test.cpp
)
# SocketIo.h is header-only and lives next to the server sources.
target_include_directories(common_tests PRIVATE ${PROJECT_SOURCE_DIR}/server/include)
target_link_libraries(common_tests PRIVATE common GTest::gtest_main)
target_compile_options(common_tests PRIVATE -Wall -Wextra)
# A hang (e.g. a sweep loop that never advances) must fail, not block ctest forever.
gtest_discover_tests(common_tests PROPERTIES TIMEOUT 30)
