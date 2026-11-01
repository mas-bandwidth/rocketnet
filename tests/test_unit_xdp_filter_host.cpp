/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP filter's decisions, judged as host C (tier 1 of the XDP tests).
// server_xdp/server_xdp.c compiles unchanged behind the shim in
// tests/xdp_host/, and every case here is fixed frame bytes in, a verdict and
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
// by exactly one, and that every other counter stayed at zero (STANDARD.md,
// Demonstrating conformance: a must-fire with a must-not-fire).

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_unit_registry.h"
#include "resource_file.h"

#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_limits.h"

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST

#include "xdp_host/server_xdp_host.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"
#include "../sdk/rocketnet_replay.h"
#include "../sdk/rocketnet_xdp_frame.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_config.h"

// The XDP verdicts, from the kernel's uapi enum xdp_action.

static const int XDP_HOST_ABORTED = 0;
static const int XDP_HOST_DROP = 1;
static const int XDP_HOST_PASS = 2;
static const int XDP_HOST_REDIRECT = 4;

// A row names the counter it must move with XDP_HOST_COUNTS(counter), and a
// row that must move none with XDP_HOST_NO_COUNTER.
// test_xdp_transport_host_every_counter_has_a_case reads this file for the XDP_HOST_COUNTS
// spelling of every counter sdk/rocketnet_wire.h defines.

#define XDP_HOST_COUNTS( counter ) ( counter )
#define XDP_HOST_NO_COUNTER ( -1 )

static uint64_t xdp_host_runtime_counter_mask = 0;
static uint32_t xdp_host_runtime_counter_rows = 0;

// The configured server: 192.0.2.10 and 2001:db8::10 (documentation
// addresses, RFC 5737 and RFC 3849), port 40000, epoch 0, the published test
// filter value.

static const uint8_t xdp_host_server_ipv4[4] = { 192, 0, 2, 10 };
static const uint8_t xdp_host_server_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
static const uint8_t xdp_host_client_ipv4[4] = { 198, 51, 100, 7 };
static const uint8_t xdp_host_client_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07 };
static const uint16_t xdp_host_server_port = 40000;

// another address on the server's networks, for frames that are not its

static const uint8_t xdp_host_other_ipv4[4] = { 192, 0, 2, 11 };
static const uint8_t xdp_host_other_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11 };
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
    uint16_t ipv4_frag_off = 0x4000;        // IPv4 flags and fragment offset: DF, offset 0
    uint8_t protocol = 17;                  // IPv4 protocol or IPv6 next header
    int ipv6_fragment = -1;                 // >= 0: an IPv6 fragment header (next header 44) with
                                            // this offset and flags field (offset << 3 | M)
    uint8_t ipv6_fragment_next = 17;        // the fragment header's next header
    std::vector<int> ipv6_extensions;       // real extension headers after the fixed header, in order:
                                            // 0, 43, 60 (8 bytes each), 51 (AH, 12 bytes),
                                            // 50 (ESP, SPI and sequence only), or 44 + fragment
                                            // field << 8 (a fragment header with that field)
    int ipv6_first_extension_length = -1;   // >= 0: override the first extension header's declared length
    const uint8_t * destination = NULL;     // NULL: the configured server address
    uint16_t port = xdp_host_server_port;
    int payload_bytes = 100;                // UDP payload: prefilter header + body
    uint8_t packet_type = 5;
    uint8_t epoch = 0;
    uint8_t filter[4] = { 0xb7, 0x59, 0x01, 0x00 };
    uint32_t sequence = 0;                  // the 4 little-endian bytes after the prefilter header
    uint16_t source_port = xdp_host_client_port;
    int truncate_to = -1;                   // >= 0: cut the frame to this many bytes
    int override_udp_len = -1;              // >= 0: override UDP header length field
    int override_ip_len = -1;               // >= 0: override the IPv4 total length or IPv6 payload length field
    int pad_bytes = 0;                      // zero bytes after the UDP payload (link-layer padding)
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
        xdp_host_put16( out, (uint16_t) ( f.override_ip_len >= 0 ? f.override_ip_len : header_bytes + udp_bytes ) );
        xdp_host_put16( out, 0x0000 );
        xdp_host_put16( out, f.ipv4_frag_off );
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

        const bool fragment = f.ipv6_fragment >= 0;
        int extension_bytes = 0;
        for ( size_t i = 0; i < f.ipv6_extensions.size(); i++ )
        {
            const int type = f.ipv6_extensions[i] & 0xff;
            extension_bytes += type == 51 ? 12 : 8;
        }

        const uint8_t first_next_header = fragment ? 44 : ( !f.ipv6_extensions.empty() ? (uint8_t) ( f.ipv6_extensions[0] & 0xff ) : f.protocol );
        out.push_back( 0x60 );
        out.push_back( 0x00 );
        out.push_back( 0x00 );
        out.push_back( 0x00 );
        xdp_host_put16( out, (uint16_t) ( f.override_ip_len >= 0 ? f.override_ip_len : udp_bytes + ( fragment ? 8 : 0 ) + extension_bytes ) );
        out.push_back( first_next_header );
        out.push_back( 64 );
        out.insert( out.end(), xdp_host_client_ipv6, xdp_host_client_ipv6 + 16 );

        const uint8_t * destination = f.destination ? f.destination : xdp_host_server_ipv6;
        out.insert( out.end(), destination, destination + 16 );

        // RFC 8200 section 4.5: next header, reserved, the fragment offset
        // in 8-byte units shifted left 3 with M in bit 0, identification

        if ( fragment )
        {
            out.push_back( f.ipv6_fragment_next );
            out.push_back( 0x00 );
            xdp_host_put16( out, (uint16_t) f.ipv6_fragment );
            xdp_host_put16( out, 0x1234 );
            xdp_host_put16( out, 0x5678 );
        }

        // RFC 8200 section 4.3 to 4.6 and RFC 4302 section 2: each header
        // names the next, the last names the frame's protocol (UDP)

        for ( size_t i = 0; i < f.ipv6_extensions.size(); i++ )
        {
            const int type = f.ipv6_extensions[i] & 0xff;
            const uint8_t next = i + 1 < f.ipv6_extensions.size() ? (uint8_t) ( f.ipv6_extensions[i + 1] & 0xff ) : f.protocol;
            if ( type == 50 )
            {
                // ESP: SPI and sequence; everything after is ciphertext

                for ( int b = 0; b < 8; b++ )
                {
                    out.push_back( (uint8_t) ( 0x10 + b ) );
                }
            }
            else if ( type == 51 )
            {
                // AH: next header, payload length (12 bytes: 1), reserved, SPI, sequence

                out.push_back( next );
                out.push_back( i == 0 && f.ipv6_first_extension_length >= 0 ? (uint8_t) f.ipv6_first_extension_length : 1 );

                for ( int b = 0; b < 10; b++ )
                {
                    out.push_back( 0x00 );
                }
            }
            else if ( type == 44 )
            {
                out.push_back( next );
                out.push_back( 0x00 );
                xdp_host_put16( out, (uint16_t) ( f.ipv6_extensions[i] >> 8 ) );
                xdp_host_put16( out, 0x1234 );
                xdp_host_put16( out, 0x5678 );
            }
            else
            {
                // hop-by-hop, routing or destination options: length 0 (8
                // bytes), the rest a PadN option

                out.push_back( next );
                out.push_back( i == 0 && f.ipv6_first_extension_length >= 0 ? (uint8_t) f.ipv6_first_extension_length : 0 );
                out.push_back( 1 );
                out.push_back( 4 );

                for ( int b = 0; b < 4; b++ )
                {
                    out.push_back( 0x00 );
                }
            }
        }
    }

    // RFC 768: source port, destination port, length, checksum (zero)

    xdp_host_put16( out, f.source_port );
    xdp_host_put16( out, f.port );
    xdp_host_put16( out, (uint16_t) ( f.override_udp_len >= 0 ? f.override_udp_len : udp_bytes ) );
    xdp_host_put16( out, 0x0000 );

    // the prefilter header (type, epoch, filter value), the sequence little
    // endian, then a zero body; a payload shorter than the header carries
    // only its first bytes

    const uint8_t prefilter[10] = { f.packet_type, f.epoch, f.filter[0], f.filter[1], f.filter[2], f.filter[3],
                                    (uint8_t) ( f.sequence & 0xff ), (uint8_t) ( ( f.sequence >> 8 ) & 0xff ),
                                    (uint8_t) ( ( f.sequence >> 16 ) & 0xff ), (uint8_t) ( ( f.sequence >> 24 ) & 0xff ) };

    for ( int i = 0; i < f.payload_bytes; i++ )
    {
        out.push_back( i < 10 ? prefilter[i] : 0x00 );
    }

    for ( int i = 0; i < f.pad_bytes; i++ )
    {
        out.push_back( 0x00 );
    }

    if ( f.truncate_to >= 0 && (size_t) f.truncate_to < out.size() )
    {
        out.resize( (size_t) f.truncate_to );
    }

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
    memset( &config, 0, sizeof( config ) );
    if ( !c.any_ipv4 )
        memcpy( &config.public_address, xdp_host_server_ipv4, 4 );      // network order, as stored
    const uint8_t port_bytes[2] = { (uint8_t) ( c.port >> 8 ), (uint8_t) ( c.port & 0xff ) };
    memcpy( &config.public_port, port_bytes, 2 );                       // network order, as stored
    memcpy( config.packet_filter, xdp_host_filter_value, 4 );
    config.filter_epoch = c.epoch;
    if ( !c.any_ipv6 )
    {
        memcpy( config.public_address_ipv6, xdp_host_server_ipv6, 16 );
    }

    rocketnet_xdp_host_set_config( &config );
    rocketnet_xdp_host_set_config_lookup_fails( c.lookup_fails ? 1 : 0 );
}

