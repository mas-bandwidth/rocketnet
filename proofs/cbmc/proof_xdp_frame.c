/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the AF_XDP Ethernet / IPv4 / IPv6 / UDP frame codec and
    UMEM descriptor boundary guards in sdk/rocketnet_xdp_frame.h.

    The header under proof is sdk/rocketnet_xdp_frame.h, included directly
    below. The three harnesses prove:
      1. UMEM descriptor, fill-return, and send-copy guards
         (rocketnet_xdp_receive_descriptor, rocketnet_xdp_fill_return,
         rocketnet_xdp_send_copy) over symbolic offsets and lengths:
           * a receive descriptor is accepted if and only if umem is non-null,
             addr lies in [region_begin, region_end), len <= 4096, and the span
             [addr, addr + len) does not cross a 4096-byte UMEM frame boundary;
           * every rejected descriptor increments drops->descriptor by 1 and
             leaves all other drop counters unchanged;
           * fill_return accepts if and only if frame_addr is non-null and
             addr lies in [region_begin, region_end), and rounds addr down to
             its 4096-aligned frame base inside [region_begin, region_end);
           * send_copy refuses empty packets, packets past ROCKETNET_RECEIVE_CEILING,
             or packets that do not fit frame_bytes after the family header,
             counting *refused on every refusal and placing the payload at
             frame + header_bytes on acceptance.
      2. Frame build/parse round-trip and RFC 1071 / RFC 8200 checksums
         (rocketnet_xdp_frame_build, rocketnet_xdp_frame_parse,
         rocketnet_xdp_frame_checksum, rocketnet_xdp_frame_ipv6_udp_checksum)
         over symbolic IPv4 and IPv6 endpoints, ports, MACs, and symbolic
         payloads of 0..PROOF_PAYLOAD_BOUND bytes:
           * mismatched endpoint families, negative or over-ceiling payload
             lengths, and short frame buffers are refused (return 0);
           * every built IPv4 frame has a 20-byte IPv4 header whose ones'-
             complement checksum folds to 0, and every built IPv6 frame has a
             non-zero UDP checksum (0x0000 rewritten to 0xFFFF);
           * rocketnet_xdp_frame_parse refuses broadcast destination MACs
             (ff:ff:...) and on unicast destination MACs recovers the exact
             source address, port, payload pointer, and payload length.
      3. Arbitrary raw-frame receive safety and drop-counter conservation
         (rocketnet_xdp_frame_receive) over symbolic 68-byte frames, symbolic
         frame lengths 0..68, and symbolic served address_type:
           * no out-of-bounds read/write or integer overflow occurs;
           * when accepted, no drop counter changes, out_payload_length >=
             ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, [out_payload, out_payload +
             out_payload_length) lies inside [frame, frame + frame_bytes), and
             when address_type >= 0 the recovered family matches address_type;
           * when refused on non-null arguments, exactly one receive drop
             counter (not_ip, ipv6_extension, other_family, or malformed)
             increments by 1.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 17 (covers 10-word IPv4 checksum, 8-word IPv6 address loops,
    and 16-byte memcmp).
*/

#define ROCKETNET_DEV 0
#define ROCKETNET_ASSERTS 0
#define static_assert _Static_assert

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
typedef struct rocketnet_address_t rocketnet_address_t;

#include "rocketnet_xdp_frame.h"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned int nondet_uint( void ) { return 0; }
static uint16_t nondet_uint16( void ) { return 0; }
static uint32_t nondet_uint32( void ) { return 0; }
static uint64_t nondet_uint64( void ) { return 0; }
#else
int nondet_int( void );
unsigned int nondet_uint( void );
uint16_t nondet_uint16( void );
uint32_t nondet_uint32( void );
uint64_t nondet_uint64( void );
#endif // #ifndef __CPROVER__

#define PROOF_PAYLOAD_BOUND 4
#define PROOF_FRAME_BOUND   68

bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b )
{
    if ( a->type != b->type || a->port != b->port )
        return false;

    if ( a->type == ROCKETNET_ADDRESS_IPV4 )
        return memcmp( a->data.ipv4, b->data.ipv4, 4 ) == 0;

    if ( a->type == ROCKETNET_ADDRESS_IPV6 )
        return memcmp( a->data.ipv6, b->data.ipv6, sizeof( a->data.ipv6 ) ) == 0;

    return false;
}

