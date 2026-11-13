/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Functional (a real I/O thread, a bounded real-time wait for its stop): the
// thread a loopback client runs. The loopback driver has no wait and no wake,
// so the driver alone would put a client on the BASE path; the client
// transport puts a threaded loopback client on the I/O core by name
// (sdk/rocketnet_client_transport.cpp, the io_thread decision at create; the
// production library's branch is the one this guards, so the case runs in
// functional and in functional_production). The client is the transport
// layer's, created against a loopback hub of the case's own; no server is
// needed to see the path, and nothing goes on a socket.

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// defined in tests/functional/rocketnet_functional_tests.cpp
void rocketnet_functional_log_ring_dump();

static void gaps_r2_ct_check_handler( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_functional_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define gaps_r2_ct_check( condition )                                                                \
    do                                                                                               \
    {                                                                                                \
        if ( !( condition ) )                                                                        \
        {                                                                                            \
            gaps_r2_ct_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ ); \
        }                                                                                            \
    } while ( 0 )

#define GAPS_R2_CT_STOP_SECONDS 10.0 // a bound, never a cadence: a stop past it fails the case

// mutation gap: hunt 2/7240 (a production loopback client runs the I/O core)

void rocketnet_test_gaps_r2_ct_loopback_client_runs_the_io_core()
{
    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 2, &hub_status );
    gaps_r2_ct_check( hub != NULL && hub_status == ROCKETNET_STATUS_OK );

    // a token sealed to a key of the case's own (a production client refuses
    // the published development key), naming 127.0.0.1:40000 on the hub

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = 1;
    signed_fields.create_timestamp = 1000;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memset( signed_fields.server_public_key, 0x42, ROCKETNET_KEY_BYTES );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );
    rocketnet_write_signed_connect_data( &signed_fields, token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    memset( config.packet_filter, 0x5A, sizeof( config.packet_filter ) );
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    config.loopback_hub = hub;

    int status = -1;
    struct rocketnet_client_transport_t * transport = rocketnet_client_transport_create_internal( NULL, token, &config, 1000.0, &status );
    gaps_r2_ct_check( transport != NULL );
    gaps_r2_ct_check( status == ROCKETNET_STATUS_OK );

    const bool io_core = rocketnet_client_transport_client_io( transport ) != NULL;
    if ( !io_core )
    {
        printf( "mutation gap hunt 2/7240: a threaded loopback client runs the BASE path (no I/O core): its driver has no wait or wake, and the name loopback must put it on the I/O core\n" );
    }
    gaps_r2_ct_check( io_core );

    rocketnet_client_transport_stop( transport );
    const double deadline = rocketnet_platform_time() + GAPS_R2_CT_STOP_SECONDS;
    while ( !rocketnet_client_transport_stopped( transport ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    gaps_r2_ct_check( rocketnet_client_transport_stopped( transport ) );
    rocketnet_client_transport_destroy( transport );

    gaps_r2_ct_check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );

    printf( "loopback client: I/O core, stopped and destroyed\n" );
}
