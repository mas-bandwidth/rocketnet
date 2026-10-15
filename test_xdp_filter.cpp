/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The XDP receive filter, judged by the kernel.
//
// Loads server_xdp/server_xdp.c through the verifier and runs crafted frames
// through it with BPF_PROG_TEST_RUN: the kernel returns the program's verdict
// on each frame and transmits nothing. No socket, no interface and no packet
// traffic is involved, and no XDP program is attached anywhere.
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
//     sudo ./bin/test_xdp_filter [-v] [--base base_server_xdp.o] [server_xdp.o]
//
// -v prints the last line of the verifier's log for the load. --base names an
// object built from an earlier server_xdp.c (with its own rocketnet_wire.h);
// verdicts_unchanged_from_base runs the same frames through both and fails
// without it. Exit 0: every case passed. Exit 1: a case failed. Exit 2: the
// object did not load (the verifier log tail is printed) or the process lacks
// privilege.
//
// Every frame raises RECEIVED and exactly one other counter; run_frame
// asserts that shape on every row, and received_equals_the_sum_of_verdicts
// checks conservation over the whole run.

#include "rocketnet_wire.h"

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
    int ethertype;                              // -1 = the family's own
    uint8_t ipv4_ihl;                           // IPv4 only: the header length nibble
    uint8_t ipv4_protocol;                      // IPv4 only: the protocol byte
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
    spec.ethertype = -1;
    spec.ipv4_ihl = 5;
    spec.ipv4_protocol = IP_UDP;
    return spec;
}

static void put16( uint8_t * p, uint16_t value )
{
    p[0] = (uint8_t) ( value >> 8 );
    p[1] = (uint8_t) value;
}

// Ethernet, then the IP header for the family, then any extension header,
// then UDP, then the RocketNet packet: the prefilter header (type, epoch,
// filter value) and zeros after it.

