/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Platform socket hygiene: pure functions verifying that mapped IPv6 sources
// are refused on non-dual-stack receive paths and descriptor 0 is treated as
// a valid socket handle.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_platform.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

static void test_platform_id_is_the_build_platform()
{
    rocketnet_unit_check( rocketnet_platform_id() == ROCKETNET_PLATFORM );
}

static void test_platform_socket_hygiene_mapped_source_decision()
{
    struct rocketnet_address_t mapped;
    memset( &mapped, 0, sizeof( mapped ) );
    rocketnet_unit_check( rocketnet_address_parse( &mapped, "[::ffff:1.2.3.4]:40000" ) );
    rocketnet_unit_check( rocketnet_address_is_ipv4_in_ipv6( &mapped ) );

    // Demonstrates the bug at BASE:
    // rocketnet_address_convert_ipv6_to_ipv4 unconditionally accepts mapped addresses.

    struct rocketnet_address_t converted = mapped;
    rocketnet_address_convert_ipv6_to_ipv4( &converted );
    rocketnet_unit_check( converted.type == ROCKETNET_ADDRESS_IPV4 );

    // whereas rocketnet_address_accept_received_source refuses mapped addresses when dual_stack is false:

    struct rocketnet_address_t refused = mapped;
    rocketnet_unit_check( !rocketnet_address_accept_received_source( &refused, false ) );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    // on the receive path with a non-dual-stack socket:
    struct rocketnet_platform_socket_t s;
    memset( &s, 0, sizeof( s ) );
    s.ipv6 = false; // non-dual-stack

    struct sockaddr_storage storage;
    memset( &storage, 0, sizeof( storage ) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) &storage;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    inet_pton( AF_INET6, "::ffff:1.2.3.4", &in6->sin6_addr );

    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );
    bool accepted = rocketnet_platform_mmsg_read_sockaddr( &s, &storage, &from );

    // At BASE, mmsg_read_sockaddr accepted the mapped source on a non-dual-stack socket (accepted == true).
    // With the fix, it calls rocketnet_address_accept_received_source and refuses it (accepted == false).

    rocketnet_unit_check( !accepted );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
}

static void test_platform_socket_hygiene_descriptor_valid()
{
    // File descriptor 0 is STDIN_FILENO, a legal socket descriptor if stdin was closed.
    // At BASE, rocketnet_platform_socket_destroy checked `socket->handle != 0`,
    // treating descriptor 0 as none/invalid and leaking it.
    // Valid descriptors are >= 0 (POSIX) and != INVALID_SOCKET (Windows).

    rocketnet_unit_check( rocketnet_platform_socket_handle_is_valid( 0 ) );
    rocketnet_unit_check( !rocketnet_platform_socket_handle_is_valid( (rocketnet_platform_socket_handle_t) -1 ) );
}

// The single-packet receive path drops a refused source and reads the next
// datagram (goto receive_again in each rocketnet_platform_socket_receive_packet
// in sdk/rocketnet_platform.cpp), so spoofed chaff never reads as "socket
// drained". The receive seam feeds scripted datagrams in place of the kernel;
// the socket object has no descriptor and nothing is opened.

struct platform_socket_hygiene_datagram_t
{
    uint8_t addr[16];
    uint16_t port;
    const char * payload;
};

struct platform_socket_hygiene_script_t
{
    const platform_socket_hygiene_datagram_t * datagrams;
    int count;
    int calls;
};

static int platform_socket_hygiene_scripted_receive( void * user, void * packet_data, int max_packet_size, struct sockaddr_storage * from )
{
    platform_socket_hygiene_script_t * script = (platform_socket_hygiene_script_t *) user;
    const int index = script->calls++;
    if ( index >= script->count )
        return -1;
    const platform_socket_hygiene_datagram_t * datagram = &script->datagrams[index];
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) from;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( datagram->port );
    memcpy( &in6->sin6_addr, datagram->addr, 16 );
    const int bytes = (int) strlen( datagram->payload );
    rocketnet_unit_check( bytes <= max_packet_size );

    memcpy( packet_data, datagram->payload, (size_t) bytes );
    return bytes;
}

