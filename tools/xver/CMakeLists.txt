# The cross-version check's harness (tools/xver/xver.c), built against one
# rocketnet source tree: XVER_TREE is that tree (a release tag's worktree, or
# this repository). The tree is added as a subdirectory, so xver links that
# tree's own rocketnet_dev with that tree's own dependencies, and nothing in
# the tree is edited. tools/xver/xver.go runs it:
#
#   cmake -S tools/xver -B <build dir> -DXVER_TREE=<tree> -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build dir> --target xver
#
# The main build compiles the same file against this tree (CMakeLists.txt,
# ROCKETNET_BUILD_EXAMPLES), so a change to the public API that breaks the
# harness fails per commit, not only at night.

cmake_minimum_required(VERSION 3.20)

project(rocketnet_xver LANGUAGES C CXX)

if(NOT XVER_TREE)
    message(FATAL_ERROR "tools/xver: set XVER_TREE to the rocketnet source tree to build against")
endif()

# EXCLUDE_FROM_ALL: build only what xver links, not the tree's other targets
add_subdirectory("${XVER_TREE}" rocketnet EXCLUDE_FROM_ALL)

add_executable(xver xver.c)
target_link_libraries(xver PRIVATE rocketnet_dev)
target_compile_options(xver PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion -Werror)
