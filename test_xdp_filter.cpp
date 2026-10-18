/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The XDP receive filter, judged by the kernel.
//
// Loads server_xdp/server_xdp.c through the verifier and runs crafted frames
// through it with BPF_PROG_TEST_RUN: the kernel returns the program's verdict
// on each frame and transmits nothing. No socket, no interface and no packet
// traffic is involved, and no XDP program is attached anywhere -- except in
// xdp_on_link_reply_reaches_the_peer, which runs only inside a virtme-ng
// guest and attaches the server to a veth pair it creates and removes.
//
// bpf_redirect_map only returns XDP_REDIRECT when the map slot is populated,
// and an XSKMAP slot needs an AF_XDP socket. This test changes the program's
// map type to a DEVMAP before load (the program bytes are the same) and
// points slot 0 at the loopback interface, so a frame the filter accepts
// comes back as XDP_REDIRECT. The program under test is the one the library
// embeds; nothing in it is weakened to make a case pass.
//
// Loading a BPF program needs root, or CAP_BPF and CAP_NET_ADMIN:
//
//     sudo ./bin/test_xdp_filter [-v] [server_xdp.o]
//
// -v prints the last line of the verifier's log for the load. Exit 0: every case passed. Exit 1: a case failed. Exit 2: the object did
// not load (the verifier log tail is printed) or the process lacks privilege.

#include "rocketnet_wire.h"
#include "rocketnet_xdp_replay_marks.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <net/if.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ROCKETNET_XDP_FILTER_OBJECT
#define ROCKETNET_XDP_FILTER_OBJECT "server_xdp.o"
#endif

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800
#endif

#ifndef ETH_P_IPV6
#define ETH_P_IPV6 0x86DD
#endif

// the headers IPv6 may carry between the fixed header and UDP, by next header value

#define IPV6_HOP_BY_HOP 0
#define IPV6_ROUTING 43
#define IPV6_FRAGMENT 44
#define IPV6_ESP 50
#define IPV6_AH 51
#define IPV6_ICMPV6 58
#define IPV6_DESTINATION_OPTIONS 60
#define IP_UDP 17

#define NUM_COUNTER_SLOTS ROCKETNET_SERVER_XDP_NUM_COUNTERS

static const uint16_t server_port = 40000;
static const uint16_t client_port = 50000;
static const uint8_t server_ipv4[4] = { 192, 0, 2, 10 };
static const uint8_t other_ipv4[4] = { 192, 0, 2, 11 };
static const uint8_t client_ipv4[4] = { 198, 51, 100, 7 };
static const uint8_t server_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
static const uint8_t other_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11 };
static const uint8_t client_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07 };
static const uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES] = { 0xA1, 0xB2, 0xC3, 0xD4 };
static const uint8_t filter_epoch = 0;

#define FRAME_CAPACITY 2048

struct frame_t
{
    uint8_t data[FRAME_CAPACITY];
    int size;
};

// what one crafted frame looks like

struct spec_t
{
    bool ipv6;                                  // false: IPv4
    int next_header;                            // IPv6 only; -1 = UDP follows the fixed header directly
    uint16_t destination_port;
    const uint8_t * destination_address;        // 4 or 16 bytes; NULL = the server's address
    int packet_bytes;                           // the UDP payload, the RocketNet packet
    uint8_t packet_type;
    uint8_t epoch;
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    int truncate_to;                            // cut the frame to this many bytes; -1 = whole frame
    uint32_t sequence;                          // the 4 little-endian bytes after the prefilter header
    uint16_t source_port;                       // the client's UDP source port
};

static spec_t spec_default( bool ipv6 )
{
    spec_t spec;
    memset( &spec, 0, sizeof(spec) );
    spec.ipv6 = ipv6;
    spec.next_header = -1;
    spec.destination_port = server_port;
    spec.destination_address = NULL;
    spec.packet_bytes = ROCKETNET_RECEIVE_CEILING;      // the largest packet a server accepts
    spec.packet_type = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    spec.epoch = filter_epoch;
    memcpy( spec.filter, filter_value, sizeof(spec.filter) );
    spec.truncate_to = -1;
    spec.sequence = 0;
    spec.source_port = client_port;
    return spec;
}

static void put16( uint8_t * p, uint16_t value )
{
    p[0] = (uint8_t) ( value >> 8 );
    p[1] = (uint8_t) value;
}

// Ethernet, then the IP header for the family, then any extension header,
// then UDP, then the RocketNet packet: the prefilter header (type, epoch,
// filter value), the sequence little endian, and zeros after it.

static void build_frame( frame_t * frame, const spec_t & spec )
{
    memset( frame, 0, sizeof(*frame) );
    uint8_t * p = frame->data;

    memset( p, 0x02, 6 );                       // destination mac
    memset( p + 6, 0x04, 6 );                   // source mac
    put16( p + 12, spec.ipv6 ? ETH_P_IPV6 : ETH_P_IP );
    p += ROCKETNET_ETHERNET_HEADER_BYTES;

    const int udp_bytes = ROCKETNET_UDP_HEADER_BYTES + spec.packet_bytes;
    const bool has_extension = spec.ipv6 && spec.next_header >= 0 && spec.next_header != IPV6_ICMPV6;

    if ( !spec.ipv6 )
    {
        const uint8_t * destination = spec.destination_address ? spec.destination_address : server_ipv4;
        p[0] = 0x45;                            // version 4, ihl 5
        put16( p + 2, (uint16_t) ( ROCKETNET_IPV4_HEADER_BYTES + udp_bytes ) );
        p[8] = 64;                              // ttl
        p[9] = IP_UDP;
        memcpy( p + 12, client_ipv4, 4 );
        memcpy( p + 16, destination, 4 );
        p += ROCKETNET_IPV4_HEADER_BYTES;
    }
    else
    {
        const uint8_t * destination = spec.destination_address ? spec.destination_address : server_ipv6;
        const int extension_bytes = has_extension ? 8 : 0;
        p[0] = 0x60;                            // version 6
        put16( p + 4, (uint16_t) ( extension_bytes + udp_bytes ) );
        p[6] = (uint8_t) ( spec.next_header >= 0 ? spec.next_header : IP_UDP );
        p[7] = 64;                              // hop limit
        memcpy( p + 8, client_ipv6, 16 );
        memcpy( p + 24, destination, 16 );
        p += ROCKETNET_IPV6_HEADER_BYTES;
        if ( has_extension )
        {
            p[0] = IP_UDP;                      // this header is followed by UDP
            if ( spec.next_header == IPV6_FRAGMENT )
            {
                p[3] = 0x01;                    // offset 0, more fragments
                p[7] = 0x2A;                    // identification
            }
            p += extension_bytes;
        }
    }

    // a frame that only wanted a different next header (ICMPv6) carries the
    // same bytes after the fixed header; the filter must never look at them

    put16( p, spec.source_port );               // source port
    put16( p + 2, spec.destination_port );
    put16( p + 4, (uint16_t) udp_bytes );
    p += ROCKETNET_UDP_HEADER_BYTES;

    if ( spec.packet_bytes > 0 )
        p[0] = spec.packet_type;
    if ( spec.packet_bytes > 1 )
        p[1] = spec.epoch;
    if ( spec.packet_bytes > 2 + ROCKETNET_FILTER_VALUE_BYTES - 1 )
        memcpy( p + 2, spec.filter, ROCKETNET_FILTER_VALUE_BYTES );
    for ( int i = 0; i < 4 && ROCKETNET_PREFILTER_BYTES + i < spec.packet_bytes; i++ )
        p[ROCKETNET_PREFILTER_BYTES + i] = (uint8_t) ( spec.sequence >> ( 8 * i ) );
    p += spec.packet_bytes;

    frame->size = (int) ( p - frame->data );
    if ( spec.truncate_to >= 0 && spec.truncate_to < frame->size )
        frame->size = spec.truncate_to;
}

