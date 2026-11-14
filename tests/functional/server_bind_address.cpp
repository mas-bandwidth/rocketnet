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
#include "../check_count.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define bind_check( condition )                                                                       \
    do                                                                                                \
    {                                                                                                 \
        ROCKETNET_TEST_COUNT_CHECK();                                                                 \
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
        ROCKETNET_TEST_COUNT_CHECK();                                                                 \
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

#include "../../examples/shutdown.h"
#include "rocketnet_connect_token.h"

#include <sys/resource.h>

uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

// msgx's shared socket binds with neither SO_REUSEADDR nor SO_REUSEPORT and
// sets them after the bind, so a port another process holds is refused like
// every transport's; its client sockets still join the port. With the options
// set before the bind, macOS let it succeed and the more specific socket took
// the other's datagrams (the parallel functional suite lost packets to it).
static struct rocketnet_server_t * bind_started_msgx( const char * public_address )
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
    config.transport = "msgx";
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, public_address[0] == '[' ? NULL : public_address, public_address[0] == '[' ? public_address : NULL, &config, &status );
    bind_check( server != NULL && status == ROCKETNET_STATUS_OK );
    if ( rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        return NULL;
    }
    return server;
}

static void bind_stopped_msgx( struct rocketnet_server_t * server )
{
    double time = 1000.0;
    rocketnet_server_stop( server );
    while ( !rocketnet_server_stopped( server ) )
    {
        time += 0.01;
        rocketnet_server_update( server, time );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_server_destroy( server );
}

// a port another msgx server holds (its shared socket has SO_REUSEPORT, set
// after its bind) is refused too: never a silent share of its datagrams.
// IPv6 is skipped where the host has none.
static void bind_refused_held_by_msgx( bool ipv6 )
{
    struct rocketnet_server_t * holder = bind_started_msgx( ipv6 ? "[::1]:0" : "127.0.0.1:0" );
    if ( !holder )
    {
        bind_check( ipv6 );
        return;
    }
    char held_address[64];
    snprintf( held_address, sizeof( held_address ), ipv6 ? "[::1]:%u" : "127.0.0.1:%u", (unsigned) rocketnet_server_port( holder ) );
    bind_check( bind_refused_start( "msgx", held_address, NULL ) == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
    bind_stopped_msgx( holder );
}

void rocketnet_test_server_bind_refused_msgx()
{
    bind_refused_for( "msgx" );
    bind_refused_ipv6_for( "msgx" );
    bind_refused_held_by_msgx( false );
    bind_refused_held_by_msgx( true );
}

// A server that serves both families binds the IPv6 wildcard [::] with
// IPV6_V6ONLY off. Darwin binds a dual-stack IPv6 wildcard without checking
// the IPv4 port space: without an IPv4 reservation of the port first, the
// bind succeeds over a port a plain IPv4 socket holds, and that socket then
// receives every IPv4 datagram (the server starts, and no IPv4 client ever
// reaches it). Both macOS transports reserve; these cases hold portable to it
// as well as msgx. Real sockets: functional tests. Their rows run only where
// the host has IPv6 loopback (runs_here rocketnet_test_server_bind_dual_stack_runs_here
// below); elsewhere they print SKIPPED, and run by name there they fail.

// these cases name their transports: an outer suite pin is set aside for the
// case and put back after it
struct bind_pins_t
{
    char server[64];
    char client[64];
    bool server_set;
    bool client_set;
};

static void bind_pin_save( const char * name, char * value, size_t bytes, bool * set )
{
    const char * now = getenv( name );
    *set = now != NULL;
    snprintf( value, bytes, "%s", now ? now : "" );
    unsetenv( name );
}

static void bind_pins_clear( struct bind_pins_t * pins )
{
    bind_pin_save( "ROCKETNET_SERVER_TRANSPORT", pins->server, sizeof( pins->server ), &pins->server_set );
    bind_pin_save( "ROCKETNET_CLIENT_TRANSPORT", pins->client, sizeof( pins->client ), &pins->client_set );
}

static void bind_pins_restore( const struct bind_pins_t * pins )
{
    if ( pins->server_set )
    {
        setenv( "ROCKETNET_SERVER_TRANSPORT", pins->server, 1 );
    }
    if ( pins->client_set )
    {
        setenv( "ROCKETNET_CLIENT_TRANSPORT", pins->client, 1 );
    }
}

static enum rocketnet_status_t bind_dual_stack_start( const char * transport, unsigned port, struct rocketnet_server_t ** started )
{
    struct rocketnet_server_config_t config;
    bind_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
#if ROCKETNET_DEV
    // the both-families case connects with development tokens
    config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config.backend_public_key, rocketnet_dev_backend_public_key, sizeof( config.backend_public_key ) );
#else // #if ROCKETNET_DEV
    // a production library refuses the published development keys
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    bind_check( rocketnet_generate_backend_keypair( backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    memcpy( config.backend_public_key, backend_public, sizeof( config.backend_public_key ) );
    config.protocol_id = 0x6475616c73746b31ULL;
    config.packet_filter[0] = 0x64;
    config.packet_filter[1] = 0x75;
    config.packet_filter[2] = 0x61;
    config.packet_filter[3] = 0x6c;
#endif // #if ROCKETNET_DEV
    config.max_clients = 2;
    config.transport = transport;

    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%u", port );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%u", port );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, ipv4_address, ipv6_address, &config, &status );
    bind_check( server != NULL && status == ROCKETNET_STATUS_OK );
    const enum rocketnet_status_t start_status = rocketnet_server_start( server );
    if ( start_status == ROCKETNET_STATUS_OK && started )
    {
        *started = server;
        return start_status;
    }
    if ( start_status == ROCKETNET_STATUS_OK )
    {
        bind_stopped_msgx( server );
    }
    else
    {
        rocketnet_server_destroy( server );
    }
    return start_status;
}

// the runs_here of the three dual-stack rows: an IPv6 socket binds [::1]
bool rocketnet_test_server_bind_dual_stack_runs_here()
{
    const int probe = socket( AF_INET6, SOCK_DGRAM, IPPROTO_UDP );
    if ( probe < 0 )
    {
        return false;
    }
    struct sockaddr_in6 any;
    memset( &any, 0, sizeof( any ) );
    any.sin6_family = AF_INET6;
    any.sin6_addr = in6addr_loopback;
    const bool bound = bind( probe, (const struct sockaddr *) &any, sizeof( any ) ) == 0;
    close( probe );
    return bound;
}

// a plain IPv4 socket on 0.0.0.0 at a kernel-chosen port; returns the socket
static int bind_hold_ipv4_wildcard( unsigned * port )
{
    const int holder = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    bind_check( holder >= 0 );
    struct sockaddr_in held;
    memset( &held, 0, sizeof( held ) );
    held.sin_family = AF_INET;
    held.sin_addr.s_addr = htonl( INADDR_ANY );
    bind_check( bind( holder, (const struct sockaddr *) &held, sizeof( held ) ) == 0 );
    socklen_t held_bytes = sizeof( held );
    bind_check( getsockname( holder, (struct sockaddr *) &held, &held_bytes ) == 0 );
    *port = ntohs( held.sin_port );
    return holder;
}

// an explicit port: a plain IPv4 socket holds 0.0.0.0:P, and a server asked
// for [::]:P is refused with TRANSPORT_BIND_REFUSED
static void bind_dual_stack_ipv4_held_for( const char * transport )
{
    unsigned port = 0;
    const int holder = bind_hold_ipv4_wildcard( &port );
    const enum rocketnet_status_t status = bind_dual_stack_start( transport, port, NULL );
    printf( "  %s: [::]:%u with 0.0.0.0:%u held -> %s\n", transport, port, port, rocketnet_status_name( status ) );
    fflush( stdout );
    close( holder );
    bind_check( status == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
}

void rocketnet_test_server_bind_dual_stack_ipv4_held()
{
    bind_check( rocketnet_test_server_bind_dual_stack_runs_here() ); // the row runs only where this holds
    struct bind_pins_t pins;
    bind_pins_clear( &pins );
    bind_dual_stack_ipv4_held_for( "portable" );
    bind_dual_stack_ipv4_held_for( "msgx" );
    bind_pins_restore( &pins );
}

// port 0: with many IPv4 ports held on 0.0.0.0, a dual-stack start never
// returns one of them. Measured without the reservation on the studio: about
// one start in six took a held port (69 of 400 with 3000 held), so 40 starts
// with 2000 held are red with near certainty when the reservation is missing.
#define BIND_HELD_IPV4_PORTS 2000
#define BIND_PORT_ZERO_STARTS 40

static void bind_dual_stack_port_zero_for( const char * transport, const unsigned char * held_port )
{
    int landed = 0;
    for ( int i = 0; i < BIND_PORT_ZERO_STARTS; i++ )
    {
        struct rocketnet_server_t * server = NULL;
        bind_check( bind_dual_stack_start( transport, 0, &server ) == ROCKETNET_STATUS_OK );
        const uint16_t port = rocketnet_server_port( server );
        bind_check( port != 0 );
        if ( held_port[port] )
        {
            landed++;
        }
        bind_stopped_msgx( server );
    }
    printf( "  %s: %d dual-stack [::]:0 starts, %d on a held IPv4 port\n", transport, BIND_PORT_ZERO_STARTS, landed );
    fflush( stdout );
    bind_check( landed == 0 );
}

void rocketnet_test_server_bind_dual_stack_port_zero()
{
    bind_check( rocketnet_test_server_bind_dual_stack_runs_here() ); // the row runs only where this holds

    struct rlimit limit;
    bind_check( getrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    const rlim_t wanted = (rlim_t) BIND_HELD_IPV4_PORTS + 1024;
    if ( limit.rlim_cur < wanted )
    {
        limit.rlim_cur = limit.rlim_max < wanted ? limit.rlim_max : wanted;
        bind_check( setrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    }

    static int holders[BIND_HELD_IPV4_PORTS];
    static unsigned char held_port[65536];
    memset( held_port, 0, sizeof( held_port ) );
    for ( int i = 0; i < BIND_HELD_IPV4_PORTS; i++ )
    {
        unsigned port = 0;
        holders[i] = bind_hold_ipv4_wildcard( &port );
        held_port[port] = 1;
    }

    struct bind_pins_t pins;
    bind_pins_clear( &pins );
    bind_dual_stack_port_zero_for( "portable", held_port );
    bind_dual_stack_port_zero_for( "msgx", held_port );
    bind_pins_restore( &pins );

    for ( int i = 0; i < BIND_HELD_IPV4_PORTS; i++ )
    {
        close( holders[i] );
    }
}

// the control: an IPv4 client and an IPv6 client both connect to one msgx
// server that serves both families (bound [::] under its reservation), so the
// reservation leaves the port to the server and its client sockets join it.
// Development tokens: not in functional_production.
#if ROCKETNET_DEV
void rocketnet_test_server_msgx_dual_stack_both_families_connect()
{
    bind_check( rocketnet_test_server_bind_dual_stack_runs_here() ); // the row runs only where this holds

    struct bind_pins_t pins;
    bind_pins_clear( &pins );
    struct rocketnet_server_t * server = NULL;
    bind_check( bind_dual_stack_start( "msgx", 0, &server ) == ROCKETNET_STATUS_OK );
    bind_check( strcmp( rocketnet_server_transport_name( server ), "msgx" ) == 0 );
    const unsigned port = rocketnet_server_port( server );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );

    char ipv4_address[64];
    char ipv6_address[64];
    snprintf( ipv4_address, sizeof( ipv4_address ), "127.0.0.1:%u", port );
    snprintf( ipv6_address, sizeof( ipv6_address ), "[::1]:%u", port );

    struct rocketnet_client_t * clients[2];
    const double case_start = rocketnet_platform_time();
    for ( int family = 0; family < 2; family++ )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        bind_check( rocketnet_create_dev_connect_token( family == 0 ? ipv4_address : NULL, family == 0 ? NULL : ipv6_address, server_key, 300, 10,
                                                        0x6475616c00ULL + (uint64_t) family, ROCKETNET_DEV_PROTOCOL_ID,
                                                        rocketnet_functional_token_timestamp( 0, case_start ), NULL, token ) == ROCKETNET_STATUS_OK );
        struct rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "portable";
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        clients[family] = rocketnet_client_create( NULL, &client_config, &status );
        bind_check( clients[family] && status == ROCKETNET_STATUS_OK );
        bind_check( rocketnet_client_connect( clients[family], token ) == ROCKETNET_STATUS_OK );
    }

    const double deadline = rocketnet_platform_time() + 10.0;
    bool connected = false;
    while ( !connected && rocketnet_platform_time() < deadline )
    {
        const double now = 1000.0 + ( rocketnet_platform_time() - case_start );
        rocketnet_server_update( server, now );
        connected = true;
        for ( int family = 0; family < 2; family++ )
        {
            rocketnet_client_update( clients[family], now );
            rocketnet_client_send_packets( clients[family] );
            connected = connected && rocketnet_client_state( clients[family] ) == ROCKETNET_CLIENT_STATE_CONNECTED;
        }
        rocketnet_server_send_packets( server );
        connected = connected && rocketnet_server_num_connected_clients( server ) == 2;
        rocketnet_platform_sleep( 0.001 );
    }
    printf( "  msgx [::]:%u: IPv4 client %d, IPv6 client %d, server connected %d\n", port, rocketnet_client_state( clients[0] ),
            rocketnet_client_state( clients[1] ), rocketnet_server_num_connected_clients( server ) );
    fflush( stdout );
    bind_check( connected );

    const double now = 1000.0 + ( rocketnet_platform_time() - case_start );
    for ( int family = 0; family < 2; family++ )
    {
        rn_cli_client_stop_and_destroy( clients[family], now );
    }
    rn_cli_server_stop_and_destroy( server );
    bind_pins_restore( &pins );
}
#endif // #if ROCKETNET_DEV

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
// host's (WSAEADDRNOTAVAIL). A control start on a free port runs first and
// must succeed, so the status checked is the bind's: a transport that does not
// start fails the case (every Windows CI host runs iocp and RIO; the rio rows
// elsewhere check rocketnet_server_rio_available too). Real sockets: a
// functional test.

#include <winsock2.h>
#include <ws2tcpip.h>

#define bind_check( condition )                                                                       \
    do                                                                                                \
    {                                                                                                 \
        ROCKETNET_TEST_COUNT_CHECK();                                                                 \
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
    // start (port 0). No released port is rebound: another process could take
    // it between the close and the start.
    const enum rocketnet_status_t control = bind_refused_start( transport, "127.0.0.1:0", NULL );
    if ( control != ROCKETNET_STATUS_OK )
    {
        printf( "  the %s transport did not start on a free port: %s\n", transport, rocketnet_status_name( control ) );
        fflush( stdout );
    }
    bind_check( control == ROCKETNET_STATUS_OK );

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
