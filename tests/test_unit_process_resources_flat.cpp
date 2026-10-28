/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// THE FLAT RULE's handle (fd) count (tests/process_resources.h,
// process_resources_flat) fails on growth only. The counts are fakes and the
// run length is an argument: no process is read and no time passes.

#include "test_unit_registry.h"

#include "process_resources.h"

// a fall passes: the handles of retiring OS pool workers close with them
static void test_process_resources_handle_fall_is_flat()
{
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 115, 113, 1800.0 ) == 1 );
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 106, 104, 1800.0 ) == 1 );
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 106, 106, 1800.0 ) == 1 );
}

// a rise fails, by one handle, at any run length; an unread count fails
static void test_process_resources_handle_rise_is_not_flat()
{
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 106, 107, 1800.0 ) == 0 );
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 106, 107, 0.0 ) == 0 );
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, -1, 104, 1800.0 ) == 0 );
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_HANDLES, 106, -1, 1800.0 ) == 0 );
    // the thread count is unchanged: it must still equal its start
    rocketnet_unit_check( process_resources_flat( PROCESS_RESOURCES_THREADS, 4, 3, 1800.0 ) == 0 );
}

REGISTER_UNIT_TEST( test_process_resources_handle_fall_is_flat );
REGISTER_UNIT_TEST( test_process_resources_handle_rise_is_not_flat );
