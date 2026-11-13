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
    It also requires every message to arrive. A client row in a DEV build also
    requires that the clients' update opened no session packet on its own
    thread (rocketnet_crypto_tally): once connected, the I/O thread opens
    every payload (proofs/tla/RNClientIoDecrypt.tla OpenPayloads). To add a
    transport, add a row to receive_latency_rows and register its case in
    rocketnet_functional_tests.cpp.

    The xdp row runs only where the veth cases run (root in the xdp guest,
    rocketnet_test_xdp_veth_runs_here): the server binds the near end of the
    case's veth pair, and the clients connect from the far end's network
    namespace, where their sockets stay for the case.
*/

#include "../../examples/shutdown.h"
#include "../time_scale.h"
#include "../token_string.h"
#include "rocketnet.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_driver.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
#include "../native_shutdown.h"
#include "rocketnet_server_socket.h"
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
struct rocketnet_server_transport_t * rocketnet_xdp_veth_start( bool ipv6, const char * case_name, char * server_address, size_t server_address_bytes );
void rocketnet_xdp_veth_note_server( struct rocketnet_server_transport_t * server );
void rocketnet_xdp_veth_pair_down( struct rocketnet_server_transport_t * destroyed );
const char * rocketnet_xdp_veth_netns_path();
const char * rocketnet_xdp_veth_host( bool ipv6, bool far );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

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
    bool veth; // the server on the near end of the case's veth pair, the clients on the far end
};

