/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0-or-later or MBSL-1.0. See LICENSE and MBSL.md. */
#pragma once

#ifndef ROCKETNET_EXAMPLES_WHITELIST_H
#define ROCKETNET_EXAMPLES_WHITELIST_H

// The game server's side of the backend: it registers the server with the
// backend, then fetches the server's whitelist (the client ids the backend
// matched to this server and has not banned) every few seconds, on its own
// thread, and hands each new list to the game loop through two preallocated
// buffers. The frame never waits on the network: the lock below is held
// only to swap which buffer is which, never across a request.
//
// The HTTP client is the smallest that does the job: HTTP/1.1 over plain
// TCP (POSIX sockets or Winsock), one request per connection, literal IP
// addresses only, Content-Length bodies only, and one deadline for the
// whole request (connect, send and receive): a backend that answers slowly,
// or trickles its answer, fails the fetch once RN_CLI_HTTP_TIMEOUT_MS has
// gone by, and the last good list is kept. A game uses its own HTTP client, with TLS and its backend's
// authentication. Nothing here is part of rocketnet, and rocketnet calls
// none of it: the game loop in backend_server.c reads the list and
// disconnects the clients that are not on it.
//
// The whitelist on the wire is the body of GET /servers/{id}/whitelist:
// each client id as 8 bytes little endian, ascending, nothing else.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // #ifndef WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else // #ifdef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif // #ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RN_CLI_WHITELIST_MAX_ENTRIES    4096                                    // a longer list is refused, and the last good one kept
#define RN_CLI_HTTP_HEADER_BYTES        4096                                    // the most of a response that is not its body
#define RN_CLI_HTTP_RESPONSE_BYTES      ( RN_CLI_HTTP_HEADER_BYTES + RN_CLI_WHITELIST_MAX_ENTRIES * 8 )
#define RN_CLI_HTTP_REQUEST_BYTES       2048
#define RN_CLI_HTTP_TIMEOUT_MS          2000                                    // the whole request: connect, send and receive; the fetch period
#define RN_CLI_HTTP_POLL_STEP_MS        100                                     // the longest one wait on the socket, inside the deadline
#define RN_CLI_HTTP_ERROR_BYTES         256

// ---------------------------------------------------------------------------
// a lock and a sleep, from the platform
// ---------------------------------------------------------------------------

struct rn_cli_lock_t
{
#ifdef _WIN32
    CRITICAL_SECTION section;
#else // #ifdef _WIN32
    pthread_mutex_t mutex;
#endif // #ifdef _WIN32
};

static inline void rn_cli_lock_init( struct rn_cli_lock_t * lock )
{
#ifdef _WIN32
    InitializeCriticalSection( &lock->section );
#else // #ifdef _WIN32
    pthread_mutex_init( &lock->mutex, NULL );
#endif // #ifdef _WIN32
}

static inline void rn_cli_lock_destroy( struct rn_cli_lock_t * lock )
{
#ifdef _WIN32
    DeleteCriticalSection( &lock->section );
#else // #ifdef _WIN32
    pthread_mutex_destroy( &lock->mutex );
#endif // #ifdef _WIN32
}

static inline void rn_cli_lock_acquire( struct rn_cli_lock_t * lock )
{
#ifdef _WIN32
    EnterCriticalSection( &lock->section );
#else // #ifdef _WIN32
    pthread_mutex_lock( &lock->mutex );
#endif // #ifdef _WIN32
}

static inline void rn_cli_lock_release( struct rn_cli_lock_t * lock )
{
#ifdef _WIN32
    LeaveCriticalSection( &lock->section );
#else // #ifdef _WIN32
    pthread_mutex_unlock( &lock->mutex );
#endif // #ifdef _WIN32
}

static inline void rn_cli_sleep_ms( int milliseconds )
{
#ifdef _WIN32
    Sleep( (DWORD) milliseconds );
#else // #ifdef _WIN32
    (void) poll( NULL, 0, milliseconds );
#endif // #ifdef _WIN32
}

// ---------------------------------------------------------------------------
// the HTTP/1.1 client
// ---------------------------------------------------------------------------

