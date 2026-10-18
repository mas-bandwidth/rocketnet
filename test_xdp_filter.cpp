/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The XDP receive filter, judged by the kernel.
//
// Loads server_xdp/server_xdp.c through the verifier and runs crafted frames
// through it with BPF_PROG_TEST_RUN: the kernel returns the program's verdict
// on each frame and transmits nothing. The filter cases involve no socket, no
// interface and no packet traffic, and no XDP program is attached anywhere.
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
// One case binds a real AF_XDP socket, and only inside a virtme-ng guest:
// xsk_bind_falls_back_to_copy creates its own veth pair in the guest, binds on
// it with the server's bind config (sdk/rocketnet_xdp_socket_config.h), sees
// the zero-copy bind refused (veth has no zero-copy support) and the copy bind
// succeed, reads the bound mode back, and removes the pair. Outside a guest the
// case skips with its reason and creates nothing, so no host interface is touched.
//
// -v prints the last line of the verifier's log for the load. Exit 0: every case passed or skipped with its reason. Exit 1: a case failed. Exit 2: the object did
// not load (the verifier log tail is printed) or the process lacks privilege.

#include "rocketnet_wire.h"
#include "rocketnet_xdp_replay_marks.h"
#include "rocketnet_xdp_socket_config.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <net/if.h>
#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

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

// The AF_XDP bind on a veth pair the case owns, inside the virtme-ng guest only.

static const char * zc_veth = "rnxsk0";
static const char * case_skip_reason = NULL;
static const char * zc_veth_peer = "rnxsk1";

static bool inside_virtme_guest()
{
    FILE * file = fopen( "/proc/cmdline", "r" );
    if ( !file )
        return false;
    char line[4096];
    const bool read = fgets( line, sizeof(line), file ) != NULL;
    fclose( file );
    return read && strstr( line, "virtme" ) != NULL;
}

// One AF_XDP socket with its umem and rings, bound with the config's bind
// flags on queue 0. Returns true when the bind succeeds and fills zero_copy
// from getsockopt(SOL_XDP, XDP_OPTIONS); else errno_out holds the refusal.

static bool xsk_raw_bind( int ifindex, const struct xsk_socket_config & config, bool * zero_copy, int * errno_out )
{
    const size_t frame_size = 4096;
    const size_t frames = 64;
    const int ring_size = 64;

    *zero_copy = false;
    *errno_out = 0;

    void * area = mmap( NULL, frame_size * frames, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if ( area == MAP_FAILED )
    {
        *errno_out = errno;
        return false;
    }

    bool bound = false;
    int fd = socket( AF_XDP, SOCK_RAW, 0 );
    if ( fd < 0 )
    {
        *errno_out = errno;
        munmap( area, frame_size * frames );
        return false;
    }

    struct xdp_umem_reg reg;
    memset( &reg, 0, sizeof(reg) );
    reg.addr = (uint64_t) (uintptr_t) area;
    reg.len = frame_size * frames;
    reg.chunk_size = (uint32_t) frame_size;
    reg.headroom = 0;

    const int fill_size = ring_size, complete_size = ring_size;
    const int rx_size = (int) config.rx_size, tx_size = (int) config.tx_size;

    if ( setsockopt( fd, SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg) ) != 0 ||
         setsockopt( fd, SOL_XDP, XDP_UMEM_FILL_RING, &fill_size, sizeof(fill_size) ) != 0 ||
         setsockopt( fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &complete_size, sizeof(complete_size) ) != 0 ||
         setsockopt( fd, SOL_XDP, XDP_RX_RING, &rx_size, sizeof(rx_size) ) != 0 ||
         setsockopt( fd, SOL_XDP, XDP_TX_RING, &tx_size, sizeof(tx_size) ) != 0 )
    {
        *errno_out = errno;
        printf( "  socket setup refused: %s\n", strerror( *errno_out ) );
    }
    else
    {
        struct sockaddr_xdp address;
        memset( &address, 0, sizeof(address) );
        address.sxdp_family = AF_XDP;
        address.sxdp_ifindex = (uint32_t) ifindex;
        address.sxdp_queue_id = 0;
        address.sxdp_flags = config.bind_flags;

        if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) != 0 )
        {
            *errno_out = errno;
        }
        else
        {
            struct xdp_options options;
            memset( &options, 0, sizeof(options) );
            socklen_t length = sizeof(options);
            if ( getsockopt( fd, SOL_XDP, XDP_OPTIONS, &options, &length ) != 0 )
            {
                *errno_out = errno;
                printf( "  getsockopt XDP_OPTIONS refused: %s\n", strerror( *errno_out ) );
            }
            else
            {
                *zero_copy = ( options.flags & XDP_OPTIONS_ZEROCOPY ) != 0;
                bound = true;
            }
        }
    }

    close( fd );
    munmap( area, frame_size * frames );
    return bound;
}