// the rows: one case each (rocketnet_functional_tests.cpp registers them)
static const struct receive_latency_row_t receive_latency_rows[] = {
    { "server_gso", "gso", "portable", RECEIVE_LATENCY_SERVER, false },
    { "server_io_uring", "io_uring", "portable", RECEIVE_LATENCY_SERVER, false },
    { "server_xdp", "xdp", "portable", RECEIVE_LATENCY_SERVER, true },
    { "server_rio", "rio", "portable", RECEIVE_LATENCY_SERVER, false },
    { "server_iocp", "iocp", "portable", RECEIVE_LATENCY_SERVER, false },
    { "client_portable", "portable", "portable", RECEIVE_LATENCY_CLIENT, false },
    { "client_gso", "portable", "gso", RECEIVE_LATENCY_CLIENT, false },
    { "client_msgx", "portable", "msgx", RECEIVE_LATENCY_CLIENT, false },
    { "client_iocp", "iocp", "iocp", RECEIVE_LATENCY_CLIENT, false },
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

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

// the veth row's teardown, on every path out of the case: the pair down and
// the two environment variables back as they were

static void receive_latency_veth_down( const char * name, bool restore_xdp_threads, const char * saved_xdp_threads, bool restore_os_cpus, const char * saved_os_cpus )
{
    rocketnet_xdp_veth_pair_down( NULL );
    receive_latency_check( restore_xdp_threads ? setenv( "ROCKETNET_SERVER_NUM_XDP_THREADS", saved_xdp_threads, 1 ) == 0
                                               : unsetenv( "ROCKETNET_SERVER_NUM_XDP_THREADS" ) == 0,
                           name, "xdp threads restored" );
    receive_latency_check( restore_os_cpus ? setenv( "ROCKETNET_SERVER_NUM_OS_CPUS", saved_os_cpus, 1 ) == 0 : unsetenv( "ROCKETNET_SERVER_NUM_OS_CPUS" ) == 0,
                           name, "os cpus restored" );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

// the io_uring row in a build without liburing: the transport has no
// backend, and its documented fallback (rocketnet_test_server_pin_allows_io_uring)
// runs the row on another transport, which the case does not measure
static bool receive_latency_io_uring_unbuilt( const struct receive_latency_row_t * row )
{
    return strcmp( row->server_transport, "io_uring" ) == 0 && !rocketnet_transport_definition( ROCKETNET_TRANSPORT_IO_URING )->backend;
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

    char bind_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES] = "127.0.0.1:0";
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    // the pair comes up with a transport of its own, which is then
    // destroyed; the server takes the near end with one xdp thread
    char saved_xdp_threads[64] = { 0 };
    char saved_os_cpus[64] = { 0 };
    const char * had_xdp_threads = getenv( "ROCKETNET_SERVER_NUM_XDP_THREADS" );
    const char * had_os_cpus = getenv( "ROCKETNET_SERVER_NUM_OS_CPUS" );
    if ( had_xdp_threads )
    {
        snprintf( saved_xdp_threads, sizeof( saved_xdp_threads ), "%s", had_xdp_threads );
    }
    if ( had_os_cpus )
    {
        snprintf( saved_os_cpus, sizeof( saved_os_cpus ), "%s", had_os_cpus );
    }
    const bool restore_xdp_threads = had_xdp_threads != NULL;
    const bool restore_os_cpus = had_os_cpus != NULL;
    if ( row->veth )
    {
        char veth_address[128];
        struct rocketnet_server_transport_t * pair = rocketnet_xdp_veth_start( false, "test_receive_latency_server_xdp", veth_address, sizeof( veth_address ) );
        receive_latency_check( pair != NULL, name, "veth pair" );
        rocketnet_xdp_veth_note_server( NULL );
        rocketnet_server_transport_stop( pair );
        for ( int i = 0; i < 100 && rocketnet_server_transport_state( pair ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; ++i )
        {
            rocketnet_server_transport_update( pair, rocketnet_platform_time() );
            rocketnet_platform_sleep( 0.001 );
        }
        rn_native_server_transport_stop_and_destroy( pair );
#if ROCKETNET_DEV
        rocketnet_server_socket_allow_commit();
#endif // #if ROCKETNET_DEV
        receive_latency_check( setenv( "ROCKETNET_SERVER_NUM_XDP_THREADS", "1", 1 ) == 0, name, "xdp threads" );
        receive_latency_check( setenv( "ROCKETNET_SERVER_NUM_OS_CPUS", "1", 1 ) == 0, name, "os cpus" );
        snprintf( bind_address, sizeof( bind_address ), "%s", veth_address ); // the pair's port, which its capture records
    }
#else  // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    receive_latency_check( !row->veth, name, "a veth row in a build without xdp" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, bind_address, NULL, &server_config, &status );
    receive_latency_check( server && status == ROCKETNET_STATUS_OK, name, "server create" );
    receive_latency_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK, name, "server start" );

    // a server row measures the server transport it names. A leg whose server
    // pin excludes it, or a host that cannot grant it, skips the row through
    // its predicate before it runs (SKIPPED with that predicate's name,
    // expected_skips.txt); a row that runs asked for a transport the leg
    // allows, so being granted another fails the case. The one fallback that
    // passes is the io_uring row's documented one
    // (rocketnet_test_server_pin_allows_io_uring): a build without liburing
    // still runs the row and the request falls to portable, which the case
    // reports and does not measure. A client row measures the client's
    // transport, so its server may be any (a leg's server pin).
    const char * server_granted = rocketnet_server_transport_name( server );
    if ( row->direction == RECEIVE_LATENCY_SERVER && ( !server_granted || strcmp( server_granted, row->server_transport ) != 0 ) )
    {
        const bool unbuilt_io_uring = receive_latency_io_uring_unbuilt( row );
        printf( "receive latency %s: granted %s, not %s: %s\n", name, server_granted ? server_granted : "none", row->server_transport,
                unbuilt_io_uring ? "io_uring is not built, so the pin falls to another transport: nothing to measure" : "the requested transport was not granted" );
        fflush( stdout );
        rn_cli_server_stop_and_destroy( server );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
        if ( row->veth )
        {
            receive_latency_veth_down( name, restore_xdp_threads, saved_xdp_threads, restore_os_cpus, saved_os_cpus );
        }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
        receive_latency_check( unbuilt_io_uring, name, "the server was granted another transport than the row measures" );
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
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
        if ( row->veth )
        {
            // the client's socket opens in the far namespace and stays there
            const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
            const int far_ns = open( rocketnet_xdp_veth_netns_path(), O_RDONLY );
            receive_latency_check( self_ns >= 0 && far_ns >= 0 && setns( far_ns, CLONE_NEWNET ) == 0, name, "enter the far namespace" );
            status = rocketnet_client_connect( clients[c], token );
            receive_latency_check( setns( self_ns, CLONE_NEWNET ) == 0, name, "leave the far namespace" );
            close( self_ns );
            close( far_ns );
            receive_latency_check( status == ROCKETNET_STATUS_OK, name, "client connect" );
            continue;
        }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
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

    // a client row measures the client transport it names, and is registered
    // only where that transport ships and no pin excludes it: a client
    // granted any other transport fails the row, so a leg meant to measure it
    // never passes having measured something else
    if ( row->direction == RECEIVE_LATENCY_CLIENT )
    {
        for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
        {
            const char * granted = rocketnet_client_transport_name( clients[c] );
            if ( !granted || strcmp( granted, row->client_transport ) != 0 )
            {
                printf( "receive latency %s: client %d granted %s, not %s\n", name, c, granted ? granted : "none", row->client_transport );
                fflush( stdout );
                receive_latency_check( false, name, "the client was granted another transport than the row measures" );
            }
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
    // the session packets the clients' update opened on the client thread
    uint64_t client_thread_opens = 0;

    const double start = rocketnet_time() + 0.1;

    std::thread client_thread( [&]()
                               {
        if ( row->direction == RECEIVE_LATENCY_SERVER )
        {
            // each frame: every client sends at each phase, timed from the
            // server's update. A frame the thread missed (the host stalled it
            // past an update) is sent at once, so every message is still
            // sent; its lateness is measured from its own send time
            int sent_frame = -1;
            while ( !stop.load() )
            {
                for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
                {
                    receive_latency_pump( clients[c] );
                }
                const int k = server_updates.load() - 1;
                const int last = k < frames ? k : frames - 1;
                for ( int f = sent_frame + 1; f <= last; f++ )
                {
                    const double begun = last_update.load();
                    for ( int phase = 0; phase < RECEIVE_LATENCY_PHASES; phase++ )
                    {
                        if ( f == k )
                        {
                            receive_latency_wait_until( begun + receive_latency_phase[phase] / rate );
                        }
                        for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
                        {
                            receive_latency_check( receive_latency_send( clients[c], receive_latency_sequence( f, phase, c ) ), name, "client send refused" );
                        }
                    }
                    sent_frame = f;
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
#if ROCKETNET_DEV
                const uint64_t opens_before = rocketnet_crypto_tally_get().aead_open;
#endif // #if ROCKETNET_DEV
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
#if ROCKETNET_DEV
                client_thread_opens += rocketnet_crypto_tally_get().aead_open - opens_before;
#endif // #if ROCKETNET_DEV
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

    // wait is in updates; reported in milliseconds at the rate run, updates after
    const double mean = total > lost ? sum / ( total - lost ) : 0.0;
    printf( "receive latency %s: %d messages, %d late, %d lost, wait mean %.1f ms max %.1f ms at %.0f Hz (mean %.3f max %.3f updates)\n", name,
            total, late, lost, mean * 1000.0 / rate, worst * 1000.0 / rate, rate, mean, worst );
    fflush( stdout );

    for ( int c = 0; c < RECEIVE_LATENCY_CLIENTS; c++ )
    {
        rn_cli_client_stop_and_destroy( clients[c], rocketnet_time() );
    }
    rn_cli_server_stop_and_destroy( server );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( row->veth )
    {
        receive_latency_veth_down( name, restore_xdp_threads, saved_xdp_threads, restore_os_cpus, saved_os_cpus );
    }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP

    receive_latency_check( lost == 0, name, "a message never arrived" );
    char too_late[128];
    snprintf( too_late, sizeof( too_late ), "more messages than the bound waited an extra %.1f ms at %.0f Hz (one update)", 1000.0 / rate, rate );
    receive_latency_check( late * 100 <= total * RECEIVE_LATENCY_LATE_PERCENT, name, too_late );
    receive_latency_check( client_thread_opens == 0, name, "the clients' update opened session packets the I/O thread passed raw" );
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
void rocketnet_test_receive_latency_server_xdp();
void rocketnet_test_receive_latency_client_portable();
void rocketnet_test_receive_latency_client_gso();
void rocketnet_test_receive_latency_client_msgx();
void rocketnet_test_receive_latency_client_iocp();

void rocketnet_test_receive_latency_server_gso()
{
    receive_latency_run_named( "server_gso" );
}

void rocketnet_test_receive_latency_server_io_uring()
{
    receive_latency_run_named( "server_io_uring" );
}

void rocketnet_test_receive_latency_server_xdp()
{
    receive_latency_run_named( "server_xdp" );
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

void rocketnet_test_receive_latency_client_iocp()
{
    receive_latency_run_named( "client_iocp" );
}

void rocketnet_test_receive_latency_server_rio();
void rocketnet_test_receive_latency_server_iocp();

void rocketnet_test_receive_latency_server_rio()
{
    receive_latency_run_named( "server_rio" );
}

void rocketnet_test_receive_latency_server_iocp()
{
    receive_latency_run_named( "server_iocp" );
}
