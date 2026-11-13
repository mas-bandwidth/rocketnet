/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Cases for the survivors of the 2026-11-13 candidate-delta mutation run over
// the XDP frame codec and reply MAC table (sdk/rocketnet_xdp_frame.h) and the
// segmented-send refusal table (sdk/rocketnet_server_gso.h): each pins a rule
// a surviving mutant broke with no test noticing. VLAN 1 is a tagged VLAN and
// a priority tag (VLAN 0) is no VLAN; a header-only frame parses; the IPv6 UDP
// length's high byte; the IPv6 extension check at exactly the fixed header,
// stepped over the tag; a parsed packet below the smallest server-inbound
// packet is dropped; the reply MAC table evicts the oldest answered entry,
// else the oldest learned one; the refusal count stops at the threshold.
//
// Header-only code: nothing here opens a socket, reads a clock or sleeps.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_frame.h"
#include "../sdk/rocketnet_server_gso.h"

#define check rocketnet_unit_check

static const uint8_t gaps_server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t gaps_gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

// a frame from 192.168.1.10:40000 (or [2001:db8::1]:40000) to the server, its
// payload payload_bytes of 0x5A, tagged with vlan_id when it is above 0.
// Returns the frame length (0: refused).
static int gaps_build( uint8_t * frame, size_t frame_bytes, int ipv6, int payload_bytes, int vlan_id )
{
    rocketnet_address_t source;
    rocketnet_address_t destination;
    if ( ipv6 )
    {
        check( rocketnet_address_parse( &source, "[2001:db8::1]:40000" ) );
        check( rocketnet_address_parse( &destination, "[2001:db8::2]:40001" ) );
    }
    else
    {
        check( rocketnet_address_parse( &source, "192.168.1.10:40000" ) );
        check( rocketnet_address_parse( &destination, "192.168.1.20:40001" ) );
    }

    const int header_bytes = rocketnet_xdp_frame_header_bytes_vlan( source.type, vlan_id );
    if ( payload_bytes > 0 && (size_t) ( header_bytes + payload_bytes ) <= frame_bytes )
    {
        memset( frame + header_bytes, 0x5A, (size_t) payload_bytes );
    }

    return rocketnet_xdp_frame_build_vlan( frame, frame_bytes, gaps_server_mac, gaps_gateway_mac, &source, &destination, payload_bytes, vlan_id );
}

// ----------------------------------------------------------------
// VLAN 1 is a VLAN; a priority tag is not this server's VLAN
// ----------------------------------------------------------------

