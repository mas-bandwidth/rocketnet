/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows lifecycle stress tests: repeated init/term, server create/destroy,
    same-port server restart, client connect/disconnect churn, and mid-traffic
    teardown across portable, iocp, and rio transports, verifying process
    handle counts and thread counts remain bounded.
*/

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <tlhelp32.h>

#include "rocketnet.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>

static void windows_lifecycle_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                     \
do                                                                                                                       \
{                                                                                                                        \
    if ( !(condition) )                                                                                                  \
        windows_lifecycle_check_handler( #condition, (const char *) __FUNCTION__, (const char *) __FILE__, __LINE__ );   \
} while ( 0 )

struct windows_process_counts_t
{
    DWORD handles;
    DWORD threads;
};

static DWORD windows_current_thread_count()
{
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    rocketnet_check( snapshot != INVALID_HANDLE_VALUE );

    THREADENTRY32 entry;
    memset( &entry, 0, sizeof( entry ) );
    entry.dwSize = sizeof( entry );

    DWORD count = 0;
    if ( Thread32First( snapshot, &entry ) )
    {
        do
        {
            if ( entry.th32OwnerProcessID == pid )
                count++;
            entry.dwSize = sizeof( entry );
        }
        while ( Thread32Next( snapshot, &entry ) );
    }

    const BOOL closed = CloseHandle( snapshot );
    rocketnet_check( closed != FALSE );
    rocketnet_check( count > 0 );
    return count;
}

static windows_process_counts_t windows_record_counts()
{
    windows_process_counts_t counts;
    counts.threads = windows_current_thread_count();
    counts.handles = 0;
    const BOOL ok = GetProcessHandleCount( GetCurrentProcess(), &counts.handles );
    rocketnet_check( ok != FALSE );
    rocketnet_check( counts.handles > 0 );
    return counts;
}

// A handle leak is a difference either way: the handle check fails when the count moves
// by more than the tolerance in either direction. A thread leak is growth only: the thread
// check fails when the count grows past the baseline plus the tolerance, and never when it
// shrinks, because the process thread count also holds Windows-owned thread-pool workers
// that are alive when the baseline is taken and exit on their own during a case.
static void windows_assert_counts_within( const char * label, windows_process_counts_t before, windows_process_counts_t after, long tolerance )
{
    const long handle_diff = (long) after.handles - (long) before.handles;
    const long thread_diff = (long) after.threads - (long) before.threads;
    if ( labs( handle_diff ) > tolerance || thread_diff > tolerance )
    {
        printf( "%s count mismatch: handles before=%lu after=%lu (diff=%+ld, limit=+/-%ld), threads before=%lu after=%lu (diff=%+ld, limit=+%ld)\n",
                label,
                (unsigned long) before.handles, (unsigned long) after.handles, handle_diff, tolerance,
                (unsigned long) before.threads, (unsigned long) after.threads, thread_diff, tolerance );
        fflush( stdout );
    }
    rocketnet_check( labs( handle_diff ) <= tolerance );
    rocketnet_check( thread_diff <= tolerance );
}

class windows_transport_env_guard_t
{
    struct saved_var_t
    {
        const char * name;
        std::string value;
        bool was_set;
    };
    saved_var_t vars[2];

public:
    windows_transport_env_guard_t()
    {
        const char * names[2] = { "ROCKETNET_SERVER_TRANSPORT", "ROCKETNET_CLIENT_TRANSPORT" };
        for ( int i = 0; i < 2; i++ )
        {
            vars[i].name = names[i];
            const char * val = getenv( names[i] );
            vars[i].was_set = ( val != NULL && val[0] != '\0' );
            vars[i].value = val ? val : "";
            rocketnet_check( _putenv_s( names[i], "" ) == 0 );
        }
    }

    ~windows_transport_env_guard_t()
    {
        for ( int i = 0; i < 2; i++ )
        {
            if ( vars[i].was_set )
                rocketnet_check( _putenv_s( vars[i].name, vars[i].value.c_str() ) == 0 );
            else
                rocketnet_check( _putenv_s( vars[i].name, "" ) == 0 );
        }
    }
};

static const char * const k_windows_server_transports[3] = { "portable", "iocp", "rio" };

static struct rocketnet_server_t * windows_start_server( const char * address, const char * transport, int max_clients )
{
    struct rocketnet_server_config_t config;
    rocketnet_check( rocketnet_server_dev_config( &config ) == ROCKETNET_STATUS_OK );
    config.max_clients = max_clients;
    config.transport = transport;

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, &config, NULL, 0 );
    rocketnet_check( server != NULL );
    const enum rocketnet_status_t status = rocketnet_server_start( server );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_server_running( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_name( server ) != NULL );
    rocketnet_check( strcmp( rocketnet_server_transport_name( server ), transport ) == 0 );
    return server;
}

