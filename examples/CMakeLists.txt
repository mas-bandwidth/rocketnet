# Every examples/*.c builds as example_<name>, linked to the development
# library so a development connect token can be minted locally. No per-file
# line: adding an example is adding one C file to this directory.
file( GLOB ROCKETNET_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c" )
foreach( example_source IN LISTS ROCKETNET_EXAMPLE_SOURCES )
    get_filename_component( example_name "${example_source}" NAME_WE )
    add_executable( example_${example_name} "${example_source}" )
    target_link_libraries( example_${example_name} PRIVATE rocketnet_dev )
endforeach()

# backend_check_token runs the library's own token checks on a token one of
# the backends in examples/backend/ minted (tools/run_backend_examples.py). It
# is a test of the backends, not an example, so it may use the library's
# internal headers.
add_executable( backend_check_token "${CMAKE_CURRENT_SOURCE_DIR}/backend/check_token.cpp" )
target_link_libraries( backend_check_token PRIVATE rocketnet_dev )
