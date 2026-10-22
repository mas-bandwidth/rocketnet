/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The installed-package consumer: rocketnet.h and the installed library
// through find_package( RocketNet ), built as C and (through main.cpp) as C++
// against both the static and the shared target, -Wall -Wextra -Werror. It
// calls every function the header declares (the development connect token
// only where the library is a development build): the lifecycle and logging
// setters, the name and status tables, the server's public key and addresses, the development connect token, the config
// refusals (a zeroed config is CONFIG_NOT_INITIALIZED), a server and a client
// on 127.0.0.1 with one receipted message each way, every stats struct, the
// server ending the connection, a reconnect, and a clean teardown.

#include <rocketnet.h>

#if defined(EXPECT_SHARED) && !defined(ROCKETNET_SHARED)
#error Shared consumer did not inherit the DLL import contract
#endif
#if !defined(EXPECT_SHARED) && defined(ROCKETNET_SHARED)
#error Static consumer inherited the DLL import contract
#endif
/* This consumer builds in the package's configuration, so its NDEBUG is the
   library's: the library asserts on a NULL argument exactly when NDEBUG is off. */
#ifdef NDEBUG
#define EXPECTED_NULL_ASSERTIONS 0
#else
#define EXPECTED_NULL_ASSERTIONS 2
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK( condition )                                                                  \
    do                                                                                      \
    {                                                                                       \
        if ( !( condition ) )                                                               \
        {                                                                                   \
            fprintf( stderr, "consumer failed: %s (line %d)\n", #condition, __LINE__ ); \
            return 1;                                                                       \
        }                                                                                   \
    } while ( 0 )

#define PROTOCOL_ID 0x7061636b61676501ull
#define CLIENT_ID 1234


// ---------------------------------------------------------------------------
// the process-wide hooks: allocator, assert function, log function, and the
// client's state change callback

static void * consumer_malloc( void * context, size_t bytes )
{
    (void) context;
    return malloc( bytes );
}

static void consumer_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static int assertions;

static void consumer_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    ++assertions;
}

struct log_capture_t
{
    int lines;
    enum rocketnet_log_level_t level;
};

static void consumer_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    struct log_capture_t * capture = (struct log_capture_t *) context;
    (void) message;
    (void) length;
    capture->lines++;
    capture->level = level;
}

static int saw_connected;

static void consumer_state_change( void * context, enum rocketnet_client_state_t previous_state, enum rocketnet_client_state_t current_state )
{
    (void) context;
    (void) previous_state;
    if ( current_state == ROCKETNET_CLIENT_STATE_CONNECTED )
        saw_connected = 1;
}

#if ROCKETNET_DEV

static const char client_message[] = "hello from the client";
static const char server_message[] = "hello from the server";

// ---------------------------------------------------------------------------
// one frame for both ends, in the order a game runs it: update, receive,
// queue (done by the caller between frames), send last

struct exchange_t
{
    int server_got;
    int client_got;
};

static int frame( struct rocketnet_server_t * server, struct rocketnet_client_t * client, double now, struct exchange_t * exchange )
{
    rocketnet_server_update( server, now );
    rocketnet_client_update( client, now );

    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            CHECK( batch->client_index[i] == 0 );
            CHECK( batch->channel[i] == 0 );
            // a receipt type arrives as its twin: the receipt is the sender's
            CHECK( batch->reliability[i] == ROCKETNET_RELIABILITY_RELIABLE_ORDERED );
            CHECK( batch->bytes[i] == (int) sizeof( client_message ) && memcmp( batch->data[i], client_message, sizeof( client_message ) ) == 0 );
            exchange->server_got++;
        }
    }

    const struct rocketnet_message_batch_t * incoming = NULL;
    while ( ( incoming = rocketnet_client_receive_messages( client ) ) != NULL )
    {
        for ( int i = 0; i < incoming->num_messages; i++ )
        {
            CHECK( incoming->bytes[i] == (int) sizeof( server_message ) && memcmp( incoming->data[i], server_message, sizeof( server_message ) ) == 0 );
            exchange->client_got++;
        }
    }

    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server );
    return 0;
}

#endif // #if ROCKETNET_DEV

