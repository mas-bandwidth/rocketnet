/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Receive latency: a functional test (real sockets on 127.0.0.1, real time,
    threads) of how many updates a message waits between its send and the
    receive call that returns it. Over loopback the wire takes microseconds,
    so a message sent well inside a frame must be returned by the first update
    after its send. A transport whose worker-to-main handoff holds a datagram
    back returns it one update late (sdk/rocketnet_receive_delivery_check.h is
    the same rule as a unit test, with no time).

    The server updates at RECEIVE_LATENCY_RATE on the case's thread and the two
    clients run on a second thread. Each frame, every message goes out at 15%
    and again at 55% of the frame, so the receiver's handoff gets two datagrams
    a frame some time apart. Each row is one transport pair and one direction,
    and is one case:

      server  each client sends the server a message at both phases, timed
              from the server's update; the receive call is the server's
              update.
      client  the server sends each client a message at both phases, between
              its updates; the clients update on their own thread at 75% of
              the frame, and the receive call is the client's update. The
              row measures the client transport it names (the I/O thread's
              receive handoff, sdk/rocketnet_receive_handoff.h); its server
              may be any.

    A message is late when the receive call that returned it is not the first
    one to begin after its send. The case asserts on the count of late
    messages, never on wall time: at most RECEIVE_LATENCY_LATE_PERCENT of them.
    A rule that holds datagrams back an update makes at least the second
    message of each frame late (half of all, and nearly all in practice); a
    loaded runner that stalls a thread for a fifth of a frame makes a few late.
    It also requires every message to arrive. ROCKETNET_RECEIVE_LATENCY_REPORT
    names a file each run appends its counts to, for a repeated run (a passing
    case's output is not shown). To add a transport, add a row to
    receive_latency_rows and register its case in rocketnet_functional_tests.cpp.
*/

#include "../../examples/shutdown.h"
#include "../time_scale.h"
#include "../token_string.h"
#include "rocketnet.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

// at a case time scale of 1; a scaled run (Debug, sanitizers, emulation)
// slows the rate and shortens the run by its scale, so a thread the host
// stalls for a third of a frame stays as rare and the case's wall time stays
// about five seconds
#define RECEIVE_LATENCY_RATE 20.0
#define RECEIVE_LATENCY_FRAMES 100
#define RECEIVE_LATENCY_MIN_FRAMES 20
#define RECEIVE_LATENCY_CLIENTS 2
#define RECEIVE_LATENCY_PHASES 2
#define RECEIVE_LATENCY_CLIENT_PHASE 0.75
#define RECEIVE_LATENCY_LATE_PERCENT 10
#define RECEIVE_LATENCY_PROTOCOL_ID 0x524c4154454e4359ULL

uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

// any filter but the published test filter, which a production library refuses
static const uint8_t receive_latency_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x52, 0x4C, 0x41, 0x54 };

enum receive_latency_direction_t
{
    RECEIVE_LATENCY_SERVER,
    RECEIVE_LATENCY_CLIENT,
};

struct receive_latency_row_t
{
    const char * name;
    const char * server_transport;
    const char * client_transport;
    enum receive_latency_direction_t direction;
};

// the rows: one case each (rocketnet_functional_tests.cpp registers them)
static const struct receive_latency_row_t receive_latency_rows[] = {
    { "server_gso", "gso", "portable", RECEIVE_LATENCY_SERVER },
    { "server_io_uring", "io_uring", "portable", RECEIVE_LATENCY_SERVER },
    { "client_portable", "portable", "portable", RECEIVE_LATENCY_CLIENT },
    { "client_gso", "portable", "gso", RECEIVE_LATENCY_CLIENT },
    { "client_msgx", "portable", "msgx", RECEIVE_LATENCY_CLIENT },
};

// the phase of each send in the frame
static const double receive_latency_phase[RECEIVE_LATENCY_PHASES] = { 0.15, 0.55 };

// message frame, phase, client: each message's index and sequence
static uint32_t receive_latency_sequence( int frame, int phase, int client )
{
    return (uint32_t) ( ( frame * RECEIVE_LATENCY_PHASES + phase ) * RECEIVE_LATENCY_CLIENTS + client );
}

struct receive_latency_payload_t
{
    uint32_t sequence;
    uint32_t pad;
    double send_time;
};

static void receive_latency_check( bool condition, const char * row, const char * what )
{
    if ( condition )
    {
        return;
    }
    printf( "check failed: receive latency %s: %s\n", row, what );
    fflush( stdout );
    exit( 1 );
}

