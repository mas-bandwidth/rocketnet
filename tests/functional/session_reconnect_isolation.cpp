/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// End-to-end session reconnect isolation across the real OS socket and I/O
// worker handoffs. Each supported client and server transport is covered once
// with a compatible counterpart (no Cartesian product):
//   - portable server + portable client (every OS)
//   - gso server + gso client (Linux)
//   - io_uring server + portable client (Linux, where io_uring is available)
//   - xdp server + portable client in an isolated veth namespace (Linux, where
//     the veth XDP environment is available)
//   - msgx server + msgx client (macOS)
//   - iocp server + iocp client (Windows)
//   - rio server + portable client (Windows)
//
// Each case connects one rocketnet_client_t to server_a (slot 0, tag 0xA1),
// exchanges warmup traffic, queues fresh unreliable, reliable ordered with ACK
// receipt, and multi-fragment reliable ordered messages in both directions,
// pushes them into the real I/O worker handoffs, disconnects, waits on bounded
// asynchronous completion (rocketnet_client_disconnected), reconnects the same
// client object to server_a (reusing slot 0, tag 0xB1) and then to a second
// server_b (slot 0, tag 0xC1), drains all receive and receipt surfaces, and
// proves that zero old-generation messages or receipts are delivered or
// counted as new-session activity while new-generation traffic completes.

#include "../native_shutdown.h"
#include "../token_string.h"
#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
struct rocketnet_server_transport_t * rocketnet_xdp_veth_start( bool ipv6, const char * case_name, char * server_address, size_t server_address_bytes );
void rocketnet_xdp_veth_note_server( struct rocketnet_server_transport_t * server );
void rocketnet_xdp_veth_pair_down( struct rocketnet_server_transport_t * destroyed );
const char * rocketnet_xdp_veth_netns_path();
const char * rocketnet_xdp_veth_host( bool ipv6, bool far );
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// defined in tests/functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

static void session_isolation_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define session_check( condition )                                                                           \
    do                                                                                                       \
    {                                                                                                        \
        if ( !( condition ) )                                                                                \
        {                                                                                                    \
            session_isolation_check_handler( #condition, (const char *) __FUNCTION__, __FILE__, __LINE__ );  \
        }                                                                                                    \
    } while ( 0 )

class session_env_guard_t
{
public:
    explicit session_env_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        char * current = NULL;
        size_t current_bytes = 0;
        session_check( _dupenv_s( &current, &current_bytes, name.c_str() ) == 0 );
        if ( current )
        {
            present = true;
            value = current;
            free( current );
        }
        session_check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        const char * current = getenv( name.c_str() );
        if ( current )
        {
            present = true;
            value = current;
        }
        session_check( unsetenv( name.c_str() ) == 0 );
#endif
    }

    ~session_env_guard_t()
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        if ( present )
        {
            (void) _putenv_s( name.c_str(), value.c_str() );
        }
        else
        {
            (void) _putenv_s( name.c_str(), "" );
        }
#else
        if ( present )
        {
            (void) setenv( name.c_str(), value.c_str(), 1 );
        }
        else
        {
            (void) unsetenv( name.c_str() );
        }
#endif
    }

private:
    std::string name;
    bool present;
    std::string value;
};