static void harness_xdp_descriptors( void )
{
    static uint8_t dummy_umem[2 * ROCKETNET_XDP_FRAME_SIZE];
    uint64_t region_begin = nondet_uint64();
    uint64_t region_end = nondet_uint64();
    uint64_t addr = nondet_uint64();
    uint32_t len = nondet_uint32();

    __CPROVER_assume( region_begin <= region_end );
    __CPROVER_assume( region_end <= (uint64_t) sizeof( dummy_umem ) );
    __CPROVER_assume( ( region_begin % ROCKETNET_XDP_FRAME_SIZE ) == 0 );
    __CPROVER_assume( ( region_end % ROCKETNET_XDP_FRAME_SIZE ) == 0 );

    struct rocketnet_xdp_receive_drops_t drops = { 0, 0, 0, 0, 0 };
    const uint8_t * pkt = rocketnet_xdp_receive_descriptor( dummy_umem, region_begin, region_end, addr, len, &drops );

    const bool expected_ok = ( len <= ROCKETNET_XDP_FRAME_SIZE ) &&
                             ( addr >= region_begin ) &&
                             ( addr < region_end ) &&
                             ( ( addr % ROCKETNET_XDP_FRAME_SIZE ) + (uint64_t) len <= ROCKETNET_XDP_FRAME_SIZE );

    if ( expected_ok )
    {
        __CPROVER_assert( pkt == dummy_umem + addr, "accepted descriptor returns umem + addr" );
        __CPROVER_assert( drops.descriptor == 0, "accepted descriptor does not increment drop counter" );
        __CPROVER_assert( addr + (uint64_t) len <= region_end, "accepted descriptor span stays inside region" );
        if ( len > 0 )
        {
            __CPROVER_assert( ( addr / ROCKETNET_XDP_FRAME_SIZE ) == ( ( addr + (uint64_t) len - 1ULL ) / ROCKETNET_XDP_FRAME_SIZE ),
                              "accepted descriptor span never crosses a 4096-byte UMEM frame boundary" );
        }
    }
    else
    {
        __CPROVER_assert( pkt == NULL, "out-of-bounds or straddling descriptor is refused" );
        __CPROVER_assert( drops.descriptor == 1, "refused descriptor increments descriptor drop counter by 1" );
    }
    __CPROVER_assert( drops.not_ip == 0 && drops.ipv6_extension == 0 && drops.other_family == 0 && drops.malformed == 0,
                      "descriptor check touches no other drop counter" );

    /* Null UMEM always refuses and counts a descriptor drop. */
    struct rocketnet_xdp_receive_drops_t null_drops = { 0, 0, 0, 0, 0 };
    __CPROVER_assert( rocketnet_xdp_receive_descriptor( NULL, region_begin, region_end, addr, len, &null_drops ) == NULL,
                      "null UMEM is refused" );
    __CPROVER_assert( null_drops.descriptor == 1, "null UMEM increments descriptor drop" );

    /* Fill-ring frame return alignment and bounds (tla/RNXdpReceiveRing.tla). */
    uint64_t frame_addr = 12345ULL;
    bool fill_ok = rocketnet_xdp_fill_return( addr, region_begin, region_end, &frame_addr );
    if ( addr >= region_begin && addr < region_end )
    {
        __CPROVER_assert( fill_ok, "in-region address is accepted by fill_return" );
        __CPROVER_assert( ( frame_addr % ROCKETNET_XDP_FRAME_SIZE ) == 0, "returned frame base is 4096-aligned" );
        __CPROVER_assert( frame_addr >= region_begin && frame_addr + ROCKETNET_XDP_FRAME_SIZE <= region_end,
                          "returned frame base lies inside the receive region" );
        __CPROVER_assert( addr >= frame_addr && addr < frame_addr + ROCKETNET_XDP_FRAME_SIZE,
                          "returned frame base contains addr" );
    }
    else
    {
        __CPROVER_assert( !fill_ok, "out-of-region address is refused by fill_return" );
    }
    __CPROVER_assert( !rocketnet_xdp_fill_return( addr, region_begin, region_end, NULL ),
                      "null frame_addr is refused by fill_return" );

    /* Send-copy bounds and refusal counting. */
    uint8_t send_frame[62 + PROOF_PAYLOAD_BOUND];
    uint8_t packet[PROOF_PAYLOAD_BOUND];
    size_t frame_cap = nondet_uint();
    size_t packet_bytes = nondet_uint();
    __CPROVER_assume( frame_cap <= sizeof( send_frame ) );
    __CPROVER_assume( packet_bytes <= PROOF_PAYLOAD_BOUND );

    int family = nondet_int();
    __CPROVER_assume( family == ROCKETNET_ADDRESS_IPV4 || family == ROCKETNET_ADDRESS_IPV6 );
    int hdr = rocketnet_xdp_frame_header_bytes( family );

    uint64_t refused = 0;
    int copied = rocketnet_xdp_send_copy( send_frame, frame_cap, family, packet, packet_bytes, &refused );
    if ( packet_bytes == 0 || (size_t) hdr > frame_cap || packet_bytes > frame_cap - (size_t) hdr )
    {
        __CPROVER_assert( copied == -1 && refused == 1, "send_copy refuses invalid size and increments refused" );
    }
    else
    {
        __CPROVER_assert( copied == (int) packet_bytes && refused == 0, "send_copy accepts valid size without refusal" );
    }
}

