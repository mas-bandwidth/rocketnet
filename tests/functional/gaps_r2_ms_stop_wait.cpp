/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// gaps_r2_ms_stop_wait.cpp -- the message object's stop wait on the real
// clock (sdk/rocketnet_message_server.cpp, rn_message_link_stop_and_wait). A
// create refused after its link was built stops the owned transport and
// polls it until it reports stopped or a one-second budget ends, sleeping
// 1 ms between polls. A transport that never stops is polled for the whole
// budget: the wait sleeps between its polls (about one per millisecond),
// ends at the deadline, and keeps the transport rather than destroy it.
// Functional: it reads the platform clock and sleeps for one second. The
// unit suite runs the same loop on a counter clock through the DEV hold
// (tests/test_unit_client_io_stop.cpp), which never reaches the sleep.
//
// mutation gap: hunt 1/908 (rocketnet_platform_sleep in the wait deleted: a busy poll)

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_message_object.h"
#include "rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

static void gaps_r2ms_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define gaps_r2ms_check( condition )                                                                           \
    do                                                                                                         \
    {                                                                                                          \
        if ( !( condition ) )                                                                                  \
        {                                                                                                      \
            gaps_r2ms_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ );            \
        }                                                                                                      \
    } while ( 0 )

struct gaps_r2ms_never_stops_t
{
    int stops;
    long updates;
    int destroys;
};

static void gaps_r2ms_update( void * transport, double time )
{
    (void) time;
    ( (gaps_r2ms_never_stops_t *) transport )->updates++;
}

static void gaps_r2ms_stop( void * transport )
{
    ( (gaps_r2ms_never_stops_t *) transport )->stops++;
}

static bool gaps_r2ms_stopped( const void * transport )
{
    (void) transport;
    return false;
}

static void gaps_r2ms_destroy( void * transport )
{
    ( (gaps_r2ms_never_stops_t *) transport )->destroys++;
}

void test_gaps_r2ms_stop_wait_sleeps_between_polls()
{
    gaps_r2ms_never_stops_t transport;
    memset( &transport, 0, sizeof( transport ) );

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = &transport;
    link.update = gaps_r2ms_update;
    link.stop = gaps_r2ms_stop;
    link.stopped = gaps_r2ms_stopped;
    link.destroy = gaps_r2ms_destroy;

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 0;                                    // refused: the create stops the link it was handed

    rocketnet_quiet( 1 );
    const double start = rocketnet_time();
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 1, 2, &config, NULL, 0, 0, 1000.0 );
    const double waited = rocketnet_time() - start;
    rocketnet_quiet( 0 );

    gaps_r2ms_check( object == NULL );
    gaps_r2ms_check( transport.stops == 1 );
    gaps_r2ms_check( waited >= 0.9 );                           // the whole budget, on the real clock
    gaps_r2ms_check( transport.destroys == 0 );                 // kept: the worker still owns it

    // one poll per 1 ms sleep: about 1000 over the second. A wait that does
    // not sleep polls as fast as the update returns, millions of times.

    if ( transport.updates > 5000 )
    {
        printf( "fault: mutation gap hunt 1/908: the stop wait polled a transport that never stops %ld times in %.2f seconds: it does not sleep between polls\n",
                transport.updates, waited );
        fflush( stdout );
    }
    gaps_r2ms_check( transport.updates >= 1 && transport.updates <= 5000 );
}

#endif // #if ROCKETNET_DEV
