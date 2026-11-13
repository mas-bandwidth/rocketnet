/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP frame codec (sdk/rocketnet_xdp_frame.h). Pure byte codec, no
// sockets, no threads, no wall clock. The goldens are independent: produced
// once by a standalone python3/struct script (in the card's pull request
// body), not by the code under test.

#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"
#include "test_env_source.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"
#include "../sdk/rocketnet_config.h"

#include <stdlib.h>

static const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t golden_payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
static const uint8_t golden_ipv4[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00,
    0x45, 0x00, 0x00, 0x20, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb7, 0x5e,
    0xc0, 0xa8, 0x01, 0x0a, 0xc0, 0xa8, 0x01, 0x14,
    0x9c, 0x40, 0x9c, 0x41, 0x00, 0x0c, 0x00, 0x00,
    0xde, 0xad, 0xbe, 0xef
};
static const uint8_t golden_ipv6[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x86, 0xdd,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x11, 0x40,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x9c, 0x40, 0x9c, 0x41, 0x00, 0x0c, 0xce, 0x41,
    0xde, 0xad, 0xbe, 0xef
};

static void test_ipv4_frame_matches_golden()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "192.168.1.20:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );

    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( header_bytes == 42 );

    memcpy( frame + header_bytes, golden_payload, sizeof( golden_payload ) );

    int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) );
    rocketnet_unit_check( len == 46 );
    rocketnet_unit_check( memcmp( frame, golden_ipv4, sizeof( golden_ipv4 ) ) == 0 );
}

static void test_ipv6_frame_matches_golden()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );

    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( header_bytes == 62 );

    memcpy( frame + header_bytes, golden_payload, sizeof( golden_payload ) );

    int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) );
    rocketnet_unit_check( len == 66 );
    rocketnet_unit_check( memcmp( frame, golden_ipv6, sizeof( golden_ipv6 ) ) == 0 );
}

// an independent ones'-complement fold, for the second checksum check.

static uint32_t fold16( uint32_t sum )
{
    while ( sum >> 16 )
    {
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    }

    return sum;
}

static uint32_t sum_words( const uint8_t * data, size_t bytes )
{
    uint32_t sum = 0;
    size_t i = 0;
    while ( i + 1 < bytes )
    {
        sum += (uint32_t) ( ( (uint16_t) data[i] << 8 ) | data[i + 1] );
        i += 2;
    }

    if ( i < bytes )
    {
        sum += (uint32_t) ( (uint16_t) data[i] << 8 );
    }

    return sum;
}

static void test_ipv6_udp_checksum_verifies()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 62, golden_payload, sizeof( golden_payload ) );
    int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) );
    rocketnet_unit_check( len == 66 );

    // pseudo-header: source (16), destination (16), UDP length (32), three
    // zero bytes, next header 17.

    uint8_t pseudo[40];
    memcpy( pseudo, frame + 22, 32 );
    pseudo[32] = 0;
    pseudo[33] = 0;
    pseudo[34] = 0;
    pseudo[35] = 0x0c;
    pseudo[36] = 0;
    pseudo[37] = 0;
    pseudo[38] = 0;
    pseudo[39] = 0x11;

    uint32_t sum = sum_words( pseudo, sizeof( pseudo ) ) + sum_words( frame + 54, 8 + sizeof( golden_payload ) );
    rocketnet_unit_check( fold16( sum ) == 0xFFFF );
}

// the "computed zero is sent as FFFF" rule (RFC 768), driven through the
// checksum helper with a crafted pseudo-header + UDP segment whose ones-sum
// folds to 0xFFFF.

static void test_ipv6_checksum_zero_is_sent_as_ffff()
{
    uint8_t src[16], dst[16];
    memset( src, 0, sizeof( src ) );
    memset( dst, 0, sizeof( dst ) );

    uint8_t udp[8] = { 0xFF, 0xDE, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 };
    uint16_t csum = rocketnet_xdp_frame_ipv6_udp_checksum( src, dst, 8, udp, 0 );
    rocketnet_unit_check( csum == 0xFFFF );
}

static void test_ipv6_max_payload_is_1452_and_fits_1514()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[2048];
    memset( frame, 0xAB, sizeof( frame ) );

    const int payload = ROCKETNET_MAX_PACKET_BYTES;
    rocketnet_unit_check( payload == 1452 );

    memcpy( frame + 62, frame + 1000, 0 ); // no-op; payload stays as placed
    int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, payload );
    rocketnet_unit_check( len == 14 + 40 + 8 + payload );
    rocketnet_unit_check( len == 1514 );
}

// the largest frame of each family fits a 1500-byte link and not a
// 1400-byte one; an unknown MTU (0) fits it
static void test_xdp_frame_fits_mtu_judges_the_ip_packet()
{
    rocketnet_address_t s6, d6, s4, d4;
    rocketnet_unit_check( rocketnet_address_parse( &s6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d6, "[2001:db8::2]:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );

    uint8_t frame[2048];
    const int ipv6 = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &s6, &d6, ROCKETNET_MAX_PACKET_BYTES );
    const int ipv4 = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, ROCKETNET_MAX_PACKET_BYTES );
    rocketnet_unit_check( ipv6 == 1514 );
    rocketnet_unit_check( ipv4 == 1494 );

    rocketnet_unit_check( rocketnet_xdp_frame_fits_mtu( ipv6, 1500 ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_fits_mtu( ipv6, 1499 ) );
    rocketnet_unit_check( rocketnet_xdp_frame_fits_mtu( ipv4, 1480 ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_fits_mtu( ipv4, 1400 ) );
    rocketnet_unit_check( rocketnet_xdp_frame_fits_mtu( ipv6, 0 ) );
}

static void test_ipv6_payload_over_budget_is_refused()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[2048];
    int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, ROCKETNET_MAX_PACKET_BYTES + 1 );
    rocketnet_unit_check( len == 0 );

    rocketnet_address_t s4, d4;
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );

    len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, ROCKETNET_RECEIVE_PACKET_BYTES + 1 );
    rocketnet_unit_check( len == 0 );

    // a family mismatch between the two addresses is refused too

    len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &d4, 4 );
    rocketnet_unit_check( len == 0 );
}

