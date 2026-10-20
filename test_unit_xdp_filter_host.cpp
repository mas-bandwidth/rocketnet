/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP filter's decisions, judged as host C (tier 1 of the XDP tests).
// server_xdp/server_xdp.c compiles unchanged behind the shim in
// test/xdp_host/, and every case here is fixed frame bytes in, a verdict and
// the stats counters out: no kernel, no root, no socket, no real time. The
// kernel tier (test_xdp_filter, BPF_PROG_TEST_RUN) judges the same program
// with the verifier and the real helpers.
//
// Every frame is written byte by byte in this file (Ethernet, then IPv4 or
// IPv6, then UDP, then the 6-byte prefilter header, the 4-byte little-endian
// sequence and a zero body), never
// from the filter's own code. The cases are data-driven: one row per frame
// (bytes, receive queue, wanted verdict, wanted counter), so the kernel tier
// can replay the same rows and any disagreement names a shim defect or a
// kernel difference. Each row asserts its verdict, that its counter moved
// by exactly one, and that every other counter stayed at zero (standard/TRANSPORT.md,
// Demonstrating conformance: a must-fire with a must-not-fire).

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_unit_registry.h"
#include "test/resource_file.h"

#include "sdk/rocketnet_wire.h"
#include "sdk/rocketnet_xdp_limits.h"

#if ROCKETNET_XDP_FILTER_HOST

#include "test/xdp_host/server_xdp_host.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_address.h"
#include "sdk/rocketnet_xdp_replay_marks.h"

// The XDP verdicts, from the kernel's uapi enum xdp_action.

static const int XDP_HOST_ABORTED = 0;
static const int XDP_HOST_DROP = 1;
static const int XDP_HOST_PASS = 2;
static const int XDP_HOST_REDIRECT = 4;

// A row names the counter it must move with XDP_HOST_COUNTS(counter), and a
// row that must move none with XDP_HOST_NO_COUNTER.
// xdp_host_every_counter_has_a_case reads this file for the XDP_HOST_COUNTS
// spelling of every counter sdk/rocketnet_wire.h defines.

#define XDP_HOST_COUNTS( counter ) ( counter )
#define XDP_HOST_NO_COUNTER ( -1 )

// The configured server: 192.0.2.10 and 2001:db8::10 (documentation
// addresses, RFC 5737 and RFC 3849), port 40000, epoch 0, the published test
// filter value.

static const uint8_t xdp_host_server_ipv4[4] = { 192, 0, 2, 10 };
static const uint8_t xdp_host_server_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
static const uint8_t xdp_host_client_ipv4[4] = { 198, 51, 100, 7 };
static const uint8_t xdp_host_client_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07 };
static const uint16_t xdp_host_server_port = 40000;
static const uint8_t xdp_host_filter_value[4] = { 0xb7, 0x59, 0x01, 0x00 };

// The frame a row sends. Everything not named takes the default: a payload
// packet (type 5) of 100 UDP payload bytes, epoch 0, the test filter value
// and sequence 0, from the client's port 50000 to the configured server
// address and port.

static const uint16_t xdp_host_client_port = 50000;

struct xdp_host_frame_t
{
    int family = 4;                         // 4 or 6
    uint16_t ethertype = 0;                 // 0: the family's own (0x0800 or 0x86dd)
    int ipv4_ihl = 5;                       // IPv4 header words; options are zero bytes
    uint8_t protocol = 17;                  // IPv4 protocol or IPv6 next header
    const uint8_t * destination = NULL;     // NULL: the configured server address
    uint16_t port = xdp_host_server_port;
    int payload_bytes = 100;                // UDP payload: prefilter header + body
    uint8_t packet_type = 5;
    uint8_t epoch = 0;
    uint8_t filter[4] = { 0xb7, 0x59, 0x01, 0x00 };
    uint32_t sequence = 0;                  // the 4 little-endian bytes after the prefilter header
    uint16_t source_port = xdp_host_client_port;
    int truncate_to = -1;                   // >= 0: cut the frame to this many bytes
};

static void xdp_host_put16( std::vector<uint8_t> & out, uint16_t value )
{
    out.push_back( (uint8_t) ( value >> 8 ) );
    out.push_back( (uint8_t) ( value & 0xff ) );
}

static std::vector<uint8_t> xdp_host_frame_bytes( const xdp_host_frame_t & f )
{
    std::vector<uint8_t> out;

    // Ethernet: destination, source, ethertype

    const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    out.insert( out.end(), server_mac, server_mac + 6 );
    out.insert( out.end(), client_mac, client_mac + 6 );
    const uint16_t ethertype = f.ethertype != 0 ? f.ethertype : ( f.family == 4 ? 0x0800 : 0x86dd );
    xdp_host_put16( out, ethertype );
    if ( ethertype == 0x8100 || ethertype == 0x88a8 )
    {
        // an 802.1Q tag (VLAN 42), then the inner ethertype
        xdp_host_put16( out, 42 );
        xdp_host_put16( out, f.family == 4 ? 0x0800 : 0x86dd );
    }

    const int udp_bytes = 8 + f.payload_bytes;

    if ( f.family == 4 )
    {
        // RFC 791: version 4 and ihl, tos, total length, id, DF, ttl,
        // protocol, checksum (unchecked by the filter), source, destination

        const int header_bytes = f.ipv4_ihl * 4;
        out.push_back( (uint8_t) ( 0x40 | ( f.ipv4_ihl & 0x0f ) ) );
        out.push_back( 0x00 );
        xdp_host_put16( out, (uint16_t) ( header_bytes + udp_bytes ) );
        xdp_host_put16( out, 0x0000 );
        xdp_host_put16( out, 0x4000 );
        out.push_back( 64 );
        out.push_back( f.protocol );
        xdp_host_put16( out, 0x0000 );
        out.insert( out.end(), xdp_host_client_ipv4, xdp_host_client_ipv4 + 4 );
        const uint8_t * destination = f.destination ? f.destination : xdp_host_server_ipv4;
        out.insert( out.end(), destination, destination + 4 );
        for ( int i = 20; i < header_bytes; i++ )
            out.push_back( 0x00 );      // option bytes: end of options list
    }
    else
    {
        // RFC 8200 section 3: version 6, traffic class and flow label zero,
        // payload length, next header, hop limit, source, destination

        out.push_back( 0x60 );
        out.push_back( 0x00 );
        out.push_back( 0x00 );
        out.push_back( 0x00 );
        xdp_host_put16( out, (uint16_t) udp_bytes );
        out.push_back( f.protocol );
        out.push_back( 64 );
        out.insert( out.end(), xdp_host_client_ipv6, xdp_host_client_ipv6 + 16 );
        const uint8_t * destination = f.destination ? f.destination : xdp_host_server_ipv6;
        out.insert( out.end(), destination, destination + 16 );
    }

    // RFC 768: source port, destination port, length, checksum (zero)

    xdp_host_put16( out, f.source_port );
    xdp_host_put16( out, f.port );
    xdp_host_put16( out, (uint16_t) udp_bytes );
    xdp_host_put16( out, 0x0000 );

    // the prefilter header (type, epoch, filter value), the sequence little
    // endian, then a zero body; a payload shorter than the header carries
    // only its first bytes

    const uint8_t prefilter[10] = { f.packet_type, f.epoch, f.filter[0], f.filter[1], f.filter[2], f.filter[3],
                                    (uint8_t) ( f.sequence & 0xff ), (uint8_t) ( ( f.sequence >> 8 ) & 0xff ),
                                    (uint8_t) ( ( f.sequence >> 16 ) & 0xff ), (uint8_t) ( ( f.sequence >> 24 ) & 0xff ) };
    for ( int i = 0; i < f.payload_bytes; i++ )
        out.push_back( i < 10 ? prefilter[i] : 0x00 );

    if ( f.truncate_to >= 0 && (size_t) f.truncate_to < out.size() )
        out.resize( (size_t) f.truncate_to );

    return out;
}