// The replay map key of the client's address in a family, at a source port,
// built by the same function the transport's marks use.

static struct rocketnet_server_xdp_replay_key_t xdp_host_client_key( int family, uint16_t port )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );

    if ( family == 4 )
    {
        address.type = ROCKETNET_ADDRESS_IPV4;
        memcpy( address.data.ipv4, xdp_host_client_ipv4, 4 );
    }
    else
    {
        address.type = ROCKETNET_ADDRESS_IPV6;

        for ( int i = 0; i < 8; i++ )
        {
            address.data.ipv6[i] = (uint16_t) ( ( xdp_host_client_ipv6[i * 2] << 8 ) | xdp_host_client_ipv6[i * 2 + 1] );
        }
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
        case 0:  return "XDP_ABORTED";
        case 1:  return "XDP_DROP";
        case 2:  return "XDP_PASS";
        case 3:  return "XDP_TX";
        case 4:  return "XDP_REDIRECT";
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
    {
        rocketnet_xdp_host_set_socket_slot( (uint32_t) row.populated_slot, 1 );
    }

    if ( row.replay_entry )
    {
        const struct rocketnet_server_xdp_replay_key_t key = xdp_host_client_key( row.frame.family, xdp_host_client_port );
        rocketnet_xdp_host_set_replay_entry( &key, row.replay_high_water );
    }

    const std::vector<uint8_t> bytes = xdp_host_frame_bytes( row.frame );
    const int verdict = rocketnet_xdp_host_run( bytes.data(), (int) bytes.size(), row.queue );

    bool ok = verdict == row.want_verdict;
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        ok = ok && rocketnet_xdp_host_counter( i ) == ( i == row.want_counter ? 1u : 0u );
    }

    if ( !ok )
    {
        printf( "xdp_host row \"%s\": verdict %s (%d), want %s (%d); counters:",
                row.label.c_str(), xdp_host_verdict_name( verdict ), verdict,
                xdp_host_verdict_name( row.want_verdict ), row.want_verdict );

        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        {
            printf( " [%d]=%llu%s", i, (unsigned long long) rocketnet_xdp_host_counter( i ), i == row.want_counter ? " (want 1)" : "" );
        }

        printf( "\n" );
        fflush( stdout );
    }

    rocketnet_unit_check( verdict == row.want_verdict );

    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        rocketnet_unit_check( rocketnet_xdp_host_counter( i ) == ( i == row.want_counter ? 1u : 0u ) );
    }

    if ( row.want_counter >= 0 )
    {
        rocketnet_unit_check( row.want_counter < ROCKETNET_SERVER_XDP_NUM_COUNTERS );

        xdp_host_runtime_counter_mask |= (uint64_t) 1 << row.want_counter;
        xdp_host_runtime_counter_rows++;
    }

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
    {
        xdp_host_run_row( rows[i] );
    }
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

    const struct
    {
        uint8_t type;
        int bytes;
    } inbound[] = { { 0, 710 }, { 3, 50 }, { 4, 41 }, { 5, 34 }, { 6, 33 }, { 8, 33 } };

    for ( size_t i = 0; i < sizeof( inbound ) / sizeof( inbound[0] ); i++ )
    {
        xdp_host_row_t row = xdp_host_row( "an inbound type at its smallest size", family );
        row.label += " (type " + std::to_string( inbound[i].type ) + ")";
        row.frame.packet_type = inbound[i].type;
        row.frame.payload_bytes = inbound[i].bytes;
        rows.push_back( row );
    }
}

static void xdp_host_ipv4_rocketnet_packet_redirects_case()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_redirect_rows( 4, rows );
    xdp_host_run_rows( rows );
}

// The master registered unit executes every registered host-filter case in
// this file. xdp_host_run_row records a counter only after its verdict and
// complete counter table assertions pass, so the final mask is an execution
// result rather than a lexical inventory.
static void test_xdp_transport_host_ipv4_rocketnet_packet_redirects()
{
    xdp_host_runtime_counter_mask = 0;
    xdp_host_runtime_counter_rows = 0;
    xdp_host_ipv4_rocketnet_packet_redirects_case();

    const char host_prefix[] = "test_xdp_transport_host_";
    const char filter_prefix[] = "test_xdp_filter_host_";
    const int test_count = rocketnet_unit_tests_count();
    for ( int i = 0; i < test_count; i++ )
    {
        const char * test_name = rocketnet_unit_tests_name( i );
        rocketnet_unit_check( test_name != NULL );

        const bool host_case = strncmp( test_name, host_prefix, sizeof( host_prefix ) - 1 ) == 0;
        const bool filter_case = strncmp( test_name, filter_prefix, sizeof( filter_prefix ) - 1 ) == 0;
        if ( ( host_case || filter_case ) &&
             strcmp( test_name, "test_xdp_transport_host_ipv4_rocketnet_packet_redirects" ) != 0 )
        {
            struct rocketnet_unit_file_test_t * test = rocketnet_unit_file_find( test_name );
            rocketnet_unit_check( test != NULL );

            test->function();
        }
    }

    const uint64_t expected_mask = ( (uint64_t) 1 << ROCKETNET_SERVER_XDP_NUM_COUNTERS ) - 1;
    printf( "XDP runtime counter coverage: rows=%u mask=0x%llx expected=0x%llx counters=%d\n",
            xdp_host_runtime_counter_rows, (unsigned long long) xdp_host_runtime_counter_mask,
            (unsigned long long) expected_mask, ROCKETNET_SERVER_XDP_NUM_COUNTERS );
    fflush( stdout );

    rocketnet_unit_check( xdp_host_runtime_counter_mask == expected_mask );
    rocketnet_unit_check( xdp_host_runtime_counter_rows >= ROCKETNET_SERVER_XDP_NUM_COUNTERS );
}

static void test_xdp_transport_host_ipv6_rocketnet_packet_redirects()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_redirect_rows( 6, rows );
    xdp_host_run_rows( rows );
}

// Both configured destinations redirect. A third address passes.
static void test_xdp_transport_host_both_destinations_redirect_and_a_third_passes()
{
    std::vector<xdp_host_row_t> rows;
    rows.push_back( xdp_host_row( "ipv4 destination of a two-family config", 4 ) );
    rows.push_back( xdp_host_row( "ipv6 destination of a two-family config", 6 ) );

    static const uint8_t third[4] = { 192, 0, 2, 11 };
    xdp_host_row_t other = xdp_host_row( "a third address passes", 4 );
    other.frame.destination = third;
    rows.push_back( xdp_host_expect( other, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_redirect_needs_a_populated_slot()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };

    for ( int f = 0; f < 2; f++ )
    {
        // an empty slot: bpf_redirect_map returns the low bits of
        // BPF_F_INGRESS, XDP_DROP; REDIRECTED does not move and
        // DROPPED_NO_SOCKET counts the drop

        xdp_host_row_t empty = xdp_host_row( "no slot populated", families[f] );
        empty.populated_slot = -1;
        rows.push_back( xdp_host_expect( empty, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET ) ) );

        xdp_host_row_t other = xdp_host_row( "slot 2 populated, frame on queue 1", families[f] );
        other.queue = 1;
        other.populated_slot = 2;
        rows.push_back( xdp_host_expect( other, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET ) ) );

        xdp_host_row_t beyond = xdp_host_row( "queue index past the map", families[f] );
        beyond.queue = ROCKETNET_XDP_MAX_QUEUES;
        rows.push_back( xdp_host_expect( beyond, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET ) ) );

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
    rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET ) == 1 );

    (void) XDP_HOST_ABORTED;
}

