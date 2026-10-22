/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

/* The split client's start barrier (load_split_startup.h). Build and run
   beside the harness, from the repository root:

       cc -std=c11 -I. tools/test_load_split_startup.c -o test_load_split_startup && ./test_load_split_startup
       cl /W4 /WX /I. tools\test_load_split_startup.c && test_load_split_startup.exe
*/

#include "tools/load_split_startup.h"

#include <assert.h>
#include <stdio.h>

/* The client transport hands the message layer payloads from before it
 * reports CONNECTED, and a send before CONNECTED is dropped at start. An ARM
 * that arrives in that window is answered once the client is connected, not
 * on arrival, where its ACK would be dropped and the server would wait out
 * its start deadline (captain, 1000 clients on one host, 3 runs in 10). */
static void test_split_startup_ack_waits_for_connected( void )
{
    struct split_startup_t startup = { 0, 0, 0, 0, 0 };
    assert( split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    assert( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    assert( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    assert( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_ACK );
    split_startup_sent( &startup, SPLIT_STARTUP_SEND_ACK );
    assert( startup.armed );
    assert( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_NONE );
}

static void test_split_startup_sequence( void )
{
    struct split_startup_t startup = { 0, 0, 0, 0, 0 };
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_GO ) );
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    assert( split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_ARM ) );
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_GO ) );       /* its ACK is not sent yet */
    split_startup_sent( &startup, split_startup_due( &startup, 1 ) );
    assert( split_startup_receive( &startup, SPLIT_STARTUP_GO ) );
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );      /* its GO ACK is not sent yet */
    assert( split_startup_due( &startup, 0 ) == SPLIT_STARTUP_SEND_NONE );
    assert( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_GO_ACK );
    split_startup_sent( &startup, SPLIT_STARTUP_SEND_GO_ACK );
    assert( split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    assert( !split_startup_receive( &startup, SPLIT_STARTUP_RUN ) );
    assert( startup.running );
    assert( split_startup_due( &startup, 1 ) == SPLIT_STARTUP_SEND_NONE );
}

int main( void )
{
    test_split_startup_ack_waits_for_connected();
    printf( "test_split_startup_ack_waits_for_connected\n" );
    test_split_startup_sequence();
    printf( "test_split_startup_sequence\n" );
    return 0;
}