struct xdp_host_config_t
{
    bool any_ipv4 = false;          // public_address 0
    bool any_ipv6 = false;          // public_address_ipv6 all zero
    uint16_t port = xdp_host_server_port;
    uint8_t epoch = 0;
    bool lookup_fails = false;
};

static void xdp_host_apply_config( const xdp_host_config_t & c )
{
    struct rocketnet_server_xdp_config_t config;
    memset( &config, 0, sizeof(config) );
    if ( !c.any_ipv4 )
        memcpy( &config.public_address, xdp_host_server_ipv4, 4 );      // network order, as stored
    const uint8_t port_bytes[2] = { (uint8_t) ( c.port >> 8 ), (uint8_t) ( c.port & 0xff ) };
    memcpy( &config.public_port, port_bytes, 2 );                       // network order, as stored
    memcpy( config.filter_value, xdp_host_filter_value, 4 );
    config.filter_epoch = c.epoch;
    if ( !c.any_ipv6 )
        memcpy( config.public_address_ipv6, xdp_host_server_ipv6, 16 );
    rocketnet_xdp_host_set_config( &config );
    rocketnet_xdp_host_set_config_lookup_fails( c.lookup_fails ? 1 : 0 );
}

// The replay map key of the client's address in a family, at a source port,
// built by the same function the transport's marks use.

static struct rocketnet_server_xdp_replay_key_t xdp_host_client_key( int family, uint16_t port )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof(address) );
    if ( family == 4 )
    {
        address.type = ROCKETNET_ADDRESS_IPV4;
        memcpy( address.data.ipv4, xdp_host_client_ipv4, 4 );
    }
    else
    {
        address.type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; i++ )
            address.data.ipv6[i] = (uint16_t) ( ( xdp_host_client_ipv6[i * 2] << 8 ) | xdp_host_client_ipv6[i * 2 + 1] );
    }
    address.port = port;
    struct rocketnet_server_xdp_replay_key_t key;
    rocketnet_xdp_replay_key_from_address( &address, &key );
    return key;
}

// One row: a frame, the receive queue it arrives on, the socket slot that
// is populated (-1: none), the configuration, the replay map entry for the
// client's address and port 50000 in the frame's family (none unless
// replay_entry), and what must happen.

struct xdp_host_row_t
{
    std::string label;
    xdp_host_frame_t frame;
    uint32_t queue = 0;
    int populated_slot = 0;
    xdp_host_config_t config;
    bool replay_entry = false;
    uint32_t replay_high_water = 0;
    int want_verdict = XDP_HOST_REDIRECT;
    int want_counter = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
};

static const char * xdp_host_verdict_name( int verdict )
{
    switch ( verdict )
    {
        case 0: return "XDP_ABORTED";
        case 1: return "XDP_DROP";
        case 2: return "XDP_PASS";
        case 3: return "XDP_TX";
        case 4: return "XDP_REDIRECT";
        default: return "not an XDP verdict";
    }
}

// Runs one row from a reset state and checks it; on any mismatch the row,
// its verdict and every counter print before the check fails.

static void xdp_host_run_row( const xdp_host_row_t & row )
{
    rocketnet_xdp_host_reset();
    xdp_host_apply_config( row.config );
    if ( row.populated_slot >= 0 )
        rocketnet_xdp_host_set_socket_slot( (uint32_t) row.populated_slot, 1 );
    if ( row.replay_entry )
    {
        const struct rocketnet_server_xdp_replay_key_t key = xdp_host_client_key( row.frame.family, xdp_host_client_port );
        rocketnet_xdp_host_set_replay_entry( &key, row.replay_high_water );
    }

    const std::vector<uint8_t> bytes = xdp_host_frame_bytes( row.frame );
    const int verdict = rocketnet_xdp_host_run( bytes.data(), (int) bytes.size(), row.queue );

    bool ok = verdict == row.want_verdict;
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        ok = ok && rocketnet_xdp_host_counter( i ) == ( i == row.want_counter ? 1u : 0u );

    if ( !ok )
    {
        printf( "xdp_host row \"%s\": verdict %s (%d), want %s (%d); counters:",
                row.label.c_str(), xdp_host_verdict_name( verdict ), verdict,
                xdp_host_verdict_name( row.want_verdict ), row.want_verdict );
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
            printf( " [%d]=%llu%s", i, (unsigned long long) rocketnet_xdp_host_counter( i ), i == row.want_counter ? " (want 1)" : "" );
        printf( "\n" );
        fflush( stdout );
    }

    rocketnet_unit_check( verdict == row.want_verdict );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        rocketnet_unit_check( rocketnet_xdp_host_counter( i ) == ( i == row.want_counter ? 1u : 0u ) );

    // a redirect decision is observed independently of the counter: the
    // helper was asked for exactly this queue's slot

    if ( row.want_verdict == XDP_HOST_REDIRECT )
    {
        rocketnet_unit_check( rocketnet_xdp_host_redirect_calls() == 1 );
        rocketnet_unit_check( rocketnet_xdp_host_last_redirect_key() == row.queue );
    }
}