static int platform_socket_hygiene_receive_scripted( bool ipv6, platform_socket_hygiene_script_t * script, struct rocketnet_address_t * from, uint8_t * packet, int packet_capacity )
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.ipv6 = ipv6;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    socket.handle = INVALID_SOCKET;
#else
    socket.handle = -1;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_platform_socket_set_receive_hook( &socket, platform_socket_hygiene_scripted_receive, script );
    const int bytes = rocketnet_platform_socket_receive_packet( &socket, from, packet, packet_capacity );
    rocketnet_platform_socket_set_receive_hook( &socket, NULL, NULL );

    return bytes;
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static void test_platform_socket_hygiene_batch_hook_drained_reads_nothing()
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.handle = INVALID_SOCKET;

    platform_socket_hygiene_script_t script = { NULL, 0, 0 };
    rocketnet_platform_socket_set_receive_hook( &socket, platform_socket_hygiene_scripted_receive, &script );

    struct rocketnet_address_t from[4];
    uint8_t packets[4 * 64];
    int packet_bytes[4];
    const int count = rocketnet_platform_socket_receive_packets( &socket, from, packets, 64, packet_bytes, 4 );
    rocketnet_platform_socket_set_receive_hook( &socket, NULL, NULL );

    rocketnet_unit_check( count == 0 );
    rocketnet_unit_check( script.calls == 1 );
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_batch_hook_drained_reads_nothing );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static const platform_socket_hygiene_datagram_t platform_socket_hygiene_mapped = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4 }, 40000, "spoof" };
static const platform_socket_hygiene_datagram_t platform_socket_hygiene_native = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 40001, "good" };

static void test_platform_socket_hygiene_refused_source_reads_next()
{
    // a mapped source on a socket that is not dual stack is refused, and the
    // receive goes on to the next datagram instead of returning 0

    const platform_socket_hygiene_datagram_t datagrams[] = { platform_socket_hygiene_mapped, platform_socket_hygiene_native };
    platform_socket_hygiene_script_t script = { datagrams, 2, 0 };
    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );

    uint8_t packet[64];
    const int bytes = platform_socket_hygiene_receive_scripted( false, &script, &from, packet, (int) sizeof( packet ) );
    rocketnet_unit_check( bytes == 4 );
    rocketnet_unit_check( memcmp( packet, "good", 4 ) == 0 );
    rocketnet_unit_check( script.calls == 2 );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( from.port == 40001 );
    rocketnet_unit_check( from.data.ipv6[7] == 1 );
}

static void test_platform_socket_hygiene_refused_source_then_drained()
{
    // only refused datagrams queued: the receive drops each, then reports
    // drained (0) when the source runs dry

    const platform_socket_hygiene_datagram_t datagrams[] = { platform_socket_hygiene_mapped, platform_socket_hygiene_mapped };
    platform_socket_hygiene_script_t script = { datagrams, 2, 0 };
    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );

    uint8_t packet[64];
    const int bytes = platform_socket_hygiene_receive_scripted( false, &script, &from, packet, (int) sizeof( packet ) );
    rocketnet_unit_check( bytes == 0 );
    rocketnet_unit_check( script.calls == 3 );
}

static void test_platform_socket_hygiene_dual_stack_accepts_mapped()
{
    // on a dual stack socket a mapped source is an ipv4 client: accepted at
    // once and canonicalized to ipv4

    const platform_socket_hygiene_datagram_t datagrams[] = { platform_socket_hygiene_mapped };
    platform_socket_hygiene_script_t script = { datagrams, 1, 0 };
    struct rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );

    uint8_t packet[64];
    const int bytes = platform_socket_hygiene_receive_scripted( true, &script, &from, packet, (int) sizeof( packet ) );
#if defined( __OpenBSD__ )
    // OpenBSD keeps IPv6 sockets IPv6-only, so a mapped source is refused
    rocketnet_unit_check( bytes == 0 );
    rocketnet_unit_check( script.calls == 2 );
#else
    rocketnet_unit_check( bytes == 5 );
    rocketnet_unit_check( script.calls == 1 );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 1 && from.data.ipv4[1] == 2 && from.data.ipv4[2] == 3 && from.data.ipv4[3] == 4 );
    rocketnet_unit_check( from.port == 40000 );
#endif // #if defined( __OpenBSD__ )
}

// A stream of chaff far longer than any budget: zero-length datagrams, or
// mapped sources on a socket that is not dual stack. Every read spends one
// unit of the call's read budget, kept or skipped, so a call returns 0 after
// a bounded number of reads (64 for the single-packet call, max_packets for
// a batch) instead of reading until the source runs dry. The seam counts the
// reads; the socket has no descriptor and nothing is opened.

struct platform_socket_hygiene_chaff_t
{
    platform_socket_hygiene_datagram_t datagram;
    int limit;
    int calls;
};