// ---------------------------------------------------------------------------
// passes

static void test_xdp_transport_host_unconfigured_passes()
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

static void test_xdp_transport_host_other_port_passes()
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

static void test_xdp_transport_host_ipv4_wrong_address_passes()
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

static void test_xdp_transport_host_ipv6_wrong_address_passes()
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

// IPv4 options (ihl 6 to 15): RocketNet never sends them, and in XDP mode no
// kernel socket listens on the server's port, so a datagram to the
// server behind options drops counted DROPPED_IP_OPTIONS_OR_EXTENSION,
// before any size, epoch or filter check. With options to anything else it
// passes untouched, uncounted.

static void test_xdp_transport_host_ipv4_options_to_our_port_drop()
{
    std::vector<xdp_host_row_t> rows;
    const int dropped = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION );
    for ( int ihl = 6; ihl <= 15; ihl++ )
    {
        xdp_host_row_t row = xdp_host_row( "IPv4 with options to the server", 4 );
        row.label += " (ihl " + std::to_string( ihl ) + ")";
        row.frame.ipv4_ihl = ihl;
        rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, dropped ) );
    }

    xdp_host_row_t any = xdp_host_row( "IPv4 with options, any-address config", 4 );
    any.frame.ipv4_ihl = 6;
    any.config.any_ipv4 = true;
    rows.push_back( xdp_host_expect( any, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t epoch = xdp_host_row( "IPv4 with options to the server, wrong epoch", 4 );
    epoch.frame.ipv4_ihl = 6;
    epoch.frame.epoch = 9;
    rows.push_back( xdp_host_expect( epoch, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t large = xdp_host_row( "IPv4 with options to the server past the receive ceiling", 4 );
    large.frame.ipv4_ihl = 6;
    large.frame.payload_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    rows.push_back( xdp_host_expect( large, XDP_HOST_DROP, dropped ) );

    // the same datagram with no options is a RocketNet packet

    rows.push_back( xdp_host_row( "the same datagram without options", 4 ) );

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_ipv4_options_elsewhere_pass()
{
    std::vector<xdp_host_row_t> rows;
    for ( int ihl = 6; ihl <= 15; ihl++ )
    {
        xdp_host_row_t row = xdp_host_row( "IPv4 with options to another port", 4 );
        row.label += " (ihl " + std::to_string( ihl ) + ")";
        row.frame.ipv4_ihl = ihl;
        row.frame.port = 53;
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }

    xdp_host_row_t address = xdp_host_row( "IPv4 with options to another address", 4 );
    address.frame.ipv4_ihl = 6;
    address.frame.destination = xdp_host_other_ipv4;
    rows.push_back( xdp_host_expect( address, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t tcp = xdp_host_row( "IPv4 with options carrying TCP", 4 );
    tcp.frame.ipv4_ihl = 6;
    tcp.frame.protocol = 6;
    rows.push_back( xdp_host_expect( tcp, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t unconfigured = xdp_host_row( "IPv4 with options to an unconfigured server", 4 );
    unconfigured.frame.ipv4_ihl = 6;
    unconfigured.config.port = 0;
    rows.push_back( xdp_host_expect( unconfigured, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t lookup = xdp_host_row( "IPv4 with options when the config lookup fails", 4 );
    lookup.frame.ipv4_ihl = 6;
    lookup.config.lookup_fails = true;
    rows.push_back( xdp_host_expect( lookup, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // the UDP header cut by the frame end: nothing names the port

    xdp_host_row_t cut = xdp_host_row( "IPv4 with options, UDP header cut by the frame end", 4 );
    cut.frame.ipv4_ihl = 15;
    cut.frame.truncate_to = 14 + 60 + 7;
    rows.push_back( xdp_host_expect( cut, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_row_t bare = xdp_host_row( "IPv4 with options, nothing behind them", 4 );
    bare.frame.ipv4_ihl = 8;
    bare.frame.truncate_to = 14 + 32;
    rows.push_back( xdp_host_expect( bare, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    xdp_host_run_rows( rows );
}

// ---------------------------------------------------------------------------
// short headers: a frame that names an IP family but cannot hold its fixed
// header drops, uncounted

static void test_xdp_transport_host_ipv4_short_header_drops()
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

static void test_xdp_transport_host_ipv6_short_header_drops()
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

// RFC 8200 section 4, by name: hop-by-hop options, routing, destination
// options and the authentication header carry their own next header, so
// the filter walks the chain (SERVER_XDP_IPV6_EXTENSION_WALK headers at
// most). A datagram to the server behind them drops counted
// DROPPED_IP_OPTIONS_OR_EXTENSION, before any size, epoch or filter check:
// RocketNet never sends extension headers, and in XDP mode no kernel socket
// listens on the server's port.

static const struct
{
    int type;
    const char * name;
} xdp_host_ipv6_extensions[] = {
    { 0, "hop-by-hop options" },
    { 43, "routing" },
    { 60, "destination options" },
    { 51, "authentication header" },
};

static void test_xdp_transport_host_ipv6_extension_headers_to_our_port_drop()
{
    std::vector<xdp_host_row_t> rows;
    const int dropped = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION );

    for ( size_t i = 0; i < sizeof( xdp_host_ipv6_extensions ) / sizeof( xdp_host_ipv6_extensions[0] ); i++ )
    {
        xdp_host_row_t row = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        row.label += " to the server";
        row.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        rows.push_back( xdp_host_expect( row, XDP_HOST_DROP, dropped ) );

        xdp_host_row_t any = row;
        any.label += ", any-address config";
        any.config.any_ipv6 = true;
        rows.push_back( xdp_host_expect( any, XDP_HOST_DROP, dropped ) );

        xdp_host_row_t filter = row;
        filter.label += ", wrong filter value";
        filter.frame.filter[0] = 0x00;
        rows.push_back( xdp_host_expect( filter, XDP_HOST_DROP, dropped ) );

        xdp_host_row_t large = row;
        large.label += ", past the receive ceiling";
        large.frame.payload_bytes = ROCKETNET_RECEIVE_CEILING + 1;
        rows.push_back( xdp_host_expect( large, XDP_HOST_DROP, dropped ) );
    }

    // the longest chain the walk follows, and a first fragment header
    // (offset 0, more fragments) inside a chain

    xdp_host_row_t chain = xdp_host_row( "a chain of four extension headers to the server", 6 );
    chain.frame.ipv6_extensions = { 0, 60, 43, 51 };
    rows.push_back( xdp_host_expect( chain, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t first = xdp_host_row( "hop-by-hop then a first fragment header, to the server", 6 );
    first.frame.ipv6_extensions = { 0, 44 | ( 0x0001 << 8 ) };
    rows.push_back( xdp_host_expect( first, XDP_HOST_DROP, dropped ) );

    // the same datagram with UDP right after the fixed header is a
    // RocketNet packet

    rows.push_back( xdp_host_row( "the same datagram without extension headers", 6 ) );

    xdp_host_run_rows( rows );
}

// every other extension-header frame passes to the kernel stack counted
// PASSED_IPV6_EXTENSION: not addressed to the server, behind ESP, behind a
// non-first fragment header, a chain longer than the walk, or cut short

static void test_xdp_transport_host_ipv6_extension_headers_elsewhere_pass_counted()
{
    std::vector<xdp_host_row_t> rows;
    const int passed = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );

    for ( size_t i = 0; i < sizeof( xdp_host_ipv6_extensions ) / sizeof( xdp_host_ipv6_extensions[0] ); i++ )
    {
        xdp_host_row_t port = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        port.label += " to another port";
        port.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        port.frame.port = 53;
        rows.push_back( xdp_host_expect( port, XDP_HOST_PASS, passed ) );

        xdp_host_row_t address = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        address.label += " to another address";
        address.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        address.frame.destination = xdp_host_other_ipv6;
        rows.push_back( xdp_host_expect( address, XDP_HOST_PASS, passed ) );

        xdp_host_row_t tcp = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        tcp.label += " carrying TCP";
        tcp.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        tcp.frame.protocol = 6;
        rows.push_back( xdp_host_expect( tcp, XDP_HOST_PASS, passed ) );

        xdp_host_row_t unconfigured = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        unconfigured.label += " to an unconfigured server";
        unconfigured.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        unconfigured.config.port = 0;
        rows.push_back( xdp_host_expect( unconfigured, XDP_HOST_PASS, passed ) );

        xdp_host_row_t bare = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        bare.label += " with nothing behind the fixed header";
        bare.frame.protocol = (uint8_t) xdp_host_ipv6_extensions[i].type;
        bare.frame.truncate_to = 14 + 40;
        rows.push_back( xdp_host_expect( bare, XDP_HOST_PASS, passed ) );

        xdp_host_row_t cut = xdp_host_row( xdp_host_ipv6_extensions[i].name, 6 );
        cut.label += ", UDP header cut by the frame end";
        cut.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        cut.frame.truncate_to = 14 + 40 + ( xdp_host_ipv6_extensions[i].type == 51 ? 12 : 8 ) + 7;
        rows.push_back( xdp_host_expect( cut, XDP_HOST_PASS, passed ) );
    }

    // ESP: the ciphertext names no port, even when the bytes behind the ESP
    // header read as UDP to the server

    xdp_host_row_t esp = xdp_host_row( "encapsulating security payload, UDP-looking bytes to the server's port", 6 );
    esp.frame.ipv6_extensions.push_back( 50 );
    rows.push_back( xdp_host_expect( esp, XDP_HOST_PASS, passed ) );

    xdp_host_row_t behind_esp = xdp_host_row( "hop-by-hop then ESP", 6 );
    behind_esp.frame.ipv6_extensions = { 0, 50 };
    rows.push_back( xdp_host_expect( behind_esp, XDP_HOST_PASS, passed ) );

    // a non-first fragment names no port either

    xdp_host_row_t middle = xdp_host_row( "hop-by-hop then a middle fragment header", 6 );
    middle.frame.ipv6_extensions = { 0, 44 | ( ( ( 185 << 3 ) | 1 ) << 8 ) };
    rows.push_back( xdp_host_expect( middle, XDP_HOST_PASS, passed ) );

    // one header past the walk's bound

    xdp_host_row_t longer = xdp_host_row( "a chain of five extension headers", 6 );
    longer.frame.ipv6_extensions = { 0, 60, 43, 60, 60 };
    rows.push_back( xdp_host_expect( longer, XDP_HOST_PASS, passed ) );

    // a hop-by-hop header claiming more bytes than the frame holds

    xdp_host_row_t overlong = xdp_host_row( "hop-by-hop header whose length runs past the frame", 6 );
    overlong.frame.protocol = 0;
    overlong.frame.source_port = 0x11ff;    // read as next header 17, length 255 units
    rows.push_back( xdp_host_expect( overlong, XDP_HOST_PASS, passed ) );

    xdp_host_row_t lookup = xdp_host_row( "hop-by-hop to the server when the config lookup fails", 6 );
    lookup.frame.ipv6_extensions.push_back( 0 );
    lookup.config.lookup_fails = true;
    rows.push_back( xdp_host_expect( lookup, XDP_HOST_PASS, passed ) );

    xdp_host_run_rows( rows );
}

// The fragment rows of a family: the first fragment of a datagram to this
// server drops counted DROPPED_FIRST_FRAGMENT, before any size, epoch or
// filter check; every other fragment passes counted PASSED_FRAGMENT. IPv4
// fragments are flags and offset in frag_off (RFC 791); IPv6 fragments carry
// a real fragment header (RFC 8200 section 4.5) between the fixed header and
// the bytes that follow it.

// a first fragment (offset 0, more-fragments set) in a family's own field

static xdp_host_row_t xdp_host_fragment_row( const char * label, int family, int offset_units, bool more )
{
    xdp_host_row_t row = xdp_host_row( label, family );
    if ( family == 4 )
        row.frame.ipv4_frag_off = (uint16_t) ( ( more ? 0x2000 : 0 ) | ( offset_units & 0x1fff ) );
    else
        row.frame.ipv6_fragment = ( offset_units << 3 ) | ( more ? 1 : 0 );
    return row;
}

static void xdp_host_first_fragment_drop_rows( int family, std::vector<xdp_host_row_t> & rows )
{
    const int dropped = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT );

    rows.push_back( xdp_host_expect( xdp_host_fragment_row( "first fragment to the server", family, 0, true ), XDP_HOST_DROP, dropped ) );

    xdp_host_row_t wrong_epoch = xdp_host_fragment_row( "first fragment to the server with a wrong epoch", family, 0, true );
    wrong_epoch.frame.epoch = 9;
    rows.push_back( xdp_host_expect( wrong_epoch, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t wrong_filter = xdp_host_fragment_row( "first fragment to the server with a wrong filter value", family, 0, true );
    wrong_filter.frame.filter[0] = 0x00;
    rows.push_back( xdp_host_expect( wrong_filter, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t tiny = xdp_host_fragment_row( "first fragment to the server with a 1-byte payload", family, 0, true );
    tiny.frame.payload_bytes = 1;
    rows.push_back( xdp_host_expect( tiny, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t large = xdp_host_fragment_row( "first fragment to the server past the receive ceiling", family, 0, true );
    large.frame.payload_bytes = ROCKETNET_RECEIVE_CEILING + 1;
    rows.push_back( xdp_host_expect( large, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t invalid = xdp_host_fragment_row( "first fragment to the server of an outbound type", family, 0, true );
    invalid.frame.packet_type = 2;
    rows.push_back( xdp_host_expect( invalid, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t replayed = xdp_host_fragment_row( "first fragment to the server behind the replay window", family, 0, true );
    replayed.frame.sequence = 0;
    replayed.replay_entry = true;
    replayed.replay_high_water = 1000;
    rows.push_back( xdp_host_expect( replayed, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t any = xdp_host_fragment_row( "first fragment with the any-address config", family, 0, true );
    any.config.any_ipv4 = true;
    any.config.any_ipv6 = true;
    rows.push_back( xdp_host_expect( any, XDP_HOST_DROP, dropped ) );

    xdp_host_row_t empty = xdp_host_fragment_row( "first fragment to the server, no socket slot", family, 0, true );
    empty.populated_slot = -1;
    rows.push_back( xdp_host_expect( empty, XDP_HOST_DROP, dropped ) );
}

static void xdp_host_fragment_pass_rows( int family, std::vector<xdp_host_row_t> & rows )
{
    const int passed = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT );

    // non-first fragments, whatever their bytes look like: the 8 bytes after
    // the IP header here read as a UDP header to the server's port

    rows.push_back( xdp_host_expect( xdp_host_fragment_row( "middle fragment to the server's port", family, 185, true ), XDP_HOST_PASS, passed ) );

    xdp_host_row_t last = xdp_host_fragment_row( "last fragment to the server's port", family, 370, false );
    last.frame.payload_bytes = 1;
    rows.push_back( xdp_host_expect( last, XDP_HOST_PASS, passed ) );

    xdp_host_row_t middle_other = xdp_host_fragment_row( "middle fragment to another port", family, 185, true );
    middle_other.frame.port = 53;
    rows.push_back( xdp_host_expect( middle_other, XDP_HOST_PASS, passed ) );

    // first fragments that are not this server's

    xdp_host_row_t other_port = xdp_host_fragment_row( "first fragment to another port", family, 0, true );
    other_port.frame.port = 53;
    rows.push_back( xdp_host_expect( other_port, XDP_HOST_PASS, passed ) );

    xdp_host_row_t other_address = xdp_host_fragment_row( "first fragment to another address", family, 0, true );
    other_address.frame.destination = family == 4 ? xdp_host_other_ipv4 : xdp_host_other_ipv6;
    rows.push_back( xdp_host_expect( other_address, XDP_HOST_PASS, passed ) );

    xdp_host_row_t unconfigured = xdp_host_fragment_row( "first fragment to an unconfigured server", family, 0, true );
    unconfigured.config.port = 0;
    rows.push_back( xdp_host_expect( unconfigured, XDP_HOST_PASS, passed ) );

    xdp_host_row_t no_config = xdp_host_fragment_row( "first fragment when the config lookup fails", family, 0, true );
    no_config.config.lookup_fails = true;
    rows.push_back( xdp_host_expect( no_config, XDP_HOST_PASS, passed ) );

    // a first fragment whose UDP header is cut by data_end: the program
    // cannot say it is ours

    const int ip_bytes = family == 4 ? 20 : 40 + 8;
    xdp_host_row_t cut = xdp_host_fragment_row( "first fragment with its UDP header cut by the frame end", family, 0, true );
    cut.frame.truncate_to = 14 + ip_bytes + 7;
    rows.push_back( xdp_host_expect( cut, XDP_HOST_PASS, passed ) );

    xdp_host_row_t bare = xdp_host_fragment_row( "first fragment with nothing behind the IP header", family, 0, true );
    bare.frame.truncate_to = 14 + ip_bytes;
    rows.push_back( xdp_host_expect( bare, XDP_HOST_PASS, passed ) );

    // a whole datagram to the server is no fragment: it redirects

    if ( family == 4 )
    {
        xdp_host_row_t df = xdp_host_row( "DF set, offset zero: a whole datagram", 4 );
        rows.push_back( df );

        xdp_host_row_t whole = xdp_host_row( "no flags, offset zero: a whole datagram", 4 );
        whole.frame.ipv4_frag_off = 0x0000;
        rows.push_back( whole );
    }
    else
    {
        rows.push_back( xdp_host_row( "UDP directly after the fixed header: a whole datagram", 6 ) );
    }
}

static void test_xdp_transport_host_ipv4_first_fragment_drops_counted()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_first_fragment_drop_rows( 4, rows );

    // DF with more-fragments is still a first fragment by its offset and M
    // bit: frag_off & 0x3fff is 0x2000 either way

    xdp_host_row_t df_more = xdp_host_row( "DF and more-fragments, offset 0", 4 );
    df_more.frame.ipv4_frag_off = 0x6000;
    rows.push_back( xdp_host_expect( df_more, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT ) ) );

    // IP options in front of the UDP header move it, and the first
    // fragment is still found to be the server's

    for ( int ihl = 6; ihl <= 15; ihl += 9 )
    {
        xdp_host_row_t options = xdp_host_fragment_row( "first fragment with IP options to the server", 4, 0, true );
        options.label += " (ihl " + std::to_string( ihl ) + ")";
        options.frame.ipv4_ihl = ihl;
        rows.push_back( xdp_host_expect( options, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT ) ) );
    }

    xdp_host_row_t options_other = xdp_host_fragment_row( "first fragment with IP options to another port", 4, 0, true );
    options_other.frame.ipv4_ihl = 6;
    options_other.frame.port = 53;
    rows.push_back( xdp_host_expect( options_other, XDP_HOST_PASS, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT ) ) );

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_ipv6_first_fragment_drops_counted()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_first_fragment_drop_rows( 6, rows );

    const int dropped = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT );
    for ( size_t i = 0; i < sizeof( xdp_host_ipv6_extensions ) / sizeof( xdp_host_ipv6_extensions[0] ); i++ )
    {
        xdp_host_row_t extension = xdp_host_fragment_row( "first fragment with an extension header to the server", 6, 0, true );
        extension.label += ": ";
        extension.label += xdp_host_ipv6_extensions[i].name;
        extension.frame.ipv6_fragment_next = (uint8_t) xdp_host_ipv6_extensions[i].type;
        extension.frame.ipv6_extensions.push_back( xdp_host_ipv6_extensions[i].type );
        rows.push_back( xdp_host_expect( extension, XDP_HOST_DROP, dropped ) );
    }

    xdp_host_row_t chain = xdp_host_fragment_row( "first fragment with four extension headers to the server", 6, 0, true );
    chain.frame.ipv6_fragment_next = 0;
    chain.frame.ipv6_extensions = { 0, 60, 43, 51 };
    rows.push_back( xdp_host_expect( chain, XDP_HOST_DROP, dropped ) );

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_ipv6_fragment_passes_counted()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_fragment_pass_rows( 6, rows );

    const int passed = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT );

    // a fragment header whose own 8 bytes are cut by the frame end, or
    // with nothing behind the fixed header

    xdp_host_row_t header_cut = xdp_host_fragment_row( "fragment header cut by the frame end", 6, 0, true );
    header_cut.frame.truncate_to = 14 + 40 + 7;
    rows.push_back( xdp_host_expect( header_cut, XDP_HOST_PASS, passed ) );

    xdp_host_row_t header_bare = xdp_host_fragment_row( "fragment next header with nothing behind the fixed header", 6, 0, true );
    header_bare.frame.truncate_to = 14 + 40;
    rows.push_back( xdp_host_expect( header_bare, XDP_HOST_PASS, passed ) );

    // a first fragment of something other than UDP, and an atomic
    // fragment (offset 0, M clear: RFC 6946), are not first fragments of a
    // datagram to this server

    xdp_host_row_t tcp = xdp_host_fragment_row( "first fragment of a TCP segment", 6, 0, true );
    tcp.frame.ipv6_fragment_next = 6;
    rows.push_back( xdp_host_expect( tcp, XDP_HOST_PASS, passed ) );

    xdp_host_row_t atomic = xdp_host_fragment_row( "atomic fragment to the server's port", 6, 0, false );
    rows.push_back( xdp_host_expect( atomic, XDP_HOST_PASS, passed ) );

    // A fixed-header fragment remains a fragment while the bounded walker
    // looks through headers after it. Only offset-zero, more-fragments UDP
    // to this server drops; every other outcome keeps the fragment counter.

    xdp_host_row_t other_port = xdp_host_fragment_row( "first fragment with extensions to another port", 6, 0, true );
    other_port.frame.ipv6_fragment_next = 0;
    other_port.frame.ipv6_extensions = { 0, 60 };
    other_port.frame.port = 53;
    rows.push_back( xdp_host_expect( other_port, XDP_HOST_PASS, passed ) );

    xdp_host_row_t other_address = xdp_host_fragment_row( "first fragment with extensions to another address", 6, 0, true );
    other_address.frame.ipv6_fragment_next = 43;
    other_address.frame.ipv6_extensions = { 43, 51 };
    other_address.frame.destination = xdp_host_other_ipv6;
    rows.push_back( xdp_host_expect( other_address, XDP_HOST_PASS, passed ) );

    xdp_host_row_t other_service = xdp_host_fragment_row( "first fragment with extensions carrying ICMPv6", 6, 0, true );
    other_service.frame.ipv6_fragment_next = 0;
    other_service.frame.ipv6_extensions = { 0, 60 };
    other_service.frame.protocol = 58;
    rows.push_back( xdp_host_expect( other_service, XDP_HOST_PASS, passed ) );

    xdp_host_row_t middle_extensions = xdp_host_fragment_row( "middle fragment with extension-looking bytes", 6, 185, true );
    middle_extensions.frame.ipv6_fragment_next = 0;
    middle_extensions.frame.ipv6_extensions = { 0, 60 };
    rows.push_back( xdp_host_expect( middle_extensions, XDP_HOST_PASS, passed ) );

    xdp_host_row_t atomic_extensions = xdp_host_fragment_row( "atomic fragment with extensions to the server", 6, 0, false );
    atomic_extensions.frame.ipv6_fragment_next = 0;
    atomic_extensions.frame.ipv6_extensions = { 0, 60 };
    rows.push_back( xdp_host_expect( atomic_extensions, XDP_HOST_PASS, passed ) );

    xdp_host_row_t esp = xdp_host_fragment_row( "first fragment followed by ESP", 6, 0, true );
    esp.frame.ipv6_fragment_next = 50;
    esp.frame.ipv6_extensions = { 50 };
    rows.push_back( xdp_host_expect( esp, XDP_HOST_PASS, passed ) );

    xdp_host_row_t nested = xdp_host_fragment_row( "first fragment followed by an atomic fragment header", 6, 0, true );
    nested.frame.ipv6_fragment_next = 44;
    nested.frame.ipv6_extensions = { 44 };
    rows.push_back( xdp_host_expect( nested, XDP_HOST_PASS, passed ) );

    xdp_host_row_t extension_cut = xdp_host_fragment_row( "first fragment with its extension header cut", 6, 0, true );
    extension_cut.frame.ipv6_fragment_next = 0;
    extension_cut.frame.ipv6_extensions = { 0 };
    extension_cut.frame.truncate_to = 14 + 40 + 8 + 1;
    rows.push_back( xdp_host_expect( extension_cut, XDP_HOST_PASS, passed ) );

    xdp_host_row_t udp_cut = xdp_host_fragment_row( "first fragment with an extension and its UDP header cut", 6, 0, true );
    udp_cut.frame.ipv6_fragment_next = 60;
    udp_cut.frame.ipv6_extensions = { 60 };
    udp_cut.frame.truncate_to = 14 + 40 + 8 + 8 + 7;
    rows.push_back( xdp_host_expect( udp_cut, XDP_HOST_PASS, passed ) );

    xdp_host_row_t overlong = xdp_host_fragment_row( "first fragment with an extension length beyond the frame", 6, 0, true );
    overlong.frame.ipv6_fragment_next = 0;
    overlong.frame.ipv6_extensions = { 0 };
    overlong.frame.ipv6_first_extension_length = 255;
    const std::vector<uint8_t> overlong_bytes = xdp_host_frame_bytes( overlong.frame );
    rocketnet_unit_check( overlong_bytes[14 + 40 + 8 + 1] == 255 );

    rows.push_back( xdp_host_expect( overlong, XDP_HOST_PASS, passed ) );

    xdp_host_row_t walk_limit = xdp_host_fragment_row( "first fragment with five extension headers", 6, 0, true );
    walk_limit.frame.ipv6_fragment_next = 0;
    walk_limit.frame.ipv6_extensions = { 0, 60, 43, 60, 51 };
    rows.push_back( xdp_host_expect( walk_limit, XDP_HOST_PASS, passed ) );

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_ipv4_fragment_passes_counted()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_fragment_pass_rows( 4, rows );

    const int passed = XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT );

    // a non-first fragment with IP options names no port: it passes; so
    // does a first fragment of something other than UDP

    xdp_host_row_t options = xdp_host_fragment_row( "middle fragment with IP options (ihl 6) to the server's port", 4, 185, true );
    options.frame.ipv4_ihl = 6;
    rows.push_back( xdp_host_expect( options, XDP_HOST_PASS, passed ) );

    xdp_host_row_t tcp = xdp_host_fragment_row( "first fragment of a TCP segment", 4, 0, true );
    tcp.frame.protocol = 6;
    rows.push_back( xdp_host_expect( tcp, XDP_HOST_PASS, passed ) );

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_icmpv6_passes_uncounted()
{
    std::vector<xdp_host_row_t> rows;

    xdp_host_row_t icmp = xdp_host_row( "ICMPv6", 6 );
    icmp.frame.protocol = 58;
    rows.push_back( xdp_host_expect( icmp, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );

    // every other next header that is not UDP and not an extension header

    const uint8_t others[] = { 6, 41, 59, 135, 255 };

    for ( size_t i = 0; i < sizeof( others ); i++ )
    {
        xdp_host_row_t row = xdp_host_row( "another next header", 6 );
        row.label += " (" + std::to_string( others[i] ) + ")";
        row.frame.protocol = others[i];
        rows.push_back( xdp_host_expect( row, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }

    xdp_host_run_rows( rows );
}

static void test_xdp_transport_host_vlan_tagged_passes()
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

static void test_xdp_transport_host_too_small_drops()
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

static void test_xdp_transport_host_too_large_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };

    for ( int f = 0; f < 2; f++ )
    {
        // the receive ceiling is 1452, one 1500-MTU IPv6 datagram's payload,
        // for both families

        xdp_host_row_t ceiling = xdp_host_row( "a payload packet at the ceiling, 1452 bytes", families[f] );
        ceiling.frame.payload_bytes = 1452;
        rows.push_back( ceiling );

        const int sizes[2] = { 1453, 2000 };

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

static void test_xdp_transport_host_wrong_epoch_drops()
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

static void test_xdp_transport_host_wrong_filter_drops()
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

static void test_xdp_transport_host_invalid_type_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };

    for ( int f = 0; f < 2; f++ )
    {
        // server-to-client types at their own exact sizes, reserved types,
        // and inbound types one byte off their size

        const struct
        {
            uint8_t type;
            int bytes;
            const char * name;
        } refused[] = {
            { 1, 34, "connection denied" },
            { 2, 66, "connection challenge" },
            { 7, 38, "connection cookie" },
            { 9, 40, "reserved type 9" },
            { 9, 1452, "reserved type 9 at the receive ceiling" },
            { 10, 100, "reserved type 10" },
            { 255, 100, "reserved type 255" },
            { 0, 709, "connection request one byte short" },
            { 0, 711, "connection request one byte long" },
            { 3, 49, "connection response one byte short" },
            { 3, 51, "connection response one byte long" },
            { 4, 40, "keep-alive below its minimum" },
            { 5, 33, "payload below its minimum" },
        };

        for ( size_t i = 0; i < sizeof( refused ) / sizeof( refused[0] ); i++ )
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
// STANDARD.md (Packet sizes; Reading packets: the server ignores denied,
// challenge and cookie, and types 9 to 255 are reserved). The table is
// written here, never taken from ROCKETNET_WIRE_SERVER_PACKET_VALID.

static void test_xdp_transport_host_type_and_size_sweep_matches_the_standard()
{
    const int ceiling = 1452;   // 1500 - 40 - 8

    struct inbound_t
    {
        int type;
        int min_bytes;
        int max_bytes;
    };
    const inbound_t standard[] = {
        { 0, 710, 710 },        // connection request
        { 3, 50, 50 },          // connection response
        { 4, 41, ceiling },     // keep-alive
        { 5, 34, ceiling },     // payload
        { 6, 33, ceiling },     // disconnect
        { 8, 33, ceiling },     // ack
    };
    const int standard_rows = (int) ( sizeof( standard ) / sizeof( standard[0] ) );

    int floor = ceiling;
    for ( int i = 0; i < standard_rows; i++ )
    {
        floor = standard[i].min_bytes < floor ? standard[i].min_bytes : floor;
    }

    rocketnet_unit_check( floor == 33 );

    // the first reserved type is 9, and the shared inbound rule accepts it
    // at no size

    rocketnet_unit_check( ROCKETNET_FIRST_RESERVED_PACKET_TYPE == 9 );
    for ( int bytes = 0; bytes <= ceiling + 1; bytes++ )
    {
        rocketnet_unit_check( !ROCKETNET_WIRE_SERVER_PACKET_VALID( ROCKETNET_FIRST_RESERVED_PACKET_TYPE, bytes ) );
    }

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
                {
                    image[type_offset] = (uint8_t) type;
                }

                bool valid = false;
                for ( int i = 0; i < standard_rows; i++ )
                {
                    valid = valid || ( standard[i].type == type && bytes >= standard[i].min_bytes && bytes <= standard[i].max_bytes );
                }

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
                {
                    ok = ok && rocketnet_xdp_host_counter( i ) == want_counters[i];
                }

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

    // 2 families x 256 types x 1454 sizes, and the accepted set is exactly
    // the table's: 2 x ( 1 + 1 + 1412 + 1419 + 1420 + 1420 )

    rocketnet_unit_check( runs == 2L * 256 * ( ceiling + 2 ) );
    rocketnet_unit_check( redirects == 2L * ( 1 + 1 + 1412 + 1419 + 1420 + 1420 ) );
}

// ---------------------------------------------------------------------------
// replay: the read-only high-water map (proofs/tla/RNXdpReplay.tla, KernelFilter).
// With an entry for the source address and port, a session packet whose
// sequence is ROCKETNET_XDP_REPLAY_WINDOW or more behind the high-water
// drops; inside the window or ahead it goes on; handshake types and sources
// with no entry are not checked. The kernel window is the userspace replay
// window (ROCKETNET_REPLAY_WINDOW_BITS, 4096 by default), so the kernel
// never drops a reordered packet that userspace would accept.

static_assert( ROCKETNET_XDP_REPLAY_WINDOW == ROCKETNET_REPLAY_WINDOW_BITS, "the kernel replay window is the userspace replay window" );

static const uint32_t xdp_host_replay_high_water = 2u * ROCKETNET_XDP_REPLAY_WINDOW;

static uint32_t xdp_host_replay_behind( uint32_t distance )
{
    return xdp_host_replay_high_water - distance;
}

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
    const uint32_t window = ROCKETNET_XDP_REPLAY_WINDOW;
    const uint32_t high_water = xdp_host_replay_high_water;

    rows.push_back( xdp_host_expect( xdp_host_replay_row( "payload a window + 1 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                          xdp_host_replay_behind( window + 1 ), high_water ),
                                     XDP_HOST_DROP, replayed ) );
    rows.push_back( xdp_host_expect( xdp_host_replay_row( "payload a window behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                          xdp_host_replay_behind( window ), high_water ),
                                     XDP_HOST_DROP, replayed ) );
    rows.push_back( xdp_host_replay_row( "payload a window - 1 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                         xdp_host_replay_behind( window - 1 ), high_water ) );
    rows.push_back( xdp_host_replay_row( "payload 4000 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                         xdp_host_replay_behind( 4000 ), high_water ) );
    rows.push_back( xdp_host_replay_row( "payload 16 behind the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                         xdp_host_replay_behind( 16 ), high_water ) );
    rows.push_back( xdp_host_replay_row( "payload at the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, high_water, high_water ) );
    rows.push_back( xdp_host_replay_row( "payload one ahead of the high-water", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, high_water + 1, high_water ) );

    const struct
    {
        uint8_t type;
        const char * label;
    } session[] = {
        { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, "keep-alive a window behind the high-water" },
        { ROCKETNET_CONNECTION_ACK_PACKET, "ack a window behind the high-water" },
        { ROCKETNET_CONNECTION_DISCONNECT_PACKET, "disconnect a window behind the high-water" },
    };
    for ( size_t i = 0; i < sizeof( session ) / sizeof( session[0] ); i++ )
        rows.push_back( xdp_host_expect( xdp_host_replay_row( session[i].label, family, session[i].type, xdp_host_replay_behind( window ), high_water ),
                                         XDP_HOST_DROP, replayed ) );

    // the handshake types are not checked

    rows.push_back( xdp_host_replay_row( "connection request a window behind is not checked", family, ROCKETNET_CONNECTION_REQUEST_PACKET,
                                         xdp_host_replay_behind( window ), high_water ) );
    rows.push_back( xdp_host_replay_row( "connection response a window behind is not checked", family, ROCKETNET_CONNECTION_RESPONSE_PACKET,
                                         xdp_host_replay_behind( window ), high_water ) );

    // another source port has no entry

    xdp_host_row_t other = xdp_host_replay_row( "another source port has no entry", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                xdp_host_replay_behind( window ), high_water );
    other.frame.source_port = xdp_host_client_port + 1;
    rows.push_back( other );

    // no entry at all

    xdp_host_row_t none = xdp_host_replay_row( "no replay entry", family, ROCKETNET_CONNECTION_PAYLOAD_PACKET, xdp_host_replay_behind( window ), high_water );
    none.replay_entry = false;
    rows.push_back( none );

    // the wrap: the difference is modulo 2^32, read as signed

    rows.push_back( xdp_host_expect( xdp_host_replay_row( "high-water 5, sequence a window behind is behind across the wrap", family,
                                                          ROCKETNET_CONNECTION_PAYLOAD_PACKET, 5u - window, 5 ),
                                     XDP_HOST_DROP, replayed ) );
    rows.push_back( xdp_host_replay_row( "high-water 5, sequence 0xfffffff0 is inside the window across the wrap", family,
                                         ROCKETNET_CONNECTION_PAYLOAD_PACKET, 0xfffffff0u, 5 ) );
    rows.push_back( xdp_host_replay_row( "high-water 0xfffffff0, sequence 5 is ahead across the wrap", family,
                                         ROCKETNET_CONNECTION_PAYLOAD_PACKET, 5, 0xfffffff0u ) );
}

static void test_xdp_transport_host_replay_drops_behind_the_window()
{
    std::vector<xdp_host_row_t> rows;
    xdp_host_replay_rows( 4, rows );
    xdp_host_replay_rows( 6, rows );
    xdp_host_run_rows( rows );
}

// The program never writes the map: one entry at a high-water a window past
// sequence 84, then a forged payload far ahead and a hundred more session
// packets through the same state, each asserting its verdict and that
// exactly its counter moved by one, and the entry still reads its
// high-water with no other entry made.

static void test_xdp_transport_host_replay_never_writes_the_map()
{
    const int families[2] = { 4, 6 };
    const uint8_t session[4] = { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                 ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET };

    for ( int f = 0; f < 2; f++ )
    {
        rocketnet_xdp_host_reset();
        xdp_host_apply_config( xdp_host_config_t() );
        rocketnet_xdp_host_set_socket_slot( 0, 1 );

        const struct rocketnet_server_xdp_replay_key_t key = xdp_host_client_key( families[f], xdp_host_client_port );
        const uint32_t entry_high_water = ROCKETNET_XDP_REPLAY_WINDOW + 84u;
        rocketnet_xdp_host_set_replay_entry( &key, entry_high_water );

        for ( int i = -1; i < 100; i++ )
        {
            xdp_host_frame_t frame;
            frame.family = families[f];
            frame.packet_type = i < 0 ? ROCKETNET_CONNECTION_PAYLOAD_PACKET : session[i % 4];
            frame.sequence = i < 0 ? 0x7fffffffu : (uint32_t) ( i * 3 );
            const bool behind = (int32_t) ( entry_high_water - frame.sequence ) >= ROCKETNET_XDP_REPLAY_WINDOW;
            const int want_verdict = behind ? XDP_HOST_DROP : XDP_HOST_REDIRECT;
            const int want_counter = behind ? XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED )
                                            : XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );

            uint64_t before[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
            for ( int c = 0; c < ROCKETNET_SERVER_XDP_NUM_COUNTERS; c++ )
            {
                before[c] = rocketnet_xdp_host_counter( c );
            }

            const std::vector<uint8_t> bytes = xdp_host_frame_bytes( frame );
            const int verdict = rocketnet_xdp_host_run( bytes.data(), (int) bytes.size(), 0 );

            bool ok = verdict == want_verdict;
            for ( int c = 0; c < ROCKETNET_SERVER_XDP_NUM_COUNTERS; c++ )
            {
                ok = ok && rocketnet_xdp_host_counter( c ) - before[c] == ( c == want_counter ? 1u : 0u );
            }

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
            rocketnet_unit_check( high_water == entry_high_water );
        }

        // 1 forged redirect, then rows 0..28 (sequence 0..84) drop and rows
        // 29..99 (sequence 87..297) redirect

        rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED ) == 29 );
        rocketnet_unit_check( rocketnet_xdp_host_counter( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED ) == 72 );

        uint32_t high_water = 0;
        rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &key, &high_water ) == 1 );
        rocketnet_unit_check( high_water == entry_high_water );

        const struct rocketnet_server_xdp_replay_key_t other = xdp_host_client_key( families[f], xdp_host_client_port + 1 );
        rocketnet_unit_check( rocketnet_xdp_host_get_replay_entry( &other, &high_water ) == 0 );
    }
}

// ---------------------------------------------------------------------------
// every counter has a case: each ROCKETNET_SERVER_XDP_COUNTER_* that
// sdk/rocketnet_wire.h defines (other than the count) must appear in this
// file as a row's XDP_HOST_COUNTS(...), so a new counter cannot land without
// a case that makes it move.

static void test_xdp_transport_host_udp_length_drops()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };

    for ( int f = 0; f < 2; f++ )
    {
        xdp_host_row_t short_len = xdp_host_row( "UDP header length field below 8 bytes", families[f] );
        short_len.frame.override_udp_len = 7;
        rows.push_back( xdp_host_expect( short_len, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL ) ) );

        xdp_host_row_t zero_len = xdp_host_row( "UDP header length field is 0", families[f] );
        zero_len.frame.override_udp_len = 0;
        rows.push_back( xdp_host_expect( zero_len, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL ) ) );

        xdp_host_row_t past_end = xdp_host_row( "UDP header length field exceeds frame end", families[f] );
        past_end.frame.override_udp_len = 8 + past_end.frame.payload_bytes + 20;
        rows.push_back( xdp_host_expect( past_end, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );
    }

    for ( int f = 0; f < 2; f++ )
    {
        // link-layer padding after the datagram: the UDP length names the
        // payload, so the frame redirects at its real size

        xdp_host_row_t padded = xdp_host_row( "frame padded past the UDP length", families[f] );
        padded.frame.pad_bytes = 6;
        rows.push_back( padded );

        // the IP length names fewer bytes than the UDP length, and the
        // padding makes the frame long enough for the UDP length

        xdp_host_row_t ip_short = xdp_host_row( "IP length shorter than the UDP length", families[f] );
        ip_short.frame.pad_bytes = 6;
        ip_short.frame.override_ip_len = ( families[f] == 4 ? 20 : 0 ) + 8 + ip_short.frame.payload_bytes - 4;
        rows.push_back( xdp_host_expect( ip_short, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );

        // an IP length that names less than a UDP header, or one byte more
        // than the frame holds, leaves no datagram bound the UDP length fits
        // in, so the datagram to this server counts as too large

        xdp_host_row_t ip_zero = xdp_host_row( "IP length 0", families[f] );
        ip_zero.frame.override_ip_len = 0;
        rows.push_back( xdp_host_expect( ip_zero, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );

        xdp_host_row_t ip_past = xdp_host_row( "IP length one byte past the frame", families[f] );
        ip_past.frame.override_ip_len = ( families[f] == 4 ? 20 : 0 ) + 8 + ip_past.frame.payload_bytes + 1;
        rows.push_back( xdp_host_expect( ip_past, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );

        // to another port the same malformed length passes untouched

        xdp_host_row_t ip_zero_other = xdp_host_row( "IP length 0 to another port", families[f] );
        ip_zero_other.frame.override_ip_len = 0;
        ip_zero_other.frame.port = xdp_host_server_port + 1;
        rows.push_back( xdp_host_expect( ip_zero_other, XDP_HOST_PASS, XDP_HOST_NO_COUNTER ) );
    }

    xdp_host_run_rows( rows );
    // also verify the host frame parser refuses corrupt/short UDP length
    {
        xdp_host_frame_t f;
        f.override_udp_len = 7;
        std::vector<uint8_t> frame = xdp_host_frame_bytes( f );
        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_len = 0;
        rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame.data(), frame.size(), &from, &payload, &payload_len ) );

        f.override_udp_len = 0;
        frame = xdp_host_frame_bytes( f );
        rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame.data(), frame.size(), &from, &payload, &payload_len ) );
    }

    // a UDP length past the frame end is refused by the host parser too, in
    // both families, as server_xdp_udp drops it

    for ( int family = 4; family <= 6; family += 2 )
    {
        xdp_host_frame_t f;
        f.family = family;
        f.override_udp_len = 8 + f.payload_bytes + 20;
        const std::vector<uint8_t> frame = xdp_host_frame_bytes( f );
        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_len = 0;
        rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame.data(), frame.size(), &from, &payload, &payload_len ) );
    }

    // the padding strip: trailing link-layer padding is not payload; the
    // parsed payload is the UDP length less the UDP header, in both families

    for ( int family = 4; family <= 6; family += 2 )
    {
        xdp_host_frame_t f;
        f.family = family;
        f.pad_bytes = 6;
        const std::vector<uint8_t> frame = xdp_host_frame_bytes( f );
        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_len = 0;
        rocketnet_unit_check( rocketnet_xdp_frame_parse( frame.data(), frame.size(), &from, &payload, &payload_len ) );
        rocketnet_unit_check( payload_len == f.payload_bytes );
        rocketnet_unit_check( payload == frame.data() + frame.size() - f.pad_bytes - f.payload_bytes );
    }
}

#endif // #if defined(ROCKETNET_XDP_FILTER_HOST) && ROCKETNET_XDP_FILTER_HOST

// The xdp transport's default queue rule: create and the memory plan take the
// same count when num_xdp_queues is zero or negative. The xdp branch of
// rocketnet_builtin_server_memory_plan exists on Linux only.

#if defined( ROCKETNET_XDP ) && ROCKETNET_XDP

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_memory.h"

static void test_xdp_transport_queue_default_matches_memory_plan()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );

    rocketnet_server_memory_plan_t plan;

    config.num_xdp_queues = 0;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "xdp", plan ) );
    rocketnet_unit_check( plan.worker_threads == ROCKETNET_XDP_DEFAULT_QUEUES );
    rocketnet_unit_check( plan.sockets == ROCKETNET_XDP_DEFAULT_QUEUES );

    config.num_xdp_queues = 3;
    rocketnet_unit_check( rocketnet_builtin_server_memory_plan( &config, "xdp", plan ) );
    rocketnet_unit_check( plan.worker_threads == 3 );
    rocketnet_unit_check( plan.sockets == 3 );
}

#endif // #if ROCKETNET_XDP

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_rocketnet_packet_redirects );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_rocketnet_packet_redirects );
REGISTER_UNIT_TEST( test_xdp_transport_host_both_destinations_redirect_and_a_third_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_redirect_needs_a_populated_slot );
REGISTER_UNIT_TEST( test_xdp_transport_host_unconfigured_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_other_port_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_wrong_address_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_wrong_address_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_options_to_our_port_drop );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_options_elsewhere_pass );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_short_header_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_short_header_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_extension_headers_to_our_port_drop );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_extension_headers_elsewhere_pass_counted );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_fragment_passes_counted );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_fragment_passes_counted );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv4_first_fragment_drops_counted );
REGISTER_UNIT_TEST( test_xdp_transport_host_ipv6_first_fragment_drops_counted );
REGISTER_UNIT_TEST( test_xdp_transport_host_icmpv6_passes_uncounted );
REGISTER_UNIT_TEST( test_xdp_transport_host_vlan_tagged_passes );
REGISTER_UNIT_TEST( test_xdp_transport_host_too_small_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_too_large_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_wrong_epoch_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_wrong_filter_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_invalid_type_drops );
REGISTER_UNIT_TEST( test_xdp_transport_host_type_and_size_sweep_matches_the_standard );
REGISTER_UNIT_TEST( test_xdp_transport_host_replay_drops_behind_the_window );
REGISTER_UNIT_TEST( test_xdp_transport_host_replay_never_writes_the_map );

REGISTER_UNIT_TEST( test_xdp_transport_host_udp_length_drops );

// A UDP length past the IP payload, and a UDP length past the frame, are
// forged lengths. Both count as too large. A length below the UDP header
// stays too small, and that row lives in test_xdp_transport_host_udp_length_drops.

static void test_xdp_filter_host_udp_length_past_bounds_counts_too_large()
{
    std::vector<xdp_host_row_t> rows;
    const int families[2] = { 4, 6 };

    for ( int f = 0; f < 2; f++ )
    {
        xdp_host_row_t past_frame = xdp_host_row( "UDP length past the frame counts too large", families[f] );
        past_frame.frame.override_udp_len = 8 + past_frame.frame.payload_bytes + 20;
        rows.push_back( xdp_host_expect( past_frame, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );

        xdp_host_row_t past_ip = xdp_host_row( "UDP length past the IP payload counts too large", families[f] );
        past_ip.frame.pad_bytes = 6;
        past_ip.frame.override_ip_len = ( families[f] == 4 ? 20 : 0 ) + 8 + past_ip.frame.payload_bytes - 4;
        rows.push_back( xdp_host_expect( past_ip, XDP_HOST_DROP, XDP_HOST_COUNTS( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE ) ) );
    }

    xdp_host_run_rows( rows );
}

REGISTER_UNIT_TEST( test_xdp_filter_host_udp_length_past_bounds_counts_too_large );
#endif // #if defined(ROCKETNET_XDP_FILTER_HOST) && ROCKETNET_XDP_FILTER_HOST

#if defined( ROCKETNET_XDP ) && ROCKETNET_XDP
REGISTER_UNIT_TEST( test_xdp_transport_queue_default_matches_memory_plan );
#endif // #if ROCKETNET_XDP