static void test_build_refuses_missing_inputs_and_short_frames()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "192.168.1.20:40001" ) );

    uint8_t frame[2048];
    memset( frame, 0x5A, sizeof( frame ) );

    // a missing frame, MAC or address

    rocketnet_unit_check( rocketnet_xdp_frame_build( NULL, sizeof( frame ), server_mac, gateway_mac, &src, &dst, 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), NULL, gateway_mac, &src, &dst, 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, NULL, &src, &dst, 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, NULL, &dst, 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, NULL, 4 ) == 0 );

    // a negative payload length

    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, -1 ) == 0 );

    // a frame one byte short of the 42 header bytes and a 4-byte payload,
    // then exactly long enough

    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, 45, server_mac, gateway_mac, &src, &dst, 4 ) == 0 );

    // nothing was written by any refusal

    for ( size_t i = 0; i < sizeof( frame ); i++ )
    {
        rocketnet_unit_check( frame[i] == 0x5A );
    }

    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, 46, server_mac, gateway_mac, &src, &dst, 4 ) == 46 );
}

static void test_parse_round_trips_both_families()
{
    rocketnet_address_t src4, dst4, src6, dst6;
    rocketnet_unit_check( rocketnet_address_parse( &src4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst4, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &src6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    uint8_t frame[256];

    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 42, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src4, &dst4, (int) sizeof( golden_payload ) ) == 46 );

    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;
    rocketnet_unit_check( rocketnet_xdp_frame_parse( frame, 46, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.port == 40000 );
    rocketnet_unit_check( from.data.ipv4[0] == 192 && from.data.ipv4[1] == 168 );
    rocketnet_unit_check( from.data.ipv4[2] == 1 && from.data.ipv4[3] == 10 );
    rocketnet_unit_check( payload_bytes == 4 && memcmp( payload, golden_payload, 4 ) == 0 );

    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 62, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src6, &dst6, (int) sizeof( golden_payload ) ) == 66 );
    rocketnet_unit_check( rocketnet_xdp_frame_parse( frame, 66, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( from.port == 40000 );
    rocketnet_unit_check( from.data.ipv6[0] == 0x2001 && from.data.ipv6[1] == 0x0db8 );
    rocketnet_unit_check( from.data.ipv6[7] == 1 );
    rocketnet_unit_check( payload_bytes == 4 && memcmp( payload, golden_payload, 4 ) == 0 );
}

static void test_parse_refuses_extension_headers()
{
    rocketnet_address_t src6, dst6;
    rocketnet_unit_check( rocketnet_address_parse( &src6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 62, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src6, &dst6, (int) sizeof( golden_payload ) ) == 66 );

    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;

    // next header not 17 (e.g. an extension header) is refused

    frame[20] = 0x3c; // IPv6 fragment header
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, 66, &from, &payload, &payload_bytes ) );

    frame[20] = 17;

    // IPv4 with ihl != 5 is refused

    rocketnet_address_t src4, dst4;
    rocketnet_unit_check( rocketnet_address_parse( &src4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst4, "192.168.1.20:40001" ) );

    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 42, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src4, &dst4, (int) sizeof( golden_payload ) ) == 46 );

    frame[14] = 0x46;
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, 46, &from, &payload, &payload_bytes ) );

    // a non-IP ethertype is refused

    memset( frame, 0, sizeof( frame ) );
    frame[12] = 0x08;
    frame[13] = 0x06; // ARP
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, sizeof( frame ), &from, &payload, &payload_bytes ) );
}

static void test_parse_refuses_truncated_ipv6_frames()
{
    // every length from 42 to 61 is refused before the source address or UDP
    // port is read. Each truncated frame sits in a heap buffer of exactly its
    // own length, so a read past its end trips Asan.

    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;
    for ( size_t len = 42; len <= 61; ++len )
    {
        uint8_t * buf = (uint8_t *) malloc( len );
        rocketnet_unit_check( buf != NULL );

        memcpy( buf, golden_ipv6, len );
        rocketnet_unit_check( !rocketnet_xdp_frame_parse( buf, len, &from, &payload, &payload_bytes ) );

        free( buf );
    }
}

// the reply-MAC rule (rocketnet_xdp_reply_mac): learned client, gateway
// otherwise, the newest MAC when a client's source MAC changes, and a refusal
// counted when there is neither. No socket: the table is driven directly.

static struct rocketnet_xdp_reply_macs_t reply_macs_under_test;
static struct rocketnet_xdp_reply_mac_entry_t reply_macs_under_test_entries[ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES];

// binds the table to its storage, sized as a default-config server sizes it,
// and resets it with gateway (NULL: none)
static void reply_macs_under_test_bind( struct rocketnet_xdp_reply_macs_t * table, const uint8_t * gateway )
{
    rocketnet_unit_check( rocketnet_xdp_reply_macs_bind( table, reply_macs_under_test_entries, ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES ) );
    rocketnet_unit_check( (int) table->mask + 1 == rocketnet_xdp_reply_mac_entries( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    rocketnet_xdp_reply_macs_reset( table, gateway );
}

static void test_xdp_transport_reply_mac_follows_the_client()
{
    static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
    static const uint8_t client_new_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

    struct rocketnet_xdp_reply_macs_t * table = &reply_macs_under_test;
    rocketnet_address_t client, stranger, client6;
    rocketnet_unit_check( rocketnet_address_parse( &client, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &stranger, "192.168.1.21:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client6, "[2001:db8::20]:40001" ) );

    uint64_t refused = 0;

    // with a gateway: an unlearned address goes to the gateway

    reply_macs_under_test_bind( table, gateway_mac );
    const uint8_t * mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // an answered packet from the client teaches its MAC: replies follow it,
    // and only for that address

    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // the same IP on another port is another client

    rocketnet_address_t other_port = client;
    other_port.port = 40002;
    mac = rocketnet_xdp_reply_mac( table, &other_port, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // the client's source MAC changes: the newest one wins

    rocketnet_xdp_reply_macs_learn( table, &client, client_new_mac );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_new_mac, 6 ) == 0 );

    // a multicast or zero source MAC is never learned

    rocketnet_xdp_reply_macs_learn( table, &stranger, zero_mac );

    static const uint8_t multicast_mac[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    rocketnet_xdp_reply_macs_learn( table, &stranger, multicast_mac );
    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // IPv6 clients are learned the same way

    rocketnet_xdp_reply_macs_learn( table, &client6, client_mac );
    mac = rocketnet_xdp_reply_mac( table, &client6, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    // with a gateway, an unauthenticated frame's MAC never wins over it
    {
        static const uint8_t spoofer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
        mac = rocketnet_xdp_reply_mac( table, &stranger, spoofer_mac, &refused );
        rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );
    }

    // a forgotten client falls back to the gateway

    rocketnet_xdp_reply_macs_forget( table, &client );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );
    rocketnet_unit_check( refused == 0 );

    // no gateway: learned clients still get their MAC; an unlearned address is
    // refused and counted, never given a zero MAC

    rocketnet_xdp_reply_macs_reset( table, NULL );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 1 );

    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 2 );

    // with no gateway, a direct response to an unlearned address goes to the
    // source MAC of the frame it answers (how a connect handshake on link
    // gets its first replies), never to a zero or multicast one; a learned
    // client keeps its learned MAC whatever an unauthenticated frame from its
    // address carries

    static const uint8_t stranger_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
    mac = rocketnet_xdp_reply_mac( table, &stranger, stranger_mac, &refused );
    rocketnet_unit_check( mac && memcmp( mac, stranger_mac, 6 ) == 0 );

    mac = rocketnet_xdp_reply_mac( table, &stranger, zero_mac, &refused );
    rocketnet_unit_check( mac == NULL );

    mac = rocketnet_xdp_reply_mac( table, &stranger, multicast_mac, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 4 );

    mac = rocketnet_xdp_reply_mac( table, &client, stranger_mac, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    // the table is fixed-size: far more clients than entries all resolve to a
    // MAC learned for them or to the refusal, and the newest client is held

    rocketnet_address_t many = client;
    uint8_t many_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };

    for ( int i = 0; i < (int) ( table->mask + 1 ) * 2; ++i )
    {
        many.port = (uint16_t) ( 1000 + i );
        many_mac[4] = (uint8_t) ( i >> 8 );
        many_mac[5] = (uint8_t) i;
        rocketnet_xdp_reply_macs_learn( table, &many, many_mac );
        mac = rocketnet_xdp_reply_mac( table, &many, NULL, &refused );
        rocketnet_unit_check( mac && memcmp( mac, many_mac, 6 ) == 0 );
    }

    rocketnet_unit_check( refused == 4 );

    // the frame built for a learned client carries its MAC as the destination

    rocketnet_xdp_reply_macs_reset( table, gateway_mac );
    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );

    rocketnet_address_t src;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 42, golden_payload, sizeof( golden_payload ) );
    mac = rocketnet_xdp_reply_mac( table, &client, NULL, &refused );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, mac, &src, &client, (int) sizeof( golden_payload ) ) == 46 );
    rocketnet_unit_check( memcmp( frame, client_mac, 6 ) == 0 );
    rocketnet_unit_check( memcmp( frame + 6, server_mac, 6 ) == 0 );
}

