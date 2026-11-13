/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A server bound to a specific local address (bind_ipv4_address) is not
// reached on another address of the same machine. Linux routes all of
// 127.0.0.0/8 to the loopback interface, so 127.0.0.2 is a second local
// address. A raw socket sends a 100-byte datagram to 127.0.0.2 on the
// server's port, then a 7-byte one to 127.0.0.1. Loopback delivers one
// socket's datagrams in order, so once the server has counted the near one
// it has counted the far one too, if it reached it. Bound to 127.0.0.1 it
// counts one datagram; the control, the wildcard bind of an unset field,
// counts two. The byte totals carry the transport's per-datagram overhead
// (the IP and UDP headers), the same for both datagrams, so the control's
// extra bytes are the far datagram's 100 plus that overhead.
//
// Real sockets and real time: a functional test.

#include "rocketnet.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define bind_check( condition )                                                                       \
    do                                                                                                \
    {                                                                                                 \
        if ( !( condition ) )                                                                         \
        {                                                                                             \
            printf( "check failed: ( %s ), file %s, line %d\n", #condition, __FILE__, __LINE__ );    \
            fflush( stdout );                                                                         \
            exit( 1 );                                                                                \
        }                                                                                             \
    } while ( 0 )

#define BIND_FAR_BYTES  100
#define BIND_NEAR_BYTES 7

static void bind_send( int s, const char * ip, uint16_t port, int bytes )
{
    struct sockaddr_in to;
    memset( &to, 0, sizeof( to ) );
    to.sin_family = AF_INET;
    to.sin_port = htons( port );
    bind_check( inet_pton( AF_INET, ip, &to.sin_addr ) == 1 );
    uint8_t buffer[BIND_FAR_BYTES];
    memset( buffer, 0x5a, sizeof( buffer ) );
    bind_check( sendto( s, buffer, (size_t) bytes, 0, (const struct sockaddr *) &to, sizeof( to ) ) == bytes );
}

struct bind_counted_t
{
    uint64_t packets;
    uint64_t bytes;
};

// what the server counts from one far datagram then one near one, read once
// it has counted at least target_packets
static bind_counted_t bind_counted( const char * bind_ipv4_address, uint64_t target_packets )
{
    struct rocketnet_server_config_t config;
    bind_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    bind_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    memcpy( config.backend_public_key, backend_public, sizeof( config.backend_public_key ) );
    config.protocol_id = 0x62696e6461646472ULL;
    config.max_clients = 1;
    config.packet_filter[0] = 0x62;
    config.packet_filter[1] = 0x69;
    config.packet_filter[2] = 0x6e;
    config.packet_filter[3] = 0x64;
    config.transport = "portable";
    config.bind_ipv4_address = bind_ipv4_address;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    bind_check( server != NULL && status == ROCKETNET_STATUS_OK );
    bind_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    const uint16_t port = rocketnet_server_port( server );
    bind_check( port != 0 );

    struct rocketnet_server_stats_t before;
    rocketnet_server_stats( server, &before );

    const int s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    bind_check( s >= 0 );
    bind_send( s, "127.0.0.2", port, BIND_FAR_BYTES );
    bind_send( s, "127.0.0.1", port, BIND_NEAR_BYTES );

    double time = 1000.0;
    const double deadline = rocketnet_platform_time() + 5.0;
    bind_counted_t counted = { 0, 0 };
    while ( counted.packets < target_packets )
    {
        bind_check( rocketnet_platform_time() < deadline );
        time += 0.01;
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
        struct rocketnet_server_stats_t now;
        rocketnet_server_stats( server, &now );
        counted.packets = now.packets_received - before.packets_received;
        counted.bytes = now.bytes_received - before.bytes_received;
        if ( counted.packets < target_packets )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }

    close( s );
    rocketnet_server_stop( server );
    while ( !rocketnet_server_stopped( server ) )
    {
        time += 0.01;
        rocketnet_server_update( server, time );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_destroy( server );
    return counted;
}

void rocketnet_test_server_bind_address_not_reached_on_another_address()
{
    const bind_counted_t wildcard = bind_counted( NULL, 2 );
    const bind_counted_t bound = bind_counted( "127.0.0.1", 1 );
    printf( "  counted: wildcard %llu datagrams %llu bytes, bound to 127.0.0.1 %llu datagrams %llu bytes\n", (unsigned long long) wildcard.packets,
            (unsigned long long) wildcard.bytes, (unsigned long long) bound.packets, (unsigned long long) bound.bytes );
    fflush( stdout );

    // the control shows the far datagram does reach a wildcard server
    bind_check( wildcard.packets == 2 );
    bind_check( bound.packets == 1 );
    bind_check( bound.bytes >= BIND_NEAR_BYTES );
    const uint64_t overhead = bound.bytes - BIND_NEAR_BYTES;
    bind_check( wildcard.bytes == BIND_FAR_BYTES + BIND_NEAR_BYTES + 2 * overhead );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define bind_check( condition )                                                                       \
    do                                                                                                \
    {                                                                                                 \
        if ( !( condition ) )                                                                         \
        {                                                                                             \
            printf( "check failed: ( %s ), file %s, line %d\n", #condition, __FILE__, __LINE__ );    \
            fflush( stdout );                                                                         \
            exit( 1 );                                                                                \
        }                                                                                             \
    } while ( 0 )
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX

// A start whose bind the platform refuses returns
// ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED, per transport: the port is held by
// another socket (EADDRINUSE), or the bind address is not an address of this
// host (EADDRNOTAVAIL). A control start on a free port succeeds, so the
// status is the bind's and not the transport's absence.
// Real sockets: a functional test.

static enum rocketnet_status_t bind_refused_start( const char * transport, const char * public_address, const char * bind_ipv4_address )
{
    struct rocketnet_server_config_t config;
    bind_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    bind_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    memcpy( config.backend_public_key, backend_public, sizeof( config.backend_public_key ) );
    config.protocol_id = 0x62696e6472666564ULL;
    config.max_clients = 1;
    config.packet_filter[0] = 0x62;
    config.packet_filter[1] = 0x69;
    config.packet_filter[2] = 0x6e;
    config.packet_filter[3] = 0x65;
    config.transport = transport;
    config.bind_ipv4_address = bind_ipv4_address;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, public_address[0] == '[' ? NULL : public_address, public_address[0] == '[' ? public_address : NULL, &config, &status );
    bind_check( server != NULL && status == ROCKETNET_STATUS_OK );
    const enum rocketnet_status_t start_status = rocketnet_server_start( server );
    if ( start_status == ROCKETNET_STATUS_OK )
    {
        double time = 1000.0;
        rocketnet_server_stop( server );
        while ( !rocketnet_server_stopped( server ) )
        {
            time += 0.01;
            rocketnet_server_update( server, time );
            rocketnet_platform_sleep( 0.001 );
        }
    }
    rocketnet_server_destroy( server );
    return start_status;
}

// An IPv6 server on a held port: one dual-stack holder owns the IPv6 and the
// IPv4 wildcard of the port, so the server's bind (on macOS its IPv4 port
// reservation first) is refused. Skipped where the host has no IPv6.
static void bind_refused_ipv6_for( const char * transport )
{
    const int holder = socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( holder < 0 )
    {
        return;
    }
    const int dual = 0;
    setsockopt( holder, IPPROTO_IPV6, IPV6_V6ONLY, &dual, sizeof( dual ) );
    struct sockaddr_in6 held;
    memset( &held, 0, sizeof( held ) );
    held.sin6_family = AF_INET6;
    held.sin6_addr = in6addr_any;
    if ( bind( holder, (const struct sockaddr *) &held, sizeof( held ) ) != 0 )
    {
        close( holder );
        return;
    }
    socklen_t held_bytes = sizeof( held );
    bind_check( getsockname( holder, (struct sockaddr *) &held, &held_bytes ) == 0 );
    char held_address[64];
    snprintf( held_address, sizeof( held_address ), "[::1]:%u", (unsigned) ntohs( held.sin6_port ) );

    bind_check( bind_refused_start( transport, held_address, NULL ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );

    // the control: the same transport starts on a port the kernel picks at the
    // start (port 0), so no port is released and taken again. A freed port
    // could be taken by another process before the start binds it.
    close( holder );
    bind_check( bind_refused_start( transport, "[::1]:0", NULL ) == ROCKETNET_STATUS_OK );
}

static void bind_refused_for( const char * transport )
{
    // a plain socket holds a port, without SO_REUSEPORT
    const int holder = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    bind_check( holder >= 0 );
    struct sockaddr_in held;
    memset( &held, 0, sizeof( held ) );
    held.sin_family = AF_INET;
    bind_check( inet_pton( AF_INET, "127.0.0.1", &held.sin_addr ) == 1 );
    bind_check( bind( holder, (const struct sockaddr *) &held, sizeof( held ) ) == 0 );
    socklen_t held_bytes = sizeof( held );
    bind_check( getsockname( holder, (struct sockaddr *) &held, &held_bytes ) == 0 );
    const unsigned port = ntohs( held.sin_port );
    char held_address[64];
    snprintf( held_address, sizeof( held_address ), "127.0.0.1:%u", port );

    // the held port: refused
    bind_check( bind_refused_start( transport, held_address, NULL ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );

    // the holder gone: a bind address that is not this host's is refused
    // (EADDRNOTAVAIL), on a port nothing holds
    close( holder );
    bind_check( bind_refused_start( transport, held_address, "192.0.2.1" ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );

    // the control: the same transport starts on a port the kernel picks at the
    // start (port 0). A port released by the holder is not rebound: another
    // process could take it between the close and the start.
    bind_check( bind_refused_start( transport, "127.0.0.1:0", NULL ) == ROCKETNET_STATUS_OK );
}

void rocketnet_test_server_bind_refused_portable()
{
    bind_refused_for( "portable" );
    bind_refused_ipv6_for( "portable" );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

// msgx's shared socket binds with neither SO_REUSEADDR nor SO_REUSEPORT and
// sets them after the bind, so a port another process holds is refused like
// every transport's; its client sockets still join the port. With the options
// set before the bind, macOS let it succeed and the more specific socket took
// the other's datagrams (the parallel functional suite lost packets to it).
void rocketnet_test_server_bind_refused_msgx()
{
    bind_refused_for( "msgx" );
    bind_refused_ipv6_for( "msgx" );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

void rocketnet_test_server_bind_refused_gso()
{
    bind_refused_for( "gso" );
    bind_refused_ipv6_for( "gso" );
}

void rocketnet_test_server_bind_refused_io_uring()
{
    bind_refused_for( "io_uring" );
    bind_refused_ipv6_for( "io_uring" );
}

// the default, no transport named: every transport the server tries is
// refused the same bind
void rocketnet_test_server_bind_refused_default_transports()
{
    bind_refused_for( NULL );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// Windows: a start whose bind Winsock refuses (WSAEADDRINUSE: another socket
// holds the port, no SO_REUSEADDR) returns ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED
// from the iocp and rio transports, and from a bind address that is not this
// host's (WSAEADDRNOTAVAIL). A control start on the free port runs first: a
// host where the transport cannot start at all (rio needs the RIO extension)
// skips, so the status checked is the bind's. Real sockets: a functional test.

#include <winsock2.h>
#include <ws2tcpip.h>

#define bind_check( condition )                                                                       \
    do                                                                                                \
    {                                                                                                 \
        if ( !( condition ) )                                                                         \
        {                                                                                             \
            printf( "check failed: ( %s ), file %s, line %d\n", #condition, __FILE__, __LINE__ );    \
            fflush( stdout );                                                                         \
            exit( 1 );                                                                                \
        }                                                                                             \
    } while ( 0 )

static enum rocketnet_status_t bind_refused_start( const char * transport, const char * public_address, const char * bind_ipv4_address )
{
    struct rocketnet_server_config_t config;
    bind_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    bind_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    memcpy( config.backend_public_key, backend_public, sizeof( config.backend_public_key ) );
    config.protocol_id = 0x62696e6472666564ULL;
    config.max_clients = 1;
    config.packet_filter[0] = 0x62;
    config.packet_filter[1] = 0x69;
    config.packet_filter[2] = 0x6e;
    config.packet_filter[3] = 0x65;
    config.transport = transport;
    config.bind_ipv4_address = bind_ipv4_address;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, public_address, NULL, &config, &status );
    bind_check( server != NULL && status == ROCKETNET_STATUS_OK );
    const enum rocketnet_status_t start_status = rocketnet_server_start( server );
    if ( start_status == ROCKETNET_STATUS_OK )
    {
        double time = 1000.0;
        rocketnet_server_stop( server );
        while ( !rocketnet_server_stopped( server ) )
        {
            time += 0.01;
            rocketnet_server_update( server, time );
            rocketnet_platform_sleep( 0.001 );
        }
    }
    rocketnet_server_destroy( server );
    return start_status;
}

static void bind_refused_for( const char * transport )
{
    WSADATA data;
    bind_check( WSAStartup( MAKEWORD( 2, 2 ), &data ) == 0 );

    // the control first: the transport starts on a port the kernel picks at the
    // start (port 0), or the host cannot run it. No released port is rebound:
    // another process could take it between the close and the start.
    if ( bind_refused_start( transport, "127.0.0.1:0", NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "  skipped: the %s transport does not start on this host\n", transport );
        fflush( stdout );
        WSACleanup();
        return;
    }

    // a port held by another socket for the rest of the test
    SOCKET holder = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    bind_check( holder != INVALID_SOCKET );
    struct sockaddr_in held;
    memset( &held, 0, sizeof( held ) );
    held.sin_family = AF_INET;
    bind_check( inet_pton( AF_INET, "127.0.0.1", &held.sin_addr ) == 1 );
    bind_check( bind( holder, (const struct sockaddr *) &held, sizeof( held ) ) == 0 );
    int held_bytes = (int) sizeof( held );
    bind_check( getsockname( holder, (struct sockaddr *) &held, &held_bytes ) == 0 );
    char held_address[64];
    snprintf( held_address, sizeof( held_address ), "127.0.0.1:%u", (unsigned) ntohs( held.sin_port ) );

    // the held port: refused
    bind_check( bind_refused_start( transport, held_address, NULL ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
    closesocket( holder );

    // an address that is not this host's: refused
    bind_check( bind_refused_start( transport, held_address, "192.0.2.1" ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
    WSACleanup();
}

void rocketnet_test_server_bind_refused_iocp()
{
    bind_refused_for( "iocp" );
}

void rocketnet_test_server_bind_refused_rio()
{
    bind_refused_for( "rio" );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