#ifdef _WIN32
typedef SOCKET rn_cli_socket_t;
#define RN_CLI_NO_SOCKET INVALID_SOCKET
#else // #ifdef _WIN32
typedef int rn_cli_socket_t;
#define RN_CLI_NO_SOCKET -1
#endif // #ifdef _WIN32

// Winsock needs a start before its first use and an end after its last; on
// POSIX these do nothing. Returns 1 on success.
static inline int rn_cli_http_init( void )
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup( MAKEWORD( 2, 2 ), &data ) == 0;
#else // #ifdef _WIN32
    return 1;
#endif // #ifdef _WIN32
}

static inline void rn_cli_http_term( void )
{
#ifdef _WIN32
    WSACleanup();
#endif // #ifdef _WIN32
}

static inline void rn_cli_http_close( rn_cli_socket_t s )
{
#ifdef _WIN32
    closesocket( s );
#else // #ifdef _WIN32
    close( s );
#endif // #ifdef _WIN32
}

// a monotonic clock in milliseconds, for the request's deadline
static inline int64_t rn_cli_http_now_ms( void )
{
#ifdef _WIN32
    return (int64_t) GetTickCount64();
#else // #ifdef _WIN32
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif // #ifdef _WIN32
}

// waits until deadline_ms (rn_cli_http_now_ms) at the latest for s to be
// readable (or writable), in polls of at most RN_CLI_HTTP_POLL_STEP_MS;
// returns 1 when it is, 0 once the deadline has passed or the poll fails
static inline int rn_cli_http_wait( rn_cli_socket_t s, int for_write, int64_t deadline_ms )
{
    for ( ;; )
    {
        const int64_t remaining = deadline_ms - rn_cli_http_now_ms();
        if ( remaining <= 0 )
        {
            return 0;
        }
        const int wait_ms = (int) ( remaining < RN_CLI_HTTP_POLL_STEP_MS ? remaining : RN_CLI_HTTP_POLL_STEP_MS );
#ifdef _WIN32
        WSAPOLLFD poll_fd;
        poll_fd.fd = s;
        poll_fd.events = for_write ? POLLWRNORM : POLLRDNORM;
        poll_fd.revents = 0;
        const int result = WSAPoll( &poll_fd, 1, wait_ms );
        if ( result < 0 )
        {
            return 0;
        }
#else // #ifdef _WIN32
        struct pollfd poll_fd;
        poll_fd.fd = s;
        poll_fd.events = for_write ? POLLOUT : POLLIN;
        poll_fd.revents = 0;
        const int result = poll( &poll_fd, 1, wait_ms );
        if ( result < 0 && errno != EINTR )
        {
            return 0;
        }
#endif // #ifdef _WIN32
        if ( result == 1 )
        {
            return 1;
        }
    }
}

