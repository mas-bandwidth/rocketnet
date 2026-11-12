/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP frame reader: rocketnet_xdp_frame_parse and
// rocketnet_xdp_frame_receive (sdk/rocketnet_xdp_frame.h), the first
// userspace code that reads a raw Ethernet frame from the NIC on the XDP
// transport. The header is portable C++, so this target builds and runs
// wherever the unit suite builds test_unit_xdp_frame.cpp: every platform.
//
// INPUT: [served][frame...]
//   served % 3: the family the server serves, as the receive loop passes it:
//               0 IPv4, 1 IPv6, 2 either (address_type -1)
//   frame:      the received frame, at most ROCKETNET_XDP_FRAME_SIZE bytes
//               (rocketnet_xdp_receive_descriptor refuses a longer one before
//               the frame is read), and exactly as long as the input, so a
//               read past its end trips AddressSanitizer
//
// THE ORACLES, each from the contract in sdk/rocketnet_xdp_frame.h:
//   - parse accepts: the payload starts at the family's header offset (42 or
//     62), lies inside the frame, and its length is the UDP length less the
//     UDP header; the from-address carries the family, address and port the
//     frame's headers hold; the destination MAC is not broadcast
//   - parse refuses: the payload pointer and length are unchanged
//   - receive takes a frame exactly when parse accepts it, the payload holds
//     the smallest server-inbound packet and the family is served; it then
//     hands back parse's payload, length and address, and counts nothing
//   - receive drops: exactly one drop counter is raised, never descriptor,
//     and every output is unchanged; not_ip exactly when the frame is too
//     short to name an ethertype or names neither IP family
//
// THE SEED GENERATOR: ROCKETNET_FUZZ_SEED_DIR=<dir> writes one named seed per
// receive outcome and per refusal the unit tests name, each built with the
// real rocketnet_xdp_frame_build, replays each through the target, and exits
// 1 printing INCOMPLETE unless every seed reaches the outcome it is named
// for.

#include "rocketnet_internal.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portable_crt.h"

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
    {
        return;
    }
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// what receive did with one frame

enum
{
    FUZZ_XDP_TAKEN = 0,
    FUZZ_XDP_NOT_IP,
    FUZZ_XDP_IPV6_EXTENSION,
    FUZZ_XDP_OTHER_FAMILY,
    FUZZ_XDP_MALFORMED,
    FUZZ_XDP_IGNORED,               // the input names no frame the receive loop can see
};

static const int fuzz_xdp_served[3] = { ROCKETNET_ADDRESS_IPV4, ROCKETNET_ADDRESS_IPV6, -1 };

