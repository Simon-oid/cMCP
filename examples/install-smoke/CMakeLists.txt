# Standalone CMakeLists for the install-smoke consumer (CMake flavor).
# Built outside cMCP's tree against an installed cMCP via find_package.
# Invoke from examples/install-smoke/run.sh, which points
# CMAKE_PREFIX_PATH at the temp install root.

cmake_minimum_required(VERSION 3.16)
project(cmake_smoke C)

find_package(cmcp REQUIRED COMPONENTS core server client)

add_executable(cmake-smoke smoke.c)
target_link_libraries(cmake-smoke PRIVATE
    cmcp::server cmcp::client cmcp::core)
target_compile_features(cmake-smoke PRIVATE c_std_11)
target_compile_options(cmake-smoke PRIVATE -Wall -Wextra -Wpedantic)
