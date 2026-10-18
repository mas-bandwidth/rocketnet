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

#include "sdk/rocketnet.h"
#include "sdk/rocketnet_address.h"
#include "sdk/rocketnet_xdp_frame.h"

static const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t golden_payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
static const uint8_t golden_ipv4[] = {
    0x02,0x00,0x00,0x00,0x00,0x02, 0x02,0x00,0x00,0x00,0x00,0x01,
    0x08,0x00,
    0x45,0x00,0x00,0x20,0x00,0x00,0x40,0x00,0x40,0x11,0xb7,0x5e,
    0xc0,0xa8,0x01,0x0a,0xc0,0xa8,0x01,0x14,
    0x9c,0x40,0x9c,0x41,0x00,0x0c,0x00,0x00,
    0xde,0xad,0xbe,0xef
};
static const uint8_t golden_ipv6[] = {
    0x02,0x00,0x00,0x00,0x00,0x02, 0x02,0x00,0x00,0x00,0x00,0x01,
    0x86,0xdd,
    0x60,0x00,0x00,0x00,0x00,0x0c,0x11,0x40,
    0x20,0x01,0x0d,0xb8,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,
    0x20,0x01,0x0d,0xb8,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,
    0x9c,0x40,0x9c,0x41,0x00,0x0c,0xce,0x41,
    0xde,0xad,0xbe,0xef
};

static void ipv4_frame_matches_golden()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "192.168.1.20:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof(frame) );
    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( header_bytes == 42 );
    memcpy( frame + header_bytes, golden_payload, sizeof(golden_payload) );

    int len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &dst, (int) sizeof(golden_payload) );
    rocketnet_unit_check( len == 46 );
    rocketnet_unit_check( memcmp( frame, golden_ipv4, sizeof(golden_ipv4) ) == 0 );
}

static void ipv6_frame_matches_golden()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof(frame) );
    const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( header_bytes == 62 );
    memcpy( frame + header_bytes, golden_payload, sizeof(golden_payload) );

    int len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &dst, (int) sizeof(golden_payload) );
    rocketnet_unit_check( len == 66 );
    rocketnet_unit_check( memcmp( frame, golden_ipv6, sizeof(golden_ipv6) ) == 0 );
}

// an independent ones'-complement fold, for the second checksum check.

static uint32_t fold16( uint32_t sum )
{
    while ( sum >> 16 )
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    return sum;
}

static uint32_t sum_words( const uint8_t * data, size_t bytes )
{
    uint32_t sum = 0;
    size_t i = 0;
    while ( i + 1 < bytes )
    {
        sum += ( (uint16_t) data[i] << 8 ) | data[i + 1];
        i += 2;
    }
    if ( i < bytes )
        sum += ( (uint16_t) data[i] << 8 );
    return sum;
}

static void ipv6_udp_checksum_verifies()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 62, golden_payload, sizeof(golden_payload) );
    int len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &dst, (int) sizeof(golden_payload) );
    rocketnet_unit_check( len == 66 );

    // pseudo-header: source (16), destination (16), UDP length (32), three
    // zero bytes, next header 17.
    uint8_t pseudo[40];
    memcpy( pseudo, frame + 22, 32 );
    pseudo[32] = 0; pseudo[33] = 0; pseudo[34] = 0; pseudo[35] = 0x0c;
    pseudo[36] = 0; pseudo[37] = 0; pseudo[38] = 0; pseudo[39] = 0x11;

    uint32_t sum = sum_words( pseudo, sizeof(pseudo) )
                 + sum_words( frame + 54, 8 + sizeof(golden_payload) );
    rocketnet_unit_check( fold16( sum ) == 0xFFFF );
}

// the "computed zero is sent as FFFF" rule (RFC 768), driven through the
// checksum helper with a crafted pseudo-header + UDP segment whose ones-sum
// folds to 0xFFFF.

static void ipv6_checksum_zero_is_sent_as_ffff()
{
    uint8_t src[16], dst[16];
    memset( src, 0, sizeof(src) );
    memset( dst, 0, sizeof(dst) );
    uint8_t udp[8] = { 0xFF, 0xDE, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 };
    uint16_t csum = rocketnet_xdp_frame_ipv6_udp_checksum( src, dst, 8, udp, 0 );
    rocketnet_unit_check( csum == 0xFFFF );
}

static void ipv6_max_payload_is_1452_and_fits_1514()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[2048];
    memset( frame, 0xAB, sizeof(frame) );
    const int payload = ROCKETNET_MAX_PACKET_BYTES_IPV6;
    rocketnet_unit_check( payload == 1452 );
    memcpy( frame + 62, frame + 1000, 0 ); // no-op; payload stays as placed
    int len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &dst, payload );
    rocketnet_unit_check( len == 14 + 40 + 8 + payload );
    rocketnet_unit_check( len == 1514 );
}