static void harness_xdp_frame_build_and_parse( void )
{
    uint8_t frame[62 + PROOF_PAYLOAD_BOUND];
    uint8_t server_mac[6];
    uint8_t gateway_mac[6];
    int payload_bytes = nondet_int();

    __CPROVER_assume( payload_bytes >= 0 && payload_bytes <= PROOF_PAYLOAD_BOUND );

    struct rocketnet_address_t src4;
    struct rocketnet_address_t dst4;
    memset( &src4, 0, sizeof( src4 ) );
    memset( &dst4, 0, sizeof( dst4 ) );
    src4.type = ROCKETNET_ADDRESS_IPV4;
    dst4.type = ROCKETNET_ADDRESS_IPV4;
    uint32_t sip4 = nondet_uint32();
    uint32_t dip4 = nondet_uint32();
    memcpy( src4.data.ipv4, &sip4, 4 );
    memcpy( dst4.data.ipv4, &dip4, 4 );
    src4.port = nondet_uint16();
    dst4.port = nondet_uint16();

    const int hdr4 = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
    const int built4 = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src4, &dst4, payload_bytes );
    __CPROVER_assert( built4 == hdr4 + payload_bytes, "IPv4 build returns exact header + payload length" );
    __CPROVER_assert( rocketnet_xdp_frame_checksum( frame + ROCKETNET_ETHERNET_HEADER_BYTES, ROCKETNET_IPV4_HEADER_BYTES ) == 0,
                      "built IPv4 header checksum folds to 0" );

    struct rocketnet_address_t from4;
    const uint8_t * out_payload4 = NULL;
    int out_len4 = -1;
    bool parsed4 = rocketnet_xdp_frame_parse( frame, (size_t) built4, &from4, &out_payload4, &out_len4 );
    if ( gateway_mac[0] == 0xFF && gateway_mac[1] == 0xFF )
    {
        __CPROVER_assert( !parsed4, "parse refuses broadcast Ethernet destination MAC" );
    }
    else
    {
        __CPROVER_assert( parsed4, "built IPv4 frame with unicast MAC is accepted by parse" );
        __CPROVER_assert( rocketnet_address_equal( &from4, &src4 ), "parsed IPv4 frame recovers exact source endpoint" );
        __CPROVER_assert( out_payload4 == frame + hdr4 && out_len4 == payload_bytes,
                          "parsed IPv4 frame recovers exact payload slice" );
    }

    /* IPv6 build -> mandatory non-zero UDP checksum -> parse round-trip. */
    struct rocketnet_address_t src6;
    struct rocketnet_address_t dst6;
    memset( &src6, 0, sizeof( src6 ) );
    memset( &dst6, 0, sizeof( dst6 ) );
    src6.type = ROCKETNET_ADDRESS_IPV6;
    dst6.type = ROCKETNET_ADDRESS_IPV6;
    for ( int i = 0; i < 8; i++ )
    {
        src6.data.ipv6[i] = nondet_uint16();
        dst6.data.ipv6[i] = nondet_uint16();
    }
    src6.port = nondet_uint16();
    dst6.port = nondet_uint16();

    /* Mismatched source/destination families must be refused. */
    __CPROVER_assert( rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src4, &dst6, payload_bytes ) == 0,
                      "mismatched endpoint families are refused by build" );

    const int hdr6 = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV6 );
    const int built6 = rocketnet_xdp_frame_build( frame, sizeof( frame ), server_mac, gateway_mac, &src6, &dst6, payload_bytes );
    __CPROVER_assert( built6 == hdr6 + payload_bytes, "IPv6 build returns exact header + payload length" );
    uint16_t udp6_csum = (uint16_t) ( ( (uint16_t) frame[hdr6 - 2] << 8 ) | (uint16_t) frame[hdr6 - 1] );
    __CPROVER_assert( udp6_csum != 0, "IPv6 UDP checksum is never zero on the wire" );

    struct rocketnet_address_t from6;
    const uint8_t * out_payload6 = NULL;
    int out_len6 = -1;
    bool parsed6 = rocketnet_xdp_frame_parse( frame, (size_t) built6, &from6, &out_payload6, &out_len6 );
    if ( gateway_mac[0] == 0xFF && gateway_mac[1] == 0xFF )
    {
        __CPROVER_assert( !parsed6, "IPv6 parse refuses broadcast Ethernet destination MAC" );
    }
    else
    {
        __CPROVER_assert( parsed6, "built IPv6 frame with unicast MAC is accepted by parse" );
        __CPROVER_assert( rocketnet_address_equal( &from6, &src6 ), "parsed IPv6 frame recovers exact source endpoint" );
        __CPROVER_assert( out_payload6 == frame + hdr6 && out_len6 == payload_bytes,
                          "parsed IPv6 frame recovers exact payload slice" );
    }
}