static void xdp_host_run_rows( const std::vector<xdp_host_row_t> & rows )
{
    rocketnet_unit_check( !rows.empty() );
    for ( size_t i = 0; i < rows.size(); i++ )
        xdp_host_run_row( rows[i] );
}

// a row that sends the default frame of a family and expects a redirect

static xdp_host_row_t xdp_host_row( const char * label, int family )
{
    xdp_host_row_t row;
    row.label = label;
    row.frame.family = family;
    return row;
}

static xdp_host_row_t xdp_host_expect( xdp_host_row_t row, int verdict, int counter )
{
    row.want_verdict = verdict;
    row.want_counter = counter;
    return row;
}

// ---------------------------------------------------------------------------
// parse and redirect

static void xdp_host_redirect_rows( int family, std::vector<xdp_host_row_t> & rows )
{
    rows.push_back( xdp_host_row( "payload packet to the configured address, queue 0", family ) );

    xdp_host_row_t any = xdp_host_row( "payload packet with the any-address config", family );
    any.config.any_ipv4 = true;
    any.config.any_ipv6 = true;
    rows.push_back( any );

    xdp_host_row_t queue = xdp_host_row( "the rx queue index selects slot 7", family );
    queue.queue = 7;
    queue.populated_slot = 7;
    rows.push_back( queue );

    xdp_host_row_t last = xdp_host_row( "the last queue slot", family );
    last.queue = ROCKETNET_XDP_MAX_QUEUES - 1;
    last.populated_slot = ROCKETNET_XDP_MAX_QUEUES - 1;
    rows.push_back( last );

    // one row per inbound type at its smallest accepted size

    const struct { uint8_t type; int bytes; } inbound[] = { { 0, 710 }, { 3, 50 }, { 4, 41 }, { 5, 34 }, { 6, 33 }, { 8, 33 }, { 9, 40 } };
    for ( size_t i = 0; i < sizeof(inbound) / sizeof(inbound[0]); i++ )
    {
        xdp_host_row_t row = xdp_host_row( "an inbound type at its smallest size", family );
        row.label += " (type " + std::to_string( inbound[i].type ) + ")";
        row.frame.packet_type = inbound[i].type;
        row.frame.payload_bytes = inbound[i].bytes;
        rows.push_back( row );
    }
}

static void xdp_host_ipv4_rocketnet_packet_redirects()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_redirect_rows( 4, rows );
    xdp_host_run_rows( rows );
}

static void xdp_host_ipv6_rocketnet_packet_redirects()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_redirect_rows( 6, rows );
    xdp_host_run_rows( rows );
}

