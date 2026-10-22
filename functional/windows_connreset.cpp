/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Windows connection reset. Winsock answers a UDP send to a port no socket
    holds with an ICMP port unreachable that it reports to the next receive
    as WSAECONNRESET (or WSAENETRESET, or an unreachable completion status).
    A receive loop that stops at that error, or a completion that is not
    posted again, stalls a server whose client died without closing its
    socket. Every rocketnet socket turns the reports off and every receive
    path skips them (DRIVERS.md, "Windows connection reset"); these cases
    prove it per server transport and family with a client process ended by
    TerminateProcess, with sends to a closed port, and for the client.
*/

#include "../test/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "../test/portable_crt.h"
#include "../test/time_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#include "rocketnet_server_iocp.h"

#define NOMINMAX
#include <windows.h>

#define WINDOWS_CONNRESET_CHILD_ENV "ROCKETNET_CONNRESET_CHILD_SERVER"
#define WINDOWS_CONNRESET_PARENT_ENV "ROCKETNET_CONNRESET_PARENT_SERVER"
#define WINDOWS_CONNRESET_TICK ( 1.0 / 60.0 )
// the 50 ms allowance over one tick is a Release bound; a Debug or sanitizer
// build scales it by the suites' one time scale (test/time_scale.h). Measured
// 2026-10-22 on captain, Debug, exclusive, 10 runs of the 16 cases: max gap
// 183.5 ms, 20 of 160 over the unscaled 66.7 ms. Release keeps 66.7 ms.
#define WINDOWS_CONNRESET_GAP_BOUND ( WINDOWS_CONNRESET_TICK + 0.050 * ROCKETNET_FUNCTIONAL_TIME_SCALE )
#define WINDOWS_CONNRESET_LIVE_CLIENTS 3
#define WINDOWS_CONNRESET_PAYLOAD_BYTES 32
// packets a server sends each client per tick in the hard-killed case: a
// snapshot split across a few packets, so the dead client draws several ICMP
// reports per tick, as a game server's traffic does
#define WINDOWS_CONNRESET_SNAPSHOT_PACKETS 4

static void windows_connreset_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "windows_connreset check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define windows_connreset_require( condition ) windows_connreset_check( (condition), #condition, __LINE__ )

struct windows_connreset_transport_t
{
    const char * transport;        // the server transport pinned by the config
    bool plain;                 // iocp with USO and URO disabled
    bool ipv6;
};

static void windows_connreset_clear_env()
{
    // the case pins its own transports: an ambient pin must not override them
    _putenv_s( "ROCKETNET_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_SERVER_TRANSPORT", "" );
    _putenv_s( "ROCKETNET_CLIENT_TRANSPORT", "" );
}

static const char * windows_connreset_host( bool ipv6 )
{
    return ipv6 ? "[::1]" : "127.0.0.1";
}

