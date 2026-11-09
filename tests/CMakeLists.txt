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

include(GoogleTest)

add_executable(common_tests
    error_test.cpp
    result_test.cpp
    socketio_test.cpp
    parser_test.cpp
)
# SocketIo.h is header-only and lives next to the server sources.
target_include_directories(common_tests PRIVATE ${PROJECT_SOURCE_DIR}/server/include)
target_link_libraries(common_tests PRIVATE common GTest::gtest_main)
target_compile_options(common_tests PRIVATE -Wall -Wextra)
gtest_discover_tests(common_tests)
