/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The split client's start barrier steps (tools/load_split_startup.h): which
// start message a client accepts, and when it answers.

#include "test_unit_registry.h"

#include "../tools/load_split_startup.h"

#define check rocketnet_unit_check

// The client transport hands the message layer payloads from before it
// reports CONNECTED, and a send before CONNECTED is dropped at start. An ARM
// that arrives in that window is answered once the client is connected, not
// on arrival, where its ACK would be dropped and the server would wait out
// its start deadline (Windows x64 MSVC, 1000 clients on one host, 3 runs in 10).

static void test_load_split_startup_ack_waits_for_connected()
{
    struct split_startup_t startup = { 0, 0, 0, 0, 0 };
    check( split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    check( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    check( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    check( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_ACK );
    split_startup_sent( &startup, SPLIT_STARTUP_SEND_ACK );
    check( startup.armed );
    check( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_NONE );
}

static void test_load_split_startup_sequence()
{
    struct split_startup_t startup = { 0, 0, 0, 0, 0 };
    check( !split_startup_receive( &startup, SPLIT_STARTUP_GO ) );
    check( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    check( split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    check( !split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    check( !split_startup_receive( &startup, SPLIT_STARTUP_GO ) );       // its ACK is not sent yet
    split_startup_sent( &startup, split_startup_due( &startup, 1 ) );
    check( split_startup_receive( &startup, SPLIT_STARTUP_GO ) );
    check( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );      // its GO ACK is not sent yet
    check( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    check( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_GO_ACK );
    split_startup_sent( &startup, SPLIT_STARTUP_SEND_GO_ACK );
    check( split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    check( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    check( startup.running );
    check( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_NONE );
}

REGISTER_UNIT_TEST( test_load_split_startup_ack_waits_for_connected );
REGISTER_UNIT_TEST( test_load_split_startup_sequence );