static struct rocketnet_server_transport_t * windows_connreset_server( const windows_connreset_transport_t * transport, int port, int max_clients )
{
    windows_connreset_clear_env();
    rocketnet_server_iocp_test_force_flags( transport->plain ? ( ROCKETNET_IOCP_DISABLE_USO | ROCKETNET_IOCP_DISABLE_URO ) : 0 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    snprintf( config.transport, sizeof( config.transport ), "%s", transport->transport );

    char bind[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( bind, sizeof( bind ), "%s:%d", windows_connreset_host( transport->ipv6 ), port );
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, bind, &config );
    rocketnet_server_iocp_test_force_flags( 0 );
    windows_connreset_require( server != NULL );
    windows_connreset_require( strcmp( rocketnet_server_transport_io_name( server ), transport->transport ) == 0 );
    return server;
}

static void windows_connreset_server_address( struct rocketnet_server_transport_t * server, bool ipv6, char * address, size_t bytes )
{
    snprintf( address, bytes, "%s:%d", windows_connreset_host( ipv6 ), rocketnet_server_transport_port( server ) );
}

static struct rocketnet_client_transport_t * windows_connreset_client( const char * server_address )
{
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    windows_connreset_require( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    windows_connreset_require( client != NULL );
    return client;
}

static void windows_connreset_client_tick( struct rocketnet_client_transport_t * client, double now, uint32_t sequence, bool send, uint64_t * sent, uint64_t * received )
{
    rocketnet_client_transport_update( client, now );
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    while ( rocketnet_client_transport_receive_packet( client, payload, (int) sizeof( payload ) ) > 0 )
    {
        if ( received )
            ( *received )++;
    }
    if ( send && rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        memset( payload, 0, WINDOWS_CONNRESET_PAYLOAD_BYTES );
        memcpy( payload, &sequence, sizeof( sequence ) );
        rocketnet_client_transport_send_packet( client, payload, WINDOWS_CONNRESET_PAYLOAD_BYTES );
        if ( sent )
            ( *sent )++;
    }
}

// the hard-killed peer: a client in its own process, ended by TerminateProcess
// with no disconnect packet. A job object ends it with this process if the
// case fails first.

struct windows_connreset_child_t
{
    HANDLE job;
    HANDLE process;
};

// after_create, when set, runs the moment CreateProcessW returns: the
// child-dies-with-its-parent case ends the parent there
static void windows_connreset_child_start( windows_connreset_child_t * child, const char * server_address, void ( *after_create )( DWORD child_id ) = NULL )
{
    child->job = CreateJobObjectW( NULL, NULL );
    windows_connreset_require( child->job != NULL );
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset( &limits, 0, sizeof( limits ) );
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    windows_connreset_require( SetInformationJobObject( child->job, JobObjectExtendedLimitInformation, &limits, sizeof( limits ) ) );

    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW( NULL, path, MAX_PATH );
    windows_connreset_require( length > 0 && length < MAX_PATH );
    wchar_t command[MAX_PATH + 64];
    _snwprintf_s( command, MAX_PATH + 64, _TRUNCATE, L"\"%s\" --run test_windows_connreset_child_client", path );

    // the child is created inside the job (PROC_THREAD_ATTRIBUTE_JOB_LIST), so
    // there is no moment it exists outside it: created suspended and assigned
    // afterwards, a parent ended in between left it suspended forever
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList( NULL, 1, 0, &attribute_bytes );
    windows_connreset_require( attribute_bytes > 0 && attribute_bytes <= 256 );
    alignas( 16 ) uint8_t attribute_storage[256];
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) attribute_storage;
    windows_connreset_require( InitializeProcThreadAttributeList( attributes, 1, 0, &attribute_bytes ) );
    windows_connreset_require( UpdateProcThreadAttribute( attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &child->job, sizeof( child->job ), NULL, NULL ) );

    windows_connreset_require( _putenv_s( WINDOWS_CONNRESET_CHILD_ENV, server_address ) == 0 );
    STARTUPINFOEXW startup;
    memset( &startup, 0, sizeof( startup ) );
    startup.StartupInfo.cb = sizeof( startup );
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info;
    memset( &info, 0, sizeof( info ) );
    const BOOL created = CreateProcessW( path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &info );
    _putenv_s( WINDOWS_CONNRESET_CHILD_ENV, "" );
    DeleteProcThreadAttributeList( attributes );
    windows_connreset_require( created );
    if ( after_create )
        after_create( info.dwProcessId );
    CloseHandle( info.hThread );
    child->process = info.hProcess;
}

static void windows_connreset_child_kill( windows_connreset_child_t * child )
{
    windows_connreset_require( TerminateProcess( child->process, 1 ) );
    windows_connreset_require( WaitForSingleObject( child->process, 5000 ) == WAIT_OBJECT_0 );
}

static void windows_connreset_child_close( windows_connreset_child_t * child )
{
    CloseHandle( child->process );
    CloseHandle( child->job );
}

// the child process: connects, then exchanges a payload every tick until the
// parent ends it. Without the parent's environment it returns at once.
void windows_connreset_child_client()
{
    char server_address[256];
    if ( !rocketnet_test_getenv( WINDOWS_CONNRESET_CHILD_ENV, server_address, sizeof( server_address ) ) || server_address[0] == '\0' )
        return;
    struct rocketnet_client_transport_t * client = windows_connreset_client( server_address );
    const double start = rocketnet_platform_time();
    uint32_t sequence = 0;
    while ( rocketnet_platform_time() - start < 60.0 )
    {
        windows_connreset_client_tick( client, rocketnet_platform_time(), sequence++, true, NULL, NULL );
        rocketnet_platform_sleep( WINDOWS_CONNRESET_TICK );
    }
    rn_native_client_transport_stop_and_destroy( client );
}

// the parent in the child-dies-with-its-parent case: starts a child client,
// prints its process id and ends itself the moment the child exists, as the
// suite runner ends a case it stops. Without the case's environment it
// returns at once.
static void windows_connreset_parent_ends( DWORD child_id )
{
    printf( "windows_connreset_child_id %lu\n", (unsigned long) child_id );
    fflush( stdout );
    TerminateProcess( GetCurrentProcess(), 0 );
}

void windows_connreset_child_parent()
{
    char server_address[256];
    if ( !rocketnet_test_getenv( WINDOWS_CONNRESET_PARENT_ENV, server_address, sizeof( server_address ) ) || server_address[0] == '\0' )
        return;
    windows_connreset_child_t child;
    windows_connreset_child_start( &child, server_address, windows_connreset_parent_ends );
}

// opens process id only while it is still this executable: an id is reused
// once its process is gone, so the image is named before acting on it
static HANDLE windows_connreset_open_own_image( DWORD id, const wchar_t * path )
{
    HANDLE process = OpenProcess( SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id );
    if ( process == NULL )
        return NULL;
    wchar_t image[MAX_PATH];
    DWORD length = MAX_PATH;
    if ( !QueryFullProcessImageNameW( process, 0, image, &length ) || _wcsicmp( image, path ) != 0 )
    {
        CloseHandle( process );
        return NULL;
    }
    return process;
}

// a parent ended before it can clean up takes its child client with it: the
// child is in the parent's kill-on-close job from the moment it exists
void windows_connreset_child_dies_with_its_parent()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW( NULL, path, MAX_PATH );
    windows_connreset_require( length > 0 && length < MAX_PATH );
    wchar_t command[MAX_PATH + 64];
    _snwprintf_s( command, MAX_PATH + 64, _TRUNCATE, L"\"%s\" --run test_windows_connreset_child_parent", path );