static bool xsk_bind_on_veth( const char * name )
{
    const int ifindex = (int) if_nametoindex( name );
    if ( ifindex == 0 )
    {
        printf( "  %s has no ifindex\n", name );
        return false;
    }

    struct xsk_socket_config config;

    // the first attempt asks for zero copy: veth refuses it

    rocketnet_xdp_socket_config( &config, 64, 64, true );
    if ( config.xdp_flags != 0 || ( config.bind_flags & XDP_ZEROCOPY ) == 0 )
    {
        printf( "  the zero copy config does not ask for zero copy at bind (xdp_flags %u bind_flags %u)\n", config.xdp_flags, config.bind_flags );
        return false;
    }

    bool zero_copy = false;
    int refusal = 0;
    if ( xsk_raw_bind( ifindex, config, &zero_copy, &refusal ) )
    {
        printf( "  the zero copy bind on %s succeeded (zero_copy %d); veth should refuse it\n", name, zero_copy ? 1 : 0 );
        return false;
    }
    printf( "  zero copy bind on %s refused: %s\n", name, strerror( refusal ) );

    // the fallback asks for copy and binds

    rocketnet_xdp_socket_config( &config, 64, 64, false );
    if ( config.xdp_flags != 0 || ( config.bind_flags & XDP_COPY ) == 0 || ( config.bind_flags & XDP_ZEROCOPY ) != 0 )
    {
        printf( "  the fallback config does not ask for copy at bind (xdp_flags %u bind_flags %u)\n", config.xdp_flags, config.bind_flags );
        return false;
    }

    if ( !xsk_raw_bind( ifindex, config, &zero_copy, &refusal ) )
    {
        printf( "  the copy bind on %s was refused: %s\n", name, strerror( refusal ) );
        return false;
    }

    printf( "  copy bind on %s bound; XDP_OPTIONS reads %s\n", name, zero_copy ? "zero copy" : "copy" );
    return !zero_copy;
}

static bool xsk_bind_falls_back_to_copy( filter_t * )
{
    if ( !inside_virtme_guest() )
    {
        case_skip_reason = "not inside a virtme-ng guest: this case creates a veth pair and runs only in the VM";
        return true;
    }

    // a failed add means the pair is not ours (the name may already be taken), so nothing is removed

    char command[256];
    snprintf( command, sizeof(command), "ip link add %s type veth peer name %s", zc_veth, zc_veth_peer );
    if ( system( command ) != 0 )
    {
        printf( "  could not create the veth pair %s/%s; nothing removed\n", zc_veth, zc_veth_peer );
        return false;
    }

    // from here the pair is ours and is removed on every exit path

    snprintf( command, sizeof(command), "ip link set %s up && ip link set %s up", zc_veth, zc_veth_peer );
    const bool up = system( command ) == 0;
    if ( !up )
        printf( "  could not bring up the veth pair %s/%s\n", zc_veth, zc_veth_peer );

    const bool ok = up && xsk_bind_on_veth( zc_veth );

    // deleting one end removes the pair

    snprintf( command, sizeof(command), "ip link del %s", zc_veth );
    const bool removed = system( command ) == 0;
    if ( !removed )
        printf( "  could not remove the veth pair %s/%s\n", zc_veth, zc_veth_peer );

    return ok && removed;
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
    { "xsk_bind_falls_back_to_copy", xsk_bind_falls_back_to_copy },
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
    int skipped = 0;
    for ( int i = 0; i < (int) ( sizeof(cases) / sizeof(cases[0]) ); i++ )
    {
        case_skip_reason = NULL;
        const bool ok = cases[i].function( &filter );
        if ( ok && case_skip_reason )
        {
            printf( "%s: skipped (%s)\n", cases[i].name, case_skip_reason );
            skipped++;
            continue;
        }
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

    const int total = (int) ( sizeof(cases) / sizeof(cases[0]) );
    if ( skipped )
        printf( "test_xdp_filter: %d cases passed, %d skipped\n", total - skipped, skipped );
    else
        printf( "test_xdp_filter: %d cases passed\n", total );
    return 0;
}