static void ipv6_payload_over_budget_is_refused()
{
    rocketnet_address_t src, dst;
    rocketnet_unit_check( rocketnet_address_parse( &src, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst, "[2001:db8::2]:40001" ) );

    uint8_t frame[2048];
    int len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &dst, ROCKETNET_MAX_PACKET_BYTES_IPV6 + 1 );
    rocketnet_unit_check( len == 0 );

    rocketnet_address_t s4, d4;
    rocketnet_unit_check( rocketnet_address_parse( &s4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &d4, "192.168.1.20:40001" ) );
    len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &s4, &d4, ROCKETNET_MAX_PACKET_BYTES + 1 );
    rocketnet_unit_check( len == 0 );

    // a family mismatch between the two addresses is refused too.
    len = rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src, &d4, 4 );
    rocketnet_unit_check( len == 0 );
}

static void parse_round_trips_both_families()
{
    rocketnet_address_t src4, dst4, src6, dst6;
    rocketnet_unit_check( rocketnet_address_parse( &src4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst4, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &src6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    uint8_t frame[256];

    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 42, golden_payload, sizeof(golden_payload) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src4, &dst4, (int) sizeof(golden_payload) ) == 46 );
    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;
    rocketnet_unit_check( rocketnet_xdp_frame_parse( frame, 46, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.port == 40000 );
    rocketnet_unit_check( from.data.ipv4[0] == 192 && from.data.ipv4[1] == 168 );
    rocketnet_unit_check( from.data.ipv4[2] == 1 && from.data.ipv4[3] == 10 );
    rocketnet_unit_check( payload_bytes == 4 && memcmp( payload, golden_payload, 4 ) == 0 );

    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 62, golden_payload, sizeof(golden_payload) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src6, &dst6, (int) sizeof(golden_payload) ) == 66 );
    rocketnet_unit_check( rocketnet_xdp_frame_parse( frame, 66, &from, &payload, &payload_bytes ) );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( from.port == 40000 );
    rocketnet_unit_check( from.data.ipv6[0] == 0x2001 && from.data.ipv6[1] == 0x0db8 );
    rocketnet_unit_check( from.data.ipv6[7] == 1 );
    rocketnet_unit_check( payload_bytes == 4 && memcmp( payload, golden_payload, 4 ) == 0 );
}

static void parse_refuses_extension_headers()
{
    rocketnet_address_t src6, dst6;
    rocketnet_unit_check( rocketnet_address_parse( &src6, "[2001:db8::1]:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst6, "[2001:db8::2]:40001" ) );

    uint8_t frame[128];
    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 62, golden_payload, sizeof(golden_payload) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src6, &dst6, (int) sizeof(golden_payload) ) == 66 );

    rocketnet_address_t from;
    const uint8_t * payload;
    int payload_bytes;

    // next header not 17 (e.g. an extension header) is refused.
    frame[20] = 0x3c; // IPv6 fragment header
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, 66, &from, &payload, &payload_bytes ) );
    frame[20] = 17;

    // IPv4 with ihl != 5 is refused.
    rocketnet_address_t src4, dst4;
    rocketnet_unit_check( rocketnet_address_parse( &src4, "192.168.1.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &dst4, "192.168.1.20:40001" ) );
    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 42, golden_payload, sizeof(golden_payload) );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, gateway_mac, &src4, &dst4, (int) sizeof(golden_payload) ) == 46 );
    frame[14] = 0x46;
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, 46, &from, &payload, &payload_bytes ) );

    // a non-IP ethertype is refused.
    memset( frame, 0, sizeof(frame) );
    frame[12] = 0x08; frame[13] = 0x06; // ARP
    rocketnet_unit_check( !rocketnet_xdp_frame_parse( frame, sizeof(frame), &from, &payload, &payload_bytes ) );
}

// the reply-MAC rule (rocketnet_xdp_reply_mac): learned peer, gateway
// otherwise, the newest MAC when a peer's source MAC changes, and a refusal
// counted when there is neither. No socket: the table is driven directly.

static struct rocketnet_xdp_reply_macs_t reply_macs_under_test;