static struct rocketnet_client_t * windows_create_client()
{
    struct rocketnet_client_config_t config;
    rocketnet_check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL, 0 );
    rocketnet_check( client != NULL );
    return client;
}

// Skip one 0.1s client send interval each time the client handshake state moves
// (as functional/posix_lifecycle_stress.cpp does), so the next update sends
// immediately without sleeping 200ms per connect on the 10 Hz send_rate timer.
static double windows_connect_client_to_server( struct rocketnet_client_t * client, struct rocketnet_server_t * server,
                                                const char * server_address, uint64_t client_id, int expected_connected )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, client_id, (uint64_t) time( NULL ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double client_clock_offset = 0.0;
    enum rocketnet_client_state_t previous_state = rocketnet_client_state( client );
    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        const double now = rocketnet_platform_time();
        rocketnet_check( rocketnet_client_update( client, now + client_clock_offset ) == ROCKETNET_UPDATE_OK );
        rocketnet_check( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        const enum rocketnet_client_state_t state = rocketnet_client_state( client );
        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_num_connected_clients( server ) == expected_connected &&
             rocketnet_server_client_connected( server, rocketnet_client_index( client ) ) )
            break;
        if ( state != previous_state )
        {
            previous_state = state;
            client_clock_offset += 0.1;
            continue;
        }
        rocketnet_platform_sleep( 0.0005 );
    }

    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_num_connected_clients( server ) == expected_connected );
    rocketnet_check( rocketnet_server_client_connected( server, rocketnet_client_index( client ) ) == 1 );
    return client_clock_offset;
}

static void windows_exchange_one_reliable_message( struct rocketnet_client_t * client, struct rocketnet_server_t * server, uint32_t tag, double client_clock_offset )
{
    const uint64_t slot = rocketnet_client_index( client );
    uint8_t to_server[8];
    uint8_t to_client[8];
    memcpy( to_server, "c2s:", 4 );
    memcpy( to_server + 4, &tag, 4 );
    memcpy( to_client, "s2c:", 4 );
    memcpy( to_client + 4, &tag, 4 );

    uint8_t * client_out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    rocketnet_check( client_out != NULL );
    memcpy( client_out, to_server, 8 );
    rocketnet_check( rocketnet_client_finish_message( client, 8 ) != 0 );

    uint8_t * server_out = rocketnet_server_start_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
    rocketnet_check( server_out != NULL );
    memcpy( server_out, to_client, 8 );
    rocketnet_check( rocketnet_server_finish_message( server, slot, 8 ) != 0 );

    client_clock_offset += 0.1;
    bool server_got = false;
    bool client_got = false;
    const double start = rocketnet_platform_time();
    while ( ( !server_got || !client_got ) && rocketnet_platform_time() - start < 5.0 )
    {
        const double now = rocketnet_platform_time();
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_check( rocketnet_client_update( client, now + client_clock_offset ) == ROCKETNET_UPDATE_OK );
        rocketnet_check( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );

        if ( !server_got )
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            if ( rocketnet_server_receive_message( server, slot, 0, &data, &bytes ) )
            {
                rocketnet_check( bytes == 8 );
                rocketnet_check( memcmp( data, to_server, 8 ) == 0 );
                rocketnet_server_release_message( server, slot, data );
                server_got = true;
            }
        }

        if ( !client_got )
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            if ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
            {
                rocketnet_check( bytes == 8 );
                rocketnet_check( memcmp( data, to_client, 8 ) == 0 );
                rocketnet_client_release_message( client, data );
                client_got = true;
            }
        }

        if ( !server_got || !client_got )
            rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( server_got );
    rocketnet_check( client_got );
}