    SECURITY_ATTRIBUTES inherit;
    memset( &inherit, 0, sizeof( inherit ) );
    inherit.nLength = sizeof( inherit );
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    windows_connreset_require( CreatePipe( &read_end, &write_end, &inherit, 0 ) );
    windows_connreset_require( SetHandleInformation( read_end, HANDLE_FLAG_INHERIT, 0 ) );

    // a closed port on loopback: the child client never connects, it only
    // has to exist
    windows_connreset_require( _putenv_s( WINDOWS_CONNRESET_PARENT_ENV, "127.0.0.1:9" ) == 0 );
    STARTUPINFOW startup;
    memset( &startup, 0, sizeof( startup ) );
    startup.cb = sizeof( startup );
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION info;
    memset( &info, 0, sizeof( info ) );
    const BOOL created = CreateProcessW( path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &info );
    _putenv_s( WINDOWS_CONNRESET_PARENT_ENV, "" );
    CloseHandle( write_end );
    windows_connreset_require( created );
    CloseHandle( info.hThread );

    char output[4096];
    size_t used = 0;
    DWORD read = 0;
    while ( used < sizeof( output ) - 1 && ReadFile( read_end, output + used, (DWORD) ( sizeof( output ) - 1 - used ), &read, NULL ) && read > 0 )
        used += read;
    output[used] = '\0';
    CloseHandle( read_end );
    windows_connreset_require( WaitForSingleObject( info.hProcess, 10000 ) == WAIT_OBJECT_0 );
    CloseHandle( info.hProcess );

    const char * line = strstr( output, "windows_connreset_child_id " );
    windows_connreset_require( line != NULL );
    const DWORD child_id = (DWORD) strtoul( line + strlen( "windows_connreset_child_id " ), NULL, 10 );
    windows_connreset_require( child_id != 0 );

    HANDLE child = windows_connreset_open_own_image( child_id, path );
    if ( child == NULL )
        return;
    const DWORD waited = WaitForSingleObject( child, 5000 );
    if ( waited != WAIT_OBJECT_0 )
        TerminateProcess( child, 1 );
    CloseHandle( child );
    windows_connreset_require( waited == WAIT_OBJECT_0 );
}

// one server, three live clients in this process and one client in a child
// process, all exchanging a payload every tick

