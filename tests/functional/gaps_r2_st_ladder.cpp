/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// gaps_r2_st_ladder.cpp -- functional: the server transport ladder stops at
// its first grant. The portable rung is transport 0 and opens a real UDP
// socket, so the case is functional, not a unit case. At a port-zero
// address, a list naming xdp (which cannot discover a port) after portable
// leaves the server on portable, and the ladder never visits xdp after its
// grant: no "transport xdp refused" warning, no "granted ... after
// refusals" summary. (A second open over the granted one is itself refused
// and the status is not read on a grant, so a ladder that went on would
// change only what it logs; the case reads the log.)
// mutation gap: hunt 2/5044 (ladder loop granted_transport < 0 to <= 0)

#include "../native_shutdown.h"
#include "../loopback_hub.h"
#include "rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_DEV

extern void rocketnet_functional_restore_log_function();

static void gaps_r2_st_ladder_fault( const char * fault, const char * function, const char * file, int line )
{
    printf( "fault: %s\n", fault );
    printf( "check failed: function %s, file %s, line %d\n", function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define gaps_r2_st_ladder_expect( condition, fault )                                                   \
    do                                                                                                 \
    {                                                                                                  \
        if ( !( condition ) )                                                                          \
        {                                                                                              \
            gaps_r2_st_ladder_fault( fault, (const char *) __FUNCTION__, __FILE__, __LINE__ );        \
        }                                                                                              \
    } while ( 0 )

static int g_gaps_r2_st_ladder_after_grant = 0;

static void gaps_r2_st_ladder_log( void *, rocketnet_log_level_t, const char * message, size_t length )
{
    if ( length > 0 && ( strstr( message, "transport xdp refused" ) != NULL || strstr( message, "after refusals" ) != NULL ) )
    {
        g_gaps_r2_st_ladder_after_grant++;
    }
}

void test_gaps_r2_st_ladder_stops_at_a_portable_grant()
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 2;
    config.loopback_hub = rocketnet_test_loopback_hub();
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable,xdp" );

    // the resolved create takes no environment pass: a transport pin in the
    // runner's environment does not displace this list
    const rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    g_gaps_r2_st_ladder_after_grant = 0;
    rocketnet_log_function( gaps_r2_st_ladder_log, NULL );
    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_resolved( NULL, "127.0.0.1:0", &config, 0, &status );
    rocketnet_functional_restore_log_function();
    rocketnet_log_level( saved );

    gaps_r2_st_ladder_expect( server != NULL && status == ROCKETNET_STATUS_OK, "a server listing portable then xdp did not start on 127.0.0.1:0" );

    const char * name = rocketnet_server_transport_io_name( server );
    gaps_r2_st_ladder_expect( name != NULL && strcmp( name, "portable" ) == 0, "a server listing portable first is not on portable" );
    gaps_r2_st_ladder_expect( g_gaps_r2_st_ladder_after_grant == 0, "the ladder granted portable and went on to try the next rung (xdp named refused after the grant)" );

    rn_native_server_transport_stop_and_destroy( server );
}

#endif // #if ROCKETNET_DEV