// Warm up one-time thread-local sleep timer and Winsock provider state before
// taking a baseline handle/thread snapshot.
static void windows_warmup_transports()
{
    rocketnet_platform_sleep( 0.001 );
    for ( int t = 0; t < 3; t++ )
    {
        struct rocketnet_server_t * server = windows_start_server( "127.0.0.1:0", k_windows_server_transports[t], 4 );
        char address[64];
        snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
        struct rocketnet_client_t * client = windows_create_client();
        const double offset = windows_connect_client_to_server( client, server, address, (uint64_t) ( 10 + t ), 1 );
        windows_exchange_one_reliable_message( client, server, (uint32_t) t, offset );
        rocketnet_client_disconnect( client );
        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
    }
    rocketnet_platform_sleep( 0.010 );
}

// 1. windows_init_term_cycles
void windows_init_term_cycles()
{
    rocketnet_platform_sleep( 0.001 );

    // The suite runner calls rocketnet_init(NULL) before invoking the case.
    // Terminate that outer session first so the 1000-cycle loop exercises real
    // rocketnet_init and rocketnet_term transitions from the uninitialized state.
    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );

    WSADATA wsa_data;
    memset( &wsa_data, 0, sizeof( wsa_data ) );
    rocketnet_check( WSAStartup( MAKEWORD( 2, 2 ), &wsa_data ) == 0 );

    SOCKET caller_socket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    rocketnet_check( caller_socket != INVALID_SOCKET );

    sockaddr_in bind_addr;
    memset( &bind_addr, 0, sizeof( bind_addr ) );
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    bind_addr.sin_port = 0;
    rocketnet_check( bind( caller_socket, (const sockaddr *) &bind_addr, sizeof( bind_addr ) ) == 0 );

    sockaddr_in bound_addr;
    int bound_len = sizeof( bound_addr );
    memset( &bound_addr, 0, sizeof( bound_addr ) );
    rocketnet_check( getsockname( caller_socket, (sockaddr *) &bound_addr, &bound_len ) == 0 );

    const DWORD timeout_ms = 2000;
    rocketnet_check( setsockopt( caller_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout_ms, sizeof( timeout_ms ) ) == 0 );

    // One warm-up cycle so any one-time library init is settled before the baseline.
    rocketnet_check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );

    const windows_process_counts_t before = windows_record_counts();

    for ( int i = 0; i < 1000; i++ )
    {
        rocketnet_check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
        rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    }

    const windows_process_counts_t after = windows_record_counts();
    windows_assert_counts_within( "windows_init_term_cycles", before, after, 2 );

    // The socket opened with the caller's own WSAStartup before the loop must
    // still send and receive after 1000 rocketnet_init/rocketnet_term cycles.
    const char ping[4] = { 'p', 'i', 'n', 'g' };
    rocketnet_check( sendto( caller_socket, ping, 4, 0, (const sockaddr *) &bound_addr, sizeof( bound_addr ) ) == 4 );

    char pong[4] = { 0, 0, 0, 0 };
    sockaddr_in from_addr;
    int from_len = sizeof( from_addr );
    rocketnet_check( recvfrom( caller_socket, pong, 4, 0, (sockaddr *) &from_addr, &from_len ) == 4 );
    rocketnet_check( memcmp( ping, pong, 4 ) == 0 );

    rocketnet_check( closesocket( caller_socket ) == 0 );
    rocketnet_check( WSACleanup() == 0 );

    // Verify WSACleanup balanced the caller's WSAStartup: Winsock is now uninitialized.
    SOCKET probe = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    rocketnet_check( probe == INVALID_SOCKET );
    rocketnet_check( WSAGetLastError() == WSANOTINITIALISED );

    // Restore rocketnet_init for the suite runner's post-test rocketnet_term().
    rocketnet_check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
}

