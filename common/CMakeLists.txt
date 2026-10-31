# Header-only code shared by server and client (RAII wrappers, protocol helpers, ...).
# Switch to add_library(common STATIC ...) once it has .cpp files.
add_library(common INTERFACE)
target_include_directories(common INTERFACE include)