static uint32_t session_client_send_tagged( rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, uint8_t tag, uint8_t seq_byte, int bytes )
{
    uint8_t * out = rocketnet_client_start_message( client, channel, reliability, bytes );
    session_check( out != NULL );
    if ( !out )
    {
        return 0;
    }
    memset( out, (int) tag, (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = seq_byte;
    }
    const uint32_t serial = rocketnet_client_finish_message( client, bytes );
    session_check( serial != 0 );
    return serial;
}

static uint32_t session_server_send_tagged( rocketnet_server_t * server, uint64_t dest, int channel, enum rocketnet_reliability_t reliability, uint8_t tag, uint8_t seq_byte, int bytes )
{
    uint8_t * out = rocketnet_server_start_message( server, dest, channel, reliability, bytes );
    session_check( out != NULL );
    if ( !out )
    {
        return 0;
    }
    memset( out, (int) tag, (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = seq_byte;
    }
    const uint32_t serial = rocketnet_server_finish_message( server, bytes );
    session_check( serial != 0 );
    return serial;
}

static void session_assert_client_metrics_reset( const rocketnet_client_stats_t * stats )
{
    session_check( stats->rtt_ms == 0.0 );
    session_check( stats->jitter_ms == 0.0 );
    session_check( stats->packet_loss_percent == 0.0 );
    session_check( stats->packets_sent_per_second == 0.0 );
    session_check( stats->packets_received_per_second == 0.0 );
    session_check( stats->bandwidth_sent_kbps == 0.0 );
    session_check( stats->bandwidth_received_kbps == 0.0 );
}

static const uint64_t SESSION_ISO_PROTOCOL_ID = 0x5E551041501A7101ULL;
static const uint8_t SESSION_ISO_PACKET_FILTER[ROCKETNET_PACKET_FILTER_BYTES] = { 0x51, 0x52, 0x53, 0x54 };

static void session_mint_token( const uint8_t * backend_secret, rocketnet_server_t * server, const char * host_ip, uint64_t client_id, char * token )
{
    char ipv4_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( ipv4_address, sizeof( ipv4_address ), "%s:%u", host_ip, (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { ipv4_address };
    const uint64_t timestamp = rocketnet_functional_token_timestamp( 0, rocketnet_platform_time() );
    session_check( rn_test_server_token_string( backend_secret, server, SESSION_ISO_PROTOCOL_ID,
                                                addresses, 1, client_id, timestamp, timestamp + 30ULL,
                                                10, NULL, token ) == ROCKETNET_STATUS_OK );
}

static void session_wait_client_disconnected( rocketnet_client_t * client, rocketnet_server_t * server_a, rocketnet_server_t * server_b, double wall_start )
{
    const double start = rocketnet_time();
    while ( !rocketnet_client_disconnected( client ) && rocketnet_time() - start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        session_check( rocketnet_client_receive_messages( client ) == NULL );
        rocketnet_client_send_packets( client );
        if ( server_a && rocketnet_server_running( server_a ) )
        {
            rocketnet_server_update( server_a, now );
            while ( rocketnet_server_receive_messages( server_a ) != NULL )
            {
            }
            rocketnet_server_send_packets( server_a );
        }
        if ( server_b && rocketnet_server_running( server_b ) )
        {
            rocketnet_server_update( server_b, now );
            while ( rocketnet_server_receive_messages( server_b ) != NULL )
            {
            }
            rocketnet_server_send_packets( server_b );
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( rocketnet_client_disconnected( client ) );
}

static void session_run_isolation_pair( const char * server_transport, const char * client_transport, const char * bind_host, const char * dial_host, uint16_t fixed_port_a, uint16_t fixed_port_b, const char * far_netns_path )
{
    session_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    session_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    session_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );

    uint8_t seed[32];
    for ( int i = 0; i < 32; ++i )
    {
        seed[i] = (uint8_t) ( 0x4E ^ i );
    }
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    session_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config_a;
    rocketnet_server_default_config( &server_config_a );
    server_config_a.protocol_id = SESSION_ISO_PROTOCOL_ID;
    memcpy( server_config_a.backend_public_key, backend_public, sizeof( server_config_a.backend_public_key ) );
    memcpy( server_config_a.packet_filter, SESSION_ISO_PACKET_FILTER, sizeof( server_config_a.packet_filter ) );
    server_config_a.max_clients = 1;
    server_config_a.transport = server_transport;

    rocketnet_server_config_t server_config_b = server_config_a;

    char bind_a[64];
    char bind_b[64];
    snprintf( bind_a, sizeof( bind_a ), "%s:%u", bind_host, (unsigned) fixed_port_a );
    snprintf( bind_b, sizeof( bind_b ), "%s:%u", bind_host, (unsigned) fixed_port_b );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server_a = rocketnet_server_create( NULL, bind_a, NULL, &server_config_a, &status );
    session_check( server_a != NULL && status == ROCKETNET_STATUS_OK );
    session_check( rocketnet_server_start( server_a ) == ROCKETNET_STATUS_OK );
    session_check( rocketnet_server_transport_name( server_a ) != NULL );
    session_check( strcmp( rocketnet_server_transport_name( server_a ), server_transport ) == 0 );

    rocketnet_server_t * server_b = NULL;
    if ( far_netns_path == NULL )
    {
        server_b = rocketnet_server_create( NULL, bind_b, NULL, &server_config_b, &status );
        session_check( server_b != NULL && status == ROCKETNET_STATUS_OK );
        session_check( rocketnet_server_start( server_b ) == ROCKETNET_STATUS_OK );
        session_check( rocketnet_server_transport_name( server_b ) != NULL );
        session_check( strcmp( rocketnet_server_transport_name( server_b ), server_transport ) == 0 );
    }

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    memcpy( client_config.packet_filter, SESSION_ISO_PACKET_FILTER, sizeof( client_config.packet_filter ) );
    client_config.transport = client_transport;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    session_check( client != NULL && status == ROCKETNET_STATUS_OK );

    const double wall_start = rocketnet_time();

    // --- Generation 1 on server_a (tag 0xA1) ---
    char token_a1[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    session_mint_token( backend_secret, server_a, dial_host, 0x7101ULL, token_a1 );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( far_netns_path != NULL )
    {
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( far_netns_path, O_RDONLY );
        session_check( self_ns >= 0 && far_ns >= 0 );
        session_check( setns( far_ns, CLONE_NEWNET ) == 0 );
        status = rocketnet_client_connect( client, token_a1 );
        session_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
    }
    else
#else
    (void) far_netns_path;
#endif
    {
        status = rocketnet_client_connect( client, token_a1 );
    }
    session_check( status == ROCKETNET_STATUS_OK );
    session_check( rocketnet_client_transport_name( client ) != NULL );
    session_check( strcmp( rocketnet_client_transport_name( client ), client_transport ) == 0 );

    double connect_start = rocketnet_time();
    while ( rocketnet_time() - connect_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server_a, 0 ) )
        {
            break;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    session_check( rocketnet_client_index( client ) == 0 );
    session_check( rocketnet_server_client_connected( server_a, 0 ) == 1 );

    // Warmup exchange on Generation 1 so both sides complete a full round-trip.
    const uint32_t warm_c = session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA0, 0, 16 );
    const uint32_t warm_s = session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA0, 0, 16 );
    int warm_c_got = 0, warm_s_got = 0, warm_c_rcpt = 0, warm_s_rcpt = 0;
    const double warm_start = rocketnet_time();
    while ( ( warm_c_got < 1 || warm_s_got < 1 || warm_c_rcpt < 1 || warm_s_rcpt < 1 ) && rocketnet_time() - warm_start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->data[i][0] == 0xA0 );
                warm_c_got++;
            }
        }
        while ( ( batch = rocketnet_server_receive_messages( server_a ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->data[i][0] == 0xA0 );
                warm_s_got++;
            }
        }
        struct rocketnet_receipt_t rcpts[8];
        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int i = 0; i < cr; ++i )
        {
            session_check( rcpts[i].serial == warm_c && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            warm_c_rcpt++;
        }
        const int sr = rocketnet_server_receipts( server_a, 0, rcpts, 8 );
        for ( int i = 0; i < sr; ++i )
        {
            session_check( rcpts[i].serial == warm_s && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            warm_s_rcpt++;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( warm_c_got == 1 && warm_s_got == 1 && warm_c_rcpt == 1 && warm_s_rcpt == 1 );

    // Queue in-flight and queued Generation-1 traffic across disconnect:
    // unreliable, reliable ordered with ACK receipt, and multi-fragment reliable ordered.
    for ( int i = 0; i < 4; ++i )
    {
        (void) session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 0xA1, (uint8_t) i, 32 );
        (void) session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, (uint8_t) ( 10 + i ), 48 );
        (void) session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 0xA1, (uint8_t) i, 32 );
        (void) session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, (uint8_t) ( 10 + i ), 48 );
    }
    (void) session_client_send_tagged( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xA1, 99, 2600 );
    (void) session_server_send_tagged( server_a, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xA1, 99, 2600 );

    // Flush one batch to the I/O workers, then queue more messages before disconnecting.
    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server_a );
    (void) session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 50, 48 );
    (void) session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 50, 48 );

    rocketnet_client_disconnect( client );
    session_wait_client_disconnected( client, server_a, NULL, wall_start );

    struct rocketnet_client_stats_t stats_after_disc1;
    memset( &stats_after_disc1, 0, sizeof( stats_after_disc1 ) );
    rocketnet_client_stats( client, &stats_after_disc1 );
    session_assert_client_metrics_reset( &stats_after_disc1 );
    session_check( rocketnet_client_receive_messages( client ) == NULL );

    // the ended connection's receipts stay readable until the next update
    // (rocketnet.h); after it none remain to cross into the next connection
    rocketnet_client_update( client, 1000.0 + ( rocketnet_time() - wall_start ) );
    session_check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );

    // Wait for server_a to retire slot 0 so the reconnect reuses slot 0 cleanly.
    const double empty_start = rocketnet_time();
    while ( rocketnet_server_client_connected( server_a, 0 ) && rocketnet_time() - empty_start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        session_check( rocketnet_client_receive_messages( client ) == NULL );
        rocketnet_server_update( server_a, now );
        while ( rocketnet_server_receive_messages( server_a ) != NULL )
        {
        }
        rocketnet_server_send_packets( server_a );
        rocketnet_sleep( 0.002 );
    }
    session_check( rocketnet_server_client_connected( server_a, 0 ) == 0 );
    while ( rocketnet_server_receive_messages( server_a ) != NULL )
    {
    }
    session_check( rocketnet_server_receipts( server_a, 0, NULL, 0 ) == 0 );

    struct rocketnet_server_client_stats_t empty_slot_stats;
    memset( &empty_slot_stats, 0xFF, sizeof( empty_slot_stats ) );
    rocketnet_server_client_stats( server_a, 0, &empty_slot_stats );
    session_check( empty_slot_stats.packets_sent == 0 && empty_slot_stats.packets_received == 0 );
    session_check( empty_slot_stats.bytes_sent == 0 && empty_slot_stats.bytes_received == 0 );
    session_check( empty_slot_stats.rtt_ms == 0.0 && empty_slot_stats.jitter_ms == 0.0 && empty_slot_stats.packet_loss_percent == 0.0 );

    // --- Generation 2: reconnect the same client object to server_a (tag 0xB1) ---
    char token_a2[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    session_mint_token( backend_secret, server_a, dial_host, 0x7102ULL, token_a2 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( far_netns_path != NULL )
    {
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( far_netns_path, O_RDONLY );
        session_check( self_ns >= 0 && far_ns >= 0 );
        session_check( setns( far_ns, CLONE_NEWNET ) == 0 );
        status = rocketnet_client_connect( client, token_a2 );
        session_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
    }
    else
#endif
    {
        status = rocketnet_client_connect( client, token_a2 );
    }
    session_check( status == ROCKETNET_STATUS_OK );

    struct rocketnet_client_stats_t stats_connect2;
    memset( &stats_connect2, 0, sizeof( stats_connect2 ) );
    rocketnet_client_stats( client, &stats_connect2 );
    session_assert_client_metrics_reset( &stats_connect2 );
    const uint64_t client_payloads_recv_base2 = stats_connect2.payloads_received;
    const uint64_t client_receipts_acked_base2 = stats_connect2.receipts_acked;

    connect_start = rocketnet_time();
    while ( rocketnet_time() - connect_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server_a, 0 ) )
        {
            break;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    session_check( rocketnet_client_index( client ) == 0 );
    session_check( rocketnet_server_client_connected( server_a, 0 ) == 1 );
    session_check( rocketnet_server_client_id( server_a, 0 ) == 0x7102ULL );

    // Before sending any Generation-2 messages, verify no stale messages or receipts arrived.
    session_check( rocketnet_client_receive_messages( client ) == NULL );
    session_check( rocketnet_server_receive_messages( server_a ) == NULL );
    session_check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );
    session_check( rocketnet_server_receipts( server_a, 0, NULL, 0 ) == 0 );

    struct rocketnet_server_client_stats_t slot_a_fresh;
    memset( &slot_a_fresh, 0, sizeof( slot_a_fresh ) );
    rocketnet_server_client_stats( server_a, 0, &slot_a_fresh );
    struct rocketnet_server_stats_t server_a_base2;
    memset( &server_a_base2, 0, sizeof( server_a_base2 ) );
    rocketnet_server_stats( server_a, &server_a_base2 );

    const uint32_t gen2_c_serial = session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 1, 32 );
    (void) session_client_send_tagged( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xB1, 2, 2600 );
    const uint32_t gen2_s_serial = session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 1, 32 );
    (void) session_server_send_tagged( server_a, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xB1, 2, 2600 );

    int gen2_c_msgs = 0, gen2_s_msgs = 0, gen2_c_rcpts = 0, gen2_s_rcpts = 0;
    const double gen2_start = rocketnet_time();
    while ( ( gen2_c_msgs < 2 || gen2_s_msgs < 2 || gen2_c_rcpts < 1 || gen2_s_rcpts < 1 ) && rocketnet_time() - gen2_start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->bytes[i] >= 2 );
                session_check( batch->data[i][0] == 0xB1 );
                gen2_c_msgs++;
            }
        }
        while ( ( batch = rocketnet_server_receive_messages( server_a ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->client_index[i] == 0 );
                session_check( batch->bytes[i] >= 2 );
                session_check( batch->data[i][0] == 0xB1 );
                gen2_s_msgs++;
            }
        }
        struct rocketnet_receipt_t rcpts[8];
        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int i = 0; i < cr; ++i )
        {
            session_check( rcpts[i].serial == gen2_c_serial && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen2_c_rcpts++;
        }
        const int sr = rocketnet_server_receipts( server_a, 0, rcpts, 8 );
        for ( int i = 0; i < sr; ++i )
        {
            session_check( rcpts[i].serial == gen2_s_serial && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen2_s_rcpts++;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( gen2_c_msgs == 2 && gen2_s_msgs == 2 && gen2_c_rcpts == 1 && gen2_s_rcpts == 1 );

    struct rocketnet_server_client_stats_t slot_a_gen2;
    memset( &slot_a_gen2, 0, sizeof( slot_a_gen2 ) );
    rocketnet_server_client_stats( server_a, 0, &slot_a_gen2 );
    session_check( slot_a_gen2.packets_sent > slot_a_fresh.packets_sent && slot_a_gen2.packets_received > slot_a_fresh.packets_received );
    struct rocketnet_server_stats_t server_a_gen2;
    memset( &server_a_gen2, 0, sizeof( server_a_gen2 ) );
    rocketnet_server_stats( server_a, &server_a_gen2 );
    session_check( server_a_gen2.receipts_acked - server_a_base2.receipts_acked == 1 );

    struct rocketnet_client_stats_t client_gen2;
    memset( &client_gen2, 0, sizeof( client_gen2 ) );
    rocketnet_client_stats( client, &client_gen2 );
    session_check( client_gen2.payloads_received > client_payloads_recv_base2 );
    session_check( client_gen2.receipts_acked - client_receipts_acked_base2 == 1 );

    // Queue late Generation-2 messages and disconnect before reconnecting to a second server.
    (void) session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 77, 32 );
    (void) session_server_send_tagged( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 77, 32 );
    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server_a );

    rocketnet_client_disconnect( client );
    session_wait_client_disconnected( client, server_a, server_b, wall_start );

    // For single-veth XDP, stop and destroy server_a on the veth interface and bring up server_b on a different port.
    if ( server_b == NULL )
    {
        rn_cli_server_stop_and_destroy( server_a );
        server_a = NULL;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
        if ( far_netns_path != NULL )
        {
#if ROCKETNET_DEV
            rocketnet_server_socket_allow_commit();
#endif // #if ROCKETNET_DEV
            server_config_b.num_xdp_threads = 1;
            server_config_b.num_os_cpus = 1;
        }
#endif
        server_b = rocketnet_server_create( NULL, bind_b, NULL, &server_config_b, &status );
        session_check( server_b != NULL && status == ROCKETNET_STATUS_OK );
        session_check( rocketnet_server_start( server_b ) == ROCKETNET_STATUS_OK );
        session_check( strcmp( rocketnet_server_transport_name( server_b ), server_transport ) == 0 );
    }

    // --- Generation 3: reconnect the same client object to server_b (tag 0xC1) ---
    char token_b1[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    session_mint_token( backend_secret, server_b, dial_host, 0x8101ULL, token_b1 );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( far_netns_path != NULL )
    {
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( far_netns_path, O_RDONLY );
        session_check( self_ns >= 0 && far_ns >= 0 );
        session_check( setns( far_ns, CLONE_NEWNET ) == 0 );
        status = rocketnet_client_connect( client, token_b1 );
        session_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
    }
    else
#endif
    {
        status = rocketnet_client_connect( client, token_b1 );
    }
    session_check( status == ROCKETNET_STATUS_OK );

    struct rocketnet_client_stats_t stats_connect3;
    memset( &stats_connect3, 0, sizeof( stats_connect3 ) );
    rocketnet_client_stats( client, &stats_connect3 );
    session_assert_client_metrics_reset( &stats_connect3 );
    const uint64_t client_payloads_recv_base3 = stats_connect3.payloads_received;
    const uint64_t client_receipts_acked_base3 = stats_connect3.receipts_acked;

    connect_start = rocketnet_time();
    while ( rocketnet_time() - connect_start < 10.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        if ( server_a )
        {
            rocketnet_server_update( server_a, now );
            while ( rocketnet_server_receive_messages( server_a ) != NULL )
            {
            }
            rocketnet_server_send_packets( server_a );
        }
        rocketnet_server_update( server_b, now );
        rocketnet_server_send_packets( server_b );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_client_connected( server_b, 0 ) )
        {
            break;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    session_check( rocketnet_server_client_connected( server_b, 0 ) == 1 );
    session_check( rocketnet_server_client_id( server_b, 0 ) == 0x8101ULL );

    struct rocketnet_server_client_stats_t slot_b_fresh;
    memset( &slot_b_fresh, 0, sizeof( slot_b_fresh ) );
    rocketnet_server_client_stats( server_b, 0, &slot_b_fresh );
    struct rocketnet_server_stats_t server_b_base3;
    memset( &server_b_base3, 0, sizeof( server_b_base3 ) );
    rocketnet_server_stats( server_b, &server_b_base3 );

    const uint32_t gen3_c_serial = session_client_send_tagged( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xC1, 1, 32 );
    const uint32_t gen3_s_serial = session_server_send_tagged( server_b, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xC1, 1, 32 );

    int gen3_c_msgs = 0, gen3_s_msgs = 0, gen3_c_rcpts = 0, gen3_s_rcpts = 0;
    const double gen3_start = rocketnet_time();
    while ( ( gen3_c_msgs < 1 || gen3_s_msgs < 1 || gen3_c_rcpts < 1 || gen3_s_rcpts < 1 ) && rocketnet_time() - gen3_start < 5.0 )
    {
        const double now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        if ( server_a )
        {
            rocketnet_server_update( server_a, now );
            while ( rocketnet_server_receive_messages( server_a ) != NULL )
            {
            }
            rocketnet_server_send_packets( server_a );
        }
        rocketnet_server_update( server_b, now );
        rocketnet_server_send_packets( server_b );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->bytes[i] >= 2 );
                session_check( batch->data[i][0] == 0xC1 );
                gen3_c_msgs++;
            }
        }
        while ( ( batch = rocketnet_server_receive_messages( server_b ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                session_check( batch->client_index[i] == 0 );
                session_check( batch->bytes[i] >= 2 );
                session_check( batch->data[i][0] == 0xC1 );
                gen3_s_msgs++;
            }
        }
        struct rocketnet_receipt_t rcpts[8];
        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int i = 0; i < cr; ++i )
        {
            session_check( rcpts[i].serial == gen3_c_serial && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen3_c_rcpts++;
        }
        const int sr = rocketnet_server_receipts( server_b, 0, rcpts, 8 );
        for ( int i = 0; i < sr; ++i )
        {
            session_check( rcpts[i].serial == gen3_s_serial && rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen3_s_rcpts++;
        }
        rocketnet_sleep( 0.002 );
    }
    session_check( gen3_c_msgs == 1 && gen3_s_msgs == 1 && gen3_c_rcpts == 1 && gen3_s_rcpts == 1 );

    struct rocketnet_server_client_stats_t slot_b_gen3;
    memset( &slot_b_gen3, 0, sizeof( slot_b_gen3 ) );
    rocketnet_server_client_stats( server_b, 0, &slot_b_gen3 );
    session_check( slot_b_gen3.packets_sent > slot_b_fresh.packets_sent && slot_b_gen3.packets_received > slot_b_fresh.packets_received );
    struct rocketnet_server_stats_t server_b_gen3;
    memset( &server_b_gen3, 0, sizeof( server_b_gen3 ) );
    rocketnet_server_stats( server_b, &server_b_gen3 );
    session_check( server_b_gen3.receipts_acked - server_b_base3.receipts_acked == 1 );

    struct rocketnet_client_stats_t client_gen3;
    memset( &client_gen3, 0, sizeof( client_gen3 ) );
    rocketnet_client_stats( client, &client_gen3 );
    session_check( client_gen3.payloads_received > client_payloads_recv_base3 );
    session_check( client_gen3.receipts_acked - client_receipts_acked_base3 == 1 );

    rn_cli_client_stop_and_destroy( client, 1000.0 + ( rocketnet_time() - wall_start ) );
    if ( server_a )
    {
        rn_cli_server_stop_and_destroy( server_a );
    }
    if ( server_b )
    {
        rn_cli_server_stop_and_destroy( server_b );
    }
}

// A small server is sized from its own max_clients (sdk/rocketnet_constants.h,
// "Per-server sizing"): a server of 1, 8 or 15 clients holds that many slots'
// buffers and no 512-client floor. Each run creates a server of exactly that
// many clients, connects that many clients, exchanges messages in all eight
// reliability modes both ways (each mode on its own channel, one reliable
// ordered message of three fragments each way), disconnects every client,
// waits for every slot to empty, and does it all again on the same server.
// The reliable modes must arrive complete (the ordered ones in order), the
// reliable sequenced one must reach its last message, and each unreliable
// mode, resent until one arrives, must arrive at least once.

#define SMALL_SERVER_MAX_CLIENTS    15
#define SMALL_SERVER_MODES          8
#define SMALL_SERVER_SEQUENCES      4           // per client, mode and direction; the last is the fragmented one in mode 3
#define SMALL_SERVER_BYTES          40
#define SMALL_SERVER_FRAGMENTED     2600

struct small_server_track_t
{
    uint32_t got[2][SMALL_SERVER_MAX_CLIENTS][SMALL_SERVER_MODES];     // bit s: sequence s arrived (direction 0: client to server)
    int last[2][SMALL_SERVER_MAX_CLIENTS][SMALL_SERVER_MODES];          // the last sequence an ordered or sequenced mode delivered
};

static bool small_server_mode_reliable( int mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE || mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT || mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static bool small_server_mode_ordered( int mode )
{
    // the unreliable sequenced mode is resent below, so a resent copy may
    // follow its first one: its order is not checked here
    return mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED || mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED;
}

static int small_server_message_bytes( int mode, int sequence )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED && sequence == SMALL_SERVER_SEQUENCES - 1 ? SMALL_SERVER_FRAGMENTED : SMALL_SERVER_BYTES;
}

static void small_server_fill( uint8_t * out, int bytes, uint8_t tag, int client, int mode, int sequence )
{
    for ( int i = 0; i < bytes; ++i )
    {
        out[i] = (uint8_t) ( tag ^ ( i * 7 ) ^ client ^ ( mode << 4 ) ^ sequence );
    }
    out[0] = tag;
    out[1] = (uint8_t) client;
    out[2] = (uint8_t) mode;
    out[3] = (uint8_t) sequence;
}

static void small_server_send( rocketnet_server_t * server, rocketnet_client_t * client, int slot, int c, int mode, int sequence, uint8_t tag )
{
    const int bytes = small_server_message_bytes( mode, sequence );
    const int channel = mode;       // each mode on its own channel (channels are per connection; the default config has 32): a sequenced resend never shares a channel with the ordered modes
    uint8_t * out = server ? rocketnet_server_start_message( server, (uint64_t) slot, channel, (enum rocketnet_reliability_t) mode, bytes )
                           : rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, bytes );
    session_check( out != NULL );
    small_server_fill( out, bytes, tag, c, mode, sequence );
    const uint32_t finished = server ? rocketnet_server_finish_message( server, bytes ) : rocketnet_client_finish_message( client, bytes );
    session_check( finished != 0 );
}