// 2. windows_server_create_destroy_cycles
void windows_server_create_destroy_cycles()
{
    windows_transport_env_guard_t env_guard;
    windows_warmup_transports();

    const windows_process_counts_t overall_before = windows_record_counts();

    for ( int t = 0; t < 3; t++ )
    {
        const char * transport = k_windows_server_transports[t];
        const windows_process_counts_t before = windows_record_counts();

        for ( int i = 0; i < 200; i++ )
        {
            struct rocketnet_server_t * server = windows_start_server( "127.0.0.1:0", transport, 4 );
            rocketnet_check( rocketnet_server_update( server, rocketnet_platform_time() ) == ROCKETNET_UPDATE_OK );
            rocketnet_server_destroy( server );
        }

        const windows_process_counts_t after_first = windows_record_counts();
        windows_assert_counts_within( transport, before, after_first, 2 );

        for ( int i = 0; i < 200; i++ )
        {
            struct rocketnet_server_t * server = windows_start_server( "127.0.0.1:0", transport, 4 );
            rocketnet_check( rocketnet_server_update( server, rocketnet_platform_time() ) == ROCKETNET_UPDATE_OK );
            rocketnet_server_destroy( server );
        }

        const windows_process_counts_t after_second = windows_record_counts();
        windows_assert_counts_within( transport, before, after_second, 2 );
        rocketnet_check( (long) after_second.handles <= (long) after_first.handles + 2 );
        rocketnet_check( (long) after_second.threads <= (long) after_first.threads + 2 );
    }

    const windows_process_counts_t overall_after = windows_record_counts();
    windows_assert_counts_within( "windows_server_create_destroy_cycles", overall_before, overall_after, 2 );
}

// 3. windows_server_restart_same_port
void windows_server_restart_same_port()
{
    windows_transport_env_guard_t env_guard;
    windows_warmup_transports();

    // Pick a free UDP port once.
    struct rocketnet_server_t * probe_server = windows_start_server( "127.0.0.1:0", "portable", 4 );
    const uint16_t port = rocketnet_server_port( probe_server );
    rocketnet_check( port != 0 );
    rocketnet_server_destroy( probe_server );

    char bind_address[64];
    snprintf( bind_address, sizeof( bind_address ), "127.0.0.1:%u", (unsigned) port );

    const windows_process_counts_t before = windows_record_counts();

    for ( int t = 0; t < 3; t++ )
    {
        const char * transport = k_windows_server_transports[t];
        for ( int i = 0; i < 100; i++ )
        {
            const double rebind_start = rocketnet_platform_time();
            struct rocketnet_server_t * server = windows_start_server( bind_address, transport, 4 );
            const double rebind_elapsed = rocketnet_platform_time() - rebind_start;
            rocketnet_check( rebind_elapsed < 1.0 );
            rocketnet_check( rocketnet_server_port( server ) == port );

            struct rocketnet_client_t * client = windows_create_client();
            const uint64_t client_id = (uint64_t) ( 1000 + t * 1000 + i );
            const double offset = windows_connect_client_to_server( client, server, bind_address, client_id, 1 );
            windows_exchange_one_reliable_message( client, server, (uint32_t) ( t * 1000 + i ), offset );

            rocketnet_client_destroy( client );
            rocketnet_server_destroy( server );
        }
    }

    const windows_process_counts_t after = windows_record_counts();
    windows_assert_counts_within( "windows_server_restart_same_port", before, after, 2 );
}

