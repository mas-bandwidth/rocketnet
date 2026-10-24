# Every examples/*.c builds as example_<name>, and examples/game_loop/ as
# example_game_loop. Most use the development
# library to mint development tokens locally. The backend examples use the
# selected library mode and can connect with production backend keys.
file( GLOB ROCKETNET_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c" )
foreach( example_source IN LISTS ROCKETNET_EXAMPLE_SOURCES )
    get_filename_component( example_name "${example_source}" NAME_WE )
    add_executable( example_${example_name} "${example_source}" )
    if(example_name STREQUAL "backend_client" OR example_name STREQUAL "backend_server")
        # These examples accept real backend keys and exercise the selected build mode.
        target_link_libraries( example_${example_name} PRIVATE rocketnet )
    else()
        target_link_libraries( example_${example_name} PRIVATE rocketnet_dev )
    endif()
endforeach()

# examples/game_loop/ is a game's shape: a launcher, a server and a client,
# each side's network step on its own thread beside the frame.
add_executable( example_game_loop
    "${CMAKE_CURRENT_SOURCE_DIR}/game_loop/launcher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_loop/server.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/game_loop/client.cpp" )
target_include_directories( example_game_loop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" )
target_link_libraries( example_game_loop PRIVATE rocketnet_dev Threads::Threads )

# The soak allocator telemetry uses a native lock across callback threads.
target_link_libraries( example_soak PRIVATE Threads::Threads )

# backend_check_token runs the library's own token checks on a token one of
# the backends in examples/backend/ minted (tools/run_backend_examples.py). It
# is a test of the backends, not an example, so it may use the library's
# internal headers.
add_executable( backend_check_token "${CMAKE_CURRENT_SOURCE_DIR}/backend/check_token.cpp" )
target_link_libraries( backend_check_token PRIVATE rocketnet_dev )