int main( void )
{
    // ---- version, tables and names: no init needed

    CHECK( rocketnet_version() == ROCKETNET_VERSION );
    CHECK( ROCKETNET_VERSION == ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH );

    for ( int i = ROCKETNET_STATUS_OK; i >= ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION; i-- )
        CHECK( rocketnet_status_name( (enum rocketnet_status_t) i ) != NULL );
    CHECK( strcmp( rocketnet_status_name( ROCKETNET_STATUS_OK ), "OK" ) == 0 );
    CHECK( strcmp( rocketnet_status_name( ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED ), "CONFIG_NOT_INITIALIZED" ) == 0 );

    for ( int i = ROCKETNET_LOG_LEVEL_NONE; i <= ROCKETNET_LOG_LEVEL_SPAM; i++ )
        CHECK( rocketnet_log_level_name( (enum rocketnet_log_level_t) i ) != NULL );
    CHECK( strcmp( rocketnet_log_level_name( ROCKETNET_LOG_LEVEL_ERROR ), "ERROR" ) == 0 );

    for ( int i = ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED; i <= ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        CHECK( rocketnet_client_state_name( (enum rocketnet_client_state_t) i ) != NULL );
    CHECK( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED ), "SERVER_DISCONNECTED" ) == 0 );
    CHECK( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT ), "RELIABLE_TIMED_OUT" ) == 0 );
    CHECK( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_PROTOCOL_VIOLATION ), "PROTOCOL_VIOLATION" ) == 0 );
    CHECK( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH ), "CONFIG_MISMATCH" ) == 0 );
    CHECK( strcmp( rocketnet_client_state_name( ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED ), "MESSAGE_MEMORY_EXHAUSTED" ) == 0 );

    CHECK( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_NONE ) != NULL );
    CHECK( rocketnet_disconnect_reason_name( ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED ) != NULL );

    // ---- process-wide setup, before init

    rocketnet_thread_priority( true );
    rocketnet_assert_function( consumer_assert );

    CHECK( rocketnet_init( NULL, consumer_malloc, consumer_free ) == ROCKETNET_STATUS_OK );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    // ---- the backend public key the server verifies tokens with: the
    // published development key in the development library (its tokens come
    // from rocketnet_create_dev_connect_token); in a release library, which
    // refuses the development key, any other backend's key

#if ROCKETNET_DEV
    static const uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
#else
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    for ( int i = 0; i < ROCKETNET_BACKEND_PUBLIC_KEY_BYTES; i++ )
        backend_public_key[i] = (uint8_t) ( 0x51 + 3 * i );
    static const uint8_t title_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x5A, 0x5A, 0x5A, 0x5A };
#endif

    // ---- configs: create is the one check. A NULL config is the defaults; a
    // zeroed config was never filled by default_config and is refused with
    // CONFIG_NOT_INITIALIZED and one log line (through the log function); in
    // quiet mode the same refusal logs nothing

    {
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
#if ROCKETNET_DEV
        struct rocketnet_client_t * null_client = rocketnet_client_create( NULL, NULL, &status );
        CHECK( null_client != NULL && status == ROCKETNET_STATUS_OK );
        rocketnet_client_destroy( null_client );
        struct rocketnet_server_t * null_server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, NULL, &status );
        CHECK( null_server != NULL && status == ROCKETNET_STATUS_OK );
        rocketnet_server_destroy( null_server );