// the loaded program

struct filter_t
{
    struct bpf_object * object;
    int program_fd;
    int config_fd;
    int stats_fd;
    int stats_value_bytes;
    int num_cpus;
};

static char verifier_log[1 << 22];

static void print_log_tail( int lines )
{
    const size_t length = strnlen( verifier_log, sizeof(verifier_log) );
    if ( length == 0 )
        return;
    size_t end = length;
    while ( end > 0 && ( verifier_log[end - 1] == '\n' || verifier_log[end - 1] == ' ' ) )
        end--;
    size_t start = end;
    int seen = 0;
    while ( start > 0 )
    {
        if ( verifier_log[start - 1] == '\n' && ++seen == lines )
            break;
        start--;
    }
    printf( "verifier log tail:\n%.*s\n", (int) ( end - start ), verifier_log + start );
}

static int print_libbpf( enum libbpf_print_level level, const char * format, va_list args )
{
    if ( level == LIBBPF_DEBUG )
        return 0;
    return vfprintf( stderr, format, args );
}

// exit code 2 on any failure to get the program loaded

static bool load_filter( filter_t * filter, const char * path )
{
    memset( filter, 0, sizeof(*filter) );

    libbpf_set_print( print_libbpf );

    struct bpf_object_open_opts options;
    memset( &options, 0, sizeof(options) );
    options.sz = sizeof(options);
    options.kernel_log_buf = verifier_log;
    options.kernel_log_size = sizeof(verifier_log);
    options.kernel_log_level = 1;

    filter->object = bpf_object__open_file( path, &options );
    if ( filter->object == NULL )
    {
        fprintf( stderr, "could not open %s: %s\n", path, strerror( errno ) );
        return false;
    }

    // nothing is pinned under /sys/fs/bpf, and the XSKMAP becomes a DEVMAP
    // so a redirect can succeed without an AF_XDP socket

    struct bpf_map * map;
    bpf_object__for_each_map( map, filter->object )
    {
        bpf_map__set_pin_path( map, NULL );
    }

    struct bpf_program * program = bpf_object__find_program_by_name( filter->object, "server_xdp_filter" );
    if ( program == NULL || bpf_program__set_type( program, BPF_PROG_TYPE_XDP ) != 0 )
    {
        fprintf( stderr, "the program has no server_xdp_filter to load as XDP\n" );
        return false;
    }

    struct bpf_map * socket_map = bpf_object__find_map_by_name( filter->object, "server_xdp_socket_map" );
    if ( socket_map == NULL || bpf_map__set_type( socket_map, BPF_MAP_TYPE_DEVMAP ) != 0 )
    {
        fprintf( stderr, "the program has no server_xdp_socket_map to change\n" );
        return false;
    }

    const int result = bpf_object__load( filter->object );
    if ( result != 0 )
    {
        fprintf( stderr, "load failed: %s\n", strerror( -result ) );
        if ( -result == EPERM || -result == EACCES )
            fprintf( stderr, "loading a BPF program needs root, or CAP_BPF and CAP_NET_ADMIN\n" );
        print_log_tail( 30 );
        return false;
    }

    struct bpf_map * config_map = bpf_object__find_map_by_name( filter->object, "server_xdp_config_map" );
    struct bpf_map * stats_map = bpf_object__find_map_by_name( filter->object, "server_xdp_stats_map" );
    if ( config_map == NULL || stats_map == NULL )
    {
        fprintf( stderr, "the program lacks its config and stats maps\n" );
        return false;
    }

    filter->program_fd = bpf_program__fd( program );
    filter->config_fd = bpf_map__fd( config_map );
    filter->stats_fd = bpf_map__fd( stats_map );
    filter->stats_value_bytes = (int) bpf_map__value_size( stats_map );
    filter->num_cpus = libbpf_num_possible_cpus();

    // slot 0 of the redirect map is the loopback interface: the program
    // asks for the receive queue, which test_run always reports as 0

    const __u32 key = 0;
    const __u32 loopback = if_nametoindex( "lo" );
    if ( loopback == 0 || bpf_map_update_elem( bpf_map__fd( socket_map ), &key, &loopback, BPF_ANY ) != 0 )
    {
        fprintf( stderr, "could not populate the redirect map: %s\n", strerror( errno ) );
        return false;
    }

    return true;
}

// The config the library writes, with the IPv6 destination given. The write
// is the map's own value size, so the bytes are the layout the program was
// compiled against.