// connects to a literal "a.b.c.d:port" or "[ipv6]:port" by deadline_ms,
// and leaves the socket non-blocking
static inline rn_cli_socket_t rn_cli_http_connect( const char * address, int64_t deadline_ms, char * error, size_t error_bytes )
{
    char host[64];
    const char * colon = strrchr( address, ':' );
    size_t host_bytes = colon ? (size_t) ( colon - address ) : 0;
    const char * host_start = address;
    if ( host_bytes >= 2 && address[0] == '[' && address[host_bytes - 1] == ']' )
    {
        host_start++;
        host_bytes -= 2;
    }
    if ( !colon || host_bytes == 0 || host_bytes >= sizeof( host ) || colon[1] == '\0' )
    {
        snprintf( error, error_bytes, "backend address \"%s\" is not ip:port", address );
        return RN_CLI_NO_SOCKET;
    }
    memcpy( host, host_start, host_bytes );
    host[host_bytes] = '\0';

    struct addrinfo hints;
    memset( &hints, 0, sizeof( hints ) );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    struct addrinfo * found = NULL;
    if ( getaddrinfo( host, colon + 1, &hints, &found ) != 0 || !found )
    {
        snprintf( error, error_bytes, "backend address \"%s\" is not a literal ip and port", address );
        return RN_CLI_NO_SOCKET;
    }

    rn_cli_socket_t s = socket( found->ai_family, found->ai_socktype, found->ai_protocol );
    if ( s == RN_CLI_NO_SOCKET )
    {
        freeaddrinfo( found );
        snprintf( error, error_bytes, "could not create a socket" );
        return RN_CLI_NO_SOCKET;
    }

#ifdef _WIN32
    u_long non_blocking = 1;
    const int set_non_blocking = ioctlsocket( s, FIONBIO, &non_blocking ) == 0;
    const int connect_result = set_non_blocking ? connect( s, found->ai_addr, (int) found->ai_addrlen ) : -1;
    const int in_progress = connect_result != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else // #ifdef _WIN32
    const int flags = fcntl( s, F_GETFL, 0 );
    const int set_non_blocking = flags >= 0 && fcntl( s, F_SETFL, flags | O_NONBLOCK ) == 0;
    const int connect_result = set_non_blocking ? connect( s, found->ai_addr, found->ai_addrlen ) : -1;
    const int in_progress = connect_result != 0 && errno == EINPROGRESS;
#endif // #ifdef _WIN32
    freeaddrinfo( found );

    if ( connect_result != 0 )
    {
        int socket_error = 0;
#ifdef _WIN32
        int length = (int) sizeof( socket_error );
        const int connected = in_progress && rn_cli_http_wait( s, 1, deadline_ms ) && getsockopt( s, SOL_SOCKET, SO_ERROR, (char *) &socket_error, &length ) == 0 && socket_error == 0;
#else // #ifdef _WIN32
        socklen_t length = (socklen_t) sizeof( socket_error );
        const int connected = in_progress && rn_cli_http_wait( s, 1, deadline_ms ) && getsockopt( s, SOL_SOCKET, SO_ERROR, &socket_error, &length ) == 0 && socket_error == 0;
#endif // #ifdef _WIN32
        if ( !connected )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "could not connect to the backend at %s", address );
            return RN_CLI_NO_SOCKET;
        }
    }

    return s;
}

static inline int rn_cli_http_lower( int c )
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// finds a header's value in [headers, end), name matched without case;
// returns NULL when it is not there
static inline const char * rn_cli_http_header( const char * headers, const char * end, const char * name )
{
    const size_t name_bytes = strlen( name );
    for ( const char * line = headers; line < end; )
    {
        const char * line_end = line;
        while ( line_end < end && *line_end != '\r' )
        {
            line_end++;
        }
        if ( (size_t) ( line_end - line ) > name_bytes && line[name_bytes] == ':' )
        {
            size_t i = 0;
            while ( i < name_bytes && rn_cli_http_lower( (unsigned char) line[i] ) == rn_cli_http_lower( (unsigned char) name[i] ) )
            {
                i++;
            }
            if ( i == name_bytes )
            {
                const char * value = line + name_bytes + 1;
                while ( value < line_end && ( *value == ' ' || *value == '\t' ) )
                {
                    value++;
                }
                return value;
            }
        }
        line = line_end + 2;
    }
    return NULL;
}

// This client reads bodies with one decimal Content-Length and no transfer coding.
static inline int rn_cli_http_length_matches( const char * headers, const char * end, int body_bytes )
{
    const char * p = rn_cli_http_header( headers, end, "Content-Length" );
    if ( !p || *p < '0' || *p > '9' || rn_cli_http_header( headers, end, "Transfer-Encoding" ) )
    {
        return 0;
    }

    int64_t length = 0;
    while ( *p >= '0' && *p <= '9' )
    {
        length = length * 10 + ( *p - '0' );
        if ( length > body_bytes )
        {
            return 0;
        }
        p++;
    }
    while ( *p == ' ' || *p == '\t' )
    {
        p++;
    }
    return length == body_bytes && end - p >= 2 && p[0] == '\r' && p[1] == '\n' &&
           !rn_cli_http_header( p + 2, end, "Content-Length" );
}