static int platform_socket_hygiene_chaff_receive( void * user, void * packet_data, int max_packet_size, struct sockaddr_storage * from )
{
    platform_socket_hygiene_chaff_t * chaff = (platform_socket_hygiene_chaff_t *) user;
    if ( ++chaff->calls > chaff->limit )
        return -1;
    struct sockaddr_in6 * in6 = (struct sockaddr_in6 *) from;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( chaff->datagram.port );
    memcpy( &in6->sin6_addr, chaff->datagram.addr, 16 );
    const int bytes = (int) strlen( chaff->datagram.payload );
    rocketnet_unit_check( bytes <= max_packet_size );

    memcpy( packet_data, chaff->datagram.payload, (size_t) bytes );
    return bytes;
}

enum platform_socket_hygiene_arm_t
{
    PLATFORM_SOCKET_HYGIENE_SINGLE,
    PLATFORM_SOCKET_HYGIENE_BATCH,
    PLATFORM_SOCKET_HYGIENE_MSGX,
    PLATFORM_SOCKET_HYGIENE_MMSG,
};

static int platform_socket_hygiene_chaff_reads( platform_socket_hygiene_arm_t arm, const platform_socket_hygiene_datagram_t & datagram, int max_packets )
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    socket.handle = INVALID_SOCKET;
#else
    socket.handle = -1;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if defined( __APPLE__ )
    socket.msgx = arm == PLATFORM_SOCKET_HYGIENE_MSGX;
#endif // #if defined( __APPLE__ )

    platform_socket_hygiene_chaff_t chaff = { datagram, 1000000, 0 };
    rocketnet_platform_socket_set_receive_hook( &socket, platform_socket_hygiene_chaff_receive, &chaff );

    struct rocketnet_address_t from[128];
    int packet_bytes[128];
    static uint8_t packet_data[128 * 64];
    rocketnet_unit_check( max_packets <= 128 );

    int received = -1;
    if ( arm == PLATFORM_SOCKET_HYGIENE_SINGLE )
        received = rocketnet_platform_socket_receive_packet( &socket, from, packet_data, 64 );
#if defined( __linux__ )
    else if ( arm == PLATFORM_SOCKET_HYGIENE_MMSG )
        received = rocketnet_platform_socket_receive_packets_mmsg( &socket, from, packet_data, 64, packet_bytes, max_packets );
#endif // #if defined( __linux__ )
    else
        received = rocketnet_platform_socket_receive_packets( &socket, from, packet_data, 64, packet_bytes, max_packets );

    rocketnet_platform_socket_set_receive_hook( &socket, NULL, NULL );

    rocketnet_unit_check( received == 0 );

    return chaff.calls;
}

static const platform_socket_hygiene_datagram_t platform_socket_hygiene_empty = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 40001, "" };

static const platform_socket_hygiene_datagram_t platform_socket_hygiene_chaff[] = { platform_socket_hygiene_empty, platform_socket_hygiene_mapped };

static void platform_socket_hygiene_chaff_batch_is_bounded( platform_socket_hygiene_arm_t arm )
{
    const int budgets[] = { 1, 8, 128 };

    for ( const platform_socket_hygiene_datagram_t & datagram : platform_socket_hygiene_chaff )
    {
        for ( int max_packets : budgets )
        {
            rocketnet_unit_check( platform_socket_hygiene_chaff_reads( arm, datagram, max_packets ) == max_packets );
        }
    }
}

static void test_platform_socket_hygiene_single_chaff_drain_is_bounded()
{
    for ( const platform_socket_hygiene_datagram_t & datagram : platform_socket_hygiene_chaff )
    {
        rocketnet_unit_check( platform_socket_hygiene_chaff_reads( PLATFORM_SOCKET_HYGIENE_SINGLE, datagram, 1 ) == 64 );
    }
}

static void test_platform_socket_hygiene_batch_chaff_drain_is_bounded()
{
    // the portable batch: the per-packet loop on every platform but macOS
    // (the generic pump), the per-packet arm of the msgx call on macOS

    platform_socket_hygiene_chaff_batch_is_bounded( PLATFORM_SOCKET_HYGIENE_BATCH );
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_single_chaff_drain_is_bounded );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_batch_chaff_drain_is_bounded );

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS && !defined( __OpenBSD__ )

// Every read a POSIX receive skips is counted process-wide: a zero-length
// datagram in rocketnet_platform_receive_empties, a refused source in
// rocketnet_platform_receive_refused_sources, one count per read.