// 4. windows_client_connect_disconnect_cycles
// The thread check counts growth only (windows_assert_counts_within): the baseline is
// taken while Windows-owned thread-pool workers are alive, and they may exit during the
// 200 cycles, so a thread count below the baseline is not a leak.
void windows_client_connect_disconnect_cycles()
{
    windows_transport_env_guard_t env_guard;
    windows_warmup_transports();

    const windows_process_counts_t overall_before = windows_record_counts();

    for ( int t = 0; t < 3; t++ )
    {
        const char * transport = k_windows_server_transports[t];

        // Count from before the server is created, so the span that ends after
        // the server is destroyed also holds what the server itself leaves behind.
        const windows_process_counts_t before_server = windows_record_counts();

        struct rocketnet_server_t * server = windows_start_server( "127.0.0.1:0", transport, 8 );
        const uint16_t port = rocketnet_server_port( server );
        rocketnet_check( port != 0 );

        char server_address[64];
        snprintf( server_address, sizeof( server_address ), "127.0.0.1:%u", (unsigned) port );

        rocketnet_check( rocketnet_server_num_connected_clients( server ) == 0 );
        const windows_process_counts_t before_loop = windows_record_counts();

        for ( int i = 0; i < 200; i++ )
        {
            struct rocketnet_client_t * client = windows_create_client();
            const uint64_t client_id = (uint64_t) ( 10000 + t * 1000 + i );
            const double offset = windows_connect_client_to_server( client, server, server_address, client_id, 1 );
            windows_exchange_one_reliable_message( client, server, (uint32_t) ( t * 1000 + i ), offset );

            rocketnet_client_disconnect( client );
            rocketnet_client_destroy( client );

            const double disconnect_start = rocketnet_platform_time();
            while ( rocketnet_server_num_connected_clients( server ) != 0 &&
                    rocketnet_platform_time() - disconnect_start < 2.0 )
            {
                rocketnet_check( rocketnet_server_update( server, rocketnet_platform_time() ) == ROCKETNET_UPDATE_OK );
                rocketnet_server_send_packets( server );
                rocketnet_platform_sleep( 0.0005 );
            }
            rocketnet_check( rocketnet_server_num_connected_clients( server ) == 0 );
        }

        rocketnet_check( rocketnet_server_num_connected_clients( server ) == 0 );
        const windows_process_counts_t after_loop = windows_record_counts();
        windows_assert_counts_within( transport, before_loop, after_loop, 2 );

        rocketnet_server_destroy( server );

        const windows_process_counts_t after_server = windows_record_counts();
        windows_assert_counts_within( transport, before_server, after_server, 2 );
    }

    const windows_process_counts_t overall_after = windows_record_counts();
    windows_assert_counts_within( "windows_client_connect_disconnect_cycles", overall_before, overall_after, 2 );
}