// One request. The response is read into response (response_bytes long,
// preallocated) and *body points into it at the body of *body_bytes.
// Returns the HTTP status, or 0 for any failure, with the cause in error.
// The whole request, connect to close, gets RN_CLI_HTTP_TIMEOUT_MS.
static inline int rn_cli_http_request( const char * address, const char * method, const char * path, const char * content_type,
                                       const char * body, int body_bytes, char * response, int response_bytes,
                                       const char ** response_body, int * response_body_bytes, char * error, size_t error_bytes )
{
    const int64_t deadline_ms = rn_cli_http_now_ms() + RN_CLI_HTTP_TIMEOUT_MS;
    *response_body = NULL;
    *response_body_bytes = 0;

    char request[RN_CLI_HTTP_REQUEST_BYTES];
    const int header_bytes = snprintf( request, sizeof( request ),
                                       "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %d\r\n%s%s%s\r\n",
                                       method, path, address, body_bytes,
                                       content_type ? "Content-Type: " : "", content_type ? content_type : "", content_type ? "\r\n" : "" );
    if ( header_bytes <= 0 || header_bytes + body_bytes > (int) sizeof( request ) )
    {
        snprintf( error, error_bytes, "%s %s: the request does not fit in %d bytes", method, path, RN_CLI_HTTP_REQUEST_BYTES );
        return 0;
    }
    if ( body_bytes > 0 )
    {
        memcpy( request + header_bytes, body, (size_t) body_bytes );
    }
    const int request_bytes = header_bytes + body_bytes;

    rn_cli_socket_t s = rn_cli_http_connect( address, deadline_ms, error, error_bytes );
    if ( s == RN_CLI_NO_SOCKET )
    {
        return 0;
    }

    for ( int sent = 0; sent < request_bytes; )
    {
        if ( !rn_cli_http_wait( s, 1, deadline_ms ) )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "%s %s: timed out sending", method, path );
            return 0;
        }
        // A backend restart during the write must fail this fetch and leave the game running.
#ifdef _WIN32
        const int n = send( s, request + sent, request_bytes - sent, 0 );
#else // #ifdef _WIN32
        const int n = (int) send( s, request + sent, (size_t) ( request_bytes - sent ), MSG_NOSIGNAL );
#endif // #ifdef _WIN32
        if ( n <= 0 )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "%s %s: the backend closed the connection while sending", method, path );
            return 0;
        }
        sent += n;
    }

    // read to the close; keep one byte free so a response that does not fit is seen
    int received = 0;
    for ( ;; )
    {
        if ( received == response_bytes )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "%s %s: the response is over %d bytes", method, path, response_bytes );
            return 0;
        }
        if ( !rn_cli_http_wait( s, 0, deadline_ms ) )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "%s %s: timed out receiving", method, path );
            return 0;
        }
#ifdef _WIN32
        const int n = recv( s, response + received, response_bytes - received, 0 );
#else // #ifdef _WIN32
        const int n = (int) recv( s, response + received, (size_t) ( response_bytes - received ), 0 );
#endif // #ifdef _WIN32
        if ( n < 0 )
        {
            rn_cli_http_close( s );
            snprintf( error, error_bytes, "%s %s: the connection failed while receiving", method, path );
            return 0;
        }
        if ( n == 0 )
        {
            break;
        }
        received += n;
    }
    rn_cli_http_close( s );

    // "HTTP/1.1 200 OK\r\n", headers, a blank line, then exactly Content-Length bytes
    const char * end = response + received;
    const char * headers_end = NULL;
    for ( const char * p = response; end - p >= 4; p++ )
    {
        if ( memcmp( p, "\r\n\r\n", 4 ) == 0 )
        {
            headers_end = p + 2;
            break;
        }
    }
    if ( !headers_end || received < 12 || memcmp( response, "HTTP/1.", 7 ) != 0 || response[8] != ' ' ||
         response[9] < '1' || response[9] > '5' || response[10] < '0' || response[10] > '9' || response[11] < '0' || response[11] > '9' )
    {
        snprintf( error, error_bytes, "%s %s: the response is not HTTP/1.x", method, path );
        return 0;
    }
    const int status = ( response[9] - '0' ) * 100 + ( response[10] - '0' ) * 10 + ( response[11] - '0' );

    // 1xx, 204 and 304 have no body, and need no Content-Length
    const char * body_start = headers_end + 2;
    if ( status < 200 || status == 204 || status == 304 )
    {
        if ( end != body_start )
        {
            snprintf( error, error_bytes, "%s %s: a status %d response has a body", method, path, status );
            return 0;
        }
        *response_body = body_start;
        return status;
    }

    const int received_body_bytes = (int) ( end - body_start );
    if ( !rn_cli_http_length_matches( response, headers_end, received_body_bytes ) )
    {
        snprintf( error, error_bytes, "%s %s: expected one Content-Length matching the %d byte body", method, path, received_body_bytes );
        return 0;
    }

    *response_body = body_start;
    *response_body_bytes = received_body_bytes;
    return status;
}