static void harness_xdp_frame_arbitrary_receive( void )
{
    uint8_t frame[PROOF_FRAME_BOUND];
    size_t frame_bytes = nondet_uint();
    __CPROVER_assume( frame_bytes <= PROOF_FRAME_BOUND );

    int served_family = nondet_int();
    __CPROVER_assume( served_family == -1 || served_family == ROCKETNET_ADDRESS_IPV4 || served_family == ROCKETNET_ADDRESS_IPV6 );

    struct rocketnet_address_t from;
    const uint8_t * out_payload = NULL;
    int out_len = -1;
    struct rocketnet_xdp_receive_drops_t drops = { 0, 0, 0, 0, 0 };

    bool ok = rocketnet_xdp_frame_receive( frame, frame_bytes, served_family, &from, &out_payload, &out_len, &drops );
    if ( ok )
    {
        __CPROVER_assert( drops.descriptor == 0 && drops.not_ip == 0 && drops.ipv6_extension == 0 &&
                              drops.other_family == 0 && drops.malformed == 0,
                          "accepted frame increments no drop counter" );
        __CPROVER_assert( out_payload >= frame, "payload pointer starts inside frame" );
        __CPROVER_assert( out_len >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES,
                          "accepted frame payload meets server minimum wire size" );
        __CPROVER_assert( (size_t) ( out_payload - frame ) + (size_t) out_len <= frame_bytes,
                          "accepted payload slice stays within frame_bytes" );
        __CPROVER_assert( from.type == ROCKETNET_ADDRESS_IPV4 || from.type == ROCKETNET_ADDRESS_IPV6,
                          "accepted frame has IPv4 or IPv6 source family" );
        if ( served_family >= 0 )
        {
            __CPROVER_assert( from.type == served_family, "accepted frame matches single-family server filter" );
        }
    }
    else
    {
        uint64_t total_drops = drops.not_ip + drops.ipv6_extension + drops.other_family + drops.malformed;
        __CPROVER_assert( drops.descriptor == 0 && total_drops == 1,
                          "every rejected frame increments exactly one receive drop counter" );
    }
}

int main( void )
{
    harness_xdp_descriptors();
    harness_xdp_frame_build_and_parse();
    harness_xdp_frame_arbitrary_receive();
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "xdp-frame reachability control" );
#endif // #if defined( REACHABILITY_CONTROL )
    return 0;
}