struct windows_connreset_session_t
{
    struct rocketnet_server_transport_t * server;
    struct rocketnet_client_transport_t * live[WINDOWS_CONNRESET_LIVE_CLIENTS];
    int live_index[WINDOWS_CONNRESET_LIVE_CLIENTS];
    double last_receive[WINDOWS_CONNRESET_LIVE_CLIENTS];
    uint64_t client_sent[WINDOWS_CONNRESET_LIVE_CLIENTS];
    uint64_t client_received[WINDOWS_CONNRESET_LIVE_CLIENTS];
    uint64_t server_received[WINDOWS_CONNRESET_LIVE_CLIENTS];
    uint64_t server_sent[WINDOWS_CONNRESET_LIVE_CLIENTS];
    bool measuring;
    double max_gap;
    int child_index;
    uint32_t sequence;
    windows_connreset_child_t child;
};

static int windows_connreset_live_slot( windows_connreset_session_t * session, int client_index )
{
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
        if ( session->live_index[i] == client_index )
            return i;
    return -1;
}

// one server tick: update every client, update the server, read every
// payload (recording each live client's receive gap), send sends_per_client
// payloads to every connected client (the dead one too, until it times out),
// and extra_dead_sends more to the dead client's slot
static void windows_connreset_tick( windows_connreset_session_t * session, int sends_per_client, int extra_dead_sends )
{
    const double now = rocketnet_platform_time();
    session->sequence++;
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
        windows_connreset_client_tick( session->live[i], now, session->sequence, true,
                                       &session->client_sent[i], &session->client_received[i] );
    rocketnet_server_transport_update( session->server, now );
    const int num_payloads = rocketnet_server_transport_num_payloads( session->server );
    for ( int i = 0; i < num_payloads; i++ )
    {
        int client_index = -1;
        int payload_bytes = 0;
        rocketnet_server_transport_payload( session->server, i, &client_index, &payload_bytes );
        const int slot = windows_connreset_live_slot( session, client_index );
        if ( slot < 0 )
            continue;
        session->server_received[slot]++;
        if ( session->measuring )
        {
            const double gap = now - session->last_receive[slot];
            if ( gap > session->max_gap )
                session->max_gap = gap;
        }
        session->last_receive[slot] = now;
    }
    uint8_t payload[WINDOWS_CONNRESET_PAYLOAD_BYTES];
    memset( payload, 0x5a, sizeof( payload ) );
    for ( int client_index = 0; client_index < 8; client_index++ )
    {
        if ( !rocketnet_server_transport_is_client_connected( session->server, client_index ) )
            continue;
        const int slot = windows_connreset_live_slot( session, client_index );
        for ( int j = 0; j < sends_per_client; j++ )
        {
            rocketnet_server_transport_send_packet( session->server, client_index, payload, (int) sizeof( payload ) );
            if ( slot >= 0 )
                session->server_sent[slot]++;
        }
        if ( client_index == session->child_index )
            for ( int j = 0; j < extra_dead_sends; j++ )
                rocketnet_server_transport_send_packet( session->server, client_index, payload, (int) sizeof( payload ) );
    }
    rocketnet_server_transport_send_packets( session->server );
}

static void windows_connreset_sleep_to( double next )
{
    const double now = rocketnet_platform_time();
    if ( next > now )
        rocketnet_platform_sleep( next - now );
}