// mutation gap: delta sdk/rocketnet_xdp_frame.h:126 off_by_one (vlan_id > 1)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:398 off_by_one (vlan_id <= 1)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:546 off_by_one (the untagged parse passes VLAN 1)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:695 off_by_one (the untagged receive passes VLAN 1)
static void test_gaps_delta_server_vlan_one_is_tagged()
{
    check( rocketnet_xdp_frame_tag_bytes( 1 ) == ROCKETNET_VLAN_TAG_BYTES && "a server on VLAN 1 must tag its frames: VLAN 1 lost its 802.1Q tag" );
    check( rocketnet_xdp_frame_header_bytes_vlan( ROCKETNET_ADDRESS_IPV4, 1 ) == 46 && "the VLAN 1 IPv4 header is not 14 + 4 + 20 + 8 bytes" );

    uint8_t frame[256];
    memset( frame, 0, sizeof( frame ) );
    const int payload_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
    const int length = gaps_build( frame, sizeof( frame ), 0, payload_bytes, 1 );
    check( length == 46 + payload_bytes && "a VLAN 1 frame is not 4 bytes longer than an untagged one" );
    check( frame[12] == 0x81 && frame[13] == 0x00 && frame[14] == 0x00 && frame[15] == 0x01 && "a VLAN 1 frame does not carry the 802.1Q tag of VLAN 1" );

    // a VLAN 1 server takes its own tagged frame

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_length = 0;
    check( rocketnet_xdp_frame_parse_vlan( frame, (size_t) length, 1, &from, &payload, &payload_length ) && "a VLAN 1 server refused a frame tagged with VLAN 1" );
    check( payload_length == payload_bytes && "a VLAN 1 frame parsed to the wrong payload length" );

    // an untagged server refuses it, by parse and by receive

    check( !rocketnet_xdp_frame_parse( frame, (size_t) length, &from, &payload, &payload_length ) && "the untagged parse took a frame tagged with VLAN 1" );

    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );
    check( !rocketnet_xdp_frame_receive( frame, (size_t) length, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_length, &drops ) && "the untagged receive took a frame tagged with VLAN 1" );
    check( drops.not_ip == 1 && rocketnet_xdp_receive_drops_total( &drops ) == 1 && "a VLAN 1 frame on an untagged receive was not counted not_ip" );

    // and so does a server of another VLAN

    check( !rocketnet_xdp_frame_parse_vlan( frame, (size_t) length, 2, &from, &payload, &payload_length ) && "a VLAN 2 server took a frame tagged with VLAN 1" );
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:398 relational (vlan_id < 0)
static void test_gaps_delta_server_priority_tag_is_not_untagged()
{
    // an 802.1Q tag with VLAN ID 0 (a priority tag) in front of a frame an
    // untagged server would take: the untagged server refuses any tag

    uint8_t frame[256];
    memset( frame, 0, sizeof( frame ) );
    const int length = gaps_build( frame, sizeof( frame ), 0, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, 5 );
    check( length > 0 );
    frame[14] = 0x00;
    frame[15] = 0x00;

    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_length = 0;
    check( rocketnet_xdp_frame_vlan_tag( frame, (size_t) length, 0 ) == -1 && "an untagged server read a priority tag (VLAN 0) as its own" );
    check( !rocketnet_xdp_frame_parse( frame, (size_t) length, &from, &payload, &payload_length ) && "the untagged parse took a priority-tagged frame" );

    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );
    check( !rocketnet_xdp_frame_receive( frame, (size_t) length, ROCKETNET_ADDRESS_IPV4, &from, &payload, &payload_length, &drops ) && "the untagged receive took a priority-tagged frame" );
    check( drops.not_ip == 1 && "a priority-tagged frame on an untagged receive was not counted not_ip" );
}

// ----------------------------------------------------------------
// the size edges of build, fit and parse
// ----------------------------------------------------------------

// mutation gap: delta sdk/rocketnet_xdp_frame.h:228 off_by_one (mtu <= 1)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:259 relational and off_by_one (payload_bytes <= 0, < 1)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:452 relational (e_bytes <= the IPv4 header)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:500 relational (e_bytes <= the IPv6 header)
static void test_gaps_delta_server_header_only_frames()
{
    // an MTU of 1 is a known MTU, and no frame fits it; only 0 is unknown

    check( !rocketnet_xdp_frame_fits_mtu( 14 + 20 + 8 + 1, 1 ) && "an MTU of 1 was read as unknown: a 29-byte IP packet fit it" );
    check( rocketnet_xdp_frame_fits_mtu( 14 + 1, 1 ) && "a 1-byte IP packet does not fit an MTU of 1" );

    // a zero-byte payload builds a header-only frame, which parses back to an
    // empty payload, in both families

    const int ipv6_header_bytes[2] = { 42, 62 };
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        uint8_t frame[128];
        memset( frame, 0, sizeof( frame ) );
        const int length = gaps_build( frame, sizeof( frame ), ipv6, 0, 0 );
        check( length == ipv6_header_bytes[ipv6] && "a zero-byte payload was refused: build returned no header-only frame" );

        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_length = -1;
        check( rocketnet_xdp_frame_parse( frame, (size_t) length, &from, &payload, &payload_length ) && "a header-only frame (exactly Ethernet + IP + UDP) was refused by the parse" );
        check( payload_length == 0 && "a header-only frame parsed to a nonzero payload" );
        check( from.port == 40000 && "a header-only frame parsed to the wrong source port" );
    }
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:505 off_by_one (<< 7, << 9)
static void test_gaps_delta_server_ipv6_udp_length_high_byte()
{
    // a UDP length of 300 + 8 = 0x0134: its high byte is read whole

    const int payload_sizes[3] = { 248, 300, 1200 };
    for ( int i = 0; i < 3; i++ )
    {
        uint8_t frame[1500];
        memset( frame, 0, sizeof( frame ) );
        const int length = gaps_build( frame, sizeof( frame ), 1, payload_sizes[i], 0 );
        check( length == 62 + payload_sizes[i] );

        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_length = 0;
        check( rocketnet_xdp_frame_parse( frame, (size_t) length, &from, &payload, &payload_length ) && "an IPv6 frame with a UDP length of 256 or more was refused: its length's high byte misread" );
        check( payload_length == payload_sizes[i] && "an IPv6 UDP length of 256 or more parsed to the wrong payload length" );
    }
}