static void platform_socket_hygiene_chaff_is_counted( platform_socket_hygiene_arm_t arm, int max_packets )
{
    const uint64_t empties = rocketnet_platform_receive_empties();
    const uint64_t refused = rocketnet_platform_receive_refused_sources();

    const int empty_reads = platform_socket_hygiene_chaff_reads( arm, platform_socket_hygiene_empty, max_packets );
    rocketnet_unit_check( rocketnet_platform_receive_empties() - empties == (uint64_t) empty_reads );
    rocketnet_unit_check( rocketnet_platform_receive_refused_sources() == refused );

    const int refused_reads = platform_socket_hygiene_chaff_reads( arm, platform_socket_hygiene_mapped, max_packets );
    rocketnet_unit_check( rocketnet_platform_receive_refused_sources() - refused == (uint64_t) refused_reads );
    rocketnet_unit_check( rocketnet_platform_receive_empties() - empties == (uint64_t) empty_reads );
}

static void test_platform_socket_hygiene_skipped_reads_are_counted()
{
    platform_socket_hygiene_chaff_is_counted( PLATFORM_SOCKET_HYGIENE_SINGLE, 1 );
    platform_socket_hygiene_chaff_is_counted( PLATFORM_SOCKET_HYGIENE_BATCH, 8 );
#if defined( __APPLE__ )
    platform_socket_hygiene_chaff_is_counted( PLATFORM_SOCKET_HYGIENE_MSGX, 8 );
#endif // #if defined( __APPLE__ )
#if defined( __linux__ )
    platform_socket_hygiene_chaff_is_counted( PLATFORM_SOCKET_HYGIENE_MMSG, 8 );
#endif // #if defined( __linux__ )
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_skipped_reads_are_counted );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS && !defined( __OpenBSD__ )

#if defined( __APPLE__ )
static void test_platform_socket_hygiene_msgx_chaff_drain_is_bounded()
{
    platform_socket_hygiene_chaff_batch_is_bounded( PLATFORM_SOCKET_HYGIENE_MSGX );
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_msgx_chaff_drain_is_bounded );
#endif // #if defined( __APPLE__ )

#if defined( __linux__ )
static void test_platform_socket_hygiene_mmsg_chaff_drain_is_bounded()
{
    platform_socket_hygiene_chaff_batch_is_bounded( PLATFORM_SOCKET_HYGIENE_MMSG );
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_mmsg_chaff_drain_is_bounded );
#endif // #if defined( __linux__ )

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && ROCKETNET_DEV

// A weather errno that repeats (a lasting mbuf shortage keeps returning
// ENOBUFS) must not spin a drain: each receive arm bounds its weather skips
// per call and returns. The seam answers every receive with the errno; the
// socket has no descriptor and nothing is opened.

static int platform_socket_hygiene_weather_drain( bool msgx, int max_packets )
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.handle = -1;
    socket.msgx = msgx;

    struct rocketnet_address_t from[128];
    int packet_bytes[128];
    static uint8_t packet_data[128 * 64];

    rocketnet_platform_inject_receive_errno_repeat_for_testing( ENOBUFS, 1000000 );

    const int received = rocketnet_platform_socket_receive_packets( &socket, from, packet_data, 64, packet_bytes, max_packets );
    rocketnet_unit_check( received == 0 );

    const int remaining = rocketnet_platform_receive_errno_remaining_for_testing();
    rocketnet_platform_inject_receive_errno_for_testing( 0 );

    return remaining;
}

static void test_platform_receive_weather_drain_is_bounded()
{
    // the per-packet arm returns after a bounded number of weather skips

    const int budgets[] = { 1, 8, 128 };

    for ( int max_packets : budgets )
    {
        rocketnet_unit_check( platform_socket_hygiene_weather_drain( false, max_packets ) == 1000000 - max_packets );
    }
}

static void test_platform_single_receive_weather_drain_is_bounded()
{
    struct rocketnet_platform_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.handle = -1;

    struct rocketnet_address_t from;
    uint8_t packet_data[64];
    rocketnet_platform_inject_receive_errno_repeat_for_testing( ENOBUFS, 1000000 );

    const int received = rocketnet_platform_socket_receive_packet( &socket, &from, packet_data, (int) sizeof( packet_data ) );
    const int remaining = rocketnet_platform_receive_errno_remaining_for_testing();
    rocketnet_platform_inject_receive_errno_for_testing( 0 );

    rocketnet_unit_check( received == 0 );
    rocketnet_unit_check( remaining == 1000000 - 64 );
}