static void windows_connreset_session_start( windows_connreset_session_t * session, const windows_connreset_transport_t * transport )
{
    memset( session, 0, sizeof( *session ) );
    session->child_index = -1;
    session->server = windows_connreset_server( transport, 0, 8 );
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    windows_connreset_server_address( session->server, transport->ipv6, server_address, sizeof( server_address ) );

    windows_connreset_child_start( &session->child, server_address );
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
    {
        session->live[i] = windows_connreset_client( server_address );
        session->live_index[i] = -1;
    }

    // all four connect, the child's slot is the one no live client holds
    const double start = rocketnet_platform_time();
    double next = start;
    for ( ;; )
    {
        windows_connreset_tick( session, 1, 0 );
        bool connected = rocketnet_server_transport_num_connected_clients( session->server ) == WINDOWS_CONNRESET_LIVE_CLIENTS + 1;
        for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
            connected = connected && rocketnet_client_transport_state( session->live[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
        if ( connected )
            break;
        windows_connreset_require( rocketnet_platform_time() - start < 10.0 );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
        session->live_index[i] = rocketnet_client_transport_client_index( session->live[i] );
    for ( int client_index = 0; client_index < 8; client_index++ )
        if ( rocketnet_server_transport_is_client_connected( session->server, client_index ) && windows_connreset_live_slot( session, client_index ) < 0 )
            session->child_index = client_index;
    windows_connreset_require( session->child_index >= 0 );

    // one second of traffic before the measurement starts
    const double warm = rocketnet_platform_time();
    while ( rocketnet_platform_time() - warm < 1.0 )
    {
        windows_connreset_tick( session, 1, 0 );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
    const double now = rocketnet_platform_time();
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
    {
        windows_connreset_require( session->server_received[i] > 0 );
        session->last_receive[i] = now;
    }
    session->measuring = true;
}

static void windows_connreset_session_finish( windows_connreset_session_t * session )
{
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
        rn_native_client_transport_stop_and_destroy( session->live[i] );
    rn_native_server_transport_stop_and_destroy( session->server );
    windows_connreset_child_close( &session->child );
}

static const char * windows_connreset_label( const windows_connreset_transport_t * transport )
{
    if ( transport->plain )
        return transport->ipv6 ? "iocp (USO and URO off) ipv6" : "iocp (USO and URO off) ipv4";
    static char label[64];
    snprintf( label, sizeof( label ), "%s %s", transport->transport, transport->ipv6 ? "ipv6" : "ipv4" );
    return label;
}

// windows_connreset_hard_killed_client: the child ends mid-stream; the server
// keeps sending to it, so the stack answers with ICMP port unreachable. The
// three live clients see no receive gap past one update interval plus 50 ms,
// the dead client times out by the normal timeout and frees its slot, and a
// fifth client then connects within its handshake time. The server sends
// every client a snapshot of WINDOWS_CONNRESET_SNAPSHOT_PACKETS packets a tick.
static void windows_connreset_hard_killed_client( const windows_connreset_transport_t * transport )
{
    const uint64_t resets_before = rocketnet_platform_receive_resets();
    windows_connreset_session_t session;
    windows_connreset_session_start( &session, transport );

    windows_connreset_child_kill( &session.child );
    const double killed = rocketnet_platform_time();
    double next = killed;
    while ( rocketnet_server_transport_is_client_connected( session.server, session.child_index ) )
    {
        windows_connreset_tick( &session, WINDOWS_CONNRESET_SNAPSHOT_PACKETS, 0 );
        windows_connreset_require( rocketnet_platform_time() - killed < ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS + 5.0 );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
    const double timed_out = rocketnet_platform_time() - killed;
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
    {
        windows_connreset_require( rocketnet_client_transport_state( session.live[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        windows_connreset_require( rocketnet_server_transport_is_client_connected( session.server, session.live_index[i] ) );
    }
    windows_connreset_require( rocketnet_server_transport_num_connected_clients( session.server ) == WINDOWS_CONNRESET_LIVE_CLIENTS );
    printf( "windows_connreset_hard_killed_client %s: dead client timed out after %.2f s, max receive gap so far %.1f ms\n",
            windows_connreset_label( transport ), timed_out, session.max_gap * 1000.0 );
    fflush( stdout );

    // the slot is free: a fifth client connects within its handshake time
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    windows_connreset_server_address( session.server, transport->ipv6, server_address, sizeof( server_address ) );
    struct rocketnet_client_transport_t * fifth = windows_connreset_client( server_address );
    const double fifth_start = rocketnet_platform_time();
    while ( rocketnet_client_transport_state( fifth ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        windows_connreset_tick( &session, WINDOWS_CONNRESET_SNAPSHOT_PACKETS, 0 );
        windows_connreset_client_tick( fifth, rocketnet_platform_time(), session.sequence, true, NULL, NULL );
        windows_connreset_require( rocketnet_client_transport_state( fifth ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
        windows_connreset_require( rocketnet_platform_time() - fifth_start < 5.0 );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
    const double fifth_connect = rocketnet_platform_time() - fifth_start;
    const uint64_t resets = rocketnet_platform_receive_resets() - resets_before;

    printf( "windows_connreset_hard_killed_client %s: max receive gap %.1f ms (bound %.1f ms), dead client timed out after %.2f s, fifth client connected in %.3f s, reset statuses skipped %llu\n",
            windows_connreset_label( transport ), session.max_gap * 1000.0, WINDOWS_CONNRESET_GAP_BOUND * 1000.0, timed_out, fifth_connect, (unsigned long long) resets );
    fflush( stdout );

    windows_connreset_require( session.max_gap <= WINDOWS_CONNRESET_GAP_BOUND );
    windows_connreset_require( timed_out >= ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS - 1.0 );
    windows_connreset_require( timed_out <= ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS + 2.0 );
    // the reports are off on every socket: the stack delivered none
    windows_connreset_require( resets == 0 );

    rn_native_client_transport_stop_and_destroy( fifth );
    windows_connreset_session_finish( &session );
}

// no sends: every side only reads, for seconds, counting what still arrives
static void windows_connreset_drain( windows_connreset_session_t * session, double seconds )
{
    const double start = rocketnet_platform_time();
    double next = start;
    while ( rocketnet_platform_time() - start < seconds )
    {
        const double now = rocketnet_platform_time();
        for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
            windows_connreset_client_tick( session->live[i], now, 0, false, NULL, &session->client_received[i] );
        rocketnet_server_transport_update( session->server, now );
        const int num_payloads = rocketnet_server_transport_num_payloads( session->server );
        for ( int j = 0; j < num_payloads; j++ )
        {
            int client_index = -1;
            int payload_bytes = 0;
            rocketnet_server_transport_payload( session->server, j, &client_index, &payload_bytes );
            const int slot = windows_connreset_live_slot( session, client_index );
            if ( slot >= 0 )
                session->server_received[slot]++;
        }
        rocketnet_server_transport_send_packets( session->server );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
}

// windows_connreset_send_to_closed_port: with the child ended, the server
// sends 1000 datagrams to its port, which no socket holds, while the live
// clients exchange traffic: no stall, no disconnect, and every payload each
// side sent is received, exactly
static void windows_connreset_send_to_closed_port( const windows_connreset_transport_t * transport )
{
    const uint64_t resets_before = rocketnet_platform_receive_resets();
    windows_connreset_session_t session;
    windows_connreset_session_start( &session, transport );
    windows_connreset_child_kill( &session.child );

    // counts start here, on a link drained quiet: nothing sent before is
    // still in flight
    session.measuring = false;
    windows_connreset_drain( &session, 0.25 );
    session.measuring = true;
    const double restart = rocketnet_platform_time();
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
    {
        session.last_receive[i] = restart;
        session.client_sent[i] = 0;
        session.client_received[i] = 0;
        session.server_received[i] = 0;
        session.server_sent[i] = 0;
    }
    int dead_sends = 0;
    double next = rocketnet_platform_time();
    while ( dead_sends < 1000 )
    {
        // the per-tick send to every connected slot is one, plus nine extra
        windows_connreset_tick( &session, 1, 9 );
        dead_sends += 10;
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }

    // stop sending; drain until every count settles
    session.measuring = false;
    windows_connreset_drain( &session, 0.5 );
    const uint64_t resets = rocketnet_platform_receive_resets() - resets_before;

    printf( "windows_connreset_send_to_closed_port %s: %d datagrams to the closed port, max receive gap %.1f ms (bound %.1f ms), reset statuses skipped %llu\n",
            windows_connreset_label( transport ), dead_sends, session.max_gap * 1000.0, WINDOWS_CONNRESET_GAP_BOUND * 1000.0, (unsigned long long) resets );
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
        printf( "    live client %d: client sent %llu, server received %llu; server sent %llu, client received %llu\n", i,
                (unsigned long long) session.client_sent[i], (unsigned long long) session.server_received[i],
                (unsigned long long) session.server_sent[i], (unsigned long long) session.client_received[i] );
    fflush( stdout );

    windows_connreset_require( session.max_gap <= WINDOWS_CONNRESET_GAP_BOUND );
    for ( int i = 0; i < WINDOWS_CONNRESET_LIVE_CLIENTS; i++ )
    {
        windows_connreset_require( rocketnet_client_transport_state( session.live[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        windows_connreset_require( rocketnet_server_transport_is_client_connected( session.server, session.live_index[i] ) );
        windows_connreset_require( session.server_received[i] == session.client_sent[i] );
        windows_connreset_require( session.client_received[i] == session.server_sent[i] );
    }
    windows_connreset_require( resets == 0 );

    windows_connreset_session_finish( &session );
}

// windows_client_connreset_server_gone: the client sends its connection
// requests to a port that was just closed, so its own socket is answered
// with ICMP port unreachable; then a server comes up on that port and the
// client connects. The client's I/O thread never exits on the reports.
static void windows_client_connreset_server_gone( bool ipv6 )
{
    windows_connreset_clear_env();
    const uint64_t resets_before = rocketnet_platform_receive_resets();

    struct rocketnet_address_t reserve;
    windows_connreset_require( rocketnet_address_parse( &reserve, ipv6 ? "[::1]:0" : "127.0.0.1:0" ) );
    struct rocketnet_platform_socket_t * holder = rocketnet_platform_socket_create( NULL, &reserve, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 65536, 65536 );
    windows_connreset_require( holder != NULL );
    const int port = reserve.port;
    rocketnet_platform_socket_destroy( holder );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "%s:%d", windows_connreset_host( ipv6 ), port );
    struct rocketnet_client_transport_t * client = windows_connreset_client( server_address );

    // one second of connection requests into the closed port
    double next = rocketnet_platform_time();
    const double start = next;
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        windows_connreset_client_tick( client, rocketnet_platform_time(), 0, true, NULL, NULL );
        windows_connreset_require( rocketnet_client_transport_state( client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }

    windows_connreset_transport_t transport = { "portable", false, ipv6 };
    struct rocketnet_server_transport_t * server = windows_connreset_server( &transport, port, 4 );
    const double up = rocketnet_platform_time();
    uint64_t server_received = 0;
    uint64_t client_received = 0;
    uint32_t sequence = 0;
    while ( server_received < 10 || client_received < 10 )
    {
        const double now = rocketnet_platform_time();
        windows_connreset_client_tick( client, now, sequence++, true, NULL, &client_received );
        rocketnet_server_transport_update( server, now );
        server_received += (uint64_t) rocketnet_server_transport_num_payloads( server );
        if ( rocketnet_server_transport_is_client_connected( server, 0 ) )
        {
            uint8_t payload[WINDOWS_CONNRESET_PAYLOAD_BYTES];
            memset( payload, 0x3c, sizeof( payload ) );
            rocketnet_server_transport_send_packet( server, 0, payload, (int) sizeof( payload ) );
        }
        rocketnet_server_transport_send_packets( server );
        windows_connreset_require( rocketnet_client_transport_state( client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
        windows_connreset_require( now - up < 5.0 );
        next += WINDOWS_CONNRESET_TICK;
        windows_connreset_sleep_to( next );
    }
    const uint64_t resets = rocketnet_platform_receive_resets() - resets_before;
    printf( "windows_client_connreset_server_gone %s: connected and exchanged in %.3f s after the server came up, reset statuses skipped %llu\n",
            ipv6 ? "ipv6" : "ipv4", rocketnet_platform_time() - up, (unsigned long long) resets );
    fflush( stdout );
    windows_connreset_require( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    windows_connreset_require( resets == 0 );

    rn_native_client_transport_stop_and_destroy( client );
    rn_native_server_transport_stop_and_destroy( server );
}

#define WINDOWS_CONNRESET_CASES( name, function )                                                        \
    void name##_portable_ipv4()   { windows_connreset_transport_t t = { "portable", false, false }; function( &t ); } \
    void name##_portable_ipv6()   { windows_connreset_transport_t t = { "portable", false, true };  function( &t ); } \
    void name##_iocp_ipv4()       { windows_connreset_transport_t t = { "iocp", false, false };     function( &t ); } \
    void name##_iocp_ipv6()       { windows_connreset_transport_t t = { "iocp", false, true };      function( &t ); } \
    void name##_iocp_plain_ipv4() { windows_connreset_transport_t t = { "iocp", true, false };      function( &t ); } \
    void name##_iocp_plain_ipv6() { windows_connreset_transport_t t = { "iocp", true, true };       function( &t ); } \
    void name##_rio_ipv4()        { windows_connreset_transport_t t = { "rio", false, false };      function( &t ); } \
    void name##_rio_ipv6()        { windows_connreset_transport_t t = { "rio", false, true };       function( &t ); }

WINDOWS_CONNRESET_CASES( windows_connreset_hard_killed_client, windows_connreset_hard_killed_client )
WINDOWS_CONNRESET_CASES( windows_connreset_send_to_closed_port, windows_connreset_send_to_closed_port )

void windows_client_connreset_server_gone_ipv4() { windows_client_connreset_server_gone( false ); }
void windows_client_connreset_server_gone_ipv6() { windows_client_connreset_server_gone( true ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