#else
        // a production build refuses the defaults' published test packet filter
        rocketnet_quiet( true );
        CHECK( rocketnet_client_create( NULL, NULL, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );
        CHECK( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, NULL, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );
        // server required_memory runs create's config check, so it refuses
        // the same defaults with 0
        CHECK( rocketnet_server_required_memory( NULL ) == 0 );
        rocketnet_quiet( false );
#endif
        CHECK( rocketnet_client_required_memory( NULL ) > 0 );
#if ROCKETNET_DEV
        CHECK( rocketnet_server_required_memory( NULL ) > 0 );
#endif

        struct rocketnet_client_config_t zeroed_client;
        struct rocketnet_server_config_t zeroed_server;
        memset( &zeroed_client, 0, sizeof( zeroed_client ) );
        memset( &zeroed_server, 0, sizeof( zeroed_server ) );

        struct log_capture_t capture;
        memset( &capture, 0, sizeof( capture ) );
        rocketnet_log_function( consumer_log, &capture );

        status = ROCKETNET_STATUS_OK;
        CHECK( rocketnet_client_create( NULL, &zeroed_client, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
        CHECK( capture.lines == 1 && capture.level == ROCKETNET_LOG_LEVEL_ERROR );
        status = ROCKETNET_STATUS_OK;
        CHECK( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &zeroed_server, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
        CHECK( capture.lines == 2 );
        CHECK( rocketnet_client_required_memory( &zeroed_client ) == 0 );
        CHECK( rocketnet_server_required_memory( &zeroed_server ) == 0 );
        const int lines_before_quiet = capture.lines;

        rocketnet_quiet( true );
        status = ROCKETNET_STATUS_OK;
        CHECK( rocketnet_client_create( NULL, &zeroed_client, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
        rocketnet_quiet( false );
        CHECK( capture.lines == lines_before_quiet );

        rocketnet_log_function( NULL, NULL );
    }

    // ---- the server: the backend key set in code; the server makes its own
    // keypair at create

    struct rocketnet_server_config_t server_config;
    CHECK( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    CHECK( server_config.config_bytes == sizeof( server_config ) );
    server_config.protocol_id = PROTOCOL_ID;
    server_config.max_clients = 2;
    // no server transport pin: a production library refuses the portable
    // server, so the default auto order grants a native transport
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( server_config.backend_public_key ) );
#if !ROCKETNET_DEV
    // a production build needs the title's own packet filter
    memcpy( server_config.packet_filter, title_packet_filter, sizeof( server_config.packet_filter ) );
#endif
    CHECK( rocketnet_server_required_memory( &server_config ) > 0 );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    CHECK( server != NULL );
    CHECK( !rocketnet_server_running( server ) );
    CHECK( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    CHECK( rocketnet_server_running( server ) );
    CHECK( rocketnet_server_transport_name( server ) != NULL );
    CHECK( strcmp( rocketnet_server_transport_name( server ), "portable" ) != 0 );
    CHECK( rocketnet_server_max_clients( server ) == 2 );
    CHECK( rocketnet_server_port( server ) != 0 );

    // ---- the server's own public key (valid from create) and the addresses
    // it advertises once started: IPv4 only here, bound to 127.0.0.1

    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t zero_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memset( server_public_key, 0, sizeof( server_public_key ) );
    memset( zero_key, 0, sizeof( zero_key ) );
    rocketnet_server_public_key( server, server_public_key );
    CHECK( memcmp( server_public_key, zero_key, sizeof( zero_key ) ) != 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char expected_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( expected_address, sizeof( expected_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    CHECK( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    CHECK( strcmp( server_address, expected_address ) == 0 );
    {
        char ipv6_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
        CHECK( !rocketnet_server_ipv6_address( server, ipv6_address, sizeof( ipv6_address ) ) );
        CHECK( ipv6_address[0] == '\0' );
    }

#if ROCKETNET_DEV
    // ---- a development connect token, a string, for this server's address
    // and public key

    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, CLIENT_ID, PROTOCOL_ID,
                                               (uint64_t) time( NULL ), NULL, connect_token ) == ROCKETNET_STATUS_OK );
    CHECK( strlen( connect_token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
#endif

    // ---- the client

    struct rocketnet_client_config_t client_config;
    CHECK( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    CHECK( client_config.config_bytes == sizeof( client_config ) );
    client_config.transport = "portable";
    client_config.state_change_callback = consumer_state_change;
#if !ROCKETNET_DEV
    memcpy( client_config.packet_filter, title_packet_filter, sizeof( client_config.packet_filter ) );
#endif
    CHECK( rocketnet_client_required_memory( &client_config ) > 0 );

    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    CHECK( client != NULL );
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    CHECK( rocketnet_client_index( client ) == -1 );
    // a connect token is the NUL-terminated string the backend gives; anything
    // else is refused
    CHECK( rocketnet_client_connect( client, "not a connect token" ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    CHECK( rocketnet_client_state( client ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_disconnect( client );
    CHECK( rocketnet_client_disconnected( client ) );

#if ROCKETNET_DEV
    CHECK( rocketnet_client_connect( client, connect_token ) == ROCKETNET_STATUS_OK );
    CHECK( strcmp( rocketnet_client_transport_name( client ), "portable" ) == 0 );

    // ---- connect, then one receipted message each way

    // the game's clock starts at 1000.0, with real seconds on top
    const double wall_start = rocketnet_time();
    double now = 1000.0;
    struct exchange_t exchange;
    memset( &exchange, 0, sizeof( exchange ) );
    uint32_t client_serial = 0;
    uint32_t server_serial = 0;
    int client_acked = 0;
    int server_acked = 0;

    while ( !( exchange.server_got && exchange.client_got && client_acked && server_acked ) && rocketnet_time() - wall_start < 10.0 )
    {
        now = 1000.0 + ( rocketnet_time() - wall_start );
        CHECK( frame( server, client, now, &exchange ) == 0 );
        CHECK( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );

        if ( !client_serial && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            CHECK( rocketnet_client_index( client ) == 0 );
            CHECK( rocketnet_client_set_send_kbps( client, 0 ) == ROCKETNET_STATUS_OK );
            CHECK( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
            CHECK( rocketnet_client_set_send_kbps( client, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS ) == ROCKETNET_STATUS_OK );

            // an aborted start sends nothing, and the next start is free
            CHECK( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8 ) != NULL );
            rocketnet_client_abort_message( client );

            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, (int) sizeof( client_message ) );
            CHECK( out != NULL );
            memcpy( out, client_message, sizeof( client_message ) );
            client_serial = rocketnet_client_finish_message( client, (int) sizeof( client_message ) );
            CHECK( client_serial != 0 );
        }

        if ( exchange.server_got && !server_serial )
        {
            CHECK( rocketnet_server_set_send_kbps( server, 0, 0 ) == ROCKETNET_STATUS_OK );

            // an aborted start sends nothing
            CHECK( rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8 ) != NULL );
            rocketnet_server_abort_message( server );

            // a refused finish aborts the pending start, so the next start is
            // never blocked
            CHECK( rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8 ) != NULL );
            CHECK( rocketnet_server_finish_message( server, 0 ) == 0 );

            uint8_t * out = rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, (int) sizeof( server_message ) );
            CHECK( out != NULL );
            memcpy( out, server_message, sizeof( server_message ) );
            server_serial = rocketnet_server_finish_message( server, (int) sizeof( server_message ) );
            CHECK( server_serial != 0 );
        }

        struct rocketnet_receipt_t receipts[16];
        const int client_receipts = rocketnet_client_receipts( client, receipts, 16 );
        for ( int i = 0; i < client_receipts && i < 16; i++ )
            if ( receipts[i].serial == client_serial && receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                client_acked = 1;
        const int server_receipts = rocketnet_server_receipts( server, 0, receipts, 16 );
        for ( int i = 0; i < server_receipts && i < 16; i++ )
            if ( receipts[i].serial == server_serial && receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                server_acked = 1;

        rocketnet_sleep( 0.001 );
    }

    CHECK( saw_connected );
    CHECK( exchange.server_got == 1 );
    CHECK( exchange.client_got == 1 );
    CHECK( client_acked && server_acked );

    // ---- the queries and the three stats structs, each one call into a
    // snapshot the caller owns

    CHECK( rocketnet_server_num_connected_clients( server ) == 1 );
    CHECK( rocketnet_server_client_connected( server, 0 ) );
    CHECK( !rocketnet_server_client_connected( server, 1 ) );
    CHECK( rocketnet_server_client_id( server, 0 ) == CLIENT_ID );
    CHECK( rocketnet_server_disconnect_reason( server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );

    struct rocketnet_client_stats_t client_stats;
    rocketnet_client_stats( client, &client_stats );
    CHECK( client_stats.packets_sent > 0 && client_stats.packets_received > 0 );
    CHECK( client_stats.bytes_sent > 0 && client_stats.bytes_received > 0 );
    CHECK( client_stats.payloads_received > 0 );
    CHECK( client_stats.receipts_acked >= 1 );
    CHECK( client_stats.receipts_dropped == 0 );
    CHECK( client_stats.rtt_ms >= 0.0 && client_stats.jitter_ms >= 0.0 && client_stats.packet_loss_percent >= 0.0 );
    CHECK( client_stats.bandwidth_sent_kbps >= 0.0 && client_stats.bandwidth_received_kbps >= 0.0 );

    struct rocketnet_server_stats_t server_stats;
    rocketnet_server_stats( server, &server_stats );
    CHECK( server_stats.packets_sent > 0 && server_stats.packets_received > 0 );
    CHECK( server_stats.connections_accepted == 1 );
    CHECK( server_stats.payloads_received > 0 );
    CHECK( server_stats.receipts_acked >= 1 );
    CHECK( server_stats.receipts_dropped == 0 );
    CHECK( server_stats.api_misuses == client_stats.api_misuses );

    struct rocketnet_server_client_stats_t slot_stats;
    rocketnet_server_client_stats( server, 0, &slot_stats );
    CHECK( slot_stats.packets_sent > 0 && slot_stats.bytes_received > 0 );
    CHECK( slot_stats.rtt_ms >= 0.0 && slot_stats.bandwidth_acked_kbps >= 0.0 );
    rocketnet_server_client_stats( server, 1, &slot_stats );
    CHECK( slot_stats.packets_sent == 0 && slot_stats.packets_received == 0 );

#else
    // ---- a release library mints no tokens: the client stays disconnected,
    // and the stats structs read the idle objects

    struct rocketnet_client_stats_t client_stats;
    rocketnet_client_stats( client, &client_stats );
    CHECK( client_stats.packets_sent == 0 && client_stats.receipts_dropped == 0 );
    struct rocketnet_server_stats_t server_stats;
    rocketnet_server_stats( server, &server_stats );
    CHECK( server_stats.connections_accepted == 0 && server_stats.receipts_dropped == 0 );
    CHECK( server_stats.api_misuses == client_stats.api_misuses );
#endif

    // ---- api misuse is a stats field: two deliberate NULL calls count two
    // (a debug library also calls the installed assert function for each)

    {
        const uint64_t misuse_before = server_stats.api_misuses;
        const int assertions_before = assertions;
        CHECK( rocketnet_client_state( NULL ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        struct rocketnet_client_stats_t null_stats;
        rocketnet_client_stats( NULL, &null_stats );
        CHECK( null_stats.api_misuses == misuse_before + 2 );
        CHECK( null_stats.packets_sent == 0 );
        CHECK( assertions == assertions_before + EXPECTED_NULL_ASSERTIONS );
    }

#if ROCKETNET_DEV
    // ---- the server ends the connection: the client's state says how, and
    // connect is accepted again from that ended state

    rocketnet_server_disconnect_client( server, 0 );
    while ( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED && rocketnet_time() - wall_start < 20.0 )
    {
        now = 1000.0 + ( rocketnet_time() - wall_start );
        CHECK( frame( server, client, now, &exchange ) == 0 );
        rocketnet_sleep( 0.001 );
    }
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    CHECK( rocketnet_client_index( client ) == -1 );
    CHECK( !rocketnet_server_client_connected( server, 0 ) );
    rocketnet_client_disconnect( client );
    while ( !rocketnet_client_disconnected( client ) && rocketnet_time() - wall_start < 20.0 )
        rocketnet_sleep( 0.001 );
    CHECK( rocketnet_client_disconnected( client ) );

    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, CLIENT_ID, PROTOCOL_ID,
                                               (uint64_t) time( NULL ), NULL, connect_token ) == ROCKETNET_STATUS_OK );
    CHECK( rocketnet_client_connect( client, connect_token ) == ROCKETNET_STATUS_OK );
    while ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED && rocketnet_time() - wall_start < 30.0 )
    {
        now = 1000.0 + ( rocketnet_time() - wall_start );
        CHECK( frame( server, client, now, &exchange ) == 0 );
        CHECK( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
        rocketnet_sleep( 0.001 );
    }
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    rocketnet_server_disconnect_all_clients( server );
    while ( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED && rocketnet_time() - wall_start < 40.0 )
    {
        now = 1000.0 + ( rocketnet_time() - wall_start );
        CHECK( frame( server, client, now, &exchange ) == 0 );
        rocketnet_sleep( 0.001 );
    }
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    CHECK( rocketnet_server_num_connected_clients( server ) == 0 );
#endif

    // ---- teardown

    rocketnet_client_disconnect( client );
    const double disconnect_deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_client_disconnected( client ) && rocketnet_time() < disconnect_deadline )
        rocketnet_sleep( 0.001 );
    CHECK( rocketnet_client_disconnected( client ) );
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_destroy( client );
    rocketnet_server_stop( server );
    const double stop_deadline = rocketnet_time() + 10.0;
    while ( !rocketnet_server_stopped( server ) && rocketnet_time() < stop_deadline )
        rocketnet_sleep( 0.001 );
    CHECK( rocketnet_server_stopped( server ) );
    CHECK( !rocketnet_server_running( server ) );
    rocketnet_server_destroy( server );

    rocketnet_assert_function( NULL );
    CHECK( rocketnet_term() == ROCKETNET_STATUS_OK );

    puts( "installed SDK consumer passed" );
    return 0;
}