// The AF_XDP transport serves IPv4 and IPv6. The cases below hold the codec
// and the receive dispatch to that: an IPv6 frame as Linux sends it (a
// nonzero flow label) parses, an IPv6 frame built into a reused buffer carries
// the right UDP checksum, an IPv6 frame with an extension header is dropped
// and counted by name, and IPv4 is byte-for-byte what it was.

// an IPv6 UDP frame as a Linux sender writes it: traffic class 0, flow label
// 0xabcde (Linux sets one by default), the golden addresses, ports and payload

static const uint8_t flow_label_ipv6[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x86, 0xdd,
    0x60, 0x0a, 0xbc, 0xde, 0x00, 0x0c, 0x11, 0x40,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0xc3, 0x50, 0x9c, 0x40, 0x00, 0x0c, 0x00, 0x00,
    0xde, 0xad, 0xbe, 0xef
};

static void test_xdp_transport_frame_parse_ipv6_udp()
{
    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;

    rocketnet_unit_check( rocketnet_xdp_frame_parse( flow_label_ipv6, sizeof( flow_label_ipv6 ), &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( from.port == 50000 );
    rocketnet_unit_check( from.data.ipv6[0] == 0x2001 && from.data.ipv6[1] == 0x0db8 );
    rocketnet_unit_check( from.data.ipv6[2] == 0 && from.data.ipv6[3] == 0 && from.data.ipv6[4] == 0 );
    rocketnet_unit_check( from.data.ipv6[5] == 0 && from.data.ipv6[6] == 0 && from.data.ipv6[7] == 7 );
    rocketnet_unit_check( payload == flow_label_ipv6 + 62 );
    rocketnet_unit_check( payload_bytes == 4 && memcmp( payload, golden_payload, 4 ) == 0 );

    // the same frame with a traffic class set parses the same

    uint8_t frame[sizeof( flow_label_ipv6 )];
    memcpy( frame, flow_label_ipv6, sizeof( frame ) );
    frame[14] = 0x6b;
    frame[15] = 0x8a;
    rocketnet_unit_check( rocketnet_xdp_frame_parse( frame, sizeof( frame ), &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 && from.port == 50000 && from.data.ipv6[7] == 7 );

    // a version nibble that is not 6 is refused

    frame[14] = 0x4b;
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, sizeof( frame ), &from, &payload, &payload_bytes ) );
}

// The known vector: the golden IPv6 frame above (2001:db8::1 port 40000 to
// 2001:db8::2 port 40001, payload de ad be ef), whose UDP checksum is 0xce41,
// computed independently of the code under test. The build writes it into a
// frame buffer still holding a previous datagram's bytes, as a reused UMEM
// frame does, so a stale checksum field cannot leak into the sum.

static void test_xdp_transport_frame_build_ipv6_checksum_vector()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0xA5, sizeof( frame ) );
    memcpy( frame + 62, golden_payload, sizeof( golden_payload ) );

    const int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) );
    rocketnet_unit_check( len == 66 );
    rocketnet_unit_check( frame[60] == 0xce && frame[61] == 0x41 );
    rocketnet_unit_check( memcmp( frame, golden_ipv6, sizeof( golden_ipv6 ) ) == 0 );

    // built again over itself (the field now holds 0xce41): the same bytes

    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) ) == 66 );
    rocketnet_unit_check( memcmp( frame, golden_ipv6, sizeof( golden_ipv6 ) ) == 0 );
}

// The userspace receive: an IPv6 frame whose next header is an extension
// header is dropped and counted under ipv6_extension, never parsed at the
// UDP offsets, whatever sits behind the header.

static void test_xdp_transport_receive_ipv6_extension_dropped_counted()
{
    static const uint8_t extensions[6] = { 0, 43, 44, 60, 51, 50 };

    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    for ( int i = 0; i < 6; i++ )
    {
        uint8_t frame[128];
        memset( frame, 0, sizeof( frame ) );
        memcpy( frame, flow_label_ipv6, sizeof( flow_label_ipv6 ) );
        frame[20] = extensions[i];

        rocketnet_address_t from;
        memset( &from, 0x5A, sizeof( from ) );

        const uint8_t * payload = NULL;
        int payload_bytes = -1;
        rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame, 62 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
        rocketnet_unit_check( payload == NULL && payload_bytes == -1 );
        rocketnet_unit_check( drops.ipv6_extension == (uint64_t) ( i + 1 ) );
    }

    rocketnet_unit_check( drops.not_ip == 0 && drops.malformed == 0 && drops.other_family == 0 );

    // a next header that is neither UDP nor an extension (ICMPv6) is dropped
    // as malformed, not as an extension

    uint8_t frame[128];
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame, flow_label_ipv6, sizeof( flow_label_ipv6 ) );
    frame[20] = 58;

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame, 62 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.ipv6_extension == 6 && drops.malformed == 1 );
}