static void test_platform_msgx_receive_weather_drain_is_bounded()
{
    // the msgx arm (msgx set, no pause) returns after at most max_packets skips

    const int budgets[] = { 1, 8, 128 };

    for ( int max_packets : budgets )
    {
        rocketnet_unit_check( platform_socket_hygiene_weather_drain( true, max_packets ) == 1000000 - max_packets );
    }
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && ROCKETNET_DEV

void rocketnet_platform_socket_apply_dont_fragment( bool ipv6, void ( *set_option )( void * user, int level, int option_name, int value ), void * user );

struct platform_socket_hygiene_options_t
{
    int levels[4];
    int names[4];
    int values[4];
    int count;
};

static void platform_socket_hygiene_record_option( void * user, int level, int option_name, int value )
{
    platform_socket_hygiene_options_t * options = (platform_socket_hygiene_options_t *) user;
    rocketnet_unit_check( options->count < 4 );
    options->levels[options->count] = level;
    options->names[options->count] = option_name;
    options->values[options->count] = value;
    options->count++;
}

static void test_platform_socket_hygiene_dual_stack_sets_both_dont_fragment()
{
    platform_socket_hygiene_options_t ipv4_options;
    memset( &ipv4_options, 0, sizeof( ipv4_options ) );
    rocketnet_platform_socket_apply_dont_fragment( false, platform_socket_hygiene_record_option, &ipv4_options );

    platform_socket_hygiene_options_t ipv6_options;
    memset( &ipv6_options, 0, sizeof( ipv6_options ) );
    rocketnet_platform_socket_apply_dont_fragment( true, platform_socket_hygiene_record_option, &ipv6_options );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_unit_check( ipv4_options.count == 1 );
    rocketnet_unit_check( ipv4_options.levels[0] == IPPROTO_IP && ipv4_options.names[0] == IP_DONTFRAGMENT && ipv4_options.values[0] == 1 );
    rocketnet_unit_check( ipv6_options.count == 2 );
    rocketnet_unit_check( ipv6_options.levels[0] == IPPROTO_IPV6 && ipv6_options.names[0] == IPV6_DONTFRAG && ipv6_options.values[0] == 1 );
    rocketnet_unit_check( ipv6_options.levels[1] == IPPROTO_IP && ipv6_options.names[1] == IP_DONTFRAGMENT && ipv6_options.values[1] == 1 );
#elif defined( __linux__ )
    rocketnet_unit_check( ipv4_options.count == 1 );
    rocketnet_unit_check( ipv4_options.levels[0] == IPPROTO_IP && ipv4_options.names[0] == IP_MTU_DISCOVER && ipv4_options.values[0] == IP_PMTUDISC_DO );
    rocketnet_unit_check( ipv6_options.count == 2 );
    rocketnet_unit_check( ipv6_options.levels[0] == IPPROTO_IPV6 && ipv6_options.names[0] == IPV6_MTU_DISCOVER && ipv6_options.values[0] == IPV6_PMTUDISC_DO );
    rocketnet_unit_check( ipv6_options.levels[1] == IPPROTO_IP && ipv6_options.names[1] == IP_MTU_DISCOVER && ipv6_options.values[1] == IP_PMTUDISC_DO );
#elif defined( IPV6_DONTFRAG ) && defined( IP_DONTFRAG )
    rocketnet_unit_check( ipv4_options.count == 1 );
    rocketnet_unit_check( ipv4_options.levels[0] == IPPROTO_IP && ipv4_options.names[0] == IP_DONTFRAG && ipv4_options.values[0] == 1 );
    rocketnet_unit_check( ipv6_options.count == 2 );
    rocketnet_unit_check( ipv6_options.levels[0] == IPPROTO_IPV6 && ipv6_options.names[0] == IPV6_DONTFRAG && ipv6_options.values[0] == 1 );
    rocketnet_unit_check( ipv6_options.levels[1] == IPPROTO_IP && ipv6_options.names[1] == IP_DONTFRAG && ipv6_options.values[1] == 1 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

REGISTER_UNIT_TEST( test_platform_socket_hygiene_mapped_source_decision );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_dual_stack_sets_both_dont_fragment );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_descriptor_valid );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_refused_source_reads_next );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_refused_source_then_drained );
REGISTER_UNIT_TEST( test_platform_socket_hygiene_dual_stack_accepts_mapped );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && ROCKETNET_DEV
REGISTER_UNIT_TEST( test_platform_receive_weather_drain_is_bounded );
REGISTER_UNIT_TEST( test_platform_msgx_receive_weather_drain_is_bounded );
REGISTER_UNIT_TEST( test_platform_single_receive_weather_drain_is_bounded );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC && ROCKETNET_DEV
REGISTER_UNIT_TEST( test_platform_id_is_the_build_platform );