// the index of the first update in times[0 .. count) that began at or after t
static int receive_latency_first_after( const std::vector<double> & times, double t )
{
    int lo = 0;
    int hi = (int) times.size();
    while ( lo < hi )
    {
        const int mid = ( lo + hi ) / 2;
        if ( times[(size_t) mid] < t )
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

// sleeps to within a millisecond of due, then spins to it
static double receive_latency_wait_until( double due )
{
    double now = rocketnet_time();
    if ( due - now > 0.002 )
    {
        rocketnet_sleep( due - now - 0.001 );
    }
    while ( ( now = rocketnet_time() ) < due )
    {
    }
    return now;
}

static bool receive_latency_send( struct rocketnet_client_t * client, uint32_t sequence )
{
    struct receive_latency_payload_t payload;
    memset( &payload, 0, sizeof( payload ) );
    payload.sequence = sequence;
    if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( payload ) ) )
    {
        return false;
    }
    uint8_t * data = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( payload ) );
    if ( !data )
    {
        return false;
    }
    payload.send_time = rocketnet_time();
    memcpy( data, &payload, sizeof( payload ) );
    if ( !rocketnet_client_finish_message( client, (int) sizeof( payload ) ) )
    {
        return false;
    }
    rocketnet_client_send_packets( client );
    return true;
}

static void receive_latency_pump( struct rocketnet_client_t * client )
{
    rocketnet_client_update( client, rocketnet_time() );
    while ( rocketnet_client_receive_messages( client ) != NULL )
    {
    }
    rocketnet_client_send_packets( client );
}