static void xdp_host_redirect_needs_a_populated_slot()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        // an empty slot: bpf_redirect_map returns the low bits of
        // BPF_F_INGRESS, XDP_DROP, and REDIRECTED does not move

        xdp_host_row_t empty = xdp_host_row( "no slot populated", families[f] );
        empty.populated_slot = -1;
        rows.push_back( xdp_host_expect( empty, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t other = xdp_host_row( "slot 2 populated, frame on queue 1", families[f] );
        other.queue = 1;
        other.populated_slot = 2;
        rows.push_back( xdp_host_expect( other, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t beyond = xdp_host_row( "queue index past the map", families[f] );
        beyond.queue = ROCKETNET_XDP_MAX_QUEUES;
        rows.push_back( xdp_host_expect( beyond, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t own = xdp_host_row( "slot 1 populated, frame on queue 1", families[f] );
        own.queue = 1;
        own.populated_slot = 1;
        rows.push_back( own );
    }
    xdp_host_run_rows( rows );

    // the helper was asked, and refused: the drop is the helper's verdict,
    // not an earlier check

    rocketnet_xdp_host_reset();
    xdp_host_apply_config( xdp_host_config_t() );
    const std::vector<uint8_t> bytes = xdp_host_frame_bytes( xdp_host_frame_t() );
    rocketnet_unit_check( rocketnet_xdp_host_run( bytes.data(), (int) bytes.size(), 5 ) == XDP_HOST_DROP );
    rocketnet_unit_check( rocketnet_xdp_host_redirect_calls() == 1 );
    rocketnet_unit_check( rocketnet_xdp_host_last_redirect_key() == 5 );
    rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED ) == 0 );
    (void) XDP_HOST_ABORTED;
}

// ---------------------------------------------------------------------------
// passes

static void xdp_host_unconfigured_passes()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        xdp_host_row_t zero = xdp_host_row( "configured port 0", families[f] );
        zero.config.port = 0;
        zero.config.any_ipv4 = true;
        zero.config.any_ipv6 = true;
        zero.frame.port = 0;
        rows.push_back( xdp_host_expect( zero, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t missing = xdp_host_row( "config lookup returns NULL", families[f] );
        missing.config.lookup_fails = true;
        rows.push_back( xdp_host_expect( missing, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_other_port_passes()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        xdp_host_row_t next = xdp_host_row( "the next port up", families[f] );
        next.frame.port = xdp_host_server_port + 1;
        rows.push_back( xdp_host_expect( next, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t swapped = xdp_host_row( "the port's bytes swapped", families[f] );
        swapped.frame.port = (uint16_t) ( ( xdp_host_server_port >> 8 ) | ( ( xdp_host_server_port & 0xff ) << 8 ) );
        rows.push_back( xdp_host_expect( swapped, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t any = xdp_host_row( "another port with the any-address config", families[f] );
        any.frame.port = 53;
        any.config.any_ipv4 = true;
        any.config.any_ipv6 = true;
        rows.push_back( xdp_host_expect( any, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t tcp = xdp_host_row( "TCP to the server port", families[f] );
        tcp.frame.protocol = 6;
        rows.push_back( xdp_host_expect( tcp, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        // the UDP header does not fit: the filter has no port to judge

        xdp_host_row_t cut = xdp_host_row( "UDP header cut to 7 bytes", families[f] );
        cut.frame.truncate_to = 14 + ( families[f] == 4 ? 20 : 40 ) + 7;
        rows.push_back( xdp_host_expect( cut, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }

    xdp_host_row_t arp = xdp_host_row( "an ARP frame", 4 );
    arp.frame.ethertype = 0x0806;
    rows.push_back( xdp_host_expect( arp, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_run_rows( rows );
}

static void xdp_host_ipv4_wrong_address_passes()
{
    std::vector<xdp_host_row_t> rows;
    static const uint8_t next_address[4] = { 192, 0, 2, 11 };
    static const uint8_t reversed[4] = { 10, 2, 0, 192 };
    static const uint8_t zero[4] = { 0, 0, 0, 0 };

    xdp_host_row_t next = xdp_host_row( "the next address up", 4 );
    next.frame.destination = next_address;
    rows.push_back( xdp_host_expect( next, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t swapped = xdp_host_row( "the address in host byte order", 4 );
    swapped.frame.destination = reversed;
    rows.push_back( xdp_host_expect( swapped, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t unspecified = xdp_host_row( "destination 0.0.0.0 against a configured address", 4 );
    unspecified.frame.destination = zero;
    rows.push_back( xdp_host_expect( unspecified, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // the any-address config takes every destination

    xdp_host_row_t any = xdp_host_row( "the next address up with the any-address config", 4 );
    any.frame.destination = next_address;
    any.config.any_ipv4 = true;
    rows.push_back( any );

    xdp_host_run_rows( rows );
}

static void xdp_host_ipv6_wrong_address_passes()
{
    std::vector<xdp_host_row_t> rows;

    // one differing byte in each of the four 32-bit words the filter compares

    static uint8_t words[4][16];
    for ( int w = 0; w < 4; w++ )
    {
        memcpy( words[w], xdp_host_server_ipv6, 16 );
        words[w][w * 4 + 1] ^= 0x01;
        xdp_host_row_t row = xdp_host_row( "the destination differs in one word", 6 );
        row.label += " (word " + std::to_string( w ) + ")";
        row.frame.destination = words[w];
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }

    static const uint8_t ipv4_mapped[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 10 };
    xdp_host_row_t mapped = xdp_host_row( "the IPv4 address, mapped", 6 );
    mapped.frame.destination = ipv4_mapped;
    rows.push_back( xdp_host_expect( mapped, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // the any-address config takes every destination; the IPv4 any-address
    // does not open IPv6

    xdp_host_row_t any = xdp_host_row( "a differing destination with the IPv6 any-address config", 6 );
    any.frame.destination = words[3];
    any.config.any_ipv6 = true;
    rows.push_back( any );

    xdp_host_row_t v4any = xdp_host_row( "a differing destination with only the IPv4 any-address config", 6 );
    v4any.frame.destination = words[3];
    v4any.config.any_ipv4 = true;
    rows.push_back( xdp_host_expect( v4any, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_run_rows( rows );
}

static void xdp_host_ipv4_options_pass()
{
    std::vector<xdp_host_row_t> rows;
    for ( int ihl = 6; ihl <= 15; ihl++ )
    {
        xdp_host_row_t row = xdp_host_row( "IPv4 with options", 4 );
        row.label += " (ihl " + std::to_string( ihl ) + ")";
        row.frame.ipv4_ihl = ihl;
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }
    xdp_host_run_rows( rows );
}

// ---------------------------------------------------------------------------
// short headers: a frame that names an IP family but cannot hold its fixed
// header drops, uncounted

static void xdp_host_ipv4_short_header_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int cuts[3] = { 14, 15, 14 + 19 };
    for ( int i = 0; i < 3; i++ )
    {
        xdp_host_row_t row = xdp_host_row( "IPv4 header cut short", 4 );
        row.label += " (frame " + std::to_string( cuts[i] ) + " bytes)";
        row.frame.truncate_to = cuts[i];
        rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );
    }

    // the whole header and no UDP: no longer short, nothing to judge

    xdp_host_row_t whole = xdp_host_row( "IPv4 header whole, no UDP", 4 );
    whole.frame.truncate_to = 14 + 20;
    rows.push_back( xdp_host_expect( whole, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // an Ethernet header cut short is not an IP frame at all

    xdp_host_row_t eth = xdp_host_row( "Ethernet header cut to 13 bytes", 4 );
    eth.frame.truncate_to = 13;
    rows.push_back( xdp_host_expect( eth, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_run_rows( rows );
}

static void xdp_host_ipv6_short_header_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int cuts[3] = { 14, 15, 14 + 39 };
    for ( int i = 0; i < 3; i++ )
    {
        xdp_host_row_t row = xdp_host_row( "IPv6 header cut short", 6 );
        row.label += " (frame " + std::to_string( cuts[i] ) + " bytes)";
        row.frame.truncate_to = cuts[i];
        rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );
    }

    xdp_host_row_t whole = xdp_host_row( "IPv6 header whole, no UDP", 6 );
    whole.frame.truncate_to = 14 + 40;
    rows.push_back( xdp_host_expect( whole, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // a short header behind an extension next header still drops: the
    // length check comes first

    xdp_host_row_t extension = xdp_host_row( "IPv6 header cut short, next header hop-by-hop", 6 );
    extension.frame.protocol = 0;
    extension.frame.truncate_to = 14 + 39;
    rows.push_back( xdp_host_expect( extension, XDP_HOST_DROP, XDP_HOST_NO_COUNTER ) );

    xdp_host_run_rows( rows );
}

// ---------------------------------------------------------------------------
// IPv6 next headers other than UDP

static void xdp_host_ipv6_extension_headers_pass_counted()
{
    std::vector<xdp_host_row_t> rows;

    // RFC 8200 section 4, by name; a RocketNet packet sits behind each, and
    // the filter does not walk the chain

    const struct { uint8_t next_header; const char * name; } extensions[] = {
        { 0, "hop-by-hop options" }, { 43, "routing" }, { 60, "destination options" },
        { 51, "authentication header" }, { 50, "encapsulating security payload" },
    };
    for ( size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++ )
    {
        xdp_host_row_t row = xdp_host_row( extensions[i].name, 6 );
        row.frame.protocol = extensions[i].next_header;
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );

        // counted on any port, and with nothing behind the fixed header

        xdp_host_row_t other = xdp_host_row( extensions[i].name, 6 );
        other.label += " to another port";
        other.frame.protocol = extensions[i].next_header;
        other.frame.port = 53;
        rows.push_back( xdp_host_expect( other, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );

        xdp_host_row_t bare = xdp_host_row( extensions[i].name, 6 );
        bare.label += " with nothing behind the fixed header";
        bare.frame.protocol = extensions[i].next_header;
        bare.frame.truncate_to = 14 + 40;
        rows.push_back( xdp_host_expect( bare, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_ipv6_fragment_passes_counted()
{
    std::vector<xdp_host_row_t> rows;

    // RFC 8200 section 4.5: the fragment header (44) passes to the kernel
    // stack for reassembly, counted, never dropped

    xdp_host_row_t row = xdp_host_row( "fragment header", 6 );
    row.frame.protocol = 44;
    rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );

    xdp_host_row_t tiny = xdp_host_row( "fragment header with a 1-byte payload", 6 );
    tiny.frame.protocol = 44;
    tiny.frame.payload_bytes = 1;
    rows.push_back( xdp_host_expect( tiny, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );

    xdp_host_row_t unconfigured = xdp_host_row( "fragment header to an unconfigured server", 6 );
    unconfigured.frame.protocol = 44;
    unconfigured.config.port = 0;
    rows.push_back( xdp_host_expect( unconfigured, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION ) ) );

    xdp_host_run_rows( rows );
}

static void xdp_host_icmpv6_passes_uncounted()
{
    std::vector<xdp_host_row_t> rows;

    xdp_host_row_t icmp = xdp_host_row( "ICMPv6", 6 );
    icmp.frame.protocol = 58;
    rows.push_back( xdp_host_expect( icmp, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // every other next header that is not UDP and not an extension header

    const uint8_t others[] = { 6, 41, 59, 135, 255 };
    for ( size_t i = 0; i < sizeof(others); i++ )
    {
        xdp_host_row_t row = xdp_host_row( "another next header", 6 );
        row.label += " (" + std::to_string( others[i] ) + ")";
        row.frame.protocol = others[i];
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_vlan_tagged_passes()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        xdp_host_row_t q = xdp_host_row( "802.1Q tag in front of a RocketNet packet", families[f] );
        q.frame.ethertype = 0x8100;
        rows.push_back( xdp_host_expect( q, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

        xdp_host_row_t ad = xdp_host_row( "802.1ad tag in front of a RocketNet packet", families[f] );
        ad.frame.ethertype = 0x88a8;
        rows.push_back( xdp_host_expect( ad, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }
    xdp_host_run_rows( rows );
}

// ---------------------------------------------------------------------------
// drops on the server's port: each counted by its reason

static void xdp_host_too_small_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        // the floor is the smallest inbound wire size, 33 (disconnect and
        // ack); one below drops, the floor itself is accepted

        const int sizes[4] = { 0, 1, 6, 32 };
        for ( int i = 0; i < 4; i++ )
        {
            xdp_host_row_t row = xdp_host_row( "UDP payload below the floor", families[f] );
            row.label += " (" + std::to_string( sizes[i] ) + " bytes)";
            row.frame.packet_type = 6;
            row.frame.payload_bytes = sizes[i];
            rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL ) ) );
        }

        xdp_host_row_t floor = xdp_host_row( "a disconnect at the floor, 33 bytes", families[f] );
        floor.frame.packet_type = 6;
        floor.frame.payload_bytes = 33;
        rows.push_back( floor );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_too_large_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        // the receive ceiling is 1472, one 1500-MTU IPv4 datagram's payload,
        // for both families

        xdp_host_row_t ceiling = xdp_host_row( "a payload packet at the ceiling, 1472 bytes", families[f] );
        ceiling.frame.payload_bytes = 1472;
        rows.push_back( ceiling );

        const int sizes[2] = { 1473, 2000 };
        for ( int i = 0; i < 2; i++ )
        {
            xdp_host_row_t row = xdp_host_row( "a payload packet above the ceiling", families[f] );
            row.label += " (" + std::to_string( sizes[i] ) + " bytes)";
            row.frame.payload_bytes = sizes[i];
            rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );
        }

        // the size bounds come before the prefilter header: an oversized
        // packet with a wrong epoch is too large, not the wrong epoch

        xdp_host_row_t first = xdp_host_row( "above the ceiling with a wrong epoch", families[f] );
        first.frame.payload_bytes = 1473;
        first.frame.epoch = 9;
        rows.push_back( xdp_host_expect( first, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_wrong_epoch_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        const uint8_t epochs[3] = { 1, 2, 255 };
        for ( int i = 0; i < 3; i++ )
        {
            xdp_host_row_t row = xdp_host_row( "a nonzero epoch at an epoch-0 server", families[f] );
            row.label += " (" + std::to_string( epochs[i] ) + ")";
            row.frame.epoch = epochs[i];
            rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH ) ) );
        }

        xdp_host_row_t keyed = xdp_host_row( "epoch 0 at an epoch-3 server", families[f] );
        keyed.config.epoch = 3;
        rows.push_back( xdp_host_expect( keyed, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH ) ) );

        xdp_host_row_t held = xdp_host_row( "epoch 3 at an epoch-3 server", families[f] );
        held.config.epoch = 3;
        held.frame.epoch = 3;
        rows.push_back( held );

        // the epoch is compared before the value

        xdp_host_row_t both = xdp_host_row( "a wrong epoch and a wrong filter value", families[f] );
        both.frame.epoch = 1;
        both.frame.filter[0] ^= 0xff;
        rows.push_back( xdp_host_expect( both, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH ) ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_wrong_filter_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        for ( int b = 0; b < 4; b++ )
        {
            xdp_host_row_t row = xdp_host_row( "one filter value byte differs", families[f] );
            row.label += " (byte " + std::to_string( b ) + ")";
            row.frame.filter[b] ^= 0x01;
            rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER ) ) );
        }

        // the filter value is compared before the type

        xdp_host_row_t both = xdp_host_row( "a wrong filter value and a reserved type", families[f] );
        both.frame.filter[3] ^= 0x80;
        both.frame.packet_type = 200;
        rows.push_back( xdp_host_expect( both, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER ) ) );
    }
    xdp_host_run_rows( rows );
}

static void xdp_host_invalid_type_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        // server-to-client types at their own exact sizes, reserved types,
        // and inbound types one byte off their size

        const struct { uint8_t type; int bytes; const char * name; } refused[] = {
            { 1, 34, "connection denied" }, { 2, 66, "connection challenge" }, { 7, 38, "connection cookie" },
            { 10, 100, "reserved type 10" }, { 255, 100, "reserved type 255" },
            { 0, 709, "connection request one byte short" }, { 0, 711, "connection request one byte long" },
            { 3, 49, "connection response one byte short" }, { 3, 51, "connection response one byte long" },
            { 4, 40, "keep-alive below its minimum" }, { 5, 33, "payload below its minimum" },
            { 9, 39, "payload fragment below its minimum" },
        };
        for ( size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++ )
        {
            xdp_host_row_t row = xdp_host_row( refused[i].name, families[f] );
            row.frame.packet_type = refused[i].type;
            row.frame.payload_bytes = refused[i].bytes;
            rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE ) ) );
        }
    }
    xdp_host_run_rows( rows );
}

// ---------------------------------------------------------------------------
// the exhaustive sweep: every type byte at every UDP payload size from 0 to
// one past the receive ceiling, both families, against the size table of
// standard/TRANSPORT.md t8.11 and t10 (the server ignores denied,
// challenge and cookie, and types 10 to 255 are reserved). The table is
// written here, never taken from ROCKETNET_WIRE_SERVER_PACKET_VALID.

static void xdp_host_type_and_size_sweep_matches_the_standard()
{
    const int ceiling = 1472;   // 1500 - 20 - 8

    struct inbound_t { int type; int min_bytes; int max_bytes; };
    const inbound_t standard[] = {
        { 0, 710, 710 },        // connection request
        { 3, 50, 50 },          // connection response
        { 4, 41, ceiling },     // keep-alive
        { 5, 34, ceiling },     // payload
        { 6, 33, ceiling },     // disconnect
        { 8, 33, ceiling },     // ack
        { 9, 40, ceiling },     // payload fragment
    };
    const int standard_rows = (int) ( sizeof(standard) / sizeof(standard[0]) );

    int floor = ceiling;
    for ( int i = 0; i < standard_rows; i++ )
        floor = standard[i].min_bytes < floor ? standard[i].min_bytes : floor;
    rocketnet_unit_check( floor == 33 );

    // One reset per family, one frame per size with its type byte patched
    // per run, and the counters checked against a running expectation after
    // every run, which is the same claim as a reset per run (exactly one
    // counter moved, by one) at a fraction of the cost under the sanitizers.

    long runs = 0;
    long redirects = 0;
    const int families[2] = { 4, 6 };
    for ( int f = 0; f < 2; f++ )
    {
        rocketnet_xdp_host_reset();
        xdp_host_apply_config( xdp_host_config_t() );
        rocketnet_xdp_host_set_socket_slot( 0, 1 );
        uint64_t want_counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = { 0 };

        for ( int bytes = 0; bytes <= ceiling + 1; bytes++ )
        {
            xdp_host_frame_t frame;
            frame.family = families[f];
            frame.payload_bytes = bytes;
            std::vector<uint8_t> image = xdp_host_frame_bytes( frame );
            const size_t type_offset = image.size() - (size_t) bytes;

            for ( int type = 0; type <= 255; type++ )
            {
                if ( bytes > 0 )
                    image[type_offset] = (uint8_t) type;

                bool valid = false;
                for ( int i = 0; i < standard_rows; i++ )
                    valid = valid || ( standard[i].type == type && bytes >= standard[i].min_bytes && bytes <= standard[i].max_bytes );

                int want_verdict = XDP_HOST_DROP;
                int want_counter;
                if ( bytes < floor )
                    want_counter = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
                else if ( bytes > ceiling )
                    want_counter = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE );
                else if ( !valid )
                    want_counter = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE );
                else
                {
                    want_verdict = XDP_HOST_REDIRECT;
                    want_counter = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
                    redirects++;
                }
                want_counters[want_counter]++;

                const int verdict = rocketnet_xdp_host_run( image.data(), (int) image.size(), 0 );
                bool ok = verdict == want_verdict;
                for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
                    ok = ok && rocketnet_xdp_host_counter( i ) == want_counters[i];
                if ( !ok )
                {
                    printf( "xdp_host sweep: family %d, type %d, %d bytes: verdict %s (%d), want %s (%d), counter %d\n",
                            families[f], type, bytes, xdp_host_verdict_name( verdict ), verdict,
                            xdp_host_verdict_name( want_verdict ), want_verdict, want_counter );
                    fflush( stdout );
                }
                rocketnet_unit_check( ok );
                runs++;
            }
        }
    }

    // 2 families x 256 types x 1474 sizes, and the accepted set is exactly
    // the table's: 2 x ( 1 + 1 + 1432 + 1439 + 1440 + 1440 + 1433 )

    rocketnet_unit_check( runs == 2L * 256 * ( ceiling + 2 ) );
    rocketnet_unit_check( redirects == 2L * ( 1 + 1 + 1432 + 1439 + 1440 + 1440 + 1433 ) );
}

// ---------------------------------------------------------------------------
// replay: the read-only high-water map (tla/RNXdpReplay.tla, KernelFilter).
// With an entry for the source address and port, a session packet whose
// sequence is ROCKETNET_XDP_REPLAY_WINDOW (16) or more behind the high-water
// drops; inside the window or ahead it goes on; handshake types and sources
// with no entry are not checked.

static xdp_host_row_t xdp_host_replay_row( const char * label, int family, uint8_t type, uint32_t sequence, uint32_t high_water )
{
    xdp_host_row_t row = xdp_host_row( label, family );
    row.frame.packet_type = type;
    row.frame.sequence = sequence;
    if ( type == ROCKETNET_CONNECTION_REQUEST_PACKET )
        row.frame.payload_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
    else if ( type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
        row.frame.payload_bytes = ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES;
    row.replay_entry = true;
    row.replay_high_water = high_water;
    return row;
}

static void xdp_host_replay_rows( int family, std::vector<xdp_host_row_t> & rows )
{
    const int replayed = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED );

    rows.push_back( xdp_host_expect( xdp_host_replay_row( "payload 16 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100 ),
                                     XDP_HOST_DROP, replayed ) );
    rows.push_back( xdp_host_replay_row( "payload 15 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 85, 100 ) );
    rows.push_back( xdp_host_replay_row( "payload at the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 100, 100 ) );
    rows.push_back( xdp_host_replay_row( "payload one ahead of the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 101, 100 ) );

    const struct { uint8_t type; const char * label; } session[] =
    {
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, "keep-alive 16 behind the high-water" },
        { ROCKETNET_CONNECTION_ACK_PACKET, "ack 16 behind the high-water" },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, "disconnect 16 behind the high-water" },
        { ROCKETNET_CONNECTION_FRAGMENT_PACKET, "fragment 16 behind the high-water" },
    };
    for ( size_t i = 0; i < sizeof(session) / sizeof(session[0]); i++ )
        rows.push_back( xdp_host_expect( xdp_host_replay_row( session[i].label, family, session[i].type, 84, 100 ),
                                         XDP_HOST_DROP, replayed ) );

    // the handshake types are not checked
    rows.push_back( xdp_host_replay_row( "connection request at 84 is not checked", family, ROCKETNET_CONNECTION_REQUEST_PACKET, 84, 100 ) );
    rows.push_back( xdp_host_replay_row( "connection response at 84 is not checked", family, ROCKETNET_CONNECTION_RESPONSE_PACKET, 84, 100 ) );

    // another source port has no entry
    xdp_host_row_t other = xdp_host_replay_row( "another source port has no entry", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100 );
    other.frame.source_port = xdp_host_client_port + 1;
    rows.push_back( other );

    // no entry at all
    xdp_host_row_t none = xdp_host_replay_row( "no replay entry", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 84, 100 );
    none.replay_entry = false;
    rows.push_back( none );

    // the wrap: the difference is modulo 2^32, read as signed
    rows.push_back( xdp_host_expect( xdp_host_replay_row( "high-water 5, sequence 0xfffffff0 is behind across the wrap", family,
                                                          ROCKETNET_CONNECTION_PAYLOAD_PACKET, 0xfffffff0u, 5 ),
                                     XDP_HOST_DROP, replayed ) );
    rows.push_back( xdp_host_replay_row( "high-water 0xfffffff0, sequence 5 is ahead across the wrap", family,
                                         ROCKETNET_CONNECTION_PAYLOAD_PACKET, 5, 0xfffffff0u ) );
}

static void xdp_host_replay_drops_behind_the_window()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_replay_rows( 4, rows );
    xdp_host_replay_rows( 6, rows );
    xdp_host_run_rows( rows );
}

// The program never writes the map: one entry at high-water 100, then a
// forged payload far ahead and a hundred more session packets through the
// same state, each asserting its verdict and that exactly its counter moved
// by one, and the entry still reads 100 with no other entry made.

static void xdp_host_replay_never_writes_the_map()
{
    const int families[2] = { 4, 6 };
    const uint8_t session[5] = { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                 ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET,
                                 ROCKETNET_CONNECTION_FRAGMENT_PACKET };
    for ( int f = 0; f < 2; f++ )
    {
        rocketnet_xdp_host_reset();
        xdp_host_apply_config( xdp_host_config_t() );
        rocketnet_xdp_host_set_socket_slot( 0, 1 );
        const struct rocketnet_server_xdp_replay_key_t key = xdp_host_client_key( families[f], xdp_host_client_port );
        rocketnet_xdp_host_set_replay_entry( &key, 100 );

        for ( int i = -1; i < 100; i++ )
        {
            xdp_host_frame_t frame;
            frame.family = families[f];
            frame.packet_type = i < 0 ? ROCKETNET_CONNECTION_PAYLOAD_PACKET : session[i % 5];
            frame.sequence = i < 0 ? 0x7fffffffu : (uint32_t) ( i * 3 );
            const bool behind = (int32_t) ( 100u - frame.sequence ) >= ROCKETNET_XDP_REPLAY_WINDOW;
            const int want_verdict = behind ? XDP_HOST_DROP : XDP_HOST_REDIRECT;
            const int want_counter = behind ? XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED )
                                            : XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );

            uint64_t before[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
            for ( int c = 0; c < ROCKETNET_SERVER_XDP_NUM_COUNTERS; c++ )
                before[c] = rocketnet_xdp_host_counter( c );

            const std::vector<uint8_t> bytes = xdp_host_frame_bytes( frame );
            const int verdict = rocketnet_xdp_host_run( bytes.data(), (int) bytes.size(), 0 );

            bool ok = verdict == want_verdict;
            for ( int c = 0; c < ROCKETNET_SERVER_XDP_NUM_COUNTERS; c++ )
                ok = ok && rocketnet_xdp_host_counter( c ) - before[c] == ( c == want_counter ? 1u : 0u );
            if ( !ok )
            {
                printf( "xdp_host replay never writes: family %d, row %d, type %d, sequence 0x%08x: verdict %s (%d), want %s (%d)\n",
                        families[f], i, frame.packet_type, frame.sequence, xdp_host_verdict_name( verdict ), verdict,
                        xdp_host_verdict_name( want_verdict ), want_verdict );
                fflush( stdout );
            }
            rocketnet_unit_check( ok );

            uint32_t high_water = 0;
            rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
            rocketnet_unit_check( high_water == 100 );
        }

        // 1 forged redirect, then rows 0..28 (sequence 0..84) drop and rows
        // 29..99 (sequence 87..297) redirect
        rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED ) == 29 );
        rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED ) == 72 );

        uint32_t high_water = 0;
        rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
        rocketnet_unit_check( high_water == 100 );
        const struct rocketnet_server_xdp_replay_key_t other = xdp_host_client_key( families[f], xdp_host_client_port + 1 );
        rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &other, &high_water ) == 0 );
    }
}

// ---------------------------------------------------------------------------
// every counter has a case: each ROCKETNET_SERVER_XDP_COUNTER_* that
// sdk/rocketnet_wire.h defines (other than the count) must appear in this
// file as a row's XDP_HOST_COUNTS(...), so a new counter cannot land without
// a case that makes it move.

static std::string xdp_host_read_file( const char * relative )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    std::string text;
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( file == NULL )
    {
        printf( "xdp_host_every_counter_has_a_case: cannot read %s\n", path.c_str() );
        fflush( stdout );
        return text;
    }
    char buffer[4096];
    size_t got;
    while ( ( got = fread( buffer, 1, sizeof(buffer), file ) ) > 0 )
        text.append( buffer, got );
    fclose( file );
    return text;
}

static void xdp_host_every_counter_has_a_case()
{
    const std::string wire = xdp_host_read_file( "sdk/rocketnet_wire.h" );
    const std::string self = xdp_host_read_file( "test_unit_xdp_filter_host.cpp" );
    rocketnet_unit_check( !wire.empty() );
    rocketnet_unit_check( !self.empty() );

    const std::string define = "#define ROCKETNET_SERVER_XDP_COUNTER_";
    std::vector<std::string> names;
    size_t at = 0;
    while ( ( at = wire.find( define, at ) ) != std::string::npos )
    {
        size_t end = at + strlen( "#define " );
        size_t stop = end;
        while ( stop < wire.size() && ( isalnum( (unsigned char) wire[stop] ) || wire[stop] == '_' ) )
            stop++;
        names.push_back( wire.substr( end, stop - end ) );
        at = stop;
    }

    rocketnet_unit_check( (int) names.size() == ROCKETNET_SERVER_XDP_NUM_COUNTERS );

    int missing = 0;
    for ( size_t i = 0; i < names.size(); i++ )
    {
        const std::string needle = std::string( "XDP_HOST_" ) + "COUNTS( " + names[i] + " )";
        if ( self.find( needle ) == std::string::npos )
        {
            printf( "xdp_host_every_counter_has_a_case: %s has no case\n", names[i].c_str() );
            fflush( stdout );
            missing++;
        }
    }
    rocketnet_unit_check( missing == 0 );
}

#else // #if ROCKETNET_XDP_FILTER_HOST

// The host filter target is built for GCC or Clang on a little-endian,
// non-Windows host (CMakeLists.txt); elsewhere each case names itself as
// skipped.

#define XDP_HOST_SKIPPED( name )                                                                                  \
    static void name()                                                                                            \
    {                                                                                                             \
        printf( #name " [SKIPPED the XDP filter host target needs GCC or Clang on a little-endian, non-Windows host]\n" ); \
        fflush( stdout );                                                                                         \
    }

XDP_HOST_SKIPPED( xdp_host_ipv4_rocketnet_packet_redirects )
XDP_HOST_SKIPPED( xdp_host_ipv6_rocketnet_packet_redirects )
XDP_HOST_SKIPPED( xdp_host_redirect_needs_a_populated_slot )
XDP_HOST_SKIPPED( xdp_host_unconfigured_passes )
XDP_HOST_SKIPPED( xdp_host_other_port_passes )
XDP_HOST_SKIPPED( xdp_host_ipv4_wrong_address_passes )
XDP_HOST_SKIPPED( xdp_host_ipv6_wrong_address_passes )
XDP_HOST_SKIPPED( xdp_host_ipv4_options_pass )
XDP_HOST_SKIPPED( xdp_host_ipv4_short_header_drops )
XDP_HOST_SKIPPED( xdp_host_ipv6_short_header_drops )
XDP_HOST_SKIPPED( xdp_host_ipv6_extension_headers_pass_counted )
XDP_HOST_SKIPPED( xdp_host_ipv6_fragment_passes_counted )
XDP_HOST_SKIPPED( xdp_host_icmpv6_passes_uncounted )
XDP_HOST_SKIPPED( xdp_host_vlan_tagged_passes )
XDP_HOST_SKIPPED( xdp_host_too_small_drops )
XDP_HOST_SKIPPED( xdp_host_too_large_drops )
XDP_HOST_SKIPPED( xdp_host_wrong_epoch_drops )
XDP_HOST_SKIPPED( xdp_host_wrong_filter_drops )
XDP_HOST_SKIPPED( xdp_host_invalid_type_drops )
XDP_HOST_SKIPPED( xdp_host_type_and_size_sweep_matches_the_standard )
XDP_HOST_SKIPPED( xdp_host_every_counter_has_a_case )
XDP_HOST_SKIPPED( xdp_host_replay_drops_behind_the_window )
XDP_HOST_SKIPPED( xdp_host_replay_never_writes_the_map )

#endif // #if ROCKETNET_XDP_FILTER_HOST

REGISTER_UNIT_TEST( xdp_host_ipv4_rocketnet_packet_redirects );
REGISTER_UNIT_TEST( xdp_host_ipv6_rocketnet_packet_redirects );
REGISTER_UNIT_TEST( xdp_host_redirect_needs_a_populated_slot );
REGISTER_UNIT_TEST( xdp_host_unconfigured_passes );
REGISTER_UNIT_TEST( xdp_host_other_port_passes );
REGISTER_UNIT_TEST( xdp_host_ipv4_wrong_address_passes );
REGISTER_UNIT_TEST( xdp_host_ipv6_wrong_address_passes );
REGISTER_UNIT_TEST( xdp_host_ipv4_options_pass );
REGISTER_UNIT_TEST( xdp_host_ipv4_short_header_drops );
REGISTER_UNIT_TEST( xdp_host_ipv6_short_header_drops );
REGISTER_UNIT_TEST( xdp_host_ipv6_extension_headers_pass_counted );
REGISTER_UNIT_TEST( xdp_host_ipv6_fragment_passes_counted );
REGISTER_UNIT_TEST( xdp_host_icmpv6_passes_uncounted );
REGISTER_UNIT_TEST( xdp_host_vlan_tagged_passes );
REGISTER_UNIT_TEST( xdp_host_too_small_drops );
REGISTER_UNIT_TEST( xdp_host_too_large_drops );
REGISTER_UNIT_TEST( xdp_host_wrong_epoch_drops );
REGISTER_UNIT_TEST( xdp_host_wrong_filter_drops );
REGISTER_UNIT_TEST( xdp_host_invalid_type_drops );
REGISTER_UNIT_TEST( xdp_host_type_and_size_sweep_matches_the_standard );
REGISTER_UNIT_TEST( xdp_host_every_counter_has_a_case );
REGISTER_UNIT_TEST( xdp_host_replay_drops_behind_the_window );
REGISTER_UNIT_TEST( xdp_host_replay_never_writes_the_map );