// IPv4 is unchanged: the build into a dirty buffer is the golden bytes, the
// parse reads them back, and the receive takes them at the IPv4 offsets.

static void test_xdp_transport_frame_ipv4_unchanged()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "192.168.1.20:40001" ) );

    uint8_t frame[256];
    memset( frame, 0xA5, sizeof( frame ) );
    memcpy( frame + 42, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, (int) sizeof( golden_payload ) ) == 46 );
    rocketnet_unit_check( memcmp( frame, golden_ipv4, sizeof( golden_ipv4 ) ) == 0 );

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    rocketnet_unit_check( rocketnet_xdp_frame_parse( golden_ipv4, sizeof( golden_ipv4 ), &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 && from.port == 40000 );
    rocketnet_unit_check( from.data.ipv4[0] == 192 && from.data.ipv4[1] == 168 && from.data.ipv4[2] == 1 && from.data.ipv4[3] == 10 );
    rocketnet_unit_check( payload == golden_ipv4 + 42 && payload_bytes == 4 );

    // the IPv4 parse still takes only ihl 5 and UDP

    uint8_t copy[sizeof( golden_ipv4 )];
    memcpy( copy, golden_ipv4, sizeof( copy ) );
    copy[14] = 0x46;
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( copy, sizeof( copy ), &from, &payload, &payload_bytes ) );

    copy[14] = 0x45;
    copy[23] = 6;
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( copy, sizeof( copy ), &from, &payload, &payload_bytes ) );

    // the receive takes a server-sized IPv4 packet at offset 42

    const int packet = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 16;
    memset( frame, 0, sizeof( frame ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src, &dst, packet ) == 42 + packet );

    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );
    rocketnet_unit_check( rocketnet_xdp_frame_receive( frame, (size_t) ( 42 + packet ), ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( payload == frame + 42 && payload_bytes == packet );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 && from.port == 40000 );
    rocketnet_unit_check( drops.not_ip == 0 && drops.ipv6_extension == 0 && drops.malformed == 0 && drops.other_family == 0 );
}

// The receive dispatches on the ethertype: IPv4 at its offsets, IPv6 at the
// 40-byte fixed header, the from-address in the family it read. A frame of
// the family the server does not serve, a frame that is neither family and a
// frame too short for any server packet are each dropped and counted.