static void receive_latency_run( const struct receive_latency_row_t * row )
{
    const char * name = row->name;

    double env_scale = 1.0;
    receive_latency_check( rocketnet_test_case_env_time_scale( &env_scale ) != 0, name, "ROCKETNET_TEST_CASE_TIME_SCALE is not a number in (0, 1000]" );
    const double scale = ROCKETNET_TEST_CASE_TIME_SCALE * env_scale;
    const double rate = RECEIVE_LATENCY_RATE / scale;
    const int scaled_frames = (int) ( RECEIVE_LATENCY_FRAMES / scale );
    const int frames = scaled_frames > RECEIVE_LATENCY_MIN_FRAMES ? scaled_frames : RECEIVE_LATENCY_MIN_FRAMES;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x6C ^ i );
    }
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[64];
    receive_latency_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK, name, "backend keypair" );

    struct rocketnet_server_config_t server_config;
    receive_latency_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK, name, "server default config" );
    server_config.protocol_id = RECEIVE_LATENCY_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    memcpy( server_config.packet_filter, receive_latency_packet_filter, sizeof( server_config.packet_filter ) );
    server_config.transport = row->server_transport;
    server_config.max_clients = RECEIVE_LATENCY_CLIENTS;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    receive_latency_check( server && status == ROCKETNET_STATUS_OK, name, "server create" );
    receive_latency_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK, name, "server start" );

    // a build or kernel without the transport falls to another one through
    // the pin's own fallback (rocketnet_test_server_pin_allows_io_uring): the
    // row measures only the transport it names. A client row measures the
    // client's transport, so its server may be any (a leg's server pin).
    if ( row->direction == RECEIVE_LATENCY_SERVER && strcmp( rocketnet_server_transport_name( server ), row->server_transport ) != 0 )
    {
        printf( "receive latency %s: granted %s, not %s: nothing to measure\n", name, rocketnet_server_transport_name( server ), row->server_transport );
        fflush( stdout );
        rn_cli_server_stop_and_destroy( server );
        return;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    receive_latency_check( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ), name, "server address" );
    const char * server_addresses[] = { server_address };

    struct rocketnet_client_config_t client_config;
    receive_latency_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK, name, "client default config" );
    client_config.transport = row->client_transport;
    memcpy( client_config.packet_filter, receive_latency_packet_filter, sizeof( client_config.packet_filter ) );

    struct rocketnet_client_t * clients[RECEIVE_LATENCY_CLIENTS];
    for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
    {
        const uint64_t create_timestamp = rocketnet_functional_token_timestamp( 0, rocketnet_time() );
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        receive_latency_check( rn_test_server_token_string( backend_secret_key, server, RECEIVE_LATENCY_PROTOCOL_ID, server_addresses, 1, (uint64_t) c + 1,
                                                            create_timestamp, create_timestamp + 120, 10, NULL, token ) == ROCKETNET_STATUS_OK,
                               name, "connect token" );
        clients[c] = rocketnet_client_create( NULL, &client_config, &status );
        receive_latency_check( clients[c] && status == ROCKETNET_STATUS_OK, name, "client create" );
        receive_latency_check( rocketnet_client_connect( clients[c], token ) == ROCKETNET_STATUS_OK, name, "client connect" );
    }

    // connect, both sides on this thread

    const double connect_deadline = rocketnet_time() + 10.0;
    for ( ;; )
    {
        rocketnet_server_update( server, rocketnet_time() );
        while ( rocketnet_server_receive_messages( server ) != NULL )
        {
        }
        rocketnet_server_send_packets( server );
        int connected = 0;
        for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
        {
            receive_latency_pump( clients[c] );
            connected += rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED;
        }
        if ( connected == RECEIVE_LATENCY_CLIENTS )
        {
            break;
        }
        receive_latency_check( rocketnet_time() < connect_deadline, name, "clients did not connect within 10 s" );
        rocketnet_sleep( 0.001 );
    }

    int slot[RECEIVE_LATENCY_CLIENTS];
    for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
    {
        slot[c] = rocketnet_client_index( clients[c] );
        receive_latency_check( slot[c] >= 0, name, "client has no slot" );
    }

    // a client row measures only the client transport it names: a platform
    // or build without it grants another
    if ( row->direction == RECEIVE_LATENCY_CLIENT )
    {
        const char * granted = rocketnet_client_transport_name( clients[0] );
        if ( !granted || strcmp( granted, row->client_transport ) != 0 )
        {
            printf( "receive latency %s: client granted %s, not %s: nothing to measure\n", name, granted ? granted : "none", row->client_transport );
            fflush( stdout );
            for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
            {
                rn_cli_client_stop_and_destroy( clients[c], rocketnet_time() );
            }
            rn_cli_server_stop_and_destroy( server );
            return;
        }
    }

    const int total = frames * RECEIVE_LATENCY_PHASES * RECEIVE_LATENCY_CLIENTS;
    std::vector<double> send_time( (size_t) total, 0.0 );
    std::vector<int> received_at( (size_t) total, -1 );
    // both sides update frames + 2 times, the last two after the last send
    std::vector<double> server_update( (size_t) frames + 2, 0.0 );
    std::vector<double> client_update( (size_t) frames + 2, 0.0 );

    // the server's update starts, published to the client thread
    std::atomic<int> server_updates( 0 );
    std::atomic<double> last_update( 0.0 );
    std::atomic<bool> stop( false );

    const double start = rocketnet_time() + 0.1;

    std::thread client_thread( [&]()
                               {
        if ( row->direction == RECEIVE_LATENCY_SERVER )
        {
            // each frame: every client sends at each phase, timed from the
            // server's update
            int sent_frame = -1;
            while ( !stop.load() )
            {
                for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
                {
                    receive_latency_pump( clients[c] );
                }
                const int k = server_updates.load() - 1;
                if ( k > sent_frame && k < frames )
                {
                    const double begun = last_update.load();
                    for ( int phase = 0; phase < RECEIVE_LATENCY_PHASES; phase++ )
                    {
                        receive_latency_wait_until( begun + receive_latency_phase[phase] / rate );
                        for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
                        {
                            receive_latency_check( receive_latency_send( clients[c], receive_latency_sequence( k, phase, c ) ), name, "client send refused" );
                        }
                    }
                    sent_frame = k;
                }
                rocketnet_sleep( 0.0005 );
            }
            return;
        }

        // client direction: the clients update late in each frame, two frames
        // past the server's last send
        for ( int j = 0; j < frames + 2; j++ )
        {
            const double now = receive_latency_wait_until( start + ( j + RECEIVE_LATENCY_CLIENT_PHASE ) / rate );
            client_update[(size_t) j] = now;
            for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
            {
                rocketnet_client_update( clients[c], now );
                const struct rocketnet_message_batch_t * batch;
                while ( ( batch = rocketnet_client_receive_messages( clients[c] ) ) != NULL )
                {
                    for ( int i = 0; i < batch->num_messages; i++ )
                    {
                        struct receive_latency_payload_t payload;
                        if ( batch->bytes[i] != (int) sizeof( payload ) )
                        {
                            continue;
                        }
                        memcpy( &payload, batch->data[i], sizeof( payload ) );
                        if ( payload.sequence < (uint32_t) total && received_at[payload.sequence] < 0 )
                        {
                            received_at[payload.sequence] = j;
                            send_time[payload.sequence] = payload.send_time;
                        }
                    }
                }
                rocketnet_client_send_packets( clients[c] );
            }
        } } );

    // the server, on an absolute schedule

    for ( int k = 0; k < frames + 2; k++ )
    {
        const double now = receive_latency_wait_until( start + (double) k / rate );
        server_update[(size_t) k] = now;
        rocketnet_server_update( server, now );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                struct receive_latency_payload_t payload;
                if ( row->direction != RECEIVE_LATENCY_SERVER || batch->bytes[i] != (int) sizeof( payload ) )
                {
                    continue;
                }
                memcpy( &payload, batch->data[i], sizeof( payload ) );
                if ( payload.sequence < (uint32_t) total && received_at[payload.sequence] < 0 )
                {
                    received_at[payload.sequence] = k;
                    send_time[payload.sequence] = payload.send_time;
                }
            }
        }
        rocketnet_server_send_packets( server );
        if ( row->direction == RECEIVE_LATENCY_CLIENT && k < frames )
        {
            // each frame: the server sends every client a message at each phase
            for ( int phase = 0; phase < RECEIVE_LATENCY_PHASES; phase++ )
            {
                receive_latency_wait_until( now + receive_latency_phase[phase] / rate );
                for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
                {
                    struct receive_latency_payload_t payload;
                    memset( &payload, 0, sizeof( payload ) );
                    payload.sequence = receive_latency_sequence( k, phase, c );
                    payload.send_time = rocketnet_time();
                    uint8_t * data = rocketnet_server_start_message( server, (uint64_t) slot[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( payload ) );
                    receive_latency_check( data != NULL, name, "server send refused" );
                    memcpy( data, &payload, sizeof( payload ) );
                    receive_latency_check( rocketnet_server_finish_message( server, (int) sizeof( payload ) ) != 0, name, "server finish refused" );
                }
                rocketnet_server_send_packets( server );
            }
        }
        last_update.store( now );
        server_updates.store( k + 1 );
    }

    stop.store( true );
    client_thread.join();

    // count: a message is late when the update that returned it is not the
    // first to begin after its send

    const std::vector<double> & updates = row->direction == RECEIVE_LATENCY_SERVER ? server_update : client_update;
    int late = 0;
    int lost = 0;
    double sum = 0.0;
    double worst = 0.0;
    for ( int s = 0; s < total; s++ )
    {
        if ( received_at[(size_t) s] < 0 )
        {
            lost++;
            continue;
        }
        const int due = receive_latency_first_after( updates, send_time[(size_t) s] );
        late += received_at[(size_t) s] > due ? 1 : 0;
        const double wait = ( updates[(size_t) received_at[(size_t) s]] - send_time[(size_t) s] ) * rate;
        sum += wait;
        worst = wait > worst ? wait : worst;
    }

    char line[256];
    snprintf( line, sizeof( line ), "receive latency %s: %d messages, %d late, %d lost, wait in updates mean %.3f max %.3f\n", name, total, late, lost,
              total > lost ? sum / ( total - lost ) : 0.0, worst );
    printf( "%s", line );
    fflush( stdout );

    // a passing case's output is not shown (tests/suite_runner.h): a repeated
    // run that wants the counts names a file to append them to
    char report[512];
    if ( rocketnet_test_getenv( "ROCKETNET_RECEIVE_LATENCY_REPORT", report, sizeof( report ) ) && report[0] != '\0' )
    {
        FILE * file = rocketnet_test_fopen( report, "a" );
        if ( file )
        {
            fputs( line, file );
            fclose( file );
        }
    }

    for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
    {
        rn_cli_client_stop_and_destroy( clients[c], rocketnet_time() );
    }
    rn_cli_server_stop_and_destroy( server );

    receive_latency_check( lost == 0, name, "a message never arrived" );
    receive_latency_check( late * 100 <= total * RECEIVE_LATENCY_LATE_PERCENT, name, "more messages than the bound waited an extra update" );
}

static void receive_latency_run_named( const char * row_name )
{
    for ( size_t i = 0; i < sizeof( receive_latency_rows ) / sizeof( receive_latency_rows[0] ); i++ )
    {
        if ( strcmp( receive_latency_rows[i].name, row_name ) == 0 )
        {
            receive_latency_run( &receive_latency_rows[i] );
            return;
        }
    }
    receive_latency_check( false, row_name, "no such row" );
}

void rocketnet_test_receive_latency_server_gso();
void rocketnet_test_receive_latency_server_io_uring();
void rocketnet_test_receive_latency_client_portable();
void rocketnet_test_receive_latency_client_gso();
void rocketnet_test_receive_latency_client_msgx();

void rocketnet_test_receive_latency_server_gso()
{
    receive_latency_run_named( "server_gso" );
}

void rocketnet_test_receive_latency_server_io_uring()
{
    receive_latency_run_named( "server_io_uring" );
}

void rocketnet_test_receive_latency_client_portable()
{
    receive_latency_run_named( "client_portable" );
}

void rocketnet_test_receive_latency_client_gso()
{
    receive_latency_run_named( "client_gso" );
}

void rocketnet_test_receive_latency_client_msgx()
{
    receive_latency_run_named( "client_msgx" );
}