static bool write_config( filter_t * filter, const uint8_t * ipv4, const uint8_t * ipv6 )
{
    struct rocketnet_server_xdp_config_t config;
    memset( &config, 0, sizeof(config) );
    if ( ipv4 )
        memcpy( &config.public_address, ipv4, 4 );
    config.public_port = (uint16_t) ( ( server_port >> 8 ) | ( ( server_port & 0xFF ) << 8 ) );
    memcpy( config.filter_value, filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    config.filter_epoch = filter_epoch;
    if ( ipv6 )
        memcpy( config.public_address_ipv6, ipv6, 16 );
    const __u32 key = 0;
    if ( bpf_map_update_elem( filter->config_fd, &key, &config, BPF_ANY ) != 0 )
    {
        fprintf( stderr, "could not write the config map: %s\n", strerror( errno ) );
        return false;
    }
    return true;
}

static void read_counters( filter_t * filter, uint64_t * counters )
{
    memset( counters, 0, NUM_COUNTER_SLOTS * sizeof(uint64_t) );
    const int stride = ( filter->stats_value_bytes + 7 ) / 8 * 8;
    uint8_t * values = (uint8_t*) calloc( (size_t) filter->num_cpus, (size_t) stride );
    const __u32 key = 0;
    if ( values && bpf_map_lookup_elem( filter->stats_fd, &key, values ) == 0 )
    {
        for ( int cpu = 0; cpu < filter->num_cpus; cpu++ )
        {
            const uint64_t * value = (const uint64_t*) ( values + cpu * stride );
            for ( int i = 0; i < NUM_COUNTER_SLOTS && i < filter->stats_value_bytes / 8; i++ )
                counters[i] += value[i];
        }
    }
    free( values );
}

static const char * verdict_name( int verdict )
{
    switch ( verdict )
    {
        case XDP_ABORTED:   return "XDP_ABORTED";
        case XDP_DROP:      return "XDP_DROP";
        case XDP_PASS:      return "XDP_PASS";
        case XDP_TX:        return "XDP_TX";
        case XDP_REDIRECT:  return "XDP_REDIRECT";
        default:            return "XDP_?";
    }
}

// Run one frame. counter < 0: no counter moves. Otherwise exactly that
// counter goes up by one and every other counter stays where it was.

static bool run_frame( filter_t * filter, const char * label, const spec_t & spec, int want_verdict, int want_counter )
{
    frame_t frame;
    build_frame( &frame, spec );

    uint64_t before[NUM_COUNTER_SLOTS];
    read_counters( filter, before );

    static uint8_t out[4096];
    struct bpf_test_run_opts options;
    memset( &options, 0, sizeof(options) );
    options.sz = sizeof(options);
    options.data_in = frame.data;
    options.data_size_in = (__u32) frame.size;
    options.data_out = out;
    options.data_size_out = sizeof(out);
    options.repeat = 1;

    const int result = bpf_prog_test_run_opts( filter->program_fd, &options );
    if ( result != 0 )
    {
        printf( "  FAIL %s: test_run failed: %s\n", label, strerror( errno ) );
        return false;
    }

    uint64_t after[NUM_COUNTER_SLOTS];
    read_counters( filter, after );

    bool ok = (int) options.retval == want_verdict;
    if ( !ok )
        printf( "  FAIL %s: verdict %s, wanted %s\n", label, verdict_name( (int) options.retval ), verdict_name( want_verdict ) );

    for ( int i = 0; i < NUM_COUNTER_SLOTS; i++ )
    {
        const uint64_t want = ( i == want_counter ) ? 1 : 0;
        if ( after[i] - before[i] != want )
        {
            printf( "  FAIL %s: counter %d moved by %llu, wanted %llu\n", label, i,
                    (unsigned long long) ( after[i] - before[i] ), (unsigned long long) want );
            ok = false;
        }
    }
    return ok;
}

#define NO_COUNTER -1

// the cases

typedef bool (*case_function_t)( filter_t * filter );

static bool ipv4_rocketnet_packet_redirects( filter_t * filter )
{
    if ( !write_config( filter, server_ipv4, NULL ) )
        return false;
    return run_frame( filter, "ipv4 max-size packet", spec_default( false ), XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
}

static bool ipv6_rocketnet_packet_redirects( filter_t * filter )
{
    bool ok = true;

    // the configured destination

    ok &= write_config( filter, NULL, server_ipv6 );
    ok &= run_frame( filter, "ipv6 max-size packet to the configured address", spec_default( true ), XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );

    // an all-zero IPv6 destination accepts any destination, as IPv4 does

    ok &= write_config( filter, NULL, NULL );
    ok &= run_frame( filter, "ipv6 max-size packet, any destination", spec_default( true ), XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );

    // the smallest packet a server accepts, and the same frame from IPv4

    spec_t smallest = spec_default( true );
    smallest.packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
    smallest.packet_type = ROCKETNET_CONNECTION_DISCONNECT_PACKET;
    ok &= run_frame( filter, "ipv6 smallest packet", smallest, XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
    return ok;
}

// RFC 8200 section 4: each of these follows the fixed header. The filter does
// not walk the chain: the frame goes to the kernel stack, counted by name.

static bool ipv6_extension_header_passes( filter_t * filter )
{
    static const int headers[] = { IPV6_HOP_BY_HOP, IPV6_ROUTING, IPV6_DESTINATION_OPTIONS, IPV6_AH, IPV6_ESP };
    static const char * names[] = { "hop-by-hop", "routing", "destination options", "AH", "ESP" };
    bool ok = write_config( filter, NULL, server_ipv6 );
    for ( int i = 0; i < (int) ( sizeof(headers) / sizeof(headers[0]) ); i++ )
    {
        spec_t spec = spec_default( true );
        spec.next_header = headers[i];
        char label[64];
        snprintf( label, sizeof(label), "ipv6 %s header", names[i] );
        ok &= run_frame( filter, label, spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );
    }

    // an extension header in front of a packet that would be dropped is still
    // passed, not dropped: the filter never opens it

    spec_t spec = spec_default( true );
    spec.next_header = IPV6_HOP_BY_HOP;
    spec.packet_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    ok &= run_frame( filter, "ipv6 hop-by-hop header, oversize packet behind it", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );
    return ok;
}

// RFC 8200 section 4.5: the fragment header. A fragmented datagram is not a
// RocketNet packet on this path and is never dropped by it.

static bool ipv6_fragment_passes( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.next_header = IPV6_FRAGMENT;
    ok &= run_frame( filter, "ipv6 fragment header", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );

    // fragments of other ports pass the same way, counted the same way

    spec.destination_port = 53;
    ok &= run_frame( filter, "ipv6 fragment header, other port", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );
    return ok;
}

static bool ipv6_wrong_address_passes( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );

    spec_t wrong_address = spec_default( true );
    wrong_address.destination_address = other_ipv6;
    ok &= run_frame( filter, "ipv6 other destination address", wrong_address, XDP_PASS, NO_COUNTER );

    // a destination that differs only in the first or last byte

    uint8_t first_byte[16];
    memcpy( first_byte, server_ipv6, 16 );
    first_byte[0] ^= 0x01;
    wrong_address.destination_address = first_byte;
    ok &= run_frame( filter, "ipv6 destination differing in the first byte", wrong_address, XDP_PASS, NO_COUNTER );

    spec_t wrong_port = spec_default( true );
    wrong_port.destination_port = server_port + 1;
    ok &= run_frame( filter, "ipv6 other destination port", wrong_port, XDP_PASS, NO_COUNTER );

    // the IPv4 destination in the config does not apply to IPv6 frames, and
    // the IPv6 one does not apply to IPv4 frames

    ok &= write_config( filter, server_ipv4, server_ipv6 );
    spec_t ipv4_wrong = spec_default( false );
    ipv4_wrong.destination_address = other_ipv4;
    ok &= run_frame( filter, "ipv4 other destination address", ipv4_wrong, XDP_PASS, NO_COUNTER );
    return ok;
}

static bool ipv6_short_header_drops( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES - 1;
    ok &= run_frame( filter, "ipv6 header cut one byte short", spec, XDP_DROP, NO_COUNTER );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + 1;
    ok &= run_frame( filter, "ipv6 header cut to one byte", spec, XDP_DROP, NO_COUNTER );
    return ok;
}

static bool ipv6_too_large_drops( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.packet_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    ok &= run_frame( filter, "ipv6 packet one byte over the ceiling", spec, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE );
    return ok;
}

static bool ipv6_icmpv6_passes( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.next_header = IPV6_ICMPV6;
    ok &= run_frame( filter, "icmpv6", spec, XDP_PASS, NO_COUNTER );

    // a neighbor solicitation sized frame and a bare header

    spec.packet_bytes = 24;
    ok &= run_frame( filter, "icmpv6 neighbor solicitation sized", spec, XDP_PASS, NO_COUNTER );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES;
    ok &= run_frame( filter, "icmpv6, fixed header only", spec, XDP_PASS, NO_COUNTER );
    return ok;
}

// The post-UDP checks are one function for both families; each drop reason
// has the same verdict and the same counter on IPv6 as on IPv4.

static bool ipv6_same_drops_as_ipv4( filter_t * filter )
{
    bool ok = true;
    for ( int family = 0; family < 2; family++ )
    {
        const bool ipv6 = family == 1;
        const char * name = ipv6 ? "ipv6" : "ipv4";
        char label[96];

        ok &= write_config( filter, server_ipv4, server_ipv6 );

        spec_t too_small = spec_default( ipv6 );
        too_small.packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1;
        snprintf( label, sizeof(label), "%s packet below the smallest type", name );
        ok &= run_frame( filter, label, too_small, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );

        spec_t wrong_epoch = spec_default( ipv6 );
        wrong_epoch.epoch = filter_epoch + 1;
        snprintf( label, sizeof(label), "%s wrong epoch", name );
        ok &= run_frame( filter, label, wrong_epoch, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH );

        spec_t wrong_filter = spec_default( ipv6 );
        wrong_filter.filter[3] ^= 0x80;
        snprintf( label, sizeof(label), "%s wrong filter value", name );
        ok &= run_frame( filter, label, wrong_filter, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER );

        spec_t denied = spec_default( ipv6 );
        denied.packet_type = ROCKETNET_CONNECTION_DENIED_PACKET;
        denied.packet_bytes = ROCKETNET_CONNECTION_DENIED_WIRE_BYTES;
        snprintf( label, sizeof(label), "%s server-to-client type", name );
        ok &= run_frame( filter, label, denied, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE );

        spec_t request_wrong_size = spec_default( ipv6 );
        request_wrong_size.packet_type = ROCKETNET_CONNECTION_REQUEST_PACKET;
        request_wrong_size.packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1;
        snprintf( label, sizeof(label), "%s connection request one byte short", name );
        ok &= run_frame( filter, label, request_wrong_size, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE );

        spec_t request = spec_default( ipv6 );
        request.packet_type = ROCKETNET_CONNECTION_REQUEST_PACKET;
        request.packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
        snprintf( label, sizeof(label), "%s connection request at its exact size", name );
        ok &= run_frame( filter, label, request, XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
    }
    return ok;
}

// the config map the header describes is the one the object was built with

static bool config_layout_matches_header( filter_t * filter )
{
    struct bpf_map * config_map = bpf_object__find_map_by_name( filter->object, "server_xdp_config_map" );
    struct bpf_map * stats_map = bpf_object__find_map_by_name( filter->object, "server_xdp_stats_map" );
    bool ok = true;
    if ( bpf_map__value_size( config_map ) != sizeof(struct rocketnet_server_xdp_config_t) )
    {
        printf( "  FAIL config map value is %u bytes, the header says %u\n", bpf_map__value_size( config_map ), (unsigned) sizeof(struct rocketnet_server_xdp_config_t) );
        ok = false;
    }
    if ( bpf_map__value_size( stats_map ) != sizeof(struct rocketnet_server_xdp_stats_t) )
    {
        printf( "  FAIL stats map value is %u bytes, the header says %u\n", bpf_map__value_size( stats_map ), (unsigned) sizeof(struct rocketnet_server_xdp_stats_t) );
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// replay: the read-only high-water map (tla/RNXdpReplay.tla, KernelFilter).
// The same rows as the host tier's xdp_host_replay_drops_behind_the_window,
// judged here by the verifier-accepted program, the kernel's hash map and
// its helpers. load_filter cleared every pin path, so the map is private to
// this process.

static int replay_map_fd( filter_t * filter )
{
    struct bpf_map * replay_map = bpf_object__find_map_by_name( filter->object, "server_xdp_replay_map" );
    return replay_map ? bpf_map__fd( replay_map ) : -1;
}

// the key of the client's address in a family, at a source port, built by
// the same function the transport's marks use

static struct rocketnet_server_xdp_replay_key_t replay_client_key( bool ipv6, uint16_t port )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof(address) );
    if ( !ipv6 )
    {
        address.type = ROCKETNET_ADDRESS_IPV4;
        memcpy( address.data.ipv4, client_ipv4, 4 );
    }
    else
    {
        address.type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; i++ )
            address.data.ipv6[i] = (uint16_t) ( ( client_ipv6[i * 2] << 8 ) | client_ipv6[i * 2 + 1] );
    }
    address.port = port;
    struct rocketnet_server_xdp_replay_key_t key;
    rocketnet_xdp_replay_key_from_address( &address, &key );
    return key;
}

static bool replay_clear( int fd )
{
    struct rocketnet_server_xdp_replay_key_t key;
    while ( bpf_map_get_next_key( fd, NULL, &key ) == 0 )
    {
        if ( bpf_map_delete_elem( fd, &key ) != 0 )
        {
            printf( "  FAIL could not delete a replay map entry: %s\n", strerror( errno ) );
            return false;
        }
    }
    return true;
}

static bool replay_write( int fd, bool ipv6, uint32_t high_water )
{
    const struct rocketnet_server_xdp_replay_key_t key = replay_client_key( ipv6, client_port );
    struct rocketnet_server_xdp_replay_value_t value;
    memset( &value, 0, sizeof(value) );
    value.high_water = high_water;
    if ( bpf_map_update_elem( fd, &key, &value, BPF_ANY ) != 0 )
    {
        printf( "  FAIL could not write the replay map: %s\n", strerror( errno ) );
        return false;
    }
    return true;
}

static bool replay_is_session_type( uint8_t type )
{
    return type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET ||
           type == ROCKETNET_CONNECTION_PAYLOAD_PACKET    ||
           type == ROCKETNET_CONNECTION_DISCONNECT_PACKET ||
           type == ROCKETNET_CONNECTION_ACK_PACKET        ||
           type == ROCKETNET_CONNECTION_FRAGMENT_PACKET;
}

struct replay_row_t
{
    char label[96];
    spec_t spec;
    bool entry;                 // the client's key at client_port holds high_water
    uint32_t high_water;
    int want_verdict;
    int want_counter;
};

static replay_row_t replay_row( const char * label, bool ipv6, uint8_t type, uint32_t sequence, uint32_t high_water, bool drops )
{
    replay_row_t row;
    memset( &row, 0, sizeof(row) );
    snprintf( row.label, sizeof(row.label), "%s %s", ipv6 ? "ipv6" : "ipv4", label );
    row.spec = spec_default( ipv6 );
    row.spec.packet_type = type;
    row.spec.packet_bytes = 100;
    if ( type == ROCKETNET_CONNECTION_REQUEST_PACKET )
        row.spec.packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    else if ( type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
        row.spec.packet_bytes = ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES;
    row.spec.sequence = sequence;
    row.entry = true;
    row.high_water = high_water;
    row.want_verdict = drops ? XDP_DROP : XDP_REDIRECT;
    row.want_counter = drops ? ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED : ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED;
    return row;
}

static void replay_rows( bool ipv6, std::vector<replay_row_t> & rows )
{
    rows.push_back( replay_row( "payload 16 behind the high-water", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100, true ) );
    rows.push_back( replay_row( "payload 15 behind the high-water", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 85, 100, false ) );
    rows.push_back( replay_row( "payload at the high-water", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 100, 100, false ) );
    rows.push_back( replay_row( "payload one ahead of the high-water", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 101, 100, false ) );

    rows.push_back( replay_row( "keep-alive 16 behind the high-water", ipv6, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 84, 100, true ) );
    rows.push_back( replay_row( "ack 16 behind the high-water", ipv6, ROCKETNET_CONNECTION_ACK_PACKET, 84, 100, true ) );
    rows.push_back( replay_row( "disconnect 16 behind the high-water", ipv6, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 84, 100, true ) );
    rows.push_back( replay_row( "fragment 16 behind the high-water", ipv6, ROCKETNET_CONNECTION_FRAGMENT_PACKET, 84, 100, true ) );

    // the handshake types are not checked
    rows.push_back( replay_row( "connection request at 84 is not checked", ipv6, ROCKETNET_CONNECTION_REQUEST_PACKET, 84, 100, false ) );
    rows.push_back( replay_row( "connection response at 84 is not checked", ipv6, ROCKETNET_CONNECTION_RESPONSE_PACKET, 84, 100, false ) );

    // another source port has no entry
    replay_row_t other = replay_row( "another source port has no entry", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100, false );
    other.spec.source_port = client_port + 1;
    rows.push_back( other );

    // no entry at all
    replay_row_t none = replay_row( "no replay entry", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100, false );
    none.entry = false;
    rows.push_back( none );

    // the wrap: the difference is modulo 2^32, read as signed
    rows.push_back( replay_row( "high-water 5, sequence 0xfffffff0 is behind across the wrap", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 0xfffffff0u, 5, true ) );
    rows.push_back( replay_row( "high-water 0xfffffff0, sequence 5 is ahead across the wrap", ipv6, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 5, 0xfffffff0u, false ) );
}

static bool replay_drops_behind_the_window( filter_t * filter )
{
    const int fd = replay_map_fd( filter );
    if ( fd < 0 )
    {
        printf( "  FAIL the program has no server_xdp_replay_map\n" );
        return false;
    }
    bool ok = write_config( filter, server_ipv4, server_ipv6 );
    std::vector<replay_row_t> rows;
    replay_rows( false, rows );
    replay_rows( true, rows );
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        ok &= replay_clear( fd );
        if ( rows[i].entry )
            ok &= replay_write( fd, rows[i].spec.ipv6, rows[i].high_water );
        ok &= run_frame( filter, rows[i].label, rows[i].spec, rows[i].want_verdict, rows[i].want_counter );
    }
    ok &= replay_clear( fd );
    return ok;
}

// The program never writes the map: both families' entries at high-water
// 100, then a forged payload far ahead and the frame of every row above
// through that same state, each verdict the rule's, and afterwards each
// entry still reads 100 and no key the test did not write exists.

static bool replay_never_writes_the_map( filter_t * filter )
{
    const int fd = replay_map_fd( filter );
    if ( fd < 0 )
    {
        printf( "  FAIL the program has no server_xdp_replay_map\n" );
        return false;
    }
    bool ok = write_config( filter, server_ipv4, server_ipv6 );
    ok &= replay_clear( fd );
    ok &= replay_write( fd, false, 100 );
    ok &= replay_write( fd, true, 100 );

    std::vector<spec_t> frames;
    for ( int family = 0; family < 2; family++ )
    {
        spec_t forged = spec_default( family == 1 );
        forged.packet_bytes = 100;
        forged.sequence = 0x7fffffffu;
        frames.push_back( forged );
        std::vector<replay_row_t> rows;
        replay_rows( family == 1, rows );
        for ( size_t i = 0; i < rows.size(); i++ )
            frames.push_back( rows[i].spec );
    }

    for ( size_t i = 0; i < frames.size(); i++ )
    {
        const spec_t & spec = frames[i];
        const bool checked = replay_is_session_type( spec.packet_type ) && spec.source_port == client_port;
        const bool behind = checked && (int32_t) ( 100u - spec.sequence ) >= ROCKETNET_XDP_REPLAY_WINDOW;
        char label[96];
        snprintf( label, sizeof(label), "%s frame %d, type %d, sequence 0x%08x, entry at 100",
                  spec.ipv6 ? "ipv6" : "ipv4", (int) i, spec.packet_type, spec.sequence );
        ok &= run_frame( filter, label, spec, behind ? XDP_DROP : XDP_REDIRECT,
                         behind ? ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED : ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
    }

    // each entry reads back as written
    for ( int family = 0; family < 2; family++ )
    {
        const struct rocketnet_server_xdp_replay_key_t key = replay_client_key( family == 1, client_port );
        struct rocketnet_server_xdp_replay_value_t value;
        memset( &value, 0, sizeof(value) );
        if ( bpf_map_lookup_elem( fd, &key, &value ) != 0 )
        {
            printf( "  FAIL the ipv%d entry is gone: %s\n", family == 1 ? 6 : 4, strerror( errno ) );
            ok = false;
        }
        else if ( value.high_water != 100 )
        {
            printf( "  FAIL the ipv%d entry reads %u, wrote 100\n", family == 1 ? 6 : 4, value.high_water );
            ok = false;
        }
    }

    // no key the test did not write
    const struct rocketnet_server_xdp_replay_key_t written[2] = { replay_client_key( false, client_port ), replay_client_key( true, client_port ) };
    struct rocketnet_server_xdp_replay_key_t key;
    int keys = 0;
    int result = bpf_map_get_next_key( fd, NULL, &key );
    while ( result == 0 )
    {
        keys++;
        if ( memcmp( &key, &written[0], sizeof(key) ) != 0 && memcmp( &key, &written[1], sizeof(key) ) != 0 )
        {
            printf( "  FAIL the map holds a key the test did not write (family %d)\n", key.family );
            ok = false;
        }
        struct rocketnet_server_xdp_replay_key_t next;
        result = bpf_map_get_next_key( fd, &key, &next );
        key = next;
    }
    if ( keys != 2 )
    {
        printf( "  FAIL the map holds %d keys, the test wrote 2\n", keys );
        ok = false;
    }

    ok &= replay_clear( fd );
    return ok;
}

// the replay map the header describes is the one the object was built with

static bool replay_key_layout_matches_header( filter_t * filter )
{
    struct bpf_map * replay_map = bpf_object__find_map_by_name( filter->object, "server_xdp_replay_map" );
    if ( replay_map == NULL )
    {
        printf( "  FAIL the program has no server_xdp_replay_map\n" );
        return false;
    }
    bool ok = true;
    if ( bpf_map__key_size( replay_map ) != sizeof(struct rocketnet_server_xdp_replay_key_t) )
    {
        printf( "  FAIL replay map key is %u bytes, the header says %u\n", bpf_map__key_size( replay_map ), (unsigned) sizeof(struct rocketnet_server_xdp_replay_key_t) );
        ok = false;
    }
    if ( bpf_map__value_size( replay_map ) != sizeof(struct rocketnet_server_xdp_replay_value_t) )
    {
        printf( "  FAIL replay map value is %u bytes, the header says %u\n", bpf_map__value_size( replay_map ), (unsigned) sizeof(struct rocketnet_server_xdp_replay_value_t) );
        ok = false;
    }
    return ok;
}

// ON-LINK REPLY: the one case that attaches a program and moves traffic, so it
// runs only inside a virtme-ng guest (it refuses on a host). A veth pair has
// its peer end in a network namespace with no default route; the example
// server runs with the xdp driver on the near end, the example client runs in
// the namespace, and every frame the server sends must carry the peer end's
// MAC as its destination. The namespace and the veth pair are removed on
// every exit path.

#define ONLINK_NAMESPACE "rnonlink"
#define ONLINK_NEAR "rnonlink0"
#define ONLINK_PEER "rnonlink1"
#define ONLINK_SERVER_ADDRESS "10.77.1.1:40000"
#define ONLINK_WANT_FRAMES 150
#define ONLINK_SECONDS 20.0

#ifndef BPF_FS_MAGIC
#define BPF_FS_MAGIC 0xcafe4a11
#endif

static bool onlink_run( const char * command )
{
    const int status = system( command );
    if ( status != 0 )
        printf( "  FAIL `%s` exited %d\n", command, status );
    return status == 0;
}

static double onlink_now()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool onlink_inside_vm()
{
    char line[4096] = { 0 };
    FILE * file = fopen( "/proc/cmdline", "r" );
    if ( !file )
        return false;
    const bool read = fgets( line, sizeof(line), file ) != NULL;
    fclose( file );
    return read && strstr( line, "virtme" ) != NULL;
}

static pid_t onlink_spawn( const char * program, const char * argument, const char * log_path, int netns_fd, bool xdp_server )
{
    const pid_t pid = fork();
    if ( pid != 0 )
        return pid;
    if ( netns_fd >= 0 && setns( netns_fd, CLONE_NEWNET ) != 0 )
        _exit( 126 );
    const int log_fd = open( log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    if ( log_fd >= 0 )
    {
        dup2( log_fd, 1 );
        dup2( log_fd, 2 );
        close( log_fd );
    }
    if ( xdp_server )
    {
        setenv( "ROCKETNET_SERVER_DRIVER", "xdp", 1 );
        setenv( "ROCKETNET_SERVER_NUM_XDP_QUEUES", "1", 1 );
        setenv( "ROCKETNET_SERVER_NUM_OS_CPUS", "1", 1 );
    }
    execl( program, program, argument, (char*) NULL );
    _exit( 127 );
}

static void onlink_stop( pid_t pid )
{
    if ( pid <= 0 )
        return;
    kill( pid, SIGINT );
    const double deadline = onlink_now() + 5.0;
    while ( onlink_now() < deadline )
    {
        if ( waitpid( pid, NULL, WNOHANG ) == pid )
            return;
        usleep( 10000 );
    }
    kill( pid, SIGKILL );
    waitpid( pid, NULL, 0 );
}

static bool onlink_log_has( const char * path, const char * text )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
        return false;
    char line[1024];
    bool found = false;
    while ( !found && fgets( line, sizeof(line), file ) )
        found = strstr( line, text ) != NULL;
    fclose( file );
    return found;
}

static void onlink_print_tail( const char * label, const char * path )
{
    char command[512];
    snprintf( command, sizeof(command), "tail -5 %s | sed 's/^/    %s: /'", path, label );
    if ( system( command ) != 0 )
        printf( "    %s: no log\n", label );
}

struct onlink_environment_t
{
    int home_netns_fd = -1;
    int peer_netns_fd = -1;
    int capture_fd = -1;
    pid_t server = -1;
    pid_t client = -1;
    char server_log[64] = "/tmp/rocketnet_onlink_server_XXXXXX";
    char client_log[64] = "/tmp/rocketnet_onlink_client_XXXXXX";

    ~onlink_environment_t()
    {
        onlink_stop( client );
        onlink_stop( server );
        if ( capture_fd >= 0 )
            close( capture_fd );
        if ( home_netns_fd >= 0 )
        {
            setns( home_netns_fd, CLONE_NEWNET );
            close( home_netns_fd );
        }
        if ( peer_netns_fd >= 0 )
            close( peer_netns_fd );
        if ( system( "ip netns del " ONLINK_NAMESPACE " 2>/dev/null; ip link del " ONLINK_NEAR " 2>/dev/null" ) != 0 )
        {
            // already gone: the namespace takes its veth end with it
        }
        unlink( server_log );
        unlink( client_log );
    }
};

static bool xdp_on_link_reply_reaches_the_peer( filter_t * )
{
    if ( !onlink_inside_vm() )
    {
        printf( "  FAIL this case attaches XDP to a veth pair and runs only inside a virtme-ng guest\n" );
        return false;
    }

    char exe[4096];
    const ssize_t exe_bytes = readlink( "/proc/self/exe", exe, sizeof(exe) - 1 );
    if ( exe_bytes <= 0 )
        return false;
    exe[exe_bytes] = '\0';
    char * slash = strrchr( exe, '/' );
    if ( !slash )
        return false;
    *slash = '\0';
    char server_path[4200], client_path[4200];
    snprintf( server_path, sizeof(server_path), "%s/server", exe );
    snprintf( client_path, sizeof(client_path), "%s/client", exe );

    onlink_environment_t environment;

    {
        const int server_log_fd = mkstemp( environment.server_log );
        const int client_log_fd = mkstemp( environment.client_log );
        if ( server_log_fd < 0 || client_log_fd < 0 )
            return false;
        close( server_log_fd );
        close( client_log_fd );
    }

    // the boot tier the xdp pre-flight verifies; a guest boots with an empty
    // bpffs mount point

    struct statfs bpffs;
    if ( statfs( "/sys/fs/bpf", &bpffs ) != 0 || (uint64_t) bpffs.f_type != (uint64_t) BPF_FS_MAGIC )
    {
        if ( !onlink_run( "mount -t bpf bpf /sys/fs/bpf" ) )
            return false;
    }

    if ( system( "ip netns del " ONLINK_NAMESPACE " 2>/dev/null; ip link del " ONLINK_NEAR " 2>/dev/null" ) != 0 )
    {
        // nothing left from an earlier run
    }

    if ( !onlink_run( "ip netns add " ONLINK_NAMESPACE ) ||
         !onlink_run( "ip link add " ONLINK_NEAR " type veth peer name " ONLINK_PEER ) ||
         !onlink_run( "ip link set " ONLINK_PEER " netns " ONLINK_NAMESPACE ) ||
         !onlink_run( "ip addr add 10.77.1.1/24 dev " ONLINK_NEAR ) ||
         !onlink_run( "ip link set " ONLINK_NEAR " up" ) ||
         !onlink_run( "echo 2 > /sys/class/net/" ONLINK_NEAR "/napi_defer_hard_irqs" ) ||
         !onlink_run( "echo 200000 > /sys/class/net/" ONLINK_NEAR "/gro_flush_timeout" ) ||
         !onlink_run( "ip netns exec " ONLINK_NAMESPACE " ip addr add 10.77.1.2/24 dev " ONLINK_PEER ) ||
         !onlink_run( "ip netns exec " ONLINK_NAMESPACE " ip link set " ONLINK_PEER " up" ) ||
         !onlink_run( "ip netns exec " ONLINK_NAMESPACE " ip link set lo up" ) )
        return false;

    environment.home_netns_fd = open( "/proc/self/ns/net", O_RDONLY );
    environment.peer_netns_fd = open( "/var/run/netns/" ONLINK_NAMESPACE, O_RDONLY );
    if ( environment.home_netns_fd < 0 || environment.peer_netns_fd < 0 )
    {
        printf( "  FAIL could not open the network namespaces\n" );
        return false;
    }

    // the peer end's MAC and a packet socket on it, opened inside the namespace

    uint8_t peer_mac[6];
    int peer_index = 0;
    {
        if ( setns( environment.peer_netns_fd, CLONE_NEWNET ) != 0 )
            return false;
        const int probe = socket( AF_INET, SOCK_DGRAM, 0 );
        struct ifreq ifr;
        memset( &ifr, 0, sizeof(ifr) );
        strncpy( ifr.ifr_name, ONLINK_PEER, sizeof(ifr.ifr_name) - 1 );
        const bool have_mac = probe >= 0 && ioctl( probe, SIOCGIFHWADDR, &ifr ) == 0;
        if ( have_mac )
            memcpy( peer_mac, ifr.ifr_hwaddr.sa_data, 6 );
        if ( probe >= 0 )
            close( probe );
        peer_index = (int) if_nametoindex( ONLINK_PEER );
        environment.capture_fd = socket( AF_PACKET, SOCK_RAW, htons( ETH_P_IP ) );
        if ( environment.capture_fd >= 0 )
        {
            struct sockaddr_ll bind_address;
            memset( &bind_address, 0, sizeof(bind_address) );
            bind_address.sll_family = AF_PACKET;
            bind_address.sll_protocol = htons( ETH_P_IP );
            bind_address.sll_ifindex = peer_index;
            if ( bind( environment.capture_fd, (struct sockaddr*) &bind_address, sizeof(bind_address) ) != 0 )
            {
                close( environment.capture_fd );
                environment.capture_fd = -1;
            }
        }
        if ( setns( environment.home_netns_fd, CLONE_NEWNET ) != 0 )
            return false;
        if ( !have_mac || peer_index == 0 || environment.capture_fd < 0 )
        {
            printf( "  FAIL could not read the peer end's MAC or open a packet socket on it\n" );
            return false;
        }
    }

    environment.server = onlink_spawn( server_path, ONLINK_SERVER_ADDRESS, environment.server_log, -1, true );
    environment.client = onlink_spawn( client_path, ONLINK_SERVER_ADDRESS, environment.client_log, environment.peer_netns_fd, false );

    // every frame from the server's address and port, judged by its
    // destination MAC

    int frames_to_peer = 0;
    int frames_elsewhere = 0;
    const double deadline = onlink_now() + ONLINK_SECONDS;
    while ( frames_to_peer < ONLINK_WANT_FRAMES && onlink_now() < deadline )
    {
        struct pollfd fds;
        fds.fd = environment.capture_fd;
        fds.events = POLLIN;
        fds.revents = 0;
        if ( poll( &fds, 1, 100 ) <= 0 )
            continue;
        uint8_t frame[2048];
        const ssize_t bytes = recv( environment.capture_fd, frame, sizeof(frame), 0 );
        if ( bytes < 14 + 20 + 8 )
            continue;
        if ( frame[12] != 0x08 || frame[13] != 0x00 || frame[23] != IP_UDP )
            continue;
        static const uint8_t server_ipv4[4] = { 10, 77, 1, 1 };
        const int ihl = ( frame[14] & 0x0F ) * 4;
        const uint16_t source_port = (uint16_t) ( ( frame[14 + ihl] << 8 ) | frame[14 + ihl + 1] );
        if ( memcmp( frame + 26, server_ipv4, 4 ) != 0 || source_port != 40000 )
            continue;
        if ( memcmp( frame, peer_mac, 6 ) == 0 )
        {
            frames_to_peer++;
        }
        else
        {
            if ( frames_elsewhere == 0 )
                printf( "  FAIL a reply went to %02x:%02x:%02x:%02x:%02x:%02x, not the peer's %02x:%02x:%02x:%02x:%02x:%02x\n",
                    frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                    peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5] );
            frames_elsewhere++;
        }
    }

    onlink_stop( environment.client );
    environment.client = -1;
    onlink_stop( environment.server );
    environment.server = -1;

    const bool server_was_xdp = onlink_log_has( environment.server_log, "server transport started on " ONLINK_SERVER_ADDRESS " [xdp]" );
    const bool client_connected = onlink_log_has( environment.client_log, "client state: connected" );
    const bool exchanged = onlink_log_has( environment.client_log, "client sent " );

    bool ok = true;
    if ( !server_was_xdp )
    {
        printf( "  FAIL the server did not start with the xdp driver on " ONLINK_NEAR "\n" );
        ok = false;
    }
    if ( !client_connected || !exchanged )
    {
        printf( "  FAIL the client did not connect and exchange payloads (connected %d, exchanged %d)\n", client_connected ? 1 : 0, exchanged ? 1 : 0 );
        ok = false;
    }
    if ( frames_to_peer < ONLINK_WANT_FRAMES || frames_elsewhere != 0 )
    {
        printf( "  FAIL %d frames reached the peer's MAC (want %d) and %d went elsewhere (want 0)\n", frames_to_peer, ONLINK_WANT_FRAMES, frames_elsewhere );
        ok = false;
    }
    if ( !ok )
    {
        onlink_print_tail( "server", environment.server_log );
        onlink_print_tail( "client", environment.client_log );
    }
    return ok;
}

struct case_t
{
    const char * name;
    case_function_t function;
};

static const case_t cases[] =
{
    { "ipv4_rocketnet_packet_redirects", ipv4_rocketnet_packet_redirects },
    { "ipv6_rocketnet_packet_redirects", ipv6_rocketnet_packet_redirects },
    { "ipv6_extension_header_passes", ipv6_extension_header_passes },
    { "ipv6_fragment_passes", ipv6_fragment_passes },
    { "ipv6_wrong_address_passes", ipv6_wrong_address_passes },
    { "ipv6_short_header_drops", ipv6_short_header_drops },
    { "ipv6_too_large_drops", ipv6_too_large_drops },
    { "ipv6_icmpv6_passes", ipv6_icmpv6_passes },
    { "ipv6_same_drops_as_ipv4", ipv6_same_drops_as_ipv4 },
    { "config_layout_matches_header", config_layout_matches_header },
    { "replay_drops_behind_the_window", replay_drops_behind_the_window },
    { "replay_never_writes_the_map", replay_never_writes_the_map },
    { "replay_key_layout_matches_header", replay_key_layout_matches_header },
    { "xdp_on_link_reply_reaches_the_peer", xdp_on_link_reply_reaches_the_peer },
};

int main( int argc, char ** argv )
{
    const char * path = ROCKETNET_XDP_FILTER_OBJECT;
    bool show_verifier = false;
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "-v" ) == 0 )
            show_verifier = true;
        else
            path = argv[i];
    }

    filter_t filter;
    if ( !load_filter( &filter, path ) )
        return 2;

    // the last line of the log is the verifier's verdict on the program

    if ( show_verifier )
    {
        const size_t length = strnlen( verifier_log, sizeof(verifier_log) );
        size_t end = length;
        while ( end > 0 && ( verifier_log[end - 1] == '\n' || verifier_log[end - 1] == ' ' ) )
            end--;
        size_t start = end;
        while ( start > 0 && verifier_log[start - 1] != '\n' )
            start--;
        printf( "verifier: %.*s\n", (int) ( end - start ), verifier_log + start );
    }

    int failed = 0;
    for ( int i = 0; i < (int) ( sizeof(cases) / sizeof(cases[0]) ); i++ )
    {
        const bool ok = cases[i].function( &filter );
        printf( "%s: %s\n", cases[i].name, ok ? "ok" : "FAILED" );
        if ( !ok )
            failed++;
    }

    bpf_object__close( filter.object );

    if ( failed )
    {
        printf( "%d of %d cases failed\n", failed, (int) ( sizeof(cases) / sizeof(cases[0]) ) );
        return 1;
    }

    printf( "test_xdp_filter: %d cases passed\n", (int) ( sizeof(cases) / sizeof(cases[0]) ) );
    return 0;
}