static int fuzz_xdp_run( const uint8_t * data, size_t size )
{
    if ( size < 1 || size - 1 > (size_t) ROCKETNET_XDP_FRAME_SIZE )
    {
        return FUZZ_XDP_IGNORED;
    }

    const int address_type = fuzz_xdp_served[data[0] % 3];
    const uint8_t * f = data + 1;
    const size_t frame_bytes = size - 1;

    // parse

    static const uint8_t sentinel = 0;
    rocketnet_address_t from;
    memset( &from, 0xA5, sizeof( from ) );
    const uint8_t * payload = &sentinel;
    int payload_bytes = -7;

    const bool parsed = rocketnet_xdp_frame_parse( f, frame_bytes, &from, &payload, &payload_bytes );
    if ( parsed )
    {
        const bool ipv4 = f[12] == 0x08 && f[13] == 0x00;
        const bool ipv6 = f[12] == 0x86 && f[13] == 0xDD;
        rocketnet_fuzz_require( ipv4 || ipv6, "parse accepts only an IPv4 or IPv6 ethertype" );
        rocketnet_fuzz_require( !( f[0] == 0xFF && f[1] == 0xFF ), "parse refuses a broadcast destination" );

        const size_t header_bytes = (size_t) rocketnet_xdp_frame_header_bytes( ipv4 ? ROCKETNET_ADDRESS_IPV4 : ROCKETNET_ADDRESS_IPV6 );
        rocketnet_fuzz_require( frame_bytes >= header_bytes, "an accepted frame holds its whole header set" );
        rocketnet_fuzz_require( payload == f + header_bytes, "the payload starts at the family's header offset" );
        rocketnet_fuzz_require( payload_bytes >= 0 && (size_t) payload_bytes <= frame_bytes - header_bytes, "the payload lies inside the frame" );

        const int udp_length = ( f[header_bytes - 4] << 8 ) | f[header_bytes - 3];
        rocketnet_fuzz_require( payload_bytes == udp_length - ROCKETNET_UDP_HEADER_BYTES, "the payload length is the UDP length less its header" );
        rocketnet_fuzz_require( from.port == (uint16_t) ( ( f[header_bytes - 8] << 8 ) | f[header_bytes - 7] ), "the from port is the UDP source port" );

        if ( ipv4 )
        {
            rocketnet_fuzz_require( from.type == ROCKETNET_ADDRESS_IPV4, "an IPv4 frame reads an IPv4 address" );
            rocketnet_fuzz_require( memcmp( from.data.ipv4, f + 26, 4 ) == 0, "the from address is the IPv4 source" );
        }
        else
        {
            rocketnet_fuzz_require( from.type == ROCKETNET_ADDRESS_IPV6, "an IPv6 frame reads an IPv6 address" );
            for ( int i = 0; i < 8; ++i )
            {
                rocketnet_fuzz_require( from.data.ipv6[i] == (uint16_t) ( ( f[22 + 2 * i] << 8 ) | f[23 + 2 * i] ), "the from address is the IPv6 source" );
            }
        }
    }
    else
    {
        rocketnet_fuzz_require( payload == &sentinel && payload_bytes == -7, "a refused parse writes no payload" );
    }

    // receive, the transport's per-frame dispatch

    rocketnet_address_t taken_from;
    memset( &taken_from, 0xA5, sizeof( taken_from ) );
    rocketnet_address_t untouched;
    memcpy( &untouched, &taken_from, sizeof( untouched ) );
    const uint8_t * taken_payload = &sentinel;
    int taken_bytes = -7;
    struct rocketnet_xdp_receive_drops_t drops;
    memset( &drops, 0, sizeof( drops ) );

    const bool taken = rocketnet_xdp_frame_receive( f, frame_bytes, address_type, &taken_from, &taken_payload, &taken_bytes, &drops );

    const bool served = parsed && ( address_type < 0 || from.type == address_type );
    const bool due = served && payload_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES;
    rocketnet_fuzz_require( taken == due, "receive takes exactly the parsed, served frames that hold the smallest server packet" );

    if ( taken )
    {
        rocketnet_fuzz_require( rocketnet_xdp_receive_drops_total( &drops ) == 0, "a taken frame counts no drop" );
        rocketnet_fuzz_require( taken_payload == payload && taken_bytes == payload_bytes, "receive hands back parse's payload" );
        rocketnet_fuzz_require( taken_from.type == from.type && taken_from.port == from.port &&
                                    memcmp( &taken_from.data, &from.data, sizeof( from.data ) ) == 0,
                                "receive hands back parse's address" );
        return FUZZ_XDP_TAKEN;
    }

    rocketnet_fuzz_require( rocketnet_xdp_receive_drops_total( &drops ) == 1, "a dropped frame raises exactly one drop counter" );
    rocketnet_fuzz_require( drops.descriptor == 0, "the frame dispatch never counts a descriptor drop" );
    rocketnet_fuzz_require( taken_payload == &sentinel && taken_bytes == -7 && memcmp( &taken_from, &untouched, sizeof( untouched ) ) == 0,
                            "a dropped frame writes no output" );

    const bool names_ip = frame_bytes >= (size_t) ROCKETNET_ETHERNET_HEADER_BYTES &&
                          ( ( f[12] == 0x08 && f[13] == 0x00 ) || ( f[12] == 0x86 && f[13] == 0xDD ) );
    rocketnet_fuzz_require( ( drops.not_ip == 1 ) == !names_ip, "not_ip counts exactly the frames that name no IP ethertype" );

    if ( drops.not_ip )
    {
        return FUZZ_XDP_NOT_IP;
    }
    if ( drops.ipv6_extension )
    {
        return FUZZ_XDP_IPV6_EXTENSION;
    }
    if ( drops.other_family )
    {
        return FUZZ_XDP_OTHER_FAMILY;
    }
    return FUZZ_XDP_MALFORMED;
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
//
// The frames test_unit_xdp_frame.cpp builds: one taken frame per family and
// served setting, the smallest IPv6 server packet, a flow label, link-layer
// padding, and one seed for each refusal those cases name. Every frame
// comes from rocketnet_xdp_frame_build, then one field is changed where the
// seed names a refusal.
// -----------------------------------------------------------------------

#define FUZZ_XDP_SEED_MAX ( 1 + 512 )

static const uint8_t fuzz_server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t fuzz_gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

enum
{
    FUZZ_SEED_PLAIN = 0,
    FUZZ_SEED_FLOW_LABEL,
    FUZZ_SEED_PADDED,
    FUZZ_SEED_ARP,
    FUZZ_SEED_SHORT,
    FUZZ_SEED_HOP_BY_HOP,
    FUZZ_SEED_FRAGMENT,
    FUZZ_SEED_IPV4_OPTIONS,
    FUZZ_SEED_NOT_UDP,
    FUZZ_SEED_TRUNCATED,
    FUZZ_SEED_UDP_PAST_END,
    FUZZ_SEED_UDP_UNDER_HEADER,
    FUZZ_SEED_BROADCAST,
};

struct fuzz_xdp_seed_t
{
    const char * name;
    int served;                     // input[0]
    int family;                     // the frame's family
    int payload_bytes;
    int change;
    int outcome;
};

static const fuzz_xdp_seed_t fuzz_xdp_seeds[] = {
    { "taken_ipv4", 0, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_TAKEN },
    { "taken_ipv6", 1, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_TAKEN },
    { "taken_either_ipv4", 2, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_TAKEN },
    { "taken_either_ipv6", 2, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_TAKEN },
    { "taken_ipv6_smallest", 1, ROCKETNET_ADDRESS_IPV6, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, FUZZ_SEED_PLAIN, FUZZ_XDP_TAKEN },
    { "taken_ipv6_flow_label", 1, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_FLOW_LABEL, FUZZ_XDP_TAKEN },
    { "taken_ipv4_padded", 0, ROCKETNET_ADDRESS_IPV4, 40, FUZZ_SEED_PADDED, FUZZ_XDP_TAKEN },
    { "not_ip_arp", 2, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_ARP, FUZZ_XDP_NOT_IP },
    { "not_ip_short", 2, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_SHORT, FUZZ_XDP_NOT_IP },
    { "ipv6_extension_hop_by_hop", 1, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_HOP_BY_HOP, FUZZ_XDP_IPV6_EXTENSION },
    { "ipv6_extension_fragment", 2, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_FRAGMENT, FUZZ_XDP_IPV6_EXTENSION },
    { "other_family_ipv6_on_ipv4", 0, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_OTHER_FAMILY },
    { "other_family_ipv4_on_ipv6", 1, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_PLAIN, FUZZ_XDP_OTHER_FAMILY },
    { "malformed_ipv4_options", 0, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_IPV4_OPTIONS, FUZZ_XDP_MALFORMED },
    { "malformed_ipv4_not_udp", 0, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_NOT_UDP, FUZZ_XDP_MALFORMED },
    { "malformed_ipv6_truncated", 1, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_TRUNCATED, FUZZ_XDP_MALFORMED },
    { "malformed_udp_length_past_end", 0, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_UDP_PAST_END, FUZZ_XDP_MALFORMED },
    { "malformed_udp_length_under_header", 1, ROCKETNET_ADDRESS_IPV6, 64, FUZZ_SEED_UDP_UNDER_HEADER, FUZZ_XDP_MALFORMED },
    { "malformed_below_smallest_ipv4", 0, ROCKETNET_ADDRESS_IPV4, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, FUZZ_SEED_PLAIN, FUZZ_XDP_MALFORMED },
    { "malformed_broadcast", 0, ROCKETNET_ADDRESS_IPV4, 64, FUZZ_SEED_BROADCAST, FUZZ_XDP_MALFORMED },
};

static int fuzz_xdp_seed_build( const fuzz_xdp_seed_t * seed, uint8_t * out )
{
    // a client at 192.168.1.10:40000 or [2001:db8::1]:40000 sending to the
    // server at 192.168.1.20:40001 or [2001:db8::2]:40001

    rocketnet_address_t source;
    rocketnet_address_t destination;
    memset( &source, 0, sizeof( source ) );
    memset( &destination, 0, sizeof( destination ) );
    source.type = (uint8_t) seed->family;
    destination.type = (uint8_t) seed->family;
    source.port = 40000;
    destination.port = 40001;
    if ( seed->family == ROCKETNET_ADDRESS_IPV4 )
    {
        const uint8_t s[4] = { 192, 168, 1, 10 };
        const uint8_t d[4] = { 192, 168, 1, 20 };
        memcpy( source.data.ipv4, s, 4 );
        memcpy( destination.data.ipv4, d, 4 );
    }
    else
    {
        source.data.ipv6[0] = 0x2001;
        source.data.ipv6[1] = 0x0db8;
        source.data.ipv6[7] = 1;
        destination.data.ipv6[0] = 0x2001;
        destination.data.ipv6[1] = 0x0db8;
        destination.data.ipv6[7] = 2;
    }

    uint8_t * f = out + 1;
    const int capacity = FUZZ_XDP_SEED_MAX - 1;
    memset( f, 0, (size_t) capacity );
    const int header_bytes = rocketnet_xdp_frame_header_bytes( seed->family );
    for ( int i = 0; i < seed->payload_bytes; ++i )
    {
        f[header_bytes + i] = (uint8_t) i;
    }

    int bytes = rocketnet_xdp_frame_build( f, (size_t) capacity, fuzz_server_mac, fuzz_gateway_mac, &source, &destination, seed->payload_bytes );
    if ( bytes <= 0 )
    {
        return 0;
    }

    const int udp_length = ROCKETNET_UDP_HEADER_BYTES + seed->payload_bytes;
    switch ( seed->change )
    {
        case FUZZ_SEED_FLOW_LABEL:
            f[14] = 0x6A;           // traffic class 0xA0 and a 20-bit flow label
            f[15] = 0x0B;
            f[16] = 0xCD;
            f[17] = 0xEF;
            break;
        case FUZZ_SEED_PADDED:
            bytes += 10;            // link-layer padding past the UDP length
            break;
        case FUZZ_SEED_ARP:
            f[12] = 0x08;
            f[13] = 0x06;
            break;
        case FUZZ_SEED_SHORT:
            bytes = ROCKETNET_ETHERNET_HEADER_BYTES - 1;
            break;
        case FUZZ_SEED_HOP_BY_HOP:
            f[20] = 0;
            break;
        case FUZZ_SEED_FRAGMENT:
            f[20] = 44;
            break;
        case FUZZ_SEED_IPV4_OPTIONS:
            f[14] = 0x46;
            break;
        case FUZZ_SEED_NOT_UDP:
            f[23] = 6;
            break;
        case FUZZ_SEED_TRUNCATED:
            bytes = header_bytes - 1;
            break;
        case FUZZ_SEED_UDP_PAST_END:
            f[header_bytes - 4] = (uint8_t) ( ( udp_length + 1 ) >> 8 );
            f[header_bytes - 3] = (uint8_t) ( ( udp_length + 1 ) & 0xFF );
            break;
        case FUZZ_SEED_UDP_UNDER_HEADER:
            f[header_bytes - 4] = 0;
            f[header_bytes - 3] = ROCKETNET_UDP_HEADER_BYTES - 1;
            break;
        case FUZZ_SEED_BROADCAST:
            memset( f, 0xFF, 6 );
            break;
        default:
            break;
    }

    out[0] = (uint8_t) seed->served;
    return 1 + bytes;
}

static bool fuzz_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
{
    // ROCKETNET_FUZZ_WITHHOLD=<seed name> skips that seed and reports it
    // unwritten, so the generator ends INCOMPLETE: tools/check_fuzz_seeds.sh
    // runs this control for every generator (VERIFY-5 W4)
    char withheld[256];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_WITHHOLD", withheld, sizeof( withheld ) ) && strcmp( withheld, name ) == 0 )
    {
        fprintf( stderr, "seed generator: withholding %s\n", name );
        return false;
    }

    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
    {
        fprintf( stderr, "seed generator: the seed path for %s does not fit\n", name );
        return false;
    }

    FILE * file = rocketnet_test_fopen( path, "wb" );
    if ( !file )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
        return false;
    }

    const bool ok = fwrite( data, 1, (size_t) bytes, file ) == (size_t) bytes;

    fclose( file );
    if ( !ok )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
    }

    return ok;
}

static bool fuzz_generate( const char * dir )
{
    static uint8_t seed[FUZZ_XDP_SEED_MAX];
    bool complete = true;

    for ( size_t i = 0; i < sizeof( fuzz_xdp_seeds ) / sizeof( fuzz_xdp_seeds[0] ); ++i )
    {
        const fuzz_xdp_seed_t * entry = &fuzz_xdp_seeds[i];
        const int bytes = fuzz_xdp_seed_build( entry, seed );
        if ( bytes <= 0 )
        {
            fprintf( stderr, "seed generator: no seed for %s\n", entry->name );
            complete = false;
            continue;
        }

        if ( fuzz_xdp_run( seed, (size_t) bytes ) != entry->outcome )
        {
            fprintf( stderr, "seed generator: the %s seed does not reach its outcome\n", entry->name );
            complete = false;
            continue;
        }

        if ( !fuzz_seed_write( dir, entry->name, seed, bytes ) )
        {
            complete = false;
        }
    }

    printf( "seed generator: %s\n", complete ? "every seed reaches the outcome it is named for" : "INCOMPLETE" );
    return complete;
}

// -----------------------------------------------------------------------

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        exit( fuzz_generate( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    (void) fuzz_xdp_run( data, size );
    return 0;
}