static void build_frame( frame_t * frame, const spec_t & spec )
{
    memset( frame, 0, sizeof(*frame) );
    uint8_t * p = frame->data;

    memset( p, 0x02, 6 );                       // destination mac
    memset( p + 6, 0x04, 6 );                   // source mac
    put16( p + 12, spec.ethertype >= 0 ? (uint16_t) spec.ethertype : ( spec.ipv6 ? ETH_P_IPV6 : ETH_P_IP ) );
    p += ROCKETNET_ETHERNET_HEADER_BYTES;

    const int udp_bytes = ROCKETNET_UDP_HEADER_BYTES + spec.packet_bytes;
    const bool has_extension = spec.ipv6 && spec.next_header >= 0 && spec.next_header != IPV6_ICMPV6;

    if ( !spec.ipv6 )
    {
        const uint8_t * destination = spec.destination_address ? spec.destination_address : server_ipv4;
        p[0] = (uint8_t) ( 0x40 | ( spec.ipv4_ihl & 0x0F ) );     // version 4, ihl 5 unless the spec says otherwise
        put16( p + 2, (uint16_t) ( ROCKETNET_IPV4_HEADER_BYTES + udp_bytes ) );
        p[8] = 64;                              // ttl
        p[9] = spec.ipv4_protocol;
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

    put16( p, 50000 );                          // source port
    put16( p + 2, spec.destination_port );
    put16( p + 4, (uint16_t) udp_bytes );
    p += ROCKETNET_UDP_HEADER_BYTES;

    if ( spec.packet_bytes > 0 )
        p[0] = spec.packet_type;
    if ( spec.packet_bytes > 1 )
        p[1] = spec.epoch;
    if ( spec.packet_bytes > 2 + ROCKETNET_FILTER_VALUE_BYTES - 1 )
        memcpy( p + 2, spec.filter, ROCKETNET_FILTER_VALUE_BYTES );
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
    int socket_map_fd;
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
    filter->socket_map_fd = bpf_map__fd( socket_map );

    // slot 0 of the redirect map is the loopback interface: the program
    // asks for the receive queue, which test_run always reports as 0

    const __u32 key = 0;
    const __u32 loopback = if_nametoindex( "lo" );
    if ( loopback == 0 || bpf_map_update_elem( filter->socket_map_fd, &key, &loopback, BPF_ANY ) != 0 )
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

// The counter table from rocketnet_wire.h: the name of each slot, by index.

struct counter_row_t
{
    int index;
    const char * name;
    const char * kind;
};

#define COUNTER_ROW( define, name, kind ) { define, name, kind },

static const counter_row_t counter_rows[] = { ROCKETNET_SERVER_XDP_COUNTER_TABLE( COUNTER_ROW ) };

static const char * counter_name( int index )
{
    if ( index < 0 || index >= NUM_COUNTER_SLOTS )
        return "?";
    return counter_rows[index].name;
}

// Run one frame through the program and return its verdict, or -1 when the
// kernel refused the run.

static int run_verdict( filter_t * filter, const spec_t & spec )
{
    frame_t frame;
    build_frame( &frame, spec );

    static uint8_t out[4096];
    struct bpf_test_run_opts options;
    memset( &options, 0, sizeof(options) );
    options.sz = sizeof(options);
    options.data_in = frame.data;
    options.data_size_in = (__u32) frame.size;
    options.data_out = out;
    options.data_size_out = sizeof(out);
    options.repeat = 1;

    if ( bpf_prog_test_run_opts( filter->program_fd, &options ) != 0 )
        return -1;
    return (int) options.retval;
}

// Run one frame and read the counters on both sides of it. *moved is the one
// counter other than RECEIVED that moved, -1 when none did, and -2 when the
// deltas are not RECEIVED +1 and exactly one other counter +1.

static int run_counted( filter_t * filter, const spec_t & spec, int * moved )
{
    uint64_t before[NUM_COUNTER_SLOTS];
    read_counters( filter, before );

    const int verdict = run_verdict( filter, spec );

    uint64_t after[NUM_COUNTER_SLOTS];
    read_counters( filter, after );

    *moved = -1;
    bool shape = after[ROCKETNET_SERVER_XDP_COUNTER_RECEIVED] - before[ROCKETNET_SERVER_XDP_COUNTER_RECEIVED] == 1;
    for ( int i = 0; i < NUM_COUNTER_SLOTS; i++ )
    {
        if ( i == ROCKETNET_SERVER_XDP_COUNTER_RECEIVED || after[i] == before[i] )
            continue;
        if ( after[i] - before[i] != 1 || *moved != -1 )
            shape = false;
        *moved = i;
    }
    if ( !shape )
        *moved = -2;
    return verdict;
}

// Run one frame. RECEIVED goes up by one, the named counter goes up by one,
// and every other counter stays where it was.

static bool run_frame( filter_t * filter, const char * label, const spec_t & spec, int want_verdict, int want_counter )
{
    uint64_t before[NUM_COUNTER_SLOTS];
    read_counters( filter, before );

    const int verdict = run_verdict( filter, spec );
    if ( verdict < 0 )
    {
        printf( "  FAIL %s: test_run failed: %s\n", label, strerror( errno ) );
        return false;
    }

    uint64_t after[NUM_COUNTER_SLOTS];
    read_counters( filter, after );

    bool ok = verdict == want_verdict;
    if ( !ok )
        printf( "  FAIL %s: verdict %s, wanted %s\n", label, verdict_name( verdict ), verdict_name( want_verdict ) );

    for ( int i = 0; i < NUM_COUNTER_SLOTS; i++ )
    {
        const uint64_t want = ( i == want_counter || i == ROCKETNET_SERVER_XDP_COUNTER_RECEIVED ) ? 1 : 0;
        if ( after[i] - before[i] != want )
        {
            printf( "  FAIL %s: %s moved by %llu, wanted %llu\n", label, counter_name( i ),
                    (unsigned long long) ( after[i] - before[i] ), (unsigned long long) want );
            ok = false;
        }
    }
    return ok;
}

static bool write_config_unconfigured( filter_t * filter )
{
    struct rocketnet_server_xdp_config_t config;
    memset( &config, 0, sizeof(config) );
    const __u32 key = 0;
    if ( bpf_map_update_elem( filter->config_fd, &key, &config, BPF_ANY ) != 0 )
    {
        fprintf( stderr, "could not write the config map: %s\n", strerror( errno ) );
        return false;
    }
    return true;
}

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
    ok &= run_frame( filter, "ipv6 other destination address", wrong_address, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );

    // a destination that differs only in the first or last byte

    uint8_t first_byte[16];
    memcpy( first_byte, server_ipv6, 16 );
    first_byte[0] ^= 0x01;
    wrong_address.destination_address = first_byte;
    ok &= run_frame( filter, "ipv6 destination differing in the first byte", wrong_address, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );

    spec_t wrong_port = spec_default( true );
    wrong_port.destination_port = server_port + 1;
    ok &= run_frame( filter, "ipv6 other destination port", wrong_port, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );

    // the IPv4 destination in the config does not apply to IPv6 frames, and
    // the IPv6 one does not apply to IPv4 frames

    ok &= write_config( filter, server_ipv4, server_ipv6 );
    spec_t ipv4_wrong = spec_default( false );
    ipv4_wrong.destination_address = other_ipv4;
    ok &= run_frame( filter, "ipv4 other destination address", ipv4_wrong, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );
    return ok;
}

static bool ipv6_short_header_drops( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES - 1;
    ok &= run_frame( filter, "ipv6 header cut one byte short", spec, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV6_SHORT_HEADER );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + 1;
    ok &= run_frame( filter, "ipv6 header cut to one byte", spec, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV6_SHORT_HEADER );
    return ok;
}

static bool ipv6_too_large_drops( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.packet_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    ok &= run_frame( filter, "ipv6 packet one byte over the ceiling", spec, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_LARGE );
    return ok;
}

static bool ipv6_icmpv6_passes( filter_t * filter )
{
    bool ok = write_config( filter, NULL, server_ipv6 );
    spec_t spec = spec_default( true );
    spec.next_header = IPV6_ICMPV6;
    ok &= run_frame( filter, "icmpv6", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP );

    // a neighbor solicitation sized frame and a bare header

    spec.packet_bytes = 24;
    ok &= run_frame( filter, "icmpv6 neighbor solicitation sized", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP );
    spec.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES;
    ok &= run_frame( filter, "icmpv6, fixed header only", spec, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP );
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
        snprintf( label, sizeof(label), "%s payload below the smallest type", name );
        ok &= run_frame( filter, label, too_small, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PAYLOAD_WRONG_SIZE );

        spec_t wrong_epoch = spec_default( ipv6 );
        wrong_epoch.epoch = filter_epoch + 1;
        snprintf( label, sizeof(label), "%s wrong epoch", name );
        ok &= run_frame( filter, label, wrong_epoch, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_EPOCH );

        spec_t wrong_filter = spec_default( ipv6 );
        wrong_filter.filter[3] ^= 0x80;
        snprintf( label, sizeof(label), "%s wrong filter value", name );
        ok &= run_frame( filter, label, wrong_filter, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_VALUE );

        spec_t denied = spec_default( ipv6 );
        denied.packet_type = ROCKETNET_CONNECTION_DENIED_PACKET;
        denied.packet_bytes = ROCKETNET_CONNECTION_DENIED_WIRE_BYTES;
        snprintf( label, sizeof(label), "%s server-to-client type", name );
        ok &= run_frame( filter, label, denied, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION );

        spec_t request_wrong_size = spec_default( ipv6 );
        request_wrong_size.packet_type = ROCKETNET_CONNECTION_REQUEST_PACKET;
        request_wrong_size.packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1;
        snprintf( label, sizeof(label), "%s connection request one byte short", name );
        ok &= run_frame( filter, label, request_wrong_size, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_REQUEST_WRONG_SIZE );

        spec_t request = spec_default( ipv6 );
        request.packet_type = ROCKETNET_CONNECTION_REQUEST_PACKET;
        request.packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
        snprintf( label, sizeof(label), "%s connection request at its exact size", name );
        ok &= run_frame( filter, label, request, XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
    }
    return ok;
}

// Every exit of the program raises RECEIVED and exactly one more counter: one
// frame per exit path, for IPv4 and IPv6 where both have the path. The
// redirect that finds no socket is redirect_without_a_socket_is_counted.

struct exit_row_t
{
    const char * label;
    int verdict;
    int counter;
};

static bool every_verdict_is_counted( filter_t * filter )
{
    bool ok = write_config( filter, server_ipv4, server_ipv6 );

    // the paths in front of any address family

    spec_t ethernet_short = spec_default( false );
    ethernet_short.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES - 1;
    ok &= run_frame( filter, "frame shorter than an ethernet header", ethernet_short, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_SHORT_ETHERNET );

    spec_t arp = spec_default( false );
    arp.ethertype = 0x0806;
    ok &= run_frame( filter, "ethertype is not ip", arp, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_IP );

    // the paths one family has

    spec_t ipv4_short = spec_default( false );
    ipv4_short.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV4_HEADER_BYTES - 1;
    ok &= run_frame( filter, "ipv4 header cut one byte short", ipv4_short, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV4_SHORT_HEADER );

    spec_t ipv6_short = spec_default( true );
    ipv6_short.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES - 1;
    ok &= run_frame( filter, "ipv6 header cut one byte short", ipv6_short, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IPV6_SHORT_HEADER );

    spec_t ipv4_options = spec_default( false );
    ipv4_options.ipv4_ihl = 6;
    ok &= run_frame( filter, "ipv4 header with options", ipv4_options, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV4_NOT_PLAIN_UDP );

    spec_t ipv4_tcp = spec_default( false );
    ipv4_tcp.ipv4_protocol = 6;
    ok &= run_frame( filter, "ipv4 protocol is not udp", ipv4_tcp, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV4_NOT_PLAIN_UDP );

    spec_t ipv6_icmp = spec_default( true );
    ipv6_icmp.next_header = IPV6_ICMPV6;
    ok &= run_frame( filter, "ipv6 next header is neither udp nor an extension", ipv6_icmp, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_NOT_UDP );

    spec_t ipv6_extension = spec_default( true );
    ipv6_extension.next_header = IPV6_HOP_BY_HOP;
    ok &= run_frame( filter, "ipv6 extension header", ipv6_extension, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );

    // the paths both families share: everything from the UDP header on

    struct type_row_t
    {
        uint8_t type;
        int bytes;
        int counter;
        const char * label;
    };

    static const type_row_t type_rows[] =
    {
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET,      ROCKETNET_PREFILTER_BYTES - 1,                          ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_SHORT,              "packet shorter than the prefilter header" },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET,      ROCKETNET_RECEIVE_CEILING + 1,                          ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_TOO_LARGE,              "packet one byte over the ceiling" },
        { ROCKETNET_CONNECTION_FRAGMENT_PACKET + 1, ROCKETNET_RECEIVE_CEILING,                              ROCKETNET_SERVER_XDP_COUNTER_RESERVED_TYPE,                    "reserved type" },
        { ROCKETNET_CONNECTION_DENIED_PACKET,       ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,                 ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION,                  "denied" },
        { ROCKETNET_CONNECTION_CHALLENGE_PACKET,    ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,              ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION,                  "challenge" },
        { ROCKETNET_CONNECTION_COOKIE_PACKET,       ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES,                 ROCKETNET_SERVER_XDP_COUNTER_WRONG_DIRECTION,                  "cookie" },
        { ROCKETNET_CONNECTION_REQUEST_PACKET,      ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 1,            ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_REQUEST_WRONG_SIZE,    "connection request one byte long" },
        { ROCKETNET_CONNECTION_RESPONSE_PACKET,     ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES - 1,           ROCKETNET_SERVER_XDP_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE,   "connection response one byte short" },
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,   ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - 1,     ROCKETNET_SERVER_XDP_COUNTER_KEEP_ALIVE_WRONG_SIZE,            "keep alive below its floor" },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET,      ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES - 1,        ROCKETNET_SERVER_XDP_COUNTER_PAYLOAD_WRONG_SIZE,               "payload below its floor" },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET,   ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES - 1,     ROCKETNET_SERVER_XDP_COUNTER_DISCONNECT_WRONG_SIZE,            "disconnect below its floor" },
        { ROCKETNET_CONNECTION_ACK_PACKET,          ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES - 1,            ROCKETNET_SERVER_XDP_COUNTER_ACK_WRONG_SIZE,                   "ack below its floor" },
        { ROCKETNET_CONNECTION_FRAGMENT_PACKET,     ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES - 1,       ROCKETNET_SERVER_XDP_COUNTER_FRAGMENT_WRONG_SIZE,              "fragment below its floor" },
        { ROCKETNET_CONNECTION_PAYLOAD_PACKET,      ROCKETNET_RECEIVE_CEILING,                              ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED,                       "accepted packet" },
    };

    for ( int family = 0; family < 2; family++ )
    {
        const bool ipv6 = family == 1;
        const char * name = ipv6 ? "ipv6" : "ipv4";
        const int ip_bytes = ipv6 ? ROCKETNET_IPV6_HEADER_BYTES : ROCKETNET_IPV4_HEADER_BYTES;
        char label[128];

        ok &= write_config( filter, server_ipv4, server_ipv6 );

        spec_t udp_short = spec_default( ipv6 );
        udp_short.truncate_to = ROCKETNET_ETHERNET_HEADER_BYTES + ip_bytes + ROCKETNET_UDP_HEADER_BYTES - 1;
        snprintf( label, sizeof(label), "%s udp header cut one byte short", name );
        ok &= run_frame( filter, label, udp_short, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_UDP_SHORT_HEADER );

        spec_t other_port = spec_default( ipv6 );
        other_port.destination_port = server_port + 1;
        snprintf( label, sizeof(label), "%s other destination port", name );
        ok &= run_frame( filter, label, other_port, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );

        spec_t other_address = spec_default( ipv6 );
        other_address.destination_address = ipv6 ? other_ipv6 : other_ipv4;
        snprintf( label, sizeof(label), "%s other destination address", name );
        ok &= run_frame( filter, label, other_address, XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_OTHER_DESTINATION );

        spec_t wrong_epoch = spec_default( ipv6 );
        wrong_epoch.epoch = filter_epoch + 1;
        snprintf( label, sizeof(label), "%s wrong epoch", name );
        ok &= run_frame( filter, label, wrong_epoch, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_EPOCH );

        spec_t wrong_value = spec_default( ipv6 );
        wrong_value.filter[0] ^= 0x01;
        snprintf( label, sizeof(label), "%s wrong filter value", name );
        ok &= run_frame( filter, label, wrong_value, XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_PREFILTER_WRONG_VALUE );

        for ( int i = 0; i < (int) ( sizeof(type_rows) / sizeof(type_rows[0]) ); i++ )
        {
            spec_t spec = spec_default( ipv6 );
            spec.packet_type = type_rows[i].type;
            spec.packet_bytes = type_rows[i].bytes;
            snprintf( label, sizeof(label), "%s %s", name, type_rows[i].label );
            const int verdict = type_rows[i].counter == ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED ? XDP_REDIRECT : XDP_DROP;
            ok &= run_frame( filter, label, spec, verdict, type_rows[i].counter );
        }

        ok &= write_config_unconfigured( filter );
        snprintf( label, sizeof(label), "%s filter not configured", name );
        ok &= run_frame( filter, label, spec_default( ipv6 ), XDP_PASS, ROCKETNET_SERVER_XDP_COUNTER_PASSED_NOT_CONFIGURED );
    }

    return ok;
}