// the IPv4 parse checks the frame holds the IPv4 and UDP headers before it
// reads either: frames cut short of 42 bytes, each in an allocation of exactly
// its own size, are refused without a read past the end (the ASan leg sees a
// read past it)
//
// mutation gap: delta sdk/rocketnet_xdp_frame.h:452 arithmetic (14 - 20 + 8, 14 + 20 - 8)
static void test_gaps_delta_server_short_ipv4_frames_read_nothing_past_the_end()
{
    uint8_t whole[128];
    memset( whole, 0, sizeof( whole ) );
    const int length = gaps_build( whole, sizeof( whole ), 0, 0, 0 );
    check( length == 42 );

    for ( int cut = 14; cut < 42; cut++ )
    {
        uint8_t * frame = (uint8_t *) malloc( (size_t) cut );
        check( frame );
        memcpy( frame, whole, (size_t) cut );

        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_length = 0;
        check( !rocketnet_xdp_frame_parse( frame, (size_t) cut, &from, &payload, &payload_length ) && "an IPv4 frame shorter than its headers was taken" );

        free( frame );
    }
}

// ----------------------------------------------------------------
// the receive dispatch: the extension check and the smallest packet
// ----------------------------------------------------------------

// an IPv6 frame of exactly the Ethernet and fixed IPv6 header, next header 0
// (hop-by-hop), optionally behind the tag of vlan_id
static size_t gaps_ipv6_extension_frame( uint8_t * frame, size_t frame_bytes, int vlan_id )
{
    const size_t tag_bytes = vlan_id > 0 ? 4 : 0;
    const size_t length = 14 + 40 + tag_bytes;
    check( length <= frame_bytes );
    memset( frame, 0, frame_bytes );
    memcpy( frame, gaps_gateway_mac, 6 );
    memcpy( frame + 6, gaps_server_mac, 6 );
    if ( vlan_id > 0 )
    {
        frame[12] = 0x81;
        frame[13] = 0x00;
        frame[14] = (uint8_t) ( vlan_id >> 8 );
        frame[15] = (uint8_t) ( vlan_id & 0xFF );
    }
    uint8_t * e = frame + tag_bytes;
    e[12] = 0x86;
    e[13] = 0xDD;
    e[14] = 0x60;
    e[20] = 0;          // hop-by-hop options
    e[21] = 64;
    return length;
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:641 relational (e_bytes > the fixed header)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:628 arithmetic (e_bytes = frame_bytes + tag_bytes)
static void test_gaps_delta_server_ipv6_extension_at_exactly_the_fixed_header()
{
    uint8_t frame[128];
    rocketnet_address_t from;
    const uint8_t * payload = NULL;
    int payload_length = 0;

    // untagged: 54 bytes hold the next header, so it is judged an extension

    {
        const size_t length = gaps_ipv6_extension_frame( frame, sizeof( frame ), 0 );
        check( length == 54 );
        struct rocketnet_xdp_receive_drops_t drops;
        memset( &drops, 0, sizeof( drops ) );
        check( !rocketnet_xdp_frame_receive( frame, length, ROCKETNET_ADDRESS_IPV6, &from, &payload, &payload_length, &drops ) );
        check( drops.ipv6_extension == 1 && drops.malformed == 0 && "a 54-byte IPv6 frame with a hop-by-hop next header was not counted ipv6_extension" );
    }

    // tagged on VLAN 5: the same frame behind the tag is judged the same

    {
        const size_t length = gaps_ipv6_extension_frame( frame, sizeof( frame ), 5 );
        check( length == 58 );
        struct rocketnet_xdp_receive_drops_t drops;
        memset( &drops, 0, sizeof( drops ) );
        check( !rocketnet_xdp_frame_receive_vlan( frame, length, ROCKETNET_ADDRESS_IPV6, 5, &from, &payload, &payload_length, &drops ) );
        check( drops.ipv6_extension == 1 && drops.malformed == 0 && "a tagged 58-byte IPv6 frame with a hop-by-hop next header was not counted ipv6_extension" );
    }

    // tagged, 4 bytes short of the fixed header behind the tag: too short to
    // judge the next header, so it is malformed, not an extension

    {
        const size_t length = gaps_ipv6_extension_frame( frame, sizeof( frame ), 5 ) - 4;
        struct rocketnet_xdp_receive_drops_t drops;
        memset( &drops, 0, sizeof( drops ) );
        check( !rocketnet_xdp_frame_receive_vlan( frame, length, ROCKETNET_ADDRESS_IPV6, 5, &from, &payload, &payload_length, &drops ) );
        check( drops.malformed == 1 && drops.ipv6_extension == 0 && "a tagged IPv6 frame short of the fixed header was judged by a next header past its end (the tag counted the wrong way)" );
    }
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:665 boolean (&& for ||)
static void test_gaps_delta_server_receive_drops_a_packet_below_the_smallest()
{
    for ( int ipv6 = 0; ipv6 < 2; ipv6++ )
    {
        uint8_t frame[256];
        memset( frame, 0, sizeof( frame ) );
        const int family = ipv6 ? ROCKETNET_ADDRESS_IPV6 : ROCKETNET_ADDRESS_IPV4;

        rocketnet_address_t from;
        const uint8_t * payload = NULL;
        int payload_length = 0;

        const int smallest = gaps_build( frame, sizeof( frame ), ipv6, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, 0 );
        struct rocketnet_xdp_receive_drops_t drops;
        memset( &drops, 0, sizeof( drops ) );
        check( rocketnet_xdp_frame_receive( frame, (size_t) smallest, family, &from, &payload, &payload_length, &drops ) && "the smallest server-inbound packet was not taken" );
        check( payload_length == ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );

        const int below = gaps_build( frame, sizeof( frame ), ipv6, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, 0 );
        memset( &drops, 0, sizeof( drops ) );
        check( !rocketnet_xdp_frame_receive( frame, (size_t) below, family, &from, &payload, &payload_length, &drops ) && "a well-formed packet one byte below the smallest server-inbound packet was taken" );
        check( drops.malformed == 1 && rocketnet_xdp_receive_drops_total( &drops ) == 1 && "a packet below the smallest server-inbound packet was not counted malformed" );
    }
}

// ----------------------------------------------------------------
// the reply MAC table's eviction in a full probe window
// ----------------------------------------------------------------

#define GAPS_REPLY_ENTRIES      ( 4 * ROCKETNET_XDP_REPLY_MAC_PROBES )
#define GAPS_REPLY_CLIENTS      ( ROCKETNET_XDP_REPLY_MAC_PROBES + 1 )

static struct rocketnet_xdp_reply_mac_entry_t gaps_reply_entries[GAPS_REPLY_ENTRIES];

// GAPS_REPLY_CLIENTS addresses that all hash to one slot, so the first
// ROCKETNET_XDP_REPLY_MAC_PROBES fill its probe window and the last finds it full
static void gaps_reply_colliding( struct rocketnet_xdp_reply_macs_t * table, rocketnet_address_t * clients )
{
    int found = 0;
    uint32_t want = 0;
    for ( int port = 1; port < 65536 && found < GAPS_REPLY_CLIENTS; port++ )
    {
        rocketnet_address_t client;
        memset( &client, 0, sizeof( client ) );
        client.type = ROCKETNET_ADDRESS_IPV4;
        client.data.ipv4[0] = 203;
        client.data.ipv4[1] = 0;
        client.data.ipv4[2] = 113;
        client.data.ipv4[3] = 9;
        client.port = (uint16_t) port;
        const uint32_t slot = rocketnet_xdp_reply_macs_slot( table, &client );
        if ( found == 0 )
        {
            want = slot;
        }
        if ( slot == want )
        {
            clients[found++] = client;
        }
    }
    check( found == GAPS_REPLY_CLIENTS );
}

static void gaps_reply_mac( uint8_t * mac, int i )
{
    const uint8_t value[6] = { 0x02, 0, 0, 0, 0, (uint8_t) ( i + 1 ) };
    memcpy( mac, value, 6 );
}

static bool gaps_reply_live( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * client, int i )
{
    const struct rocketnet_xdp_reply_mac_entry_t * entry = rocketnet_xdp_reply_macs_find( table, client );
    return entry != NULL && entry->mac[5] == (uint8_t) ( i + 1 );
}

static bool gaps_reply_answered( struct rocketnet_xdp_reply_macs_t * table, const rocketnet_address_t * client )
{
    return rocketnet_xdp_reply_macs_find_state( table, client, ROCKETNET_XDP_REPLY_MAC_ANSWERED ) != NULL;
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:1232 negate_condition and relational (state != ANSWERED)
// mutation gap: delta sdk/rocketnet_xdp_frame.h:1234 negate_condition and relational (learned > oldest)
static void test_gaps_delta_server_reply_learn_evicts_the_oldest_answered()
{
    struct rocketnet_xdp_reply_macs_t table;
    memset( &table, 0, sizeof( table ) );
    check( rocketnet_xdp_reply_macs_bind( &table, gaps_reply_entries, GAPS_REPLY_ENTRIES ) );

    rocketnet_address_t clients[GAPS_REPLY_CLIENTS];
    gaps_reply_colliding( &table, clients );

    uint8_t mac[6];
    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; i++ )
    {
        gaps_reply_mac( mac, i );
        rocketnet_xdp_reply_macs_learn( &table, &clients[i], mac );
    }

    // two answered entries: client 9 (learned 10th) and client 3, learned
    // again after it so it is the younger, though first in the window

    gaps_reply_mac( mac, 3 );
    rocketnet_xdp_reply_macs_learn( &table, &clients[3], mac );
    rocketnet_xdp_reply_macs_forget( &table, &clients[3] );
    rocketnet_xdp_reply_macs_forget( &table, &clients[9] );
    check( gaps_reply_answered( &table, &clients[3] ) && gaps_reply_answered( &table, &clients[9] ) );

    const int last = GAPS_REPLY_CLIENTS - 1;
    gaps_reply_mac( mac, last );
    rocketnet_xdp_reply_macs_learn( &table, &clients[last], mac );

    check( gaps_reply_live( &table, &clients[last], last ) && "a learn into a full window kept no entry" );
    check( !gaps_reply_answered( &table, &clients[9] ) && "a learn into a full window did not take the oldest answered entry" );
    check( gaps_reply_answered( &table, &clients[3] ) && "a learn into a full window took the younger answered entry, not the oldest" );
    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; i++ )
    {
        if ( i != 3 && i != 9 )
        {
            check( gaps_reply_live( &table, &clients[i], i ) && "a learn into a full window with an answered entry displaced a learned entry" );
        }
    }
}

// mutation gap: delta sdk/rocketnet_xdp_frame.h:1239 relational (learned > oldest)
static void test_gaps_delta_server_reply_learn_evicts_the_oldest_learned()
{
    struct rocketnet_xdp_reply_macs_t table;
    memset( &table, 0, sizeof( table ) );
    check( rocketnet_xdp_reply_macs_bind( &table, gaps_reply_entries, GAPS_REPLY_ENTRIES ) );

    rocketnet_address_t clients[GAPS_REPLY_CLIENTS];
    gaps_reply_colliding( &table, clients );

    uint8_t mac[6];
    for ( int i = 0; i < ROCKETNET_XDP_REPLY_MAC_PROBES; i++ )
    {
        gaps_reply_mac( mac, i );
        rocketnet_xdp_reply_macs_learn( &table, &clients[i], mac );
    }

    // client 0 learned again: client 1 is now the learned longest ago

    gaps_reply_mac( mac, 0 );
    rocketnet_xdp_reply_macs_learn( &table, &clients[0], mac );

    const int last = GAPS_REPLY_CLIENTS - 1;
    gaps_reply_mac( mac, last );
    rocketnet_xdp_reply_macs_learn( &table, &clients[last], mac );

    check( gaps_reply_live( &table, &clients[last], last ) && "a learn into a full window kept no entry" );
    check( rocketnet_xdp_reply_macs_find( &table, &clients[1] ) == NULL && "a learn into a full window of learned entries did not evict the one learned longest ago" );
    check( gaps_reply_live( &table, &clients[0], 0 ) && "a learn into a full window evicted the entry learned most recently" );
    for ( int i = 2; i < ROCKETNET_XDP_REPLY_MAC_PROBES; i++ )
    {
        check( gaps_reply_live( &table, &clients[i], i ) && "a learn into a full window evicted a learned entry that was not the oldest" );
    }
}

// ----------------------------------------------------------------
// the segmented-send refusal count stops at the threshold
// ----------------------------------------------------------------

// mutation gap: delta sdk/rocketnet_server_gso.h:191 relational (destinations <= threshold)
static void test_gaps_delta_server_segment_refusals_stop_at_the_threshold()
{
    for ( int accepted = 0; accepted < 2; accepted++ )
    {
        struct rocketnet_segment_refused_t refused;
        memset( &refused, 0, sizeof( refused ) );
        refused.accepted = accepted != 0;

        for ( int i = 0; i < 2 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD; i++ )
        {
            rocketnet_address_t destination;
            memset( &destination, 0, sizeof( destination ) );
            destination.type = ROCKETNET_ADDRESS_IPV4;
            destination.data.ipv4[0] = 198;
            destination.data.ipv4[1] = 51;
            destination.data.ipv4[2] = 100;
            destination.data.ipv4[3] = (uint8_t) ( i + 1 );
            destination.port = 50000;

            const bool fallback = rocketnet_segment_refused_add( &refused, &destination );
            const int want = i + 1 < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD ? i + 1 : ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
            check( refused.destinations == want && "the refusal count did not stop at ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD" );
            check( fallback == ( !accepted && i + 1 >= ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD ) && "segmentation turned off at the wrong refusal" );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_delta_server_vlan_one_is_tagged );
REGISTER_UNIT_TEST( test_gaps_delta_server_priority_tag_is_not_untagged );
REGISTER_UNIT_TEST( test_gaps_delta_server_header_only_frames );
REGISTER_UNIT_TEST( test_gaps_delta_server_ipv6_udp_length_high_byte );
REGISTER_UNIT_TEST( test_gaps_delta_server_short_ipv4_frames_read_nothing_past_the_end );
REGISTER_UNIT_TEST( test_gaps_delta_server_ipv6_extension_at_exactly_the_fixed_header );
REGISTER_UNIT_TEST( test_gaps_delta_server_receive_drops_a_packet_below_the_smallest );
REGISTER_UNIT_TEST( test_gaps_delta_server_reply_learn_evicts_the_oldest_answered );
REGISTER_UNIT_TEST( test_gaps_delta_server_reply_learn_evicts_the_oldest_learned );
REGISTER_UNIT_TEST( test_gaps_delta_server_segment_refusals_stop_at_the_threshold );