static void small_server_arrival( struct small_server_track_t * track, int direction, int expected_client, const uint8_t * data, int bytes, uint8_t tag )
{
    session_check( bytes >= 4 && data[0] == tag );
    const int c = data[1];
    const int mode = data[2];
    const int sequence = data[3];
    session_check( c < SMALL_SERVER_MAX_CLIENTS && mode < SMALL_SERVER_MODES && sequence < SMALL_SERVER_SEQUENCES );
    session_check( expected_client < 0 || c == expected_client );
    session_check( bytes == small_server_message_bytes( mode, sequence ) );
    uint8_t want[SMALL_SERVER_FRAGMENTED];
    small_server_fill( want, bytes, tag, c, mode, sequence );
    session_check( memcmp( want, data, (size_t) bytes ) == 0 );
    if ( small_server_mode_reliable( mode ) )
    {
        session_check( ( track->got[direction][c][mode] & ( 1u << sequence ) ) == 0 );     // never twice
    }
    if ( small_server_mode_ordered( mode ) )
    {
        session_check( sequence > track->last[direction][c][mode] );
        track->last[direction][c][mode] = sequence;
    }
    track->got[direction][c][mode] |= 1u << sequence;
}

static bool small_server_done( const struct small_server_track_t * track, int clients )
{
    const uint32_t all = ( 1u << SMALL_SERVER_SEQUENCES ) - 1;
    for ( int direction = 0; direction < 2; ++direction )
    {
        for ( int c = 0; c < clients; ++c )
        {
            for ( int mode = 0; mode < SMALL_SERVER_MODES; ++mode )
            {
                const uint32_t got = track->got[direction][c][mode];
                if ( small_server_mode_reliable( mode ) ? got != all
                                                        : ( mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED ? track->last[direction][c][mode] != SMALL_SERVER_SEQUENCES - 1 : got == 0 ) )
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static void small_server_pump( rocketnet_server_t * server, rocketnet_client_t ** clients, int count, double wall_start, struct small_server_track_t * track, uint8_t tag )
{
    const double now = 1000.0 + ( rocketnet_time() - wall_start );
    for ( int c = 0; c < count; ++c )
    {
        rocketnet_client_update( clients[c], now );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( clients[c] ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                // outside the exchange only a late copy of this session's
                // unreliable resends may arrive; never another session's
                session_check( batch->bytes[i] >= 1 && batch->data[i][0] == tag );
                if ( track )
                {
                    small_server_arrival( track, 1, c, batch->data[i], batch->bytes[i], tag );
                }
            }
        }
        // receipts stay readable until the next update (rocketnet.h): one read a pass
        struct rocketnet_receipt_t receipts[64];
        (void) rocketnet_client_receipts( clients[c], receipts, 64 );
        rocketnet_client_send_packets( clients[c] );
    }
    rocketnet_server_update( server, now );
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; ++i )
        {
            session_check( batch->bytes[i] >= 1 && batch->data[i][0] == tag );
            if ( track )
            {
                small_server_arrival( track, 0, -1, batch->data[i], batch->bytes[i], tag );
                session_check( batch->data[i][1] < SMALL_SERVER_MAX_CLIENTS );
            }
        }
    }
    for ( int slot = 0; slot < rocketnet_server_max_clients( server ); ++slot )
    {
        struct rocketnet_receipt_t receipts[64];
        (void) rocketnet_server_receipts( server, slot, receipts, 64 );
    }
    rocketnet_server_send_packets( server );
    rocketnet_sleep( 0.001 );
}

static void small_server_connect( rocketnet_client_t * client, const char * token, const char * far_netns_path )
{
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_XDP
    if ( far_netns_path != NULL )
    {
        const int self_ns = open( "/proc/self/ns/net", O_RDONLY );
        const int far_ns = open( far_netns_path, O_RDONLY );
        session_check( self_ns >= 0 && far_ns >= 0 );
        session_check( setns( far_ns, CLONE_NEWNET ) == 0 );
        status = rocketnet_client_connect( client, token );
        session_check( setns( self_ns, CLONE_NEWNET ) == 0 );
        close( self_ns );
        close( far_ns );
    }
    else
#else
    (void) far_netns_path;
#endif
    {
        status = rocketnet_client_connect( client, token );
    }
    session_check( status == ROCKETNET_STATUS_OK );
}

static void small_server_generation( rocketnet_server_t * server, rocketnet_client_t ** clients, int count, const uint8_t * backend_secret,
                                     const char * dial_host, const char * far_netns_path, double wall_start, uint8_t tag )
{
    for ( int c = 0; c < count; ++c )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        session_mint_token( backend_secret, server, dial_host, ( (uint64_t) tag << 8 ) | (uint64_t) ( c + 1 ), token );
        small_server_connect( clients[c], token, far_netns_path );
    }

    const double connect_start = rocketnet_time();
    for ( ;; )
    {
        small_server_pump( server, clients, count, wall_start, NULL, tag );
        int connected = 0;
        for ( int c = 0; c < count; ++c )
        {
            connected += rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED ? 1 : 0;
        }
        if ( connected == count && rocketnet_server_num_connected_clients( server ) == count )
        {
            break;
        }
        session_check( rocketnet_time() - connect_start < 10.0 );
    }

    int slot[SMALL_SERVER_MAX_CLIENTS];
    for ( int c = 0; c < count; ++c )
    {
        slot[c] = rocketnet_client_index( clients[c] );
        session_check( slot[c] >= 0 && slot[c] < count && rocketnet_server_client_connected( server, slot[c] ) == 1 );
    }

    // every mode both ways; the unreliable ones again each tenth pass until one arrives
    struct small_server_track_t track;
    memset( &track, 0, sizeof( track ) );
    for ( int direction = 0; direction < 2; ++direction )
    {
        for ( int c = 0; c < count; ++c )
        {
            for ( int mode = 0; mode < SMALL_SERVER_MODES; ++mode )
            {
                track.last[direction][c][mode] = -1;
            }
        }
    }
    for ( int c = 0; c < count; ++c )
    {
        for ( int mode = 0; mode < SMALL_SERVER_MODES; ++mode )
        {
            for ( int sequence = 0; sequence < SMALL_SERVER_SEQUENCES; ++sequence )
            {
                small_server_send( NULL, clients[c], 0, c, mode, sequence, tag );
                small_server_send( server, NULL, slot[c], c, mode, sequence, tag );
            }
        }
    }
    const double exchange_start = rocketnet_time();
    for ( int pass = 1; !small_server_done( &track, count ); ++pass )
    {
        session_check( rocketnet_time() - exchange_start < 20.0 );
        small_server_pump( server, clients, count, wall_start, &track, tag );
        if ( pass % 10 == 0 )
        {
            for ( int c = 0; c < count; ++c )
            {
                for ( int mode = 0; mode < SMALL_SERVER_MODES; ++mode )
                {
                    if ( small_server_mode_reliable( mode ) || mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED )
                    {
                        continue;
                    }
                    if ( track.got[0][c][mode] == 0 )
                    {
                        small_server_send( NULL, clients[c], 0, c, mode, SMALL_SERVER_SEQUENCES - 1, tag );
                    }
                    if ( track.got[1][c][mode] == 0 )
                    {
                        small_server_send( server, NULL, slot[c], c, mode, SMALL_SERVER_SEQUENCES - 1, tag );
                    }
                }
            }
        }
    }

    // every client disconnects; every slot empties
    for ( int c = 0; c < count; ++c )
    {
        rocketnet_client_disconnect( clients[c] );
    }
    const double disconnect_start = rocketnet_time();
    for ( ;; )
    {
        small_server_pump( server, clients, count, wall_start, NULL, tag );
        int disconnected = 0;
        for ( int c = 0; c < count; ++c )
        {
            disconnected += rocketnet_client_disconnected( clients[c] ) ? 1 : 0;
        }
        if ( disconnected == count && rocketnet_server_num_connected_clients( server ) == 0 )
        {
            break;
        }
        session_check( rocketnet_time() - disconnect_start < 10.0 );
    }
}