// ---------------------------------------------------------------------------
// the whitelist fetch: a thread, and two buffers handed back and forth
// ---------------------------------------------------------------------------

struct rn_cli_whitelist_t
{
    int count;
    double requested_at;        // the game's time when the main thread asked for this list
    uint64_t client_id[RN_CLI_WHITELIST_MAX_ENTRIES];      // ascending
};

struct rn_cli_whitelist_fetch_t
{
    // set before the thread starts, then read only
    char backend_address[64];
    char server_id[64];
    char registration[512];     // the PUT /servers/{id} body

    // guarded by lock: the main thread's request, and which buffer is which
    struct rn_cli_lock_t lock;
    int stop;
    int requested;              // the main thread asked for a fetch
    double request_time;        // and when, in the game's time
    int in_use;                 // the buffer the main thread reads; the thread never writes it
    int published;              // the newest whole list
    int fresh;                  // published holds a list the main thread has not taken yet

    struct rn_cli_whitelist_t lists[2];

    // the thread's own
    int registered;
    char response[RN_CLI_HTTP_RESPONSE_BYTES];
    char error[RN_CLI_HTTP_ERROR_BYTES];
};

static inline int rn_cli_whitelist_compare( const void * a, const void * b )
{
    const uint64_t x = *(const uint64_t *) a;
    const uint64_t y = *(const uint64_t *) b;
    return x < y ? -1 : ( x > y ? 1 : 0 );
}

// 1 when id is on list (ascending)
static inline int rn_cli_whitelist_contains( const struct rn_cli_whitelist_t * list, uint64_t id )
{
    int low = 0, high = list->count - 1;
    while ( low <= high )
    {
        const int middle = low + ( high - low ) / 2;
        if ( list->client_id[middle] == id )
        {
            return 1;
        }
        if ( list->client_id[middle] < id )
        {
            low = middle + 1;
        }
        else
        {
            high = middle - 1;
        }
    }
    return 0;
}

// registers the server; 1 on 204
static inline int rn_cli_whitelist_register( struct rn_cli_whitelist_fetch_t * fetch )
{
    char path[128];
    snprintf( path, sizeof( path ), "/servers/%s", fetch->server_id );
    const char * body = NULL;
    int body_bytes = 0;
    const int status = rn_cli_http_request( fetch->backend_address, "PUT", path, "application/json", fetch->registration, (int) strlen( fetch->registration ),
                                            fetch->response, (int) sizeof( fetch->response ), &body, &body_bytes, fetch->error, sizeof( fetch->error ) );
    if ( status != 204 )
    {
        if ( status != 0 )
        {
            snprintf( fetch->error, sizeof( fetch->error ), "PUT %s: status %d", path, status );
        }
        printf( "whitelist: could not register with the backend: %s\n", fetch->error );
        fflush( stdout );
        return 0;
    }
    printf( "whitelist: registered as %s with the backend at %s\n", fetch->server_id, fetch->backend_address );
    fflush( stdout );
    return 1;
}

