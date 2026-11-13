/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The io_uring transport's multishot recvmsg buffer: rocketnet_uring_recvmsg_parse
// (sdk/rocketnet_uring_codec.h), which reads the header, name area, control
// area and payload the kernel writes into a provided buffer, then the two
// steps the transport's receive completion takes on its result: the source
// conversion (rocketnet_uring_sockaddr_to_address) and the GRO split
// (rocketnet_coalesce_split). The codec's body exists only on Linux with
// IORING_RECV_MULTISHOT uapi, exactly where test_unit_uring_codec.cpp runs
// its cases; elsewhere this target compiles, its seed generator writes the
// seeds, and an input does nothing.
//
// INPUT: [mode][buffer...]
//   mode bit 0: UDP_GRO is on, so the msghdr reserves the GRO control area
//               (ROCKETNET_URING_GRO_CONTROL_BYTES); else no control area
//   mode bit 1: the socket is IPv6 (dual stack), as the source conversion
//               is told
//   buffer:     the bytes the completion's res names, at most
//               ROCKETNET_URING_RECV_BUFFER_BYTES, exactly as long as the
//               input so a read past res trips AddressSanitizer
// The name area is a sockaddr_storage, as the transport reserves it.
//
// THE ORACLES, each from the codec's contract:
//   - a buffer too short for the header and both reserved areas is refused
//   - on OK the name view is the name area at the header's end with exactly
//     the header's namelen bytes, at most the area; the payload view starts
//     after both areas with exactly the header's payloadlen bytes and ends
//     inside the buffer; the stride is not negative; at most one GRO fault
//     is counted, and a fault leaves the stride 0; no control area reads no
//     stride and counts no fault
//   - every refusal writes no view, sets the stride 0 and counts no fault
//   - the source conversion: a full IPv4 name always converts; an IPv6 name
//     shorter than sockaddr_in6 never does; a converted address has a family
//   - the split tiles the payload exactly at the stride, or refuses past
//     ROCKETNET_URING_MAX_SEGMENTS segments with nothing written
//
// THE SEED GENERATOR: ROCKETNET_FUZZ_SEED_DIR=<dir> writes the buffers
// test_unit_uring_codec.cpp builds (every verdict, every GRO fault, the
// stride, a v4-mapped source) laid out as the 64-bit Linux kernel writes them,
// in native byte order. Where the codec is compiled in, each is replayed and
// must reach its named verdict, stride and fault count, else the generator
// exits 1 printing INCOMPLETE.

#include "rocketnet_internal.h"
#include "rocketnet_uring_codec.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_coalesce.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portable_crt.h"

#if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_FUZZ_URING_ACTIVE 1
#else
#define ROCKETNET_FUZZ_URING_ACTIVE 0
#endif // #if defined( __linux__ ) && defined( IORING_RECV_MULTISHOT )

// the 64-bit Linux layout the seeds are written in; where the codec is
// compiled in, each value is held to the uapi header's

#define FUZZ_RECVMSG_OUT_BYTES      16      // struct io_uring_recvmsg_out
#define FUZZ_NAME_AREA_BYTES        128     // struct sockaddr_storage
#define FUZZ_CMSG_HEADER_BYTES      16      // struct cmsghdr: CMSG_LEN( 0 )
#define FUZZ_GRO_CONTROL_BYTES      24      // CMSG_SPACE( sizeof( int ) )
#define FUZZ_SOCKADDR_IN_BYTES      16
#define FUZZ_SOCKADDR_IN6_BYTES     28
#define FUZZ_AF_INET                2
#define FUZZ_AF_INET6               10
#define FUZZ_MSG_CTRUNC             0x08
#define FUZZ_MSG_TRUNC              0x20
#define FUZZ_IPPROTO_IP             0
#define FUZZ_IPPROTO_UDP            17
#define FUZZ_UDP_GRO                104