static void small_server_run( const char * server_transport, const char * client_transport, int count, const char * bind_host, const char * dial_host,
                              uint16_t fixed_port, const char * far_netns_path )
{
    session_check( count >= 1 && count <= SMALL_SERVER_MAX_CLIENTS );
    session_env_guard_t env_all( "ROCKETNET_TRANSPORT" );
    session_env_guard_t env_server( "ROCKETNET_SERVER_TRANSPORT" );
    session_env_guard_t env_client( "ROCKETNET_CLIENT_TRANSPORT" );
    session_env_guard_t env_max_clients( "ROCKETNET_SERVER_MAX_CLIENTS" );

    uint8_t seed[32];
    for ( int i = 0; i < 32; ++i )
    {
        seed[i] = (uint8_t) ( 0x5C ^ i );
    }
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    session_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = SESSION_ISO_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.packet_filter, SESSION_ISO_PACKET_FILTER, sizeof( server_config.packet_filter ) );
    server_config.max_clients = count;
    server_config.transport = server_transport;

    char bind[64];
    snprintf( bind, sizeof( bind ), "%s:%u", bind_host, (unsigned) fixed_port );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, bind, NULL, &server_config, &status );
    session_check( server != NULL && status == ROCKETNET_STATUS_OK );
    session_check( rocketnet_server_max_clients( server ) == count );
    session_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    session_check( rocketnet_server_transport_name( server ) != NULL && strcmp( rocketnet_server_transport_name( server ), server_transport ) == 0 );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    memcpy( client_config.packet_filter, SESSION_ISO_PACKET_FILTER, sizeof( client_config.packet_filter ) );
    client_config.transport = client_transport;
    rocketnet_client_t * clients[SMALL_SERVER_MAX_CLIENTS];
    for ( int c = 0; c < count; ++c )
    {
        clients[c] = rocketnet_client_create( NULL, &client_config, &status );
        session_check( clients[c] != NULL && status == ROCKETNET_STATUS_OK );
    }

    const double wall_start = rocketnet_time();
    small_server_generation( server, clients, count, backend_secret, dial_host, far_netns_path, wall_start, 0xD1 );
    small_server_generation( server, clients, count, backend_secret, dial_host, far_netns_path, wall_start, 0xD2 );
    printf( "small server %s: %d clients, two sessions of all eight modes, %.3f s\n", server_transport, count, rocketnet_time() - wall_start );

    for ( int c = 0; c < count; ++c )
    {
        rn_cli_client_stop_and_destroy( clients[c], 1000.0 + ( rocketnet_time() - wall_start ) );
    }
    rn_cli_server_stop_and_destroy( server );
}