// one fetch, asked for at request_time. A failure of any kind keeps the
// last good list: the main thread goes on with what it has, and a client
// not on it is still not allowed
static inline void rn_cli_whitelist_fetch_once( struct rn_cli_whitelist_fetch_t * fetch, double request_time )
{
    if ( !fetch->registered )
    {
        fetch->registered = rn_cli_whitelist_register( fetch );
        if ( !fetch->registered )
        {
            return;
        }
    }

    char path[128];
    snprintf( path, sizeof( path ), "/servers/%s/whitelist", fetch->server_id );
    const char * body = NULL;
    int body_bytes = 0;
    const int status = rn_cli_http_request( fetch->backend_address, "GET", path, NULL, NULL, 0,
                                            fetch->response, (int) sizeof( fetch->response ), &body, &body_bytes, fetch->error, sizeof( fetch->error ) );
    if ( status == 404 )
    {
        // the backend lost this server (it restarted): register again next time
        printf( "whitelist: the backend does not know this server; registering again\n" );
        fflush( stdout );
        fetch->registered = 0;
        return;
    }
    if ( status != 200 || body_bytes % 8 != 0 || body_bytes / 8 > RN_CLI_WHITELIST_MAX_ENTRIES )
    {
        if ( status == 200 )
        {
            snprintf( fetch->error, sizeof( fetch->error ), "GET %s: a %d byte body is not a list of at most %d client ids", path, body_bytes, RN_CLI_WHITELIST_MAX_ENTRIES );
        }
        else if ( status != 0 )
        {
            snprintf( fetch->error, sizeof( fetch->error ), "GET %s: status %d", path, status );
        }
        printf( "whitelist: fetch failed, keeping the last good list: %s\n", fetch->error );
        fflush( stdout );
        return;
    }

    // write the buffer the main thread is not reading; if it held the
    // published list, that list is withdrawn until this one replaces it
    rn_cli_lock_acquire( &fetch->lock );
    const int write = 1 - fetch->in_use;
    if ( fetch->published == write )
    {
        fetch->fresh = 0;
    }
    rn_cli_lock_release( &fetch->lock );

    struct rn_cli_whitelist_t * list = &fetch->lists[write];
    list->count = body_bytes / 8;
    list->requested_at = request_time;
    for ( int i = 0; i < list->count; i++ )
    {
        const uint8_t * p = (const uint8_t *) body + i * 8;
        uint64_t id = 0;
        for ( int j = 7; j >= 0; j-- )
        {
            id = ( id << 8 ) | p[j];
        }
        list->client_id[i] = id;
    }
    qsort( list->client_id, (size_t) list->count, sizeof( uint64_t ), rn_cli_whitelist_compare );

    rn_cli_lock_acquire( &fetch->lock );
    fetch->published = write;
    fetch->fresh = 1;
    rn_cli_lock_release( &fetch->lock );
}

static inline void rn_cli_whitelist_thread( void * data )
{
    struct rn_cli_whitelist_fetch_t * fetch = (struct rn_cli_whitelist_fetch_t *) data;
    for ( ;; )
    {
        rn_cli_lock_acquire( &fetch->lock );
        const int stop = fetch->stop;
        const int requested = fetch->requested;
        const double request_time = fetch->request_time;
        fetch->requested = 0;
        rn_cli_lock_release( &fetch->lock );

        if ( stop )
        {
            return;
        }
        if ( requested )
        {
            rn_cli_whitelist_fetch_once( fetch, request_time );
        }
        else
        {
            rn_cli_sleep_ms( 5 );
        }
    }
}

// The main thread's side, once a frame: asks for a fetch, and takes the
// newest list when one has arrived. Returns the list to judge clients by,
// or NULL before the first list arrives. Never waits on the network.
static inline const struct rn_cli_whitelist_t * rn_cli_whitelist_frame( struct rn_cli_whitelist_fetch_t * fetch, double now, int ask, int * have_list )
{
    rn_cli_lock_acquire( &fetch->lock );
    if ( fetch->fresh )
    {
        fetch->in_use = fetch->published;
        fetch->fresh = 0;
        *have_list = 1;
    }
    if ( ask )
    {
        fetch->requested = 1;
        fetch->request_time = now;
    }
    const int in_use = fetch->in_use;
    rn_cli_lock_release( &fetch->lock );
    return *have_list ? &fetch->lists[in_use] : NULL;
}

#endif // ROCKETNET_EXAMPLES_WHITELIST_H
