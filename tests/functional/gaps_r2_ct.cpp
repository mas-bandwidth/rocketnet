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
#include <time.h>

// defined in tests/functional/rocketnet_functional_tests.cpp
void rocketnet_functional_log_ring_dump();
// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

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

#if ROCKETNET_DEV
// mutation gap: hunt 2/7981, 2/7983, 2/7984, 2/7985 (the legacy receive thread)
//
// A client created with a link simulator setting runs the BASE path: the
// caller sends, and a legacy receive thread reads the socket, waiting 1 ms
// between passes, until stop; its last act is to say it has stopped. Over a
// loopback hub of the case's own the client (1 ms simulated latency)
// connects to a server transport and receives a payload from it, which only
// that thread can read; idle for a second its process uses well under a
// core; after stop it reports stopped. Every wait is bounded.

#define GAPS_R2_CT_CONNECT_SECONDS 10.0
#define GAPS_R2_CT_IDLE_SECONDS 1.0
#define GAPS_R2_CT_IDLE_CPU_SECONDS 0.5 // a thread spinning without its wait takes about a whole core over the idle second

static double gaps_r2_ct_process_cpu_seconds()
{
    return (double) clock() / (double) CLOCKS_PER_SEC;
}

void rocketnet_test_gaps_r2_ct_legacy_receive_thread_connects_receives_idles_and_stops()
{
    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 2, &hub_status );
    gaps_r2_ct_check( hub != NULL && hub_status == ROCKETNET_STATUS_OK );

    const double start = rocketnet_platform_time();
    double now = 1000.0;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    server_config.loopback_hub = hub;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_internal( NULL, "127.0.0.1:0", &server_config, now, &status );
    gaps_r2_ct_check( server != NULL && status == ROCKETNET_STATUS_OK );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    gaps_r2_ct_check( rocketnet_write_test_connect_token( server_address, 0x7E57C1, rocketnet_functional_token_timestamp( 0, now ), token, sizeof( token ) ) ==
                      ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = hub;
    client_config.simulated_latency_ms = 1;

    status = -1;
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_internal( NULL, token, &client_config, now, &status );
    gaps_r2_ct_check( client != NULL && status == ROCKETNET_STATUS_OK );
    gaps_r2_ct_check( rocketnet_client_transport_client_io( client ) == NULL ); // the BASE path: the legacy receive thread

    // connect, then a payload from the server

    const uint8_t payload[4] = { 0x52, 0x32, 0x43, 0x54 };
    bool connected = false;
    bool received = false;
    bool sent = false;
    while ( !received && rocketnet_platform_time() - start < GAPS_R2_CT_CONNECT_SECONDS )
    {
        now = 1000.0 + ( rocketnet_platform_time() - start );
        rocketnet_server_transport_update( server, now );
        rocketnet_client_transport_update( client, now );

        connected = connected || rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
        if ( connected && rocketnet_server_transport_is_client_connected( server, 0 ) && !sent )
        {
            rocketnet_server_transport_send_packet( server, 0, payload, (int) sizeof( payload ) );
            sent = true;
        }

        uint8_t copy[64];
        received = rocketnet_client_transport_receive_packet( client, copy, (int) sizeof( copy ) ) == (int) sizeof( payload ) && memcmp( copy, payload, sizeof( payload ) ) == 0;

        rocketnet_client_transport_send_packets( client );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    if ( !connected || !received )
    {
        printf( "mutation gap hunt 2/7981,7983: a legacy-path client %s and %s the server's payload within %g s (client state %d): its receive thread does not read the socket\n",
                connected ? "connected" : "never connected", received ? "received" : "never received", GAPS_R2_CT_CONNECT_SECONDS,
                rocketnet_client_transport_state( client ) );
    }
    gaps_r2_ct_check( connected );
    gaps_r2_ct_check( received );

    // idle: nobody updates, the receive thread waits between passes

    const double cpu_before = gaps_r2_ct_process_cpu_seconds();
    rocketnet_platform_sleep( GAPS_R2_CT_IDLE_SECONDS );
    const double idle_cpu = gaps_r2_ct_process_cpu_seconds() - cpu_before;
    if ( idle_cpu >= GAPS_R2_CT_IDLE_CPU_SECONDS )
    {
        printf( "mutation gap hunt 2/7984: an idle legacy-path client's process used %.3f s of CPU over %g s: its receive thread spins without waiting\n", idle_cpu,
                GAPS_R2_CT_IDLE_SECONDS );
    }
    gaps_r2_ct_check( idle_cpu < GAPS_R2_CT_IDLE_CPU_SECONDS );

    // stop: the thread finishes and says so

    rocketnet_client_transport_stop( client );
    const double deadline = rocketnet_platform_time() + GAPS_R2_CT_STOP_SECONDS;
    while ( !rocketnet_client_transport_stopped( client ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    const bool stopped = rocketnet_client_transport_stopped( client );
    if ( !stopped )
    {
        printf( "mutation gap hunt 2/7985: a legacy-path client did not report stopped within %g s of stop: its receive thread left without finishing the stop\n",
                GAPS_R2_CT_STOP_SECONDS );
    }
    gaps_r2_ct_check( stopped );
    rocketnet_client_transport_destroy( client );

    rocketnet_server_transport_stop( server );
    for ( int pass = 0; !rocketnet_server_transport_stopped( server ) && rocketnet_platform_time() < deadline + GAPS_R2_CT_STOP_SECONDS; pass++ )
    {
        now = 1000.0 + ( rocketnet_platform_time() - start );
        rocketnet_server_transport_update( server, now );
        rocketnet_platform_sleep( 0.001 );
    }
    gaps_r2_ct_check( rocketnet_server_transport_stopped( server ) );
    rocketnet_server_transport_destroy( server );

    gaps_r2_ct_check( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );

    printf( "legacy receive thread: connected, received, idle CPU %.3f s over %g s, stopped\n", idle_cpu, GAPS_R2_CT_IDLE_SECONDS );
}
#endif // #if ROCKETNET_DEV
