/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite: every test here opens a socket or starts a real transport.
    The assertions are the ones the unit suite used to run. A test that cannot
    run without a socket stays here. The unit suite drives the same rules
    through the state machines and the in-memory simulator.
*/

#include "../tests/native_shutdown.h"
#include "../tests/time_scale.h"
#include "rocketnet_internal.h"

// The shared harness carries one call with the retired create-time argument.
// The harness itself is outside this card's paths, so the call is routed to the
// current three-argument entry here, at its only include site: the extra
// argument is dropped and the call reaches the new signature unchanged.

#define rocketnet_server_transport_create(a, b, c, d) rocketnet_server_transport_create(a, b, c)
#include "harness.inc"
#undef rocketnet_server_transport_create

#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_client_io.h"

#include <math.h>
#include <errno.h>
#include <stdio.h>

#if defined(__APPLE__)
#include <sys/ioctl.h>
#elif defined(__linux__)
#include <linux/sock_diag.h>
#endif

#include "rocketnet_wire.h"
#include "rocketnet_fragment.h"

#include <unordered_map>
#include <atomic>

extern std::atomic<unsigned long long> g_client_ack_stats_count;

// every transport a test ends with is balanced before it is destroyed: the
// server's counters and each client's, RECEIVED == the sum of its outcomes.
// A macro, so a failure names the test that ended the transport.

#define ROCKETNET_COUNTERS_CHECK rocketnet_check
#include "../tests/counters_expect.h"

// The clean-run rule rides along: every refusal reads 0 on a run that injects
// no fault. A test that provokes a refusal on purpose ends with the _EXCEPT
// form and lists the refusals its own scenario explains.

#define ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client )                                                           \
do                                                                                                              \
{                                                                                                               \
    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );                       \
    rocketnet_counters_assert_clean( rocketnet_client_transport_counters( client ) );                           \
    rn_native_client_transport_stop_and_destroy( client );                                                               \
} while(0)

#define ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server )                                                           \
do                                                                                                              \
{                                                                                                               \
    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );                       \
    rocketnet_counters_assert_clean( rocketnet_server_transport_counters( server ) );                           \
    rn_native_server_transport_stop_and_destroy( server );                                                               \
} while(0)

#define ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( client, ... )                                               \
do                                                                                                              \
{                                                                                                               \
    rocketnet_counters_assert_conserved( rocketnet_client_transport_counters( client ) );                       \
    rocketnet_counters_assert_clean_except( rocketnet_client_transport_counters( client ), __VA_ARGS__ );       \
    rn_native_client_transport_stop_and_destroy( client );                                                               \
} while(0)

#define ROCKETNET_FUNCTIONAL_DESTROY_SERVER_EXCEPT( server, ... )                                               \
do                                                                                                              \
{                                                                                                               \
    rocketnet_counters_assert_conserved( rocketnet_server_transport_counters( server ) );                       \
    rocketnet_counters_assert_clean_except( rocketnet_server_transport_counters( server ), __VA_ARGS__ );       \
    rn_native_server_transport_stop_and_destroy( server );                                                               \
} while(0)

void rocketnet_test_server_refuses_keyless_config()
{
    // the no-default-keys gate: a config with no keys is refused, never
    // silently defaulted -- default keys are how every deployment of a
    // protocol ends up sharing one identity. the dev posture is explicit.

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config) == NULL );

    // and out-of-range client counts are refused at the same gate

    rocketnet_functional_server_config( &config );
    config.max_clients = 0;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config) == NULL );
    config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config) == NULL );
}

struct rocketnet_test_hammer_args_t
{
    struct rocketnet_server_transport_t * server;
    int thread_id;
    int packets;
};

static void rocketnet_test_hammer_thread( void * data )
{
    struct rocketnet_test_hammer_args_t * args = (struct rocketnet_test_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( args->server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        for ( int j = 2; j < 16; j++ )
            payload[j] = (uint8_t) ( args->thread_id * 31 + i * 7 + j );
        rocketnet_server_transport_finish_packet( args->server, &context, 16 );
    }
}

struct rocketnet_test_wire_hammer_args_t
{
    struct rocketnet_server_transport_t * server;
    int thread_id;
    int packets;
    uint64_t wire_bytes_sum;
};

static void rocketnet_test_wire_hammer_thread( void * data )
{
    struct rocketnet_test_wire_hammer_args_t * args = (struct rocketnet_test_wire_hammer_args_t*) data;
    uint64_t sum = 0;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( args->server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        for ( int j = 2; j < 16; j++ )
            payload[j] = (uint8_t) ( args->thread_id * 31 + i * 7 + j );
        const int wire_bytes = rocketnet_server_transport_finish_packet( args->server, &context, 16 );
        rocketnet_check( wire_bytes > 0 );
        sum += (uint64_t) wire_bytes;
    }
    args->wire_bytes_sum = sum;
}

void rocketnet_test_transport_disconnect_delivery()
{
    // the disconnect burst through the TRANSPORT, not handed to the client
    // protocol by hand -- the reviewer found the transport's disconnect
    // producer writing at the wrong frame offset while the protocol-level
    // test stayed green, because it never crossed the transport. this one
    // crosses it: the client must reach DISCONNECTED long before its 10s
    // timeout could explain it.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_server_transport_disconnect_client( server, 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 3.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_reconnect_churn()
{
    // player churn over real loopback: with one slot, five clients in turn
    // each connect with a fresh token and leave cleanly. every one connects;
    // a closed connection frees its slot at once, and nothing about the
    // tokens that came before denies the next one (tla/RNSecurityTables.tla,
    // ChurnNeverDenied; docs/SESSION.md, Token reuse). before the fix the
    // third was denied with the slot free.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 1;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    for ( int cycle = 0; cycle < 5; cycle++ )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
        rocketnet_check( client );

        double start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 )
        {
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );
            if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
                 rocketnet_client_transport_state( client ) < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
                break;
            rocketnet_platform_sleep( 0.001 );
        }
        if ( rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            printf( "reconnect churn: cycle %d of 5 ended in client state %d\n", cycle + 1, rocketnet_client_transport_state( client ) );
        rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

        rocketnet_client_transport_disconnect( client );

        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 3.0 && rocketnet_server_transport_num_connected_clients( server ) != 0 )
        {
            // the client keeps updating while the server waits: a disconnect burst whose
            // hand-off found the other send ring busy is deferred and goes out on
            // the client's next update (sdk/rocketnet_client_transport.cpp
            // rocketnet_client_transport_disconnect and _fill_destination), not before
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

        ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    }

    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

struct rocketnet_test_state_recorder_t
{
    int count;
    int previous_state[16];
    int current_state[16];
};

static void rocketnet_test_state_change_callback( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state )
{
    struct rocketnet_test_state_recorder_t * recorder = (struct rocketnet_test_state_recorder_t *) context;
    if ( recorder->count < 16 )
    {
        recorder->previous_state[recorder->count] = previous_state;
        recorder->current_state[recorder->count] = current_state;
    }
    recorder->count++;
}

void rocketnet_test_client_disconnect_reports_state_change()
{
    // a client-initiated disconnect moves the protocol to DISCONNECTED
    // directly, inside the disconnect call, and never crosses an update.
    // that transition must reach the state-change callback the same way
    // update reports its crossings, or an application that watches for its
    // own disconnect never hears it.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_test_state_recorder_t recorder = { 0, {0}, {0} };

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    client_config.state_change_callback = rocketnet_test_state_change_callback;

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( &recorder, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_client_transport_disconnect( client );
    rocketnet_client_transport_update( client, rocketnet_platform_time() );

    // the recorded pairs must end with exactly one (CONNECTED -> DISCONNECTED),
    // and that pair must be the last thing the callback heard: the disconnect
    // call reports it, and the update after it adds nothing

    rocketnet_check( recorder.count >= 1 );
    rocketnet_check( recorder.count <= 16 );

    int seen = 0;
    for ( int i = 0; i < recorder.count && i < 16; i++ )
    {
        if ( recorder.previous_state[i] == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             recorder.current_state[i] == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            seen++;
    }
    rocketnet_check( seen == 1 );

    const int last = recorder.count - 1;
    rocketnet_check( recorder.previous_state[last] == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( recorder.current_state[last] == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

// the hand-off round trip on one client transport; name is the case's name for
// its report line
static void rocketnet_test_client_io_handoff_round_trip( const char * transport, const char * name )
{
    // docs/CLIENT-IO.md Per-platform cards: a client transport on a platform
    // with the wake runs the I/O thread, so the main thread never walks the
    // send ring. A deferred hand-off loses
    // nothing: the committed slots stay in the Filling ring and leave with the
    // next hand-off (RNClientIo HandOff and Defer), so every payload arrives.

    // This fixture exercises the native client's I/O hand-off, including when
    // the surrounding suite selects loopback. Keep its UDP server and native
    // client selection local; the transport-matrix fixtures still honour pins.
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof(server_config.transport), "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "portable" ) == 0 );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "%s", transport );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), transport ) == 0 );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    // the transport reports the I/O path with its helper (sdk/rocketnet_client_io.h):
    // a PORTABLE client with wait and wake runs the I/O thread, not the BASE
    // receive thread, so the hand-off below is off the main thread.
    rocketnet_check( rocketnet_client_transport_client_io( client ) != NULL );

    const int FRAMES = 200;
    const int PACKETS_PER_FRAME = 32;
    const int PAYLOAD_BYTES = 200;
    const int TOTAL = FRAMES * PACKETS_PER_FRAME;

    static uint8_t seen[TOTAL];
    memset( seen, 0, sizeof(seen) );
    int received = 0;
    int start_refusals = 0;

    for ( int frame = 0; frame < FRAMES; frame++ )
    {
        for ( int i = 0; i < PACKETS_PER_FRAME; i++ )
        {
            const int tag = frame * PACKETS_PER_FRAME + i;
            struct rocketnet_packet_context_t context = {};
            uint8_t * payload = NULL;
            const double fill_deadline = rocketnet_platform_time() + 5.0;
            while ( !payload && rocketnet_platform_time() < fill_deadline )
            {
                payload = rocketnet_client_transport_start_packet( client, PAYLOAD_BYTES, &context );
                if ( !payload )
                {
                    // ring-full backpressure: the Filling ring is full (256
                    // slots) while the I/O thread is behind, so a start is
                    // deferred; update so a handed-off ring is retried with
                    // the same frame tag (RNClientIo Write, Defer). Each
                    // refused start counts SEND_RING_FULL once.
                    start_refusals++;
                    rocketnet_client_transport_update( client, rocketnet_platform_time() );
                    rocketnet_server_transport_update( server, rocketnet_platform_time() );
                    rocketnet_server_transport_send_packets( server );
                    const int bp_payloads = rocketnet_server_transport_num_payloads( server );
                    for ( int k = 0; k < bp_payloads; k++ )
                    {
                        int payload_client_index, payload_bytes;
                        uint8_t * bp = rocketnet_server_transport_payload( server, k, &payload_client_index, &payload_bytes );
                        rocketnet_check( payload_client_index == 0 );
                        rocketnet_check( payload_bytes == PAYLOAD_BYTES );
                        const int bp_tag = bp[0] | ( bp[1] << 8 );
                        rocketnet_check( bp_tag >= 0 && bp_tag < TOTAL );
                        for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                            rocketnet_check( bp[j] == (uint8_t) ( bp_tag * 31 + j ) );
                        rocketnet_check( !seen[bp_tag] );
                        seen[bp_tag] = 1;
                        received++;
                    }
                    rocketnet_platform_sleep( 0.001 );
                }
            }
            rocketnet_check( payload );
            payload[context.payload_index + 0] = (uint8_t) ( tag & 0xFF );
            payload[context.payload_index + 1] = (uint8_t) ( tag >> 8 );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                payload[context.payload_index + j] = (uint8_t) ( tag * 31 + j );
            rocketnet_check( rocketnet_client_transport_finish_packet( client, &context, PAYLOAD_BYTES ) > PAYLOAD_BYTES );
        }

        rocketnet_client_transport_send_packets( client );
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == PAYLOAD_BYTES );
            const int tag = payload[0] | ( payload[1] << 8 );
            rocketnet_check( tag >= 0 && tag < TOTAL );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                rocketnet_check( payload[j] == (uint8_t) ( tag * 31 + j ) );
            rocketnet_check( !seen[tag] );
            seen[tag] = 1;
            received++;
        }

        static uint8_t drain[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int drain_bytes;
        while ( ( drain_bytes = rocketnet_client_transport_receive_packet( client, drain, (int) sizeof(drain) ) ) > 0 )
            rocketnet_check( drain_bytes == PAYLOAD_BYTES );

        // the Filling ring holds 256 slots; pace the fill so the I/O thread
        // takes a handed-off ring before the next frames fill past it
        rocketnet_platform_sleep( 0.001 );
    }

    // the last ring leaves while the transports keep updating; a deferred
    // hand-off is retried every update (docs/CLIENT-IO.md Reuse only after sent)
    start = rocketnet_platform_time();
    while ( received < TOTAL && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == PAYLOAD_BYTES );
            const int tag = payload[0] | ( payload[1] << 8 );
            rocketnet_check( tag >= 0 && tag < TOTAL );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                rocketnet_check( payload[j] == (uint8_t) ( tag * 31 + j ) );
            rocketnet_check( !seen[tag] );
            seen[tag] = 1;
            received++;
        }

        static uint8_t drain[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int drain_bytes;
        while ( ( drain_bytes = rocketnet_client_transport_receive_packet( client, drain, (int) sizeof(drain) ) ) > 0 )
            rocketnet_check( drain_bytes == PAYLOAD_BYTES );

        if ( received < TOTAL )
            rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == TOTAL );

    // deferral is the common case, not a failure: a deferred hand-off loses
    // nothing, so the count is reported and every payload above still arrived
    const uint64_t deferred = rocketnet_counter_value( rocketnet_client_transport_counters( client ),
        ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED );
    printf( "%s frames=%d packets_per_frame=%d received=%d/%d deferred=%llu\n",
        name, FRAMES, PACKETS_PER_FRAME, received, TOTAL, (unsigned long long) deferred );

    // disconnect, then destroy while a ring is handed off: the disconnect
    // burst is committed to the Filling ring and handed off (or handed off by
    // destroy), and destroy runs Stop, Join, close, free (docs/CLIENT-IO.md
    // Stop and destroy order). The server must still read the disconnect.
    rocketnet_client_transport_disconnect( client );

    // ring-full backpressure is a counted refusal, not a loss: every refused
    // start above was retried with its tag and every payload arrived. The
    // counter covers at least those refusals; it may also count a protocol
    // packet an update committed while the ring was full, which carries no
    // payload. Every other counter stays clean.
    const uint64_t ring_full = rocketnet_counter_value( rocketnet_client_transport_counters( client ),
        ROCKETNET_COUNTER_SEND_RING_FULL );
    printf( "%s start_refusals=%d send_ring_full=%llu\n", name, start_refusals, (unsigned long long) ring_full );
    rocketnet_check( ring_full >= (uint64_t) start_refusals );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( client, ROCKETNET_COUNTER_SEND_RING_FULL );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void rocketnet_test_client_io_linux_handoff_round_trip()
{
    rocketnet_test_client_io_handoff_round_trip( "portable", "test_client_io_linux_handoff_round_trip" );
}

void rocketnet_test_client_io_apple_handoff_round_trip()
{
    // docs/CLIENT-IO.md Per-platform cards, rocketnet-client-io-apple: on
    // macOS and iOS the msgx client has the kqueue wake, so it runs the I/O
    // thread (RNClientIo HandOff, Defer, IoSend, IoReceive, IoWait) with its
    // batches through the sendmsg_x/recvmsg_x pump; the same round trip as
    // the Linux case, every payload byte-exact and none lost to a deferral.
    rocketnet_test_client_io_handoff_round_trip( "msgx", "test_client_io_apple_handoff_round_trip" );
}

void rocketnet_test_client_io_windows_handoff_round_trip()
{
    // docs/CLIENT-IO.md Per-platform cards, rocketnet-client-io-windows: on
    // Windows the portable client has the event wake (WSAEventSelect FD_READ
    // and a manual-reset event), so it runs the I/O thread (RNClientIo
    // HandOff, Defer, IoSend, IoReceive, IoWait); the same round trip as the
    // Linux case, every payload byte-exact and none lost to a deferral.
    rocketnet_test_client_io_handoff_round_trip( "portable", "test_client_io_windows_handoff_round_trip" );
}

static int rocketnet_test_fill_server_ring( rocketnet_server_transport_t * server, int completion_capacity )
{
    // IOCP may still own earlier sends. Keep every successful reservation in
    // this fill, without flushing, while completion workers return the rest.
    // A lost frame still fails the exact-capacity check after a bounded wait.
    const double deadline = rocketnet_platform_time() + 5.0;
    int count = 0;
    while ( count < 20000 )
    {
        rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        if ( !frame )
        {
            if ( count < completion_capacity && rocketnet_platform_time() < deadline )
            {
                rocketnet_platform_sleep( 0.001 );
                continue;
            }
            break;
        }
        frame[context.payload_index] = (uint8_t) count;
        rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 1 ) > 0 );
        count++;
    }
    if ( completion_capacity && count != completion_capacity )
        printf( "ring fill: transport=%s accepted=%d expected=%d\n", rocketnet_server_transport_io_name( server ), count, completion_capacity );
    return count;
}

void rocketnet_test_transport_ring_exhaustion()
{
    // fill past the ring, and the overflow refuses cleanly: starts beyond
    // capacity return NULL, the counter names the reason, and the full
    // capacity returns after flush and any asynchronous send completions.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // Synchronous transports are measured at their first refusal. IOCP's pool
    // has a known size, but even handshake sends may still be retiring at
    // the first fill. Wait for that whole pool, not an incidental count.
    int completion_capacity = 0;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( strcmp( rocketnet_server_transport_io_name( server ), "iocp" ) == 0 )
        completion_capacity = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
#endif
    const int capacity = rocketnet_test_fill_server_ring( server, completion_capacity );
    rocketnet_check( capacity >= 256 );
    rocketnet_check( capacity < 20000 );
    rocketnet_check( completion_capacity == 0 || capacity == completion_capacity );

    // refusal holds until the flush -- 44 more starts all refuse, and the
    // counter attributes every refusal exactly (the fill already incurred
    // refusals, so the baseline is read here)

    const uint64_t ring_full_before = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int i = 0; i < 44; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context ) == NULL );
    }
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full_before + 44 );

    rocketnet_server_transport_send_packets( server );

    // Flush submits IOCP sends; it does not synchronously reclaim them.
    // Accumulate returned frames in this one fill until the same exact
    // capacity is reached. No flush or transport update assists the refill.
    const int refill = rocketnet_test_fill_server_ring( server, completion_capacity );
    if ( refill != capacity )
        printf( "ring refill: transport=%s first=%d second=%d\n", rocketnet_server_transport_io_name( server ), capacity, refill );
    rocketnet_check( refill == capacity );

    rocketnet_server_transport_send_packets( server );

    // the next frame is whole: fresh starts succeed and one arrives. the
    // payload is unreliable and the flood may have left the client's kernel
    // receive buffer overflowing -- a transport with deep staging floods far
    // past what the buffer holds, and packets lost there are priced as lost
    // on the wire -- so recovery sends a fresh "again" every iteration
    // until one is observed, the transport's own retry idiom.

    rocketnet_server_transport_update( server, rocketnet_platform_time() );

    int recovered = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        rocketnet_check( frame || completion_capacity );
        if ( frame )
        {
            memcpy( frame + context.payload_index, "again", 5 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 5 ) > 0 );
        }
        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );
        for ( int i = 0; i < num_payloads; i++ )
        {
            if ( payload_bytes[i] == 5 && memcmp( payload_data[i], "again", 5 ) == 0 )
                recovered = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( recovered );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( client, ROCKETNET_COUNTER_SEND_RING_FULL, ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER_EXCEPT( server, ROCKETNET_COUNTER_SEND_RING_FULL, ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW );
}

struct rocketnet_test_client_hammer_args_t
{
    struct rocketnet_client_transport_t * client;
    int thread_id;
    int packets;
};

static void rocketnet_test_client_hammer_thread( void * data )
{
    struct rocketnet_test_client_hammer_args_t * args = (struct rocketnet_test_client_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( args->client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        rocketnet_check( rocketnet_client_transport_finish_packet( args->client, &context, 2 ) > 0 );
    }
}

void rocketnet_test_transport_client_concurrent_fill()
{
    // the client's fill phase under threads, mirrored from the server's
    // hammer: four fillers, one flush, every packet arrives distinct.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 40;

    struct rocketnet_test_client_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].client = client;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_client_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_client_transport_send_packets( client );

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * client_index;
        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_server_transport_payloads( server, &client_index, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( client_index[i] == 0 );
            rocketnet_check( payload_bytes[i] == 2 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_concurrent_fill()
{
    // the lock-free claim under real threads: four fillers acquire, write
    // and seal concurrently in one fill phase, the main thread flushes, and
    // every packet arrives distinct -- which also proves every sequence was
    // unique, because the client replay window would eat a duplicate.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 50;

    struct rocketnet_test_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].server = server;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_server_transport_send_packets( server );

    // every one of the two hundred distinct packets arrives

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( payload_bytes[i] == 16 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            for ( int j = 2; j < 16; j++ )
                rocketnet_check( payload_data[i][j] == (uint8_t) ( thread_id * 31 + index * 7 + j ) );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_wire_totals_concurrent_send_exact()
{
    // the four-thread hammer of test_transport_concurrent_fill, then pin the
    // whole-server and per-client sent wire totals: a lost increment from a
    // non-atomic add fails these exact asserts even without tsan.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_server_transport_send_packets( server );

    struct rocketnet_server_transport_stats_t base_server;
    memset( &base_server, 0, sizeof(base_server) );
    rocketnet_server_transport_stats( server, &base_server );
    struct rocketnet_server_transport_client_stats_t base_slot;
    memset( &base_slot, 0, sizeof(base_slot) );
    rocketnet_server_transport_client_stats( server, 0, &base_slot );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 50;
    struct rocketnet_test_wire_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];
    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].server = server;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        args[t].wire_bytes_sum = 0;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_wire_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    uint64_t total_wire = 0;
    for ( int t = 0; t < NUM_THREADS; t++ )
        total_wire += args[t].wire_bytes_sum;

    rocketnet_server_transport_send_packets( server );

    const uint64_t expected_packets = (uint64_t) ( NUM_THREADS * PACKETS_PER_THREAD );
    const uint64_t overhead = (uint64_t) ( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

    struct rocketnet_server_transport_stats_t sstats;
    memset( &sstats, 0, sizeof(sstats) );
    rocketnet_server_transport_stats( server, &sstats );
    rocketnet_check( sstats.packets_sent - base_server.packets_sent == expected_packets );
    rocketnet_check( sstats.bytes_sent - base_server.bytes_sent == total_wire + expected_packets * overhead );

    struct rocketnet_server_transport_client_stats_t cstats;
    memset( &cstats, 0, sizeof(cstats) );
    rocketnet_server_transport_client_stats( server, 0, &cstats );
    rocketnet_check( cstats.packets_sent - base_slot.packets_sent == expected_packets );
    rocketnet_check( cstats.bytes_sent - base_slot.bytes_sent == total_wire + expected_packets * overhead );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

}

static int rocketnet_test_shape_asserts = 0;

static void rocketnet_test_shape_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    rocketnet_test_shape_asserts++;
}

void rocketnet_test_transport_start_finish_shape()
{
    // the acquire/commit surface, proven over loopback: aligned roots, the
    // documented layout, seal in place, oversize refused, aborts harmless.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // a packet before connect for a bogus slot is refused at start

    struct rocketnet_packet_context_t context;
    rocketnet_check( rocketnet_server_transport_start_packet( server, 3, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context ) == NULL );

    // acquire: the root and the payload pointer are both 16-aligned, the
    // layout is the documented one, and the capacity is qword-clean

    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_check( ( (uintptr_t) frame % 16 ) == 0 );
    rocketnet_check( context.payload_index == ROCKETNET_FRAME_PAYLOAD_OFFSET );
    rocketnet_check( ( (uintptr_t) ( frame + context.payload_index ) % 16 ) == 0 );
    rocketnet_check( context.payload_capacity == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rocketnet_check( context.payload_capacity % 8 == 0 );
    rocketnet_check( context.sequence == 0 );

    // Packet metadata is a view, never authority over native storage.
    int checked_capacity = 0;
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    rocketnet_check(checked_capacity == context.payload_capacity);
    rocketnet_check(rocketnet_client_transport_packet_payload(client, &context, &checked_capacity) == NULL && checked_capacity == 0);
    for (int field = 0; field < 6; ++field)
    {
        auto changed = context;
        switch(field)
        {
            case 0: changed.frame++; break;
            case 1: changed.packet_id++; break;
            case 2: changed.sequence++; break;
            case 3: changed.client_index = -1; break;
            case 4: changed.payload_index = -1; break;
            case 5: changed.payload_capacity = INT_MAX; break;
        }
        rocketnet_check(rocketnet_server_transport_packet_payload(server, &changed, &checked_capacity) == NULL);
        auto aborted = changed;
        rocketnet_server_transport_abort_packet(server, &aborted);
        rocketnet_check(aborted.frame == NULL);
        rocketnet_test_shape_asserts = 0;
        rocketnet_assert_function( rocketnet_test_shape_assert_hook );
        rocketnet_check(rocketnet_server_transport_finish_packet(server, &changed, 1) == 0);
        rocketnet_assert_function( NULL );
        rocketnet_check( rocketnet_test_shape_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    }
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    auto expired = context;

    // oversize is refused at finish and consumes nothing visible

    rocketnet_server_transport_finish_packet( server, &context, context.payload_capacity + 1 );
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &expired, &checked_capacity) == NULL);
    rocketnet_test_shape_asserts = 0;
    rocketnet_assert_function( rocketnet_test_shape_assert_hook );
    rocketnet_check(rocketnet_server_transport_finish_packet(server, &expired, 1) == 0);
    rocketnet_assert_function( NULL );
    rocketnet_check( rocketnet_test_shape_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    // an aborted packet is harmless

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_server_transport_abort_packet( server, &context );

    for (int arena = 0; arena < 2; ++arena)
    {
    // commit: write at the payload pointer, finish, flush, and the client
    // receives exactly those bytes

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + arena, &context );
    rocketnet_check( frame );
    rocketnet_check( context.payload_index == (arena ? 0 : ROCKETNET_FRAME_PAYLOAD_OFFSET) );
    memcpy( frame + context.payload_index, "shape", 5 );
    rocketnet_server_transport_finish_packet( server, &context, 5 );
    rocketnet_server_transport_send_packets( server );

    int received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "shape", 5 ) == 0 );
            received = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received );

    struct rocketnet_packet_context_t reply;
    uint8_t * reply_frame = rocketnet_client_transport_start_packet(client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + arena, &reply);
    rocketnet_check(reply_frame);
    rocketnet_check(reply.payload_index == (arena ? 0 : ROCKETNET_FRAME_PAYLOAD_OFFSET));
    memcpy(reply_frame + reply.payload_index, "reply", 5);
    rocketnet_check(rocketnet_client_transport_finish_packet(client, &reply, 5) > 0);
    rocketnet_client_transport_send_packets(client);
    bool replied = false;
    start = rocketnet_platform_time();
    while (!replied && rocketnet_platform_time() - start < 5.0)
    {
        rocketnet_client_transport_update(client, rocketnet_platform_time());
        rocketnet_server_transport_update(server, rocketnet_platform_time());
        if (rocketnet_server_transport_num_payloads(server))
        {
            int index, bytes;
            uint8_t * payload = rocketnet_server_transport_payload(server, 0, &index, &bytes);
            rocketnet_check(payload && index == 0 && bytes == 5 && memcmp(payload, "reply", 5) == 0);
            rocketnet_check(rocketnet_server_transport_num_payloads(server) == 1);
            replied = true;
        }
        if (!replied) rocketnet_platform_sleep(0.001);
    }
    rocketnet_check(replied);
    }

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_client_receive_keeps_payloads_on_bad_capacity()
{
    // A capacity below one refuses at once: it consumes nothing, moves no
    // cursor and counts nothing, so all queued payloads stay for the next
    // call with a buffer that can hold them.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    // The server sends two payloads; the client queues them without any call to
    // receive_packet, so the cursor has not been touched. The batch is valid
    // until the next update, and with the client I/O thread the two datagrams
    // can be published in two batches (docs/CLIENT-IO.md "The receive path:
    // the handoff", RNClientIo IoPublish), so the pair is sent again until
    // one batch holds exactly this round's two, in order (read by index, so
    // the cursor stays untouched and a late payload of an earlier round is
    // never taken for this one).

    bool queued = false;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !queued )
    {
        rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "capacity", 8 );
        rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "second", 6 );
        rocketnet_server_transport_send_packets( server );

        const double sent = rocketnet_platform_time();
        while ( rocketnet_platform_time() - sent < 0.25 && !queued )
        {
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            int first_bytes = 0;
            int second_bytes = 0;
            const uint8_t * first = rocketnet_client_transport_num_payloads( client ) == 2 ? rocketnet_client_transport_payload( client, 0, &first_bytes ) : NULL;
            const uint8_t * second = first ? rocketnet_client_transport_payload( client, 1, &second_bytes ) : NULL;
            if ( first && second && first_bytes == 8 && memcmp( first, "capacity", 8 ) == 0 &&
                 second_bytes == 6 && memcmp( second, "second", 6 ) == 0 )
                queued = true;
            else
                rocketnet_platform_sleep( 0.001 );
        }
    }
    rocketnet_check( queued );
    rocketnet_check( rocketnet_client_transport_num_payloads( client ) == 2 );

    const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( client );
    const uint64_t refused_before = counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL];

    // A capacity of zero (and a negative one) returns at once without
    // consuming: the payload is still the cursor's next.

    uint8_t tiny[1];
    rocketnet_check( rocketnet_client_transport_receive_packet( client, tiny, 0 ) == 0 );
    rocketnet_check( rocketnet_client_transport_receive_packet( client, tiny, -8 ) == 0 );
    rocketnet_check( rocketnet_client_transport_receive_packet( client, tiny, -1 ) == 0 );
    rocketnet_check( counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == refused_before );

    // Both queued payloads drain intact in send order.

    const int capacity = rocketnet_client_transport_max_payload_bytes( client );
    rocketnet_check( capacity >= 8 );
    static uint8_t packet[ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES];
    int packet_bytes = rocketnet_client_transport_receive_packet( client, packet, capacity );
    rocketnet_check( packet_bytes == 8 );
    rocketnet_check( memcmp( packet, "capacity", 8 ) == 0 );

    // A zero-capacity call between payloads also leaves the remainder queued.

    rocketnet_check( rocketnet_client_transport_receive_packet( client, tiny, 0 ) == 0 );
    rocketnet_check( counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == refused_before );

    packet_bytes = rocketnet_client_transport_receive_packet( client, packet, capacity );
    rocketnet_check( packet_bytes == 6 );
    rocketnet_check( memcmp( packet, "second", 6 ) == 0 );

    // The batch is drained, so subsequent calls return zero.

    rocketnet_check( rocketnet_client_transport_receive_packet( client, packet, capacity ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_udp_loopback()
{
    // the first packets on a real socket: the whole connect over UDP through
    // loopback, on the transports -- the IO carrying the connection
    // protocol, development keys, exactly the out-of-the-box path. real
    // clocks here; the protocol underneath is the one proven on mocked time.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t loopback_connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), loopback_connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, loopback_connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // payloads both ways through the transport surfaces
    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        // out-of-range indices answer safely rather than trusting the caller
        int oob_client_index, oob_payload_bytes;
        rocketnet_check( rocketnet_server_transport_payload( server, num_payloads, &oob_client_index, &oob_payload_bytes ) == NULL );
        rocketnet_check( rocketnet_server_transport_payload( server, -1, &oob_client_index, &oob_payload_bytes ) == NULL );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "world", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    // the deliberate disconnect crosses the wire and the server empties
    rocketnet_client_transport_disconnect( client );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 )
    {
        // the client keeps updating while the server waits: a disconnect burst whose
        // hand-off found the other send ring busy is deferred and goes out on
        // the client's next update (sdk/rocketnet_client_transport.cpp
        // rocketnet_client_transport_disconnect and _fill_destination), not before
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    // Stop may need to drain asynchronous I/O. The public contract requires
    // waiting for STOPPED, not a particular number of updates.
    start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

#if ROCKETNET_DEV
void rocketnet_test_client_ipv4_without_ipv6_stack()
{
    // a host with no IPv6 stack must still reach an IPv4 server. the seam
    // makes every IPv6 socket create fail as EAFNOSUPPORT does on such a
    // host, so the bind-family choice is exercised on every build platform.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof(ipv4_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );
    snprintf( ipv6_address, sizeof(ipv6_address), "[::1]:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * token_addresses[] = { ipv4_address, ipv6_address };
    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() );
    rocketnet_check( rocketnet_write_production_connect_token( rocketnet_dev_backend_secret_key,
                     rocketnet_dev_server_public_key, ROCKETNET_DEV_PROTOCOL_ID, token_addresses, 2,
                     rocketnet_random_uint64(), create_timestamp,
                     create_timestamp + ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                     ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, NULL,
                     connect_token, sizeof(connect_token) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_connect_token_t parsed_token;
    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_connect_token( &parsed_token, connect_token );
    rocketnet_read_signed_connect_data( &signed_fields, parsed_token.signed_connect_data );
    rocketnet_check( signed_fields.address_flags == ( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) );

    rocketnet_platform_set_ipv6_socket_available_for_testing( false );

    rocketnet_test_captured_log[0] = '\0';
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_function( rocketnet_test_capture_log, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );

    rocketnet_log_level( saved_level );
    rocketnet_log_function( rocketnet_test_suite_log_function, NULL );

    rocketnet_check( client );
    rocketnet_check( strstr( rocketnet_test_captured_log, "client socket is ipv4 because ipv6 is unavailable on this host" ) != NULL );

    // the same connect and payload exchange as the udp loopback, over IPv4
    // while the host reports no IPv6 stack

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "world", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( sent_to_server == 10 );
    rocketnet_check( sent_to_client == 10 );
    rocketnet_check( server_received == 10 );
    rocketnet_check( client_received == 10 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    // an IPv6 server is unreachable without IPv6: the create refuses with
    // the existing ladder-exhausted status and says why once.

    rocketnet_check( rocketnet_functional_write_token( "[::1]:9", rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    int create_status = ROCKETNET_STATUS_OK;
    rocketnet_test_captured_log[0] = '\0';
    rocketnet_log_function( rocketnet_test_capture_log, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    struct rocketnet_client_transport_t * refused = rocketnet_client_transport_create_ex( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time(), &create_status );

    rocketnet_log_level( saved_level );
    rocketnet_log_function( rocketnet_test_suite_log_function, NULL );

    rocketnet_check( refused == NULL );
    rocketnet_check( create_status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    rocketnet_check( strstr( rocketnet_test_captured_log, "ipv6 is unavailable on this host" ) != NULL );

    rocketnet_platform_set_ipv6_socket_available_for_testing( true );
}
#endif // #if ROCKETNET_DEV

void rocketnet_test_transport_stats()
{
    // the bucket rings on both transports over UDP loopback, on a synthetic
    // game clock: the published send rates equal an independent recount of
    // the sends this test made under the same trailing-one-second rule, and
    // after a second of idle game time every published rate is zero --
    // stale buckets drop out

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, game_time ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), game_time );
    rocketnet_check( client );

    // connect on the synthetic clock, bounded by the wall clock

    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // two simulated seconds of traffic, one payload each way per frame. the
    // send times and cumulative wire-counter deltas are recorded so rates can be recounted
    // independently; two seconds ages the handshake's packets out of the
    // window. RN-1 t15 standalone acknowledgments are wire packets too;
    // the rate must include them, independently of the payload count.

    const int num_traffic_frames = 120;

    double send_times[num_traffic_frames];
    uint64_t client_sends[num_traffic_frames], server_sends[num_traffic_frames];
    struct rocketnet_client_transport_stats_t client_stats;
    struct rocketnet_server_transport_stats_t server_stats;
    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_server_transport_stats( server, &server_stats );
    uint64_t client_sent = client_stats.packets_sent;
    uint64_t server_sent = server_stats.packets_sent;

    uint8_t payload_data[100];
    memset( payload_data, 0x5A, sizeof(payload_data) );

    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        game_time += 1.0 / 60.0;

        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );

        rocketnet_client_transport_send_packet( client, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );

        send_times[i] = game_time;
        rocketnet_client_transport_stats( client, &client_stats );
        rocketnet_server_transport_stats( server, &server_stats );
        client_sends[i] = client_stats.packets_sent - client_sent;
        server_sends[i] = server_stats.packets_sent - server_sent;
        client_sent = client_stats.packets_sent;
        server_sent = server_stats.packets_sent;

        uint8_t scratch[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( client, scratch, (int) sizeof(scratch) ) > 0 ) {}

        rocketnet_platform_sleep( 0.001 );
    }

    // one more update pair at the final time publishes the window sums

    rocketnet_client_transport_update( client, game_time );
    rocketnet_server_transport_update( server, game_time );

    // the independent recount, under the published numbers' own rule: a
    // send contributes while its bucket is younger than one second

    uint64_t expected_client_sends = 0, expected_server_sends = 0;
    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        if ( send_times[i] + 1.0 >= game_time )
        {
            expected_client_sends += client_sends[i];
            expected_server_sends += server_sends[i];
        }
    }
    rocketnet_check( expected_client_sends > 0 && expected_server_sends > 0 );

    rocketnet_client_transport_stats( client, &client_stats );

    rocketnet_check( client_stats.packets_sent_per_second == (int) expected_client_sends );
    rocketnet_check( client_stats.packets_received_per_second > 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps > 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps > 0 );
    rocketnet_check( client_stats.rtt > 0.0f );
    rocketnet_check( client_stats.rtt < 1000.0f );
    rocketnet_check( client_stats.jitter >= 0.0f );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    rocketnet_server_transport_stats( server, &server_stats );

    rocketnet_check( server_stats.packets_sent_per_second == expected_server_sends );
    rocketnet_check( server_stats.packets_received_per_second > 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps > 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps > 0.0f );

    // stale bucket exclusion: disconnect, then a second and a half of idle
    // game time. nothing is sent or received, the traffic buckets age past
    // the window, and every published rate returns to zero. responses to
    // packets already in flight can arrive after the disconnect, so the
    // idle window starts over whenever either transport receives one.

    rocketnet_client_transport_disconnect( client );

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_server_transport_stats( server, &server_stats );

    uint64_t client_received_seen = client_stats.packets_received;
    uint64_t server_received_seen = server_stats.packets_received;

    int idle_frames = 0;
    int total_frames = 0;
    while ( idle_frames < 90 && total_frames < 1800 )
    {
        total_frames++;
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_client_transport_stats( client, &client_stats );
        rocketnet_server_transport_stats( server, &server_stats );
        if ( client_stats.packets_received != client_received_seen ||
             server_stats.packets_received != server_received_seen )
        {
            client_received_seen = client_stats.packets_received;
            server_received_seen = server_stats.packets_received;
            idle_frames = 0;
        }
        else
            idle_frames++;
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packets_sent_per_second == 0 );
    rocketnet_check( client_stats.packets_received_per_second == 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps == 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps == 0 );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    rocketnet_server_transport_stats( server, &server_stats );
    rocketnet_check( server_stats.packets_sent_per_second == 0 );
    rocketnet_check( server_stats.packets_received_per_second == 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps == 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps == 0.0f );

    // the server heard the disconnect burst: a burst whose hand-off the I/O
    // thread deferred is retried by the next update after the connection
    // reset, and still goes to the server (rocketnet_client_transport_io_hand_off).
    // A burst that never arrives leaves the server sending keep-alives the
    // client counts as received.

    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_client_ack_stats_computed_on_read()
{
    // the ack statistics are computed when they are read, not on every
    // update: update must not touch the counter, and one stats getter call
    // advances it by exactly one

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 1000.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, game_time ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), game_time );
    rocketnet_check( client );

    // connect on the synthetic clock, bounded by the wall clock

    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // sixty frames of traffic, one payload each way, acks flowing

    uint8_t payload_data[64];
    memset( payload_data, 0x5A, sizeof(payload_data) );

    for ( int i = 0; i < 60; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );

        rocketnet_client_transport_send_packet( client, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );

        uint8_t scratch[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( client, scratch, (int) sizeof(scratch) ) > 0 ) {}

        rocketnet_platform_sleep( 0.001 );
    }

    // (a) client-only updates must not recompute the ack statistics

    const unsigned long long before = g_client_ack_stats_count.load( std::memory_order_relaxed );
    for ( int i = 0; i < 30; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
    }
    rocketnet_check( g_client_ack_stats_count.load( std::memory_order_relaxed ) == before );

    // (b) one stats read recomputes them exactly once

    struct rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_stats( client, &stats );
    rocketnet_check( g_client_ack_stats_count.load( std::memory_order_relaxed ) == before + 1 );

    // (c) sane rtt and zero loss on the clean loopback

    rocketnet_check( stats.rtt > 0.0f && stats.rtt < 1000.0f );
    rocketnet_check( stats.packet_loss == 0.0f );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

// the NaN update below is loud in a debug build: the clamp asserts. the hook
// is non-aborting so the assert is observed rather than trapping the suite,
// the same pattern the unit tier uses for the clamp.

static int rocketnet_test_update_time_clamped_asserts = 0;

static void rocketnet_test_update_time_clamped_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    rocketnet_test_update_time_clamped_asserts++;
}

void rocketnet_test_transport_update_time_clamped()
{
    // RULING 9: a NaN update must not freeze a transport. a connected pair, a
    // NaN server update (clamped to the last good time, one counted misuse),
    // then valid updates with the client silent: the server still times the
    // client out at the token's timeout. without the clamp the NaN poisons the
    // clock and the misuse is never counted.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, game_time ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), game_time );
    rocketnet_check( client );

    // connect on the synthetic clock, bounded by the wall clock

    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    const uint64_t misuse_before = rocketnet_api_misuse_count();

    // one NaN update, clamped and counted; loud in a debug build, observed
    // through a non-aborting hook so the suite goes on

    rocketnet_test_update_time_clamped_asserts = 0;
    rocketnet_assert_function( rocketnet_test_update_time_clamped_assert_hook );

    rocketnet_server_transport_update( server, (double) NAN );

    rocketnet_assert_function( NULL );

    rocketnet_check( rocketnet_api_misuse_count() == misuse_before + 1 );

#if ROCKETNET_ASSERTS
    rocketnet_check( rocketnet_test_update_time_clamped_asserts == 1 );
#endif

    // the client now goes silent: only the server is updated, and its clock
    // advances past the token's ten second timeout

    double timeout_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - timeout_start < 5.0 )
    {
        game_time += 1.0;
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        if ( game_time > 100.0 + 20.0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_packet_loss_stats()
{
    // The client's packet_loss is outbound ACK loss: eligible retained sends
    // not yet acknowledged, and 0 when none are eligible. The server assigns
    // a session sequence only when finish commits the payload, so an aborted
    // start takes none. Clean traffic reads 0, and the aborts stay at 0.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, game_time ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), game_time );
    rocketnet_check( client );

    // This handshake uses real sockets and a real receive thread. Advancing
    // virtual time by 1/60 per 1 ms wait gives that thread only ~300 ms before
    // a five-second protocol deadline. Use actual elapsed time for connection;
    // the controlled virtual cadence below still drives the statistics case.
    const double connect_time = game_time;
    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time = connect_time + rocketnet_platform_time() - wall_start;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    if (rocketnet_client_transport_state(client) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        printf("packet loss test connect failed: %s, server clients %d\n",
            rocketnet_client_transport_state_name(rocketnet_client_transport_state(client)),
            rocketnet_server_transport_num_connected_clients(server));
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    uint8_t payload_data[64];
    memset( payload_data, 0xA5, sizeof(payload_data) );

    // Clean traffic. packet_loss is outbound ACK loss, not a receive-gap
    // count. These updates do not fill the newest half-window, so no send is
    // eligible and the published loss is 0.

    for ( int i = 0; i < 45; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    // Abort ten starts on the server. Each takes no session sequence. The
    // client's packet_loss is outbound ACK loss, and it stays 0: this run
    // still has no eligible retained send. 30 + 20 keeps the old iteration
    // count and is not a live safety margin.

    for ( int i = 0; i < 10; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context ) );
        rocketnet_server_transport_abort_packet( server, &context );
    }

    const int outbound_ack_loss_check_packets = 30 + 20;
    for ( int i = 0; i < outbound_ack_loss_check_packets; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    rocketnet_client_transport_disconnect( client );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

static void rocketnet_test_wait_socket_readable( rocketnet_platform_socket_t * socket, const char * phase )
{
    const double deadline = rocketnet_platform_time() + 30.0;
    for ( ;; )
    {
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( socket->handle, &readable );
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        const int ready = select( (int) socket->handle + 1, &readable, NULL, NULL, &tv );
        if ( ready > 0 )
            return;
        if ( ready < 0 && errno == EINTR )
            continue;
        if ( ready < 0 )
        {
            fprintf( stderr, "harness error: %s select failed (errno=%d)\n", phase, errno );
            rocketnet_check( 0 );
            return;
        }
        if ( rocketnet_platform_time() >= deadline )
        {
            fprintf( stderr, "harness timeout: %s did not become readable within the 30 s watchdog; this is a hung harness, not a transport performance failure\n", phase );
            rocketnet_check( 0 );
            return;
        }
    }
}

static void rocketnet_test_platform_socket_exchange( const char * address, int mode, float timeout )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, address );
    rocketnet_address_parse( &local_address, address );

    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, mode, timeout, 64*1024, 64*1024 );
    rocketnet_check( socket );
    local_address.port = bind_address.port;

    uint8_t sent[256];
    uint8_t received[256];
    for ( int i = 0; i < (int) sizeof(sent); i++ )
        sent[i] = (uint8_t) ( i * 37 + 11 );
    memset( received, 0, sizeof(received) );

    // The wrapper's send returns void by design (errors are dropped by
    // the platform layer), so a successful send is proven by the exact
    // receipt asserted below: length, source and byte-exact payload.
    rocketnet_platform_socket_send_packet( socket, &local_address, sent, (int) sizeof(sent) );

    // Every mode, including blocking with no timeout, first observes
    // readiness under the shared harness watchdog, so a missing datagram
    // fails as a harness timeout instead of hanging in the receive. The
    // blocking/no-timeout wrapper is still created and exercised by the
    // one receive below; readiness does not change its mode.
    rocketnet_address_t from;
    rocketnet_test_wait_socket_readable( socket, address );
    const int got = rocketnet_platform_socket_receive_packet( socket, &from, received, (int) sizeof(received) );
    rocketnet_check( got == (int) sizeof(sent) );

    rocketnet_check( from.type == bind_address.type );
    rocketnet_check( from.port == local_address.port );
    rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
    rocketnet_check( memcmp( received, sent, sizeof(sent) ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_socket()
{
    rocketnet_test_platform_socket_exchange( "127.0.0.1", ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f );
    rocketnet_test_platform_socket_exchange( "127.0.0.1", ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f );
    rocketnet_test_platform_socket_exchange( "127.0.0.1", ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f );
    rocketnet_test_platform_socket_exchange( "[::1]", ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f );
    rocketnet_test_platform_socket_exchange( "[::1]", ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f );
    rocketnet_test_platform_socket_exchange( "[::1]", ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

static void rocketnet_test_darwin_oversize_receive( const char * pump )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024, pump );
    rocketnet_check( socket != NULL );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );
    local_address.port = bind_address.port;

    uint8_t send_data[8192];
    for ( int i = 0; i < (int) sizeof(send_data); ++i )
        send_data[i] = (uint8_t) i;
    const int sizes[] = { ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1, (int) sizeof(send_data) };
    for ( int i = 0; i < 3; ++i )
    {
        rocketnet_platform_socket_send_packet( socket, &local_address, send_data, sizes[i] );
        struct
        {
            uint8_t before[16];
            uint8_t data[ROCKETNET_RECEIVE_CEILING];
            uint8_t after[16];
        } receive;
        memset( &receive, 0xA5, sizeof(receive) );
        rocketnet_address_t from;
        int packet_bytes = 0;
        const double start_time = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start_time < 5.0 )
        {
            if ( rocketnet_platform_socket_receive_packets( socket, &from, receive.data, sizeof(receive.data), &packet_bytes, 1 ) > 0 )
                break;
            rocketnet_platform_sleep( 0.001 );
        }
        if ( packet_bytes == 0 )
            fprintf( stderr, "harness timeout: darwin oversize datagram (%s, %d bytes) did not arrive within the 5 s watchdog; this is a hung harness, not a transport performance failure\n", pump, sizes[i] );
        rocketnet_check( packet_bytes == sizes[i] );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_check( memcmp( receive.data, send_data, sizeof(receive.data) ) == 0 );
        for ( int j = 0; j < 16; ++j )
        {
            rocketnet_check( receive.before[j] == 0xA5 );
            rocketnet_check( receive.after[j] == 0xA5 );
        }
        rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump ) == 0 );
    }
    rocketnet_platform_socket_destroy( socket );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_receive_drops_oversize_datagram()
{
    // The caller provides exactly ROCKETNET_RECEIVE_CEILING bytes, but
    // a larger datagram must retain its original length rather than appear
    // to be a valid packet truncated to the ceiling. Truncation happens in
    // the kernel, so this needs a real socket and lives here rather than over
    // the unit suite's in-memory simulator.

    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket != NULL );

    local_address.port = bind_address.port;

    // one byte past the fixed receive ceiling, with a valid prefilter header
    uint8_t send_data[ROCKETNET_RECEIVE_CEILING + 1];
    memset( send_data, 0, sizeof(send_data) );
    send_data[0] = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;

    rocketnet_platform_socket_send_packet( socket, &local_address, send_data, sizeof(send_data) );

    uint8_t receive_data[ROCKETNET_RECEIVE_CEILING];
    rocketnet_address_t from;
    int packet_bytes = 0;
    double start_time = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start_time < 5.0 )
    {
        const int num_received = rocketnet_platform_socket_receive_packets( socket, &from, receive_data, ROCKETNET_RECEIVE_CEILING, &packet_bytes, 1 );
        if ( num_received > 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }

    if ( packet_bytes == 0 )
        fprintf( stderr, "harness timeout: oversize datagram did not arrive within the 5 s watchdog; this is a hung harness, not a transport performance failure\n" );

    // the datagram is reported whole, not truncated to the ceiling
    rocketnet_check( packet_bytes == ROCKETNET_RECEIVE_CEILING + 1 );

#if ROCKETNET_DEV && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS) && ( ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC )
    // Live portable server + real token client, public CONNECTED handshake.
    // The private socket registry identifies the sole new client socket; the
    // over-ceiling datagram is sent through that admitted client socket. No
    // OS connect and no new seam. This leg exists only in the DEV functional
    // target; the raw-wrapper production coverage above always runs.
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 1;
    snprintf( server_config.transport, sizeof(server_config.transport), "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "portable" ) == 0 );

    char token_address[64];
    snprintf( token_address, sizeof(token_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( token_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "portable" );

    struct rocketnet_platform_socket_t * before[64];
    const int num_before = rocketnet_platform_sockets_for_testing( before, 64 );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );
    struct rocketnet_platform_socket_t * after[64];
    const int num_after = rocketnet_platform_sockets_for_testing( after, 64 );
    struct rocketnet_platform_socket_t * client_socket = NULL;
    for ( int i = 0; i < num_after; i++ )
    {
        bool old = false;
        for ( int j = 0; j < num_before; j++ )
            old = old || before[j] == after[i];
        if ( !old )
        {
            rocketnet_check( client_socket == NULL );
            client_socket = after[i];
        }
    }
    rocketnet_check( client_socket != NULL );

    rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "127.0.0.1" );
    server_address.port = rocketnet_server_transport_port( server );

    const double connect_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - connect_start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    if ( rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED || rocketnet_server_transport_num_connected_clients( server ) != 1 )
        fprintf( stderr, "harness timeout: live oversize handshake did not connect within the 5 s watchdog; this is a hung harness, not a transport performance failure\n" );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    const uint64_t before_too_large = server_counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];
    rocketnet_platform_socket_send_packet( client_socket, &server_address, send_data, (int) sizeof(send_data) );

    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( server_counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] > before_too_large )
            break;
        rocketnet_platform_sleep( 0.001 );
    }

    if ( server_counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == before_too_large )
        fprintf( stderr, "harness timeout: live server did not count the oversize datagram within the 5 s watchdog; this is a hung harness, not a transport performance failure\n" );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == before_too_large + 1 );
    rocketnet_check( rocketnet_server_transport_num_payloads( server ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER_EXCEPT( server, ROCKETNET_COUNTER_PREFILTER_TOO_LARGE );
#endif

    rocketnet_platform_socket_destroy( socket );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    rocketnet_test_darwin_oversize_receive( "msgx" );
    rocketnet_test_darwin_oversize_receive( "portable" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
}

// the batch pump seam (#100): every platform serves it -- portable loops
// underneath on most, darwin's batched syscalls when the runtime probe
// admits them -- and these tests pin the seam contract itself, so they run
// identically over either implementation. packets are verified by content
// (index and pattern), never by arrival order.

#define ROCKETNET_TEST_PUMP_PACKETS 100         // several pump chunks, with a short final chunk
#define ROCKETNET_TEST_PUMP_STRIDE  ROCKETNET_RECEIVE_CEILING        // the transport's own receive ceiling

// the pump each platform must be running when nothing forces portable: on
// macOS the msgx pump, everywhere else the portable loop. asserted inside
// the exchange so a silently-degraded transport can never pass the suite as
// "tested" -- the batch tests would otherwise green over the fallback.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "msgx"
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "portable"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

static void rocketnet_test_pump_exchange( const char * bind_string, const char * local_string, const char * expected_pump, const char * requested_pump = NULL )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 , requested_pump );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );
    local_address.port = bind_address.port;

    // batch send: varied sizes, a content pattern keyed by index

    static uint8_t send_data[ROCKETNET_TEST_PUMP_PACKETS][512];
    struct rocketnet_platform_send_t sends[ROCKETNET_TEST_PUMP_PACKETS];
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        const int bytes = 32 + i * 4;
        send_data[i][0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            send_data[i][j] = (uint8_t) ( i + j );
        sends[i].to = &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = bytes;
    }
    rocketnet_platform_socket_send_packets( socket, sends, ROCKETNET_TEST_PUMP_PACKETS );

    // batch receive: drain until everything sent has arrived. loopback
    // delivery can trickle, so poll under a deadline rather than assume
    // one call sees the batch whole.

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    int received = 0;
    double start_time = rocketnet_platform_time();
    while ( received < ROCKETNET_TEST_PUMP_PACKETS && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == ROCKETNET_TEST_PUMP_PACKETS );

    // every packet arrived whole, correctly sized, correctly attributed --
    // and exactly once

    bool seen[ROCKETNET_TEST_PUMP_PACKETS];
    memset( seen, 0, sizeof(seen) );
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        rocketnet_check( rocketnet_address_equal( &from[i], &local_address ) );
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        const int index = packet[0];
        rocketnet_check( index < ROCKETNET_TEST_PUMP_PACKETS );
        rocketnet_check( !seen[index] );
        seen[index] = true;
        rocketnet_check( packet_bytes[i] == 32 + index * 4 );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( index + j ) );
    }

    // a drained socket returns zero

    rocketnet_check( rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, ROCKETNET_TEST_PUMP_PACKETS ) == 0 );

    // a partial batch returns short without blocking: send 3, ask for many

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, send_data[i], 32 + i * 4 );

    received = 0;
    start_time = rocketnet_platform_time();
    while ( received < 3 && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == 3 );

    // the pump is still the expected one AFTER the exchange, not merely at
    // create. the create-time assertion pins only the probe (which
    // exercises recvmsg_x alone); a send-half-only breakage -- the
    // realistic ABI-drift regression -- would pause the pump mid-exchange
    // and finish portable, and the review demonstrated the whole suite
    // greening over exactly that. the pause outlives any test by design
    // (60 s), so a mid-exchange degradation cannot hide from this line.

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_batch_pump()
{
    rocketnet_test_pump_exchange( "127.0.0.1", "127.0.0.1", ROCKETNET_TEST_EXPECTED_PUMP );

    rocketnet_test_pump_exchange( "[::1]", "[::1]", ROCKETNET_TEST_EXPECTED_PUMP );
}

void rocketnet_test_platform_batch_pump_degraded()
{
    // The explicit portable request exercises the same seam and behavior as a
    // runtime degradation without adding another environment spelling.
    rocketnet_test_pump_exchange( "127.0.0.1", "127.0.0.1", "portable", "portable" );
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// zero-length chaff injection (#100's review): a zero-length datagram is
// legal UDP, and before this round the portable path treated one as
// "socket drained" -- byte-less chaff capped every drain at one packet, a
// standing DoS lever. both pumps now share skip-and-continue semantics,
// pinned here by injecting chaff between real packets from one raw socket
// (one source socket, so loopback preserves the interleaving) and
// requiring every real packet back, in order, contents intact -- which
// also proves the msgx compaction memmove, since kept packets after a
// mid-chunk skip must move down a slot.

// Completion observation, per injected datagram. On Linux,
// SK_MEMINFO_RMEM_ALLOC is socket-buffer allocation accounting (each
// queued datagram adds its skb truesize, overhead included), never a
// payload byte count: it is used only as a strict-growth signal, waited
// on after each individual send before the next send, so a growth is
// attributable to that one datagram. On macOS, FIONREAD is waited on
// until it reaches the cumulative payload of the real datagrams sent
// so far; zero-length chaff adds no payload there, and its arrival is
// carried by the single injector's FIFO order once the following real
// datagram is observed. Neither value is compared across platforms,
// and elapsed time is never a pass criterion: one shared 30 s watchdog
// only ends a hung harness and says so.
static int rocketnet_test_queue_accounting( rocketnet_platform_socket_t * socket )
{
#if defined(__APPLE__)
    int queued = 0;
    if ( ioctl( socket->handle, FIONREAD, &queued ) != 0 )
        return -1;
    return queued;
#elif defined(__linux__)
    uint32_t mem[SK_MEMINFO_VARS];
    socklen_t len = sizeof(mem);
    if ( getsockopt( socket->handle, SOL_SOCKET, SO_MEMINFO, mem, &len ) != 0 )
        return -1;
    return (int) mem[SK_MEMINFO_RMEM_ALLOC];
#else
    (void) socket;
    return -1;
#endif
}

struct rocketnet_test_queue_wait_t
{
    int accounting;
    int payload_bytes;
    double deadline;
};

static void rocketnet_test_queue_wait_init( rocketnet_platform_socket_t * socket, rocketnet_test_queue_wait_t * wait, const char * phase )
{
    wait->accounting = rocketnet_test_queue_accounting( socket );
    wait->payload_bytes = 0;
    wait->deadline = rocketnet_platform_time() + 30.0;
    if ( wait->accounting < 0 )
    {
        fprintf( stderr, "harness error: %s could not read the queue accounting (errno=%d)\n", phase, errno );
        rocketnet_check( 0 );
    }
}

static void rocketnet_test_queue_wait_datagram( rocketnet_platform_socket_t * socket, rocketnet_test_queue_wait_t * wait, int datagram_bytes, const char * phase )
{
    wait->payload_bytes += datagram_bytes;
    const int target_payload = wait->payload_bytes;
#if defined(__linux__)
    const int target_accounting = wait->accounting;
    (void) target_payload;
#elif defined(__APPLE__)
    if ( datagram_bytes == 0 )
        return;
#else
    fprintf( stderr, "harness error: %s has no queue completion observation on this platform\n", phase );
    rocketnet_check( 0 );
    return;
#endif
    for ( ;; )
    {
        const int queued = rocketnet_test_queue_accounting( socket );
        if ( queued < 0 )
        {
            fprintf( stderr, "harness error: %s could not read the queue accounting (errno=%d)\n", phase, errno );
            rocketnet_check( 0 );
            return;
        }
#if defined(__linux__)
        if ( queued > target_accounting )
        {
            wait->accounting = queued;
            return;
        }
#else
        if ( queued >= target_payload )
        {
            wait->accounting = queued;
            return;
        }
#endif
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( socket->handle, &readable );
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        const int ready = select( socket->handle + 1, &readable, NULL, NULL, &tv );
        if ( ready < 0 && errno == EINTR )
            continue;
        if ( ready < 0 )
        {
            fprintf( stderr, "harness error: %s select failed (errno=%d)\n", phase, errno );
            rocketnet_check( 0 );
            return;
        }
        if ( rocketnet_platform_time() >= wait->deadline )
        {
            fprintf( stderr, "harness timeout: %s did not complete within the 30 s watchdog; this is a hung harness, not a transport performance failure\n", phase );
            rocketnet_check( 0 );
            return;
        }
    }
}

static void rocketnet_test_pump_chaff_exchange( const char * requested_pump = NULL )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create_with_pump( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 , requested_pump );
    rocketnet_check( socket );

    const char * pump_at_create = rocketnet_platform_socket_pump( socket );

    int injector = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    rocketnet_check( injector >= 0 );

    struct sockaddr_in to;
    memset( &to, 0, sizeof(to) );
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    to.sin_port = htons( bind_address.port );

    // interleave: chaff leads, trails, and lands between every couple of
    // real packets, crossing msgx chunk boundaries (48 real > one chunk)

    const int num_real = 48;
    uint8_t real_packet[256];
    int sent_real = 0;
    rocketnet_test_queue_wait_t batch_wait;
    rocketnet_test_queue_wait_init( socket, &batch_wait, "batch chaff exchange" );
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    rocketnet_test_queue_wait_datagram( socket, &batch_wait, 0, "batch chaff exchange" );
    for ( int i = 0; i < num_real; i++ )
    {
        const int bytes = 24 + i;
        real_packet[0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            real_packet[j] = (uint8_t) ( 3 * i + j );
        rocketnet_check( sendto( injector, real_packet, (size_t) bytes, 0, (struct sockaddr*) &to, sizeof(to) ) == bytes );
        rocketnet_test_queue_wait_datagram( socket, &batch_wait, bytes, "batch chaff exchange" );
        sent_real++;
        if ( ( i % 2 ) == 1 )
        {
            rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
            rocketnet_test_queue_wait_datagram( socket, &batch_wait, 0, "batch chaff exchange" );
        }
    }
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    rocketnet_test_queue_wait_datagram( socket, &batch_wait, 0, "batch chaff exchange" );

    // drain through the batch pump: every real packet, in order, intact --
    // and in ONE receive_packets call, because a retry loop here greens
    // over the exact bug this test names (the review measured it: an
    // unfixed path consumes chaff between calls, so retries make
    // progress). loopback arrival is asynchronous (also measured: a
    // recvfrom immediately after sendto answers EAGAIN), so each send
    // above is waited on individually before the next: on Linux by
    // queue-accounting growth for that datagram, on macOS by cumulative
    // real payload (chaff carried by the injector's FIFO). No settle sleep.

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    const int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data,
                                                                    ROCKETNET_TEST_PUMP_STRIDE, packet_bytes,
                                                                    ROCKETNET_TEST_PUMP_PACKETS );
    rocketnet_check( received == sent_real );

    for ( int i = 0; i < received; i++ )
    {
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        rocketnet_check( packet[0] == (uint8_t) i );
        rocketnet_check( packet_bytes[i] == 24 + i );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( 3 * i + j ) );
    }

    // Drain only trailing zero-length chaff. The 48 real packets are already
    // validated above; this is not a retry of the batch oracle and does not
    // accumulate partial receives. It gives the per-packet phase a clean queue.

    uint8_t cleanup[ROCKETNET_TEST_PUMP_STRIDE];
    rocketnet_address_t cleanup_from;
    while ( rocketnet_platform_socket_receive_packet( socket, &cleanup_from, cleanup, sizeof(cleanup) ) > 0 )
    {
    }

    // and the per-packet primitive itself skips chaff: one zero-length
    // then one real packet, and the FIRST single-packet call must hand
    // back the real one -- a retry loop here would green over a
    // per-packet path that still reads chaff as drain-end

    rocketnet_test_queue_wait_t single_wait;
    rocketnet_test_queue_wait_init( socket, &single_wait, "single-packet chaff exchange" );
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    rocketnet_test_queue_wait_datagram( socket, &single_wait, 0, "single-packet chaff exchange" );
    uint8_t single[64];
    memset( single, 0x5a, sizeof(single) );
    rocketnet_check( sendto( injector, single, sizeof(single), 0, (struct sockaddr*) &to, sizeof(to) ) == (ssize_t) sizeof(single) );
    rocketnet_test_queue_wait_datagram( socket, &single_wait, (int) sizeof(single), "single-packet chaff exchange" );

    // completion-driven arrival for the per-packet call: each datagram
    // above was observed queued before the next was sent, so the chaff
    // is ahead of the real datagram and the FIRST call below must skip
    // it, not a retry.

    uint8_t single_received[ROCKETNET_TEST_PUMP_STRIDE];
    rocketnet_address_t single_from;
    int single_bytes = rocketnet_platform_socket_receive_packet( socket, &single_from, single_received, sizeof(single_received) );
    rocketnet_check( single_bytes == (int) sizeof(single) );
    for ( int j = 0; j < single_bytes; j++ )
        rocketnet_check( single_received[j] == 0x5a );

    // chaff exercised the pump it claims to have exercised: no silent
    // mid-run degradation

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_at_create ) == 0 );

    close( injector );
    rocketnet_platform_socket_destroy( socket );
}

// a refused destination mid-batch (#100's review, the blocker): xnu can
// deliver a prefix of a sendmsg_x batch and still return -1 with no
// count, so the send path's law is one bounded per-packet finish of the
// chunk. this pins the survivable half of that law on the real kernel: a
// batch with a refused destination in the middle must deliver every other
// packet AT LEAST once (duplicates are legal, priced, and eaten by replay
// protection above; spurious loss is not legal).

void rocketnet_test_platform_batch_pump_refused_destination()
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_t refused_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );
    rocketnet_address_parse( &refused_address, "0.0.0.0" );        // port 0: the kernel refuses it at send
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    local_address.port = bind_address.port;

    const int num_packets = 8;
    const int refused_index = 5;

    static uint8_t send_data[8][64];
    struct rocketnet_platform_send_t sends[8];
    for ( int i = 0; i < num_packets; i++ )
    {
        memset( send_data[i], i, sizeof(send_data[i]) );
        sends[i].to = i == refused_index ? &refused_address : &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = (int) sizeof(send_data[i]);
    }
    rocketnet_platform_socket_send_packets( socket, sends, num_packets );

    bool seen[8];
    memset( seen, 0, sizeof(seen) );
    int distinct = 0;

    double start_time = rocketnet_platform_time();
    while ( distinct < num_packets - 1 && rocketnet_platform_time() - start_time < 5.0 )
    {
        rocketnet_address_t from[8];
        int packet_bytes[8];
        uint8_t receive_data[8*ROCKETNET_TEST_PUMP_STRIDE];
        int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, 8 );
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
            const int index = packet[0];
            rocketnet_check( index < num_packets );
            rocketnet_check( index != refused_index );
            rocketnet_check( packet_bytes[i] == 64 );
            for ( int j = 0; j < 64; j++ )
                rocketnet_check( packet[j] == (uint8_t) index );
            if ( !seen[index] )
            {
                seen[index] = true;
                distinct++;
            }
        }
    }

    rocketnet_check( distinct == num_packets - 1 );

    // the refused destination is weather, never a pause: the pump must
    // still be the platform's expected one after the error path ran

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), ROCKETNET_TEST_EXPECTED_PUMP ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_pump_zero_length_chaff()
{
    // both pumps: whatever the platform runs by default, and the portable
    // pump forced -- identical semantics is the seam law under review

    rocketnet_test_pump_chaff_exchange();

    rocketnet_test_pump_chaff_exchange( "portable" );
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

void rocketnet_test_transport_acks_and_client_stats()
{
    // The transports end to end over UDP loopback: eight clients fill every
    // slot of an eight-slot server, payloads flow on the highest slot, and
    // the delivery notifications surface the packet ids from start_packet on
    // both ends -- the join key, through real sockets. The per-client stats
    // for the HIGH slot must move after traffic: the modulo recompute
    // covers all slots with at most 20 visits per update, which is the test the
    // reference's literal i == update_count % 6 predicate fails --
    // that form never updates any slot past five.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 8;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_t * clients[8];

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_functional_write_token( server_address, UINT64_C( 0x77000 ) + (uint64_t) i, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        clients[i] = rocketnet_client_transport_create( NULL, token, rocketnet_functional_client_config(), rocketnet_platform_time() );
        rocketnet_check( clients[i] );
    }

    double start = rocketnet_platform_time();
    int all_connected = 0;
    while ( rocketnet_platform_time() - start < 10.0 * ROCKETNET_TEST_CASE_TIME_SCALE && !all_connected )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        all_connected = 1;
        for ( int i = 0; i < 8; i++ )
            if ( rocketnet_client_transport_state( clients[i] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                all_connected = 0;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( all_connected );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 8 );

    // the client holding the highest slot -- past every index the broken
    // reference amortization would ever touch

    int high_slot = -1;
    struct rocketnet_client_transport_t * high_client = NULL;
    for ( int i = 0; i < 8; i++ )
    {
        int index = rocketnet_client_transport_client_index( clients[i] );
        if ( index > high_slot )
        {
            high_slot = index;
            high_client = clients[i];
        }
    }
    rocketnet_check( high_slot >= 6 );

    // one payload each way through acquire/commit, keeping the packet ids

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( high_client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &up_context );
    rocketnet_check( frame );
    memcpy( frame + up_context.payload_index, "ping", 4 );
    rocketnet_check( rocketnet_client_transport_finish_packet( high_client, &up_context, 4 ) > 0 );
    rocketnet_client_transport_send_packets( high_client );

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, high_slot, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &down_context );
    rocketnet_check( frame );
    memcpy( frame + down_context.payload_index, "pong", 4 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4 ) > 0 );

    // pump until both notifications surface: the server sees its id come
    // back from the high slot, the client sees its own -- each exactly the
    // id start_packet handed out

    int server_saw_ack = 0;
    int client_saw_ack = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 * ROCKETNET_TEST_CASE_TIME_SCALE && !( server_saw_ack && client_saw_ack ) )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == high_slot );
                server_saw_ack = 1;
            }
        }

        const uint64_t * client_acked_ids = NULL;
        int client_num_acked = rocketnet_client_transport_acked_packets( high_client, &client_acked_ids );
        for ( int i = 0; i < client_num_acked; i++ )
        {
            if ( client_acked_ids[i] == up_context.packet_id )
                client_saw_ack = 1;
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_saw_ack );
    rocketnet_check( client_saw_ack );

    // keep traffic flowing long enough for every round-robin phase to pass
    // several times, then the high slot's published stats have moved

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_server_transport_client_stats_t stats;
    rocketnet_server_transport_client_stats( server, high_slot, &stats );
    rocketnet_check( stats.bandwidth_received_kbps > 0.0f );
    rocketnet_check( stats.rtt >= 0.0f && stats.rtt < 1000.0f );
    rocketnet_check( stats.packet_loss >= 0.0f && stats.packet_loss <= 100.0f );

    // an out-of-range slot answers zeros, never a crash
    rocketnet_server_transport_client_stats( server, 12345, &stats );
    rocketnet_check( stats.rtt == 0.0f && stats.bandwidth_received_kbps == 0.0f );

    // and the client's rtt is live from the acks: nonzero and sane on a
    // loopback wire
    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( high_client, &client_stats );
    rocketnet_check( client_stats.rtt > 0.0f && client_stats.rtt < 1000.0f );

    for ( int i = 0; i < 8; i++ )
    {
        rocketnet_client_transport_disconnect( clients[i] );
        ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( clients[i] );
    }
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_client_stats_of_empty_slot_read_zero()
{
    // A game reading stats for a slot whose client has left must see zeros,
    // exactly as an index out of range reads: a departed client's numbers
    // must not outlive its slot. A client connects and moves its slot's
    // published stats, the server-side disconnect releases the slot, and
    // every field of that now-empty slot then reads zero.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 8;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, 0x99100ULL, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int slot = rocketnet_client_transport_client_index( client );
    rocketnet_check( slot >= 0 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, slot ) );

    // one payload each update until the slot's published stats move, running
    // long enough for every round-robin stats phase to pass several times

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        if ( frame )
        {
            memcpy( frame + context.payload_index, "ping", 4 );
            if ( rocketnet_client_transport_finish_packet( client, &context, 4 ) > 0 )
                rocketnet_client_transport_send_packets( client );
        }
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_server_transport_client_stats_t stats;
    rocketnet_server_transport_client_stats( server, slot, &stats );
    rocketnet_check( stats.bandwidth_received_kbps > 0.0f );

    // the server disconnects the client, releasing the slot immediately

    rocketnet_server_transport_disconnect_client( server, slot );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_send_packets( server );
    rocketnet_check( !rocketnet_server_transport_is_client_connected( server, slot ) );

    // an empty slot reads as zeros, as an index out of range does

    rocketnet_server_transport_client_stats( server, slot, &stats );
    rocketnet_check( stats.rtt == 0.0f );
    rocketnet_check( stats.jitter == 0.0f );
    rocketnet_check( stats.packet_loss == 0.0f );
    rocketnet_check( stats.bandwidth_sent_kbps == 0.0f );
    rocketnet_check( stats.bandwidth_received_kbps == 0.0f );
    rocketnet_check( stats.bandwidth_acked_kbps == 0.0f );

    rocketnet_client_transport_disconnect( client );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( client, ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING );
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER_EXCEPT( server, ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING );
}

void rocketnet_test_transport_fragmentation()
{
    // the declared-capacity surface end to end over UDP loopback: a
    // request above the wire's capacity routes to the arena, splits into
    // fragments at finish, crosses real sockets, and reassembles
    // byte-exact into the ordinary payload batch on the far side -- both
    // directions -- with the payload's one packet id surfacing exactly
    // once when every fragment's wire packet is acknowledged

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, 0xF6A9ULL, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    static uint8_t pattern[ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES];
    for ( int i = 0; i < (int) sizeof(pattern); i++ )
        pattern[i] = (uint8_t) ( 0xC3 + i );

    // client to server: a 5000-byte payload written into the arena frame

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    rocketnet_check( up_context.payload_index == 0 );
    rocketnet_check( up_context.payload_capacity == ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
    memcpy( frame, pattern, 5000 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 5000 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    int server_received = 0;
    int up_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( server_received && up_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5000 );
            rocketnet_check( memcmp( payload, pattern, 5000 ) == 0 );
            server_received++;
        }

        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_client_transport_acked_packets( client, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
            if ( acked_ids[i] == up_context.packet_id )
                up_acks_seen++;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( up_acks_seen == 1 );                   // the payload's one id, exactly once

    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == (uint64_t) ( ( 5000 + ROCKETNET_MAX_FRAGMENT_BYTES - 1 ) / ROCKETNET_MAX_FRAGMENT_BYTES ) );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED] == (uint64_t) ( ( 5000 + ROCKETNET_MAX_FRAGMENT_BYTES - 1 ) / ROCKETNET_MAX_FRAGMENT_BYTES ) );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // server to client: a 9000-byte payload the other way

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, 0, 9000, &down_context );
    rocketnet_check( frame );
    rocketnet_check( down_context.payload_index == 0 );
    memcpy( frame, pattern, 9000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 9000 ) > 0 );

    int client_received = 0;
    int down_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( client_received && down_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_client_transport_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_bytes;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            rocketnet_check( payload_bytes == 9000 );
            rocketnet_check( memcmp( payload, pattern, 9000 ) == 0 );
            client_received++;
        }

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == 0 );
                down_acks_seen++;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_received == 1 );
    rocketnet_check( down_acks_seen == 1 );

    const struct rocketnet_counters_t * client_counters = rocketnet_client_transport_counters( client );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] == (uint64_t) ( ( 9000 + ROCKETNET_MAX_FRAGMENT_BYTES - 1 ) / ROCKETNET_MAX_FRAGMENT_BYTES ) );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // an over-capacity request whose write turns out small collapses to
    // one ordinary payload packet -- the fragment counters do not move

    frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 64 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 64 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    server_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !server_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_bytes == 64 );
            rocketnet_check( memcmp( payload, pattern, 64 ) == 0 );
            server_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == (uint64_t) ( ( 5000 + ROCKETNET_MAX_FRAGMENT_BYTES - 1 ) / ROCKETNET_MAX_FRAGMENT_BYTES ) );        // unchanged

    // the configured maximum is readable, so callers can size buffers
    rocketnet_check( rocketnet_client_transport_max_payload_bytes( client ) == ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );

    // the copying cursor carries reassembled payloads too, sized by the
    // configured maximum
    frame = rocketnet_server_transport_start_packet( server, 0, 4000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 4000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4000 ) > 0 );

    static uint8_t cursor_buffer[ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES];
    int cursor_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !cursor_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        int cursor_bytes;
        while ( ( cursor_bytes = rocketnet_client_transport_receive_packet( client, cursor_buffer, (int) sizeof(cursor_buffer) ) ) > 0 )
        {
            rocketnet_check( cursor_bytes == 4000 );
            rocketnet_check( memcmp( cursor_buffer, pattern, 4000 ) == 0 );
            cursor_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( cursor_received == 1 );

    // NEGATIVE: a cursor buffer smaller than a reassembled payload refuses
    // it whole -- counted, never truncated, never delivered
    frame = rocketnet_server_transport_start_packet( server, 0, 3000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 3000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 3000 ) > 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 &&
            client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        uint8_t small_buffer[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        rocketnet_check( rocketnet_client_transport_receive_packet( client, small_buffer, (int) sizeof(small_buffer) ) == 0 );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 1 );

    // the reviewer's burst: twelve fragmented payloads in one fill phase --
    // 36 wire packets, ordinary load -- can all complete in ONE client
    // update, which is more completions than the client has reassembly
    // slots. the delivered mirror sizes to the drain bound, so every one
    // is delivered and BATCH_FULL stays exactly zero.
    {
        struct rocketnet_packet_context_t burst_context;
        for ( int i = 0; i < 12; i++ )
        {
            frame = rocketnet_server_transport_start_packet( server, 0, 2500, &burst_context );
            rocketnet_check( frame );
            memcpy( frame, pattern, 2500 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &burst_context, 2500 ) > 0 );
        }
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        rocketnet_platform_sleep( 0.1 );                    // let the whole burst queue on the client's receive thread

        int burst_received = 0;
        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 10.0 && burst_received < 12 )
        {
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );

            const int num_payloads = rocketnet_client_transport_num_payloads( client );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_bytes;
                uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
                rocketnet_check( payload_bytes == 2500 );
                rocketnet_check( memcmp( payload, pattern, 2500 ) == 0 );
                burst_received++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( burst_received == 12 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 15 );    // 9000 + 4000 + 3000 + the twelve
    }

    // NEGATIVE: the arena is a fixed fill-phase resource -- exhaustion
    // drops at start_packet on its own counter, and the flush reclaims it
    {
        struct rocketnet_packet_context_t contexts[ROCKETNET_SEND_ARENA_SLOTS];
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &contexts[i] ) );
        struct rocketnet_packet_context_t overflow_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) == NULL );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_SEND_ARENA_FULL] == 1 );
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_client_transport_abort_packet( client, &contexts[i] );
        rocketnet_client_transport_send_packets( client );
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) != NULL );
        rocketnet_client_transport_abort_packet( client, &overflow_context );
    }

    // NEGATIVE: a request past the configured maximum refuses outright
    {
        struct rocketnet_packet_context_t oversize_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
    }

    // nothing died on the clean wire
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );

    rocketnet_client_transport_disconnect( client );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( client, ROCKETNET_COUNTER_SEND_ARENA_FULL, ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER_EXCEPT( server, ROCKETNET_COUNTER_SEND_ARENA_FULL, ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL );
}

// the transport-level ipv6 loopback: the same out-of-the-box connect and
// payload exchange as the udp loopback test, on [::1] -- ipv6 listen is
// REQUIRED of every server transport (#97: the gamelift gateway speaks ipv6 to
// servers), so this runs on every platform and every transport the suite
// builds against, with zero transport ifdefs.

void rocketnet_test_transport_ipv6_loopback()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "[::1]", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "[::1]:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello6", 6 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world6", 6 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 6 );
            rocketnet_check( memcmp( payload, "hello6", 6 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world6", 6 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

// the transport-level dual-stack case: the server listens on the ipv6
// wildcard [::] and an ipv4 client connects to 127.0.0.1 on the granted
// port, with payloads both ways. like the ipv6 loopback above it has no
// transport ifdefs: it runs on whichever transport the suite or the env pin
// selects, so the CI transport matrix runs it per transport.

void rocketnet_test_transport_dual_stack_ipv4_client()
{
    // a server bound to the IPv6 wildcard is dual-stack: it serves IPv4 as
    // well, so a connect token naming its IPv4 loopback is accepted and the
    // handshake completes over the v4-mapped path, on every transport

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;

    // wildcard bind kept: the case is the ipv6 wildcard listen serving an ipv4 client
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "[::]", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello4", 6 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world4", 6 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 6 );
            rocketnet_check( memcmp( payload, "hello4", 6 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world4", 6 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}


void rocketnet_test_server_serves_both_families()
{
    // One server at a named IPv4 and a named IPv6 loopback on one port.
    // An IPv4 client and an IPv6 client connect in this run, payloads both ways.
    // The runner pins the server transport. This case does not.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 16;
    snprintf( server_config.public_address, sizeof( server_config.public_address ), "127.0.0.1:0" );
    snprintf( server_config.second_public_address, sizeof( server_config.second_public_address ), "[::1]:0" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
    rocketnet_check( server );

    const int port = rocketnet_server_transport_port( server );
    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%d", port );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%d", port );

    uint8_t token4[ROCKETNET_CONNECT_TOKEN_BYTES];
    uint8_t token6[ROCKETNET_CONNECT_TOKEN_BYTES];
    const double now = rocketnet_platform_time();
    rocketnet_check( rocketnet_functional_write_token( ipv4_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, now ), token4, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_functional_write_token( ipv6_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, now ), token6, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * ipv4 = rocketnet_client_transport_create( NULL, token4, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( ipv4 );
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( ipv4, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( ipv4 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( ipv4 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    struct rocketnet_client_transport_t * ipv6 = rocketnet_client_transport_create( NULL, token6, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( ipv6 );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( ipv4, rocketnet_platform_time() );
        rocketnet_client_transport_update( ipv6, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( ipv6 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 2 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( ipv6 ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 2 );

    int got_from_4 = 0, got_from_6 = 0, got_to_4 = 0, got_to_6 = 0;
    int sent_4 = 0, sent_6 = 0, sent_to_4 = 0, sent_to_6 = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_4 < 10 )
        {
            rocketnet_client_transport_send_packet( ipv4, (const uint8_t*) "hello4", 6 );
            sent_4++;
        }
        if ( sent_6 < 10 )
        {
            rocketnet_client_transport_send_packet( ipv6, (const uint8_t*) "hello6", 6 );
            sent_6++;
        }
        if ( sent_to_4 < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world4", 6 );
            sent_to_4++;
        }
        if ( sent_to_6 < 10 )
        {
            rocketnet_server_transport_send_packet( server, 1, (const uint8_t*) "world6", 6 );
            sent_to_6++;
        }

        rocketnet_server_transport_send_packets( server );
        rocketnet_client_transport_update( ipv4, rocketnet_platform_time() );
        rocketnet_client_transport_update( ipv6, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_bytes == 6 );
            if ( payload_client_index == 0 )
            {
                rocketnet_check( memcmp( payload, "hello4", 6 ) == 0 );
                got_from_4++;
            }
            else
            {
                rocketnet_check( payload_client_index == 1 );
                rocketnet_check( memcmp( payload, "hello6", 6 ) == 0 );
                got_from_6++;
            }
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( ipv4, packet, (int) sizeof( packet ) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world4", 6 ) == 0 );
            got_to_4++;
        }
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( ipv6, packet, (int) sizeof( packet ) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world6", 6 ) == 0 );
            got_to_6++;
        }

        if ( got_from_4 >= 1 && got_from_6 >= 1 && got_to_4 >= 1 && got_to_6 >= 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( got_from_4 >= 1 && got_from_6 >= 1 && got_to_4 >= 1 && got_to_6 >= 1 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( ipv6 );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( ipv4 );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_server_handshake_deadline_from_config()
{
    struct rocketnet_test_env_guard_t deadline_guard, transport_guard;
    rocketnet_test_env_clear( &deadline_guard, "ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS" );
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );
    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    config.max_clients = 1; // This case checks one handshake deadline, not server capacity.
    config.handshake_deadline_seconds = 2;
    snprintf( config.transport, sizeof(config.transport), "portable" );
    const double start_time = 100.0;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config );
    rocketnet_check( server );
    char address_string[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( address_string, sizeof(address_string), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const uint64_t timestamp = rocketnet_functional_token_timestamp( config.game_clock_epoch, start_time );
    rocketnet_check( rocketnet_functional_write_token( address_string, rocketnet_random_uint64(), timestamp, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    rocketnet_client_protocol_init( &client, &client_counters );
    rocketnet_check( rocketnet_client_protocol_connect( &client, token, start_time - 0.5, timestamp ) );
    struct rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
        ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    uint8_t packet[ROCKETNET_RECEIVE_PACKET_BYTES], payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int packet_bytes = 0;
    rocketnet_client_protocol_update( &client, start_time - 0.5, packet, &packet_bytes );
    rocketnet_check( packet_bytes > 0 );
    rocketnet_platform_socket_send_packet( client_socket, &client.server_address, packet, packet_bytes );
    const double delivery_start = rocketnet_platform_time();
    while ( !client.has_cookie && rocketnet_platform_time() - delivery_start < 5.0 )
    {
        rocketnet_server_transport_update( server, start_time );
        struct rocketnet_address_t from;
        const int received = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, sizeof(packet) );
        if ( received > 0 )
        {
            int payload_bytes = 0;
            rocketnet_client_protocol_process_packet( &client, packet, received, start_time, payload, sizeof(payload), &payload_bytes );
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client.has_cookie );
    // Keep the client waiting for its challenge: resend requests but never
    // process the challenge or send a connection response.
    for ( int retry = 0; retry < 4; ++retry )
    {
        const double now = start_time + retry * 0.5;
        rocketnet_client_protocol_update( &client, now, packet, &packet_bytes );
        rocketnet_check( packet_bytes > 0 );
        rocketnet_platform_socket_send_packet( client_socket, &client.server_address, packet, packet_bytes );
        const uint64_t expected = retry == 0 ? 1 : (uint64_t) retry;
        const int counter = retry == 0 ? ROCKETNET_COUNTER_MAPPING_CREATED : ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT;
        const double wait_start = rocketnet_platform_time();
        while ( rocketnet_server_transport_counters( server )->values[counter] < expected && rocketnet_platform_time() - wait_start < 5.0 )
        {
            rocketnet_server_transport_update( server, now );
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( rocketnet_server_transport_counters( server )->values[counter] == expected );
        rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 0 );
    }
    rocketnet_server_transport_update( server, start_time + 1.999 );
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 0 );
    rocketnet_server_transport_update( server, start_time + 2.0 );
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 1 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_server_transport_update( server, start_time + 3.0 );
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 1 );
    rocketnet_platform_socket_destroy( client_socket );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
    rocketnet_test_env_restore( &transport_guard );
    rocketnet_test_env_restore( &deadline_guard );
}


// The public client and server over the portable transport on 127.0.0.1:
// the server's max_clients override sizes the message server and the
// transport alike, and every stats struct field reads the internal value it
// names -- each counter and stat compared with the transport stats field or
// the counter-block slot it maps to, after real traffic both ways.

#if ROCKETNET_DEV
void rocketnet_test_public_fragment_delivery()
{
    constexpr int payload_bytes = 3000;
    uint8_t client_payload[payload_bytes];
    uint8_t server_payload[payload_bytes];
    for ( int i = 0; i < payload_bytes; i++ )
    {
        client_payload[i] = (uint8_t) ( ( i * 37 + i / 7 + 0xA5 ) & 0xFF );
        server_payload[i] = (uint8_t) ( ( ( i * 19 ) ^ ( i / 11 ) ^ 0x6D ) & 0xFF );
    }

    uint32_t client_checksum = 2166136261u;
    uint32_t server_checksum = 2166136261u;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        client_checksum = ( client_checksum ^ client_payload[i] ) * 16777619u;
        server_checksum = ( server_checksum ^ server_payload[i] ) * 16777619u;
    }
    rocketnet_check( client_checksum != server_checksum );

    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, sizeof(server_config.backend_public_key) );
    server_config.transport = "portable";
    server_config.max_clients = 1;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    rocketnet_check( rocketnet_server_ipv4_address( server, server_address, sizeof(server_address) ) );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( server_address, NULL, server_key,
                                                         ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                                                         ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, 78,
                                                         ROCKETNET_DEV_PROTOCOL_ID,
                                                         rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ),
                                                         NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "portable";
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    rocketnet_check( client );
    rocketnet_check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    const double connect_start = rocketnet_time();
    while ( rocketnet_time() - connect_start < 10.0 &&
            rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        const double now = 1000.0 + ( rocketnet_time() - connect_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        while ( rocketnet_server_receive_messages( server ) != NULL ) {}
        while ( rocketnet_client_receive_messages( client ) != NULL ) {}
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int client_index = rocketnet_client_index( client );
    rocketnet_check( client_index == 0 );

    int client_to_server_received = 0;
    uint32_t client_to_server_checksum = 0;
    uint8_t * outgoing = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload_bytes );
    rocketnet_check( outgoing );
    memcpy( outgoing, client_payload, sizeof(client_payload) );
    rocketnet_check( rocketnet_client_finish_message( client, payload_bytes ) != 0 );

    const double up_start = rocketnet_time();
    while ( rocketnet_time() - up_start < 10.0 && client_to_server_received == 0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - connect_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                rocketnet_check( batch->client_index[i] == client_index );
                rocketnet_check( batch->channel[i] == 0 );
                rocketnet_check( batch->reliability[i] == ROCKETNET_RELIABILITY_RELIABLE_ORDERED );
                rocketnet_check( batch->bytes[i] == payload_bytes );
                rocketnet_check( memcmp( batch->data[i], client_payload, payload_bytes ) == 0 );
                client_to_server_received++;
                rocketnet_check( client_to_server_received == 1 );
                uint32_t checksum = 2166136261u;
                for ( int j = 0; j < batch->bytes[i]; j++ )
                    checksum = ( checksum ^ batch->data[i][j] ) * 16777619u;
                client_to_server_checksum = checksum;
            }
        }
        while ( rocketnet_client_receive_messages( client ) != NULL ) {}
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    rocketnet_check( client_to_server_received == 1 );
    rocketnet_check( client_to_server_checksum == client_checksum );

    int server_to_client_received = 0;
    uint32_t server_to_client_checksum = 0;
    outgoing = rocketnet_server_start_message( server, (uint64_t) client_index, 0,
                                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload_bytes );
    rocketnet_check( outgoing );
    memcpy( outgoing, server_payload, sizeof(server_payload) );
    rocketnet_check( rocketnet_server_finish_message( server, payload_bytes ) != 0 );

    const double down_start = rocketnet_time();
    while ( rocketnet_time() - down_start < 10.0 && server_to_client_received == 0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - connect_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        while ( rocketnet_server_receive_messages( server ) != NULL ) {}
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                rocketnet_check( batch->client_index[i] == 0 );
                rocketnet_check( batch->channel[i] == 0 );
                rocketnet_check( batch->reliability[i] == ROCKETNET_RELIABILITY_RELIABLE_ORDERED );
                rocketnet_check( batch->bytes[i] == payload_bytes );
                rocketnet_check( memcmp( batch->data[i], server_payload, payload_bytes ) == 0 );
                server_to_client_received++;
                rocketnet_check( server_to_client_received == 1 );
                uint32_t checksum = 2166136261u;
                for ( int j = 0; j < batch->bytes[i]; j++ )
                    checksum = ( checksum ^ batch->data[i][j] ) * 16777619u;
                server_to_client_checksum = checksum;
            }
        }
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    rocketnet_check( server_to_client_received == 1 );
    rocketnet_check( server_to_client_checksum == server_checksum );

    // Keep draining both public queues for a short confirmation window.
    // Completeness above is primary; this catches delayed duplicate delivery.
    for ( int i = 0; i < 20; i++ )
    {
        const double now = 1000.0 + ( rocketnet_time() - connect_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        while ( rocketnet_server_receive_messages( server ) != NULL )
            rocketnet_check( false );
        while ( rocketnet_client_receive_messages( client ) != NULL )
            rocketnet_check( false );
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    rocketnet_client_disconnect( client );
    for ( int i = 0; i < 100 && !rocketnet_client_disconnected( client ); i++ )
    {
        rocketnet_client_update( client, 1000.0 + ( rocketnet_time() - connect_start ) );
        rocketnet_client_send_packets( client );
        rocketnet_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_disconnected( client ) );
    rocketnet_client_destroy( client );
    rocketnet_server_stop( server );
    for ( int i = 0; i < 100 && !rocketnet_server_stopped( server ); i++ )
    {
        rocketnet_server_update( server, 1000.0 + ( rocketnet_time() - connect_start ) );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_stopped( server ) );
    rocketnet_server_destroy( server );
    printf( "test_control_fragment_packets_each_end: public delivery bytes=3000/3000 checksums=%08x/%08x\n",
            client_to_server_checksum, server_to_client_checksum );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static uint64_t rocketnet_test_block_sum( const struct rocketnet_counters_t * a, const struct rocketnet_counters_t * b, int counter )
{
    return rocketnet_counter_value( a, counter ) + rocketnet_counter_value( b, counter );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static void rocketnet_test_fill_queries_payload( uint8_t payload[16], uint8_t direction_tag, int message_index )
{
    payload[0] = 1;
    payload[1] = 2;
    payload[2] = 3;
    payload[3] = direction_tag;
    payload[4] = (uint8_t) message_index;
    for ( int i = 5; i < 16; i++ )
        payload[i] = (uint8_t) ( ( ( message_index + 1 ) * ( i + 3 ) ) ^ direction_tag );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
static void rocketnet_test_validate_queries_batch( const struct rocketnet_message_batch_t * batch,
                                                   int expected_client_index,
                                                   uint8_t direction_tag,
                                                   int expected_message_count,
                                                   bool * seen_messages,
                                                   uint64_t * messages_received,
                                                   uint64_t * message_bytes_received )
{
    rocketnet_check( batch != NULL );
    rocketnet_check( batch->num_messages > 0 && batch->num_messages <= ROCKETNET_RECEIVE_BATCH_MESSAGES );
    for ( int i = 0; i < batch->num_messages; i++ )
    {
        rocketnet_check( batch->client_index[i] == expected_client_index );
        rocketnet_check( batch->channel[i] == 0 );
        // Receipt modes arrive at the receiver without the receipt flag (sdk/rocketnet.h).
        rocketnet_check( batch->reliability[i] == ROCKETNET_RELIABILITY_RELIABLE );
        rocketnet_check( batch->bytes[i] == 16 );
        rocketnet_check( batch->data[i] != NULL );
        const int message_index = (int) batch->data[i][4];
        rocketnet_check( message_index >= 0 && message_index < expected_message_count );
        rocketnet_check( !seen_messages[message_index] );
        uint8_t expected_payload[16];
        rocketnet_test_fill_queries_payload( expected_payload, direction_tag, message_index );
        rocketnet_check( memcmp( batch->data[i], expected_payload, sizeof( expected_payload ) ) == 0 );
        seen_messages[message_index] = true;
        ( *messages_received )++;
        ( *message_bytes_received ) += (uint64_t) batch->bytes[i];
    }
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_public_client_server_queries()
{
    struct rocketnet_test_env_guard_t max_clients_guard;
    rocketnet_test_env_save( &max_clients_guard, "ROCKETNET_SERVER_MAX_CLIENTS" );
    setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "3", 1 );

    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    server_config.max_clients = 5;
    server_config.transport = "portable";
    const uint64_t memory_bytes = rocketnet_server_required_memory( &server_config );
    rocketnet_check( memory_bytes > 0 );
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_max_clients( server ) == 3 );
    rocketnet_test_env_restore( &max_clients_guard );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    rocketnet_check( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( server_address, NULL, server_key, ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS, ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, 77, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ), NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "portable";
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    if ( client && rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
        client = NULL;
    }
    rocketnet_check( client );

    const int expected_message_count = 20;
    const uint8_t client_to_server_tag = 0xC2;
    const uint8_t server_to_client_tag = 0x52;
    bool server_seen[20] = {};
    bool client_seen[20] = {};
    uint64_t client_messages_sent = 0;
    uint64_t client_message_bytes_sent = 0;
    uint64_t server_messages_sent = 0;
    uint64_t server_message_bytes_sent = 0;
    uint64_t server_messages_received = 0;
    uint64_t server_message_bytes_received = 0;
    uint64_t client_messages_received = 0;
    uint64_t client_message_bytes_received = 0;

    const double wall_start = rocketnet_time();
    int exchanged = 0;
    while ( exchanged < expected_message_count && rocketnet_time() - wall_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            uint8_t client_payload[16];
            uint8_t server_payload[16];
            rocketnet_test_fill_queries_payload( client_payload, client_to_server_tag, exchanged );
            rocketnet_test_fill_queries_payload( server_payload, server_to_client_tag, exchanged );
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, (int) sizeof( client_payload ) );
            rocketnet_check( out != NULL );
            memcpy( out, client_payload, sizeof( client_payload ) );
            rocketnet_check( rocketnet_client_finish_message( client, (int) sizeof( client_payload ) ) != 0 );
            client_messages_sent++;
            client_message_bytes_sent += sizeof( client_payload );
            out = rocketnet_server_start_message( server, (uint64_t) rocketnet_client_index( client ), 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, (int) sizeof( server_payload ) );
            rocketnet_check( out != NULL );
            memcpy( out, server_payload, sizeof( server_payload ) );
            rocketnet_check( rocketnet_server_finish_message( server, (int) sizeof( server_payload ) ) != 0 );
            server_messages_sent++;
            server_message_bytes_sent += sizeof( server_payload );
            exchanged++;
        }
        // Validate every delivered batch entry before the next update (standard/MESSAGES.md m1.6).
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            rocketnet_test_validate_queries_batch( batch, rocketnet_client_index( client ), client_to_server_tag,
                                                   expected_message_count, server_seen,
                                                   &server_messages_received, &server_message_bytes_received );
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            rocketnet_test_validate_queries_batch( incoming, 0, server_to_client_tag,
                                                   expected_message_count, client_seen,
                                                   &client_messages_received, &client_message_bytes_received );
        }
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }
    rocketnet_check( exchanged == expected_message_count );
    int post_delivery_ticks = 0;
    while ( post_delivery_ticks < 20 && rocketnet_time() - wall_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        // Every delivery is received and validated before the next update (standard/MESSAGES.md m1.6).
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            rocketnet_test_validate_queries_batch( batch, rocketnet_client_index( client ), client_to_server_tag,
                                                   expected_message_count, server_seen,
                                                   &server_messages_received, &server_message_bytes_received );
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            rocketnet_test_validate_queries_batch( incoming, 0, server_to_client_tag,
                                                   expected_message_count, client_seen,
                                                   &client_messages_received, &client_message_bytes_received );
        }
        if ( server_messages_received >= (uint64_t) expected_message_count &&
             client_messages_received >= (uint64_t) expected_message_count &&
             rn_client_stats_of( client ).receipts_acked >= client_messages_sent &&
             rn_server_stats_of( server ).receipts_acked >= server_messages_sent )
        {
            post_delivery_ticks++;
        }
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }

    // Independent application-message byte and count oracle in each direction.
    rocketnet_check( client_messages_sent == (uint64_t) expected_message_count );
    rocketnet_check( server_messages_sent == (uint64_t) expected_message_count );
    rocketnet_check( client_message_bytes_sent == (uint64_t) ( expected_message_count * 16 ) );
    rocketnet_check( server_message_bytes_sent == (uint64_t) ( expected_message_count * 16 ) );
    rocketnet_check( server_messages_received == client_messages_sent );
    rocketnet_check( server_message_bytes_received == client_message_bytes_sent );
    rocketnet_check( client_messages_received == server_messages_sent );
    rocketnet_check( client_message_bytes_received == server_message_bytes_sent );
    for ( int i = 0; i < expected_message_count; i++ )
    {
        rocketnet_check( server_seen[i] );
        rocketnet_check( client_seen[i] );
    }

    // read every value through its stats call and through the layer it names,
    // with no update between, so both see the same numbers

    struct rocketnet_message_client_t * message_client = rocketnet_client_message_client( client );
    struct rocketnet_client_transport_t * client_transport = rocketnet_message_client_transport( message_client );
    const struct rocketnet_counters_t * ct = rocketnet_client_transport_counters( client_transport );
    const struct rocketnet_counters_t * cm = rocketnet_message_client_counters( message_client );
    struct rocketnet_client_transport_stats_t cs;
    rocketnet_client_transport_stats( client_transport, &cs );
    const struct rocketnet_client_stats_t client_public = rn_client_stats_of( client );
    const uint64_t ipv4_udp_header_bytes = (uint64_t) rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 );

    // packets_sent, packets_received, bytes_sent, bytes_received count UDP
    // datagrams and wire bytes (including IPv4+UDP headers and handshake/keepalive
    // traffic) across client lifetime, so exact datagram/wire totals match cs while
    // wire bytes strictly exceed delivered application message bytes plus IPv4+UDP headers.
    rocketnet_check( client_public.packets_sent == cs.packets_sent && cs.packets_sent > 0 );
    rocketnet_check( client_public.packets_received == cs.packets_received && cs.packets_received > 0 );
    rocketnet_check( client_public.bytes_sent == cs.bytes_sent && cs.bytes_sent > cs.packets_sent );
    rocketnet_check( client_public.bytes_received == cs.bytes_received && cs.bytes_received > cs.packets_received );
    rocketnet_check( client_public.bytes_sent > server_message_bytes_received + client_public.packets_sent * ipv4_udp_header_bytes );
    rocketnet_check( client_public.bytes_received > client_message_bytes_received + client_public.packets_received * ipv4_udp_header_bytes );
    // payloads_received counts opened transport payload packets delivered to the
    // message layer (carrying the connection's CONFIG control record as well as
    // packed or single application message records), so it is distinct from both
    // client_messages_received and total UDP datagrams in packets_received.
    rocketnet_check( client_public.payloads_received == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED ) );
    rocketnet_check( client_public.payloads_received > 0 );
    rocketnet_check( client_public.packets_received > client_public.payloads_received );
    // Resend and duplicate counts depend on runtime UDP scheduling and path loss.
    rocketnet_check( client_public.messages_resent == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_RESENT ) );
    rocketnet_check( client_public.messages_duplicated == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) );
    // Every client-to-server send used RELIABLE_WITH_ACK_RECEIPT while connected and was delivered.
    rocketnet_check( client_public.receipts_acked == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) );
    rocketnet_check( client_public.receipts_acked == client_messages_sent && client_public.receipts_acked == server_messages_received );
    rocketnet_check( client_public.receipts_lost == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) );
    rocketnet_check( client_public.receipts_lost == 0 );
    rocketnet_check( client_public.receipts_dropped == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL ) );
    rocketnet_check( client_public.receipts_dropped == 0 );
    rocketnet_check( client_public.messages_dropped_over_budget == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) );
    rocketnet_check( client_public.messages_dropped_over_budget == 0 );
    rocketnet_check( client_public.messages_dropped_not_connected == rocketnet_test_block_sum( ct, cm, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) );
    rocketnet_check( client_public.messages_dropped_not_connected == 0 );

    // RTT, jitter, loss estimate, and trailing-second rates depend on OS scheduling
    // and sliding-window frame timing, so they are compared to the transport stats layer.
    rocketnet_check( client_public.rtt_ms == (double) cs.rtt && client_public.rtt_ms >= 0.0 );
    rocketnet_check( client_public.jitter_ms == (double) cs.jitter && client_public.jitter_ms >= 0.0 );
    rocketnet_check( client_public.packet_loss_percent == (double) cs.packet_loss && client_public.packet_loss_percent >= 0.0 );
    rocketnet_check( client_public.packets_sent_per_second == (double) cs.packets_sent_per_second && client_public.packets_sent_per_second >= 0.0 );
    rocketnet_check( client_public.packets_received_per_second == (double) cs.packets_received_per_second && client_public.packets_received_per_second >= 0.0 );
    rocketnet_check( client_public.bandwidth_sent_kbps == (double) cs.bandwidth_sent_kbps && client_public.bandwidth_sent_kbps >= 0.0 );
    rocketnet_check( client_public.bandwidth_received_kbps == (double) cs.bandwidth_received_kbps && client_public.bandwidth_received_kbps >= 0.0 );

    struct rocketnet_message_server_t * message_server = rocketnet_server_message_server( server );
    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( message_server );
    const struct rocketnet_counters_t * st = rocketnet_server_transport_counters( server_transport );
    const struct rocketnet_counters_t * sm = rocketnet_message_server_counters( message_server );
    struct rocketnet_server_transport_stats_t ss;
    rocketnet_server_transport_stats( server_transport, &ss );
    const struct rocketnet_server_stats_t server_public = rn_server_stats_of( server );

    rocketnet_check( server_public.packets_sent == ss.packets_sent && ss.packets_sent > 0 );
    rocketnet_check( server_public.packets_received == ss.packets_received && ss.packets_received > 0 );
    rocketnet_check( server_public.bytes_sent == ss.bytes_sent && ss.bytes_sent > ss.packets_sent );
    rocketnet_check( server_public.bytes_received == ss.bytes_received && ss.bytes_received > ss.packets_received );
    rocketnet_check( server_public.bytes_sent > client_message_bytes_received + server_public.packets_sent * ipv4_udp_header_bytes );
    rocketnet_check( server_public.bytes_received > server_message_bytes_received + server_public.packets_received * ipv4_udp_header_bytes );
    rocketnet_check( server_public.connections_accepted == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_CLIENT_CONNECTED ) );
    rocketnet_check( server_public.connections_accepted == 1 );
    rocketnet_check( server_public.connections_denied_full == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS ) + rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS ) );
    rocketnet_check( server_public.connections_denied_full == 0 );
    rocketnet_check( server_public.handshakes_dropped_over_budget == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED ) );
    rocketnet_check( server_public.handshakes_dropped_over_budget == 0 );
    rocketnet_check( server_public.payloads_received == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_PAYLOAD_DELIVERED ) );
    rocketnet_check( server_public.payloads_received > 0 );
    rocketnet_check( server_public.packets_received > server_public.payloads_received );
    rocketnet_check( server_public.messages_resent == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_RESENT ) );
    rocketnet_check( server_public.messages_duplicated == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) );
    rocketnet_check( server_public.receipts_acked == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) );
    rocketnet_check( server_public.receipts_acked == server_messages_sent && server_public.receipts_acked == client_messages_received );
    rocketnet_check( server_public.receipts_lost == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) );
    rocketnet_check( server_public.receipts_lost == 0 );
    rocketnet_check( server_public.receipts_dropped == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL ) );
    rocketnet_check( server_public.receipts_dropped == 0 );
    rocketnet_check( server_public.messages_dropped_over_budget == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) );
    rocketnet_check( server_public.messages_dropped_over_budget == 0 );
    rocketnet_check( server_public.messages_dropped_not_connected == rocketnet_test_block_sum( st, sm, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) );
    rocketnet_check( server_public.messages_dropped_not_connected == 0 );

    rocketnet_check( server_public.packets_sent_per_second == (double) ss.packets_sent_per_second && server_public.packets_sent_per_second >= 0.0 );
    rocketnet_check( server_public.packets_received_per_second == (double) ss.packets_received_per_second && server_public.packets_received_per_second >= 0.0 );
    rocketnet_check( server_public.bandwidth_sent_kbps == (double) ss.bandwidth_sent_kbps && server_public.bandwidth_sent_kbps >= 0.0 );
    rocketnet_check( server_public.bandwidth_received_kbps == (double) ss.bandwidth_received_kbps && server_public.bandwidth_received_kbps >= 0.0 );

    const int slot = (int) rocketnet_client_index( client );
    rocketnet_check( rocketnet_server_client_connected( server, slot ) && rocketnet_server_client_id( server, slot ) == 77 );
    struct rocketnet_server_transport_client_stats_t cls;
    rocketnet_server_transport_client_stats( server_transport, slot, &cls );
    const struct rocketnet_server_client_stats_t slot_public = rn_server_client_stats_of( server, slot );
    // Per-slot wire totals reset when the slot is assigned on connection acceptance,
    // so they exclude pre-slot handshake datagrams counted in whole-server server_public
    // while still covering all connected session traffic and payload deliveries.
    rocketnet_check( slot_public.packets_sent == cls.packets_sent && cls.packets_sent > 0 );
    rocketnet_check( slot_public.packets_received == cls.packets_received && cls.packets_received > 0 );
    rocketnet_check( slot_public.bytes_sent == cls.bytes_sent && cls.bytes_sent > cls.packets_sent );
    rocketnet_check( slot_public.bytes_received == cls.bytes_received && cls.bytes_received > cls.packets_received );
    rocketnet_check( slot_public.packets_sent < server_public.packets_sent );
    rocketnet_check( slot_public.bytes_sent < server_public.bytes_sent );
    rocketnet_check( slot_public.packets_received >= server_public.payloads_received && slot_public.packets_received < server_public.packets_received );
    rocketnet_check( slot_public.bytes_received < server_public.bytes_received );
    rocketnet_check( slot_public.bytes_sent > client_message_bytes_received + slot_public.packets_sent * ipv4_udp_header_bytes );
    rocketnet_check( slot_public.bytes_received > server_message_bytes_received + slot_public.packets_received * ipv4_udp_header_bytes );
    rocketnet_check( slot_public.rtt_ms == (double) cls.rtt && slot_public.rtt_ms >= 0.0 );
    rocketnet_check( slot_public.jitter_ms == (double) cls.jitter && slot_public.jitter_ms >= 0.0 );
    rocketnet_check( slot_public.packet_loss_percent == (double) cls.packet_loss && slot_public.packet_loss_percent >= 0.0 );
    rocketnet_check( slot_public.bandwidth_sent_kbps == (double) cls.bandwidth_sent_kbps && slot_public.bandwidth_sent_kbps >= 0.0 );
    rocketnet_check( slot_public.bandwidth_received_kbps == (double) cls.bandwidth_received_kbps && slot_public.bandwidth_received_kbps >= 0.0 );
    rocketnet_check( slot_public.bandwidth_acked_kbps == (double) cls.bandwidth_acked_kbps && slot_public.bandwidth_acked_kbps >= 0.0 );

    // a free slot, and a slot past max_clients, read 0
    const int free_slot = slot == 0 ? 1 : 0;
    rocketnet_check( !rocketnet_server_client_connected( server, free_slot ) );
    rocketnet_check( rn_server_client_stats_of( server, free_slot ).packets_sent == 0 );
    rocketnet_check( rn_server_client_stats_of( server, free_slot ).rtt_ms == 0.0 );
    rocketnet_check( rn_server_client_stats_of( server, 3 ).packets_sent == 0 );
    rocketnet_check( rocketnet_server_client_id( server, 3 ) == 0 );

    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
}
#endif // #if ROCKETNET_DEV

void rocketnet_test_config_precedence()
{
    // the transport's payload capacity is a constant now, so the removed
    // field's env name is inert: nothing displaces anything

    struct rocketnet_test_env_guard_t payload_guard;
    rocketnet_test_env_clear( &payload_guard, "ROCKETNET_SERVER_MAX_PAYLOAD_BYTES" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    // This selection/configuration fixture needs one connection, not 1000 slots.
    config.max_clients = 1;

    // default layer

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    // the removed field's env name moves nothing: the transport's payload
    // capacity is a constant, not a configured one

    setenv( "ROCKETNET_SERVER_MAX_PAYLOAD_BYTES", "20000", 1 );
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &payload_guard );
}

// the rung-forcing exchange: pin a transport list on the server, expect the
// named grant, and pass real payloads both ways through it -- the matrix
// doctrine's runtime form. the pin rides the struct, so ROCKETNET_SERVER_TRANSPORT
// is cleared for the duration (env would win, by design). port zero per
// the #107 law: the kernel grants the port and the token is minted for
// what was granted, so no other process on the box can race this test.

// whether a whole-suite pin (pin_name) leaves this selection check
// meaningful: no pin, or a pin of the expected transport
static bool rocketnet_test_transport_pin_allows( const char * pin_name, const char * expected_transport )
{
    char pin[256];
    return !rocketnet_test_getenv( pin_name, pin, sizeof( pin ) ) || pin[0] == '\0' || ( expected_transport && strcmp( pin, expected_transport ) == 0 );
}

static void rocketnet_test_transport_io_pin_exchange( const char * transport_list, const char * expected_transport )
{
    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    // This selection/configuration fixture needs one connection, not 1000 slots.
    server_config.max_clients = 1;
    snprintf( server_config.transport, sizeof(server_config.transport), "%s", transport_list );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config);
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), expected_transport ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent < 5 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "rung!", 5 );
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "rung!", 5 );
            sent++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload && payload_bytes == 5 && memcmp( payload, "rung!", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 && memcmp( packet, "rung!", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 5 && client_received >= 5 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 5 );
    rocketnet_check( client_received >= 5 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &transport_guard );
}

void rocketnet_test_server_transport_io_pin_portable()
{
    // the portable rung carries traffic on every platform -- the floor
    // must never be the untested tier

    rocketnet_test_transport_io_pin_exchange( "portable", "portable" );
}

void rocketnet_test_server_transport_io_pin_loopback()
{
    // the loopback rung is pin-only and carries datagrams in process, with
    // no socket. both ends pin it here: auto never selects it, so a
    // server-only pin would leave the client on a socket path.

    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    // Keep a spare slot so the socket-client refusal below cannot pass
    // merely because the loopback client filled the server.
    server_config.max_clients = 2;
    snprintf( server_config.transport, sizeof(server_config.transport), "loopback" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config);
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "loopback" );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), "loopback" ) == 0 );

    // Built-in loopback must use the same worker-owned send rings as the
    // socket transports even though its backend has no native wait object.
    // A custom BASE backend remains a separate compatibility path.
    rocketnet_check( client_config.step_driven == 0 );
    rocketnet_check( rocketnet_client_transport_client_io( client ) != NULL );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent < 5 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "rung!", 5 );
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "rung!", 5 );
            sent++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload && payload_bytes == 5 && memcmp( payload, "rung!", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 && memcmp( packet, "rung!", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 5 && client_received >= 5 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 5 );
    rocketnet_check( client_received >= 5 );

    // a second client on a socket transport must never reach the same
    // address: the loopback server holds no socket, so only an in-process
    // endpoint can answer. this is what proves the traffic never left.

    struct rocketnet_client_transport_config_t portable_config;
    rocketnet_functional_client_default_config( &portable_config );
    snprintf( portable_config.transport, sizeof(portable_config.transport), "portable" );

    struct rocketnet_client_transport_t * portable = rocketnet_client_transport_create( NULL, connect_token, &portable_config, rocketnet_platform_time() );
    rocketnet_check( portable );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( portable ), "portable" ) == 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        rocketnet_client_transport_update( portable, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( portable ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( portable );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void rocketnet_test_transport_env_pins_loopback()
{
    // the per-endpoint env names win over the struct: both configs keep
    // their default "auto" and the environment still selects loopback on
    // both ends. the process-wide spelling stays cleared.

    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_save( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_save( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
    setenv( "ROCKETNET_SERVER_TRANSPORT", "loopback", 1 );
    setenv( "ROCKETNET_CLIENT_TRANSPORT", "loopback", 1 );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    // This selection/configuration fixture needs one connection, not 1000 slots.
    server_config.max_clients = 1;
    rocketnet_check( strcmp( server_config.transport, "auto" ) == 0 );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config);
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "loopback" ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    rocketnet_check( strcmp( client_config.transport, "auto" ) == 0 );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), "loopback" ) == 0 );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void transport_env_pin_is_honoured()
{
    char server_pin[256];
    char client_pin[256];
    const bool has_server_pin = ( rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", server_pin, sizeof( server_pin ) ) && server_pin[0] != '\0' );
    const bool has_client_pin = ( rocketnet_test_getenv( "ROCKETNET_CLIENT_TRANSPORT", client_pin, sizeof( client_pin ) ) && client_pin[0] != '\0' );

    if ( has_server_pin )
    {
        struct rocketnet_server_transport_config_t server_config;
        rocketnet_functional_server_config( &server_config );
        server_config.max_clients = 1;
        rocketnet_check( strcmp( server_config.transport, "auto" ) == 0 );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ) );
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), server_pin ) == 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
    }

    if ( has_client_pin )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_functional_write_token( "127.0.0.1:41040", rocketnet_random_uint64(), rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

        struct rocketnet_client_transport_config_t client_config;
        rocketnet_functional_client_default_config( &client_config );
        rocketnet_check( strcmp( client_config.transport, "auto" ) == 0 );

        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
        rocketnet_check( client );
        rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), client_pin ) == 0 );
        ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    }
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void rocketnet_test_server_transport_io_pin_iocp()
{
    rocketnet_test_transport_io_pin_exchange( "iocp", "iocp" );
}

void rocketnet_test_server_transport_io_pin_rio()
{
    rocketnet_test_transport_io_pin_exchange( "rio", "rio" );
}

// the rio transport (sdk/rocketnet_server_rio_windows.cpp, tla/RNRioDriver.tla)
// against a plain UDP client on loopback

#include "rocketnet_server_rio.h"

// The I/O worker returns a posted phase idle only after its completions are
// retired, so start_packet may be refused (and count SEND_FULL) until a later
// flush opens the other phase. Every earlier reservation is finished, so the
// retry may flush. refused counts the refusals for the counter checks.
static uint8_t * rocketnet_test_server_rio_start( rocketnet_server_rio_t * rio, const rocketnet_address_t * to,
    uint64_t * packet_id, uint64_t * refused )
{
    const double start = rocketnet_platform_time();
    for ( ;; )
    {
        uint8_t * frame = rocketnet_server_rio_start_packet( rio, to, packet_id );
        if ( frame || rocketnet_platform_time() - start >= 5.0 )
            return frame;
        ( *refused )++;
        rocketnet_server_rio_send_packets( rio );
        rocketnet_platform_sleep( 0.0005 );
    }
}

void rocketnet_test_server_rio_probe()
{
    rocketnet_check( rocketnet_server_rio_available() );

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );
    rocketnet_check( bind_address.port != 0 );

    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( rio, counters );
    for ( int i = 0; i < ROCKETNET_RIO_NUM_COUNTERS; i++ )
        rocketnet_check( counters[i] == 0 );

    // nothing but the receive slices is outstanding, and a flush with
    // nothing staged posts nothing and commits nothing
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );
    rocketnet_server_rio_send_packets( rio );
    rocketnet_server_rio_receive_packets( rio );
    rocketnet_check( rocketnet_server_rio_process_packets( rio )->num_packets == 0 );
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );
    rocketnet_server_rio_counters( rio, counters );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_COMMITS] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] == 0 );

    // a reserved slot that is aborted is never posted
    rocketnet_address_t to;
    rocketnet_check( rocketnet_address_parse( &to, "127.0.0.1:9" ) );
    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_server_rio_start_packet( rio, &to, &packet_id );
    rocketnet_check( frame );
    rocketnet_server_rio_abort_packet( rio, packet_id, frame );
    rocketnet_server_rio_send_packets( rio );
    rocketnet_server_rio_counters( rio, counters );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_COMMITS] == 0 );
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );

    // destroy with nothing but receives outstanding: closesocket completes
    // them, the drain accounts for every one, then the region is freed
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );

#if ROCKETNET_DEV
    // each development constructor stage refuses cleanly and unwinds what it built
    for ( int stage = 1; stage <= 6; stage++ )
    {
        rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        rocketnet_check( rocketnet_server_rio_create( NULL, &address, stage ) == NULL );
    }
#endif

    // and a create after the refusals still works
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
}

// every size the wire allows, each packet tagged by its index in its first
// two bytes, in chunks so neither socket buffer overflows on loopback

#define ROCKETNET_RIO_TEST_PACKETS ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 )
#define ROCKETNET_RIO_TEST_CHUNK 128

static_assert( ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES >= 2, "the tag is two bytes" );

void rocketnet_test_server_rio_wire_ipv4()
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    rocketnet_check( client_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    server_address.port = bind_address.port;

    static bool seen[ROCKETNET_RIO_TEST_PACKETS];
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // client -> transport: each datagram surfaces byte-exact, from the client

    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;
    for ( int first = 0; first < ROCKETNET_RIO_TEST_PACKETS; first += ROCKETNET_RIO_TEST_CHUNK )
    {
        const int last = first + ROCKETNET_RIO_TEST_CHUNK < ROCKETNET_RIO_TEST_PACKETS ? first + ROCKETNET_RIO_TEST_CHUNK : ROCKETNET_RIO_TEST_PACKETS;
        for ( int i = first; i < last; i++ )
        {
            const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + i;
            packet[0] = (uint8_t) i;
            packet[1] = (uint8_t) ( i >> 8 );
            for ( int j = 2; j < packet_bytes; j++ )
                packet[j] = (uint8_t) ( i * 31 + j );
            rocketnet_platform_socket_send_packet( client_socket, &server_address, packet, packet_bytes );
        }
        double start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 && num_seen < last )
        {
            rocketnet_server_rio_receive_packets( rio );
            rocketnet_server_socket_process_packets_t * received = rocketnet_server_rio_process_packets( rio );
            for ( int k = 0; k < received->num_packets; k++ )
            {
                const uint8_t * data = received->packet_data[k];
                const int tag = data[0] | ( data[1] << 8 );
                rocketnet_check( tag >= first && tag < last );
                rocketnet_check( !seen[tag] );
                rocketnet_check( received->packet_bytes[k] == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + tag );
                rocketnet_check( received->from[k].type == ROCKETNET_ADDRESS_IPV4 );
                rocketnet_check( received->from[k].port == client_address.port );
                rocketnet_check( received->from[k].data.ipv4[0] == 127 && received->from[k].data.ipv4[3] == 1 );
                for ( int j = 2; j < received->packet_bytes[k]; j++ )
                    rocketnet_check( data[j] == (uint8_t) ( tag * 31 + j ) );
                seen[tag] = true;
                num_seen++;
            }
            rocketnet_platform_sleep( 0.0005 );
        }
        rocketnet_check( num_seen == last );
    }

    // transport -> client: one chunk per phase, so the chunks rotate through the
    // three send phases; a flush keeps a phase open while the next is not yet
    // idle, so the wait flushes again, as a frame loop does; each reply
    // arrives byte-exact

    memset( seen, 0, sizeof(seen) );
    num_seen = 0;
    int flushes_with_packets = 0;
    uint64_t refused = 0;
    bool used_phase[3] = { false, false, false };
    for ( int first = 0; first < ROCKETNET_RIO_TEST_PACKETS; first += ROCKETNET_RIO_TEST_CHUNK )
    {
        const int last = first + ROCKETNET_RIO_TEST_CHUNK < ROCKETNET_RIO_TEST_PACKETS ? first + ROCKETNET_RIO_TEST_CHUNK : ROCKETNET_RIO_TEST_PACKETS;
        for ( int i = first; i < last; i++ )
        {
            uint64_t packet_id = 0;
            uint8_t * frame = rocketnet_test_server_rio_start( rio, &client_address, &packet_id, &refused );
            rocketnet_check( frame );
            used_phase[packet_id / ROCKETNET_RIO_SEND_QUEUE_SIZE] = true;
            const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + i;
            uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
            wire[0] = (uint8_t) i;
            wire[1] = (uint8_t) ( i >> 8 );
            for ( int j = 2; j < packet_bytes; j++ )
                wire[j] = (uint8_t) ( i * 47 + j );
            rocketnet_server_rio_finish_packet( rio, packet_id, frame, packet_bytes );
        }
        rocketnet_server_rio_send_packets( rio );
        flushes_with_packets++;

        double start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 && num_seen < last )
        {
            rocketnet_server_rio_send_packets( rio );
            rocketnet_address_t from;
            const int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
            if ( packet_bytes <= 0 )
            {
                rocketnet_platform_sleep( 0.0005 );
                continue;
            }
            rocketnet_check( from.port == bind_address.port );
            const int tag = packet[0] | ( packet[1] << 8 );
            rocketnet_check( tag >= first && tag < last );
            rocketnet_check( !seen[tag] );
            rocketnet_check( packet_bytes == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + tag );
            for ( int j = 2; j < packet_bytes; j++ )
                rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
            seen[tag] = true;
            num_seen++;
        }
        rocketnet_check( num_seen == last );
    }
    rocketnet_check( used_phase[0] && used_phase[1] && used_phase[2] );

    // the last phase's completions retire on the next flushes
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    double start = rocketnet_platform_time();
    do
    {
        rocketnet_server_rio_send_packets( rio );
        rocketnet_server_rio_counters( rio, counters );
        if ( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] >= ROCKETNET_RIO_TEST_PACKETS )
            break;
        rocketnet_platform_sleep( 0.0005 );
    } while ( rocketnet_platform_time() - start < 5.0 );

    rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_rio_transport_server_wire_ipv4: %d packets each way, %d flushes, COMMITS %llu, SEND_WAITS %llu",
                      ROCKETNET_RIO_TEST_PACKETS, flushes_with_packets,
                      (unsigned long long) counters[ROCKETNET_RIO_COUNTER_COMMITS], (unsigned long long) counters[ROCKETNET_RIO_COUNTER_SEND_WAITS] );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] == ROCKETNET_RIO_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == ROCKETNET_RIO_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] == counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_COMMITS] >= (uint64_t) flushes_with_packets );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FAILED] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FULL] == refused );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_FAILED] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED] == 0 );
    // the worker counts a completion before it re-posts or retires the request
    start = rocketnet_platform_time();
    while ( rocketnet_server_rio_pending( rio ) != ROCKETNET_RIO_RECEIVES && rocketnet_platform_time() - start < 5.0 )
        rocketnet_platform_sleep( 0.0005 );
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
}

// one exact packet in each direction, with the client and destination families
// selected by the caller; source normalization stays observable on receive
static void rocketnet_test_server_rio_wire_exchange( rocketnet_server_rio_t * rio,
    struct rocketnet_platform_socket_t * client_socket, const rocketnet_address_t * client_address,
    const rocketnet_address_t * server_address )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
    for ( int i = 0; i < packet_bytes; ++i )
        packet[i] = (uint8_t) ( 0x40 + i );

    rocketnet_platform_socket_send_packet( client_socket, server_address, packet, packet_bytes );

    bool received_from_client = false;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !received_from_client )
    {
        rocketnet_server_rio_receive_packets( rio );
        rocketnet_server_socket_process_packets_t * received = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < received->num_packets; ++i )
        {
            rocketnet_check( !received_from_client );
            rocketnet_check( received->packet_bytes[i] == packet_bytes );
            rocketnet_check( memcmp( received->packet_data[i], packet, packet_bytes ) == 0 );
            rocketnet_check( rocketnet_address_equal( &received->from[i], client_address ) );
            received_from_client = true;
        }
        if ( !received_from_client )
            rocketnet_platform_sleep( 0.0005 );
    }
    rocketnet_check( received_from_client );

    uint64_t packet_id = 0;
    uint64_t refused = 0;
    uint8_t * frame = rocketnet_test_server_rio_start( rio, client_address, &packet_id, &refused );
    rocketnet_check( frame );
    uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
    for ( int i = 0; i < packet_bytes; ++i )
        wire[i] = (uint8_t) ( 0x70 + i );
    rocketnet_server_rio_finish_packet( rio, packet_id, frame, packet_bytes );
    rocketnet_server_rio_send_packets( rio );

    bool received_reply = false;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !received_reply )
    {
        rocketnet_server_rio_send_packets( rio ); // a flush may keep the phase open a frame
        rocketnet_address_t from;
        const int bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, sizeof(packet) );
        if ( bytes > 0 )
        {
            rocketnet_check( bytes == packet_bytes );
            for ( int i = 0; i < packet_bytes; ++i )
                rocketnet_check( packet[i] == (uint8_t) ( 0x70 + i ) );
            rocketnet_check( from.port == server_address->port );
            received_reply = true;
        }
        else
            rocketnet_platform_sleep( 0.0005 );
    }
    rocketnet_check( received_reply );
}

static struct rocketnet_platform_socket_t * rocketnet_test_server_rio_client_socket( const char * address_string,
    rocketnet_address_t * client_address )
{
    rocketnet_check( rocketnet_address_parse( client_address, address_string ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, client_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    rocketnet_check( client_address->port != 0 );
    return client_socket;
}

void rocketnet_test_server_rio_wire_ipv6()
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "[::1]:0" ) );
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );

    rocketnet_address_t client_address;
    struct rocketnet_platform_socket_t * client_socket = rocketnet_test_server_rio_client_socket( "[::1]:0", &client_address );
    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "[::1]:0" ) );
    server_address.port = bind_address.port;

    rocketnet_test_server_rio_wire_exchange( rio, client_socket, &client_address, &server_address );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
}

void rocketnet_test_server_rio_wire_dual_stack()
{
    rocketnet_address_t bind_address;
    // wildcard bind kept: the case is the ipv6 wildcard listen serving ipv4 and ipv6 clients
    rocketnet_check( rocketnet_address_parse( &bind_address, "[::]:0" ) );
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );

    rocketnet_address_t client_address;
    struct rocketnet_platform_socket_t * client_socket = rocketnet_test_server_rio_client_socket( "127.0.0.1:0", &client_address );
    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    server_address.port = bind_address.port;
    rocketnet_test_server_rio_wire_exchange( rio, client_socket, &client_address, &server_address );
    rocketnet_platform_socket_destroy( client_socket );

    client_socket = rocketnet_test_server_rio_client_socket( "[::1]:0", &client_address );
    rocketnet_check( rocketnet_address_parse( &server_address, "[::1]:0" ) );
    server_address.port = bind_address.port;
    rocketnet_test_server_rio_wire_exchange( rio, client_socket, &client_address, &server_address );
    rocketnet_platform_socket_destroy( client_socket );

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
}

void rocketnet_test_server_rio_send_full()
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind_address );
    rocketnet_check( rio );

    rocketnet_address_t destination;
    rocketnet_check( rocketnet_address_parse( &destination, "127.0.0.1:9" ) );
    uint8_t * frame = NULL;
    for ( int i = 0; i < ROCKETNET_RIO_SEND_QUEUE_SIZE; ++i )
    {
        uint64_t packet_id = 0;
        frame = rocketnet_server_rio_start_packet( rio, &destination, &packet_id );
        rocketnet_check( frame );
        rocketnet_server_rio_abort_packet( rio, packet_id, frame );
    }

    uint64_t packet_id = 0;
    rocketnet_check( rocketnet_server_rio_start_packet( rio, &destination, &packet_id ) == NULL );
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( rio, counters );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FULL] == 1 );

    rocketnet_server_rio_send_packets( rio );
    rocketnet_server_rio_counters( rio, counters );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FAILED] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_COMMITS] == 0 );
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );

    frame = rocketnet_server_rio_start_packet( rio, &destination, &packet_id );
    rocketnet_check( frame );
    rocketnet_server_rio_abort_packet( rio, packet_id, frame );
    rocketnet_server_rio_send_packets( rio );
    rocketnet_server_rio_counters( rio, counters );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FULL] == 1 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_PACKETS_SENT] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_SEND_FAILED] == 0 );
    rocketnet_check( counters[ROCKETNET_RIO_COUNTER_COMMITS] == 0 );
    rocketnet_check( rocketnet_server_rio_pending( rio ) == ROCKETNET_RIO_RECEIVES );

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
}

// the iocp transport's USO/URO tier against a plain UDP client on loopback: the
// client sends tagged packets in, the transport must surface each byte-exact from
// the client's address; the transport then sends replies, deliberately in
// equal-size same-destination runs (the shape USO coalesces), and the client
// must receive each as its own datagram, byte-exact and in order

#define ROCKETNET_IOCP_TEST_PACKETS 200

static int rocketnet_test_iocp_reply_bytes( int tag )
{
    return tag < ROCKETNET_IOCP_TEST_PACKETS / 2 ? 400
         : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
}

static void rocketnet_test_iocp_exchange( rocketnet_server_iocp_t * iocp, const rocketnet_address_t * bind_address )
{
    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    rocketnet_check( client_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    server_address.port = bind_address->port;

    // client -> transport: equal-size bursts first (the shape URO may coalesce),
    // then every size the wire allows

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < ROCKETNET_IOCP_TEST_PACKETS; i++ )
    {
        const int packet_bytes = rocketnet_test_iocp_reply_bytes( i );
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            packet[j] = (uint8_t) ( i * 31 + j );
        rocketnet_platform_socket_send_packet( client_socket, &server_address, packet, packet_bytes );
    }

    bool seen[ROCKETNET_IOCP_TEST_PACKETS];
    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_IOCP_TEST_PACKETS )
    {
        rocketnet_server_iocp_receive_packets( iocp );
        rocketnet_server_socket_process_packets_t * received = rocketnet_server_iocp_process_packets( iocp );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            rocketnet_check( tag < ROCKETNET_IOCP_TEST_PACKETS );
            rocketnet_check( !seen[tag] );
            rocketnet_check( received->packet_bytes[i] == rocketnet_test_iocp_reply_bytes( tag ) );
            rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 && received->from[i].port == client_address.port );
            for ( int j = 1; j < received->packet_bytes[i]; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 31 + j ) );
            seen[tag] = true;
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == ROCKETNET_IOCP_TEST_PACKETS );

    // transport -> client, in one flush

    for ( int i = 0; i < ROCKETNET_IOCP_TEST_PACKETS; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_iocp_start_packet( iocp, &client_address, &packet_id );
        rocketnet_check( frame );
        const int packet_bytes = rocketnet_test_iocp_reply_bytes( i );
        uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
        wire[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            wire[j] = (uint8_t) ( i * 47 + j );
        rocketnet_server_iocp_finish_packet( iocp, packet_id, frame, packet_bytes );
    }
    rocketnet_server_iocp_send_packets( iocp );

    int next_tag = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && next_tag < ROCKETNET_IOCP_TEST_PACKETS )
    {
        rocketnet_address_t from;
        const int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes <= 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        rocketnet_check( from.port == bind_address->port );
        const int tag = packet[0];
        rocketnet_check( tag == next_tag );                 // in order, each its own datagram
        rocketnet_check( packet_bytes == rocketnet_test_iocp_reply_bytes( tag ) );
        for ( int j = 1; j < packet_bytes; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
        next_tag++;
    }
    rocketnet_check( next_tag == ROCKETNET_IOCP_TEST_PACKETS );

    // every frame comes back to the pool once the completions retire
    start = rocketnet_platform_time();
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    do
    {
        rocketnet_server_iocp_counters( iocp, counters );
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] >= ROCKETNET_IOCP_TEST_PACKETS )
            break;
        rocketnet_platform_sleep( 0.001 );
    } while ( rocketnet_platform_time() - start < 5.0 );
    rocketnet_check( counters[ROCKETNET_IOCP_SEND_COMPLETED] == ROCKETNET_IOCP_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_IOCP_SEND_FAILED] == 0 );

    rocketnet_platform_socket_destroy( client_socket );
}

void rocketnet_test_server_iocp_uso_uro_probe()
{
    // the features are probes whose result belongs to the host; the disable
    // flags force the per-datagram path, and an exchange works on each

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rocketnet_server_iocp_t * iocp = rocketnet_server_iocp_create( NULL, &bind_address, 2 );
    rocketnet_check( iocp );
    rocketnet_check( bind_address.port != 0 );
    const int features = rocketnet_server_iocp_features( iocp );
    rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_iocp_transport_server_uso_uro_probe: this host reports USO %s, URO %s",
                      ( features & ROCKETNET_IOCP_FEATURE_USO ) ? "yes" : "no",
                      ( features & ROCKETNET_IOCP_FEATURE_URO ) ? "yes" : "no" );
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( iocp, counters );
    rocketnet_check( counters[ROCKETNET_IOCP_USO_SENDS] == 0 );
    rocketnet_check( counters[ROCKETNET_IOCP_URO_BATCHES] == 0 );
    rocketnet_check( counters[ROCKETNET_IOCP_USO_FALLBACKS] == 0 );
    rocketnet_test_iocp_exchange( iocp, &bind_address );
    rn_native_backend_stop_and_destroy( iocp, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );

#if ROCKETNET_DEV
    rocketnet_address_t plain_address;
    rocketnet_check( rocketnet_address_parse( &plain_address, "127.0.0.1:0" ) );
    rocketnet_server_iocp_t * plain = rocketnet_server_iocp_create( NULL, &plain_address, 2, 0,
                                                                    ROCKETNET_IOCP_DISABLE_USO | ROCKETNET_IOCP_DISABLE_URO );
    rocketnet_check( plain );
    rocketnet_check( ( rocketnet_server_iocp_features( plain ) & ROCKETNET_IOCP_FEATURE_USO ) == 0 );
    rocketnet_check( ( rocketnet_server_iocp_features( plain ) & ROCKETNET_IOCP_FEATURE_URO ) == 0 );
    rocketnet_test_iocp_exchange( plain, &plain_address );
    rocketnet_server_iocp_counters( plain, counters );
    rocketnet_check( counters[ROCKETNET_IOCP_USO_SENDS] == 0 );
    rocketnet_check( counters[ROCKETNET_IOCP_URO_BATCHES] == 0 );
    rn_native_backend_stop_and_destroy( plain, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
#endif
}

void rocketnet_test_server_iocp_uso_wire()
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1:0" ) );
    rocketnet_server_iocp_t * iocp = rocketnet_server_iocp_create( NULL, &bind_address, 2 );
    rocketnet_check( iocp );
    const int features = rocketnet_server_iocp_features( iocp );
    rocketnet_test_iocp_exchange( iocp, &bind_address );

    // on the USO tier the equal-size runs must actually have gone out as
    // segmented sends: the counter is the proof the fast path ran

    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( iocp, counters );
    rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_iocp_transport_server_uso_wire: USO_SENDS %llu, USO_FALLBACKS %llu, URO_BATCHES %llu",
                      (unsigned long long) counters[ROCKETNET_IOCP_USO_SENDS], (unsigned long long) counters[ROCKETNET_IOCP_USO_FALLBACKS],
                      (unsigned long long) counters[ROCKETNET_IOCP_URO_BATCHES] );
    if ( ( features & ROCKETNET_IOCP_FEATURE_USO ) && ( rocketnet_server_iocp_features( iocp ) & ROCKETNET_IOCP_FEATURE_USO ) )
        rocketnet_check( counters[ROCKETNET_IOCP_USO_SENDS] > 0 );
    rn_native_backend_stop_and_destroy( iocp, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
}
#endif

void rocketnet_test_server_port_zero_transport()
{
    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );

    // THE ACCEPT SIDE FIRST, and it is the load-bearing half: this rule
    // must never cost a port-zero create that a discovering transport can
    // serve. auto on every platform, plus every discovering transport this
    // platform has, pinned one at a time.

    for ( int i = -1; i < num_available; i++ )
    {
        const char * list = "auto";
        if ( i >= 0 )
        {
            if ( !rocketnet_transport_discovers_bound_port( available[i] ) )
                continue;                                   // the refuse side below owns those
            list = rocketnet_transport_name( available[i] );
        }

        struct rocketnet_server_transport_config_t config;
        rocketnet_functional_server_config( &config );
        // This selection/configuration fixture needs one connection, not 1000 slots.
        config.max_clients = 1;
        snprintf( config.transport, sizeof(config.transport), "%s", list );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);
        rocketnet_check( server );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
    }

    // THE REFUSE SIDE, where this platform actually carries a
    // non-discovering transport. the loop body is the linux legs; elsewhere
    // the accept side above is the whole test, which is the honest state
    // of a rule about a transport that does not exist on this platform.

    for ( int i = 0; i < num_available; i++ )
    {
        if ( rocketnet_transport_discovers_bound_port( available[i] ) )
            continue;

        const char * name = rocketnet_transport_name( available[i] );

        // a PIN on a non-discovering rung refuses the create, and the log
        // names the rule and the transport. that naming IS the fix: without
        // it an operator debugs a cookie gate that never matches.
        //
        // captured at the ERROR level ON PURPOSE, because that is the
        // level a deployed binary and this suite run at:
        // the per-rung warn is invisible there, so if the rule were only a
        // warn a pinned port-zero create would return NULL with nothing
        // anywhere saying why. The exhausted-ladder path restates it as an
        // error, and this asserts that restatement rather than the warn.

        struct rocketnet_server_transport_config_t config;
        rocketnet_functional_server_config( &config );
        // This selection/configuration fixture needs one connection, not 1000 slots.
        config.max_clients = 1;
        snprintf( config.transport, sizeof(config.transport), "%s", name );

        rocketnet_test_captured_log[0] = '\0';
        const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
        rocketnet_log_function( rocketnet_test_capture_log, NULL );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function, NULL );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "cannot discover the port it was granted and hand it back" ) != NULL );

        // and at a level that shows warns, the RUNG's own refusal names
        // the rule and the transport together -- matched as one phrase, since
        // the transport name alone also appears in the exhausted-ladder error
        // and would pass this check without the rule being mentioned

        char expected_rung_refusal[128];
        snprintf( expected_rung_refusal, sizeof(expected_rung_refusal), "server transport %s refused: a port-zero public address", name );

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log, NULL );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function, NULL );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, expected_rung_refusal ) != NULL );

        // SCOPE, the other half of correct: the rule is about port ZERO
        // and nothing else. the same pin with a real port never meets it
        // and goes on into the transport's own create, whose FIRST act is the
        // commit barrier -- the suite never mutates a host. That refusal
        // is asserted by its message rather than merely tolerated: it is
        // the one path in this test that enters the xdp transport at all, and
        // it is what keeps the BARRIER exercised, since the rule
        // short-circuits the port-zero pins that would otherwise reach it.

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log, NULL );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_test_server_transport_create_on_free_port( &config );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function, NULL );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "port-zero public address" ) == NULL );
#if ROCKETNET_XDP
        rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );
#else
        // a build with XDP compiled out (no BPF-capable clang, libbpf or
        // AF_XDP provider at configure) is supported: the rung refuses
        // cleanly at its own create and never reaches the commit barrier
        rocketnet_check( strstr( rocketnet_test_captured_log, "XDP is compiled out of this build" ) != NULL );
#endif

        // and the rung refusal is a RUNG refusal, not a create refusal:
        // put a discovering rung after it and the ladder falls, the port
        // is discovered, and the advertised address is the real one.

        snprintf( config.transport, sizeof(config.transport), "%s,portable", name );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "portable" ) == 0 );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
    }

    rocketnet_test_env_restore( &transport_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
void rocketnet_test_server_transport_io_pin_gso()
{
    rocketnet_test_transport_io_pin_exchange( "gso", "gso" );

    // per-transport bounds: a queue count past the gso geometry refuses the
    // pinned rung -- and with nothing left to fall to, the create --
    // loudly, never clamped

    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    snprintf( config.transport, sizeof(config.transport), "gso" );
    config.num_xdp_queues = ROCKETNET_GSO_MAX_QUEUES + 1;

    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config) == NULL );

    rocketnet_test_env_restore( &transport_guard );
}

#include "rocketnet_driver.h"
#include "rocketnet_server_uring.h"

void rocketnet_test_server_transport_io_pin_io_uring()
{
    // a build without liburing has an io_uring row with no backend: the row
    // is unavailable, and a pin of it is skipped softly by the ladder, so
    // a ladder that names it falls to the next rung

    const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( ROCKETNET_TRANSPORT_IO_URING );
    rocketnet_check( definition );
    if ( !definition->backend )
    {
        rocketnet_test_transport_io_pin_exchange( "io_uring,portable", "portable" );
        return;
    }

    rocketnet_test_transport_io_pin_exchange( "io_uring", "io_uring" );

    // per-transport bounds: a queue count past the io_uring geometry refuses
    // the pinned rung, and with nothing left to fall to, the create, loudly,
    // never clamped

    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    snprintf( config.transport, sizeof(config.transport), "io_uring" );
    config.num_xdp_queues = ROCKETNET_URING_MAX_QUEUES + 1;

    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config) == NULL );

    rocketnet_test_env_restore( &transport_guard );
}

#if ROCKETNET_XDP
bool rocketnet_test_xdp_veth_runs_here();
struct rocketnet_server_transport_t * rocketnet_xdp_veth_start( bool ipv6, const char * case_name, char * server_address, size_t server_address_bytes );
void rocketnet_xdp_veth_pair_down( struct rocketnet_server_transport_t * destroyed );
#endif

static enum rocketnet_log_level_t rocketnet_test_xdp_capture_begin()
{
    rocketnet_test_captured_log[0] = '\0';
    const enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_function( rocketnet_test_capture_log, NULL );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    return saved;
}

static void rocketnet_test_xdp_capture_end( enum rocketnet_log_level_t saved )
{
    rocketnet_log_level( saved );
    rocketnet_log_function( rocketnet_test_suite_log_function, NULL );
}

#if ROCKETNET_XDP
static void rocketnet_test_xdp_server_destroy( struct rocketnet_server_transport_t * server )
{
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

// The admitted guest's natural provisioned path: the production create is
// granted the requested xdp backend on the veth pair the guest cases build,
// then the pair and its server are torn down. This is not a preflight call.

static void rocketnet_test_xdp_natural_guest( const char * case_name )
{
    char natural_address[128];
    struct rocketnet_server_transport_t * natural =
        rocketnet_xdp_veth_start( false, case_name, natural_address, sizeof(natural_address) );
    rocketnet_check( natural );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( natural ), "xdp" ) == 0 );
    rocketnet_xdp_veth_pair_down( natural );
}

// The documented natural provisioned ladder result: a port-zero address has
// the port-zero rule refuse xdp before its create, and the ladder grants gso,
// which discovers the real port. The rule is named in the log.

static void rocketnet_test_xdp_ladder_port_zero( const struct rocketnet_server_transport_config_t * config )
{
    enum rocketnet_log_level_t saved_level = rocketnet_test_xdp_capture_begin();
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", config );
    rocketnet_test_xdp_capture_end( saved_level );

    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "gso" ) == 0 );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
    rocketnet_check( strstr( rocketnet_test_captured_log, "a port-zero public address asks the kernel for an ephemeral port" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "server transport ladder granted gso after refusals: xdp" ) != NULL );
    rocketnet_test_xdp_server_destroy( server );
}
#endif

void rocketnet_test_server_transport_io_pin_xdp()
{
    // The pin is asserted through the production create in every branch:
    // a natural pre-flight answer, a development-forced unavailable answer,
    // and a development-forced available answer. An available answer is
    // never paired with an allowed commit, so this case cannot attach a
    // data plane on a host it has not been admitted to; the refusal it then
    // proves is the suite's commit barrier, named in the log. On the
    // admitted XDP guest the natural available answer runs the real create
    // on the veth pair the guest cases build, and asserts the granted xdp
    // backend, then tears the pair down.

    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    char real_port_address[64];
    rocketnet_test_free_loopback_address( real_port_address, sizeof(real_port_address) );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    config.max_clients = 1;
    snprintf( config.transport, sizeof(config.transport), "xdp" );

#if ROCKETNET_XDP
#if ROCKETNET_DEV
    rocketnet_server_socket_clear_preflight_test_result();
#endif

    // the natural answer, read once; every later branch is either this
    // outcome or an explicitly forced one

    const bool naturally_provisioned = rocketnet_server_socket_preflight( real_port_address, 7 );

#if ROCKETNET_DEV
    // FORCED UNAVAILABLE: the direct pre-flight says no, and the production
    // pin reaches that same pre-flight because the barrier is lifted. The
    // pre-flight is non-mutating and returns before create's first act, so
    // no data plane can be attached.

    rocketnet_server_socket_set_preflight_test_result( 0 );
    rocketnet_check( !rocketnet_server_socket_preflight( real_port_address, 7 ) );

    rocketnet_server_socket_allow_commit();
    enum rocketnet_log_level_t saved_level = rocketnet_test_xdp_capture_begin();
    struct rocketnet_server_transport_t * server = rocketnet_test_server_transport_create_on_free_port( &config );
    rocketnet_test_xdp_capture_end( saved_level );
    rocketnet_server_socket_forbid_commit();

    rocketnet_check( server == NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "xdp pre-flight: development control selects unavailable" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "xdp pre-flight failed: this machine is not provisioned for the XDP data plane" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "refused: xdp" ) != NULL );

    // FORCED AVAILABLE: the direct pre-flight says yes, while the barrier
    // stays installed, so the production pin is refused by the barrier
    // before any pre-flight and cannot attach. The refusal is asserted by
    // its own name, not merely tolerated.

    rocketnet_server_socket_set_preflight_test_result( 1 );
    rocketnet_check( rocketnet_server_socket_preflight( real_port_address, 7 ) );

    saved_level = rocketnet_test_xdp_capture_begin();
    server = rocketnet_test_server_transport_create_on_free_port( &config );
    rocketnet_test_xdp_capture_end( saved_level );

    rocketnet_check( server == NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "refused: xdp" ) != NULL );

    rocketnet_server_socket_clear_preflight_test_result();
#endif // ROCKETNET_DEV

    if ( rocketnet_test_xdp_veth_runs_here() )
    {
        // THE ADMITTED GUEST LEG: the natural available answer runs the real
        // production create on the guest's veth pair, and the requested xdp
        // backend is the granted one. A guest that is not provisioned fails
        // this leg by name rather than standing down.

        rocketnet_test_xdp_natural_guest( "test_xdp_transport_server_io_pin" );
    }
    else if ( naturally_provisioned )
    {
        // a provisioned host that is not the admitted guest: the production
        // pin still runs and is refused by the barrier

        enum rocketnet_log_level_t natural_saved_level = rocketnet_test_xdp_capture_begin();
        struct rocketnet_server_transport_t * natural_server = rocketnet_test_server_transport_create_on_free_port( &config );
        rocketnet_test_xdp_capture_end( natural_saved_level );

        rocketnet_check( natural_server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "refused: xdp" ) != NULL );
    }
    else
    {
        // the natural unavailable answer drives the production pin, with
        // the barrier lifted so the refusal is the pre-flight's own

        rocketnet_server_socket_allow_commit();
        enum rocketnet_log_level_t natural_saved_level = rocketnet_test_xdp_capture_begin();
        struct rocketnet_server_transport_t * natural_server = rocketnet_test_server_transport_create_on_free_port( &config );
        rocketnet_test_xdp_capture_end( natural_saved_level );
        rocketnet_server_socket_forbid_commit();

        rocketnet_check( natural_server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "xdp pre-flight failed: this machine is not provisioned for the XDP data plane" ) != NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "refused: xdp" ) != NULL );
    }

#if ROCKETNET_DEV
    rocketnet_server_socket_clear_preflight_test_result();
#endif
    rocketnet_check( rocketnet_server_socket_preflight( real_port_address, 7 ) == naturally_provisioned );
#else
    enum rocketnet_log_level_t saved_level = rocketnet_test_xdp_capture_begin();
    struct rocketnet_server_transport_t * server = rocketnet_test_server_transport_create_on_free_port( &config );
    rocketnet_test_xdp_capture_end( saved_level );

    rocketnet_check( server == NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "XDP is compiled out of this build" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "refused: xdp" ) != NULL );
#endif

    rocketnet_test_env_restore( &transport_guard );
}

void rocketnet_test_server_transport_io_ladder_fall()
{
    // The fall runs the production ladder in both pre-flight answers, not
    // as a stand-down. With the pre-flight unavailable the xdp rung refuses
    // and the next rung -- gso -- grants; with it available and the commit
    // barrier installed the xdp rung refuses at the barrier and the same
    // fall grants gso. The natural available answer on the admitted guest
    // is the documented port-zero result: the rule refuses xdp before its
    // create and gso discovers the granted port. No branch attaches a data
    // plane outside the committed guest cases.

    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    char real_port_address[64];
    rocketnet_test_free_loopback_address( real_port_address, sizeof(real_port_address) );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    config.max_clients = 1;
    snprintf( config.transport, sizeof(config.transport), "xdp,gso,portable" );

#if ROCKETNET_XDP
#if ROCKETNET_DEV
    rocketnet_server_socket_clear_preflight_test_result();
#endif

    const bool naturally_provisioned = rocketnet_server_socket_preflight( real_port_address, 7 );

#if ROCKETNET_DEV
    // FORCED UNAVAILABLE: xdp refuses at its own pre-flight, the ladder
    // falls to gso on a real port, and both the refusal and the grant are
    // named.

    rocketnet_server_socket_set_preflight_test_result( 0 );
    rocketnet_check( !rocketnet_server_socket_preflight( real_port_address, 7 ) );

    rocketnet_server_socket_allow_commit();
    enum rocketnet_log_level_t saved_level = rocketnet_test_xdp_capture_begin();
    struct rocketnet_server_transport_t * server = rocketnet_test_server_transport_create_on_free_port( &config );
    rocketnet_test_xdp_capture_end( saved_level );
    rocketnet_server_socket_forbid_commit();

    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "gso" ) == 0 );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
    rocketnet_check( strstr( rocketnet_test_captured_log, "xdp pre-flight failed: this machine is not provisioned for the XDP data plane" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "server transport ladder granted gso after refusals: xdp" ) != NULL );
    rocketnet_test_xdp_server_destroy( server );

    // FORCED AVAILABLE: the direct answer is yes, the barrier stays
    // installed, xdp refuses at the barrier, and the ladder still falls to
    // gso rather than portable.

    rocketnet_server_socket_set_preflight_test_result( 1 );
    rocketnet_check( rocketnet_server_socket_preflight( real_port_address, 7 ) );

    saved_level = rocketnet_test_xdp_capture_begin();
    server = rocketnet_test_server_transport_create_on_free_port( &config );
    rocketnet_test_xdp_capture_end( saved_level );

    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "gso" ) == 0 );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
    rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );
    rocketnet_check( strstr( rocketnet_test_captured_log, "server transport ladder granted gso after refusals: xdp" ) != NULL );
    rocketnet_test_xdp_server_destroy( server );

    rocketnet_server_socket_clear_preflight_test_result();
#endif // ROCKETNET_DEV

    if ( rocketnet_test_xdp_veth_runs_here() )
    {
        // THE ADMITTED GUEST LEG: first prove the natural provisioned path
        // grants the requested xdp backend, then the documented port-zero
        // ladder result -- gso, with the port-zero rule naming xdp. A guest
        // that is not provisioned fails the first leg by name, never skips.

        rocketnet_test_xdp_natural_guest( "test_server_transport_io_ladder_fall" );
        rocketnet_test_xdp_ladder_port_zero( &config );
    }
    else if ( naturally_provisioned )
    {
        // the port-zero rule refuses xdp before its create, so the documented
        // result is gso discovering the real port; the rule is named in the
        // log. This is the natural provisioned ladder result.

        rocketnet_test_xdp_ladder_port_zero( &config );
    }
    else
    {
        // the natural unavailable answer: xdp refuses at its pre-flight on a
        // real port and the ladder falls to gso, with the refusal named

        rocketnet_server_socket_allow_commit();
        enum rocketnet_log_level_t natural_saved_level = rocketnet_test_xdp_capture_begin();
        struct rocketnet_server_transport_t * natural_server = rocketnet_test_server_transport_create_on_free_port( &config );
        rocketnet_test_xdp_capture_end( natural_saved_level );
        rocketnet_server_socket_forbid_commit();

        rocketnet_check( natural_server );
        rocketnet_check( strcmp( rocketnet_server_transport_io_name( natural_server ), "gso" ) == 0 );
        rocketnet_check( rocketnet_server_transport_port( natural_server ) != 0 );
        rocketnet_check( strstr( rocketnet_test_captured_log, "xdp pre-flight failed: this machine is not provisioned for the XDP data plane" ) != NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "server transport ladder granted gso after refusals: xdp" ) != NULL );
        rocketnet_test_xdp_server_destroy( natural_server );
    }

#if ROCKETNET_DEV
    rocketnet_server_socket_clear_preflight_test_result();
#endif
    rocketnet_check( rocketnet_server_socket_preflight( real_port_address, 7 ) == naturally_provisioned );
#else
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "gso" ) == 0 );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
#endif

    rocketnet_test_env_restore( &transport_guard );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_server_transport_io_pin_msgx()
{
    rocketnet_test_transport_io_pin_exchange( "msgx", "msgx" );
}


#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#include "rocketnet_driver.h"
#include "../tests/suite_runner.h"

// test_transport_all_skipped_uses_auto, and on Linux
// test_server_transport_io_ladder_fall, create the development config's
// full-capacity server (ROCKETNET_MAX_CLIENTS slots) on "127.0.0.1:0" and
// accept whichever rung the ladder grants, so their footprint is the server
// memory plan, not the suite-wide bound: the per-slot ACK history and the
// transport's owned receive buffers are allocated up front by design. The rungs that can grant a port-zero address
// are the auto-order transports that are not pin-only and that discover
// their bound port (the #109 rule refuses the rest before create); the
// ladder fall's "xdp,gso,portable" is a subset of them. The bound is the
// largest of their plans plus 32 MiB for the process, as the other
// full-capacity cases state.

static long rocketnet_test_port_zero_ladder_rss_bound_mb()
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    uint64_t planned = 0;
    for ( int i = 0; i < num_available; i++ )
    {
        if ( rocketnet_transport_pin_only( available[i] ) || !rocketnet_transport_discovers_bound_port( available[i] ) )
            continue;
        const uint64_t bytes = rocketnet_server_transport_required_memory( &config, rocketnet_transport_name( available[i] ) );
        if ( !bytes )
            return 0; // a grantable rung with no plan keeps the strict suite-wide bound
        if ( bytes > planned )
            planned = bytes;
    }
    const long bound = suite_rss_bound_mb( planned, 32 );
    char verbose[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
        printf( "port_zero_ladder rss_budget planned_bytes=%llu margin_mb=32 bound_mb=%ld\n",
                (unsigned long long) planned, bound );
    return bound;
}

SUITE_CASE_RSS_BOUND( test_transport_all_skipped_uses_auto, rocketnet_test_port_zero_ladder_rss_bound_mb );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
SUITE_CASE_RSS_BOUND( test_server_transport_io_ladder_fall, rocketnet_test_port_zero_ladder_rss_bound_mb );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

void rocketnet_test_transport_all_skipped_uses_auto()
{
    // a list whose every name is a platform- or endpoint-skip names no
    // transport for this endpoint at all -- a fleet-wide line read by an
    // endpoint it was not addressing -- so the endpoint falls to its own
    // auto order, loudly, instead of bricking (#110 review, ruled inside
    // the #106 ignore rule). a typo is still a typo: loud parse failure,
    // no create. refusal-exhausted lists still fail loudly -- the xdp pin
    // test proves that side.

    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.transport, sizeof(config.transport), "msgx" );
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.transport, sizeof(config.transport), "gso" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config);
    rocketnet_check( server );

    // the grant came from the endpoint's own auto order

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    bool granted_from_auto = false;
    for ( int i = 0; i < num_available; i++ )
        if ( strcmp( rocketnet_server_transport_io_name( server ), rocketnet_transport_name( available[i] ) ) == 0 )
            granted_from_auto = true;
    rocketnet_check( granted_from_auto );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    snprintf( config.transport, sizeof(config.transport), "protable" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config) == NULL );

    rocketnet_test_env_restore( &transport_guard );
}

// the transport create takes its address as given. The served address env
// overrides are read at the public create.

void rocketnet_test_config_env_aliases()
{
    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_SERVER_TRANSPORT" );

    struct rocketnet_server_transport_config_t config;

    // the served address is read at the public create
    // (ROCKETNET_SERVER_IPV4_ADDRESS, test_server_address_environment_applies_to_every_server);
    // the transport create takes its address parameter as given

    rocketnet_functional_server_config( &config );
    snprintf( config.transport, sizeof(config.transport), "portable" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "not.an.address", &config) == NULL );

    rocketnet_test_env_restore( &transport_guard );
}

void rocketnet_test_client_transport_io()
{
    struct rocketnet_test_env_guard_t transport_guard;
    rocketnet_test_env_clear( &transport_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( "127.0.0.1:41040", rocketnet_random_uint64(), rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    // the portable pin grants everywhere and reports itself

    struct rocketnet_client_transport_config_t config;
    rocketnet_functional_client_default_config( &config );
    rocketnet_check( strcmp( config.transport, "auto" ) == 0 );

    snprintf( config.transport, sizeof(config.transport), "portable" );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), "portable" ) == 0 );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the msgx pin grants on macOS and reports itself

    snprintf( config.transport, sizeof(config.transport), "msgx" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), "msgx" ) == 0 );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the staged linux transports are not client transports: a list naming
    // only them is an all-skipped list for the client endpoint, which
    // falls to the client's auto order rather than bricking every client
    // in a process that shares the fleet's transport list

    snprintf( config.transport, sizeof(config.transport), "gso" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    {
        int num_available = 0;
        const int * available = rocketnet_client_transport_io_availability( &num_available );
        bool granted_from_auto = false;
        for ( int i = 0; i < num_available; i++ )
            if ( strcmp( rocketnet_client_transport_io_name( client ), rocketnet_transport_name( available[i] ) ) == 0 )
                granted_from_auto = true;
        rocketnet_check( granted_from_auto );
    }
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );

    rocketnet_test_env_restore( &transport_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The GSO transport's own tests (#97), linux only, run on every linux build
// whatever transport the transport compiled against -- the transport module is
// always built, so its probe, its wire paths, its degraded tiers and its
// overflow behaviour are covered even when the transport above it is
// portable. Everything here is over-the-wire on loopback through real
// sockets; the client is the portable platform socket, which also proves the
// transport interoperates with a plain UDP endpoint.

#include "rocketnet_server_gso.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#ifndef UDP_GRO
#define UDP_GRO 104
#endif

// does this HOST actually coalesce? the transport's feature bits prove only
// that the setsockopts were ACCEPTED -- with `ethtool -K lo
// tx-udp-segmentation off`, a legitimate configuration, a UDP_SEGMENT
// super-datagram is segmented at transmit and arrives as plain datagrams,
// no UDP_GRO cmsg ever appears, and a counter assertion gated on the
// feature bits alone would encode that host tunable as a protocol fact.
// so probe the host itself, once, with the exact shape the provocation
// phase sends: a scratch socket with UDP_GRO enabled mails itself one
// eight-segment super-datagram over loopback and watches for an arrival
// carrying the UDP_GRO stride cmsg below its length -- the kernel's own
// statement that it delivered coalesced.

static bool rocketnet_test_gso_host_coalesces()
{
    int fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
        return false;

    struct sockaddr_in address;
    memset( &address, 0, sizeof(address) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = 0;
    socklen_t address_length = sizeof(address);
    if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) != 0 ||
         getsockname( fd, (struct sockaddr*) &address, &address_length ) != 0 )
    {
        close( fd );
        return false;
    }

    int one = 1;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_GRO, &one, sizeof(one) ) != 0 )
    {
        close( fd );
        return false;
    }

    const int probe_segments = 8;
    const int probe_segment_bytes = 100;

    uint8_t super[probe_segments * probe_segment_bytes];
    memset( super, 0xAB, sizeof(super) );

    struct iovec iov = { super, sizeof(super) };
    char control[CMSG_SPACE(sizeof(uint16_t))];
    memset( control, 0, sizeof(control) );
    struct msghdr msg;
    memset( &msg, 0, sizeof(msg) );
    msg.msg_name = &address;
    msg.msg_namelen = sizeof(address);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
    uint16_t stride = (uint16_t) probe_segment_bytes;
    memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

    if ( sendmsg( fd, &msg, 0 ) != (ssize_t) sizeof(super) )
    {
        close( fd );
        return false;
    }

    bool coalesced = false;
    int received_bytes = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 && received_bytes < (int) sizeof(super) && !coalesced )
    {
        uint8_t buffer[sizeof(super)];
        struct iovec receive_iov = { buffer, sizeof(buffer) };
        char receive_control[CMSG_SPACE(sizeof(int))];
        struct msghdr receive_msg;
        memset( &receive_msg, 0, sizeof(receive_msg) );
        receive_msg.msg_iov = &receive_iov;
        receive_msg.msg_iovlen = 1;
        receive_msg.msg_control = receive_control;
        receive_msg.msg_controllen = sizeof(receive_control);
        ssize_t bytes = recvmsg( fd, &receive_msg, MSG_DONTWAIT );
        if ( bytes <= 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        received_bytes += (int) bytes;
        for ( struct cmsghdr * c = CMSG_FIRSTHDR( &receive_msg ); c; c = CMSG_NXTHDR( &receive_msg, c ) )
        {
            if ( c->cmsg_level == IPPROTO_UDP && c->cmsg_type == UDP_GRO )
            {
                int segment_bytes = 0;
                memcpy( &segment_bytes, CMSG_DATA( c ), sizeof(int) );
                if ( segment_bytes > 0 && segment_bytes < (int) bytes )
                    coalesced = true;
            }
        }
    }

    close( fd );
    return coalesced;
}

void rocketnet_test_server_gso_probe()
{
    // create discovers an ephemeral port and reports it back; the queue
    // count and the reuseport group are facts, the segmentation features
    // are probes whose result belongs to the kernel

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );
    rocketnet_check( bind_address.port == 0 );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, 0 );
    rocketnet_check( gso );
    rocketnet_check( bind_address.port != 0 );
    rocketnet_check( rocketnet_server_gso_num_queues( gso ) == 2 );
    rocketnet_check( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT );

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] == 0 );

    rn_native_backend_stop_and_destroy( gso, rocketnet_server_gso_stop, rocketnet_server_gso_stopped, rocketnet_server_gso_destroy );

    // the disable flags force the degraded tiers, so the fallback paths are
    // testable on kernels where the probe would say yes

    rocketnet_address_t plain_address;
    rocketnet_check( rocketnet_address_parse( &plain_address, "127.0.0.1" ) );
    struct rocketnet_server_gso_t * plain = rocketnet_server_gso_create( NULL, &plain_address, 1,
                                                                         ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO );
    rocketnet_check( plain );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) == 0 );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) == 0 );
    rn_native_backend_stop_and_destroy( plain, rocketnet_server_gso_stop, rocketnet_server_gso_stopped, rocketnet_server_gso_destroy );
}

// the wire exchange, shared by the tiers and the families: a plain UDP client
// sends tagged packets of every size the wire allows into the transport, the
// transport's process batch must surface each byte-exact with the right
// source, then the transport sends tagged packets back -- deliberately
// including equal-size same-destination runs, the shape GSO coalesces --
// and the client must receive each byte-exact.

// The GSO transport's transmit worker sends published banks off the caller
// thread and counts them as it goes. A check of the send counters first waits
// for the counter to reach its value; each retry may also publish a producer
// that a contended try retained.
static void rocketnet_test_gso_wait_sends( struct rocketnet_server_gso_t * gso, int counter, uint64_t value )
{
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    const double start = rocketnet_platform_time();
    for ( ;; )
    {
        rocketnet_server_gso_send_packets( gso );
        rocketnet_server_gso_counters( gso, counters );
        if ( counters[counter] >= value || rocketnet_platform_time() - start >= 5.0 )
            return;
        rocketnet_platform_sleep( 0.001 );
    }
}

static void rocketnet_test_gso_wait_sent_or_dropped( struct rocketnet_server_gso_t * gso, uint64_t value )
{
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    const double start = rocketnet_platform_time();
    for ( ;; )
    {
        rocketnet_server_gso_send_packets( gso );
        rocketnet_server_gso_counters( gso, counters );
        if ( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] + counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] >= value ||
             rocketnet_platform_time() - start >= 5.0 )
            return;
        rocketnet_platform_sleep( 0.001 );
    }
}

static void rocketnet_test_server_gso_wire_exchange( const char * /* test_name */, const char * bind_string, const char * client_string, const char * server_host, int flags, int expect_ipv4_from )
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, bind_string ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, flags );
    rocketnet_check( gso );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, client_string ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    rocketnet_check( client_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, server_host ) );
    server_address.port = bind_address.port;

    #define ROCKETNET_GSO_TEST_PACKETS 200

    // client -> transport

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            packet[j] = (uint8_t) ( i * 31 + j );
        rocketnet_platform_socket_send_packet( client_socket, &server_address, packet, packet_bytes );
    }

    bool seen[ROCKETNET_GSO_TEST_PACKETS];
    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_server_gso_receive_packets( gso );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            const int expected_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
            rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
            rocketnet_check( !seen[tag] );
            rocketnet_check( received->packet_bytes[i] == expected_bytes );
            for ( int j = 1; j < expected_bytes; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 31 + j ) );

            // the source is the client's real port, and a v4 client arriving on
            // a dual stack listen is native ipv4 -- normalized exactly once
            // at the boundary, per the standard's law

            rocketnet_check( received->from[i].port == client_address.port );
            if ( expect_ipv4_from )
                rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 );

            seen[tag] = true;
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // transport -> client. the first half all one size -- adjacent equal-size
    // same-destination slots, the runs the GSO tier folds into
    // super-datagrams -- the second half cycling sizes

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = i < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                               : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &client_address, &packet_id );
        rocketnet_check( frame );
        uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
        wire[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            wire[j] = (uint8_t) ( i * 47 + j );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, packet_bytes );
    }

    rocketnet_server_gso_send_packets( gso );

    memset( seen, 0, sizeof(seen) );
    num_seen = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        const int tag = packet[0];
        const int expected_bytes = tag < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                                 : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
        rocketnet_check( !seen[tag] );
        rocketnet_check( packet_bytes == expected_bytes );
        for ( int j = 1; j < expected_bytes; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
        seen[tag] = true;
        num_seen++;
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // on the GSO tier those equal-size runs must actually have coalesced:
    // the counter is the proof the fast path ran, not just that packets
    // arrived

    rocketnet_test_gso_wait_sends( gso, ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT, ROCKETNET_GSO_TEST_PACKETS );
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == ROCKETNET_GSO_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] >= ROCKETNET_GSO_TEST_PACKETS );
    if ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT )
        rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] > 0 );

    // a runt and an oversize packet die at the transport's size pass -- one
    // below the smallest packet a server reads, one above the receive
    // ceiling, the pair covering both bounds of the check that guards the
    // staging memcpy -- counted, never surfaced

    const uint64_t dropped_before = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
    uint8_t runt[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1];
    memset( runt, 0xEE, sizeof(runt) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, runt, (int) sizeof(runt) );
    uint8_t oversize[ROCKETNET_RECEIVE_CEILING + 74];
    memset( oversize, 0xEF, sizeof(oversize) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, oversize, (int) sizeof(oversize) );

    start = rocketnet_platform_time();
    uint64_t dropped_after = dropped_before;
    while ( rocketnet_platform_time() - start < 5.0 && dropped_after < dropped_before + 2 )
    {
        rocketnet_server_gso_receive_packets( gso );
        rocketnet_check( rocketnet_server_gso_process_packets( gso )->num_packets == 0 );
        rocketnet_server_gso_counters( gso, counters );
        dropped_after = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( dropped_after == dropped_before + 2 );

    // provoke GRO for real: the client sends one UDP_SEGMENT super-datagram,
    // which loopback segments and the transport's UDP_GRO socket receives
    // coalesced -- the multi-segment split path runs, proven by the
    // GRO_BATCHES counter, and every segment surfaces byte-exact. the
    // suite's ordinary traffic never repeats a size back to back, so
    // without this phase the split loop would never execute under test.
    // ipv4 clients only (the sockaddr is built inline), which covers every
    // tier this helper runs.

    if ( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) &&
         ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) &&
         server_address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const bool host_coalesces = rocketnet_test_gso_host_coalesces();

        const int gro_segments = 8;
        const int gro_segment_bytes = 100;

        uint8_t super[gro_segments * gro_segment_bytes];
        for ( int i = 0; i < gro_segments; i++ )
        {
            super[i * gro_segment_bytes] = (uint8_t) ( 200 + i );
            for ( int j = 1; j < gro_segment_bytes; j++ )
                super[i * gro_segment_bytes + j] = (uint8_t) ( ( 200 + i ) * 13 + j );
        }

        struct sockaddr_in to;
        rocketnet_platform_address_to_sockaddr_in( &server_address, &to );

        struct iovec iov = { super, sizeof(super) };
        char control[CMSG_SPACE(sizeof(uint16_t))];
        memset( control, 0, sizeof(control) );
        struct msghdr msg;
        memset( &msg, 0, sizeof(msg) );
        msg.msg_name = &to;
        msg.msg_namelen = sizeof(to);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
        uint16_t stride = (uint16_t) gro_segment_bytes;
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

        rocketnet_check( sendmsg( client_socket->handle, &msg, 0 ) == (ssize_t) sizeof(super) );

        rocketnet_server_gso_counters( gso, counters );
        const uint64_t gro_before = counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES];

        bool segment_seen[gro_segments];
        memset( segment_seen, 0, sizeof(segment_seen) );
        int num_segments_seen = 0;

        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 && num_segments_seen < gro_segments )
        {
            rocketnet_server_gso_receive_packets( gso );
            struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
            for ( int i = 0; i < received->num_packets; i++ )
            {
                const int tag = received->packet_data[i][0] - 200;
                rocketnet_check( tag >= 0 && tag < gro_segments );
                rocketnet_check( !segment_seen[tag] );
                rocketnet_check( received->packet_bytes[i] == gro_segment_bytes );
                for ( int j = 1; j < gro_segment_bytes; j++ )
                    rocketnet_check( received->packet_data[i][j] == (uint8_t) ( ( 200 + tag ) * 13 + j ) );
                rocketnet_check( received->from[i].port == client_address.port );
                segment_seen[tag] = true;
                num_segments_seen++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( num_segments_seen == gro_segments );

        // the counter is the proof the coalesced path ran -- eight distinct
        // arrivals alone would also be produced by uncoalesced delivery. it
        // is asserted only where the host probe observed real coalescing:
        // the feature bits mean the setsockopts were accepted, and a host
        // with tx-udp-segmentation off on loopback delivers this same send
        // uncoalesced -- legitimate configuration, not a transport failure. on
        // such a host the byte-exact segment coverage above still ran, and
        // the skip says so out loud rather than passing silently.

        if ( host_coalesces )
        {
            rocketnet_server_gso_counters( gso, counters );
            rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES] > gro_before );
        }
    }

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( gso, rocketnet_server_gso_stop, rocketnet_server_gso_stopped, rocketnet_server_gso_destroy );
}

void rocketnet_test_server_gso_wire_ipv4()
{
    rocketnet_test_server_gso_wire_exchange( "test_gso_transport_server_wire_ipv4", "127.0.0.1", "127.0.0.1", "127.0.0.1", 0, 1 );
}

void rocketnet_test_server_gso_wire_plain()
{
    // the degraded tier over the same wire: no UDP_SEGMENT, no UDP_GRO,
    // plain sendmmsg/recvmmsg batching. the fallback path is never the
    // untested path.

    rocketnet_test_server_gso_wire_exchange( "test_gso_transport_server_wire_plain", "127.0.0.1", "127.0.0.1", "127.0.0.1", ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO, 1 );
}

void rocketnet_test_server_gso_wire_ipv6()
{
    rocketnet_test_server_gso_wire_exchange( "test_gso_transport_server_wire_ipv6", "[::1]", "[::1]", "[::1]", 0, 0 );
}

void rocketnet_test_server_gso_wire_dual_stack()
{
    // an ipv6 wildcard listen accepts an ipv4 client, whose source surfaces
    // as native ipv4 -- the v4-mapped normalization the standard makes law

    // wildcard bind kept: the case is the ipv6 wildcard listen serving an ipv4 client
    rocketnet_test_server_gso_wire_exchange( "test_gso_transport_server_wire_dual_stack", "[::]", "127.0.0.1", "127.0.0.1", 0, 1 );
}

// The gso client over the wire (sdk/rocketnet_client_gso_linux.cpp): the
// client pinned to gso and granted it, a portable server, equal-sized
// payloads both ways so every client send batch is runs that leave as
// UDP_SEGMENT messages (the segmented-message count proves the path ran), and
// every payload byte-exact, none lost, none twice. segment false is the plain
// tier: both probes are skipped, no message is segmented and receive is the
// platform's recvmmsg. server_transport "gso" sends UDP_SEGMENT runs, which
// loopback delivers whole to the client's UDP_GRO socket, so the client's GRO
// batch count proves the receive split ran.

#include "rocketnet_client_gso.h"

#if ROCKETNET_DEV
static void rocketnet_test_gso_client_wire_exchange( const char * server_bind, const char * server_host, bool segment, const char * server_transport = "portable" )
{
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof(server_config.transport), "%s", server_transport );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, server_bind, &server_config );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), server_transport ) == 0 );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "%s:%d", server_host, rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "gso" );

    rocketnet_client_gso_test_disable_segment.store( !segment );
    rocketnet_client_gso_test_disable_gro.store( !segment );
    const uint64_t segment_messages_before = rocketnet_client_gso_test_segment_messages_sent.load();
    const uint64_t gro_batches_before = rocketnet_client_gso_test_gro_batches.load();

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( client ), "gso" ) == 0 );
    rocketnet_check( rocketnet_client_transport_client_io( client ) != NULL );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    const int FRAMES = 40;
    const int PACKETS_PER_FRAME = 32;
    const int PAYLOAD_BYTES = 300;
    const int TOTAL = FRAMES * PACKETS_PER_FRAME;

    static uint8_t server_seen[FRAMES * PACKETS_PER_FRAME];
    static uint8_t client_seen[FRAMES * PACKETS_PER_FRAME];
    memset( server_seen, 0, sizeof(server_seen) );
    memset( client_seen, 0, sizeof(client_seen) );
    int server_received = 0;
    int client_received = 0;

    int client_sent = 0;
    int server_sent = 0;
    start = rocketnet_platform_time();
    while ( ( server_received < TOTAL || client_received < TOTAL ) && rocketnet_platform_time() - start < 10.0 )
    {
        // a frame's worth each way, a tag in the first two bytes and a
        // pattern of the tag after it; a refused start (a full ring) leaves
        // the rest of the frame for the next pass, so no tag is lost or sent twice
        for ( int i = 0; i < PACKETS_PER_FRAME && client_sent < TOTAL; i++ )
        {
            const int tag = client_sent;
            struct rocketnet_packet_context_t context = {};
            uint8_t * payload = rocketnet_client_transport_start_packet( client, PAYLOAD_BYTES, &context );
            if ( !payload )
                break;
            payload[context.payload_index + 0] = (uint8_t) ( tag & 0xFF );
            payload[context.payload_index + 1] = (uint8_t) ( tag >> 8 );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                payload[context.payload_index + j] = (uint8_t) ( tag * 31 + j );
            rocketnet_check( rocketnet_client_transport_finish_packet( client, &context, PAYLOAD_BYTES ) > PAYLOAD_BYTES );
            client_sent++;
        }

        for ( int i = 0; i < PACKETS_PER_FRAME && server_sent < TOTAL; i++ )
        {
            const int tag = server_sent;
            struct rocketnet_packet_context_t context;
            uint8_t * packet = rocketnet_server_transport_start_packet( server, 0, PAYLOAD_BYTES, &context );
            if ( !packet )
                break;
            packet[context.payload_index + 0] = (uint8_t) ( tag & 0xFF );
            packet[context.payload_index + 1] = (uint8_t) ( tag >> 8 );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                packet[context.payload_index + j] = (uint8_t) ( tag * 17 + j );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, PAYLOAD_BYTES ) > 0 );
            server_sent++;
        }

        rocketnet_client_transport_send_packets( client );
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == PAYLOAD_BYTES );
            const int tag = payload[0] | ( payload[1] << 8 );
            rocketnet_check( tag >= 0 && tag < TOTAL );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                rocketnet_check( payload[j] == (uint8_t) ( tag * 31 + j ) );
            rocketnet_check( !server_seen[tag] );
            server_seen[tag] = 1;
            server_received++;
        }

        static uint8_t received[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int received_bytes;
        while ( ( received_bytes = rocketnet_client_transport_receive_packet( client, received, (int) sizeof(received) ) ) > 0 )
        {
            rocketnet_check( received_bytes == PAYLOAD_BYTES );
            const int tag = received[0] | ( received[1] << 8 );
            rocketnet_check( tag >= 0 && tag < TOTAL );
            for ( int j = 2; j < PAYLOAD_BYTES; j++ )
                rocketnet_check( received[j] == (uint8_t) ( tag * 17 + j ) );
            rocketnet_check( !client_seen[tag] );
            client_seen[tag] = 1;
            client_received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( server_received == TOTAL );
    rocketnet_check( client_received == TOTAL );

    const uint64_t segment_messages = rocketnet_client_gso_test_segment_messages_sent.load() - segment_messages_before;
    if ( segment )
        rocketnet_check( segment_messages > 0 );
    else
        rocketnet_check( segment_messages == 0 );

    const uint64_t gro_batches = rocketnet_client_gso_test_gro_batches.load() - gro_batches_before;
    if ( strcmp( server_transport, "gso" ) == 0 && segment )
        rocketnet_check( gro_batches > 0 );
    if ( !segment )
        rocketnet_check( gro_batches == 0 );

    rocketnet_client_gso_test_disable_segment.store( false );
    rocketnet_client_gso_test_disable_gro.store( false );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_gso_client_wire_ipv4()
{
    rocketnet_test_gso_client_wire_exchange( "127.0.0.1", "127.0.0.1", true );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_gso_client_wire_ipv6()
{
    rocketnet_test_gso_client_wire_exchange( "[::1]", "[::1]", true );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_gso_client_wire_plain()
{
    // the plain tier over the same wire: no UDP_SEGMENT, one datagram per
    // sendmmsg message. the fallback tier is never the untested tier.
    rocketnet_test_gso_client_wire_exchange( "127.0.0.1", "127.0.0.1", false );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_gso_client_wire_gro()
{
    // the gso server sends each frame's equal-sized packets to the one client
    // as UDP_SEGMENT runs: the client receives them coalesced and splits them
    rocketnet_test_gso_client_wire_exchange( "127.0.0.1", "127.0.0.1", true, "gso" );
}
#endif // #if ROCKETNET_DEV

void rocketnet_test_server_gso_send_overflow()
{
    // fill the send staging past capacity: starts beyond it refuse with
    // NULL, the flush sends what fit, and the next phase is whole -- the
    // transport-level face of the transport's ring exhaustion property

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 1, 0 );
    rocketnet_check( gso );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );

    for ( int i = 0; i < ROCKETNET_GSO_SEND_QUEUE_SIZE; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &client_address, &packet_id );
        rocketnet_check( frame );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, 64 );
    }

    uint64_t overflow_id = 0;
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &client_address, &overflow_id ) == NULL );
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &client_address, &overflow_id ) == NULL );

    rocketnet_server_gso_send_packets( gso );

    // every staged packet was either sent or dropped, and nothing vanished
    // uncounted

    rocketnet_test_gso_wait_sent_or_dropped( gso, ROCKETNET_GSO_SEND_QUEUE_SIZE );
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] + counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == ROCKETNET_GSO_SEND_QUEUE_SIZE );

    // the next phase is whole: a fresh tagged packet flushes and arrives

    uint8_t tag[64];
    memset( tag, 0xAB, sizeof(tag) );

    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_server_gso_start_packet( gso, &client_address, &packet_id );
    rocketnet_check( frame );
    memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, tag, sizeof(tag) );
    rocketnet_server_gso_finish_packet( gso, packet_id, frame, (int) sizeof(tag) );
    rocketnet_server_gso_send_packets( gso );

    bool recovered = false;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        if ( packet_bytes == (int) sizeof(tag) && memcmp( packet, tag, sizeof(tag) ) == 0 )
            recovered = true;
    }
    rocketnet_check( recovered );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( gso, rocketnet_server_gso_stop, rocketnet_server_gso_stopped, rocketnet_server_gso_destroy );
}


#if ROCKETNET_DEV
void rocketnet_test_server_gso_shutdown_send_interruptions()
{
    rocketnet_address_t bind, to;
    rocketnet_check( rocketnet_address_parse( &bind, "127.0.0.1" ) );
    rocketnet_check( rocketnet_address_parse( &to, "127.0.0.1:40000" ) );
    rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind, 1, ROCKETNET_SERVER_GSO_DISABLE_SEGMENT );
    rocketnet_check( gso );
    uint64_t id = 0;
    uint8_t * frame = rocketnet_server_gso_start_packet( gso, &to, &id );
    rocketnet_check( frame );
    memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, 0x55, 64 );
    rocketnet_server_gso_finish_packet( gso, id, frame, 64 );
    rocketnet_server_gso_test_fail_sends( gso, 1024, EINTR );
    rocketnet_server_gso_send_packets( gso );
    rocketnet_test_gso_wait_sends( gso, ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED, 1 );
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS] == 1 );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 1 );
    rocketnet_server_gso_stop( gso );
    const double deadline = rocketnet_time() + 5.0;
    while ( !rocketnet_server_gso_stopped( gso ) && rocketnet_time() < deadline ) rocketnet_sleep( 0.001 );
    rocketnet_check( rocketnet_server_gso_stopped( gso ) );
    rocketnet_server_gso_destroy( gso );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_server_gso_held_receive_view()
{
    rocketnet_address_t bind, client_address;
    rocketnet_check( rocketnet_address_parse( &bind, "127.0.0.1" ) );
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1" ) );
    rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind, 1,
        ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO );
    rocketnet_check( gso );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE,
        ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "127.0.0.1" ) );
    server_address.port = bind.port;
    uint8_t first[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES];
    uint8_t second[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES];
    memset( first, 0xA5, sizeof(first) );
    memset( second, 0x5A, sizeof(second) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, first, sizeof(first) );

    uint8_t * held_view = NULL;
    const double first_deadline = rocketnet_time() + 5.0;
    while ( !held_view && rocketnet_time() < first_deadline )
    {
        rocketnet_server_gso_receive_packets( gso );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
        if ( received->num_packets )
        {
            rocketnet_check( received->num_packets == 1 );
            rocketnet_check( received->packet_bytes[0] == (int) sizeof(first) );
            held_view = received->packet_data[0];
            rocketnet_check( memcmp( held_view, first, sizeof(first) ) == 0 );
        }
        else
            rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( held_view );

    // The worker must publish a second packet without touching the main-held
    // first view. This condition poll observes ownership, not elapsed sleep.
    rocketnet_platform_socket_send_packet( client_socket, &server_address, second, sizeof(second) );
    const double ready_deadline = rocketnet_time() + 5.0;
    while ( !rocketnet_server_gso_test_receive_ready( gso, 0 ) && rocketnet_time() < ready_deadline )
        rocketnet_platform_sleep( 0.001 );
    rocketnet_check( rocketnet_server_gso_test_receive_ready( gso, 0 ) );
    rocketnet_check( memcmp( held_view, first, sizeof(first) ) == 0 );

    rocketnet_server_gso_receive_packets( gso );
    struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
    rocketnet_check( received->num_packets == 1 );
    rocketnet_check( received->packet_bytes[0] == (int) sizeof(second) );
    rocketnet_check( memcmp( received->packet_data[0], second, sizeof(second) ) == 0 );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( gso, rocketnet_server_gso_stop,
        rocketnet_server_gso_stopped, rocketnet_server_gso_destroy );
}
#endif // #if ROCKETNET_DEV

// The io_uring transport's own tests, linux only, in the gso section's shape:
// over the wire on loopback through real sockets and a real io_uring, the
// client the portable platform socket. They are in the suite only where the
// transport is available (rocketnet_server_uring_available).

#include "rocketnet_server_uring.h"

#if ROCKETNET_DEV
#if defined( __GNUC__ ) || defined( __clang__ )
__attribute__((noinline))
#endif
static struct rocketnet_server_uring_t * rocketnet_test_server_uring_create_stalled( rocketnet_address_t * address )
{
    return rocketnet_server_uring_create( NULL, address, 2 );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_server_uring_probe()
{
    // create discovers an ephemeral port and reports it back; the queue
    // count is a fact and every counter starts at zero

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );
    rocketnet_check( bind_address.port == 0 );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_create( NULL, &bind_address, 2 );
    rocketnet_check( uring );
    rocketnet_check( bind_address.port != 0 );
    rocketnet_check( rocketnet_server_uring_num_queues( uring ) == 2 );

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    for ( int i = 0; i < ROCKETNET_SERVER_URING_NUM_COUNTERS; i++ )
    {
        if ( i == ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS )
            continue;                               // create submits the multishot receives
        rocketnet_check( counters[i] == 0 );
    }

    // destroy cancels the multishots and drains: a second transport binds the
    // same port afterwards, so the sockets are gone

    const uint16_t port = bind_address.port;
    rn_native_backend_stop_and_destroy( uring, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );

    rocketnet_address_t again_address;
    rocketnet_check( rocketnet_address_parse( &again_address, "127.0.0.1" ) );
    again_address.port = port;
    struct rocketnet_server_uring_t * again = rocketnet_server_uring_create( NULL, &again_address, 0 );
    rocketnet_check( again );
    rocketnet_check( again_address.port == port );
    rocketnet_check( rocketnet_server_uring_num_queues( again ) == ROCKETNET_URING_DEFAULT_QUEUES );
    rn_native_backend_stop_and_destroy( again, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );

    // a queue count past the geometry refuses, never clamps

    rocketnet_address_t over_address;
    rocketnet_check( rocketnet_address_parse( &over_address, "127.0.0.1" ) );
    rocketnet_check( rocketnet_server_uring_create( NULL, &over_address, ROCKETNET_URING_MAX_QUEUES + 1 ) == NULL );

    // A stalled completion leaves the caller-owned transport pending. It
    // never turns a deadline into permission to abandon or free its storage.
    rocketnet_address_t stall_address;
    rocketnet_check( rocketnet_address_parse( &stall_address, "127.0.0.1" ) );
    struct rocketnet_server_uring_t * stalled = rocketnet_test_server_uring_create_stalled( &stall_address );
    rocketnet_check( stalled );
    const uint16_t stall_port = stall_address.port;
    const uint64_t leaked_bytes_before = rocketnet_server_uring_leaked_bytes();
    const uint64_t leaked_transports_before = rocketnet_server_uring_leaked_transports();
    rocketnet_server_uring_test_stall_destroy( stalled );
    rocketnet_server_uring_stop( stalled );
    for ( int poll = 0; poll < 1024; ++poll )
        rocketnet_check( !rocketnet_server_uring_stopped( stalled ) );
    rocketnet_check( rocketnet_server_uring_num_queues( stalled ) == 2 );
    rocketnet_check( rocketnet_server_uring_leaked_bytes() == leaked_bytes_before );
    rocketnet_check( rocketnet_server_uring_leaked_transports() == leaked_transports_before );
    rocketnet_server_uring_test_resume_stop( stalled );
    const double stop_deadline = rocketnet_time() + 5.0;
    while ( !rocketnet_server_uring_stopped( stalled ) && rocketnet_time() < stop_deadline )
        rocketnet_sleep( 0.001 );
    rocketnet_check( rocketnet_server_uring_stopped( stalled ) );
    rocketnet_server_uring_destroy( stalled );

    rocketnet_address_t rebind_address;
    rocketnet_check( rocketnet_address_parse( &rebind_address, "127.0.0.1" ) );
    rebind_address.port = stall_port;
    struct rocketnet_server_uring_t * rebound = rocketnet_server_uring_create( NULL, &rebind_address, 2 );
    rocketnet_check( rebound );
    rocketnet_server_uring_stop( rebound );
    const double rebound_deadline = rocketnet_time() + 5.0;
    while ( !rocketnet_server_uring_stopped( rebound ) && rocketnet_time() < rebound_deadline )
        rocketnet_sleep( 0.001 );
    rocketnet_check( rocketnet_server_uring_stopped( rebound ) );
    rocketnet_server_uring_destroy( rebound );
    rocketnet_check( rocketnet_server_uring_leaked_transports() == leaked_transports_before );
}
#endif // #if ROCKETNET_DEV

// the wire exchange, the gso exchange's shape for this transport: a plain UDP
// client sends tagged packets of every size the wire allows into the transport,
// each must surface byte-exact with the right source; then the transport sends
// tagged packets back, including equal-size same-destination runs, and the
// client must receive each byte-exact

// expect_segment: 1, the transport must use UDP_SEGMENT and send at least
// one segmented SENDMSG; 0, it must send none (created with
// ROCKETNET_SERVER_URING_DISABLE_SEGMENT); -1, whatever this kernel's probe
// says

static void rocketnet_test_server_uring_wire_exchange_flags( const char * bind_string, const char * client_string, const char * server_host, int expect_ipv4_from,
                                                             int create_flags, int expect_segment )
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, bind_string ) );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_create_flags( NULL, &bind_address, 2, create_flags );
    rocketnet_check( uring );
    if ( expect_segment == 1 )
        rocketnet_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) != 0 );
    if ( expect_segment == 0 )
        rocketnet_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) == 0 );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, client_string ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );
    rocketnet_check( client_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, server_host ) );
    server_address.port = bind_address.port;

    #define ROCKETNET_URING_TEST_PACKETS 200

    // client -> transport

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < ROCKETNET_URING_TEST_PACKETS; i++ )
    {
        const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            packet[j] = (uint8_t) ( i * 31 + j );
        rocketnet_platform_socket_send_packet( client_socket, &server_address, packet, packet_bytes );
    }

    bool seen[ROCKETNET_URING_TEST_PACKETS];
    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_URING_TEST_PACKETS )
    {
        rocketnet_server_uring_receive_packets( uring );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_uring_process_packets( uring );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            const int expected_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
            rocketnet_check( tag < ROCKETNET_URING_TEST_PACKETS );
            rocketnet_check( !seen[tag] );
            rocketnet_check( received->packet_bytes[i] == expected_bytes );
            for ( int j = 1; j < expected_bytes; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 31 + j ) );
            if ( expect_ipv4_from )
                rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 );
            else
                rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV6 );
            rocketnet_check( received->from[i].port == client_address.port );
            seen[tag] = true;
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == ROCKETNET_URING_TEST_PACKETS );

    // transport -> client: the first half all one size (adjacent equal-size
    // same-destination slots), the second half cycling sizes, with one
    // aborted slot in the middle that must never reach the wire

    int num_staged = 0;
    for ( int i = 0; i < ROCKETNET_URING_TEST_PACKETS; i++ )
    {
        const int packet_bytes = i < ROCKETNET_URING_TEST_PACKETS / 2 ? 400
                               : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &client_address, &packet_id );
        rocketnet_check( frame );
        uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
        wire[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            wire[j] = (uint8_t) ( i * 47 + j );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, packet_bytes );
        num_staged++;

        if ( i == ROCKETNET_URING_TEST_PACKETS / 2 )
        {
            uint64_t aborted_id = 0;
            uint8_t * aborted = rocketnet_server_uring_start_packet( uring, &client_address, &aborted_id );
            rocketnet_check( aborted );
            aborted[ROCKETNET_FRAME_WIRE_OFFSET] = 0xFF;
            rocketnet_server_uring_abort_packet( uring, aborted_id, aborted );
        }
    }
    rocketnet_check( num_staged == ROCKETNET_URING_TEST_PACKETS );

    rocketnet_server_uring_send_packets( uring );

    // send_packets reaps within its fixed completion budget
    // (ROCKETNET_URING_SEND_BUDGET) and returns even if sends remain, deferred
    // to a later call and counted SEND_DEFERRED; this batch is small enough
    // that every send completes before the call returns

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == ROCKETNET_URING_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == 0 );

    memset( seen, 0, sizeof(seen) );
    num_seen = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_URING_TEST_PACKETS )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        const int tag = packet[0];
        const int expected_bytes = tag < ROCKETNET_URING_TEST_PACKETS / 2 ? 400
                                 : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        rocketnet_check( tag < ROCKETNET_URING_TEST_PACKETS );
        rocketnet_check( !seen[tag] );
        rocketnet_check( packet_bytes == expected_bytes );
        for ( int j = 1; j < expected_bytes; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
        rocketnet_check( from.port == server_address.port );
        seen[tag] = true;
        num_seen++;
    }
    rocketnet_check( num_seen == ROCKETNET_URING_TEST_PACKETS );

    rocketnet_server_uring_counters( uring, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] == ROCKETNET_URING_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED] == ROCKETNET_URING_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS] > 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_GSO_FALLBACKS] == 0 );
    if ( expect_segment == 1 )
        rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_GSO_SENDS] > 0 );
    if ( expect_segment == 0 )
        rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_GSO_SENDS] == 0 );

    // a runt and an oversize packet die at the transport's size pass, counted,
    // never surfaced

    const uint64_t dropped_before = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE];
    uint8_t runt[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1];
    memset( runt, 0xEE, sizeof(runt) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, runt, (int) sizeof(runt) );
    uint8_t oversize[ROCKETNET_RECEIVE_CEILING + 74];
    memset( oversize, 0xEF, sizeof(oversize) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, oversize, (int) sizeof(oversize) );

    start = rocketnet_platform_time();
    uint64_t dropped_after = dropped_before;
    while ( rocketnet_platform_time() - start < 5.0 && dropped_after < dropped_before + 2 )
    {
        rocketnet_server_uring_receive_packets( uring );
        rocketnet_check( rocketnet_server_uring_process_packets( uring )->num_packets == 0 );
        rocketnet_server_uring_counters( uring, counters );
        dropped_after = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE];
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( dropped_after == dropped_before + 2 );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED] == ROCKETNET_URING_TEST_PACKETS );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( uring, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );
}

static void rocketnet_test_server_uring_wire_exchange( const char * bind_string, const char * client_string, const char * server_host, int expect_ipv4_from )
{
    rocketnet_test_server_uring_wire_exchange_flags( bind_string, client_string, server_host, expect_ipv4_from, 0, -1 );
}

void rocketnet_test_server_uring_wire_ipv4()
{
    rocketnet_test_server_uring_wire_exchange( "127.0.0.1", "127.0.0.1", "127.0.0.1", 1 );
}

// the send runs as segmented SENDMSGs (UDP_SEGMENT): the 400-byte half of the
// exchange is consecutive equal-size slots to one destination, so the
// transport must send it in segmented sends, and the far socket must still
// receive every packet byte-exact, one datagram each

void rocketnet_test_server_uring_wire_gso_ipv4()
{
    rocketnet_test_server_uring_wire_exchange_flags( "127.0.0.1", "127.0.0.1", "127.0.0.1", 1, 0, 1 );
}

void rocketnet_test_server_uring_wire_gso_ipv6()
{
    rocketnet_test_server_uring_wire_exchange_flags( "[::1]", "[::1]", "[::1]", 0, 0, 1 );
}

// the plain tier, forced: the same exchange with segmentation disabled at
// create sends one SENDMSG per packet and no segmented send

void rocketnet_test_server_uring_wire_plain()
{
    rocketnet_test_server_uring_wire_exchange_flags( "127.0.0.1", "127.0.0.1", "127.0.0.1", 1, ROCKETNET_SERVER_URING_DISABLE_SEGMENT, 0 );
}

// UDP_GRO on the io_uring receive: the client sends a burst of UDP_SEGMENT
// super-datagrams, which loopback keeps whole and the transport's UDP_GRO
// sockets receive coalesced, one provided buffer and one CQE each: the
// receive splits each buffer by its stride (rocketnet_coalesce_split) into
// the bank, so every datagram surfaces byte-exact, in arrival order, and the
// GRO_BATCHES counter proves the split ran. the bursts are a short-tail run,
// an exact multiple and one near the 64 KiB a buffer holds. the counter is
// asserted where the host probe observed real coalescing (the gso
// transport's rule: tx-udp-segmentation off on loopback is a legitimate
// host, delivering the same bytes uncoalesced)

static void rocketnet_test_server_uring_wire_gro( const char * address_string, bool ipv6 )
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, address_string ) );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_create_flags( NULL, &bind_address, 2, 0 );
    rocketnet_check( uring );
    rocketnet_check( ( rocketnet_server_uring_features( uring ) & ROCKETNET_SERVER_URING_FEATURE_GRO ) != 0 );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, address_string ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                         ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );

    struct sockaddr_storage to;
    memset( &to, 0, sizeof(to) );
    socklen_t to_bytes = 0;
    if ( ipv6 )
    {
        struct sockaddr_in6 * to6 = (struct sockaddr_in6*) &to;
        to6->sin6_family = AF_INET6;
        to6->sin6_port = htons( bind_address.port );
        to6->sin6_addr = in6addr_loopback;
        to_bytes = sizeof(struct sockaddr_in6);
    }
    else
    {
        struct sockaddr_in * to4 = (struct sockaddr_in*) &to;
        to4->sin_family = AF_INET;
        to4->sin_port = htons( bind_address.port );
        to4->sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        to_bytes = sizeof(struct sockaddr_in);
    }

    // the bursts: { segments, stride, tail } with tail 0 for an exact multiple

    const int bursts[3][3] = { { 8, 100, 0 }, { 5, 300, 150 }, { 50, 1200, 600 } };
    const int num_bursts = 3;

    int expected_bytes[128];
    int num_expected = 0;

    static uint8_t super[65000];
    for ( int b = 0; b < num_bursts; b++ )
    {
        const int segments = bursts[b][0] + ( bursts[b][2] > 0 ? 1 : 0 );
        int bytes = 0;
        for ( int i = 0; i < segments; i++ )
        {
            const int segment_bytes = i < bursts[b][0] ? bursts[b][1] : bursts[b][2];
            const int tag = num_expected;
            super[bytes] = (uint8_t) tag;
            for ( int j = 1; j < segment_bytes; j++ )
                super[bytes + j] = (uint8_t) ( tag * 13 + j );
            expected_bytes[num_expected++] = segment_bytes;
            bytes += segment_bytes;
        }
        rocketnet_check( bytes <= (int) sizeof(super) );

        struct iovec iov = { super, (size_t) bytes };
        alignas(8) char control[CMSG_SPACE(sizeof(uint16_t))];
        memset( control, 0, sizeof(control) );
        struct msghdr msg;
        memset( &msg, 0, sizeof(msg) );
        msg.msg_name = &to;
        msg.msg_namelen = to_bytes;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
        const uint16_t stride = (uint16_t) bursts[b][1];
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );
        rocketnet_check( sendmsg( client_socket->handle, &msg, 0 ) == (ssize_t) bytes );
    }

    // every datagram byte-exact and in arrival order: the bursts are one flow
    // into one queue, so the bank holds them in the order they were sent

    int num_seen = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < num_expected )
    {
        rocketnet_server_uring_receive_packets( uring );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_uring_process_packets( uring );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            rocketnet_check( tag == num_seen );
            rocketnet_check( received->packet_bytes[i] == expected_bytes[tag] );
            for ( int j = 1; j < expected_bytes[tag]; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 13 + j ) );
            rocketnet_check( received->from[i].type == ( ipv6 ? ROCKETNET_ADDRESS_IPV6 : ROCKETNET_ADDRESS_IPV4 ) );
            rocketnet_check( received->from[i].port == client_address.port );
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == num_expected );

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED] == (uint64_t) num_expected );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE] == 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW] == 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_GRO_CMSG_FAULTS] == 0 );
    if ( rocketnet_test_gso_host_coalesces() )
        rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_GRO_BATCHES] > 0 );

    // the plain tier on the same kernel: DISABLE_GRO reserves no control
    // area and reports no GRO

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( uring, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );

    rocketnet_address_t plain_address;
    rocketnet_check( rocketnet_address_parse( &plain_address, address_string ) );
    struct rocketnet_server_uring_t * plain = rocketnet_server_uring_create_flags( NULL, &plain_address, 1, ROCKETNET_SERVER_URING_DISABLE_GRO );
    rocketnet_check( plain );
    rocketnet_check( ( rocketnet_server_uring_features( plain ) & ROCKETNET_SERVER_URING_FEATURE_GRO ) == 0 );
    rn_native_backend_stop_and_destroy( plain, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );
}

void rocketnet_test_server_uring_wire_gro_ipv4()
{
    rocketnet_test_server_uring_wire_gro( "127.0.0.1", false );
}

void rocketnet_test_server_uring_wire_gro_ipv6()
{
    rocketnet_test_server_uring_wire_gro( "[::1]", true );
}

void rocketnet_test_server_uring_wire_ipv6()
{
    rocketnet_test_server_uring_wire_exchange( "[::1]", "[::1]", "[::1]", 0 );
}

void rocketnet_test_server_uring_wire_dual_stack()
{
    // an ipv6 wildcard listen accepts an ipv4 client, whose source surfaces
    // as native ipv4 -- the v4-mapped normalization the standard makes law

    // wildcard bind kept: the case is the ipv6 wildcard listen serving an ipv4 client
    rocketnet_test_server_uring_wire_exchange( "[::]", "127.0.0.1", "127.0.0.1", 1 );
}

void rocketnet_test_server_uring_send_overflow()
{
    // fill the send staging past capacity: starts beyond it refuse with
    // NULL, the flush sends what fit, and the next phase is whole -- the
    // transport-level face of the transport's ring exhaustion property

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_create( NULL, &bind_address, 1 );
    rocketnet_check( uring );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );

    for ( int i = 0; i < ROCKETNET_URING_SEND_QUEUE_SIZE; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_uring_start_packet( uring, &client_address, &packet_id );
        rocketnet_check( frame );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_uring_finish_packet( uring, packet_id, frame, 64 );
    }

    uint64_t overflow_id = 0;
    rocketnet_check( rocketnet_server_uring_start_packet( uring, &client_address, &overflow_id ) == NULL );
    rocketnet_check( rocketnet_server_uring_start_packet( uring, &client_address, &overflow_id ) == NULL );

    rocketnet_server_uring_send_packets( uring );

    // every staged packet was either sent or dropped, and nothing vanished
    // uncounted

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] + counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] == ROCKETNET_URING_SEND_QUEUE_SIZE );

    // the next phase is whole: a fresh tagged packet flushes and arrives

    uint8_t tag[64];
    memset( tag, 0xAB, sizeof(tag) );

    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_server_uring_start_packet( uring, &client_address, &packet_id );
    rocketnet_check( frame );
    memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, tag, sizeof(tag) );
    rocketnet_server_uring_finish_packet( uring, packet_id, frame, (int) sizeof(tag) );
    rocketnet_server_uring_send_packets( uring );

    bool recovered = false;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( client_socket, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        if ( packet_bytes == (int) sizeof(tag) && memcmp( packet, tag, sizeof(tag) ) == 0 )
            recovered = true;
    }
    rocketnet_check( recovered );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( uring, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );
}

void rocketnet_test_server_uring_receive_rearms_after_no_buffers()
{
    // with one queue, send more datagrams than its provided buffer ring
    // holds and do not receive them: the ring runs dry, the multishot ends
    // and must be re-armed by the next receive, and the wire works after

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );

    struct rocketnet_server_uring_t * uring = rocketnet_server_uring_create( NULL, &bind_address, 1 );
    rocketnet_check( uring );

    rocketnet_address_t client_address;
    rocketnet_check( rocketnet_address_parse( &client_address, "127.0.0.1" ) );
    struct rocketnet_platform_socket_t * client_socket = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( client_socket );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, "127.0.0.1" ) );
    server_address.port = bind_address.port;

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( uring, counters );
    const uint64_t no_buffers_before = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS];
    const uint64_t overflow_before = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW];
    const uint64_t rearms_before = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS];

    uint8_t packet[64];
    for ( int i = 0; i < ROCKETNET_URING_RECV_BUFFERS + 64; i++ )
    {
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < (int) sizeof(packet); j++ )
            packet[j] = (uint8_t) ( i * 7 + j );
        rocketnet_platform_socket_send_packet( client_socket, &server_address, packet, (int) sizeof(packet) );
    }

    int num_received = 0;
    uint64_t rearms_after = rearms_before;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && rearms_after == rearms_before )
    {
        rocketnet_server_uring_receive_packets( uring );
        num_received += rocketnet_server_uring_process_packets( uring )->num_packets;
        rocketnet_server_uring_counters( uring, counters );
        rearms_after = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS];
        if ( rearms_after == rearms_before )
            rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rearms_after > rearms_before );
    rocketnet_check( num_received >= ROCKETNET_URING_RECV_BUFFERS );

    // the ring ran dry before the transport reaped: the kernel ends the
    // multishot with ENOBUFS, counted RECEIVE_NO_BUFFERS, not
    // RECEIVE_DROPPED_OVERFLOW -- a full fill bank is a different ending,
    // and one ring's worth of datagrams fits a bank many times over

    const bool no_buffers_moved = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS] > no_buffers_before;
    const bool overflow_moved = counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW] > overflow_before;
    rocketnet_check( no_buffers_moved || overflow_moved );

    // the re-armed multishot receives the next datagram byte-exact

    uint8_t tag[64];
    memset( tag, 0xCD, sizeof(tag) );
    rocketnet_platform_socket_send_packet( client_socket, &server_address, tag, (int) sizeof(tag) );

    bool recovered = false;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        rocketnet_server_uring_receive_packets( uring );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_uring_process_packets( uring );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            if ( received->packet_bytes[i] == (int) sizeof(tag) && memcmp( received->packet_data[i], tag, sizeof(tag) ) == 0 )
                recovered = true;
        }
        if ( !recovered )
            rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( recovered );

    rocketnet_platform_socket_destroy( client_socket );
    rn_native_backend_stop_and_destroy( uring, rocketnet_server_uring_stop, rocketnet_server_uring_stopped, rocketnet_server_uring_destroy );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX


void rocketnet_test_transport_acked_unfragmented_was_in_exactly_one_batch()
{
    // P2 (docs/CHANNELS.md section 2) over real loopback: for an
    // unfragmented payload (type 5), an acked packet id means the payload
    // was in exactly one delivered batch at the receiver. Both directions, ten
    // thousand payloads each way. The payload body carries its sender
    // sequence, the acked id is joined back to that sequence, and every
    // acked id's payload must have been delivered exactly once -- with an id
    // whose payload never arrived never acked. Mutation that must bite:
    // ack a payload id without it entering the batch (or enter it twice);
    // the acked-implies-delivered-exactly-once check below goes red.

    const int total = 10000;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 1;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, 0xA2A2ULL, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_client_transport_send_packets( client );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    const int client_index = rocketnet_client_transport_client_index( client );

    uint8_t * client_delivered = (uint8_t*) rocketnet_malloc( NULL, (size_t)( total + 1 ) );
    uint8_t * client_acked = (uint8_t*) rocketnet_malloc( NULL, (size_t)( total + 1 ) );
    uint8_t * server_delivered = (uint8_t*) rocketnet_malloc( NULL, (size_t)( total + 1 ) );
    uint8_t * server_acked = (uint8_t*) rocketnet_malloc( NULL, (size_t)( total + 1 ) );
    rocketnet_check( client_delivered && client_acked && server_delivered && server_acked );
    memset( client_delivered, 0, (size_t)( total + 1 ) );
    memset( client_acked, 0, (size_t)( total + 1 ) );
    memset( server_delivered, 0, (size_t)( total + 1 ) );
    memset( server_acked, 0, (size_t)( total + 1 ) );

    std::unordered_map<uint64_t, int> client_id_to_sequence;
    std::unordered_map<uint64_t, int> server_id_to_sequence;
    client_id_to_sequence.reserve( total * 2 );
    server_id_to_sequence.reserve( total * 2 );

    int sent_client = 0, sent_server = 0, acked_client = 0, acked_server = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 90.0 && ( sent_client < total || sent_server < total ) )
    {
        if ( sent_client < total )
        {
            struct rocketnet_packet_context_t ctx;
            uint8_t * frame = rocketnet_client_transport_start_packet( client, 32, &ctx );
            if ( frame )
            {
                const int sequence = sent_client + 1;
                memcpy( frame + ctx.payload_index, &sequence, sizeof(sequence) );
                if ( rocketnet_client_transport_finish_packet( client, &ctx, sizeof(sequence) ) > 0 )
                {
                    client_id_to_sequence[ctx.packet_id] = sequence;
                    sent_client++;
                }
            }
        }
        if ( sent_server < total )
        {
            struct rocketnet_packet_context_t ctx;
            uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, 32, &ctx );
            if ( frame )
            {
                const int sequence = sent_server + 1;
                memcpy( frame + ctx.payload_index, &sequence, sizeof(sequence) );
                if ( rocketnet_server_transport_finish_packet( server, &ctx, sizeof(sequence) ) > 0 )
                {
                    server_id_to_sequence[ctx.packet_id] = sequence;
                    sent_server++;
                }
            }
        }

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_client_transport_send_packets( client );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        for ( int i = 0; i < rocketnet_client_transport_num_payloads( client ); i++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            if ( payload && payload_bytes == (int) sizeof(int) )
            {
                int sequence = 0;
                memcpy( &sequence, payload, sizeof(sequence) );
                if ( sequence >= 1 && sequence <= total )
                    server_delivered[sequence]++;
            }
        }

        for ( int i = 0; i < rocketnet_server_transport_num_payloads( server ); i++ )
        {
            int payload_client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            if ( payload && payload_bytes == (int) sizeof(int) && payload_client_index == client_index )
            {
                int sequence = 0;
                memcpy( &sequence, payload, sizeof(sequence) );
                if ( sequence >= 1 && sequence <= total )
                    client_delivered[sequence]++;
            }
        }

        {
            const uint64_t * ids = NULL;
            const int num_acked = rocketnet_client_transport_acked_packets( client, &ids );
            for ( int i = 0; i < num_acked; i++ )
            {
                std::unordered_map<uint64_t, int>::const_iterator it = client_id_to_sequence.find( ids[i] );
                if ( it != client_id_to_sequence.end() && !client_acked[it->second] )
                {
                    client_acked[it->second] = 1;
                    acked_client++;
                }
            }
        }

        {
            const int * client_indices = NULL;
            const uint64_t * ids = NULL;
            const int num_acked = rocketnet_server_transport_acked_packets( server, &client_indices, &ids );
            for ( int i = 0; i < num_acked; i++ )
            {
                if ( client_indices[i] != client_index )
                    continue;
                std::unordered_map<uint64_t, int>::const_iterator it = server_id_to_sequence.find( ids[i] );
                if ( it != server_id_to_sequence.end() && !server_acked[it->second] )
                {
                    server_acked[it->second] = 1;
                    acked_server++;
                }
            }
        }

        rocketnet_platform_sleep( 0.0005 );
    }

    for ( int sequence = 1; sequence <= total; sequence++ )
    {
        if ( client_acked[sequence] )
            rocketnet_check( client_delivered[sequence] == 1 );
        if ( server_acked[sequence] )
            rocketnet_check( server_delivered[sequence] == 1 );
        if ( client_delivered[sequence] == 0 )
            rocketnet_check( !client_acked[sequence] );
        if ( server_delivered[sequence] == 0 )
            rocketnet_check( !server_acked[sequence] );
    }
    rocketnet_check( acked_client > 0 );
    rocketnet_check( acked_server > 0 );

    rocketnet_free( NULL, client_delivered );
    rocketnet_free( NULL, client_acked );
    rocketnet_free( NULL, server_delivered );
    rocketnet_free( NULL, server_acked );
    rocketnet_client_transport_disconnect( client );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_transport_reply_opens_a_later_update()
{
    // P6 (docs/CHANNELS.md section 2) over real loopback: a reply to a packet
    // this end opened in update j is first opened by this end in update j+1
    // or later. Each pass is one update and mirrors the transport's phase
    // order: the client's update swaps receive buffers and takes the batch
    // fixed at the start of the update, then the update opens its request,
    // then the server's update takes its batch and answers. The pass in which
    // the client first opens the answer must be strictly later than the pass
    // that opened the request.
    //
    // The single mutation that must bite (applied to the bench copy, never
    // committed): move the client's update + payload-drain block below to
    // after the server's answer block, so the client's batch is fixed after
    // the request left and after the server answered; the answer then lands in
    // the same pass, reply_seen equals request_opened, and the strictly-later
    // check at the bottom goes red. The transport-side rule this pins is the
    // client buffer swap (sdk/rocketnet_client_transport.cpp:466, 563).

    const int rounds = 200;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 1;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config);
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, 0xB6B6ULL, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_client_transport_send_packets( client );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    const int client_index = rocketnet_client_transport_client_index( client );

    std::unordered_map<int, int> request_opened_update;
    std::unordered_map<int, int> reply_seen_update;

    int sent = 0;
    int observed = 0;

    for ( int update = 1; update <= rounds * 60 && observed < rounds; update++ )
    {
        // the client's update for this pass: swap the receive buffers and take
        // the batch fixed at the start of the update, before this update opens
        // anything. the client records the update each reply is first opened
        // in. THIS BLOCK is the phase-P6 boundary; the mutation moves it after
        // the server's answer block below.
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_client_transport_send_packets( client );
        for ( int i = 0; i < rocketnet_client_transport_num_payloads( client ); i++ )
        {
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            if ( payload && payload_bytes == (int) sizeof(int) )
            {
                int request = 0;
                memcpy( &request, payload, sizeof(request) );
                if ( request >= 1 && request <= rounds && reply_seen_update.find( request ) == reply_seen_update.end() )
                {
                    reply_seen_update[request] = update;
                    observed++;
                }
            }
        }

        // now this pass's send: the client opens the next request, in this
        // update. the server answers it in the server phase below, and that
        // answer cannot reach this pass's batch (already drained above).
        if ( sent < rounds )
        {
            struct rocketnet_packet_context_t ctx;
            uint8_t * frame = rocketnet_client_transport_start_packet( client, 32, &ctx );
            if ( frame )
            {
                const int request = sent + 1;
                memcpy( frame + ctx.payload_index, &request, sizeof(request) );
                if ( rocketnet_client_transport_finish_packet( client, &ctx, sizeof(request) ) > 0 )
                {
                    request_opened_update[request] = update;
                    sent++;
                    rocketnet_client_transport_send_packets( client );
                }
            }
        }

        // the server's update for this pass: take its batch and answer. the
        // request opened above has arrived, so the server answers it here; the
        // client's batched pass above cannot see the answer, which reaches the
        // client for a later update.
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        for ( int i = 0; i < rocketnet_server_transport_num_payloads( server ); i++ )
        {
            int payload_client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            if ( payload && payload_bytes == (int) sizeof(int) && payload_client_index == client_index )
            {
                int request = 0;
                memcpy( &request, payload, sizeof(request) );
                if ( request >= 1 && request <= rounds )
                {
                    struct rocketnet_packet_context_t ctx;
                    uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, 32, &ctx );
                    if ( frame )
                    {
                        memcpy( frame + ctx.payload_index, &request, sizeof(request) );
                        if ( rocketnet_server_transport_finish_packet( server, &ctx, sizeof(request) ) > 0 )
                            rocketnet_server_transport_send_packets( server );
                    }
                }
            }
        }

        rocketnet_platform_sleep( 0.0002 );
    }

    for ( int request = 1; request <= rounds; request++ )
    {
        rocketnet_check( reply_seen_update.find( request ) != reply_seen_update.end() );
        rocketnet_check( reply_seen_update[request] > request_opened_update[request] );
    }

    rocketnet_client_transport_disconnect( client );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

// The message objects over real sockets: a message server and a message
// client through loopback, through the C API alone. The game clock starts at
// T=1000.0, never 0.0, and the first update of each object is at exactly
// 1000.0: a time field left at a zero seed shows up as a handshake timeout or
// an expired token here instead of passing by accident. Real clocks and a real
// socket, so this case lives in the functional tier.

void rocketnet_test_message_objects_loopback_from_t1000()
{
    const double game_start = 1000.0;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 2;

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "127.0.0.1", &server_config, NULL, NULL, 0, 0 );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( rocketnet_message_server_transport( server ) ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_message_client_t * client = rocketnet_message_client_create( NULL, connect_token, rocketnet_functional_client_config(), NULL, NULL, 0, 0 );
    rocketnet_check( client );

    // the game's clock: 1000.0 at the first update, then real seconds on top
    const double wall_start = rocketnet_platform_time();
    double now = game_start;

    // the first updates, both at exactly T=1000.0
    rocketnet_check( rocketnet_message_server_update( server, now ) == ROCKETNET_UPDATE_OK );
    rocketnet_check( rocketnet_message_client_update( client, now ) == ROCKETNET_UPDATE_OK );
    rocketnet_check( rocketnet_message_client_state( client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    rocketnet_message_client_send_packets( client );
    rocketnet_message_server_send_packets( server );

    // connect, bounded by the wall clock
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        rocketnet_platform_sleep( 0.001 );
        now = game_start + ( rocketnet_platform_time() - wall_start );
        rocketnet_check( rocketnet_message_client_update( client, now ) == ROCKETNET_UPDATE_OK );
        rocketnet_check( rocketnet_message_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( client );
        rocketnet_message_server_send_packets( server );
        if ( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_message_server_client_connected( server, 0 ) )
            break;
        if ( rocketnet_message_client_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;
    }
    rocketnet_check( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_message_server_client_connected( server, 0 ) );
    rocketnet_check( now >= game_start );

    // reliable ordered both ways, written in place through start and finish,
    // and one unreliable message each way per round until one lands
    const int num_reliable = 20;
    int client_sent = 0;
    int server_sent = 0;
    int server_got = 0;
    int client_got = 0;
    int server_got_unreliable = 0;
    int client_got_unreliable = 0;

    const double send_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - send_start < 5.0 )
    {
        if ( client_sent < num_reliable )
        {
            uint8_t * p = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
            rocketnet_check( p );
            for ( int i = 0; i < 8; i++ )
                p[i] = (uint8_t) ( client_sent + i );
            rocketnet_check( rocketnet_message_client_finish( client, 8 ) != 0 );
            client_sent++;
        }
        if ( server_sent < num_reliable )
        {
            uint8_t * p = rocketnet_message_server_start( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
            rocketnet_check( p );
            for ( int i = 0; i < 8; i++ )
                p[i] = (uint8_t) ( 100 + server_sent + i );
            rocketnet_check( rocketnet_message_server_finish( server, 0, 8 ) != 0 );
            server_sent++;
        }
        if ( server_got_unreliable == 0 )
        {
            const uint8_t ping[4] = { 'p', 'i', 'n', 'g' };
            rocketnet_check( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, ping, 4 ) != 0 );
        }
        if ( client_got_unreliable == 0 )
        {
            const uint8_t pong[4] = { 'p', 'o', 'n', 'g' };
            rocketnet_check( rocketnet_message_server_send( server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, pong, 4 ) != 0 );
        }

        rocketnet_message_client_send_packets( client );
        rocketnet_message_server_send_packets( server );

        rocketnet_platform_sleep( 0.001 );
        now = game_start + ( rocketnet_platform_time() - wall_start );
        rocketnet_check( rocketnet_message_client_update( client, now ) == ROCKETNET_UPDATE_OK );
        rocketnet_check( rocketnet_message_server_update( server, now ) == ROCKETNET_UPDATE_OK );

        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_message_server_receive( server, NULL, NULL, &data, &bytes ) )
        {
            if ( bytes == 4 )
            {
                rocketnet_check( memcmp( data, "ping", 4 ) == 0 );
                server_got_unreliable++;
            }
            else
            {
                rocketnet_check( bytes == 8 );
                for ( int i = 0; i < 8; i++ )
                    rocketnet_check( data[i] == (uint8_t) ( server_got + i ) );          // in order, intact
                server_got++;
            }
            rocketnet_message_server_release( server, data );
        }
        while ( rocketnet_message_client_receive( client, NULL, &data, &bytes ) )
        {
            if ( bytes == 4 )
            {
                rocketnet_check( memcmp( data, "pong", 4 ) == 0 );
                client_got_unreliable++;
            }
            else
            {
                rocketnet_check( bytes == 8 );
                for ( int i = 0; i < 8; i++ )
                    rocketnet_check( data[i] == (uint8_t) ( 100 + client_got + i ) );
                client_got++;
            }
            rocketnet_message_client_release( client, data );
        }

        if ( server_got == num_reliable && client_got == num_reliable && server_got_unreliable > 0 && client_got_unreliable > 0 )
            break;
    }
    rocketnet_check( server_got == num_reliable );
    rocketnet_check( client_got == num_reliable );
    rocketnet_check( server_got_unreliable > 0 );
    rocketnet_check( client_got_unreliable > 0 );
    rocketnet_check( rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_message_client_disconnect_reason( client ) == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_check( rocketnet_message_server_disconnect_reason( server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // the deliberate disconnect crosses the wire and the server's slot empties
    rocketnet_client_transport_disconnect( rocketnet_message_client_transport( client ) );
    const double disconnect_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - disconnect_start < 2.0 )
    {
        rocketnet_platform_sleep( 0.001 );
        now = game_start + ( rocketnet_platform_time() - wall_start );
        // the client keeps updating while the server waits: a disconnect burst whose
        // hand-off found the other send ring busy is deferred and goes out on
        // the client's next update (sdk/rocketnet_client_transport.cpp
        // rocketnet_client_transport_disconnect and _fill_destination), not before
        rocketnet_client_transport_update( rocketnet_message_client_transport( client ), now );
        rocketnet_check( rocketnet_message_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        if ( !rocketnet_message_server_client_connected( server, 0 ) )
            break;
    }
    rocketnet_check( !rocketnet_message_server_client_connected( server, 0 ) );

    rn_native_message_client_stop_and_destroy( client );

    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( server );
    rocketnet_server_transport_stop( server_transport );
    const double stop_start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server_transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - stop_start < 5.0 )
    {
        rocketnet_platform_sleep( 0.001 );
        now = game_start + ( rocketnet_platform_time() - wall_start );
        rocketnet_message_server_update( server, now );
    }
    rocketnet_check( rocketnet_server_transport_state( server_transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rn_native_message_server_stop_and_destroy( server );
}

// The same event-only wake contract on each native implementation: an unset
// wake times out, set-before-wait is retained and consumed once, a second set
// after consumption wakes again, and invalid socket-read waits neither block
// nor consume the pending signal.

#if defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
static void rocketnet_test_platform_wake_event_only_set_thread( void * arg )
{
    rocketnet_platform_sleep( 0.01 );
    rocketnet_platform_wake_set( (rocketnet_platform_wake_t*) arg );
}

void rocketnet_test_platform_wake_event_only()
{
    rocketnet_platform_wake_t * wake = rocketnet_platform_wake_create( NULL, NULL );
    rocketnet_check( wake );
    rocketnet_check( rocketnet_platform_wake_allocation_bytes() > 0 );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.01 ) );

    rocketnet_platform_wake_set( wake );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.01 ) );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    rocketnet_platform_wake_set( wake );
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_test_platform_wake_event_only_set_thread, wake );
    rocketnet_check( thread );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 2.0 ) );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_platform_wake_destroy( wake );
}
#endif

// The level-triggered wake beside a real socket on Linux: the eventfd is set
// and consumed, a socket's readiness is seen only when readable is true, and
// with readable false a full receive buffer waits on the wake alone
// (docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla's IoWait and IoWake). A
// real socket and a real eventfd, so this case lives in the functional tier.

void rocketnet_test_platform_wake_linux_wakes_and_reads()
{
    // a non-blocking socket on 127.0.0.1 port 0
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );

    // socket-backed wake creation still succeeds
    rocketnet_platform_wake_t * wake = rocketnet_platform_wake_create( NULL, socket );
    rocketnet_check( wake );

    // wait(true, 0) is false: no wake set and nothing readable
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // set, then wait(true, 1.0) is true and the next wait(true, 0) is false
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 1.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // set twice, one wait(true, 0) true then false
    rocketnet_platform_wake_set( wake );
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // send a datagram from a second socket to the first
    rocketnet_address_t sender_bind;
    rocketnet_address_parse( &sender_bind, "127.0.0.1" );
    rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( sender );

    uint8_t packet[8];
    memset( packet, 0xAB, sizeof(packet) );
    rocketnet_platform_socket_send_packet( sender, &bind_address, packet, sizeof(packet) );

    // the socket is readable now, so wait(true, 1.0) is true
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 1.0 ) );

    // with that datagram still unread, wait(false, 0) is false (socket ignored)
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    // then set and wait(false, 0) is true
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    rocketnet_platform_wake_destroy( wake );
    rocketnet_platform_socket_destroy( socket );
    rocketnet_platform_socket_destroy( sender );
}

// The Linux client's mmsg batch send/receive (#client-io-linux-mmsg): two
// loopback sockets, 100 datagrams of distinct sizes 20..119 in one
// send_packets_mmsg call, receive_packets_mmsg with max 64 returning them
// in order over two calls byte-exact with the sender's address, and an
// empty socket returning 0. A real socket pair, so this case lives in the
// functional tier.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

void rocketnet_test_client_io_linux_mmsg_batch_round_trip()
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_platform_socket_t * receiver = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( receiver );

    rocketnet_address_t sender_bind;
    rocketnet_address_parse( &sender_bind, "127.0.0.1" );
    rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( sender );

    const int num_packets = 100;
    const int stride = 256;
    static uint8_t send_data[100][128];
    static uint8_t receive_data[100][stride];
    static rocketnet_address_t from[100];
    static int packet_bytes[100];

    struct rocketnet_platform_send_t sends[100];
    for ( int i = 0; i < num_packets; i++ )
    {
        const int bytes = 20 + i;
        for ( int j = 0; j < bytes; j++ )
            send_data[i][j] = (uint8_t) ( i + j );
        sends[i].to = &bind_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = bytes;
    }
    rocketnet_platform_socket_send_packets_mmsg( sender, sends, num_packets );

    int received = 0;
    double start_time = rocketnet_platform_time();
    while ( received < num_packets && rocketnet_platform_time() - start_time < 5.0 )
    {
        const int got = rocketnet_platform_socket_receive_packets_mmsg( receiver, from + received, receive_data[received], stride, packet_bytes + received, 64 );
        if ( got == 0 )
            rocketnet_platform_sleep( 0.001 );
        received += got;
    }
    rocketnet_check( received == num_packets );

    for ( int i = 0; i < num_packets; i++ )
    {
        rocketnet_check( rocketnet_address_equal( &from[i], &sender_bind ) );
        const int bytes = 20 + i;
        rocketnet_check( packet_bytes[i] == bytes );
        for ( int j = 0; j < bytes; j++ )
            rocketnet_check( receive_data[i][j] == (uint8_t) ( i + j ) );
    }

    rocketnet_check( rocketnet_platform_socket_receive_packets_mmsg( receiver, from, receive_data[0], stride, packet_bytes, 64 ) == 0 );

    // One recvmmsg result with a truncated datagram, a zero-length one and a
    // kept one exercises chunk indexing, the oversize report (true size, the
    // stored prefix) and rejection compaction. The zero-length datagram goes
    // by a raw sendto: the mmsg send path carries positive-size payloads only.
    uint8_t truncated_data[16];
    memset( truncated_data, 'T', sizeof(truncated_data) );
    uint8_t kept_data[2] = { 0xA1, 0xB2 };

    sockaddr_in zero_length_to;
    rocketnet_platform_address_to_sockaddr_in( &bind_address, &zero_length_to );
    const char zero_length_byte = 0;
    rocketnet_check( sendto( sender->handle, &zero_length_byte, 0, 0, (sockaddr*) &zero_length_to, sizeof(zero_length_to) ) == 0 );

    struct rocketnet_platform_send_t compact_sends[2];
    compact_sends[0].to = &bind_address;
    compact_sends[0].packet_data = truncated_data;
    compact_sends[0].packet_bytes = sizeof(truncated_data);
    compact_sends[1].to = &bind_address;
    compact_sends[1].packet_data = &kept_data[1];
    compact_sends[1].packet_bytes = 1;
    rocketnet_platform_socket_send_packets_mmsg( sender, compact_sends, 2 );
    rocketnet_platform_sleep( 0.01 );

    uint8_t compact_receive_data[3][8];
    rocketnet_address_t compact_from[3];
    int compact_bytes[3];
    const int compact_received = rocketnet_platform_socket_receive_packets_mmsg( receiver, compact_from, compact_receive_data[0], sizeof(compact_receive_data[0]), compact_bytes, 3 );
    rocketnet_check( compact_received == 2 );
    rocketnet_check( rocketnet_address_equal( &compact_from[0], &sender_bind ) && compact_bytes[0] == (int) sizeof(truncated_data) );
    for ( int j = 0; j < (int) sizeof(compact_receive_data[0]); j++ )
        rocketnet_check( compact_receive_data[0][j] == 'T' );
    rocketnet_check( rocketnet_address_equal( &compact_from[1], &sender_bind ) && compact_bytes[1] == 1 && compact_receive_data[1][0] == kept_data[1] );

    // A kernel-refused entry drops by itself and the unsent suffix is retried.
    uint8_t retry_byte = 0x5A;
    struct rocketnet_platform_send_t retry_sends[2];
    retry_sends[0].to = &bind_address;
    retry_sends[0].packet_data = &retry_byte;
    retry_sends[0].packet_bytes = 0x7fffffff;
    retry_sends[1].to = &bind_address;
    retry_sends[1].packet_data = &retry_byte;
    retry_sends[1].packet_bytes = 1;
    rocketnet_platform_socket_send_packets_mmsg( sender, retry_sends, 2 );

    static uint8_t retry_data[64][stride];
    rocketnet_address_t retry_from[64];
    int retry_bytes[64];
    const int retry_received = rocketnet_platform_socket_receive_packets_mmsg( receiver, retry_from, retry_data[0], stride, retry_bytes, 64 );
    rocketnet_check( retry_received == 1 );
    rocketnet_check( rocketnet_address_equal( &retry_from[0], &sender_bind ) );
    rocketnet_check( retry_bytes[0] == 1 && retry_data[0][0] == retry_byte );

    rocketnet_platform_socket_destroy( sender );
    rocketnet_platform_socket_destroy( receiver );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The kqueue wake beside a real socket on macOS and iOS: the EVFILT_USER
// trigger is set and consumed, the socket's EVFILT_READ is seen only when
// readable is true (the filter is disabled and enabled in the wait's own
// kevent call), and with readable false an unread datagram waits on the
// trigger alone (docs/CLIENT-IO.md "The wake"; tla/RNClientIo.tla's IoWait and
// IoWake). A real socket and a real kqueue, so this case lives in the
// functional tier.

void rocketnet_test_platform_wake_apple_wakes_and_reads()
{
    // a non-blocking socket on 127.0.0.1 port 0
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );

    // socket-backed wake creation still succeeds
    rocketnet_platform_wake_t * wake = rocketnet_platform_wake_create( NULL, socket );
    rocketnet_check( wake );

    // wait(true, 0) is false: no wake set and nothing readable
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // a wait with a 0.01 s timeout and nothing pending returns false, after
    // waiting (not at once)
    const double wait_start = rocketnet_platform_time();
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.01 ) );
    rocketnet_check( rocketnet_platform_time() - wait_start >= 0.005 );

    // set, then wait(true, 0) is true and the next wait(true, 0) is false
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // set twice, one wait(true, 0) true then false
    rocketnet_platform_wake_set( wake );
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // send a datagram from a second socket to the first
    rocketnet_address_t sender_bind;
    rocketnet_address_parse( &sender_bind, "127.0.0.1" );
    rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( sender );

    uint8_t packet[8];
    memset( packet, 0xAB, sizeof(packet) );
    rocketnet_platform_socket_send_packet( sender, &bind_address, packet, sizeof(packet) );

    // the socket is readable now: wait(true, 1.0) returns true once the
    // datagram lands, and with it still unread wait(true, 0) stays true
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 1.0 ) );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // with that datagram still unread, wait(false, 0) is false (the read
    // filter is disabled), and stays false
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    // then set and wait(false, 0) is true, and the set is consumed
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    // a following wait(true, 0) re-enables the read filter and sees the datagram
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // reading the datagram leaves nothing pending: wait(true, 0) is false
    rocketnet_address_t from;
    uint8_t received[64];
    rocketnet_check( rocketnet_platform_socket_receive_packet( socket, &from, received, sizeof(received) ) == (int) sizeof(packet) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    rocketnet_platform_wake_destroy( wake );
    rocketnet_platform_socket_destroy( socket );
    rocketnet_platform_socket_destroy( sender );
}

// The level-triggered wake beside a real socket on Windows: the manual-reset
// wake event is set and consumed, the socket's FD_READ event is seen only when
// readable is true, and with readable false an unread datagram waits on the
// wake alone (docs/CLIENT-IO.md "The wake" and rocketnet-client-io-windows;
// tla/RNClientIo.tla's IoWait and IoWake). A real socket and real events, so
// this case lives in the functional tier.

void rocketnet_test_platform_wake_windows_wakes_and_reads()
{
    // a non-blocking socket on 127.0.0.1 port 0
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "127.0.0.1" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );

    // socket-backed wake creation still succeeds
    rocketnet_platform_wake_t * wake = rocketnet_platform_wake_create( NULL, socket );
    rocketnet_check( wake );

    // wait(true, 0) is false: no wake set and nothing readable
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // a wait with a 0.01 s timeout and nothing pending returns false (no
    // elapsed-time check: a Windows wait deadline rounds to the clock tick)
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.01 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.01 ) );

    // set, then wait(true, 0) is true and the next wait(true, 0) is false
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // set twice, one wait(true, 0) true then false
    rocketnet_platform_wake_set( wake );
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // send a datagram from a second socket to the first
    rocketnet_address_t sender_bind;
    rocketnet_address_parse( &sender_bind, "127.0.0.1" );
    rocketnet_platform_socket_t * sender = rocketnet_platform_socket_create( NULL, &sender_bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( sender );

    uint8_t packet[8];
    memset( packet, 0xAB, sizeof(packet) );
    rocketnet_platform_socket_send_packet( sender, &bind_address, packet, sizeof(packet) );

    // the datagram makes the socket readable: wait(true, 1.0) is true, and
    // returns as soon as it lands
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 1.0 ) );

    // the drain to a would-block re-arms FD_READ
    uint8_t received[64];
    rocketnet_address_t from;
    rocketnet_check( rocketnet_platform_socket_receive_packet( socket, &from, received, sizeof(received) ) == (int) sizeof(packet) );
    rocketnet_check( rocketnet_platform_socket_receive_packet( socket, &from, received, sizeof(received) ) == 0 );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, true, 0.0 ) );

    // a second datagram, left unread: wait(false, 0) is false (socket ignored)
    rocketnet_platform_socket_send_packet( sender, &bind_address, packet, sizeof(packet) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.0 ) );
    rocketnet_check( !rocketnet_platform_wake_wait( wake, false, 0.01 ) );

    // then set and wait(false, 0) is true
    rocketnet_platform_wake_set( wake );
    rocketnet_check( rocketnet_platform_wake_wait( wake, false, 0.0 ) );

    // and the following wait(true) sees the datagram (a 1.0 s bound, as the
    // first datagram above: loopback delivery and the FD_READ post are not
    // synchronous with the send; the wait returns as soon as it lands)
    rocketnet_check( rocketnet_platform_wake_wait( wake, true, 1.0 ) );
    rocketnet_check( rocketnet_platform_socket_receive_packet( socket, &from, received, sizeof(received) ) == (int) sizeof(packet) );

    rocketnet_platform_wake_destroy( wake );
    rocketnet_platform_socket_destroy( socket );
    rocketnet_platform_socket_destroy( sender );
}

extern void rocketnet_test_lifecycle_transport();

// A maximum-size packet crosses a 1500-MTU link, whole and unfragmented, on
// each address family: the wire is the judge. The client and the server
// exchange payloads of ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, sealed into datagrams at
// the send budget (the IPv6 UDP payload of a 1500-byte MTU), and each side
// receives every one byte-exact with no fragment counted.
//
// On a bench the link is a veth pair at MTU 1500 between two network
// namespaces: ROCKETNET_TEST_MTU_SERVER_NETNS names the server end's
// namespace (a /proc/<pid>/ns/net path), and ROCKETNET_TEST_MTU_SERVER_IPV4
// and ROCKETNET_TEST_MTU_SERVER_IPV6 its addresses. The server's sockets are
// opened inside that namespace, the client's in this one, so every datagram
// crosses the veth. With none of them set the case runs on loopback.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include <sched.h>
#include <fcntl.h>
#endif

static void rocketnet_test_max_size_packet_crosses( const char * host_env, const char * loopback_host, bool ipv6 )
{
    char host_value[256];
    const char * host = host_value;
    if ( !rocketnet_test_getenv( host_env, host_value, sizeof( host_value ) ) || host_value[0] == '\0' )
        host = loopback_host;

    char bind_host[sizeof( host_value ) + 2];
    if ( ipv6 && host[0] != '[' )
        snprintf( bind_host, sizeof(bind_host), "[%s]", host );
    else
        snprintf( bind_host, sizeof(bind_host), "%s", host );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    char netns[4096];
    int self_ns = -1;
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_MTU_SERVER_NETNS", netns, sizeof( netns ) ) && netns[0] != '\0' )
    {
        self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int server_ns = open( netns, O_RDONLY );
        rocketnet_check( self_ns >= 0 && server_ns >= 0 );
        rocketnet_check( setns( server_ns, CLONE_NEWNET ) == 0 );
        close( server_ns );
    }
#endif

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, bind_host, &server_config );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    if ( self_ns >= 0 )
    {
        rocketnet_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
    }
#endif

    rocketnet_check( server );

    char server_address[sizeof( bind_host ) + 8];
    snprintf( server_address, sizeof(server_address), "%s:%d", bind_host, rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, rocketnet_functional_client_config(), rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    static uint8_t to_server[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    static uint8_t to_client[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    for ( int i = 0; i < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; i++ )
    {
        to_server[i] = (uint8_t) ( i * 7 + 1 );
        to_client[i] = (uint8_t) ( i * 11 + 3 );
    }

    const int count = 8;
    int server_received = 0, client_received = 0, sent_to_server = 0, sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < count )
        {
            rocketnet_client_transport_send_packet( client, to_server, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            sent_to_server++;
        }
        if ( sent_to_client < count )
        {
            rocketnet_server_transport_send_packet( server, 0, to_client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            sent_to_client++;
        }
        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            rocketnet_check( memcmp( payload, to_server, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == 0 );
            server_received++;
        }

        static uint8_t packet[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            rocketnet_check( memcmp( packet, to_client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == 0 );
            client_received++;
        }

        if ( server_received >= count && client_received >= count )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == count );
    rocketnet_check( client_received == count );

    // one datagram per payload: nothing fragmented on either side
    rocketnet_check( rocketnet_counter_value( rocketnet_server_transport_counters( server ), ROCKETNET_COUNTER_FRAGMENT_RECEIVED ) == 0 );
    rocketnet_check( rocketnet_counter_value( rocketnet_client_transport_counters( client ), ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED ) == 0 );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );
}

void rocketnet_test_max_size_packet_crosses_a_1500_mtu_link_ipv4()
{
    rocketnet_test_max_size_packet_crosses( "ROCKETNET_TEST_MTU_SERVER_IPV4", "127.0.0.1", false );
}

void rocketnet_test_max_size_packet_crosses_a_1500_mtu_link_ipv6()
{
    rocketnet_test_max_size_packet_crosses( "ROCKETNET_TEST_MTU_SERVER_IPV6", "[::1]", true );
}

// The AF_XDP transport carries both address families end to end, and the
// wire is the judge. A veth pair at MTU 1500: the near end in this network
// namespace, where the server runs pinned to the xdp transport (a fall to gso or
// portable fails the case by name), and the far end in its own namespace,
// where the client runs on the portable transport. One case per family: the
// largest payload the transport sends crosses ROCKETNET_XDP_VETH_COUNT times
// each way, each one byte-exact on the far side. A capture on the far end
// counts every datagram the client sent to the server, and the kernel
// filter's REDIRECTED counter must equal it, with every refusal counter zero;
// the transport's PACKETS_RECEIVED must equal it too, so userspace dropped
// nothing the program redirected. The case prints one tcpdump -vv line per
// direction and the counters; for IPv6, tcpdump must find the server's UDP
// checksum correct on every server datagram.
//
// The case attaches an XDP program and makes namespaces and a veth pair, so it
// runs only as root inside a virtme-ng guest (DRIVERS.md has the vng line),
// or as root on a machine whose operator opts in with
// ROCKETNET_TEST_XDP_VETH=1 (rocketnet_test_xdp_veth_runs_here); anywhere else
// it is not in the suite. Everything it
// creates is removed on every exit path, a failed check included.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

#include "rocketnet_server_socket.h"
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <string>
#include <vector>

#define ROCKETNET_XDP_VETH_NETNS        "rnxdpfam"
#define ROCKETNET_XDP_VETH_NEAR         "rnxdpfam0"
#define ROCKETNET_XDP_VETH_FAR          "rnxdpfam1"
#define ROCKETNET_XDP_VETH_PORT         40411
#define ROCKETNET_XDP_VETH_COUNT        20

static pid_t rocketnet_xdp_veth_capture_pid = -1;
static struct rocketnet_server_transport_t * rocketnet_xdp_veth_server = NULL;
static bool rocketnet_xdp_veth_made = false;
static char rocketnet_xdp_veth_pcap[64] = "";

static void rocketnet_xdp_veth_teardown()
{
    if ( rocketnet_xdp_veth_capture_pid > 0 )
    {
        kill( rocketnet_xdp_veth_capture_pid, SIGINT );
        waitpid( rocketnet_xdp_veth_capture_pid, NULL, 0 );
        rocketnet_xdp_veth_capture_pid = -1;
    }
    if ( rocketnet_xdp_veth_server )
    {
        rn_native_server_transport_stop_and_destroy( rocketnet_xdp_veth_server );
        rocketnet_xdp_veth_server = NULL;
    }
    if ( rocketnet_xdp_veth_made )
    {
        // deleting the namespace takes the far end, and with it the pair
        if ( system( "ip netns del " ROCKETNET_XDP_VETH_NETNS " 2>/dev/null; ip link del " ROCKETNET_XDP_VETH_NEAR " 2>/dev/null" ) != 0 )
        {
            // already gone
        }
        rocketnet_xdp_veth_made = false;
    }
    if ( rocketnet_xdp_veth_pcap[0] != '\0' )
    {
        unlink( rocketnet_xdp_veth_pcap );
        rocketnet_xdp_veth_pcap[0] = '\0';
    }
    rocketnet_server_socket_forbid_commit();
}

static void rocketnet_xdp_veth_run( const char * command )
{
    const int result = system( command );
    if ( result != 0 )
        printf( "command failed (%d): %s\n", result, command );
    rocketnet_check( result == 0 );
}

// a command's output, one string per packet: tcpdump -vv continues a packet
// on indented lines, which are joined to the line before

static std::vector<std::string> rocketnet_xdp_veth_lines( const char * command )
{
    std::vector<std::string> lines;
    FILE * file = popen( command, "r" );
    rocketnet_check( file );
    char buffer[2048];
    while ( fgets( buffer, sizeof(buffer), file ) )
    {
        buffer[strcspn( buffer, "\r\n" )] = '\0';
        if ( ( buffer[0] == ' ' || buffer[0] == '\t' ) && !lines.empty() )
        {
            const char * text = buffer;
            while ( *text == ' ' || *text == '\t' )
                text++;
            lines.back() += " ";
            lines.back() += text;
        }
        else
        {
            lines.push_back( buffer );
        }
    }
    pclose( file );
    return lines;
}

static bool rocketnet_xdp_veth_inside_virtme()
{
    FILE * file = fopen( "/proc/cmdline", "r" );
    if ( !file )
        return false;
    char line[4096];
    const bool read = fgets( line, sizeof(line), file ) != NULL;
    fclose( file );
    return read && strstr( line, "virtme" ) != NULL;
}

// the cases make a namespace and a veth pair and attach XDP, so they run
// only as root inside a virtme-ng guest, or as root with
// ROCKETNET_TEST_XDP_VETH=1
bool rocketnet_test_xdp_veth_runs_here()
{
    char opt_in[256];
    const bool opted_in = rocketnet_test_getenv( "ROCKETNET_TEST_XDP_VETH", opt_in, sizeof( opt_in ) ) && strcmp( opt_in, "1" ) == 0;
    return geteuid() == 0 && ( opted_in || rocketnet_xdp_veth_inside_virtme() );
}

static bool rocketnet_xdp_veth_capture_present = false;

// the pair, the capture and the xdp server on its near end, shared by the
// veth cases; server_address receives the address the client dials
struct rocketnet_server_transport_t * rocketnet_xdp_veth_start( bool ipv6, const char * case_name, char * server_address, size_t server_address_bytes )
{

    atexit( rocketnet_xdp_veth_teardown );

    // the boot tier the xdp pre-flight verifies; a guest boots with an empty
    // bpffs mount point

    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) 0xcafe4a11 )
        rocketnet_xdp_veth_run( "mount -t bpf bpf /sys/fs/bpf" );

    if ( system( "ip netns del " ROCKETNET_XDP_VETH_NETNS " 2>/dev/null; ip link del " ROCKETNET_XDP_VETH_NEAR " 2>/dev/null" ) != 0 )
    {
        // nothing left from an earlier run
    }

    // the pair: MTU 1500 both ends. Only the family under test is addressed.
    // IPv6 takes no link-local address and no duplicate address detection,
    // so neighbor discovery runs between the two global addresses and the
    // case starts at once.

    rocketnet_xdp_veth_made = true;
    rocketnet_xdp_veth_run( "ip netns add " ROCKETNET_XDP_VETH_NETNS );
    int veth_queues = 1;
    {
        char text[32];
        if ( rocketnet_test_getenv( "ROCKETNET_TEST_XDP_VETH_QUEUES", text, sizeof( text ) ) && atoi( text ) > 1 )
            veth_queues = atoi( text );
    }
    {
        char command[256];
        snprintf( command, sizeof(command), "ip link add " ROCKETNET_XDP_VETH_NEAR " mtu 1500 numtxqueues %d numrxqueues %d type veth peer name " ROCKETNET_XDP_VETH_FAR " mtu 1500 numtxqueues %d numrxqueues %d", veth_queues, veth_queues, veth_queues, veth_queues );
        rocketnet_xdp_veth_run( command );
    }
    rocketnet_xdp_veth_run( "ip link set " ROCKETNET_XDP_VETH_FAR " netns " ROCKETNET_XDP_VETH_NETNS );
    if ( ipv6 )
    {
        rocketnet_xdp_veth_run( "sysctl -qw net.ipv6.conf." ROCKETNET_XDP_VETH_NEAR ".addr_gen_mode=1" );
        rocketnet_xdp_veth_run( "ip netns exec " ROCKETNET_XDP_VETH_NETNS " sysctl -qw net.ipv6.conf." ROCKETNET_XDP_VETH_FAR ".addr_gen_mode=1" );
        rocketnet_xdp_veth_run( "ip addr add 2001:db8:77::1/64 dev " ROCKETNET_XDP_VETH_NEAR " nodad" );
        rocketnet_xdp_veth_run( "ip -n " ROCKETNET_XDP_VETH_NETNS " addr add 2001:db8:77::2/64 dev " ROCKETNET_XDP_VETH_FAR " nodad" );
    }
    else
    {
        rocketnet_xdp_veth_run( "ip addr add 10.77.2.1/24 dev " ROCKETNET_XDP_VETH_NEAR );
        rocketnet_xdp_veth_run( "ip -n " ROCKETNET_XDP_VETH_NETNS " addr add 10.77.2.2/24 dev " ROCKETNET_XDP_VETH_FAR );
    }
    rocketnet_xdp_veth_run( "ip link set " ROCKETNET_XDP_VETH_NEAR " up" );

    // the busy-poll pair the pre-flight verifies (R5), as a provisioned
    // interface carries it
    rocketnet_xdp_veth_run( "echo 2 > /sys/class/net/" ROCKETNET_XDP_VETH_NEAR "/napi_defer_hard_irqs" );
    rocketnet_xdp_veth_run( "echo 200000 > /sys/class/net/" ROCKETNET_XDP_VETH_NEAR "/gro_flush_timeout" );
    rocketnet_xdp_veth_run( "ip -n " ROCKETNET_XDP_VETH_NETNS " link set lo up" );
    rocketnet_xdp_veth_run( "ip -n " ROCKETNET_XDP_VETH_NETNS " link set " ROCKETNET_XDP_VETH_FAR " up" );

    // the client's checksums are filled in software, so the capture on the
    // far end judges both directions' checksums rather than reading a
    // checksum the offload would fill later

    if ( system( "command -v ethtool >/dev/null 2>&1" ) == 0 )
        rocketnet_xdp_veth_run( "ip netns exec " ROCKETNET_XDP_VETH_NETNS " ethtool -K " ROCKETNET_XDP_VETH_FAR " tx off >/dev/null" );

    const char * server_host = ipv6 ? "2001:db8:77::1" : "10.77.2.1";
    if ( ipv6 )
        snprintf( server_address, server_address_bytes, "[%s]:%d", server_host, ROCKETNET_XDP_VETH_PORT );
    else
        snprintf( server_address, server_address_bytes, "%s:%d", server_host, ROCKETNET_XDP_VETH_PORT );

    // the capture: every UDP datagram on the server's port, written to a file
    // and read back when the case ends

    const bool capture_tool_present = system( "command -v tcpdump >/dev/null 2>&1" ) == 0;
    rocketnet_xdp_veth_capture_present = capture_tool_present;
    if ( capture_tool_present )
    {
        snprintf( rocketnet_xdp_veth_pcap, sizeof(rocketnet_xdp_veth_pcap), "/tmp/rocketnet_xdp_veth_XXXXXX" );
        const int pcap_fd = mkstemp( rocketnet_xdp_veth_pcap );
        rocketnet_check( pcap_fd >= 0 );
        close( pcap_fd );

        const pid_t pid = fork();
        rocketnet_check( pid >= 0 );
        if ( pid == 0 )
        {
            const int null_fd = open( "/dev/null", O_WRONLY );
            if ( null_fd >= 0 )
            {
                dup2( null_fd, 1 );
                dup2( null_fd, 2 );
            }
            char filter[64];
            snprintf( filter, sizeof(filter), "udp port %d", ROCKETNET_XDP_VETH_PORT );
            // --immediate-mode: each packet reaches the file as it arrives.
            // Without it, libpcap's TPACKET_V3 ring holds packets in a block
            // until the block's timeout retires it, and the packets of a block
            // not yet retired when SIGINT stops tcpdump never reach the file.
            execlp( "ip", "ip", "netns", "exec", ROCKETNET_XDP_VETH_NETNS, "tcpdump", "--immediate-mode", "-i", ROCKETNET_XDP_VETH_FAR, "-n", "-U", "-s", "0", "-w", rocketnet_xdp_veth_pcap, filter, (char*) NULL );
            _exit( 127 );
        }
        rocketnet_xdp_veth_capture_pid = pid;

        // tcpdump -U -w writes the 24-byte pcap global header once its packet
        // socket is open and filtered, so the exchange starts only after the
        // file holds it, and a capture that never opens fails by name.
        const double capture_wait_start = rocketnet_platform_time();
        bool capture_ready = false;
        bool capture_exited = false;
        while ( rocketnet_platform_time() - capture_wait_start < 10.0 )
        {
            struct stat pcap_stat;
            if ( stat( rocketnet_xdp_veth_pcap, &pcap_stat ) == 0 && pcap_stat.st_size >= 24 )
            {
                capture_ready = true;
                break;
            }
            if ( waitpid( pid, NULL, WNOHANG ) == pid )
            {
                rocketnet_xdp_veth_capture_pid = -1;
                capture_exited = true;
                break;
            }
            rocketnet_platform_sleep( 0.01 );
        }
        if ( !capture_ready )
            rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: FAILED: the capture never became ready: tcpdump %s before it wrote the pcap header", case_name, capture_exited ? "exited" : "took more than 10 seconds" );
        else
            rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: capture ready after %.3f seconds", case_name, rocketnet_platform_time() - capture_wait_start );
        rocketnet_check( capture_ready );
    }
    else
    {
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: tcpdump is not installed: no capture; the traffic and the counters run, then the case fails by name", case_name );
    }

    // the server: pinned to xdp, one queue, on the near end

    rocketnet_server_socket_allow_commit();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.num_xdp_queues = veth_queues;
    server_config.num_os_cpus = 1;
    snprintf( server_config.transport, sizeof(server_config.transport), "xdp" );

    // the create at info level, so every pre-flight line is in the output
    const enum rocketnet_log_level_t log_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, server_address, &server_config );
    rocketnet_log_level( log_level );
    if ( !server || strcmp( rocketnet_server_transport_io_name( server ), "xdp" ) != 0 )
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: FAILED: the server did not start on the xdp transport (%s)", case_name, server ? rocketnet_server_transport_io_name( server ) : "create refused" );
    rocketnet_xdp_veth_server = server;
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( server ), "xdp" ) == 0 );

    return server;
}

void rocketnet_xdp_veth_note_server( struct rocketnet_server_transport_t * server )
{
    rocketnet_xdp_veth_server = server;
}

// If destroyed is the recorded server, drop that record first. Then remove
// the pair, and free a recorded server that is still set. The hard-killed
// cell records NULL before it frees its own server and passes NULL here.
void rocketnet_xdp_veth_pair_down( struct rocketnet_server_transport_t * destroyed )
{
    if ( rocketnet_xdp_veth_server == destroyed )
        rocketnet_xdp_veth_server = NULL;
    rocketnet_xdp_veth_teardown();
}

const char * rocketnet_xdp_veth_netns_path()
{
    return "/var/run/netns/" ROCKETNET_XDP_VETH_NETNS;
}

void rocketnet_xdp_veth_copy_far_mac( uint8_t * mac )
{
    // setns does not switch the mounted sysfs, so the far end's address file
    // is not visible that way. ip netns exec mounts a sysfs for that namespace.
    const std::vector<std::string> lines = rocketnet_xdp_veth_lines(
        "ip netns exec " ROCKETNET_XDP_VETH_NETNS " cat /sys/class/net/" ROCKETNET_XDP_VETH_FAR "/address" );
    rocketnet_check( lines.size() == 1 );
    unsigned int octet[6] = { 0, 0, 0, 0, 0, 0 };
    const int got = sscanf( lines[0].c_str(), "%x:%x:%x:%x:%x:%x", &octet[0], &octet[1], &octet[2], &octet[3], &octet[4], &octet[5] );
    rocketnet_check( got == 6 );
    for ( int i = 0; i < 6; i++ )
    {
        rocketnet_check( octet[i] <= 0xffu );
        mac[i] = (uint8_t) octet[i];
    }
}

// the portable client, its socket opened inside the far end's namespace
static struct rocketnet_client_transport_t * rocketnet_xdp_veth_client( const char * server_address )
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "portable" );

    const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
    const int far_ns = open( "/var/run/netns/" ROCKETNET_XDP_VETH_NETNS, O_RDONLY );
    rocketnet_check( self_ns >= 0 && far_ns >= 0 );
    rocketnet_check( setns( far_ns, CLONE_NEWNET ) == 0 );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( setns( self_ns, CLONE_NEWNET ) == 0 );
    close( self_ns );
    close( far_ns );
    rocketnet_check( client );
    return client;
}

static void rocketnet_xdp_max_size_both_ways( bool ipv6, const char * case_name )
{
    char server_address[128];
    struct rocketnet_server_transport_t * server = rocketnet_xdp_veth_start( ipv6, case_name, server_address, sizeof(server_address) );
    struct rocketnet_client_transport_t * client = rocketnet_xdp_veth_client( server_address );
    const bool capture_tool_present = rocketnet_xdp_veth_capture_present;
    const char * server_host = ipv6 ? "2001:db8:77::1" : "10.77.2.1";

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    static uint8_t to_server[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    static uint8_t to_client[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    for ( int i = 0; i < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; i++ )
    {
        to_server[i] = (uint8_t) ( i * 7 + 1 );
        to_client[i] = (uint8_t) ( i * 11 + 3 );
    }

    // a send the server refuses for want of staging is counted in
    // ROCKETNET_COUNTER_SEND_RING_FULL and never reaches the wire: the xdp
    // transport has ROCKETNET_XDP_NUM_BUFFERS staging generations
    // (sdk/rocketnet_constants.h:49), each flush publishes one
    // (sdk/rocketnet_server_socket_linux.cpp:2249), and while its send
    // thread has retired neither, a start is refused
    // (sdk/rocketnet_server_socket_linux.cpp:2118) and counted
    // (sdk/rocketnet_server_transport.cpp:2153). This loop flushes every
    // iteration, so a send thread that falls two flushes behind refuses the
    // next send. A send counts as taken when the counter did not move, and
    // a refused send is sent again, so a send dropped without a count fails
    // the client check below. ring_full_during also counts frames that the
    // server update refused.

    const struct rocketnet_counters_t * send_counters = rocketnet_server_transport_counters( server );
    const uint64_t ring_full_before = rocketnet_counter_value( send_counters, ROCKETNET_COUNTER_SEND_RING_FULL );
    const int count = ROCKETNET_XDP_VETH_COUNT;
    int server_received = 0, client_received = 0, sent_to_server = 0, sent_to_client = 0, refused_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        if ( sent_to_server < count )
        {
            rocketnet_client_transport_send_packet( client, to_server, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            sent_to_server++;
        }
        if ( sent_to_client < count )
        {
            const uint64_t ring_full = rocketnet_counter_value( rocketnet_server_transport_counters( server ), ROCKETNET_COUNTER_SEND_RING_FULL );
            rocketnet_server_transport_send_packet( server, 0, to_client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            if ( rocketnet_counter_value( rocketnet_server_transport_counters( server ), ROCKETNET_COUNTER_SEND_RING_FULL ) == ring_full )
                sent_to_client++;
            else
                refused_to_client++;
        }
        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            rocketnet_check( memcmp( payload, to_server, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == 0 );
            server_received++;
        }

        static uint8_t packet[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            rocketnet_check( memcmp( packet, to_client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == 0 );
            client_received++;
        }

        if ( server_received >= count && client_received >= count )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    const uint64_t ring_full_during = rocketnet_counter_value( rocketnet_server_transport_counters( server ), ROCKETNET_COUNTER_SEND_RING_FULL ) - ring_full_before;
    rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: server opened %d of %d, client opened %d of %d, server sends refused for staging %d (send ring full counted %llu)", case_name, server_received, count, client_received, count, refused_to_client, (unsigned long long) ring_full_during );
    rocketnet_check( server_received == count );
    rocketnet_check( client_received == count );

    // the client goes first; the server then drains what was in flight, so
    // every frame the program redirected has reached the transport before
    // the counters are read

    rn_native_client_transport_stop_and_destroy( client );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 0.5 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    if ( rocketnet_xdp_veth_capture_pid > 0 )
    {
        rocketnet_platform_sleep( 0.25 );
        kill( rocketnet_xdp_veth_capture_pid, SIGINT );
        waitpid( rocketnet_xdp_veth_capture_pid, NULL, 0 );
        rocketnet_xdp_veth_capture_pid = -1;
    }

    // the counters: the kernel filter's, surfaced on the transport, and the
    // transport's own receive count

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counters( server );
    struct counter_row_t { int index; const char * name; };
    static const counter_row_t rows[] =
    {
        { ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED,            "xdp redirected" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL,     "xdp dropped_too_small" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE,     "xdp dropped_too_large" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER,  "xdp dropped_wrong_filter" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE,  "xdp dropped_invalid_type" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH,   "xdp dropped_wrong_epoch" },
        { ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED,      "xdp dropped_replayed" },
        { ROCKETNET_COUNTER_XDP_KERNEL_PASSED_IPV6_EXTENSION, "xdp passed_ipv6_extension" },
        { ROCKETNET_COUNTER_PACKETS_RECEIVED,                 "transport packets_received" },
    };
    for ( const counter_row_t & row : rows )
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: %s = %llu", case_name, row.name, (unsigned long long) rocketnet_counter_value( counters, row.index ) );

    const uint64_t redirected = rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED );
    const uint64_t packets_received = rocketnet_counter_value( counters, ROCKETNET_COUNTER_PACKETS_RECEIVED );
    rocketnet_check( redirected >= (uint64_t) count );
    rocketnet_check( redirected == packets_received );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL ) == 0 );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE ) == 0 );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER ) == 0 );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE ) == 0 );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH ) == 0 );
    rocketnet_check( rocketnet_counter_value( counters, ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED ) == 0 );

    // the wire: every datagram on the server's port, by direction

    if ( capture_tool_present )
    {
        char command[256];
        snprintf( command, sizeof(command), "tcpdump -vv -n -r %s 2>/dev/null", rocketnet_xdp_veth_pcap );
        const std::vector<std::string> lines = rocketnet_xdp_veth_lines( command );
        char from_server[96], to_server_text[96];
        snprintf( from_server, sizeof(from_server), " %s.%d > ", server_host, ROCKETNET_XDP_VETH_PORT );
        snprintf( to_server_text, sizeof(to_server_text), " > %s.%d: ", server_host, ROCKETNET_XDP_VETH_PORT );
        const std::string * server_to_client = NULL;
        const std::string * client_to_server = NULL;
        int client_sent = 0;
        int server_sent = 0;
        int bad_checksums = 0;
        int server_checksums_ok = 0;
        size_t largest_from_server = 0, largest_to_server = 0;
        for ( const std::string & line : lines )
        {
            const size_t length_at = line.rfind( "length " );
            const size_t length = length_at == std::string::npos ? 0 : (size_t) atoi( line.c_str() + length_at + 7 );
            if ( line.find( "bad udp cksum" ) != std::string::npos )
                bad_checksums++;
            if ( line.find( from_server ) != std::string::npos )
            {
                server_sent++;
                if ( line.find( "udp sum ok" ) != std::string::npos )
                    server_checksums_ok++;
                if ( length >= largest_from_server )
                {
                    largest_from_server = length;
                    server_to_client = &line;
                }
            }
            else if ( line.find( to_server_text ) != std::string::npos )
            {
                client_sent++;
                if ( length >= largest_to_server )
                {
                    largest_to_server = length;
                    client_to_server = &line;
                }
            }
        }
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: capture: client sent %d datagrams to the server, the server sent %d, largest UDP payload %d to the server and %d from it", case_name, client_sent, server_sent, (int) largest_to_server, (int) largest_from_server );
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: client to server: %s", case_name, client_to_server ? client_to_server->c_str() : "(none captured)" );
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: server to client: %s", case_name, server_to_client ? server_to_client->c_str() : "(none captured)" );
        rocketnet_check( client_to_server && server_to_client );
        rocketnet_check( bad_checksums == 0 );
        rocketnet_check( redirected == (uint64_t) client_sent );
        if ( ipv6 )
            rocketnet_check( server_checksums_ok == server_sent );
    }

    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rn_native_server_transport_stop_and_destroy( server );
    rocketnet_xdp_veth_server = NULL;

    rocketnet_xdp_veth_teardown();

    if ( !capture_tool_present )
    {
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: FAILED: tcpdump is not installed, so no capture judges the wire", case_name );
        rocketnet_check( capture_tool_present && "tcpdump is not installed" );
    }
}

// The keep-alive that connects a client leaves ahead of the payloads the
// server sends it in the same update. The pair has no gateway, so this is the
// end-to-end guard; the hulk inversion (the reply reserved before the
// client's MAC was learned took the gateway's hairpin) is pinned by test_xdp's
// test_xdp_transport_reply_mac_refreshed_at_finish. The application sends its first
// messages to a client the update just connected; the client processes
// payloads only once connected (as netcode.io's client), so a payload that
// beat the keep-alive is refused WRONG_STATE and lost. On hulk the xdp
// server sent CONFIG and ARM, then the keep-alive ~200 us later, in every
// trace; the gso server sent the keep-alive first.
static void rocketnet_xdp_keep_alive_before_first_payloads( bool ipv6, const char * case_name )
{
    char server_address[128];
    struct rocketnet_server_transport_t * server = rocketnet_xdp_veth_start( ipv6, case_name, server_address, sizeof(server_address) );
    struct rocketnet_client_transport_t * client = rocketnet_xdp_veth_client( server_address );

    static const uint8_t first[2][16] = { { 0xC0, 0x4F, 0x16 }, { 0xA4, 0x40 } };
    bool sent = false;
    int received = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        // the application's first messages, the update that connects the client
        if ( !sent && rocketnet_server_transport_num_connected_clients( server ) == 1 )
        {
            rocketnet_server_transport_send_packet( server, 0, first[0], (int) sizeof(first[0]) );
            rocketnet_server_transport_send_packet( server, 0, first[1], (int) sizeof(first[1]) );
            sent = true;
        }
        rocketnet_server_transport_send_packets( server );

        static uint8_t packet[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == (int) sizeof(first[0]) );
            rocketnet_check( memcmp( packet, first[received < 2 ? received : 1], sizeof(first[0]) ) == 0 );
            received++;
        }
        if ( received == 2 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }

    const struct rocketnet_counters_t * counters = rocketnet_client_transport_counters( client );
    const uint64_t early = rocketnet_counter_value( counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE );
    if ( !sent || received != 2 || early != 0 )
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s: FAILED: sent %d, client received %d of 2, payloads ahead of the keep-alive %llu", case_name, sent ? 1 : 0, received, (unsigned long long) early );
    rocketnet_check( sent );
    rocketnet_check( early == 0 );
    rocketnet_check( received == 2 );

    rn_native_client_transport_stop_and_destroy( client );
    rocketnet_xdp_veth_teardown();
}

void rocketnet_test_xdp_keep_alive_before_first_payloads_ipv4()
{
    rocketnet_xdp_keep_alive_before_first_payloads( false, "test_xdp_transport_keep_alive_before_first_payloads_ipv4" );
}

void rocketnet_test_xdp_keep_alive_before_first_payloads_ipv6()
{
    rocketnet_xdp_keep_alive_before_first_payloads( true, "test_xdp_transport_keep_alive_before_first_payloads_ipv6" );
}

void rocketnet_test_xdp_max_size_both_ways_ipv4()
{
    rocketnet_xdp_max_size_both_ways( false, "test_xdp_transport_max_size_both_ways_ipv4" );
}

void rocketnet_test_xdp_max_size_both_ways_ipv6()
{
    rocketnet_xdp_max_size_both_ways( true, "test_xdp_transport_max_size_both_ways_ipv6" );
}

// the three hard-killed kinds on the xdp server cell. They live in the static
// table, not the generated hard-killed count, so a host where the veth gate
// is false omits them and an explicit --run prints the runs_here refusal.
extern void rocketnet_posix_hk_run_xdp_cell( int kind, bool ipv6 );

#if ROCKETNET_DEV
static void rocketnet_test_posix_hard_killed_client_xdp_ipv4_client_portable() { rocketnet_posix_hk_run_xdp_cell( 0, false ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static void rocketnet_test_posix_hard_killed_client_xdp_ipv6_client_portable() { rocketnet_posix_hk_run_xdp_cell( 0, true ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static void rocketnet_test_posix_send_to_closed_port_xdp_ipv4_client_portable() { rocketnet_posix_hk_run_xdp_cell( 1, false ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static void rocketnet_test_posix_send_to_closed_port_xdp_ipv6_client_portable() { rocketnet_posix_hk_run_xdp_cell( 1, true ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static void rocketnet_test_posix_client_server_gone_xdp_ipv4_client_portable() { rocketnet_posix_hk_run_xdp_cell( 2, false ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static void rocketnet_test_posix_client_server_gone_xdp_ipv6_client_portable() { rocketnet_posix_hk_run_xdp_cell( 2, true ); }
#endif // #if ROCKETNET_DEV

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP


// Cumulative wire totals over the in-process loopback transport: no socket is
// opened, and every datagram the transport carries is counted on both ends,
// handshake and keep-alives included. Because the loopback client transport uses
// the transport's polling receive thread (loopback_client_operations has no
// wait/wake hooks and rocketnet_client_transport_config_t has no calling-thread
// receive mode), these two cases belong in the functional tier where pumping
// yields to the receive thread. Both ends use [::1]: a dual-stack client binds
// as IPv6 even for an IPv4 server and the loopback transport reports that bind as
// the source, so one family keeps the cross-side totals comparable.

struct wire_totals_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

static void wire_totals_client( struct rocketnet_client_transport_t * client, wire_totals_t * totals )
{
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( client, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_server( struct rocketnet_server_transport_t * server, wire_totals_t * totals )
{
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( server, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_slot( struct rocketnet_server_transport_t * server, int slot, wire_totals_t * totals )
{
    struct rocketnet_server_transport_client_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_client_stats( server, slot, &stats );
    totals->packets_sent = stats.packets_sent;
    totals->packets_received = stats.packets_received;
    totals->bytes_sent = stats.bytes_sent;
    totals->bytes_received = stats.bytes_received;
}

static void wire_totals_pump( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time, int steps )
{
    for ( int i = 0; i < steps; i++ )
    {
        rocketnet_client_transport_update( client, *time );
        rocketnet_server_transport_update( server, *time );
        rocketnet_platform_sleep( 0.001 );
        *time += 0.001;
    }
}

static bool wire_totals_connect( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time )
{
    for ( int i = 0; i < 5000; i++ )
    {
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
        {
            wire_totals_pump( client, server, time, 4 );
            return true;
        }
        wire_totals_pump( client, server, time, 1 );
    }
    return false;
}

// drain until both directions' totals match: the server's receive is
// synchronous in its update, but the client's loopback receive runs on the
// transport's polling thread, so the client's received totals lag the
// server's sent totals until that thread has run. pumping with a 1 ms sleep
// per step lets the receive thread catch up, then the cross-side checks hold.

static bool wire_totals_drain( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time )
{
    for ( int i = 0; i < 5000; i++ )
    {
        wire_totals_pump( client, server, time, 1 );
        wire_totals_t c, s;
        wire_totals_client( client, &c );
        wire_totals_server( server, &s );
        if ( s.packets_sent == c.packets_received && c.packets_sent == s.packets_received )
            return true;
    }
    return false;
}

// the drain for a later connection: the server's lifetime totals are larger
// than this client's, so convergence is judged on the server's totals since
// the baseline taken before this client existed.

static bool wire_totals_drain_since( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server,
                                     const wire_totals_t * server_before, double * time )
{
    for ( int i = 0; i < 5000; i++ )
    {
        wire_totals_pump( client, server, time, 1 );
        wire_totals_t c, s;
        wire_totals_client( client, &c );
        wire_totals_server( server, &s );
        if ( s.packets_sent - server_before->packets_sent == c.packets_received &&
             s.packets_received - server_before->packets_received == c.packets_sent )
            return true;
    }
    return false;
}

enum { WIRE_TOTALS_GAME_CLOCK_EPOCH = 1700000000ULL };

static struct rocketnet_server_transport_t * wire_totals_server_create( const char * public_address, double time )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_functional_server_config( &config );
    config.max_clients = 1;
    config.game_clock_epoch = WIRE_TOTALS_GAME_CLOCK_EPOCH;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, public_address, &config, time, &status );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    return server;
}

static struct rocketnet_client_transport_t * wire_totals_client_create( const char * address, uint64_t client_id, double time )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( rocketnet_functional_write_token( address, client_id, rocketnet_functional_token_timestamp( WIRE_TOTALS_GAME_CLOCK_EPOCH, time ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) != ROCKETNET_STATUS_OK )
        return NULL;

    struct rocketnet_client_transport_config_t config;
    rocketnet_functional_client_default_config( &config );
    config.game_clock_epoch = WIRE_TOTALS_GAME_CLOCK_EPOCH;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    return rocketnet_client_transport_create( NULL, token, &config, time );
}

static void wire_totals_send_payloads( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, double * time )
{
    uint8_t payload[32];
    for ( int round = 0; round < 3; round++ )
    {
        memset( payload, round + 1, sizeof( payload ) );
        for ( int i = 0; i < 4; i++ )
        {
            rocketnet_client_transport_send_packet( client, payload, (int) sizeof( payload ) );
            rocketnet_server_transport_send_packet( server, 0, payload, (int) sizeof( payload ) );
        }
        rocketnet_server_transport_send_packets( server );
        wire_totals_pump( client, server, time, 4 );
    }

    const int num_payloads = rocketnet_server_transport_num_payloads( server );
    for ( int i = 0; i < num_payloads; i++ )
    {
        int payload_client_index = -1;
        int payload_bytes = 0;
        rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
    }
    uint8_t received[64];
    while ( rocketnet_client_transport_receive_packet( client, received, (int) sizeof( received ) ) > 0 ) {}
}

void wire_totals_count_every_packet_and_byte()
{
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    struct rocketnet_server_transport_t * server = wire_totals_server_create( "[::1]:0", 1000.0 );
    rocketnet_check( server != NULL );

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const int server_port = rocketnet_server_transport_port( server );
    snprintf( address, sizeof( address ), "[::1]:%d", server_port );

    double time = 1000.0;
    struct rocketnet_client_transport_t * client = wire_totals_client_create( address, 0x5701ULL, time );
    rocketnet_check( client != NULL );
    rocketnet_check( wire_totals_connect( client, server, &time ) );

    wire_totals_t client_totals, server_totals, slot_totals;
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );
    wire_totals_slot( server, 0, &slot_totals );

    rocketnet_check( client_totals.packets_sent > 0 );
    rocketnet_check( client_totals.packets_received > 0 );

    // a slot's totals start at its assignment: the handshake before it counts
    // in the whole-server totals only (the server keeps no per-source totals
    // for traffic it has not authenticated). settle, then take the handshake
    // share, which is fixed from here on.
    rocketnet_check( wire_totals_drain( client, server, &time ) );
    wire_totals_server( server, &server_totals );
    wire_totals_slot( server, 0, &slot_totals );
    const wire_totals_t handshake = { server_totals.packets_sent - slot_totals.packets_sent,
                                      server_totals.packets_received - slot_totals.packets_received,
                                      server_totals.bytes_sent - slot_totals.bytes_sent,
                                      server_totals.bytes_received - slot_totals.bytes_received };
    rocketnet_check( handshake.packets_received > 0 && handshake.packets_sent > 0 );
    rocketnet_check( handshake.bytes_received > 0 && handshake.bytes_sent > 0 );

    wire_totals_send_payloads( client, server, &time );
    rocketnet_check( wire_totals_drain( client, server, &time ) );
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );
    wire_totals_slot( server, 0, &slot_totals );

    rocketnet_check( client_totals.packets_sent == server_totals.packets_received );
    rocketnet_check( client_totals.bytes_sent == server_totals.bytes_received );
    rocketnet_check( server_totals.packets_sent == client_totals.packets_received );
    rocketnet_check( server_totals.bytes_sent == client_totals.bytes_received );
    rocketnet_check( server_totals.packets_sent == slot_totals.packets_sent + handshake.packets_sent );
    rocketnet_check( server_totals.bytes_sent == slot_totals.bytes_sent + handshake.bytes_sent );
    rocketnet_check( server_totals.packets_received == slot_totals.packets_received + handshake.packets_received );
    rocketnet_check( server_totals.bytes_received == slot_totals.bytes_received + handshake.bytes_received );

    const uint64_t packets_before_disconnect = client_totals.packets_sent;
    rocketnet_client_transport_disconnect( client );
    rocketnet_check( wire_totals_drain( client, server, &time ) );
    wire_totals_client( client, &client_totals );
    wire_totals_server( server, &server_totals );

    rocketnet_check( client_totals.packets_sent > packets_before_disconnect );
    rocketnet_check( client_totals.packets_sent == server_totals.packets_received );
    rocketnet_check( client_totals.bytes_sent == server_totals.bytes_received );
    rocketnet_check( server_totals.packets_sent == client_totals.packets_received );
    rocketnet_check( server_totals.bytes_sent == client_totals.bytes_received );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void wire_totals_reset_per_connection_not_per_server()
{
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    struct rocketnet_server_transport_t * server = wire_totals_server_create( "[::1]:0", 1000.0 );
    rocketnet_check( server != NULL );

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const int server_port = rocketnet_server_transport_port( server );
    snprintf( address, sizeof( address ), "[::1]:%d", server_port );

    double time = 1000.0;
    struct rocketnet_client_transport_t * first = wire_totals_client_create( address, 0x5702ULL, time );
    rocketnet_check( first != NULL );
    rocketnet_check( wire_totals_connect( first, server, &time ) );
    wire_totals_send_payloads( first, server, &time );

    wire_totals_t first_slot, first_server;
    wire_totals_slot( server, 0, &first_slot );
    wire_totals_server( server, &first_server );
    rocketnet_check( first_slot.packets_sent > 0 );
    rocketnet_check( first_slot.packets_received > 0 );

    rocketnet_client_transport_disconnect( first );
    for ( int i = 0; i < 200 && rocketnet_server_transport_num_connected_clients( server ) > 0; i++ )
        wire_totals_pump( first, server, &time, 1 );

    // the first client's queued sends may still be in flight after the
    // disconnect: wait on the totals agreeing so a late packet cannot land
    // after the baseline below
    rocketnet_check( wire_totals_drain( first, server, &time ) );

    wire_totals_t free_slot;
    wire_totals_slot( server, 0, &free_slot );
    rocketnet_check( free_slot.packets_sent == 0 );
    rocketnet_check( free_slot.packets_received == 0 );
    rocketnet_check( free_slot.bytes_sent == 0 );
    rocketnet_check( free_slot.bytes_received == 0 );

    wire_totals_t before_second;
    wire_totals_server( server, &before_second );
    struct rocketnet_client_transport_t * second = wire_totals_client_create( address, 0x5703ULL, time );
    rocketnet_check( second != NULL );
    rocketnet_check( wire_totals_connect( second, server, &time ) );
    rocketnet_check( wire_totals_drain_since( second, server, &before_second, &time ) );

    wire_totals_t second_slot, second_server, second_client;
    wire_totals_slot( server, 0, &second_slot );
    wire_totals_server( server, &second_server );
    wire_totals_client( second, &second_client );

    // the reused slot holds this connection's traffic since its assignment and
    // nothing of the first connection's: it is above zero and below this
    // client's own totals by exactly the handshake before the assignment
    rocketnet_check( second_slot.packets_sent > 0 && second_slot.packets_sent < second_client.packets_received );
    rocketnet_check( second_slot.packets_received > 0 && second_slot.packets_received < second_client.packets_sent );
    rocketnet_check( second_slot.bytes_sent > 0 && second_slot.bytes_sent < second_client.bytes_received );
    rocketnet_check( second_slot.bytes_received > 0 && second_slot.bytes_received < second_client.bytes_sent );
    rocketnet_check( second_server.packets_sent > first_server.packets_sent );
    rocketnet_check( second_server.packets_received > first_server.packets_received );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( second );
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( first );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

// The wire totals count each packet's own IP header: an IPv4 client adds 28 bytes
// (20 + 8) and an IPv6 client adds 48 (40 + 8). One exchange per family, with a
// settled baseline, then N packets each way whose sealed UDP payload sizes are
// known from finish_packet, so each counter delta is exact.

static void wire_bytes_per_family_exchange( const char * public_address, const char * connect_host,
                                            uint64_t client_id, int ip_udp_header_bytes )
{
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );

    double time = 1000.0;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 1;
    server_config.game_clock_epoch = WIRE_TOTALS_GAME_CLOCK_EPOCH;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

    int server_status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, public_address, &server_config, time, &server_status );
    rocketnet_check( server != NULL );
    rocketnet_check( server_status == ROCKETNET_STATUS_OK );

    char address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    const int server_port = rocketnet_server_transport_port( server );
    snprintf( address, sizeof( address ), "%s:%d", connect_host, server_port );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( address, client_id, rocketnet_functional_token_timestamp( WIRE_TOTALS_GAME_CLOCK_EPOCH, time ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    client_config.game_clock_epoch = WIRE_TOTALS_GAME_CLOCK_EPOCH;
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, time );
    rocketnet_check( client != NULL );
    rocketnet_check( wire_totals_connect( client, server, &time ) );
    rocketnet_check( wire_totals_drain( client, server, &time ) );

    wire_totals_t client_before, server_before;
    wire_totals_client( client, &client_before );
    wire_totals_server( server, &server_before );

    const uint64_t client_ack_before = rocketnet_client_transport_counters( client )->values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED];
    const uint64_t server_ack_before = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_ACK_RECEIVED];
    const int payload_count = 4;
    const int payload_bytes = 64;
    uint8_t payload[64];
    uint64_t client_wire = 0;
    uint64_t server_wire = 0;

    for ( int i = 0; i < payload_count; i++ )
    {
        memset( payload, i + 1, sizeof( payload ) );
        struct rocketnet_packet_context_t context;
        uint8_t * data = rocketnet_client_transport_start_packet( client, payload_bytes, &context );
        rocketnet_check( data != NULL );
        memcpy( data, payload, payload_bytes );
        const int wire_bytes = rocketnet_client_transport_finish_packet( client, &context, payload_bytes );
        rocketnet_check( wire_bytes > 0 );
        client_wire += (uint64_t) wire_bytes;
    }
    rocketnet_client_transport_send_packets( client );

    for ( int i = 0; i < payload_count; i++ )
    {
        memset( payload, 0x80 + i, sizeof( payload ) );
        struct rocketnet_packet_context_t context;
        uint8_t * data = rocketnet_server_transport_start_packet( server, 0, payload_bytes, &context );
        rocketnet_check( data != NULL );
        memcpy( data, payload, payload_bytes );
        const int wire_bytes = rocketnet_server_transport_finish_packet( server, &context, payload_bytes );
        rocketnet_check( wire_bytes > 0 );
        server_wire += (uint64_t) wire_bytes;
    }
    rocketnet_server_transport_send_packets( server );

    rocketnet_check( wire_totals_drain( client, server, &time ) );

    wire_totals_t client_after, server_after;
    wire_totals_client( client, &client_after );
    wire_totals_server( server, &server_after );

    // RN-1 t15: the four payloads elicit standalone acknowledgments. Their
    // receive counters independently identify the extra wire packets. All
    // sequences here are contiguous, so each ACK has the minimum block.
    const uint64_t client_acks = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_ACK_RECEIVED] - server_ack_before;
    const uint64_t server_acks = rocketnet_client_transport_counters( client )->values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] - client_ack_before;
    const uint64_t headers = (uint64_t) payload_count * (uint64_t) ip_udp_header_bytes;
    client_wire += client_acks * (uint64_t) ( ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES + ip_udp_header_bytes );
    server_wire += server_acks * (uint64_t) ( ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES + ip_udp_header_bytes );
    rocketnet_check( client_after.packets_sent - client_before.packets_sent == (uint64_t) payload_count + client_acks );
    rocketnet_check( client_after.packets_received - client_before.packets_received == (uint64_t) payload_count + server_acks );
    rocketnet_check( server_after.packets_sent - server_before.packets_sent == (uint64_t) payload_count + server_acks );
    rocketnet_check( server_after.packets_received - server_before.packets_received == (uint64_t) payload_count + client_acks );
    rocketnet_check( client_after.bytes_sent - client_before.bytes_sent == client_wire + headers );
    rocketnet_check( server_after.bytes_received - server_before.bytes_received == client_wire + headers );
    rocketnet_check( server_after.bytes_sent - server_before.bytes_sent == server_wire + headers );
    rocketnet_check( client_after.bytes_received - client_before.bytes_received == server_wire + headers );

    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( server );

    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void test_transport_wire_bytes_per_family()
{
    wire_bytes_per_family_exchange( "127.0.0.1:0", "127.0.0.1", 0x5704ULL,
                                    ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );
    wire_bytes_per_family_exchange( "[::1]:0", "[::1]", 0x5705ULL,
                                    ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );
}

struct rocketnet_functional_test_t
{
    const char * name;
    void ( *function )();
    bool ( *runs_here )(); // NULL: every host; else the case is in the suite only where it returns true
};

extern void rocketnet_test_real_environment_dev_session_sequence_start();
extern void rocketnet_test_real_environment_log_level_before_init();
extern void rocketnet_test_real_environment_config_precedence_and_invalid();
extern void rocketnet_test_real_environment_client_step_driven();
extern void rocketnet_test_real_environment_dev_simulator_number();
extern void rocketnet_test_real_environment_server_ipv4_address();

// POSIX lifecycle stress: real sockets, counted file descriptors and threads
// (and on Linux locked memory), in functional/posix_lifecycle_stress.cpp.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
extern void rocketnet_test_posix_init_term_1000();
extern void rocketnet_test_posix_server_create_destroy_1000();
extern void rocketnet_test_posix_client_create_destroy_1000();
extern void rocketnet_test_posix_server_restart_same_port_100();
extern void rocketnet_test_posix_destroy_under_traffic_100();
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
extern void rocketnet_test_posix_connect_disconnect_1000_portable_portable();
extern void rocketnet_test_posix_connect_disconnect_1000_portable_msgx();
extern void rocketnet_test_posix_connect_disconnect_1000_msgx_portable();
extern void rocketnet_test_posix_connect_disconnect_1000_msgx_msgx();
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
// one case per Linux server cell
extern bool rocketnet_test_posix_io_uring_offered();
extern bool rocketnet_test_posix_xdp_offered();
extern void rocketnet_test_posix_init_term_1000_portable();
extern void rocketnet_test_posix_init_term_1000_gso();
extern void rocketnet_test_posix_init_term_1000_io_uring();
extern void rocketnet_test_posix_init_term_1000_xdp();
extern void rocketnet_test_posix_server_create_destroy_1000_portable();
extern void rocketnet_test_posix_server_create_destroy_1000_gso();
extern void rocketnet_test_posix_server_create_destroy_1000_io_uring();
extern void rocketnet_test_posix_server_create_destroy_1000_xdp();
extern void rocketnet_test_posix_client_create_destroy_1000_portable();
extern void rocketnet_test_posix_client_create_destroy_1000_gso();
extern void rocketnet_test_posix_client_create_destroy_1000_io_uring();
extern void rocketnet_test_posix_client_create_destroy_1000_xdp();
extern void rocketnet_test_posix_server_restart_same_port_100_portable();
extern void rocketnet_test_posix_server_restart_same_port_100_gso();
extern void rocketnet_test_posix_server_restart_same_port_100_io_uring();
extern void rocketnet_test_posix_server_restart_same_port_100_xdp();
extern void rocketnet_test_posix_connect_disconnect_1000_portable();
extern void rocketnet_test_posix_connect_disconnect_1000_gso();
extern void rocketnet_test_posix_connect_disconnect_1000_io_uring();
extern void rocketnet_test_posix_connect_disconnect_1000_xdp();
extern void rocketnet_test_posix_destroy_under_traffic_100_portable();
extern void rocketnet_test_posix_destroy_under_traffic_100_gso();
extern void rocketnet_test_posix_destroy_under_traffic_100_io_uring();
extern void rocketnet_test_posix_destroy_under_traffic_100_xdp();
#endif

// the channel harness smoke cases (functional/channel_smoke.cpp, picked up by
// the functional/channel_*.cpp glob)
extern void harness_one_message_each_way_lossless();
extern void harness_one_message_each_way_lossy();
extern void harness_lossy_100_percent_negative_control();
extern void harness_no_allocation_after_create();
extern void harness_collect_preserves_other_channels();
extern void unreliable_receipts_over_lossy_link();
extern void promoted_receipts_over_lossy_reliable_blocks();
extern void reliable_receipts_over_lossy_link();
extern void state_stream_never_goes_backwards_over_lossy_link();
extern void reliable_state_stream_over_lossy_link();
extern void reliable_sequenced_long_run_across_epochs();
extern void reliable_sequenced_held_record_released();
extern void large_unreliable_sequenced_uses_transport_fragments();
extern void ordered_256k_chunked_stream_over_lossy_reordering_link();
extern void ordered_small_and_large_interleaved();
extern void config_mismatch_over_loopback();
extern void config_equal_over_lossy_loopback();
extern void reliable_receive_full_ordered_gap_recovers();
extern void reliable_receive_full_recovers_all_modes();
extern void reliable_receive_full_partial_packet_recovers();
extern void reliable_receive_full_recovers_client_to_server();
extern void reliable_receive_full_recovers_server_to_client();
extern void reliable_mixed_sizes_over_lossy_link();
extern void reliable_duplicate_after_gap_delivered_once();
extern void unreliable_at_most_once_over_duplicating_link();
extern void unreliable_dropped_while_reliable_waits_never_arrives();
extern void unreliable_dropped_when_builder_out_of_budget_never_arrives();
extern void control_disconnect_burst_each_end();
extern void control_standalone_ack_each_end();
extern void control_fragment_packets_each_end();
extern void control_denied_when_server_full();
extern void default_one_mib_messages_all_modes();
extern void unreliable_small_messages_pack_in_place();
extern void unreliable_over_receive_budget_dropped_and_counted();
extern void reliable_delivered_when_unreliable_budget_full();
extern void resend_timing_table_over_lossy_links();
extern void block_resend_cost_over_long_path();
extern void rocketnet_test_cpp_transports_connect_over_loopback();
extern void live_message_matrix_loopback();
extern void rocketnet_test_server_async_delivery_with_max_clients();

// the receive buffer sizing cases (functional/server_receive_low_tick.cpp):
// the server socket's request is the formula's value at its max_clients, the
// grant is read back and a short grant names the limit to raise. The
// low-tick case (1000 clients at 256 kbps against a server updating at 10 Hz)
// reads the kernel drop count from procfs, so it is a Linux case. It is absent
// from the default suite and registered only when the load measurement is
// explicitly requested; its delivery and connection outcomes are gates while
// drop count and update cost remain measurements.
extern void rocketnet_test_server_receive_buffer_sized_from_max_clients();
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
extern void rocketnet_test_server_receive_1000_clients_256kbps_10hz();
extern bool rocketnet_test_server_receive_low_tick_measurement_requested();
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
// oversize, runt and garbage datagrams at every Windows receive path (functional/windows_socket_errors.cpp)
extern void windows_server_drops_oversize_datagram_portable_ipv4();
extern void windows_server_drops_oversize_datagram_portable_ipv6();
extern void windows_server_drops_oversize_datagram_iocp_ipv4();
extern void windows_server_drops_oversize_datagram_iocp_ipv6();
extern void windows_server_drops_oversize_datagram_rio_ipv4();
extern void windows_server_drops_oversize_datagram_rio_ipv6();
extern void windows_server_drops_runt_and_garbage_portable_ipv4();
extern void windows_server_drops_runt_and_garbage_portable_ipv6();
extern void windows_server_drops_runt_and_garbage_iocp_ipv4();
extern void windows_server_drops_runt_and_garbage_iocp_ipv6();
extern void windows_server_drops_runt_and_garbage_rio_ipv4();
extern void windows_server_drops_runt_and_garbage_rio_ipv6();
extern void windows_server_counts_oversize_as_prefilter_too_large_iocp();
extern void windows_server_counts_oversize_as_prefilter_too_large_rio();
extern void windows_server_counts_runt_as_prefilter_too_short_rio();
extern void windows_client_drops_oversize_and_garbage_ipv4();
extern void windows_client_drops_oversize_and_garbage_ipv6();
extern void windows_per_packet_receive_counts_empty_datagram_ipv4();
extern void windows_per_packet_receive_counts_empty_datagram_ipv6();
// Windows lifecycle stress: 1000 cycles per case with handles, threads and
// private bytes counted (functional/windows_lifecycle_stress.cpp). One row per
// case per server cell: portable, iocp and rio.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
extern void rocketnet_test_windows_init_term_1000_portable();
extern void rocketnet_test_windows_server_create_destroy_1000_portable();
extern void rocketnet_test_windows_client_create_destroy_1000_portable();
extern void rocketnet_test_windows_connect_disconnect_1000_portable();
extern void rocketnet_test_windows_server_restart_1000_portable();
extern void rocketnet_test_windows_init_term_1000_iocp();
extern void rocketnet_test_windows_server_create_destroy_1000_iocp();
extern void rocketnet_test_windows_client_create_destroy_1000_iocp();
extern void rocketnet_test_windows_connect_disconnect_1000_iocp();
extern void rocketnet_test_windows_server_restart_1000_iocp();
extern void rocketnet_test_windows_init_term_1000_rio();
extern void rocketnet_test_windows_server_create_destroy_1000_rio();
extern void rocketnet_test_windows_client_create_destroy_1000_rio();
extern void rocketnet_test_windows_connect_disconnect_1000_rio();
extern void rocketnet_test_windows_server_restart_1000_rio();
extern void rocketnet_test_windows_lifecycle_ledger();
#endif
// a hard-killed client never stalls a Windows receive (functional/windows_connreset.cpp)
extern void windows_connreset_hard_killed_client_portable_ipv4();
extern void windows_connreset_hard_killed_client_portable_ipv6();
extern void windows_connreset_hard_killed_client_iocp_ipv4();
extern void windows_connreset_hard_killed_client_iocp_ipv6();
extern void windows_connreset_hard_killed_client_iocp_plain_ipv4();
extern void windows_connreset_hard_killed_client_iocp_plain_ipv6();
extern void windows_connreset_hard_killed_client_rio_ipv4();
extern void windows_connreset_hard_killed_client_rio_ipv6();
extern void windows_connreset_send_to_closed_port_portable_ipv4();
extern void windows_connreset_send_to_closed_port_portable_ipv6();
extern void windows_connreset_send_to_closed_port_iocp_ipv4();
extern void windows_connreset_send_to_closed_port_iocp_ipv6();
extern void windows_connreset_send_to_closed_port_iocp_plain_ipv4();
extern void windows_connreset_send_to_closed_port_iocp_plain_ipv6();
extern void windows_connreset_send_to_closed_port_rio_ipv4();
extern void windows_connreset_send_to_closed_port_rio_ipv6();
extern void windows_client_connreset_server_gone_ipv4();
extern void windows_client_connreset_server_gone_ipv6();
extern void windows_connreset_child_client();
extern void windows_connreset_child_parent();
extern void windows_connreset_child_dies_with_its_parent();
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// A destroyed client's receive thread always exits: destroy stores quit and
// wakes the receive thread's wait explicitly before the join. These cases use
// real threads and real waits, so they run here, not in the unit suite.

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

struct rocketnet_test_receive_thread_quit_waiter_t
{
    std::atomic<int> about_to_wait;
    std::atomic<int> waits_done;
};

static void rocketnet_test_receive_thread_quit_long_wait( void * data )
{
    rocketnet_test_receive_thread_quit_waiter_t * waiter = (rocketnet_test_receive_thread_quit_waiter_t*) data;
    waiter->about_to_wait.store( 1, std::memory_order_release );
    // an hour-long wait: only the wake ends it within the test
    rocketnet_platform_thread_wait( 3600.0 );
    // once woken, every later wait returns at once
    rocketnet_platform_thread_wait( 3600.0 );
    waiter->waits_done.store( 2, std::memory_order_release );
}

// the wake ends a wait whose timeout would not fire for an hour, whether the
// wake lands before the wait starts or during it, and stays set for later
// waits. Odd rounds wake at once (mostly before the wait), even rounds wake
// after the thread reaches its wait (mostly during it). A lost wake hangs the
// case, and the suite's time limit reports it.
void rocketnet_test_client_receive_thread_quit_wake_ends_a_long_wait()
{
    for ( int i = 0; i < 100; i++ )
    {
        rocketnet_test_receive_thread_quit_waiter_t waiter;
        waiter.about_to_wait.store( 0 );
        waiter.waits_done.store( 0 );
        rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_test_receive_thread_quit_long_wait, &waiter );
        rocketnet_check( thread );
        if ( ( i & 1 ) == 0 )
        {
            while ( !waiter.about_to_wait.load( std::memory_order_acquire ) ) {}
        }
        rocketnet_platform_thread_wake( thread );
        rocketnet_platform_thread_join( thread );
        rocketnet_platform_thread_destroy( thread );
        rocketnet_check( waiter.waits_done.load( std::memory_order_acquire ) == 2 );
    }
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// 1000 create and destroy cycles of a client transport whose receive thread
// polls the in-memory loopback transport: every destroy returns
void rocketnet_test_client_receive_thread_quit_destroy_joins_1000_times()
{
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );

    const double game_time = 1000.0;
    for ( int i = 0; i < 1000; i++ )
    {
        // a fresh token each cycle: the client checks a token's expiry
        // against the unix clock, and a slow build (tsan) outlives a dev
        // token's lifetime across 1000 cycles. 127.0.0.1:40000 is never
        // opened: the loopback transport opens no socket
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_functional_write_token( "127.0.0.1:40000", 1, rocketnet_functional_token_timestamp( client_config.game_clock_epoch, game_time ), token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, game_time );
        rocketnet_check( client != NULL );
        rn_native_client_transport_stop_and_destroy( client );
    }
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

static void rocketnet_test_receive_thread_quit_wait_after_wake( void * data )
{
    rocketnet_test_receive_thread_quit_waiter_t * waiter = (rocketnet_test_receive_thread_quit_waiter_t*) data;
    // the test wakes this thread before it sets about_to_wait, so both waits start after the wake
    while ( !waiter->about_to_wait.load( std::memory_order_acquire ) ) {}
    // an hour-long wait: only the stored wake ends it within the test
    rocketnet_platform_thread_wait( 3600.0 );
    // once woken, every later wait returns at once
    rocketnet_platform_thread_wait( 3600.0 );
    waiter->waits_done.store( 2, std::memory_order_release );
}

// a wake stored before the wait starts ends the wait and every later one. A
// lost wake hangs the case, and the suite's time limit reports it.
void rocketnet_test_client_receive_thread_quit_wake_before_wait_returns()
{
    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_test_receive_thread_quit_waiter_t waiter;
        waiter.about_to_wait.store( 0 );
        waiter.waits_done.store( 0 );
        rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_test_receive_thread_quit_wait_after_wake, &waiter );
        rocketnet_check( thread );
        rocketnet_platform_thread_wake( thread );
        waiter.about_to_wait.store( 1, std::memory_order_release );
        rocketnet_platform_thread_join( thread );
        rocketnet_platform_thread_destroy( thread );
        rocketnet_check( waiter.waits_done.load( std::memory_order_acquire ) == 2 );
    }
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// A real client and server over the portable transport on 127.0.0.1 (port
// 0): the server disconnects the connected client, and the client state reads
// ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED once its update accepts the
// server's disconnect packet, not CONNECTION_TIMED_OUT.

#if ROCKETNET_DEV
static void rocketnet_test_public_client_reads_server_disconnected()
{
    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    server_config.max_clients = 2;
    server_config.transport = "portable";
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    rocketnet_check( server );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    rocketnet_check( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( server_address, NULL, server_key, ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS, ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, 78, ROCKETNET_DEV_PROTOCOL_ID, rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ), NULL, token ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "portable";
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    if ( client && rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_client_stop_and_destroy( client );
        client = NULL;
    }
    rocketnet_check( client );

    const double wall_start = rocketnet_time();
    while ( rocketnet_time() - wall_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server, rocketnet_client_index( client ) ) )
            break;
        rocketnet_sleep( 0.005 );
    }
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( client );
    rocketnet_server_disconnect_client( server, index );
    rocketnet_server_send_packets( server );

    // well inside the connection timeout: the disconnect packet ends it
    const double disconnect_start = rocketnet_time();
    while ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_time() - disconnect_start < 2.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        rocketnet_client_update( client, now );
        rocketnet_sleep( 0.005 );
    }
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    rocketnet_check( !rocketnet_server_client_connected( server, index ) );

    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
}
#endif // #if ROCKETNET_DEV

// A development connect token naming both families, 127.0.0.1 and [::1] at
// the server's port, against a portable server bound to 127.0.0.1 (port 0),
// from a client whose host reports no IPv6 stack (the seam
// rocketnet_platform_set_ipv6_socket_available_for_testing): the client
// selects the IPv4 address and binds IPv4, the server matches the token's
// IPv4 address although its IPv6 flag is set, and the client connects.

#if ROCKETNET_DEV
static void rocketnet_test_two_address_token_connects_over_ipv4()
{
    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    server_config.max_clients = 2;
    server_config.transport = "portable";
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    rocketnet_check( server && create_status == ROCKETNET_STATUS_OK );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    rocketnet_check( server );

    char ipv4_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char ipv6_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%d", (int) rocketnet_server_port( server ) );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );
    const uint64_t create_timestamp = rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( ipv4_address, ipv6_address, server_key, 300, 10, 0x2FA4,
                                                         server_config.protocol_id, create_timestamp, NULL, token ) == ROCKETNET_STATUS_OK );

    rocketnet_platform_set_ipv6_socket_available_for_testing( false );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "portable";
    create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    rocketnet_check( client && create_status == ROCKETNET_STATUS_OK );
    rocketnet_check( client );
    const enum rocketnet_status_t connect_status = rocketnet_client_connect( client, token );
    rocketnet_check( connect_status == ROCKETNET_STATUS_OK );

    const double wall_start = rocketnet_time();
    while ( rocketnet_time() - wall_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server, rocketnet_client_index( client ) ) )
            break;
        if ( rocketnet_client_state( client ) < ROCKETNET_CLIENT_STATE_DISCONNECTED )
            break;
        rocketnet_sleep( 0.005 );
    }
    printf( "two address token: client state %d\n", (int) rocketnet_client_state( client ) );
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_client_connected( server, rocketnet_client_index( client ) ) );

    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );

    rocketnet_platform_set_ipv6_socket_available_for_testing( true );
}
#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV

// A public development token must complete the asynchronous connection and
// carry application bytes in both directions over a real portable socket.
static void rocketnet_test_public_dev_connect_token_round_trip()
{
    struct rocketnet_server_config_t server_config;
    enum rocketnet_status_t status = rocketnet_server_default_config( &server_config );
    if ( status != ROCKETNET_STATUS_OK )
    {
        rocketnet_check( false );
        return;
    }
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = 2;
    server_config.transport = "portable";

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    if ( server == NULL || status != ROCKETNET_STATUS_OK )
    {
        if ( server != NULL )
            rn_cli_server_stop_and_destroy( server );
        rocketnet_check( false );
        return;
    }
    status = rocketnet_server_start( server );
    if ( status != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        rocketnet_check( false );
        return;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    int setup_ok = rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) );
    rocketnet_server_public_key( server, server_key );
    if ( setup_ok )
    {
        status = rocketnet_create_dev_connect_token( server_address, NULL, server_key,
                                                     ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                                                     ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS,
                                                     0xD3E10, server_config.protocol_id,
                                                     rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() ),
                                                     NULL, token );
        setup_ok = status == ROCKETNET_STATUS_OK;
    }

    struct rocketnet_client_config_t client_config;
    struct rocketnet_client_t * client = NULL;
    if ( setup_ok )
    {
        setup_ok = rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK;
        client_config.transport = "portable";
    }
    if ( setup_ok )
    {
        client = rocketnet_client_create( NULL, &client_config, &status );
        setup_ok = client != NULL && status == ROCKETNET_STATUS_OK;
    }
    if ( setup_ok )
    {
        status = rocketnet_client_connect( client, token );
        setup_ok = status == ROCKETNET_STATUS_OK;
    }
    if ( !setup_ok )
    {
        if ( client != NULL )
            rn_cli_client_stop_and_destroy( client );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_check( false );
        return;
    }

    uint8_t client_payload[37];
    uint8_t server_payload[53];
    for ( int i = 0; i < (int) sizeof( client_payload ); ++i )
        client_payload[i] = (uint8_t) ( 0x31 + i * 7 );
    for ( int i = 0; i < (int) sizeof( server_payload ); ++i )
        server_payload[i] = (uint8_t) ( 0xA7 - i * 3 );

    int client_connected = 0;
    int server_connected = 0;
    int client_sent = 0;
    int server_sent = 0;
    int client_received = 0;
    int server_received = 0;
    int client_bytes = 0;
    int server_bytes = 0;
    int exchange_ok = 1;
    const double watchdog_start = rocketnet_time();
    while ( !( client_received && server_received ) && rocketnet_time() - watchdog_start < 30.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - watchdog_start );
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            client_connected = 1;
        const int client_index = rocketnet_client_index( client );
        if ( client_index >= 0 && rocketnet_server_client_connected( server, client_index ) )
            server_connected = 1;

        if ( client_connected && !client_sent )
        {
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE,
                                                            (int) sizeof( client_payload ) );
            if ( out == NULL )
                exchange_ok = 0;
            else
            {
                memcpy( out, client_payload, sizeof( client_payload ) );
                client_sent = rocketnet_client_finish_message( client, (int) sizeof( client_payload ) ) != 0;
                if ( !client_sent )
                    exchange_ok = 0;
            }
        }

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                if ( batch->client_index[i] != client_index || batch->bytes[i] != (int) sizeof( client_payload ) ||
                     memcmp( batch->data[i], client_payload, sizeof( client_payload ) ) != 0 )
                    exchange_ok = 0;
                else
                {
                    server_received = 1;
                    server_bytes += batch->bytes[i];
                }
            }
        }

        if ( server_connected && server_received && !server_sent )
        {
            uint8_t * out = rocketnet_server_start_message( server, (uint64_t) client_index, 0,
                                                            ROCKETNET_RELIABILITY_RELIABLE,
                                                            (int) sizeof( server_payload ) );
            if ( out == NULL )
                exchange_ok = 0;
            else
            {
                memcpy( out, server_payload, sizeof( server_payload ) );
                server_sent = rocketnet_server_finish_message( server, (int) sizeof( server_payload ) ) != 0;
                if ( !server_sent )
                    exchange_ok = 0;
            }
        }

        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                if ( batch->bytes[i] != (int) sizeof( server_payload ) ||
                     memcmp( batch->data[i], server_payload, sizeof( server_payload ) ) != 0 )
                    exchange_ok = 0;
                else
                {
                    client_received = 1;
                    client_bytes += batch->bytes[i];
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        if ( !( client_received && server_received ) )
            rocketnet_sleep( 0.005 );
    }

    const int both_connected = client_connected && server_connected &&
                               rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
                               rocketnet_server_client_connected( server, rocketnet_client_index( client ) );
    const int completed = both_connected && client_sent && server_received && server_sent && client_received && exchange_ok &&
                          client_bytes == (int) sizeof( server_payload ) && server_bytes == (int) sizeof( client_payload );
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    printf( "public dev token exchange: client_connected=%d server_connected=%d client_bytes=%d server_bytes=%d cleanup=ok\n",
            client_connected, server_connected, client_bytes, server_bytes );
    rocketnet_check( completed );
}

#endif // #if ROCKETNET_DEV

#if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
void rocketnet_test_darwin_send_epipe_no_signal();
#endif
void rocketnet_test_posix_server_receive_weather();
void rocketnet_test_posix_client_receive_weather();
void rocketnet_test_posix_client_drops_foreign_sources();
void rocketnet_test_posix_platform_send_seam();
void rocketnet_test_posix_transport_send_seam();
void rocketnet_test_posix_send_burst();
#if defined(__linux__)
void rocketnet_test_linux_gso_sendmmsg_errors();
void rocketnet_test_linux_gso_recvmmsg_errors();
void rocketnet_test_linux_uring_receive_cqe_errors();
void rocketnet_test_linux_uring_send_cqe_errors();
void rocketnet_test_linux_xdp_receive_weather_ipv4();
void rocketnet_test_linux_xdp_receive_weather_ipv6();
bool rocketnet_test_linux_xdp_weather_runs_here();
#if ROCKETNET_XDP
bool rocketnet_test_xdp_veth_runs_here();
#endif
#endif
#endif

// ---------------------------------------------------
// The macOS msgx client (docs/MACOS.md, "The msgx client"): the msgx backend's
// client is the portable client table (sdk/rocketnet_driver_builtin.cpp,
// rocketnet_msgx_transport), and the pin picks the pump through the name its
// create hands the socket create (portable_io_client_create ->
// portable_socket_create -> rocketnet_platform_socket_create_with_pump): "msgx"
// the sendmsg_x/recvmsg_x pump, "portable" the per-packet loop. These cases
// read the pump off the client's live socket, drive the runtime refusal
// through the platform's send seam, and carry payloads both ways per family
// against the msgx server and the portable server. The socket a client create
// made is the registry diff across the create.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

#define MSGX_CLIENT_MAX_SOCKETS 256

struct msgx_client_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
    struct rocketnet_platform_socket_t * client_socket;
};

// server_bind is the server's bind string, token_host the host the connect
// token names; client_transport NULL is the unpinned (auto) client
static void msgx_client_pair_create( struct msgx_client_pair_t * pair, const char * server_transport, const char * server_bind, const char * token_host, const char * client_transport )
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof(server_config.transport), "%s", server_transport );
    pair->server = rocketnet_server_transport_create( NULL, server_bind, &server_config );
    rocketnet_check( pair->server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( pair->server ), server_transport ) == 0 );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "%s:%d", token_host, rocketnet_server_transport_port( pair->server ) );
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    if ( client_transport )
        snprintf( client_config.transport, sizeof(client_config.transport), "%s", client_transport );

    struct rocketnet_platform_socket_t * before[MSGX_CLIENT_MAX_SOCKETS];
    const int num_before = rocketnet_platform_sockets_for_testing( before, MSGX_CLIENT_MAX_SOCKETS );
    pair->client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( pair->client );
    struct rocketnet_platform_socket_t * after[MSGX_CLIENT_MAX_SOCKETS];
    const int num_after = rocketnet_platform_sockets_for_testing( after, MSGX_CLIENT_MAX_SOCKETS );
    pair->client_socket = NULL;
    for ( int i = 0; i < num_after; i++ )
    {
        bool old = false;
        for ( int j = 0; j < num_before; j++ )
            old = old || before[j] == after[i];
        if ( !old )
        {
            rocketnet_check( pair->client_socket == NULL );
            pair->client_socket = after[i];
        }
    }
    rocketnet_check( pair->client_socket != NULL );
    // the client I/O thread runs the transport (docs/CLIENT-IO.md, RNClientIo)
    rocketnet_check( rocketnet_client_transport_client_io( pair->client ) != NULL );

    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( pair->server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( pair->server ) == 1 );
}

static void msgx_client_pair_destroy( struct msgx_client_pair_t * pair )
{
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( pair->client );
    rocketnet_server_transport_stop( pair->server );
    rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( pair->server );
}

static const int MSGX_CLIENT_PAYLOAD_BYTES = 100;

static void msgx_client_fill_payload( uint8_t * payload, int tag )
{
    payload[0] = (uint8_t) tag;
    for ( int j = 1; j < MSGX_CLIENT_PAYLOAD_BYTES; j++ )
        payload[j] = (uint8_t) ( tag * 31 + j );
}

static int msgx_client_payload_tag( const uint8_t * payload, int bytes )
{
    rocketnet_check( bytes == MSGX_CLIENT_PAYLOAD_BYTES );
    for ( int j = 1; j < MSGX_CLIENT_PAYLOAD_BYTES; j++ )
        rocketnet_check( payload[j] == (uint8_t) ( payload[0] * 31 + j ) );
    return payload[0];
}

// one hand-off of num_packets client payloads, then the server collects them
// until every tag has arrived exactly once (num_packets <= 64: one client I/O
// batch, ROCKETNET_CLIENT_IO_BATCH, so two sendmsg_x chunks of 32 at most)
static void msgx_client_send_and_collect( struct msgx_client_pair_t * pair, int num_packets )
{
    rocketnet_check( num_packets > 0 && num_packets <= ROCKETNET_CLIENT_IO_BATCH );
    for ( int tag = 0; tag < num_packets; tag++ )
    {
        struct rocketnet_packet_context_t context = {};
        uint8_t * payload = rocketnet_client_transport_start_packet( pair->client, MSGX_CLIENT_PAYLOAD_BYTES, &context );
        rocketnet_check( payload );
        msgx_client_fill_payload( payload + context.payload_index, tag );
        rocketnet_check( rocketnet_client_transport_finish_packet( pair->client, &context, MSGX_CLIENT_PAYLOAD_BYTES ) > MSGX_CLIENT_PAYLOAD_BYTES );
    }
    rocketnet_client_transport_send_packets( pair->client );

    bool seen[ROCKETNET_CLIENT_IO_BATCH] = {};
    int received = 0;
    const double start = rocketnet_platform_time();
    while ( received < num_packets && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        const int num_payloads = rocketnet_server_transport_num_payloads( pair->server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( pair->server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            const int tag = msgx_client_payload_tag( payload, payload_bytes );
            rocketnet_check( tag < num_packets && !seen[tag] );
            seen[tag] = true;
            received++;
        }
        uint8_t drain[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( pair->client, drain, (int) sizeof(drain) ) > 0 ) {}
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received == num_packets );
}

// num_packets server payloads to the client, every one received exactly once
static void msgx_client_receive_from_server( struct msgx_client_pair_t * pair, int num_packets )
{
    rocketnet_check( num_packets > 0 && num_packets <= ROCKETNET_CLIENT_IO_BATCH );
    uint8_t payload[MSGX_CLIENT_PAYLOAD_BYTES];
    for ( int tag = 0; tag < num_packets; tag++ )
    {
        msgx_client_fill_payload( payload, tag );
        rocketnet_server_transport_send_packet( pair->server, 0, payload, MSGX_CLIENT_PAYLOAD_BYTES );
    }
    rocketnet_server_transport_send_packets( pair->server );

    bool seen[ROCKETNET_CLIENT_IO_BATCH] = {};
    int received = 0;
    const double start = rocketnet_platform_time();
    while ( received < num_packets && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( pair->client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            const int tag = msgx_client_payload_tag( packet, packet_bytes );
            rocketnet_check( tag < num_packets && !seen[tag] );
            seen[tag] = true;
            received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received == num_packets );
}

struct msgx_client_env_t
{
    struct rocketnet_test_env_guard_t server_guard;
    struct rocketnet_test_env_guard_t client_guard;
};

// the pins are the case's own: a whole-suite env pin would override them
static void msgx_client_env_clear( struct msgx_client_env_t * env )
{
    rocketnet_test_env_clear( &env->server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &env->client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
}

static void msgx_client_env_restore( struct msgx_client_env_t * env )
{
    rocketnet_test_env_restore( &env->client_guard );
    rocketnet_test_env_restore( &env->server_guard );
}

// a pinned client's socket runs the pump its name asks for, before and after
// traffic both ways (the first batch call is the msgx probe and keeps msgx)
static void msgx_client_pin_selects( const char * client_transport, const char * pump )
{
    struct msgx_client_env_t env;
    msgx_client_env_clear( &env );
    struct msgx_client_pair_t pair;
    msgx_client_pair_create( &pair, "portable", "127.0.0.1", "127.0.0.1", client_transport );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( pair.client ), client_transport ? client_transport : "msgx" ) == 0 );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), pump ) == 0 );
    msgx_client_send_and_collect( &pair, 48 );
    msgx_client_receive_from_server( &pair, 48 );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), pump ) == 0 );
    msgx_client_pair_destroy( &pair );
    msgx_client_env_restore( &env );
}

void rocketnet_test_msgx_client_pin_selects_batch_pump()
{
    msgx_client_pin_selects( "msgx", "msgx" );
    // the auto choice on macOS is the msgx client, on the batch pump
    msgx_client_pin_selects( NULL, "msgx" );
}

void rocketnet_test_portable_client_pin_selects_portable_pump()
{
    msgx_client_pin_selects( "portable", "portable" );
}

// the darwin pump's runtime refusal mid-batch: one hand-off of 48 payloads is
// one client I/O batch, two sendmsg_x chunks (32 + 16). The send seam refuses
// the first sendmsg_x with ENOTSUP (not weather), which pauses the pump
// (ROCKETNET_MSGX_RETRY_SECONDS); the refused chunk finishes per-packet and
// the second chunk goes the portable loop, so every datagram of the batch is
// sent. the refused chunk counts once as a failed send (its delivered prefix
// is indeterminate) and its replay, which sent everything, counts nothing.
void rocketnet_test_msgx_client_degrades_without_loss()
{
    struct msgx_client_env_t env;
    msgx_client_env_clear( &env );
    struct msgx_client_pair_t pair;
    msgx_client_pair_create( &pair, "portable", "127.0.0.1", "127.0.0.1", "msgx" );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), "msgx" ) == 0 );

    // quiet: no client update, so the I/O thread finishes what it was handed
    // and the next batch it sends is the one below
    rocketnet_platform_sleep( 0.1 );
    const uint64_t failures_before = rocketnet_platform_socket_send_failures( pair.client_socket );
    rocketnet_platform_socket_fail_next_sends( pair.client_socket, 1, ENOTSUP );
    msgx_client_send_and_collect( &pair, 48 );
    rocketnet_platform_socket_fail_next_sends( pair.client_socket, 0, 0 );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), "portable" ) == 0 );
    rocketnet_check( rocketnet_platform_socket_send_failures( pair.client_socket ) == failures_before + 1 );

    // the paused socket keeps carrying both ways on the portable loop
    msgx_client_send_and_collect( &pair, 48 );
    msgx_client_receive_from_server( &pair, 48 );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), "portable" ) == 0 );

    // the refused chunk's one count is the client's only refusal
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT_EXCEPT( pair.client, ROCKETNET_COUNTER_SEND_FAILED );
    rocketnet_server_transport_stop( pair.server );
    rocketnet_server_transport_update( pair.server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( pair.server );
    msgx_client_env_restore( &env );
}

// the wire per family, the msgx client pinned, against the msgx server and
// the portable server: payloads both ways, the client socket on the batch pump
static void msgx_client_wire( const char * server_bind, const char * token_host )
{
    const char * servers[2] = { "msgx", "portable" };
    for ( int s = 0; s < 2; s++ )
    {
        struct msgx_client_env_t env;
        msgx_client_env_clear( &env );
        struct msgx_client_pair_t pair;
        msgx_client_pair_create( &pair, servers[s], server_bind, token_host, "msgx" );
        rocketnet_check( strcmp( rocketnet_client_transport_io_name( pair.client ), "msgx" ) == 0 );
        msgx_client_send_and_collect( &pair, 64 );
        msgx_client_receive_from_server( &pair, 64 );
        rocketnet_check( strcmp( rocketnet_platform_socket_pump( pair.client_socket ), "msgx" ) == 0 );
        msgx_client_pair_destroy( &pair );
        msgx_client_env_restore( &env );
    }
}

void rocketnet_test_msgx_client_transport_wire_ipv4() { msgx_client_wire( "127.0.0.1", "127.0.0.1" ); }
void rocketnet_test_msgx_client_transport_wire_ipv6() { msgx_client_wire( "[::1]", "[::1]" ); }
// a server on the IPv6 wildcard serving an IPv4 client
void rocketnet_test_msgx_client_transport_wire_dual_stack() { msgx_client_wire( "[::]", "127.0.0.1" ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

// ---------------------------------------------------
// The Windows iocp client (sdk/rocketnet_client_iocp_windows.cpp,
// tla/RNClientIocp.tla): the iocp client pinned, against the portable
// server, carries payloads both ways per family, the granted client
// transport is iocp, and the client runs the I/O thread design of
// docs/CLIENT-IO.md on the completion port's wait and wake.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

struct iocp_client_pair_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * client;
};

static const int IOCP_CLIENT_PAYLOAD_BYTES = 100;

static void iocp_client_fill_payload( uint8_t * payload, int tag )
{
    payload[0] = (uint8_t) tag;
    for ( int j = 1; j < IOCP_CLIENT_PAYLOAD_BYTES; j++ )
        payload[j] = (uint8_t) ( tag * 31 + j );
}

static int iocp_client_payload_tag( const uint8_t * payload, int bytes )
{
    rocketnet_check( bytes == IOCP_CLIENT_PAYLOAD_BYTES );
    for ( int j = 1; j < IOCP_CLIENT_PAYLOAD_BYTES; j++ )
        rocketnet_check( payload[j] == (uint8_t) ( payload[0] * 31 + j ) );
    return payload[0];
}

static void iocp_client_pair_create( struct iocp_client_pair_t * pair, const char * server_bind, const char * token_host )
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_functional_server_config( &server_config );
    server_config.max_clients = 4;
    snprintf( server_config.transport, sizeof(server_config.transport), "%s", "portable" );
    pair->server = rocketnet_server_transport_create( NULL, server_bind, &server_config );
    rocketnet_check( pair->server );
    rocketnet_check( strcmp( rocketnet_server_transport_io_name( pair->server ), "portable" ) == 0 );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "%s:%d", token_host, rocketnet_server_transport_port( pair->server ) );
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_functional_write_token( server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( server_config.game_clock_epoch, rocketnet_platform_time() ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_functional_client_default_config( &client_config );
    snprintf( client_config.transport, sizeof(client_config.transport), "%s", "iocp" );
    pair->client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_platform_time() );
    rocketnet_check( pair->client );
    rocketnet_check( strcmp( rocketnet_client_transport_io_name( pair->client ), "iocp" ) == 0 );
    // the client I/O thread runs the transport (docs/CLIENT-IO.md, RNClientIo)
    rocketnet_check( rocketnet_client_transport_client_io( pair->client ) != NULL );

    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        if ( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( pair->server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( pair->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( pair->server ) == 1 );
}

static void iocp_client_pair_destroy( struct iocp_client_pair_t * pair )
{
    ROCKETNET_FUNCTIONAL_DESTROY_CLIENT( pair->client );
    rocketnet_server_transport_stop( pair->server );
    rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
    ROCKETNET_FUNCTIONAL_DESTROY_SERVER( pair->server );
}

// one hand-off of num_packets client payloads; the server collects every tag once
static void iocp_client_send_and_collect( struct iocp_client_pair_t * pair, int num_packets )
{
    rocketnet_check( num_packets > 0 && num_packets <= ROCKETNET_CLIENT_IO_BATCH );
    for ( int tag = 0; tag < num_packets; tag++ )
    {
        struct rocketnet_packet_context_t context = {};
        uint8_t * payload = rocketnet_client_transport_start_packet( pair->client, IOCP_CLIENT_PAYLOAD_BYTES, &context );
        rocketnet_check( payload );
        iocp_client_fill_payload( payload + context.payload_index, tag );
        rocketnet_check( rocketnet_client_transport_finish_packet( pair->client, &context, IOCP_CLIENT_PAYLOAD_BYTES ) > IOCP_CLIENT_PAYLOAD_BYTES );
    }
    rocketnet_client_transport_send_packets( pair->client );

    bool seen[ROCKETNET_CLIENT_IO_BATCH] = {};
    int received = 0;
    const double start = rocketnet_platform_time();
    while ( received < num_packets && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        const int num_payloads = rocketnet_server_transport_num_payloads( pair->server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( pair->server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            const int tag = iocp_client_payload_tag( payload, payload_bytes );
            rocketnet_check( tag < num_packets && !seen[tag] );
            seen[tag] = true;
            received++;
        }
        uint8_t drain[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( pair->client, drain, (int) sizeof(drain) ) > 0 ) {}
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received == num_packets );
}

// num_packets server payloads, every one received by the iocp client once
static void iocp_client_receive_from_server( struct iocp_client_pair_t * pair, int num_packets )
{
    rocketnet_check( num_packets > 0 && num_packets <= ROCKETNET_CLIENT_IO_BATCH );
    uint8_t payload[IOCP_CLIENT_PAYLOAD_BYTES];
    for ( int tag = 0; tag < num_packets; tag++ )
    {
        iocp_client_fill_payload( payload, tag );
        rocketnet_server_transport_send_packet( pair->server, 0, payload, IOCP_CLIENT_PAYLOAD_BYTES );
    }
    rocketnet_server_transport_send_packets( pair->server );

    bool seen[ROCKETNET_CLIENT_IO_BATCH] = {};
    int received = 0;
    const double start = rocketnet_platform_time();
    while ( received < num_packets && rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( pair->client, rocketnet_platform_time() );
        rocketnet_server_transport_update( pair->server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( pair->server );
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( pair->client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            const int tag = iocp_client_payload_tag( packet, packet_bytes );
            rocketnet_check( tag < num_packets && !seen[tag] );
            seen[tag] = true;
            received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received == num_packets );
}

static void iocp_client_wire( const char * server_bind, const char * token_host )
{
    struct rocketnet_test_env_guard_t server_guard, client_guard;
    rocketnet_test_env_clear( &server_guard, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_test_env_clear( &client_guard, "ROCKETNET_CLIENT_TRANSPORT" );
    struct iocp_client_pair_t pair;
    iocp_client_pair_create( &pair, server_bind, token_host );
    iocp_client_send_and_collect( &pair, 64 );
    iocp_client_receive_from_server( &pair, 64 );
    // and again: every buffer the first round completed was re-posted
    iocp_client_send_and_collect( &pair, 64 );
    iocp_client_receive_from_server( &pair, 64 );
    iocp_client_pair_destroy( &pair );
    rocketnet_test_env_restore( &client_guard );
    rocketnet_test_env_restore( &server_guard );
}

void rocketnet_test_iocp_client_transport_wire_ipv4() { iocp_client_wire( "127.0.0.1", "127.0.0.1" ); }
void rocketnet_test_iocp_client_transport_wire_ipv6() { iocp_client_wire( "[::1]", "[::1]" ); }
// a server on the IPv6 wildcard serving an IPv4 client
void rocketnet_test_iocp_client_transport_wire_dual_stack() { iocp_client_wire( "[::]", "127.0.0.1" ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// getaddrinfo can open a socket (the nscd lookup on glibc), so name
// resolution is a functional case, never a unit one.

static void rocketnet_test_hostname_resolve_localhost()
{
    rocketnet_address_t address, ipv6;
    rocketnet_check(rocketnet_platform_hostname_resolve("localhost", "40000", &address));
    rocketnet_check(address.port == 40000);
    rocketnet_check(rocketnet_address_parse(&ipv6, "[::1]:40000"));
    rocketnet_check((address.type == ROCKETNET_ADDRESS_IPV4 && address.data.ipv4[0] == 127) ||
                   rocketnet_address_equal(&address, &ipv6));
}

extern void rocketnet_test_platform_thread();
extern void rocketnet_test_frame_sync_wait();
extern void rocketnet_test_frame_sync_dropped_stats();
#if ROCKETNET_DEV
extern void rocketnet_test_frame_sync_high_resolution_timer_required();
#endif // #if ROCKETNET_DEV
extern void rocketnet_test_connect_token_mint_concurrent();
extern void rocketnet_test_ack_endpoints_independent_across_threads();
extern void test_api_polish_dev_token_string();
extern void test_dev_connect_token_timeout_is_the_named_constant();
extern void test_dev_token_minting_absent_in_release();

// the corpus-consistency cases (functional/corpus_consistency.cpp): each walks a
// directory of committed inputs that grows, so none is a unit case
extern void test_pcapng_corpus_replay();
extern void test_pcapng_corpus_witness_unread_packet_goes_red();
extern void test_pcapng_corpus_covers_control_packet_types();
extern void test_fuzz_prefilter_seed_names_match_outcomes();

// the transport-selection cases check what a create selects, so a whole-suite
// pin that names another transport takes them out of the suite

static bool rocketnet_test_server_pin_allows_portable() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "portable" ); }
static bool rocketnet_test_server_pin_allows_auto() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", NULL ); }
static bool rocketnet_test_client_pin_allows_auto() { return rocketnet_test_transport_pin_allows( "ROCKETNET_CLIENT_TRANSPORT", NULL ); }
static bool rocketnet_test_a_transport_pin_is_set() { return !rocketnet_test_server_pin_allows_auto() || !rocketnet_test_client_pin_allows_auto(); }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static bool rocketnet_test_server_pin_allows_iocp() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "iocp" ); }
static bool rocketnet_test_server_pin_allows_rio() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "rio" ); }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
static bool rocketnet_test_server_pin_allows_gso() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "gso" ); }
static bool rocketnet_test_server_pin_allows_xdp() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "xdp" ); }
// a build without liburing still runs the case (the pin falls to portable);
// a kernel that refuses the ring takes it out
static bool rocketnet_test_server_pin_allows_io_uring()
{
    return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "io_uring" ) &&
           ( rocketnet_server_uring_available() || !rocketnet_transport_definition( ROCKETNET_TRANSPORT_IO_URING )->backend );
}
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
static bool rocketnet_test_server_pin_allows_msgx() { return rocketnet_test_transport_pin_allows( "ROCKETNET_SERVER_TRANSPORT", "msgx" ); }
#endif

extern void rocketnet_test_server_stats_creates_no_thread();
extern void rocketnet_test_public_step_driven_client_connects_without_a_thread();
extern void rocketnet_test_session_reconnect_isolation_portable();
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
extern void rocketnet_test_session_reconnect_isolation_gso();
extern void rocketnet_test_session_reconnect_isolation_io_uring();
#if ROCKETNET_XDP
extern void rocketnet_test_session_reconnect_isolation_xdp();
#endif
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
extern void rocketnet_test_session_reconnect_isolation_msgx();
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
extern void rocketnet_test_session_reconnect_isolation_iocp();
extern void rocketnet_test_session_reconnect_isolation_rio();
#endif

// A case that cannot be built against the production library (it calls a
// development-only hook, seam, key or token) is named in the table in both
// builds; in ./bin/functional_production its function is absent, and the
// case must be named in CMakeLists.txt's ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED_CASES
// (rocketnet_functional_production_exclusions_hold).
#if ROCKETNET_DEV
#define ROCKETNET_FUNCTIONAL_DEV_ONLY( function ) function
#else // #if ROCKETNET_DEV
#define ROCKETNET_FUNCTIONAL_DEV_ONLY( function ) NULL
#endif // #if ROCKETNET_DEV

static const struct rocketnet_functional_test_t rocketnet_functional_tests[] =
{
    { "test_real_environment_dev_session_sequence_start", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_dev_session_sequence_start ), NULL },
    { "test_real_environment_log_level_before_init", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_log_level_before_init ), NULL },
    { "test_real_environment_config_precedence_and_invalid", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_config_precedence_and_invalid ), NULL },
    { "test_real_environment_client_step_driven", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_client_step_driven ), NULL },
    { "test_real_environment_dev_simulator_number", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_dev_simulator_number ), NULL },
    { "test_real_environment_server_ipv4_address", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_real_environment_server_ipv4_address ), NULL },
    { "test_hostname_resolve_localhost", rocketnet_test_hostname_resolve_localhost, NULL },
    { "test_platform_thread", rocketnet_test_platform_thread, NULL },
    { "test_frame_sync_wait", rocketnet_test_frame_sync_wait, NULL },
    { "test_frame_sync_dropped_stats", rocketnet_test_frame_sync_dropped_stats, NULL },
#if ROCKETNET_DEV
    { "test_frame_sync_high_resolution_timer_required", rocketnet_test_frame_sync_high_resolution_timer_required, NULL },
#endif // #if ROCKETNET_DEV
    { "test_pcapng_corpus_replay", test_pcapng_corpus_replay, NULL },
    { "test_pcapng_corpus_witness_unread_packet_goes_red", test_pcapng_corpus_witness_unread_packet_goes_red, NULL },
    { "test_pcapng_corpus_covers_control_packet_types", test_pcapng_corpus_covers_control_packet_types, NULL },
    { "test_fuzz_prefilter_seed_names_match_outcomes", test_fuzz_prefilter_seed_names_match_outcomes, NULL },
    { "test_connect_token_mint_concurrent", rocketnet_test_connect_token_mint_concurrent, NULL },
    { "test_api_polish_dev_token_string", ROCKETNET_FUNCTIONAL_DEV_ONLY( test_api_polish_dev_token_string ), NULL },
    { "test_dev_connect_token_timeout_is_the_named_constant", ROCKETNET_FUNCTIONAL_DEV_ONLY( test_dev_connect_token_timeout_is_the_named_constant ), NULL },
    { "test_dev_token_minting_absent_in_release", ROCKETNET_FUNCTIONAL_DEV_ONLY( test_dev_token_minting_absent_in_release ), NULL },
#if ROCKETNET_DEV
    { "test_public_dev_connect_token_round_trip", rocketnet_test_public_dev_connect_token_round_trip, NULL },
#endif // #if ROCKETNET_DEV
    { "test_ack_endpoints_independent_across_threads", rocketnet_test_ack_endpoints_independent_across_threads, NULL },
    { "test_server_refuses_keyless_config", rocketnet_test_server_refuses_keyless_config, NULL },
    { "test_server_async_delivery_with_max_clients", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_async_delivery_with_max_clients ), NULL },
    { "test_server_receive_buffer_sized_from_max_clients", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_receive_buffer_sized_from_max_clients ), NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_server_receive_1000_clients_256kbps_10hz", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_receive_1000_clients_256kbps_10hz ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_receive_low_tick_measurement_requested ) },
#endif
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    // Windows keeps its sleep and its wake is a no-op, so only POSIX runs the wake cases
    { "test_client_receive_thread_quit_wake_ends_a_long_wait", rocketnet_test_client_receive_thread_quit_wake_ends_a_long_wait, NULL },
    { "test_client_receive_thread_quit_wake_before_wait_returns", rocketnet_test_client_receive_thread_quit_wake_before_wait_returns, NULL },
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    { "test_client_receive_thread_quit_destroy_joins_1000_times", rocketnet_test_client_receive_thread_quit_destroy_joins_1000_times, NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_portable_transport_posix_init_term_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_init_term_1000_portable ), NULL },
    { "test_gso_transport_posix_init_term_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_init_term_1000_gso ), NULL },
    { "test_io_uring_transport_posix_init_term_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_init_term_1000_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_init_term_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_init_term_1000_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
    { "test_portable_transport_posix_server_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_create_destroy_1000_portable ), NULL },
    { "test_gso_transport_posix_server_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_create_destroy_1000_gso ), NULL },
    { "test_io_uring_transport_posix_server_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_create_destroy_1000_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_server_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_create_destroy_1000_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
    { "test_portable_transport_posix_client_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_create_destroy_1000_portable ), NULL },
    { "test_gso_transport_posix_client_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_create_destroy_1000_gso ), NULL },
    { "test_io_uring_transport_posix_client_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_create_destroy_1000_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_client_create_destroy_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_create_destroy_1000_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
    { "test_portable_transport_posix_server_restart_same_port_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_restart_same_port_100_portable ), NULL },
    { "test_gso_transport_posix_server_restart_same_port_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_restart_same_port_100_gso ), NULL },
    { "test_io_uring_transport_posix_server_restart_same_port_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_restart_same_port_100_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_server_restart_same_port_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_server_restart_same_port_100_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
    { "test_portable_transport_posix_connect_disconnect_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_connect_disconnect_1000_portable ), NULL },
    { "test_gso_transport_posix_connect_disconnect_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_connect_disconnect_1000_gso ), NULL },
    { "test_io_uring_transport_posix_connect_disconnect_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_connect_disconnect_1000_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_connect_disconnect_1000", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_connect_disconnect_1000_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
    { "test_portable_transport_posix_destroy_under_traffic_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_destroy_under_traffic_100_portable ), NULL },
    { "test_gso_transport_posix_destroy_under_traffic_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_destroy_under_traffic_100_gso ), NULL },
    { "test_io_uring_transport_posix_destroy_under_traffic_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_destroy_under_traffic_100_io_uring ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_io_uring_offered ) },
    { "test_xdp_transport_posix_destroy_under_traffic_100", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_destroy_under_traffic_100_xdp ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_xdp_offered ) },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    { "test_posix_init_term_1000", rocketnet_test_posix_init_term_1000, NULL },
    { "test_posix_server_create_destroy_1000", rocketnet_test_posix_server_create_destroy_1000, NULL },
    { "test_posix_client_create_destroy_1000", rocketnet_test_posix_client_create_destroy_1000, NULL },
    { "test_posix_server_restart_same_port_100", rocketnet_test_posix_server_restart_same_port_100, NULL },
    { "test_posix_connect_disconnect_1000_portable_portable", rocketnet_test_posix_connect_disconnect_1000_portable_portable, NULL },
    { "test_posix_connect_disconnect_1000_portable_msgx", rocketnet_test_posix_connect_disconnect_1000_portable_msgx, NULL },
    { "test_posix_connect_disconnect_1000_msgx_portable", rocketnet_test_posix_connect_disconnect_1000_msgx_portable, NULL },
    { "test_posix_connect_disconnect_1000_msgx_msgx", rocketnet_test_posix_connect_disconnect_1000_msgx_msgx, NULL },
    { "test_posix_destroy_under_traffic_100", rocketnet_test_posix_destroy_under_traffic_100, NULL },
#endif
    { "test_transport_disconnect_delivery", rocketnet_test_transport_disconnect_delivery, NULL },
    { "test_transport_reconnect_churn", rocketnet_test_transport_reconnect_churn, NULL },
    { "test_client_disconnect_reports_state_change", rocketnet_test_client_disconnect_reports_state_change, NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_client_io_linux_handoff_round_trip", rocketnet_test_client_io_linux_handoff_round_trip, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    { "test_client_io_apple_handoff_round_trip", rocketnet_test_client_io_apple_handoff_round_trip, NULL },
#endif
    { "test_transport_ring_exhaustion", rocketnet_test_transport_ring_exhaustion, NULL },
    { "test_transport_client_concurrent_fill", rocketnet_test_transport_client_concurrent_fill, NULL },
    { "test_transport_concurrent_fill", rocketnet_test_transport_concurrent_fill, NULL },
    { "test_transport_wire_totals_concurrent_send_exact", rocketnet_test_transport_wire_totals_concurrent_send_exact, NULL },
    { "test_transport_start_finish_shape", rocketnet_test_transport_start_finish_shape, NULL },
    { "test_loopback_transport_udp", rocketnet_test_transport_udp_loopback, NULL },
    { "test_client_ipv4_without_ipv6_stack", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_client_ipv4_without_ipv6_stack ), NULL },
    { "test_loopback_transport_cpp_transports_connect", rocketnet_test_cpp_transports_connect_over_loopback, NULL },
    { "test_client_receive_keeps_payloads_on_bad_capacity", rocketnet_test_client_receive_keeps_payloads_on_bad_capacity, NULL },
    { "test_transport_stats", rocketnet_test_transport_stats, NULL },
    { "test_client_ack_stats_computed_on_read", rocketnet_test_client_ack_stats_computed_on_read, NULL },
    { "test_transport_update_time_clamped", rocketnet_test_transport_update_time_clamped, NULL },
    { "test_transport_packet_loss_stats", rocketnet_test_transport_packet_loss_stats, NULL },
    { "test_platform_socket", rocketnet_test_platform_socket, NULL },
    { "test_receive_drops_oversize_datagram", rocketnet_test_receive_drops_oversize_datagram, NULL },
    { "test_platform_batch_pump", rocketnet_test_platform_batch_pump, NULL },
    { "test_platform_batch_pump_degraded", rocketnet_test_platform_batch_pump_degraded, NULL },
#if defined(ROCKETNET_PLATFORM_HAS_EVENT_ONLY_WAKE)
    { "test_platform_wake_event_only", rocketnet_test_platform_wake_event_only, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_platform_wake_linux_wakes_and_reads", rocketnet_test_platform_wake_linux_wakes_and_reads, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_client_io_linux_mmsg_batch_round_trip", rocketnet_test_client_io_linux_mmsg_batch_round_trip, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    { "test_platform_wake_apple_wakes_and_reads", rocketnet_test_platform_wake_apple_wakes_and_reads, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_platform_wake_windows_wakes_and_reads", rocketnet_test_platform_wake_windows_wakes_and_reads, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_client_io_windows_handoff_round_trip", rocketnet_test_client_io_windows_handoff_round_trip, NULL },
#endif
#if !defined(_WIN32) && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    { "test_darwin_send_epipe_no_signal", rocketnet_test_darwin_send_epipe_no_signal, NULL },
#endif
    { "test_posix_server_receive_weather", rocketnet_test_posix_server_receive_weather, NULL },
    { "test_posix_client_receive_weather", rocketnet_test_posix_client_receive_weather, NULL },
    { "test_posix_client_drops_foreign_sources", rocketnet_test_posix_client_drops_foreign_sources, NULL },
    { "test_posix_platform_send_seam", rocketnet_test_posix_platform_send_seam, NULL },
    { "test_posix_transport_send_seam", rocketnet_test_posix_transport_send_seam, NULL },
    { "test_posix_send_burst", rocketnet_test_posix_send_burst, NULL },
#endif
#if defined(__linux__)
    { "test_gso_transport_linux_sendmmsg_errors", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_gso_sendmmsg_errors ), NULL },
    { "test_gso_transport_linux_recvmmsg_errors", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_gso_recvmmsg_errors ), NULL },
    { "test_io_uring_transport_linux_receive_cqe_errors", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_uring_receive_cqe_errors ), NULL },
    { "test_io_uring_transport_linux_send_cqe_errors", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_uring_send_cqe_errors ), NULL },
#if ROCKETNET_XDP
    { "test_xdp_transport_linux_receive_weather_ipv4", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_xdp_receive_weather_ipv4 ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_xdp_weather_runs_here ) },
    { "test_xdp_transport_linux_receive_weather_ipv6", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_xdp_receive_weather_ipv6 ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_linux_xdp_weather_runs_here ) },
#endif
#endif
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    { "test_platform_batch_pump_refused_destination", rocketnet_test_platform_batch_pump_refused_destination, NULL },
    { "test_platform_pump_zero_length_chaff", rocketnet_test_platform_pump_zero_length_chaff, NULL },
#endif
    { "test_transport_acks_and_client_stats", rocketnet_test_transport_acks_and_client_stats, NULL },
    { "test_client_stats_of_empty_slot_read_zero", rocketnet_test_client_stats_of_empty_slot_read_zero, NULL },
    { "test_transport_acked_unfragmented_was_in_exactly_one_batch", rocketnet_test_transport_acked_unfragmented_was_in_exactly_one_batch, NULL },
    { "test_transport_reply_opens_a_later_update", rocketnet_test_transport_reply_opens_a_later_update, NULL },
    { "test_transport_fragmentation", rocketnet_test_transport_fragmentation, NULL },
    { "test_loopback_transport_ipv6", rocketnet_test_transport_ipv6_loopback, NULL },
    { "test_transport_dual_stack_ipv4_client", rocketnet_test_transport_dual_stack_ipv4_client, NULL },
    { "test_server_serves_both_families", rocketnet_test_server_serves_both_families, NULL },
    { "test_max_size_packet_crosses_a_1500_mtu_link_ipv4", rocketnet_test_max_size_packet_crosses_a_1500_mtu_link_ipv4, NULL },
    { "test_max_size_packet_crosses_a_1500_mtu_link_ipv6", rocketnet_test_max_size_packet_crosses_a_1500_mtu_link_ipv6, NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    { "test_xdp_transport_max_size_both_ways_ipv4", rocketnet_test_xdp_max_size_both_ways_ipv4, rocketnet_test_xdp_veth_runs_here },
    { "test_xdp_transport_max_size_both_ways_ipv6", rocketnet_test_xdp_max_size_both_ways_ipv6, rocketnet_test_xdp_veth_runs_here },
    { "test_xdp_transport_keep_alive_before_first_payloads_ipv4", rocketnet_test_xdp_keep_alive_before_first_payloads_ipv4, rocketnet_test_xdp_veth_runs_here },
    { "test_xdp_transport_keep_alive_before_first_payloads_ipv6", rocketnet_test_xdp_keep_alive_before_first_payloads_ipv6, rocketnet_test_xdp_veth_runs_here },
    { "test_posix_hard_killed_client_xdp_ipv4_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_hard_killed_client_xdp_ipv4_client_portable ), ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_xdp_veth_runs_here ) },
    { "test_posix_hard_killed_client_xdp_ipv6_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_hard_killed_client_xdp_ipv6_client_portable ), rocketnet_test_xdp_veth_runs_here },
    { "test_posix_send_to_closed_port_xdp_ipv4_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_send_to_closed_port_xdp_ipv4_client_portable ), rocketnet_test_xdp_veth_runs_here },
    { "test_posix_send_to_closed_port_xdp_ipv6_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_send_to_closed_port_xdp_ipv6_client_portable ), rocketnet_test_xdp_veth_runs_here },
    { "test_posix_client_server_gone_xdp_ipv4_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_server_gone_xdp_ipv4_client_portable ), rocketnet_test_xdp_veth_runs_here },
    { "test_posix_client_server_gone_xdp_ipv6_client_portable", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_posix_client_server_gone_xdp_ipv6_client_portable ), rocketnet_test_xdp_veth_runs_here },
#endif
    { "test_config_out_of_range_refuses_create", rocketnet_test_config_out_of_range_refuses_create, NULL },
    { "test_server_handshake_deadline_from_config", rocketnet_test_server_handshake_deadline_from_config, NULL },
    { "test_config_precedence", rocketnet_test_config_precedence, NULL },
    { "test_public_client_server_queries", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_public_client_server_queries ), NULL },
    { "test_public_client_reads_server_disconnected", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_public_client_reads_server_disconnected ), NULL },
    { "test_two_address_token_connects_over_ipv4", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_two_address_token_connects_over_ipv4 ), NULL },
    { "test_portable_transport_server_io_pin", rocketnet_test_server_transport_io_pin_portable, rocketnet_test_server_pin_allows_portable },
    { "test_loopback_transport_server_io_pin", rocketnet_test_server_transport_io_pin_loopback, NULL },
    { "test_loopback_transport_env_pins", rocketnet_test_transport_env_pins_loopback, NULL },
    { "test_transport_env_pin_is_honoured", transport_env_pin_is_honoured, rocketnet_test_a_transport_pin_is_set },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_iocp_transport_server_io_pin", rocketnet_test_server_transport_io_pin_iocp, rocketnet_test_server_pin_allows_iocp },
    { "test_rio_transport_server_io_pin", rocketnet_test_server_transport_io_pin_rio, rocketnet_test_server_pin_allows_rio },
    { "test_rio_transport_server_probe", rocketnet_test_server_rio_probe, NULL },
    { "test_rio_transport_server_wire_ipv4", rocketnet_test_server_rio_wire_ipv4, NULL },
    { "test_rio_transport_server_wire_ipv6", rocketnet_test_server_rio_wire_ipv6, NULL },
    { "test_rio_transport_server_wire_dual_stack", rocketnet_test_server_rio_wire_dual_stack, NULL },
    { "test_rio_transport_server_send_full", rocketnet_test_server_rio_send_full, NULL },
    { "test_iocp_transport_server_uso_uro_probe", rocketnet_test_server_iocp_uso_uro_probe, NULL },
    { "test_iocp_transport_server_uso_wire", rocketnet_test_server_iocp_uso_wire, NULL },
#endif
    { "test_server_port_zero_transport", rocketnet_test_server_port_zero_transport, rocketnet_test_server_pin_allows_auto },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_gso_transport_server_io_pin", rocketnet_test_server_transport_io_pin_gso, rocketnet_test_server_pin_allows_gso },
    { "test_io_uring_transport_server_io_pin", rocketnet_test_server_transport_io_pin_io_uring, rocketnet_test_server_pin_allows_io_uring },
    { "test_xdp_transport_server_io_pin", rocketnet_test_server_transport_io_pin_xdp, rocketnet_test_server_pin_allows_xdp },
    { "test_server_transport_io_ladder_fall", rocketnet_test_server_transport_io_ladder_fall, rocketnet_test_server_pin_allows_gso },
    { "test_gso_transport_server_probe", rocketnet_test_server_gso_probe, NULL },
    { "test_gso_transport_server_wire_ipv4", rocketnet_test_server_gso_wire_ipv4, NULL },
    { "test_gso_transport_server_wire_plain", rocketnet_test_server_gso_wire_plain, NULL },
    { "test_gso_transport_server_wire_ipv6", rocketnet_test_server_gso_wire_ipv6, NULL },
    { "test_gso_transport_server_wire_dual_stack", rocketnet_test_server_gso_wire_dual_stack, NULL },
    { "test_gso_client_transport_wire_ipv4", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_gso_client_wire_ipv4 ), NULL },
    { "test_gso_client_transport_wire_ipv6", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_gso_client_wire_ipv6 ), NULL },
    { "test_gso_client_transport_wire_plain", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_gso_client_wire_plain ), NULL },
    { "test_gso_client_transport_wire_gro", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_gso_client_wire_gro ), NULL },
    { "test_gso_transport_server_send_overflow", rocketnet_test_server_gso_send_overflow, NULL },
    { "test_gso_shutdown_send_interruptions", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_gso_shutdown_send_interruptions ), NULL },
    { "test_gso_transport_held_receive_view", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_gso_held_receive_view ), NULL },
    { "test_io_uring_transport_server_probe", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_uring_probe ), rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_ipv4", rocketnet_test_server_uring_wire_ipv4, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_ipv6", rocketnet_test_server_uring_wire_ipv6, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_dual_stack", rocketnet_test_server_uring_wire_dual_stack, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_gso_ipv4", rocketnet_test_server_uring_wire_gso_ipv4, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_gso_ipv6", rocketnet_test_server_uring_wire_gso_ipv6, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_plain", rocketnet_test_server_uring_wire_plain, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_gro_ipv4", rocketnet_test_server_uring_wire_gro_ipv4, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_wire_gro_ipv6", rocketnet_test_server_uring_wire_gro_ipv6, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_send_overflow", rocketnet_test_server_uring_send_overflow, rocketnet_server_uring_available },
    { "test_io_uring_transport_server_receive_rearms_after_no_buffers", rocketnet_test_server_uring_receive_rearms_after_no_buffers, rocketnet_server_uring_available },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    { "test_msgx_transport_server_io_pin", rocketnet_test_server_transport_io_pin_msgx, rocketnet_test_server_pin_allows_msgx },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
    { "test_msgx_client_pin_selects_batch_pump", rocketnet_test_msgx_client_pin_selects_batch_pump, NULL },
    { "test_portable_client_pin_selects_portable_pump", rocketnet_test_portable_client_pin_selects_portable_pump, NULL },
    { "test_msgx_client_degrades_without_loss", rocketnet_test_msgx_client_degrades_without_loss, NULL },
    { "test_msgx_client_transport_wire_ipv4", rocketnet_test_msgx_client_transport_wire_ipv4, NULL },
    { "test_msgx_client_transport_wire_ipv6", rocketnet_test_msgx_client_transport_wire_ipv6, NULL },
    { "test_msgx_client_transport_wire_dual_stack", rocketnet_test_msgx_client_transport_wire_dual_stack, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_iocp_client_transport_wire_ipv4", rocketnet_test_iocp_client_transport_wire_ipv4, NULL },
    { "test_iocp_client_transport_wire_ipv6", rocketnet_test_iocp_client_transport_wire_ipv6, NULL },
    { "test_iocp_client_transport_wire_dual_stack", rocketnet_test_iocp_client_transport_wire_dual_stack, NULL },
#endif
    { "test_transport_all_skipped_uses_auto", rocketnet_test_transport_all_skipped_uses_auto, rocketnet_test_server_pin_allows_auto },
    { "test_config_env_aliases", rocketnet_test_config_env_aliases, NULL },
    { "test_client_transport_io", rocketnet_test_client_transport_io, rocketnet_test_client_pin_allows_auto },
    { "test_lifecycle_transport", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_lifecycle_transport ), NULL },
    { "test_loopback_transport_message_objects_from_t1000", rocketnet_test_message_objects_loopback_from_t1000, NULL },
    { "test_harness_one_message_each_way_lossless", ROCKETNET_FUNCTIONAL_DEV_ONLY( harness_one_message_each_way_lossless ), NULL },
    { "test_harness_one_message_each_way_lossy", ROCKETNET_FUNCTIONAL_DEV_ONLY( harness_one_message_each_way_lossy ), NULL },
    { "test_harness_lossy_100_percent_negative_control", ROCKETNET_FUNCTIONAL_DEV_ONLY( harness_lossy_100_percent_negative_control ), NULL },
    { "test_harness_no_allocation_after_create", ROCKETNET_FUNCTIONAL_DEV_ONLY( harness_no_allocation_after_create ), NULL },
    { "test_harness_collect_preserves_other_channels", ROCKETNET_FUNCTIONAL_DEV_ONLY( harness_collect_preserves_other_channels ), NULL },
    { "test_unreliable_receipts_over_lossy_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_receipts_over_lossy_link ), NULL },
    { "test_promoted_receipts_over_lossy_reliable_blocks", ROCKETNET_FUNCTIONAL_DEV_ONLY( promoted_receipts_over_lossy_reliable_blocks ), NULL },
    { "test_reliable_receipts_over_lossy_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receipts_over_lossy_link ), NULL },
    { "test_state_stream_never_goes_backwards_over_lossy_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( state_stream_never_goes_backwards_over_lossy_link ), NULL },
    { "test_reliable_state_stream_over_lossy_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_state_stream_over_lossy_link ), NULL },
    { "test_reliable_sequenced_long_run_across_epochs", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_sequenced_long_run_across_epochs ), NULL },
    { "test_reliable_sequenced_held_record_released", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_sequenced_held_record_released ), NULL },
    { "test_large_unreliable_sequenced_uses_transport_fragments", ROCKETNET_FUNCTIONAL_DEV_ONLY( large_unreliable_sequenced_uses_transport_fragments ), NULL },
    { "test_ordered_256k_chunked_stream_over_lossy_reordering_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( ordered_256k_chunked_stream_over_lossy_reordering_link ), NULL },
    { "test_ordered_small_and_large_interleaved", ROCKETNET_FUNCTIONAL_DEV_ONLY( ordered_small_and_large_interleaved ), NULL },
    { "test_loopback_transport_config_mismatch", ROCKETNET_FUNCTIONAL_DEV_ONLY( config_mismatch_over_loopback ), NULL },
    { "test_loopback_transport_config_equal_over_lossy", ROCKETNET_FUNCTIONAL_DEV_ONLY( config_equal_over_lossy_loopback ), NULL },
    { "test_reliable_receive_full_ordered_gap_recovers", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receive_full_ordered_gap_recovers ), NULL },
    { "test_reliable_receive_full_recovers_all_modes", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receive_full_recovers_all_modes ), NULL },
    { "test_reliable_receive_full_partial_packet_recovers", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receive_full_partial_packet_recovers ), NULL },
    { "test_reliable_receive_full_recovers_client_to_server", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receive_full_recovers_client_to_server ), NULL },
    { "test_reliable_receive_full_recovers_server_to_client", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_receive_full_recovers_server_to_client ), NULL },
    { "test_reliable_mixed_sizes_over_lossy_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_mixed_sizes_over_lossy_link ), NULL },
    { "test_reliable_duplicate_after_gap_delivered_once", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_duplicate_after_gap_delivered_once ), NULL },
    { "test_unreliable_at_most_once_over_duplicating_link", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_at_most_once_over_duplicating_link ), NULL },
    { "test_unreliable_dropped_while_reliable_waits_never_arrives", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_dropped_while_reliable_waits_never_arrives ), NULL },
    { "test_unreliable_dropped_when_builder_out_of_budget_never_arrives", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_dropped_when_builder_out_of_budget_never_arrives ), NULL },
    { "test_control_disconnect_burst_each_end", ROCKETNET_FUNCTIONAL_DEV_ONLY( control_disconnect_burst_each_end ), NULL },
    { "test_control_standalone_ack_each_end", ROCKETNET_FUNCTIONAL_DEV_ONLY( control_standalone_ack_each_end ), NULL },
    { "test_control_fragment_packets_each_end", ROCKETNET_FUNCTIONAL_DEV_ONLY( control_fragment_packets_each_end ), NULL },
    { "test_control_denied_when_server_full", ROCKETNET_FUNCTIONAL_DEV_ONLY( control_denied_when_server_full ), NULL },
    { "test_default_one_mib_messages_all_modes", ROCKETNET_FUNCTIONAL_DEV_ONLY( default_one_mib_messages_all_modes ), NULL },
    { "test_unreliable_small_messages_pack_in_place", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_small_messages_pack_in_place ), NULL },
    { "test_unreliable_over_receive_budget_dropped_and_counted", ROCKETNET_FUNCTIONAL_DEV_ONLY( unreliable_over_receive_budget_dropped_and_counted ), NULL },
    { "test_reliable_delivered_when_unreliable_budget_full", ROCKETNET_FUNCTIONAL_DEV_ONLY( reliable_delivered_when_unreliable_budget_full ), NULL },
    { "test_resend_timing_table_over_lossy_links", ROCKETNET_FUNCTIONAL_DEV_ONLY( resend_timing_table_over_lossy_links ), NULL },
    { "test_block_resend_cost_over_long_path", ROCKETNET_FUNCTIONAL_DEV_ONLY( block_resend_cost_over_long_path ), NULL },
    { "test_wire_totals_count_every_packet_and_byte", wire_totals_count_every_packet_and_byte, NULL },
    { "test_wire_totals_reset_per_connection_not_per_server", wire_totals_reset_per_connection_not_per_server, NULL },
    { "test_transport_wire_bytes_per_family", test_transport_wire_bytes_per_family, NULL },
    { "test_loopback_transport_live_message_matrix", ROCKETNET_FUNCTIONAL_DEV_ONLY( live_message_matrix_loopback ), NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_portable_transport_windows_server_drops_oversize_datagram_ipv4", windows_server_drops_oversize_datagram_portable_ipv4, NULL },
    { "test_portable_transport_windows_server_drops_oversize_datagram_ipv6", windows_server_drops_oversize_datagram_portable_ipv6, NULL },
    { "test_iocp_transport_windows_server_drops_oversize_datagram_ipv4", windows_server_drops_oversize_datagram_iocp_ipv4, NULL },
    { "test_iocp_transport_windows_server_drops_oversize_datagram_ipv6", windows_server_drops_oversize_datagram_iocp_ipv6, NULL },
    { "test_rio_transport_windows_server_drops_oversize_datagram_ipv4", windows_server_drops_oversize_datagram_rio_ipv4, NULL },
    { "test_rio_transport_windows_server_drops_oversize_datagram_ipv6", windows_server_drops_oversize_datagram_rio_ipv6, NULL },
    { "test_portable_transport_windows_server_drops_runt_and_garbage_ipv4", windows_server_drops_runt_and_garbage_portable_ipv4, NULL },
    { "test_portable_transport_windows_server_drops_runt_and_garbage_ipv6", windows_server_drops_runt_and_garbage_portable_ipv6, NULL },
    { "test_iocp_transport_windows_server_drops_runt_and_garbage_ipv4", windows_server_drops_runt_and_garbage_iocp_ipv4, NULL },
    { "test_iocp_transport_windows_server_drops_runt_and_garbage_ipv6", windows_server_drops_runt_and_garbage_iocp_ipv6, NULL },
    { "test_rio_transport_windows_server_drops_runt_and_garbage_ipv4", windows_server_drops_runt_and_garbage_rio_ipv4, NULL },
    { "test_rio_transport_windows_server_drops_runt_and_garbage_ipv6", windows_server_drops_runt_and_garbage_rio_ipv6, NULL },
    { "test_windows_server_counts_oversize_as_prefilter_too_large_iocp", windows_server_counts_oversize_as_prefilter_too_large_iocp, NULL },
    { "test_windows_server_counts_oversize_as_prefilter_too_large_rio", windows_server_counts_oversize_as_prefilter_too_large_rio, NULL },
    { "test_windows_server_counts_runt_as_prefilter_too_short_rio", windows_server_counts_runt_as_prefilter_too_short_rio, NULL },
    { "test_windows_client_drops_oversize_and_garbage_ipv4", windows_client_drops_oversize_and_garbage_ipv4, NULL },
    { "test_windows_client_drops_oversize_and_garbage_ipv6", windows_client_drops_oversize_and_garbage_ipv6, NULL },
    { "test_windows_per_packet_receive_counts_empty_datagram_ipv4", windows_per_packet_receive_counts_empty_datagram_ipv4, NULL },
    { "test_windows_per_packet_receive_counts_empty_datagram_ipv6", windows_per_packet_receive_counts_empty_datagram_ipv6, NULL },
    { "test_windows_lifecycle_ledger", rocketnet_test_windows_lifecycle_ledger, NULL },
    { "test_portable_transport_windows_init_term_1000", rocketnet_test_windows_init_term_1000_portable, NULL },
    { "test_portable_transport_windows_server_create_destroy_1000", rocketnet_test_windows_server_create_destroy_1000_portable, NULL },
    { "test_portable_transport_windows_client_create_destroy_1000", rocketnet_test_windows_client_create_destroy_1000_portable, NULL },
    { "test_portable_transport_windows_connect_disconnect_1000", rocketnet_test_windows_connect_disconnect_1000_portable, NULL },
    { "test_portable_transport_windows_server_restart_1000", rocketnet_test_windows_server_restart_1000_portable, NULL },
    { "test_iocp_transport_windows_init_term_1000", rocketnet_test_windows_init_term_1000_iocp, NULL },
    { "test_iocp_transport_windows_server_create_destroy_1000", rocketnet_test_windows_server_create_destroy_1000_iocp, NULL },
    { "test_iocp_transport_windows_client_create_destroy_1000", rocketnet_test_windows_client_create_destroy_1000_iocp, NULL },
    { "test_iocp_transport_windows_connect_disconnect_1000", rocketnet_test_windows_connect_disconnect_1000_iocp, NULL },
    { "test_iocp_transport_windows_server_restart_1000", rocketnet_test_windows_server_restart_1000_iocp, NULL },
    { "test_rio_transport_windows_init_term_1000", rocketnet_test_windows_init_term_1000_rio, NULL },
    { "test_rio_transport_windows_server_create_destroy_1000", rocketnet_test_windows_server_create_destroy_1000_rio, NULL },
    { "test_rio_transport_windows_client_create_destroy_1000", rocketnet_test_windows_client_create_destroy_1000_rio, NULL },
    { "test_rio_transport_windows_connect_disconnect_1000", rocketnet_test_windows_connect_disconnect_1000_rio, NULL },
    { "test_rio_transport_windows_server_restart_1000", rocketnet_test_windows_server_restart_1000_rio, NULL },
    { "test_portable_transport_windows_connreset_hard_killed_client_ipv4", windows_connreset_hard_killed_client_portable_ipv4, NULL },
    { "test_portable_transport_windows_connreset_hard_killed_client_ipv6", windows_connreset_hard_killed_client_portable_ipv6, NULL },
    { "test_iocp_transport_windows_connreset_hard_killed_client_ipv4", windows_connreset_hard_killed_client_iocp_ipv4, NULL },
    { "test_iocp_transport_windows_connreset_hard_killed_client_ipv6", windows_connreset_hard_killed_client_iocp_ipv6, NULL },
    { "test_iocp_transport_windows_connreset_hard_killed_client_plain_ipv4", windows_connreset_hard_killed_client_iocp_plain_ipv4, NULL },
    { "test_iocp_transport_windows_connreset_hard_killed_client_plain_ipv6", windows_connreset_hard_killed_client_iocp_plain_ipv6, NULL },
    { "test_rio_transport_windows_connreset_hard_killed_client_ipv4", windows_connreset_hard_killed_client_rio_ipv4, NULL },
    { "test_rio_transport_windows_connreset_hard_killed_client_ipv6", windows_connreset_hard_killed_client_rio_ipv6, NULL },
    { "test_portable_transport_windows_connreset_send_to_closed_port_ipv4", windows_connreset_send_to_closed_port_portable_ipv4, NULL },
    { "test_portable_transport_windows_connreset_send_to_closed_port_ipv6", windows_connreset_send_to_closed_port_portable_ipv6, NULL },
    { "test_iocp_transport_windows_connreset_send_to_closed_port_ipv4", windows_connreset_send_to_closed_port_iocp_ipv4, NULL },
    { "test_iocp_transport_windows_connreset_send_to_closed_port_ipv6", windows_connreset_send_to_closed_port_iocp_ipv6, NULL },
    { "test_iocp_transport_windows_connreset_send_to_closed_port_plain_ipv4", windows_connreset_send_to_closed_port_iocp_plain_ipv4, NULL },
    { "test_iocp_transport_windows_connreset_send_to_closed_port_plain_ipv6", windows_connreset_send_to_closed_port_iocp_plain_ipv6, NULL },
    { "test_rio_transport_windows_connreset_send_to_closed_port_ipv4", windows_connreset_send_to_closed_port_rio_ipv4, NULL },
    { "test_rio_transport_windows_connreset_send_to_closed_port_ipv6", windows_connreset_send_to_closed_port_rio_ipv6, NULL },
    { "test_windows_client_connreset_server_gone_ipv4", windows_client_connreset_server_gone_ipv4, NULL },
    { "test_windows_client_connreset_server_gone_ipv6", windows_client_connreset_server_gone_ipv6, NULL },
    { "test_windows_connreset_child_client", windows_connreset_child_client, NULL },
    { "test_windows_connreset_child_parent", windows_connreset_child_parent, NULL },
    { "test_windows_connreset_child_dies_with_its_parent", windows_connreset_child_dies_with_its_parent, NULL },
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_server_stats_creates_no_thread", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_server_stats_creates_no_thread ), NULL },
    { "test_public_step_driven_client_connects_without_a_thread", ROCKETNET_FUNCTIONAL_DEV_ONLY( rocketnet_test_public_step_driven_client_connects_without_a_thread ), NULL },
    { "test_session_reconnect_isolation_portable", rocketnet_test_session_reconnect_isolation_portable, NULL },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_session_reconnect_isolation_gso", rocketnet_test_session_reconnect_isolation_gso, NULL },
    { "test_session_reconnect_isolation_io_uring", rocketnet_test_session_reconnect_isolation_io_uring, rocketnet_server_uring_available },
#if ROCKETNET_XDP
    { "test_session_reconnect_isolation_xdp", rocketnet_test_session_reconnect_isolation_xdp, rocketnet_test_xdp_veth_runs_here },
#endif
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    { "test_session_reconnect_isolation_msgx", rocketnet_test_session_reconnect_isolation_msgx, NULL },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_session_reconnect_isolation_iocp", rocketnet_test_session_reconnect_isolation_iocp, NULL },
    { "test_session_reconnect_isolation_rio", rocketnet_test_session_reconnect_isolation_rio, NULL },
#endif
};

static rocketnet_test_environment_t * rocketnet_functional_environment = NULL;

void rocketnet_functional_restore_log_function()
{
    rocketnet_log_function( rocketnet_test_suite_log_function, NULL );
}

void rocketnet_functional_prepare()
{
    char verbose[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
    {
        rocketnet_test_suite_log_function = rocketnet_test_log_capture;
        rocketnet_log_function( rocketnet_test_suite_log_function, NULL );
    }
    rocketnet_forbid_xdp_commit();
    if ( rocketnet_functional_environment == NULL )
        rocketnet_functional_environment = new rocketnet_test_environment_t();
}

#if !ROCKETNET_DEV

// ./bin/functional_production's exclusion list, from CMakeLists.txt: one
// space-separated string of case names. The list is exact both ways: every
// case whose function is absent in this build is on it, and every name on it
// is a case of the table, so a case cannot leave the production run unnamed
// and the list cannot name a case that does not exist.
static const char rocketnet_functional_production_excluded[] = ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED;

static bool rocketnet_functional_production_excludes( const char * name )
{
    const size_t length = strlen( name );
    const char * p = rocketnet_functional_production_excluded;
    while ( *p )
    {
        while ( *p == ' ' )
            p++;
        const char * end = p;
        while ( *end && *end != ' ' )
            end++;
        if ( (size_t) ( end - p ) == length && memcmp( p, name, length ) == 0 )
            return true;
        p = end;
    }
    return false;
}

static bool rocketnet_functional_production_exclusions_hold()
{
    const int count = (int) ( sizeof( rocketnet_functional_tests ) / sizeof( rocketnet_functional_tests[0] ) );
    bool hold = true;
    for ( int i = 0; i < count; i++ )
    {
        if ( rocketnet_functional_tests[i].function == NULL && !rocketnet_functional_production_excludes( rocketnet_functional_tests[i].name ) )
        {
            printf( "check failed: %s is not built against the production library and is not in ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED_CASES\n", rocketnet_functional_tests[i].name );
            hold = false;
        }
    }
    const char * p = rocketnet_functional_production_excluded;
    while ( *p )
    {
        while ( *p == ' ' )
            p++;
        const char * end = p;
        while ( *end && *end != ' ' )
            end++;
        if ( end == p )
            break;
        bool found = false;
        for ( int i = 0; i < count && !found; i++ )
            found = strlen( rocketnet_functional_tests[i].name ) == (size_t) ( end - p ) && memcmp( rocketnet_functional_tests[i].name, p, (size_t) ( end - p ) ) == 0;
        if ( !found )
        {
            printf( "check failed: ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED_CASES names %.*s, which is not a functional case\n", (int) ( end - p ), p );
            hold = false;
        }
        p = end;
    }
    return hold;
}

#endif // #if !ROCKETNET_DEV

// the hard-killed client and ICMP-error cases (functional/posix_hard_killed_client.cpp)
// are a generated table of their own, one case per transport cell, after the
// static table. The xdp server cell is in the static table, gated by
// rocketnet_test_xdp_veth_runs_here, and is not repeated here.

int rocketnet_posix_hard_killed_client_test_count();
const char * rocketnet_posix_hard_killed_client_test_name( int index );
void rocketnet_posix_hard_killed_client_test_run( int index );

static const int rocketnet_functional_num_tests = (int) ( sizeof( rocketnet_functional_tests ) / sizeof( rocketnet_functional_tests[0] ) );

// the table entries that run on this host, in table order
static int rocketnet_functional_here[rocketnet_functional_num_tests];

static int rocketnet_functional_static_test_count()
{
    static int count = -1;
    if ( count < 0 )
    {
        count = 0;
#if !ROCKETNET_DEV
        if ( !rocketnet_functional_production_exclusions_hold() )
        {
            fflush( stdout );
            exit( 1 );
        }
#endif // #if !ROCKETNET_DEV
        for ( int i = 0; i < rocketnet_functional_num_tests; i++ )
        {
#if !ROCKETNET_DEV
            if ( rocketnet_functional_production_excludes( rocketnet_functional_tests[i].name ) )
                continue;
#endif // #if !ROCKETNET_DEV
            if ( !rocketnet_functional_tests[i].runs_here || rocketnet_functional_tests[i].runs_here() )
                rocketnet_functional_here[count++] = i;
        }
    }
    return count;
}

#if ROCKETNET_DEV
int rocketnet_functional_test_count()
{
    return rocketnet_functional_static_test_count() + rocketnet_posix_hard_killed_client_test_count();
}

const char * rocketnet_functional_test_name( int index )
{
    if ( index >= rocketnet_functional_static_test_count() )
        return rocketnet_posix_hard_killed_client_test_name( index - rocketnet_functional_static_test_count() );
    return rocketnet_functional_tests[rocketnet_functional_here[index]].name;
}
#else // #if ROCKETNET_DEV
// the generated hard-killed client table is not in the production run: its
// source mints development tokens (CMakeLists.txt, ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED_SOURCES)
int rocketnet_functional_test_count()
{
    return rocketnet_functional_static_test_count();
}

const char * rocketnet_functional_test_name( int index )
{
    return rocketnet_functional_tests[rocketnet_functional_here[index]].name;
}
#endif // #if ROCKETNET_DEV

int rocketnet_functional_run_named( const char * name )
{
    for ( int i = 0; i < rocketnet_functional_num_tests; i++ )
    {
        if ( strcmp( rocketnet_functional_tests[i].name, name ) != 0 )
            continue;
        if ( rocketnet_functional_tests[i].runs_here && !rocketnet_functional_tests[i].runs_here() )
        {
            printf( "check failed: ( runs_here() ), test %s cannot run on this host\n", name );
            return 1;
        }
#if !ROCKETNET_DEV
        if ( !rocketnet_functional_production_exclusions_hold() )
            return 1;
        if ( rocketnet_functional_production_excludes( name ) )
        {
            printf( "check failed: %s needs development scaffolding and is excluded from the production run (ROCKETNET_FUNCTIONAL_PRODUCTION_EXCLUDED_CASES)\n", name );
            return 1;
        }
#endif // #if !ROCKETNET_DEV
        rocketnet_test_log_ring_clear();
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s", name );
        fflush( stdout );
        rocketnet_functional_tests[i].function();
        return 0;
    }
#if ROCKETNET_DEV
    for ( int i = 0; i < rocketnet_posix_hard_killed_client_test_count(); i++ )
    {
        if ( strcmp( rocketnet_posix_hard_killed_client_test_name( i ), name ) != 0 )
            continue;
        rocketnet_test_log_ring_clear();
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s", name );
        fflush( stdout );
        rocketnet_posix_hard_killed_client_test_run( i );
        return 0;
    }
#endif // #if ROCKETNET_DEV
    fprintf( stderr, "unknown functional test %s\n", name );
    return 2;
}