static void xdp_reply_mac_follows_the_peer()
{
    static const uint8_t peer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
    static const uint8_t peer_new_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

    struct rocketnet_xdp_reply_macs_t * table = &reply_macs_under_test;
    rocketnet_address_t peer, stranger, peer6;
    rocketnet_unit_check( rocketnet_address_parse( &peer, "192.168.1.20:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &stranger, "192.168.1.21:40001" ) );
    rocketnet_unit_check( rocketnet_address_parse( &peer6, "[2001:db8::20]:40001" ) );
    uint64_t refused = 0;

    // with a gateway: an unlearned address goes to the gateway
    rocketnet_xdp_reply_macs_reset( table, gateway_mac );
    const uint8_t * mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // an answered packet from the peer teaches its MAC: replies follow it,
    // and only for that address
    rocketnet_xdp_reply_macs_learn( table, &peer, peer_mac );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, peer_mac, 6 ) == 0 );
    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // the same IP on another port is another peer
    rocketnet_address_t other_port = peer;
    other_port.port = 40002;
    mac = rocketnet_xdp_reply_mac( table, &other_port, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // the peer's source MAC changes: the newest one wins
    rocketnet_xdp_reply_macs_learn( table, &peer, peer_new_mac );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, peer_new_mac, 6 ) == 0 );

    // a multicast or zero source MAC is never learned
    rocketnet_xdp_reply_macs_learn( table, &stranger, zero_mac );
    static const uint8_t multicast_mac[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    rocketnet_xdp_reply_macs_learn( table, &stranger, multicast_mac );
    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    // IPv6 peers are learned the same way
    rocketnet_xdp_reply_macs_learn( table, &peer6, peer_mac );
    mac = rocketnet_xdp_reply_mac( table, &peer6, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, peer_mac, 6 ) == 0 );

    // a forgotten peer falls back to the gateway
    rocketnet_xdp_reply_macs_forget( table, &peer );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );
    rocketnet_unit_check( refused == 0 );

    // no gateway: learned peers still get their MAC; an unlearned address is
    // refused and counted, never given a zero MAC
    rocketnet_xdp_reply_macs_reset( table, NULL );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 1 );
    rocketnet_xdp_reply_macs_learn( table, &peer, peer_mac );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( mac && memcmp( mac, peer_mac, 6 ) == 0 );
    mac = rocketnet_xdp_reply_mac( table, &stranger, NULL, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 2 );

    // a direct response to an unlearned address goes to the source MAC of
    // the frame it answers (how a connect handshake on link gets its first
    // replies), never to a zero or multicast one; a learned peer keeps its
    // learned MAC whatever an unauthenticated frame from its address carries
    static const uint8_t stranger_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0c };
    mac = rocketnet_xdp_reply_mac( table, &stranger, stranger_mac, &refused );
    rocketnet_unit_check( mac && memcmp( mac, stranger_mac, 6 ) == 0 );
    mac = rocketnet_xdp_reply_mac( table, &stranger, zero_mac, &refused );
    rocketnet_unit_check( mac == NULL );
    mac = rocketnet_xdp_reply_mac( table, &stranger, multicast_mac, &refused );
    rocketnet_unit_check( mac == NULL );
    rocketnet_unit_check( refused == 4 );
    mac = rocketnet_xdp_reply_mac( table, &peer, stranger_mac, &refused );
    rocketnet_unit_check( mac && memcmp( mac, peer_mac, 6 ) == 0 );

    // the table is fixed-size: far more peers than entries all resolve to a
    // MAC learned for them or to the refusal, and the newest peer is held
    rocketnet_address_t many = peer;
    uint8_t many_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_ENTRIES * 2; ++i )
    {
        many.port = (uint16_t) ( 1000 + i );
        many_mac[4] = (uint8_t) ( i >> 8 );
        many_mac[5] = (uint8_t) i;
        rocketnet_xdp_reply_macs_learn( table, &many, many_mac );
        mac = rocketnet_xdp_reply_mac( table, &many, NULL, &refused );
        rocketnet_unit_check( mac && memcmp( mac, many_mac, 6 ) == 0 );
    }
    rocketnet_unit_check( refused == 4 );

    // the frame built for a learned peer carries its MAC as the destination
    rocketnet_xdp_reply_macs_reset( table, gateway_mac );
    rocketnet_xdp_reply_macs_learn( table, &peer, peer_mac );
    rocketnet_address_t src;
    rocketnet_unit_check( rocketnet_address_parse( &src, "192.168.1.10:40000" ) );
    uint8_t frame[128];
    memset( frame, 0, sizeof(frame) );
    memcpy( frame + 42, golden_payload, sizeof(golden_payload) );
    mac = rocketnet_xdp_reply_mac( table, &peer, NULL, &refused );
    rocketnet_unit_check( rocketnet_xdp_frame_build( frame, sizeof(frame), server_mac, mac, &src, &peer, (int) sizeof(golden_payload) ) == 46 );
    rocketnet_unit_check( memcmp( frame, peer_mac, 6 ) == 0 );
    rocketnet_unit_check( memcmp( frame + 6, server_mac, 6 ) == 0 );
}

REGISTER_UNIT_TEST( ipv4_frame_matches_golden );
REGISTER_UNIT_TEST( ipv6_frame_matches_golden );
REGISTER_UNIT_TEST( ipv6_udp_checksum_verifies );
REGISTER_UNIT_TEST( ipv6_checksum_zero_is_sent_as_ffff );
REGISTER_UNIT_TEST( ipv6_max_payload_is_1452_and_fits_1514 );
REGISTER_UNIT_TEST( ipv6_payload_over_budget_is_refused );
REGISTER_UNIT_TEST( parse_round_trips_both_families );
REGISTER_UNIT_TEST( parse_refuses_extension_headers );
REGISTER_UNIT_TEST( xdp_reply_mac_follows_the_peer );