// 5. windows_destroy_while_traffic
void windows_destroy_while_traffic()
{
    windows_transport_env_guard_t env_guard;
    windows_warmup_transports();

    const windows_process_counts_t before = windows_record_counts();

    uint8_t payload[256];
    for ( int i = 0; i < (int) sizeof( payload ); i++ )
        payload[i] = (uint8_t) ( i * 37 + 11 );

    for ( int cycle = 0; cycle < 50; cycle++ )
    {
        for ( int t = 0; t < 3; t++ )
        {
            const char * transport = k_windows_server_transports[t];
            struct rocketnet_server_t * server = windows_start_server( "127.0.0.1:0", transport, 8 );
            char server_address[64];
            snprintf( server_address, sizeof( server_address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );

            struct rocketnet_client_t * clients[4] = { NULL, NULL, NULL, NULL };
            for ( int c = 0; c < 4; c++ )
                clients[c] = windows_create_client();

            for ( int c = 0; c < 4; c++ )
            {
                uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
                const uint64_t client_id = (uint64_t) ( 50000 + cycle * 100 + t * 10 + c );
                rocketnet_check( rocketnet_write_dev_connect_token( server_address, client_id, (uint64_t) time( NULL ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
                rocketnet_check( rocketnet_client_connect( clients[c], token ) == ROCKETNET_STATUS_OK );
            }

            const double connect_start = rocketnet_platform_time();
            while ( rocketnet_platform_time() - connect_start < 5.0 )
            {
                const double now = rocketnet_platform_time();
                rocketnet_check( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
                rocketnet_server_send_packets( server );
                int connected_clients = 0;
                for ( int c = 0; c < 4; c++ )
                {
                    rocketnet_check( rocketnet_client_update( clients[c], now ) == ROCKETNET_UPDATE_OK );
                    rocketnet_client_send_packets( clients[c] );
                    if ( rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
                        connected_clients++;
                }
                if ( connected_clients == 4 && rocketnet_server_num_connected_clients( server ) == 4 )
                    break;
                rocketnet_platform_sleep( 0.001 );
            }

            for ( int c = 0; c < 4; c++ )
                rocketnet_check( rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
            rocketnet_check( rocketnet_server_num_connected_clients( server ) == 4 );

            // Pump reliable and unreliable messages continuously in a tight loop,
            // leaving packets and unacknowledged messages in flight at destroy.
            for ( int step = 0; step < 8; step++ )
            {
                const double now = rocketnet_platform_time();
                for ( int c = 0; c < 4; c++ )
                {
                    const uint64_t slot = rocketnet_client_index( clients[c] );
                    const int channel = step & 3;

                    uint8_t * c_rel = rocketnet_client_start_message( clients[c], channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( payload ) );
                    if ( c_rel )
                    {
                        memcpy( c_rel, payload, sizeof( payload ) );
                        rocketnet_check( rocketnet_client_finish_message( clients[c], (int) sizeof( payload ) ) != 0 );
                    }

                    uint8_t * c_unrel = rocketnet_client_start_message( clients[c], channel, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( payload ) );
                    if ( c_unrel )
                    {
                        memcpy( c_unrel, payload, sizeof( payload ) );
                        rocketnet_check( rocketnet_client_finish_message( clients[c], (int) sizeof( payload ) ) != 0 );
                    }

                    uint8_t * s_rel = rocketnet_server_start_message( server, slot, channel, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( payload ) );
                    if ( s_rel )
                    {
                        memcpy( s_rel, payload, sizeof( payload ) );
                        rocketnet_check( rocketnet_server_finish_message( server, slot, (int) sizeof( payload ) ) != 0 );
                    }

                    uint8_t * s_unrel = rocketnet_server_start_message( server, slot, channel, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( payload ) );
                    if ( s_unrel )
                    {
                        memcpy( s_unrel, payload, sizeof( payload ) );
                        rocketnet_check( rocketnet_server_finish_message( server, slot, (int) sizeof( payload ) ) != 0 );
                    }

                    rocketnet_client_send_packets( clients[c] );
                }

                rocketnet_server_send_packets( server );
                if ( step < 4 )
                {
                    rocketnet_check( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
                    for ( int c = 0; c < 4; c++ )
                        rocketnet_check( rocketnet_client_update( clients[c], now ) == ROCKETNET_UPDATE_OK );
                }
            }

            // Destroy mid-stream without draining or disconnecting first.
            if ( ( cycle & 1 ) == 0 )
            {
                const double t0 = rocketnet_platform_time();
                rocketnet_server_destroy( server );
                rocketnet_check( rocketnet_platform_time() - t0 < 2.0 );

                for ( int c = 0; c < 4; c++ )
                {
                    const double tc = rocketnet_platform_time();
                    rocketnet_client_destroy( clients[c] );
                    rocketnet_check( rocketnet_platform_time() - tc < 2.0 );
                }
            }
            else
            {
                for ( int c = 0; c < 4; c++ )
                {
                    const double tc = rocketnet_platform_time();
                    rocketnet_client_destroy( clients[c] );
                    rocketnet_check( rocketnet_platform_time() - tc < 2.0 );
                }

                const double t0 = rocketnet_platform_time();
                rocketnet_server_destroy( server );
                rocketnet_check( rocketnet_platform_time() - t0 < 2.0 );
            }
        }
    }

    const windows_process_counts_t after = windows_record_counts();
    windows_assert_counts_within( "windows_destroy_while_traffic", before, after, 2 );
}

#endif // defined(_WIN32)