// bpf_redirect_map with no socket in the queue's slot: the frame drops, and
// the drop is counted under its own name. With the slot back, the same frame
// redirects.

static bool redirect_without_a_socket_is_counted( filter_t * filter )
{
    bool ok = true;
    const __u32 key = 0;
    const __u32 loopback = if_nametoindex( "lo" );

    for ( int family = 0; family < 2; family++ )
    {
        const bool ipv6 = family == 1;
        char label[96];

        ok &= write_config( filter, server_ipv4, server_ipv6 );

        if ( bpf_map_delete_elem( filter->socket_map_fd, &key ) != 0 )
        {
            printf( "  FAIL could not empty the redirect map slot: %s\n", strerror( errno ) );
            return false;
        }
        snprintf( label, sizeof(label), "%s accepted packet, no socket for the queue", ipv6 ? "ipv6" : "ipv4" );
        ok &= run_frame( filter, label, spec_default( ipv6 ), XDP_DROP, ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REDIRECT_NO_SOCKET );

        if ( loopback == 0 || bpf_map_update_elem( filter->socket_map_fd, &key, &loopback, BPF_ANY ) != 0 )
        {
            printf( "  FAIL could not repopulate the redirect map slot: %s\n", strerror( errno ) );
            return false;
        }
        snprintf( label, sizeof(label), "%s accepted packet, socket back in the slot", ipv6 ? "ipv6" : "ipv4" );
        ok &= run_frame( filter, label, spec_default( ipv6 ), XDP_REDIRECT, ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
    }
    return ok;
}

// The userspace refusal ladder, in its order: rocketnet_prefilter_check
// (sdk/rocketnet_protocol.cpp) for the prefilter header, then the reserved
// type and wrong direction checks of rocketnet_server_protocol_process_packet
// (sdk/rocketnet_server_protocol.cpp), then the type's wire size by
// ROCKETNET_WIRE_SERVER_PACKET_VALID. The names are the ones
// rocketnet_counter_name gives the userspace counters. This binary links only
// libbpf, so the prefilter's four comparisons are restated here against the
// same constants; NULL means the frame reaches a type's RECEIVED.

static const char * userspace_reason( const uint8_t * packet, int packet_bytes )
{
    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
        return "PREFILTER_TOO_SHORT";
    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return "PREFILTER_TOO_LARGE";
    if ( packet[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
        return "PREFILTER_WRONG_EPOCH";
    if ( memcmp( packet + 2, filter_value, ROCKETNET_FILTER_VALUE_BYTES ) != 0 )
        return "PREFILTER_WRONG_VALUE";

    const uint8_t type = packet[0];
    if ( type > ROCKETNET_CONNECTION_FRAGMENT_PACKET )
        return "RESERVED_TYPE";
    if ( type == ROCKETNET_CONNECTION_DENIED_PACKET || type == ROCKETNET_CONNECTION_CHALLENGE_PACKET || type == ROCKETNET_CONNECTION_COOKIE_PACKET )
        return "WRONG_DIRECTION";
    if ( ROCKETNET_WIRE_SERVER_PACKET_VALID( type, packet_bytes ) )
        return NULL;

    switch ( type )
    {
        case ROCKETNET_CONNECTION_REQUEST_PACKET:       return "CONNECTION_REQUEST_WRONG_SIZE";
        case ROCKETNET_CONNECTION_RESPONSE_PACKET:      return "CONNECTION_RESPONSE_WRONG_SIZE";
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:    return "KEEP_ALIVE_WRONG_SIZE";
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:       return "PAYLOAD_WRONG_SIZE";
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET:    return "DISCONNECT_WRONG_SIZE";
        case ROCKETNET_CONNECTION_ACK_PACKET:           return "ACK_WRONG_SIZE";
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:      return "FRAGMENT_WRONG_SIZE";
        default:                                        return "?";
    }
}

// The sweep: every type 0-255 at the boundary sizes (the prefilter bounds,
// and each inbound type's floor, exact size and ceiling, each plus or minus
// one), with the right header, the wrong epoch and the wrong filter value,
// on both families. Tests 4 and 5 run the same frames.

static const int sweep_sizes[] =
{
    0, 1, 2, 3, 4, 5, 6, 7,
    ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES - 1, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES + 1,
    ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES - 1, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES + 1,
    ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES - 1, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES + 1,
    ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - 1, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES + 1,
    ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES - 1, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES + 1,
    ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES - 1, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES + 1,
    ROCKETNET_RECEIVE_CEILING - 1, ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1,
};

#define SWEEP_SIZES ( (int) ( sizeof(sweep_sizes) / sizeof(sweep_sizes[0]) ) )
#define SWEEP_HEADERS 3
#define SWEEP_FRAMES ( 2 * SWEEP_HEADERS * 256 * SWEEP_SIZES )

static spec_t sweep_spec( int index )
{
    const int size = index % SWEEP_SIZES;
    const int type = ( index / SWEEP_SIZES ) % 256;
    const int header = ( index / ( SWEEP_SIZES * 256 ) ) % SWEEP_HEADERS;
    const bool ipv6 = index / ( SWEEP_SIZES * 256 * SWEEP_HEADERS ) == 1;

    spec_t spec = spec_default( ipv6 );
    spec.packet_type = (uint8_t) type;
    spec.packet_bytes = sweep_sizes[size];
    if ( header == 1 )
        spec.epoch = filter_epoch + 1;
    if ( header == 2 )
        spec.filter[1] ^= 0x10;
    return spec;
}

// the RocketNet packet a spec builds, as userspace would receive it

static int sweep_packet( const spec_t & spec, uint8_t * packet )
{
    frame_t frame;
    build_frame( &frame, spec );
    const int offset = ROCKETNET_ETHERNET_HEADER_BYTES + ( spec.ipv6 ? ROCKETNET_IPV6_HEADER_BYTES : ROCKETNET_IPV4_HEADER_BYTES ) + ROCKETNET_UDP_HEADER_BYTES;
    memcpy( packet, frame.data + offset, (size_t) spec.packet_bytes );
    return spec.packet_bytes;
}

static bool reasons_match_the_userspace_ladder( filter_t * filter )
{
    bool ok = write_config( filter, server_ipv4, server_ipv6 );
    int failures = 0;
    for ( int index = 0; index < SWEEP_FRAMES; index++ )
    {
        const spec_t spec = sweep_spec( index );
        static uint8_t packet[FRAME_CAPACITY];
        const int packet_bytes = sweep_packet( spec, packet );
        const char * want = userspace_reason( packet, packet_bytes );

        int moved = -1;
        const int verdict = run_counted( filter, spec, &moved );

        const bool match = want == NULL ? ( verdict == XDP_REDIRECT && moved == ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED )
                                        : ( verdict == XDP_DROP && moved >= 0 && strcmp( counter_name( moved ), want ) == 0 );
        if ( !match )
        {
            if ( failures++ < 20 )
                printf( "  FAIL %s type %d, %d bytes, epoch %d: %s with %s, userspace says %s\n", spec.ipv6 ? "ipv6" : "ipv4",
                        spec.packet_type, spec.packet_bytes, spec.epoch, verdict_name( verdict ),
                        moved >= 0 ? counter_name( moved ) : ( moved == -1 ? "no counter" : "a counter shape other than one by one" ),
                        want ? want : "a type's RECEIVED" );
            ok = false;
        }
    }
    if ( failures > 20 )
        printf( "  ... %d frames of %d disagree\n", failures, SWEEP_FRAMES );
    return ok;
}

// The BASE program (the filter before every exit was counted), built
// separately and named with --base: the sweep runs through both, and every
// frame gets the same verdict from each. Only the attribution changes.

static const char * base_object_path = NULL;

static bool verdicts_unchanged_from_base( filter_t * filter )
{
    if ( base_object_path == NULL )
    {
        printf( "  FAIL no BASE object: run with --base <object built from the BASE server_xdp.c>\n" );
        return false;
    }

    filter_t base;
    if ( !load_filter( &base, base_object_path ) )
    {
        printf( "  FAIL could not load the BASE object %s\n", base_object_path );
        return false;
    }

    bool ok = write_config( filter, server_ipv4, server_ipv6 ) && write_config( &base, server_ipv4, server_ipv6 );
    int failures = 0;
    for ( int index = 0; index < SWEEP_FRAMES; index++ )
    {
        const spec_t spec = sweep_spec( index );
        const int now = run_verdict( filter, spec );
        const int then = run_verdict( &base, spec );
        if ( now != then || now < 0 )
        {
            if ( failures++ < 20 )
                printf( "  FAIL %s type %d, %d bytes, epoch %d: %s now, %s at BASE\n", spec.ipv6 ? "ipv6" : "ipv4",
                        spec.packet_type, spec.packet_bytes, spec.epoch, verdict_name( now ), verdict_name( then ) );
            ok = false;
        }
    }
    if ( failures > 20 )
        printf( "  ... %d frames of %d disagree\n", failures, SWEEP_FRAMES );

    bpf_object__close( base.object );
    return ok;
}

// Conservation over the whole run, summed across CPUs: every frame that
// arrived left through exactly one counted exit.

static bool received_equals_the_sum_of_verdicts( filter_t * filter )
{
    uint64_t counters[NUM_COUNTER_SLOTS];
    read_counters( filter, counters );
    uint64_t verdicts = 0;
    for ( int i = 0; i < NUM_COUNTER_SLOTS; i++ )
    {
        if ( i != ROCKETNET_SERVER_XDP_COUNTER_RECEIVED )
            verdicts += counters[i];
    }
    const uint64_t received = counters[ROCKETNET_SERVER_XDP_COUNTER_RECEIVED];
    if ( received == 0 || received != verdicts )
    {
        printf( "  FAIL RECEIVED is %llu, the verdict counters sum to %llu\n", (unsigned long long) received, (unsigned long long) verdicts );
        return false;
    }
    printf( "  RECEIVED %llu == sum of verdicts %llu\n", (unsigned long long) received, (unsigned long long) verdicts );
    return true;
}

// the counter table names each slot once, in index order, with its class

static bool counter_table_matches_the_defines( filter_t * )
{
    bool ok = (int) ( sizeof(counter_rows) / sizeof(counter_rows[0]) ) == NUM_COUNTER_SLOTS;
    for ( int i = 0; i < NUM_COUNTER_SLOTS && ok; i++ )
    {
        const bool kind_ok = strcmp( counter_rows[i].kind, "arrival" ) == 0 ? i == ROCKETNET_SERVER_XDP_COUNTER_RECEIVED
                                                                             : ( strcmp( counter_rows[i].kind, "refusal" ) == 0 || strcmp( counter_rows[i].kind, "expected" ) == 0 );
        if ( counter_rows[i].index != i || !kind_ok )
        {
            printf( "  FAIL counter table row %d is %s (%s), index %d\n", i, counter_rows[i].name, counter_rows[i].kind, counter_rows[i].index );
            ok = false;
        }
        for ( int j = 0; j < i; j++ )
        {
            if ( strcmp( counter_rows[i].name, counter_rows[j].name ) == 0 )
            {
                printf( "  FAIL counter table names %s twice\n", counter_rows[i].name );
                ok = false;
            }
        }
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

struct case_t
{
    const char * name;
    case_function_t function;
};

// received_equals_the_sum_of_verdicts runs last, over every frame before it

static const case_t cases[] =
{
    { "every_verdict_is_counted", every_verdict_is_counted },
    { "ipv4_rocketnet_packet_redirects", ipv4_rocketnet_packet_redirects },
    { "ipv6_rocketnet_packet_redirects", ipv6_rocketnet_packet_redirects },
    { "ipv6_extension_header_passes", ipv6_extension_header_passes },
    { "ipv6_fragment_passes", ipv6_fragment_passes },
    { "ipv6_wrong_address_passes", ipv6_wrong_address_passes },
    { "ipv6_short_header_drops", ipv6_short_header_drops },
    { "ipv6_too_large_drops", ipv6_too_large_drops },
    { "ipv6_icmpv6_passes", ipv6_icmpv6_passes },
    { "ipv6_same_drops_as_ipv4", ipv6_same_drops_as_ipv4 },
    { "redirect_without_a_socket_is_counted", redirect_without_a_socket_is_counted },
    { "reasons_match_the_userspace_ladder", reasons_match_the_userspace_ladder },
    { "verdicts_unchanged_from_base", verdicts_unchanged_from_base },
    { "counter_table_matches_the_defines", counter_table_matches_the_defines },
    { "config_layout_matches_header", config_layout_matches_header },
    { "received_equals_the_sum_of_verdicts", received_equals_the_sum_of_verdicts },
};

int main( int argc, char ** argv )
{
    const char * path = ROCKETNET_XDP_FILTER_OBJECT;
    bool show_verifier = false;
    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "-v" ) == 0 )
            show_verifier = true;
        else if ( strcmp( argv[i], "--base" ) == 0 && i + 1 < argc )
            base_object_path = argv[++i];
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
