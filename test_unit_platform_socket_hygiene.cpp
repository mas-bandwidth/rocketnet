/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Platform socket hygiene: pure functions verifying that mapped IPv6 sources
// are refused on non-dual-stack receive paths, socket timeout conversions
// split seconds and microseconds (tv_usec < 1000000), sleep conversions split
// seconds and microseconds without signed integer overflow, and descriptor 0
// is treated as a valid socket handle.

#include "test_unit_registry.h"

#include "sdk/rocketnet_internal.h"
#include "sdk/rocketnet_address.h"
#include "sdk/rocketnet_platform.h"

#include <stdint.h>
#include <string.h>

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static void platform_socket_hygiene_mapped_source_decision()
{
    struct rocketnet_address_t mapped;
    memset( &mapped, 0, sizeof(mapped) );
    rocketnet_unit_check( rocketnet_address_parse( &mapped, "[::ffff:1.2.3.4]:40000" ) );
    rocketnet_unit_check( rocketnet_address_is_ipv4_in_ipv6( &mapped ) );

    // Demonstrates the bug at BASE:
    // rocketnet_address_convert_ipv6_to_ipv4 unconditionally accepts mapped addresses.
    struct rocketnet_address_t converted = mapped;
    rocketnet_address_convert_ipv6_to_ipv4( &converted );
    rocketnet_unit_check( converted.type == ROCKETNET_ADDRESS_IPV4 );

    // Whereas rocketnet_address_accept_received_source refuses mapped addresses when dual_stack is false:
    struct rocketnet_address_t refused = mapped;
    rocketnet_unit_check( !rocketnet_address_accept_received_source( &refused, false ) );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
    // On the receive path with a non-dual-stack socket:
    struct rocketnet_platform_socket_t s;
    memset( &s, 0, sizeof(s) );
    s.ipv6 = false; // non-dual-stack

    struct sockaddr_storage storage;
    memset( &storage, 0, sizeof(storage) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6*) &storage;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    inet_pton( AF_INET6, "::ffff:1.2.3.4", &in6->sin6_addr );

    struct rocketnet_address_t from;
    memset( &from, 0, sizeof(from) );
    bool accepted = rocketnet_platform_mmsg_read_sockaddr( &s, &storage, &from );
    // At BASE, mmsg_read_sockaddr accepted the mapped source on a non-dual-stack socket (accepted == true).
    // With the fix, it calls rocketnet_address_accept_received_source and refuses it (accepted == false).
    rocketnet_unit_check( !accepted );
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    struct rocketnet_platform_socket_t s;
    memset( &s, 0, sizeof(s) );
    s.ipv6 = false; // non-dual-stack

    struct sockaddr_storage storage;
    memset( &storage, 0, sizeof(storage) );
    struct sockaddr_in6 * in6 = (struct sockaddr_in6*) &storage;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons( 40000 );
    inet_pton( AF_INET6, "::ffff:1.2.3.4", &in6->sin6_addr );

    struct rocketnet_address_t from;
    memset( &from, 0, sizeof(from) );
    bool accepted = rocketnet_msgx_read_sockaddr( &s, &storage, &from );
    rocketnet_unit_check( !accepted );
#endif
}

static void platform_socket_hygiene_timeout_timeval()
{
    // A 1.5 s timeout must split seconds and microseconds:
    // tv_sec = 1, tv_usec = 500000 (< 1000000, avoiding EINVAL on SO_RCVTIMEO).
    struct timeval tv = rocketnet_platform_timeval_from_seconds( 1.5f );
    rocketnet_unit_check( tv.tv_sec == 1 );
    rocketnet_unit_check( tv.tv_usec < 1000000 );
    rocketnet_unit_check( tv.tv_usec == 500000 );
}

static void platform_socket_hygiene_sleep_split()
{
    // A 3000 s sleep conversion must split seconds and microseconds:
    // 3000 s would overflow a 32-bit signed int microsecond counter (3000 * 1000000 > 2^31 - 1).
    struct rocketnet_platform_sleep_split_t split = rocketnet_platform_sleep_split( 3000.0 );
    rocketnet_unit_check( split.seconds == 3000 );
    rocketnet_unit_check( split.microseconds == 0 );
    rocketnet_unit_check( (int32_t) split.microseconds >= 0 );
}

static void platform_socket_hygiene_descriptor_valid()
{
    // File descriptor 0 is STDIN_FILENO, a legal socket descriptor if stdin was closed.
    // At BASE, rocketnet_platform_socket_destroy checked `socket->handle != 0`,
    // treating descriptor 0 as none/invalid and leaking it.
    // Valid descriptors are >= 0 (POSIX) and != INVALID_SOCKET (Windows).
    rocketnet_unit_check( rocketnet_platform_socket_handle_is_valid( 0 ) );
    rocketnet_unit_check( !rocketnet_platform_socket_handle_is_valid( -1 ) );
}

REGISTER_UNIT_TEST( platform_socket_hygiene_mapped_source_decision );
REGISTER_UNIT_TEST( platform_socket_hygiene_timeout_timeval );
REGISTER_UNIT_TEST( platform_socket_hygiene_sleep_split );
REGISTER_UNIT_TEST( platform_socket_hygiene_descriptor_valid );