#if ROCKETNET_FUZZ_URING_ACTIVE
static_assert( sizeof( struct io_uring_recvmsg_out ) == FUZZ_RECVMSG_OUT_BYTES, "the seeds' header size is the kernel's" );
static_assert( sizeof( struct sockaddr_storage ) == FUZZ_NAME_AREA_BYTES, "the seeds' name area is the transport's" );
static_assert( CMSG_LEN( 0 ) == FUZZ_CMSG_HEADER_BYTES, "the seeds' cmsg header size is the kernel's" );
static_assert( ROCKETNET_URING_GRO_CONTROL_BYTES == FUZZ_GRO_CONTROL_BYTES, "the seeds' control area is the transport's" );
static_assert( sizeof( struct sockaddr_in ) == FUZZ_SOCKADDR_IN_BYTES && sizeof( struct sockaddr_in6 ) == FUZZ_SOCKADDR_IN6_BYTES, "the seeds' sockaddr sizes are the kernel's" );
static_assert( AF_INET == FUZZ_AF_INET && AF_INET6 == FUZZ_AF_INET6, "the seeds' families are the kernel's" );
static_assert( MSG_CTRUNC == FUZZ_MSG_CTRUNC && MSG_TRUNC == FUZZ_MSG_TRUNC, "the seeds' flags are the kernel's" );
static_assert( IPPROTO_IP == FUZZ_IPPROTO_IP && IPPROTO_UDP == FUZZ_IPPROTO_UDP && UDP_GRO == FUZZ_UDP_GRO, "the seeds' cmsg level and type are the kernel's" );

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
    {
        return;
    }
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE

// what one buffer did: the verdict, the stride and the GRO faults counted

struct fuzz_uring_result_t
{
    int verdict;                    // -1: the input names no buffer, or the codec is compiled out
    int segment_bytes;
    uint64_t faults;
};

#if ROCKETNET_FUZZ_URING_ACTIVE
static uint32_t fuzz_get_u32( const uint8_t * p )
{
    uint32_t value;
    memcpy( &value, p, sizeof( value ) );
    return value;
}
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE

static fuzz_uring_result_t fuzz_uring_run( const uint8_t * data, size_t size )
{
    fuzz_uring_result_t result = { -1, 0, 0 };
    if ( size < 1 || size - 1 > (size_t) ROCKETNET_URING_RECV_BUFFER_BYTES )
    {
        return result;
    }

#if ROCKETNET_FUZZ_URING_ACTIVE
    const bool gro = ( data[0] & 1 ) != 0;
    const bool socket_ipv6 = ( data[0] & 2 ) != 0;
    const uint8_t * buffer = data + 1;
    const int buffer_bytes = (int) ( size - 1 );
    const int msg_namelen = FUZZ_NAME_AREA_BYTES;
    const int msg_controllen = gro ? (int) ROCKETNET_URING_GRO_CONTROL_BYTES : 0;

    static const uint8_t sentinel = 0;
    const uint8_t * name = &sentinel;
    const uint8_t * payload = &sentinel;
    int name_bytes = -7;
    int payload_bytes = -7;
    int segment_bytes = -7;
    uint64_t faults = 1000;

    const int verdict = rocketnet_uring_recvmsg_parse( buffer, buffer_bytes, msg_namelen, msg_controllen,
                                                       &name, &name_bytes, &payload, &payload_bytes, &segment_bytes, &faults );

    rocketnet_fuzz_require( verdict == ROCKETNET_URING_RECV_OK || verdict == ROCKETNET_URING_RECV_SHORT_BUFFER ||
                                verdict == ROCKETNET_URING_RECV_NAME_TRUNCATED || verdict == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED,
                            "the verdict is one the codec names" );

    const bool fits = buffer_bytes >= FUZZ_RECVMSG_OUT_BYTES + msg_namelen + msg_controllen;
    rocketnet_fuzz_require( fits || verdict == ROCKETNET_URING_RECV_SHORT_BUFFER, "a buffer short of the header and both areas is refused" );

    result.verdict = verdict;

    if ( verdict != ROCKETNET_URING_RECV_OK )
    {
        rocketnet_fuzz_require( name == &sentinel && payload == &sentinel && name_bytes == -7 && payload_bytes == -7,
                                "a refused buffer writes no view" );
        rocketnet_fuzz_require( segment_bytes == 0, "a refused buffer reads no stride" );
        rocketnet_fuzz_require( faults == 1000, "a refused buffer counts no GRO fault" );
        return result;
    }

    rocketnet_fuzz_require( fits, "an accepted buffer holds the header and both areas" );
    rocketnet_fuzz_require( name == buffer + FUZZ_RECVMSG_OUT_BYTES, "the name view is the name area" );
    rocketnet_fuzz_require( name_bytes >= 0 && (uint32_t) name_bytes == fuzz_get_u32( buffer + 0 ) && name_bytes <= msg_namelen,
                            "the name view has the header's namelen, inside the name area" );
    rocketnet_fuzz_require( payload == buffer + FUZZ_RECVMSG_OUT_BYTES + msg_namelen + msg_controllen, "the payload view follows both areas" );
    rocketnet_fuzz_require( payload_bytes >= 0 && (uint32_t) payload_bytes == fuzz_get_u32( buffer + 8 ), "the payload view has the header's payloadlen" );
    rocketnet_fuzz_require( payload_bytes <= buffer_bytes - ( FUZZ_RECVMSG_OUT_BYTES + msg_namelen + msg_controllen ), "the payload view ends inside the buffer" );
    rocketnet_fuzz_require( ( fuzz_get_u32( buffer + 12 ) & FUZZ_MSG_TRUNC ) == 0, "a truncated payload is never accepted" );
    rocketnet_fuzz_require( segment_bytes >= 0, "the stride is not negative" );
    rocketnet_fuzz_require( faults == 1000 || faults == 1001, "one buffer counts at most one GRO fault" );
    rocketnet_fuzz_require( faults == 1000 || segment_bytes == 0, "a GRO fault leaves the stride 0" );
    rocketnet_fuzz_require( msg_controllen > 0 || ( segment_bytes == 0 && faults == 1000 ), "no control area reads no stride and counts no fault" );

    result.segment_bytes = segment_bytes;
    result.faults = faults - 1000;

    // the source, as the receive completion converts it

    struct sockaddr_storage source;
    memset( &source, 0, sizeof( source ) );
    memcpy( &source, name, (size_t) name_bytes );

    rocketnet_address_t from;
    memset( &from, 0, sizeof( from ) );
    const bool source_ok = rocketnet_uring_sockaddr_to_address( &source, name_bytes, socket_ipv6, &from );
    if ( source.ss_family == AF_INET && name_bytes >= FUZZ_SOCKADDR_IN_BYTES )
    {
        // a source port of 0 is refused (rocketnet_address_accept_received_source)
        const bool port_zero = ( (const struct sockaddr_in *) &source )->sin_port == 0;
        rocketnet_fuzz_require( source_ok == !port_zero && from.type == ROCKETNET_ADDRESS_IPV4, "a full IPv4 name converts to an IPv4 address unless its port is 0" );
    }

    if ( source.ss_family == AF_INET6 && name_bytes < FUZZ_SOCKADDR_IN6_BYTES )
    {
        rocketnet_fuzz_require( !source_ok, "a short IPv6 name never converts" );
    }

    if ( source_ok )
    {
        rocketnet_fuzz_require( from.type == ROCKETNET_ADDRESS_IPV4 || from.type == ROCKETNET_ADDRESS_IPV6, "a converted source has a family" );
    }

    // the split at the stride, into the transport's segment arrays

    int offsets[ROCKETNET_URING_MAX_SEGMENTS + 1];
    int lengths[ROCKETNET_URING_MAX_SEGMENTS + 1];
    for ( int i = 0; i <= ROCKETNET_URING_MAX_SEGMENTS; ++i )
    {
        offsets[i] = -1;
        lengths[i] = -1;
    }

    const int segments = rocketnet_coalesce_split( payload_bytes, segment_bytes, offsets, lengths, ROCKETNET_URING_MAX_SEGMENTS );
    rocketnet_fuzz_require( offsets[ROCKETNET_URING_MAX_SEGMENTS] == -1 && lengths[ROCKETNET_URING_MAX_SEGMENTS] == -1,
                            "the split writes no segment past its bound" );
    if ( payload_bytes == 0 )
    {
        rocketnet_fuzz_require( segments == 0, "an empty payload splits into nothing" );
    }
    else if ( segments < 0 )
    {
        rocketnet_fuzz_require( segments == -1 && segment_bytes > 0 &&
                                    ( (int64_t) payload_bytes + segment_bytes - 1 ) / segment_bytes > ROCKETNET_URING_MAX_SEGMENTS,
                                "the split refuses only past its segment bound" );
        rocketnet_fuzz_require( offsets[0] == -1 && lengths[0] == -1, "a refused split writes nothing" );
    }
    else
    {
        const int stride = segment_bytes > 0 ? segment_bytes : payload_bytes;
        int next = 0;
        for ( int i = 0; i < segments; ++i )
        {
            rocketnet_fuzz_require( offsets[i] == next && lengths[i] > 0 && lengths[i] <= stride, "the segments are contiguous, each at most the stride" );
            rocketnet_fuzz_require( i == segments - 1 || lengths[i] == stride, "only the last segment is shorter" );
            next += lengths[i];
        }

        rocketnet_fuzz_require( segments >= 1 && next == payload_bytes, "the segments tile the payload" );
    }
#else // #if ROCKETNET_FUZZ_URING_ACTIVE
    (void) data;
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE

    return result;
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
// -----------------------------------------------------------------------

#define FUZZ_URING_SEED_MAX 1024

// the verdicts, ROCKETNET_URING_RECV_* where the codec is compiled

enum
{
    FUZZ_RECV_OK = 0,
    FUZZ_RECV_SHORT_BUFFER = 1,
    FUZZ_RECV_NAME_TRUNCATED = 2,
    FUZZ_RECV_PAYLOAD_TRUNCATED = 3,
};

#if ROCKETNET_FUZZ_URING_ACTIVE
static_assert( ROCKETNET_URING_RECV_OK == FUZZ_RECV_OK && ROCKETNET_URING_RECV_SHORT_BUFFER == FUZZ_RECV_SHORT_BUFFER &&
                   ROCKETNET_URING_RECV_NAME_TRUNCATED == FUZZ_RECV_NAME_TRUNCATED && ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED == FUZZ_RECV_PAYLOAD_TRUNCATED,
               "the seeds' verdicts are the codec's" );
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE

enum
{
    FUZZ_NAME_IPV4 = 0,
    FUZZ_NAME_IPV6,
    FUZZ_NAME_V4_MAPPED,
    FUZZ_NAME_NONE,
};

struct fuzz_uring_seed_t
{
    const char * name;
    uint8_t mode;
    int source;
    uint32_t namelen;               // the header's namelen; 0 with FUZZ_NAME_NONE
    uint32_t controllen;            // the control bytes the kernel wrote
    uint32_t payloadlen;            // the header's payloadlen
    uint32_t flags;
    int payload_bytes;              // the payload bytes present in the buffer
    int cmsg_level;
    int cmsg_type;
    uint64_t cmsg_len;              // 0: no cmsg
    int stride;
    int short_by;                   // bytes cut from the end of the reserved areas
    int verdict;
    int segment_bytes;
    uint64_t faults;
};

static const fuzz_uring_seed_t fuzz_uring_seeds[] = {
    { "ok_ipv4_name", 0, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 64, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_OK, 0, 0 },
    { "ok_ipv6_name", 2, FUZZ_NAME_IPV6, FUZZ_SOCKADDR_IN6_BYTES, 0, 64, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_OK, 0, 0 },
    { "ok_v4_mapped_name", 2, FUZZ_NAME_V4_MAPPED, FUZZ_SOCKADDR_IN6_BYTES, 0, 64, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_OK, 0, 0 },
    { "ok_zero_payload", 0, FUZZ_NAME_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, FUZZ_RECV_OK, 0, 0 },
    { "short_buffer", 0, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 0, 0, 0, 0, 0, 0, 0, 1, FUZZ_RECV_SHORT_BUFFER, 0, 0 },
    { "short_buffer_gro", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 0, 0, 0, 0, 0, 0, 0, 1, FUZZ_RECV_SHORT_BUFFER, 0, 0 },
    { "name_truncated", 0, FUZZ_NAME_IPV4, FUZZ_NAME_AREA_BYTES + 1, 0, 64, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_NAME_TRUNCATED, 0, 0 },
    { "payload_truncated_msg_trunc", 0, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 64, FUZZ_MSG_TRUNC, 64, 0, 0, 0, 0, 0, FUZZ_RECV_PAYLOAD_TRUNCATED, 0, 0 },
    { "payload_truncated_past_end", 0, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 65, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_PAYLOAD_TRUNCATED, 0, 0 },
    { "payload_truncated_top_bit", 0, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 0x80000000u, 0, 64, 0, 0, 0, 0, 0, FUZZ_RECV_PAYLOAD_TRUNCATED, 0, 0 },
    { "gro_stride", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, 0, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES + 4, 100, 0, FUZZ_RECV_OK, 100, 0 },
    { "gro_stride_past_segment_bound", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, 0, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES + 4, 1, 0, FUZZ_RECV_OK, 1, 0 },
    { "gro_no_cmsg", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, 0, 250, 0, 250, 0, 0, 0, 0, 0, FUZZ_RECV_OK, 0, 0 },
    { "gro_other_cmsg", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, 0, 250, FUZZ_IPPROTO_IP, 1, FUZZ_CMSG_HEADER_BYTES + 4, 100, 0, FUZZ_RECV_OK, 0, 0 },
    { "gro_fault_cmsg_short_for_int", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, 0, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES + 2, 100, 0, FUZZ_RECV_OK, 0, 1 },
    { "gro_fault_cmsg_under_header", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, 0, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES - 1, 100, 0, FUZZ_RECV_OK, 0, 1 },
    { "gro_fault_cmsg_past_control", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_CMSG_HEADER_BYTES, 250, 0, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES + 4, 100, 0, FUZZ_RECV_OK, 0, 1 },
    { "gro_fault_ctrunc", 1, FUZZ_NAME_IPV4, FUZZ_SOCKADDR_IN_BYTES, FUZZ_GRO_CONTROL_BYTES, 250, FUZZ_MSG_CTRUNC, 250, FUZZ_IPPROTO_UDP, FUZZ_UDP_GRO, FUZZ_CMSG_HEADER_BYTES + 4, 100, 0, FUZZ_RECV_OK, 0, 1 },
};

static void fuzz_put_u16_native( uint8_t * p, uint16_t value )
{
    memcpy( p, &value, sizeof( value ) );
}

static void fuzz_put_u32_native( uint8_t * p, uint32_t value )
{
    memcpy( p, &value, sizeof( value ) );
}

static void fuzz_put_u64_native( uint8_t * p, uint64_t value )
{
    memcpy( p, &value, sizeof( value ) );
}

static int fuzz_uring_seed_build( const fuzz_uring_seed_t * seed, uint8_t * out )
{
    const int control_area = ( seed->mode & 1 ) ? FUZZ_GRO_CONTROL_BYTES : 0;
    const int bytes = FUZZ_RECVMSG_OUT_BYTES + FUZZ_NAME_AREA_BYTES + control_area + seed->payload_bytes - seed->short_by;
    if ( bytes < 0 || 1 + bytes > FUZZ_URING_SEED_MAX )
    {
        return 0;
    }

    memset( out, 0, FUZZ_URING_SEED_MAX );
    out[0] = seed->mode;
    uint8_t * b = out + 1;

    fuzz_put_u32_native( b + 0, seed->namelen );
    fuzz_put_u32_native( b + 4, seed->controllen );
    fuzz_put_u32_native( b + 8, seed->payloadlen );
    fuzz_put_u32_native( b + 12, seed->flags );

    // the name: sa_family native, the port and address in network order

    uint8_t * name = b + FUZZ_RECVMSG_OUT_BYTES;
    switch ( seed->source )
    {
        case FUZZ_NAME_IPV4:
            fuzz_put_u16_native( name, FUZZ_AF_INET );
            name[2] = 0x9C;                         // port 40000
            name[3] = 0x40;
            name[4] = 127;
            name[7] = 1;
            break;
        case FUZZ_NAME_IPV6:
            fuzz_put_u16_native( name, FUZZ_AF_INET6 );
            name[2] = 0x9C;
            name[3] = 0x40;
            name[8] = 0x20;                         // 2001:db8::1
            name[9] = 0x01;
            name[10] = 0x0D;
            name[11] = 0xB8;
            name[23] = 0x01;
            break;
        case FUZZ_NAME_V4_MAPPED:
            fuzz_put_u16_native( name, FUZZ_AF_INET6 );
            name[2] = 0x9C;
            name[3] = 0x40;
            name[18] = 0xFF;                        // ::ffff:127.0.0.1
            name[19] = 0xFF;
            name[20] = 127;
            name[23] = 1;
            break;
        default:
            break;
    }

    if ( seed->cmsg_len > 0 && control_area > 0 )
    {
        uint8_t * cmsg = b + FUZZ_RECVMSG_OUT_BYTES + FUZZ_NAME_AREA_BYTES;
        fuzz_put_u64_native( cmsg + 0, seed->cmsg_len );
        fuzz_put_u32_native( cmsg + 8, (uint32_t) seed->cmsg_level );
        fuzz_put_u32_native( cmsg + 12, (uint32_t) seed->cmsg_type );
        fuzz_put_u32_native( cmsg + FUZZ_CMSG_HEADER_BYTES, (uint32_t) seed->stride );
    }

    for ( int i = 0; i < seed->payload_bytes - seed->short_by; ++i )
    {
        b[FUZZ_RECVMSG_OUT_BYTES + FUZZ_NAME_AREA_BYTES + control_area + i] = (uint8_t) i;
    }

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
    static uint8_t seed[FUZZ_URING_SEED_MAX];
    bool complete = true;

    for ( size_t i = 0; i < sizeof( fuzz_uring_seeds ) / sizeof( fuzz_uring_seeds[0] ); ++i )
    {
        const fuzz_uring_seed_t * entry = &fuzz_uring_seeds[i];
        const int bytes = fuzz_uring_seed_build( entry, seed );
        if ( bytes <= 0 )
        {
            fprintf( stderr, "seed generator: no seed for %s\n", entry->name );
            complete = false;
            continue;
        }

#if ROCKETNET_FUZZ_URING_ACTIVE
        const fuzz_uring_result_t result = fuzz_uring_run( seed, (size_t) bytes );
        if ( result.verdict != entry->verdict || result.segment_bytes != entry->segment_bytes || result.faults != entry->faults )
        {
            fprintf( stderr, "seed generator: the %s seed does not reach its verdict\n", entry->name );
            complete = false;
            continue;
        }
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE

        if ( !fuzz_seed_write( dir, entry->name, seed, bytes ) )
        {
            complete = false;
        }
    }

#if ROCKETNET_FUZZ_URING_ACTIVE
    printf( "seed generator: %s\n", complete ? "every seed reaches the verdict it is named for" : "INCOMPLETE" );
#else // #if ROCKETNET_FUZZ_URING_ACTIVE
    printf( "seed generator: %s\n", complete ? "seeds written; the codec is compiled out here, so none was replayed" : "INCOMPLETE" );
#endif // #if ROCKETNET_FUZZ_URING_ACTIVE
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
    (void) fuzz_uring_run( data, size );
    return 0;
}