static void small_server_counts( const char * server_transport, const char * client_transport, const char * bind_host, const char * dial_host,
                                 uint16_t fixed_port, const char * far_netns_path )
{
    small_server_run( server_transport, client_transport, 1, bind_host, dial_host, fixed_port, far_netns_path );
    small_server_run( server_transport, client_transport, 8, bind_host, dial_host, fixed_port, far_netns_path );
    small_server_run( server_transport, client_transport, 15, bind_host, dial_host, fixed_port, far_netns_path );
}

void rocketnet_test_small_server_sessions_portable()
{
    small_server_counts( "portable", "portable", "127.0.0.1", "127.0.0.1", 0, NULL );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
void rocketnet_test_small_server_sessions_gso()
{
    small_server_counts( "gso", "gso", "127.0.0.1", "127.0.0.1", 0, NULL );
}

void rocketnet_test_small_server_sessions_io_uring()
{
    small_server_counts( "io_uring", "portable", "127.0.0.1", "127.0.0.1", 0, NULL );
}

#if ROCKETNET_XDP
// 1, 8 and 15 clients: up to 15 the reply MAC table is held at its 64-entry
// minimum (four probe windows, the smallest table the transport binds), so
// each of these creates proves the minimum is enough to start and serve
void rocketnet_test_small_server_sessions_xdp()
{
    char veth_address[128];
    struct rocketnet_server_transport_t * initial_server = rocketnet_xdp_veth_start( false, "test_small_server_sessions_xdp", veth_address, sizeof( veth_address ) );
    session_check( initial_server != NULL );
    rocketnet_xdp_veth_note_server( NULL );
    rocketnet_server_transport_stop( initial_server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( initial_server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; ++i )
    {
        rocketnet_server_transport_update( initial_server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rn_native_server_transport_stop_and_destroy( initial_server );

#if ROCKETNET_DEV
    rocketnet_server_socket_allow_commit();
#endif // #if ROCKETNET_DEV
    session_env_guard_t env_xdp_threads( "ROCKETNET_SERVER_NUM_XDP_THREADS" );
    session_env_guard_t env_os_cpus( "ROCKETNET_SERVER_NUM_OS_CPUS" );
    session_check( setenv( "ROCKETNET_SERVER_NUM_XDP_THREADS", "1", 1 ) == 0 );
    session_check( setenv( "ROCKETNET_SERVER_NUM_OS_CPUS", "1", 1 ) == 0 );

    const char * near_host = rocketnet_xdp_veth_host( false, false );
    small_server_counts( "xdp", "portable", near_host, near_host, 40421, rocketnet_xdp_veth_netns_path() );
    rocketnet_xdp_veth_pair_down( NULL );
}
#endif // #if ROCKETNET_XDP
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
void rocketnet_test_small_server_sessions_msgx()
{
    small_server_counts( "msgx", "msgx", "127.0.0.1", "127.0.0.1", 0, NULL );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void rocketnet_test_small_server_sessions_iocp()
{
    small_server_counts( "iocp", "iocp", "127.0.0.1", "127.0.0.1", 0, NULL );
}

void rocketnet_test_small_server_sessions_rio()
{
    small_server_counts( "rio", "portable", "127.0.0.1", "127.0.0.1", 0, NULL );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

void rocketnet_test_session_reconnect_isolation_portable()
{
    session_run_isolation_pair( "portable", "portable", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
void rocketnet_test_session_reconnect_isolation_gso()
{
    session_run_isolation_pair( "gso", "gso", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}

void rocketnet_test_session_reconnect_isolation_io_uring()
{
    session_run_isolation_pair( "io_uring", "portable", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}

#if ROCKETNET_XDP
void rocketnet_test_session_reconnect_isolation_xdp()
{
    char veth_address[128];
    struct rocketnet_server_transport_t * initial_server = rocketnet_xdp_veth_start( false, "test_session_reconnect_isolation_xdp", veth_address, sizeof( veth_address ) );
    session_check( initial_server != NULL );
    rocketnet_xdp_veth_note_server( NULL );
    rocketnet_server_transport_stop( initial_server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( initial_server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; ++i )
    {
        rocketnet_server_transport_update( initial_server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rn_native_server_transport_stop_and_destroy( initial_server );

#if ROCKETNET_DEV
    rocketnet_server_socket_allow_commit();
#endif // #if ROCKETNET_DEV
    session_env_guard_t env_xdp_threads( "ROCKETNET_SERVER_NUM_XDP_THREADS" );
    session_env_guard_t env_os_cpus( "ROCKETNET_SERVER_NUM_OS_CPUS" );
    session_check( setenv( "ROCKETNET_SERVER_NUM_XDP_THREADS", "1", 1 ) == 0 );
    session_check( setenv( "ROCKETNET_SERVER_NUM_OS_CPUS", "1", 1 ) == 0 );

    const char * near_host = rocketnet_xdp_veth_host( false, false );
    session_run_isolation_pair( "xdp", "portable", near_host, near_host, 40411, 40412, rocketnet_xdp_veth_netns_path() );
    rocketnet_xdp_veth_pair_down( NULL );
}
#endif // ROCKETNET_XDP
#endif // ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
void rocketnet_test_session_reconnect_isolation_msgx()
{
    session_run_isolation_pair( "msgx", "msgx", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void rocketnet_test_session_reconnect_isolation_iocp()
{
    session_run_isolation_pair( "iocp", "iocp", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}

void rocketnet_test_session_reconnect_isolation_rio()
{
    session_run_isolation_pair( "rio", "portable", "127.0.0.1", "127.0.0.1", 0, 0, NULL );
}
#endif