static void test_xdp_transport_receive_dispatches_both_families_counted()
{
    rocketnet_address_t src4, dst4, src6, dst6;
    rocketnet_unit_check( rocketnet_address_parse( &src4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst4, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &src6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    const int packet = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 16;
    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;

    // IPv6, served: taken at offset 62, the from-address IPv6

    uint8_t frame6[256];
    memset( frame6, 0, sizeof( frame6 ) );

    for ( int i = 0; i < packet; i++ )
    {
        frame6[62 + i] = (uint8_t) ( i * 3 + 1 );
    }

    const int len6 = rocketnet_xdp_frame_build( frame6, sizeof( frame6 ), server_mac, gateway_mac, &src6, &dst6, packet );
    rocketnet_unit_check( len6 == 62 + packet );
    rocketnet_unit_check( rocketnet_xdp_frame_receive( frame6, (size_t) len6, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 && from.port == 40000 && from.data.ipv6[7] == 1 );
    rocketnet_unit_check( payload == frame6 + 62 && payload_bytes == packet && payload[0] == 1 && payload[1] == 4 );

    // IPv4, served: taken at offset 42

    uint8_t frame4[256];
    memset( frame4, 0, sizeof( frame4 ) );
    const int len4 = rocketnet_xdp_frame_build( frame4, sizeof( frame4 ), server_mac, gateway_mac, &src4, &dst4, packet );
    rocketnet_unit_check( len4 == 42 + packet );
    rocketnet_unit_check( rocketnet_xdp_frame_receive( frame4, (size_t) len4, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 && payload == frame4 + 42 && payload_bytes == packet );
    rocketnet_unit_check( drops.not_ip == 0 && drops.ipv6_extension == 0 && drops.malformed == 0 && drops.other_family == 0 );

    // each family at a server of the other: dropped, counted

    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame6, (size_t) len6, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame4, (size_t) len4, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.other_family == 2 );

    // too short for the smallest server packet, each family: malformed

    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame6, (size_t) ( 62 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1 ), ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame4, (size_t) ( 42 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1 ), ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.malformed == 2 );

    // cut below the length its UDP header names: malformed, as the kernel
    // filter drops it (server_xdp_udp); a frame whose UDP length is the
    // smallest server packet is taken at that size

    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame6, (size_t) ( 62 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES ), ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.malformed == 3 );

    uint8_t smallest6[256];
    memset( smallest6, 0, sizeof( smallest6 ) );
    const int smallest_len6 = rocketnet_xdp_frame_build( smallest6, sizeof( smallest6 ), server_mac, gateway_mac, &src6, &dst6, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    rocketnet_unit_check( smallest_len6 == 62 + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    rocketnet_unit_check( rocketnet_xdp_frame_receive( smallest6, (size_t) smallest_len6, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( payload_bytes == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

    // ARP, and a frame too short to name an ethertype: not_ip

    frame4[12] = 0x08;
    frame4[13] = 0x06;
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame4, (size_t) len4, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame6, 13, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.not_ip == 2 );
    rocketnet_unit_check( drops.ipv6_extension == 0 && drops.other_family == 2 && drops.malformed == 3 );
}

// sdk/rocketnet_address.h: where the real IPv6 header is seen, as here, an
// IPv4-mapped source (::ffff:a.b.c.d, RFC 4291 2.5.5.2) never occurs
// legitimately; it is spoofed and dropped, as every socket transport drops
// it. Dropped at an IPv6 server and at one serving both families, counted
// malformed; the same frame from a native IPv6 source is taken.

static void test_xdp_transport_receive_drops_mapped_source()
{
    rocketnet_address_t mapped, native, dst6;
    memset( &mapped, 0, sizeof( mapped ) );
    mapped.type = ROCKETNET_ADDRESS_IPV6;
    mapped.data.ipv6[5] = 0xFFFF;
    mapped.data.ipv6[6] = 0x0A00;
    mapped.data.ipv6[7] = 0x0001;
    mapped.port = 50000;
    rocketnet_unit_check( rocketnet_address_is_ipv4_in_ipv6( &mapped ) );
    rocketnet_unit_check( rocketnet_address_parse( &native, "[2001:db8::1]:50000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    const int packet = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 16;
    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    uint8_t frame[256];
    memset( frame, 0, sizeof( frame ) );
    const int len = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &mapped, &dst6, packet );
    rocketnet_unit_check( len == 62 + packet );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame, (size_t) len, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( frame, (size_t) len, -1, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.malformed == 2 );
    rocketnet_unit_check( drops.not_ip == 0 && drops.ipv6_extension == 0 && drops.other_family == 0 && drops.descriptor == 0 );

    memset( frame, 0, sizeof( frame ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &native, &dst6, packet ) == len );
    rocketnet_unit_check( rocketnet_xdp_frame_receive( frame, (size_t) len, -1, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 && from.port == 50000 && payload_bytes == packet );
    rocketnet_unit_check( drops.malformed == 2 );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_drops_mapped_source );

// The server transport order keeps xdp for either family: the list parses the same
// for an IPv4 and an IPv6 server, with xdp first under "auto".

static void test_xdp_transport_order_keeps_xdp_for_ipv6()
{
    const int linux_server[] = { ROCKETNET_TRANSPORT_XDP, ROCKETNET_TRANSPORT_GSO, ROCKETNET_TRANSPORT_PORTABLE };
    struct rocketnet_transport_order_t transport_order;

    rocketnet_unit_check( rocketnet_transport_order_parse( "auto", linux_server, 3, &transport_order ) );
    rocketnet_unit_check( transport_order.num_candidates == 3 && transport_order.candidates[0] == ROCKETNET_TRANSPORT_XDP );

    rocketnet_unit_check( rocketnet_transport_order_parse( "gso,xdp,portable", linux_server, 3, &transport_order ) );
    rocketnet_unit_check( transport_order.num_candidates == 3 && transport_order.candidates[1] == ROCKETNET_TRANSPORT_XDP );

    rocketnet_unit_check( rocketnet_transport_order_parse( "xdp", linux_server, 3, &transport_order ) );
    rocketnet_unit_check( transport_order.num_candidates == 1 && transport_order.candidates[0] == ROCKETNET_TRANSPORT_XDP );
}

// one strict parse serves both the interface's sysfs address file and the
// override: exactly 17 characters after a single trailing newline is
// stripped, hex pairs separated by ':' or '.', and no write on refusal.

static void test_xdp_transport_mac_address_parse_is_strict()
{
    static const uint8_t expected[6] = { 0x02, 0x42, 0xac, 0x11, 0x00, 0x02 };
    static const uint8_t untouched[6] = { 0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A };

    uint8_t mac[6];

    memset( mac, 0xAA, sizeof( mac ) );
    rocketnet_unit_check( rocketnet_xdp_parse_mac_address( "02:42:ac:11:00:02\n", mac ) );
    rocketnet_unit_check( memcmp( mac, expected, sizeof( expected ) ) == 0 );

    memset( mac, 0xAA, sizeof( mac ) );
    rocketnet_unit_check( rocketnet_xdp_parse_mac_address( "02.42.ac.11.00.02", mac ) );
    rocketnet_unit_check( memcmp( mac, expected, sizeof( expected ) ) == 0 );

    static const char * const refused[] = {
        "",
        "\n",
        "02:42:ac:11:00",
        "02:42:ac:11:00:0g",
        "02-42-ac-11-00-02",
        "02:42:ac:11:00:020"
    };

    for ( size_t i = 0; i < sizeof( refused ) / sizeof( refused[0] ); ++i )
    {
        memcpy( mac, untouched, sizeof( untouched ) );
        rocketnet_unit_check( !rocketnet_xdp_parse_mac_address( refused[i], mac ) );
        rocketnet_unit_check( memcmp( mac, untouched, sizeof( untouched ) ) == 0 );
    }
}

// cpu 0 is a cpu: a list entry may start with it, and a range may end on it
static void test_xdp_cpu_list_entry_takes_cpu_zero()
{
    const char * cursor = "0,0-3,0-0\n";
    int first = -1;
    int last = -1;

    rocketnet_unit_check( rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    rocketnet_unit_check( first == 0 && last == 0 );
    rocketnet_unit_check( rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    rocketnet_unit_check( first == 0 && last == 3 );
    rocketnet_unit_check( rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    rocketnet_unit_check( first == 0 && last == 0 );
    rocketnet_unit_check( *cursor == '\0' );

    cursor = "a";
    rocketnet_unit_check( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    cursor = "1-";
    rocketnet_unit_check( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );

    // a comma with no entry after it is not a separator: "0," and "0,\n"
    // are not cpu lists (scripts/rocketnet-provision cpu_list_has agrees)
    cursor = "0,";
    rocketnet_unit_check( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    cursor = "0,\n";
    rocketnet_unit_check( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    cursor = "0,,1";
    rocketnet_unit_check( !rocketnet_xdp_cpu_list_entry( &cursor, &first, &last ) );
    rocketnet_unit_check( rocketnet_xdp_cpu_list_has( "0,", 0 ) == -1 );
    rocketnet_unit_check( rocketnet_xdp_cpu_list_has( "1-3,\n", 2 ) == -1 );
    rocketnet_unit_check( !rocketnet_xdp_pin_targets_isolated( "1-7,", 0, 7, NULL ) );
}

// one NAT address, many clients: ports that differ only in their high byte
// each keep a learned reply MAC. One more client than a probe window holds,
// so a slot hash blind to the port's high byte puts them all in one window
// and the learn evicts the first.
static struct rocketnet_xdp_reply_mac_entry_t port_high_byte_entries[ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES];

static void test_xdp_reply_macs_ports_differing_in_the_high_byte_spread()
{
    struct rocketnet_xdp_reply_macs_t table;
    memset( &table, 0, sizeof( table ) );
    rocketnet_unit_check( rocketnet_xdp_reply_macs_bind( &table, port_high_byte_entries, ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES ) );
    rocketnet_xdp_reply_macs_reset( &table, NULL );

    const int clients = ROCKETNET_XDP_REPLY_MAC_PROBES + 1;

    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_address_t client;
        memset( &client, 0, sizeof( client ) );
        client.type = ROCKETNET_ADDRESS_IPV4;
        client.data.ipv4[0] = 203;
        client.data.ipv4[1] = 0;
        client.data.ipv4[2] = 113;
        client.data.ipv4[3] = 9;
        client.port = (uint16_t) ( ( i + 1 ) << 8 );
        const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t) ( i + 1 ) };
        rocketnet_xdp_reply_macs_learn( &table, &client, mac );
    }

    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_address_t client;
        memset( &client, 0, sizeof( client ) );
        client.type = ROCKETNET_ADDRESS_IPV4;
        client.data.ipv4[0] = 203;
        client.data.ipv4[1] = 0;
        client.data.ipv4[2] = 113;
        client.data.ipv4[3] = 9;
        client.port = (uint16_t) ( ( i + 1 ) << 8 );
        const struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( &table, &client );
        rocketnet_unit_check( entry != NULL );
        rocketnet_unit_check( entry && entry->mac[5] == (uint8_t) ( i + 1 ) );
    }
}

// A server create that pins xdp with an IPv6 address is not refused for its
// family: the transport order hands it to the xdp transport, whose create meets this
// process's commit barrier (the unit suite forbids an XDP data plane) and
// refuses there, before any socket opens. No line says the transport serves
// IPv4 only, and the status is not INVALID_TRANSPORT_LIST. The xdp row
// exists only on Linux: elsewhere a pin of it falls to the auto order, whose
// transports open a real socket, so the case is built on Linux only.

#if ROCKETNET_XDP

static int xdp_ipv6_family_refusals = 0;
static int xdp_ipv6_barrier_lines = 0;

static void xdp_ipv6_capture( void *, enum rocketnet_log_level_t level, const char * message, size_t )
{
    (void) level;

    if ( strstr( message, "serves IPv4" ) != NULL || strstr( message, "only support ipv4" ) != NULL )
    {
        xdp_ipv6_family_refusals++;
    }

    if ( strstr( message, "xdp create refused: this process forbids committing" ) != NULL )
    {
        xdp_ipv6_barrier_lines++;
    }
}

#endif // #if ROCKETNET_XDP

#if ROCKETNET_XDP
static void test_xdp_transport_ipv6_pin_reaches_the_xdp_transport_at_create()
{
    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    bool xdp_available = false;
    for ( int i = 0; i < num_available; i++ )
    {
        xdp_available = xdp_available || available[i] == ROCKETNET_TRANSPORT_XDP;
    }

    rocketnet_unit_check( xdp_available );

    // the env pin wins over the struct's list: set it aside for the case

#if ROCKETNET_DEV
    char saved[256];
    const bool pinned = rocketnet_test_env_getenv( "ROCKETNET_SERVER_TRANSPORT", saved, sizeof( saved ) ) != 0;
    rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_TRANSPORT" );
#endif // #if ROCKETNET_DEV

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    memset( config.backend_public_key, 0x42, 32 );
    memset( config.server_public_key, 0x43, 32 );
    memset( config.server_secret_key, 0x44, 32 );
    config.protocol_id = 1;
    strcpy( config.transport, "xdp" );

    const enum rocketnet_log_level_t level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_DEBUG );
    rocketnet_log_function( xdp_ipv6_capture, NULL );
    xdp_ipv6_family_refusals = 0;
    xdp_ipv6_barrier_lines = 0;

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_internal( NULL, "[2001:db8::10]:40000", &config, 1000.0, &status );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( level );
#if ROCKETNET_DEV
    if ( pinned )
    {
        rocketnet_test_env_setenv( "ROCKETNET_SERVER_TRANSPORT", saved, 1 );
    }
#endif // #if ROCKETNET_DEV

    rocketnet_unit_check( server == NULL );
    rocketnet_unit_check( status != ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );

    // the barrier's refusal is neither "not built" nor "families": the
    // transport order is exhausted
    rocketnet_unit_check( status == ROCKETNET_STATUS_NO_TRANSPORT_FOUND );
    rocketnet_unit_check( xdp_ipv6_family_refusals == 0 );
    rocketnet_unit_check( xdp_ipv6_barrier_lines == 1 );
}
#endif // #if ROCKETNET_XDP

// a Linux build without the xdp transport: xdp is a transport of the
// platform, so a pin names it and refuses at open

#if defined( __linux__ ) && !ROCKETNET_XDP
static void test_xdp_disabled_pin_refuses_and_auto_keeps_fallbacks()
{
    int count = 0;
    const int * available = rocketnet_server_transport_io_availability( &count );
    rocketnet_unit_check( count > 0 );

    struct rocketnet_transport_order_t transport_order;
    rocketnet_unit_check( rocketnet_transport_order_parse( "xdp", available, count, &transport_order ) );
    rocketnet_unit_check( transport_order.num_candidates == 1 && transport_order.candidates[0] == ROCKETNET_TRANSPORT_XDP );
    rocketnet_unit_check( transport_order.num_skipped == 0 );

    rocketnet_server_transport_io_t io = {};
    rocketnet_server_transport_io_config_t config = {};
    rocketnet_address_t bind = {};
    const rocketnet_transport_definition_t * xdp = rocketnet_transport_definition( ROCKETNET_TRANSPORT_XDP );
    rocketnet_unit_check( xdp->backend && xdp->backend->server );
    rocketnet_unit_check( !rocketnet_server_transport_io_open( xdp->backend->server, NULL, &config, &bind, &io ) );
    rocketnet_unit_check( io.instance == NULL );
    rocketnet_unit_check( rocketnet_transport_order_parse( "auto", available, count, &transport_order ) );
    rocketnet_unit_check( transport_order.num_candidates >= 2 );
    rocketnet_unit_check( transport_order.candidates[0] == ROCKETNET_TRANSPORT_XDP );
    rocketnet_unit_check( transport_order.candidates[1] == ROCKETNET_TRANSPORT_GSO );
}

REGISTER_UNIT_TEST( test_xdp_disabled_pin_refuses_and_auto_keeps_fallbacks );
#endif // #if defined( __linux__ ) && !ROCKETNET_XDP

// ---------------------------------------------------------------------------
// 802.1Q: the transport of a server whose address is on a VLAN device. The
// tagged goldens are the untagged goldens above with the four tag bytes
// written in after the MACs by hand (81 00, then the TCI): the IP and UDP
// headers and their checksums do not change.

static void vlan_golden( const uint8_t * untagged, size_t untagged_bytes, uint16_t tci, uint8_t * out )
{
    memcpy( out, untagged, 12 );
    out[12] = 0x81;
    out[13] = 0x00;
    out[14] = (uint8_t) ( tci >> 8 );
    out[15] = (uint8_t) tci;
    memcpy( out + 16, untagged + 12, untagged_bytes - 12 );
}

static void test_xdp_vlan_frame_matches_golden_and_round_trips()
{
    rocketnet_address_t s4, d4, s6, d6;
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &s6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d6, "[2001:db8::2]:40001" ) );

    uint8_t expected[128];
    uint8_t frame[128];
    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;

    rocketnet_unit_check( rocketnet_xdp_frame_header_bytes_vlan( ROCKETNET_ADDRESS_IPV4, 100 ) == 46 );
    rocketnet_unit_check( rocketnet_xdp_frame_header_bytes_vlan( ROCKETNET_ADDRESS_IPV6, 100 ) == 66 );
    rocketnet_unit_check( rocketnet_xdp_frame_header_bytes_vlan( ROCKETNET_ADDRESS_IPV4, 0 ) == 42 );

    // IPv4, VLAN 100 (0x064)

    vlan_golden( golden_ipv4, sizeof( golden_ipv4 ), 0x0064, expected );
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 46, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, 4, 100 ) == 50 );
    rocketnet_unit_check( memcmp( frame, expected, sizeof( golden_ipv4 ) + 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_parse_vlan( frame, 50, 100, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 && from.port == 40000 && from.data.ipv4[3] == 10 );
    rocketnet_unit_check( payload == frame + 46 && payload_bytes == 4 );

    // IPv6, VLAN 4094, the highest

    vlan_golden( golden_ipv6, sizeof( golden_ipv6 ), 0x0FFE, expected );
    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 66, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s6, &d6, 4, 4094 ) == 70 );
    rocketnet_unit_check( memcmp( frame, expected, sizeof( golden_ipv6 ) + 4 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_parse_vlan( frame, 70, 4094, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 && from.port == 40000 && from.data.ipv6[7] == 1 );
    rocketnet_unit_check( payload == frame + 66 && payload_bytes == 4 );

    // VLAN 0 is the untagged frame, byte for byte

    memset( frame, 0, sizeof( frame ) );
    memcpy( frame + 42, golden_payload, sizeof( golden_payload ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, 4, 0 ) == 46 );
    rocketnet_unit_check( memcmp( frame, golden_ipv4, sizeof( golden_ipv4 ) ) == 0 );

    // a tag carrying priority 7 and DEI is taken: only the 12-bit VLAN ID is matched

    vlan_golden( golden_ipv4, sizeof( golden_ipv4 ), 0xF064, expected );
    rocketnet_unit_check( rocketnet_xdp_frame_parse_vlan( expected, 50, 100, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( payload == expected + 46 && payload_bytes == 4 );
}

// the refusals: a tag of another VLAN, a tag on an untagged transport, a tag
// cut by the frame end, and the untagged frame a NIC hands over with its tag
// stripped (taken)
static void test_xdp_vlan_parse_refuses_other_tags_and_takes_stripped_frames()
{
    uint8_t tagged[128];
    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;

    vlan_golden( golden_ipv4, sizeof( golden_ipv4 ), 0x0064, tagged );

    rocketnet_unit_check( !rocketnet_xdp_frame_parse_vlan( tagged, 50, 101, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_parse_vlan( tagged, 50, 4094, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_parse_vlan( tagged, 50, 0, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( tagged, 50, &from, &payload, &payload_bytes ) );

    // the tag needs 18 bytes, the tagged IPv4 header set 46 and this frame's
    // UDP length 50

    rocketnet_unit_check( rocketnet_xdp_frame_vlan_tag( tagged, 17, 100 ) == -1 );
    rocketnet_unit_check( rocketnet_xdp_frame_vlan_tag( tagged, 18, 100 ) == 4 );
    rocketnet_unit_check( rocketnet_xdp_frame_vlan_tag( tagged, 18, 101 ) == -1 );
    rocketnet_unit_check( rocketnet_xdp_frame_vlan_tag( golden_ipv4, sizeof( golden_ipv4 ), 100 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_vlan_tag( golden_ipv4, sizeof( golden_ipv4 ), 0 ) == 0 );
    for ( size_t bytes = 14; bytes < 50; bytes++ )
    {
        rocketnet_unit_check( !rocketnet_xdp_frame_parse_vlan( tagged, bytes, 100, &from, &payload, &payload_bytes ) );
    }
    rocketnet_unit_check( rocketnet_xdp_frame_parse_vlan( tagged, 50, 100, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( payload_bytes == 4 );

    // stripped: the untagged frame is taken by a VLAN transport

    rocketnet_unit_check( rocketnet_xdp_frame_parse_vlan( golden_ipv4, sizeof( golden_ipv4 ), 100, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( payload == golden_ipv4 + 42 && payload_bytes == 4 );
}

// receive counts a refused tag as not IP, and dispatches a matching or a
// stripped one
static void test_xdp_vlan_receive_counts_refused_tags()
{
    rocketnet_address_t s4, d4;
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );

    const int body = 64;
    rocketnet_unit_check( body >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    uint8_t tagged[256];
    uint8_t untagged[256];
    memset( tagged, 0x33, sizeof( tagged ) );
    memset( untagged, 0x33, sizeof( untagged ) );
    const int tagged_bytes = rocketnet_xdp_frame_build_vlan( tagged, sizeof( tagged ), server_mac, gateway_mac, &s4, &d4, body, 7 );
    const int untagged_bytes = rocketnet_xdp_frame_build( untagged, sizeof( untagged ), server_mac, gateway_mac, &s4, &d4, body );
    rocketnet_unit_check( tagged_bytes == 46 + body && untagged_bytes == 42 + body );

    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;
    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    rocketnet_unit_check( rocketnet_xdp_frame_receive_vlan( tagged, (size_t) tagged_bytes, ROCKETNET_ADDRESS_IPV4, 7, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( payload == tagged + 46 && payload_bytes == body );
    rocketnet_unit_check( rocketnet_xdp_frame_receive_vlan( untagged, (size_t) untagged_bytes, ROCKETNET_ADDRESS_IPV4, 7, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( payload == untagged + 42 && payload_bytes == body );
    rocketnet_unit_check( rocketnet_xdp_receive_drops_total( &drops ) == 0 );

    rocketnet_unit_check( !rocketnet_xdp_frame_receive_vlan( tagged, (size_t) tagged_bytes, ROCKETNET_ADDRESS_IPV4, 8, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive( tagged, (size_t) tagged_bytes, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_receive_vlan( tagged, 17, ROCKETNET_ADDRESS_IPV4, 7, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.not_ip == 3 && rocketnet_xdp_receive_drops_total( &drops ) == 3 );

    // a tagged frame cut inside its headers is malformed

    rocketnet_unit_check( !rocketnet_xdp_frame_receive_vlan( tagged, 45, ROCKETNET_ADDRESS_IPV4, 7, &from, &payload, &payload_bytes, &drops ) );
    rocketnet_unit_check( drops.malformed == 1 );
}

// the size limits: the payload limits are the untagged ones (1452 both
// families), the frame is 4 bytes longer, and the MTU judges the IP packet,
// which the tag does not change
static void test_xdp_vlan_size_limits()
{
    rocketnet_address_t s6, d6, s4, d4;
    rocketnet_unit_check( rocketnet_address_parse( &s6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d6, "[2001:db8::2]:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );

    uint8_t frame[2048];
    memset( frame, 0, sizeof( frame ) );
    const int ipv6 = rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s6, &d6, ROCKETNET_MAX_PACKET_BYTES, 9 );
    const int ipv4 = rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, ROCKETNET_RECEIVE_PACKET_BYTES, 9 );
    rocketnet_unit_check( ipv6 == 1518 );
    rocketnet_unit_check( ipv4 == 1498 );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s6, &d6, ROCKETNET_MAX_PACKET_BYTES + 1, 9 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, ROCKETNET_RECEIVE_PACKET_BYTES + 1, 9 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, 1517, server_mac, gateway_mac, &s6, &d6, ROCKETNET_MAX_PACKET_BYTES, 9 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, 1518, server_mac, gateway_mac, &s6, &d6, ROCKETNET_MAX_PACKET_BYTES, 9 ) == 1518 );

    rocketnet_unit_check( rocketnet_xdp_frame_fits_mtu_vlan( ipv6, 1500, 9 ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_fits_mtu_vlan( ipv6, 1499, 9 ) );
    rocketnet_unit_check( rocketnet_xdp_frame_fits_mtu_vlan( ipv4, 1480, 9 ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_fits_mtu_vlan( ipv4, 1479, 9 ) );
    rocketnet_unit_check( !rocketnet_xdp_frame_fits_mtu( ipv6, 1500 ) );

    // a VLAN ID outside 0..4094 is refused, nothing written

    memset( frame, 0x5A, sizeof( frame ) );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, 4, 4095 ) == 0 );
    rocketnet_unit_check( rocketnet_xdp_frame_build_vlan( frame, sizeof( frame ), server_mac, gateway_mac, &s4, &d4, 4, -1 ) == 0 );
    for ( size_t i = 0; i < sizeof( frame ); i++ )
    {
        rocketnet_unit_check( frame[i] == 0x5A );
    }

    // the send copy places the packet behind the tag, and refuses a frame
    // that fits the untagged header but not the tagged one

    uint8_t packet[ROCKETNET_MAX_PACKET_BYTES];
    memset( packet, 0xC3, sizeof( packet ) );
    uint64_t refused = 0;
    rocketnet_unit_check( rocketnet_xdp_send_copy_vlan( frame, sizeof( frame ), ROCKETNET_ADDRESS_IPV6, 9, packet, sizeof( packet ), &refused ) == (int) sizeof( packet ) );
    rocketnet_unit_check( frame[65] == 0x5A && frame[66] == 0xC3 && frame[66 + sizeof( packet ) - 1] == 0xC3 );
    rocketnet_unit_check( rocketnet_xdp_send_copy( frame, 1514, ROCKETNET_ADDRESS_IPV6, packet, sizeof( packet ), &refused ) == (int) sizeof( packet ) );
    rocketnet_unit_check( rocketnet_xdp_send_copy_vlan( frame, 1517, ROCKETNET_ADDRESS_IPV6, 9, packet, sizeof( packet ), &refused ) == -1 );
    rocketnet_unit_check( refused == 1 );
}

REGISTER_UNIT_TEST( test_xdp_vlan_frame_matches_golden_and_round_trips );
REGISTER_UNIT_TEST( test_xdp_vlan_parse_refuses_other_tags_and_takes_stripped_frames );
REGISTER_UNIT_TEST( test_xdp_vlan_receive_counts_refused_tags );
REGISTER_UNIT_TEST( test_xdp_vlan_size_limits );
REGISTER_UNIT_TEST( test_ipv4_frame_matches_golden );
REGISTER_UNIT_TEST( test_ipv6_frame_matches_golden );
REGISTER_UNIT_TEST( test_ipv6_udp_checksum_verifies );
REGISTER_UNIT_TEST( test_ipv6_checksum_zero_is_sent_as_ffff );
REGISTER_UNIT_TEST( test_ipv6_max_payload_is_1452_and_fits_1514 );
REGISTER_UNIT_TEST( test_xdp_frame_fits_mtu_judges_the_ip_packet );
REGISTER_UNIT_TEST( test_ipv6_payload_over_budget_is_refused );
REGISTER_UNIT_TEST( test_build_refuses_missing_inputs_and_short_frames );
REGISTER_UNIT_TEST( test_parse_round_trips_both_families );
REGISTER_UNIT_TEST( test_parse_refuses_extension_headers );
REGISTER_UNIT_TEST( test_parse_refuses_truncated_ipv6_frames );
REGISTER_UNIT_TEST( test_xdp_transport_reply_mac_follows_the_client );
REGISTER_UNIT_TEST( test_xdp_transport_frame_parse_ipv6_udp );
REGISTER_UNIT_TEST( test_xdp_transport_frame_build_ipv6_checksum_vector );
REGISTER_UNIT_TEST( test_xdp_transport_receive_ipv6_extension_dropped_counted );
REGISTER_UNIT_TEST( test_xdp_transport_frame_ipv4_unchanged );
REGISTER_UNIT_TEST( test_xdp_transport_receive_dispatches_both_families_counted );
REGISTER_UNIT_TEST( test_xdp_transport_order_keeps_xdp_for_ipv6 );
REGISTER_UNIT_TEST( test_xdp_transport_mac_address_parse_is_strict );
REGISTER_UNIT_TEST( test_xdp_cpu_list_entry_takes_cpu_zero );
REGISTER_UNIT_TEST( test_xdp_reply_macs_ports_differing_in_the_high_byte_spread );
#if ROCKETNET_XDP
REGISTER_UNIT_TEST( test_xdp_transport_ipv6_pin_reaches_the_xdp_transport_at_create );
#endif // #if ROCKETNET_XDP
