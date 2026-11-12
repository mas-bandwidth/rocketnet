/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP filter program itself (sdk/xdp/server_xdp.c, the BPF program the
// kernel runs on every frame), fuzzed in user space through the host shim the
// unit suite already uses (tests/xdp_host/). The shim copies each frame into a
// heap allocation of exactly its size, so a read the program makes past
// data_end is an ASan report; the shim object is built with the same
// sanitizers. rocketnet_fuzz_xdp_frame covers the user-space frame reader of
// the AF_XDP transport; this covers the classifier in front of it.
//
// INPUT: [flags][queue][epoch][hw0][hw1][hw2][hw3][body...]
//   flags bit 0: structured: the harness writes Ethernet, IP and UDP headers
//                around the body (the body's first 4 bytes are tweaks, the
//                rest the UDP payload); clear: the body is the raw frame
//   flags bit 1: config public_address 0 (any IPv4 destination)
//   flags bit 2: config public_address_ipv6 all zero (any IPv6 destination)
//   flags bit 3 and 4 together: the config map lookup fails
//   flags bit 5: the socket slot for the queue is populated
//   flags bit 6: a replay entry for the structured frame's source, high water hw
//   flags bit 7: structured family IPv6 (else IPv4)
//   structured tweaks: t0 bit 0 wrong destination port, bit 1 wrong destination
//   address, bit 2 IPv4 ihl/frag or IPv6 next header taken from t1, bit 3 IP
//   length off by (int8) t2, bit 4 UDP length off by (int8) t3, bit 5 UDP
//   source port t1*257
//
// ORACLES:
//   - the verdict is XDP_DROP, XDP_PASS or XDP_REDIRECT (never ABORTED or TX);
//   - a REDIRECT calls bpf_redirect_map exactly once more;
//   - DIFFERENTIAL, structured frames at epoch 0: a REDIRECT is a datagram the
//     userspace ladder's first rungs accept: the UDP payload passes
//     rocketnet_prefilter_check with the same filter value, its type and size
//     are ROCKETNET_WIRE_SERVER_PACKET_VALID, and its size is within
//     [ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, ROCKETNET_RECEIVE_CEILING];
//     and the converse: a well-formed frame (no tweak) to the configured
//     address and port, whose payload passes those checks, with a socket and
//     no replay entry, is redirected.
//
// WHERE IT RUNS: the filter is GNU C, built as host C only by GCC or Clang on
// a non-Windows host (tests/xdp_host/). The build that links the host object
// defines ROCKETNET_FUZZ_XDP_FILTER_HOST (tests/fuzz/Makefile, the fuzz
// nightly's Linux legs, CMake's fuzz_replay_ driver where the
// rocketnet_xdp_filter_host target exists); elsewhere (MSVC) the target
// compiles to a no-op and its seed generator writes the seeds without
// replaying them.
//
// THE SEED GENERATOR: ROCKETNET_FUZZ_SEED_DIR=<dir> writes, for every
// server-facing packet type at its wire sizes and both families, three seeds
// (tools/fuzz_seedgen wrote the committed set the same way): s_ structured
// with a socket, r_ the same with a replay entry at high water 1000 (the
// seeds' sequence is ahead of it, so the lookup runs and passes), w_ the same
// frame raw. Each is replayed and must reach the verdict the userspace rungs
// name for its payload: REDIRECT when the payload passes
// rocketnet_prefilter_check, ROCKETNET_WIRE_SERVER_PACKET_VALID and the size
// bounds, DROP otherwise; and each family must have seeds of both verdicts.
// It exits 1 printing INCOMPLETE otherwise.

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"

#if defined( ROCKETNET_FUZZ_XDP_FILTER_HOST )
#define ROCKETNET_FUZZ_XDP_FILTER_ACTIVE 1
#else // #if defined( ROCKETNET_FUZZ_XDP_FILTER_HOST )
#define ROCKETNET_FUZZ_XDP_FILTER_ACTIVE 0
#endif // #if defined( ROCKETNET_FUZZ_XDP_FILTER_HOST )

#if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_frame.h"
#include "xdp_host/server_xdp_host.h"
#endif // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../portable_crt.h"

#define FZ_XDP_ABORTED  0
#define FZ_XDP_DROP     1
#define FZ_XDP_PASS     2
#define FZ_XDP_TX       3
#define FZ_XDP_REDIRECT 4

static const uint8_t fz_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
static const uint8_t fz_server_ipv4[4] = { 203, 0, 113, 10 };
static const uint8_t fz_server_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a };
static const uint8_t fz_client_ipv4[4] = { 198, 51, 100, 10 };
static const uint8_t fz_client_ipv6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
static const uint16_t fz_server_port = 40000;

static void fz_put16( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) ( v >> 8 );
    p[1] = (uint8_t) v;
}

#if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE

static void fz_require( bool condition, const char * property )
{
    if ( condition )
    {
        return;
    }
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

static uint8_t fz_frame[16384];

// one input through the filter and the oracles: the verdict, or -1 when the
// input is too short to run

static int fz_xdp_run( const uint8_t * data, size_t size )
{
    if ( size < 7 )
    {
        return -1;
    }

    const uint8_t flags = data[0];
    const uint32_t queue = (uint32_t) data[1] * 5u;       // reaches past ROCKETNET_XDP_MAX_QUEUES? no: 0 .. 1275
    const uint8_t epoch = data[2] & 1 ? data[2] : 0;      // mostly the 1.0 epoch
    const uint32_t high_water = (uint32_t) data[3] | ( (uint32_t) data[4] << 8 ) | ( (uint32_t) data[5] << 16 ) | ( (uint32_t) data[6] << 24 );
    const uint8_t * body = data + 7;
    const size_t body_bytes = size - 7;

    rocketnet_xdp_host_reset();

    struct rocketnet_server_xdp_config_t config;
    memset( &config, 0, sizeof( config ) );
    if ( !( flags & 2 ) )
    {
        memcpy( &config.public_address, fz_server_ipv4, 4 );
    }
    uint8_t port_bytes[2];
    fz_put16( port_bytes, fz_server_port );
    memcpy( &config.public_port, port_bytes, 2 );
    memcpy( config.packet_filter, fz_filter, 4 );
    config.filter_epoch = epoch;
    if ( !( flags & 4 ) )
    {
        memcpy( config.public_address_ipv6, fz_server_ipv6, 16 );
    }
    rocketnet_xdp_host_set_config( &config );
    const bool lookup_fails = ( flags & 0x18 ) == 0x18;
    rocketnet_xdp_host_set_config_lookup_fails( lookup_fails ? 1 : 0 );
    const bool socket = ( flags & 0x20 ) != 0 && queue < ROCKETNET_XDP_MAX_QUEUES;
    if ( flags & 0x20 )
    {
        rocketnet_xdp_host_set_socket_slot( queue, 1 );
    }

    int frame_bytes = 0;
    bool structured = ( flags & 1 ) != 0;
    bool clean = false;
    const uint8_t * payload = NULL;
    int payload_bytes = 0;
    bool replay = false;

    if ( !structured )
    {
        frame_bytes = (int) ( body_bytes < sizeof( fz_frame ) ? body_bytes : sizeof( fz_frame ) );
        memcpy( fz_frame, body, (size_t) frame_bytes );
    }
    else
    {
        if ( body_bytes < 4 )
        {
            return -1;
        }
        const uint8_t t0 = body[0], t1 = body[1], t2 = body[2], t3 = body[3];
        payload = body + 4;
        payload_bytes = (int) ( body_bytes - 4 );
        if ( payload_bytes > 9000 )
        {
            payload_bytes = 9000;
        }
        clean = ( t0 & 0x1f ) == 0;
        const bool ipv6 = ( flags & 0x80 ) != 0;
        const uint16_t source_port = ( t0 & 0x20 ) ? (uint16_t) ( t1 * 257u ) : 50000;
        const uint16_t dest_port = ( t0 & 1 ) ? (uint16_t) ( fz_server_port + 1 ) : fz_server_port;

        uint8_t * p = fz_frame;
        memset( p, 0, 14 );
        p[0] = 0x02;
        p[6] = 0x02;
        p[11] = 1;
        fz_put16( p + 12, ipv6 ? 0x86dd : 0x0800 );
        p += 14;

        const int udp_bytes = 8 + payload_bytes;
        int ip_header = 0;
        if ( !ipv6 )
        {
            ip_header = 20;
            memset( p, 0, 20 );
            p[0] = 0x45;
            int tot = 20 + udp_bytes;
            if ( t0 & 8 )
            {
                tot += (int8_t) t2;
            }
            fz_put16( p + 2, (uint32_t) tot & 0xffff );
            if ( t0 & 4 )
            {
                p[0] = (uint8_t) ( 0x40 | ( t1 & 0x0f ) );
                fz_put16( p + 6, (uint32_t) ( t1 >> 4 ) * 0x1001u );
            }
            p[8] = 64;
            p[9] = 17;
            memcpy( p + 12, fz_client_ipv4, 4 );
            memcpy( p + 16, fz_server_ipv4, 4 );
            if ( t0 & 2 )
            {
                p[19] ^= 0x55;
            }
        }
        else
        {
            ip_header = 40;
            memset( p, 0, 40 );
            p[0] = 0x60;
            int plen = udp_bytes;
            if ( t0 & 8 )
            {
                plen += (int8_t) t2;
            }
            fz_put16( p + 4, (uint32_t) plen & 0xffff );
            p[6] = ( t0 & 4 ) ? t1 : 17;
            p[7] = 64;
            memcpy( p + 8, fz_client_ipv6, 16 );
            memcpy( p + 24, fz_server_ipv6, 16 );
            if ( t0 & 2 )
            {
                p[39] ^= 0x55;
            }
        }
        p += ip_header;

        fz_put16( p, source_port );
        fz_put16( p + 2, dest_port );
        int ulen = udp_bytes;
        if ( t0 & 0x10 )
        {
            ulen += (int8_t) t3;
        }
        fz_put16( p + 4, (uint32_t) ulen & 0xffff );
        p += 8;
        memcpy( p, payload, (size_t) payload_bytes );
        frame_bytes = 14 + ip_header + 8 + payload_bytes;

        if ( flags & 0x40 )
        {
            struct rocketnet_server_xdp_replay_key_t key;
            memset( &key, 0, sizeof( key ) );
            key.family = ipv6 ? 6 : 4;
            uint8_t sp[2];
            fz_put16( sp, source_port );
            memcpy( &key.port, sp, 2 );
            memcpy( key.address, ipv6 ? fz_client_ipv6 : fz_client_ipv4, ipv6 ? 16 : 4 );
            rocketnet_xdp_host_set_replay_entry( &key, high_water );
            replay = true;
        }
    }

    if ( frame_bytes < 1 )
    {
        return -1;
    }

    const int redirects_before = rocketnet_xdp_host_redirect_calls();
    const int verdict = rocketnet_xdp_host_run( fz_frame, frame_bytes, queue );
    const int redirects_after = rocketnet_xdp_host_redirect_calls();

    fz_require( verdict == FZ_XDP_DROP || verdict == FZ_XDP_PASS || verdict == FZ_XDP_REDIRECT,
                "the verdict is DROP, PASS or REDIRECT" );
    if ( verdict == FZ_XDP_REDIRECT )
    {
        fz_require( redirects_after == redirects_before + 1, "a REDIRECT made exactly one redirect call" );
        fz_require( socket, "a REDIRECT has a socket for its queue" );
        fz_require( !lookup_fails, "a REDIRECT had a config" );
    }

    if ( lookup_fails || epoch != 0 )
    {
        return verdict;
    }

    // DIFFERENTIAL, every frame: what the filter redirects, the AF_XDP
    // transport's own frame reader accepts, and its payload view passes the
    // userspace prefilter rungs (else the filter spends a ring slot on a frame
    // userspace throws away)

    // known difference, reported separately (HUNT-FUZZ.md): the frame reader
    // refuses a broadcast destination MAC, the filter never reads the MAC.
    // FZ_XDP_STRICT_MAC=1 keeps it in the oracle.

    static const int strict_mac = getenv( "FZ_XDP_STRICT_MAC" ) != NULL;
    const bool broadcast = frame_bytes >= 2 && fz_frame[0] == 0xff && fz_frame[1] == 0xff;

    // known difference F1: the filter never reads the IPv4 or IPv6 version nibble.
    // FZ_XDP_STRICT_VERSION=1 keeps it in the oracle.

    static const int strict_version = getenv( "FZ_XDP_STRICT_VERSION" ) != NULL;
    const bool ipv4_bad_version = frame_bytes > 14 && fz_frame[12] == 0x08 && fz_frame[13] == 0x00 && ( fz_frame[14] >> 4 ) != 4;
    const bool ipv6_bad_version = frame_bytes > 14 && fz_frame[12] == 0x86 && fz_frame[13] == 0xdd && ( fz_frame[14] >> 4 ) != 6;

    if ( verdict == FZ_XDP_REDIRECT && ( strict_mac || !broadcast ) && ( strict_version || ( !ipv4_bad_version && !ipv6_bad_version ) ) )
    {
        uint8_t * copy = (uint8_t *) malloc( (size_t) frame_bytes );
        memcpy( copy, fz_frame, (size_t) frame_bytes );
        rocketnet_address_t from;
        const uint8_t * view = NULL;
        int view_bytes = 0;
        const bool parsed = rocketnet_xdp_frame_parse( copy, (size_t) frame_bytes, &from, &view, &view_bytes );
        if ( !parsed )
        {
            fprintf( stderr, "frame_bytes %d\n", frame_bytes );
        }
        fz_require( parsed, "DIFFERENTIAL: a frame the filter redirects is one the AF_XDP frame reader parses" );
        const bool view_ok = view_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES && view_bytes <= ROCKETNET_RECEIVE_CEILING &&
                             rocketnet_prefilter_check( view, view_bytes, fz_filter ) >= 0 &&
                             ROCKETNET_WIRE_SERVER_PACKET_VALID( view[0], view_bytes );
        if ( !view_ok )
        {
            fprintf( stderr, "view_bytes %d type %d\n", view_bytes, view_bytes > 0 ? view[0] : -1 );
        }
        fz_require( view_ok, "DIFFERENTIAL: a redirected frame's payload view passes the userspace prefilter rungs" );
        free( copy );
    }

    if ( !structured )
    {
        return verdict;
    }

    const bool user_accepts = payload_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES &&
                              payload_bytes <= ROCKETNET_RECEIVE_CEILING &&
                              rocketnet_prefilter_check( payload, payload_bytes, fz_filter ) >= 0 &&
                              ROCKETNET_WIRE_SERVER_PACKET_VALID( payload[0], payload_bytes );

    if ( verdict == FZ_XDP_REDIRECT && clean )
    {
        fz_require( user_accepts, "DIFFERENTIAL: a clean frame the filter redirects passes the userspace prefilter rungs" );
    }

    if ( clean && socket && !replay && user_accepts )
    {
        if ( verdict != FZ_XDP_REDIRECT )
        {
            fprintf( stderr, "verdict %d payload_bytes %d type %d\n", verdict, payload_bytes, payload[0] );
        }
        fz_require( verdict == FZ_XDP_REDIRECT, "DIFFERENTIAL: a clean frame the userspace prefilter accepts is redirected" );
    }

    return verdict;
}

#endif // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>

#define FZ_SEED_MAX ( 11 + 14 + 40 + 8 + 2048 )

struct fz_seed_type_t
{
    const char * name;
    uint8_t type;
    int bytes;
};

static const fz_seed_type_t fz_seed_types[] = {
    { "request", 0, 710 },
    { "response", 3, 50 },
    { "keepalive", 4, 41 },
    { "keepalive", 4, 60 },
    { "payload", 5, 34 },
    { "payload", 5, 200 },
    { "payload", 5, 1416 + 17 + 16 },
    { "disconnect", 6, 33 },
    { "ack", 8, 33 },
    { "ack", 8, 64 },
    { "denied", 1, 34 },
    { "challenge", 2, 66 },
    { "cookie", 7, 38 },
};

// the UDP payload every seed carries: the type, epoch 0, the test filter
// value, then a fixed pattern (the sequence after the prefilter header is
// far ahead of the r_ seeds' high water)

static int fz_seed_payload( const fz_seed_type_t * entry, uint8_t * out )
{
    out[0] = entry->type;
    out[1] = 0;
    memcpy( out + 2, fz_filter, ROCKETNET_FILTER_VALUE_BYTES );
    for ( int i = 2 + ROCKETNET_FILTER_VALUE_BYTES; i < entry->bytes; i++ )
    {
        out[i] = (uint8_t) ( i * 13 );
    }
    return entry->bytes;
}

// the whole frame the structured mode builds for a clean frame, untweaked

static int fz_seed_frame( bool ipv6, const uint8_t * payload, int payload_bytes, uint8_t * out )
{
    uint8_t * p = out;
    memset( p, 0, 14 );
    p[0] = 0x02;
    p[6] = 0x02;
    p[11] = 1;
    fz_put16( p + 12, ipv6 ? 0x86dd : 0x0800 );
    p += 14;
    const int udp_bytes = 8 + payload_bytes;
    if ( ipv6 )
    {
        memset( p, 0, 40 );
        p[0] = 0x60;
        fz_put16( p + 4, (uint32_t) udp_bytes );
        p[6] = 17;
        p[7] = 64;
        memcpy( p + 8, fz_client_ipv6, 16 );
        memcpy( p + 24, fz_server_ipv6, 16 );
        p += 40;
    }
    else
    {
        memset( p, 0, 20 );
        p[0] = 0x45;
        fz_put16( p + 2, (uint32_t) ( 20 + udp_bytes ) );
        p[8] = 64;
        p[9] = 17;
        memcpy( p + 12, fz_client_ipv4, 4 );
        memcpy( p + 16, fz_server_ipv4, 4 );
        p += 20;
    }
    memset( p, 0, 8 );
    fz_put16( p, 50000 );
    fz_put16( p + 2, fz_server_port );
    fz_put16( p + 4, (uint32_t) udp_bytes );
    p += 8;
    memcpy( p, payload, (size_t) payload_bytes );
    p += payload_bytes;
    return (int) ( p - out );
}

static bool fz_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
{
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

static bool fz_generate( const char * dir )
{
    static uint8_t payload[2048];
    static uint8_t seed[FZ_SEED_MAX];
    bool complete = true;
    int reached[2][2] = { { 0, 0 }, { 0, 0 } };     // [family][redirect]

    for ( size_t i = 0; i < sizeof( fz_seed_types ) / sizeof( fz_seed_types[0] ); ++i )
    {
        const fz_seed_type_t * entry = &fz_seed_types[i];
        const int payload_bytes = fz_seed_payload( entry, payload );

        // the verdict the userspace rungs name for this payload

        const bool redirect = payload_bytes >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES && payload_bytes <= ROCKETNET_RECEIVE_CEILING &&
                              rocketnet_prefilter_check( payload, payload_bytes, fz_filter ) >= 0 &&
                              ROCKETNET_WIRE_SERVER_PACKET_VALID( payload[0], payload_bytes );

        for ( int family = 0; family < 2; family++ )
        {
            const bool ipv6 = family == 1;
            const uint8_t ipv6_flag = ipv6 ? 0x80 : 0;
            for ( int form = 0; form < 3; form++ )
            {
                static const char * const prefixes[3] = { "s", "r", "w" };
                int bytes = 0;
                if ( form == 0 || form == 1 )
                {
                    // structured: [flags][queue 3][epoch 0][hw x4][tweaks x4], socket populated;
                    // r_ adds a replay entry at high water 1000
                    memset( seed, 0, 11 );
                    seed[0] = (uint8_t) ( ( form == 0 ? 0x21 : 0x61 ) | ipv6_flag );
                    seed[1] = 3;
                    if ( form == 1 )
                    {
                        seed[3] = 0xe8;
                        seed[4] = 0x03;
                    }
                    memcpy( seed + 11, payload, (size_t) payload_bytes );
                    bytes = 11 + payload_bytes;
                }
                else
                {
                    // raw: [flags: socket][queue 3][epoch 0][hw x4] then the frame
                    memset( seed, 0, 7 );
                    seed[0] = 0x20;
                    seed[1] = 3;
                    bytes = 7 + fz_seed_frame( ipv6, payload, payload_bytes, seed + 7 );
                }

                char name[128];
                snprintf( name, sizeof( name ), "%s_%s_%s_%d", prefixes[form], entry->name, ipv6 ? "v6" : "v4", payload_bytes );

#if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
                const int verdict = fz_xdp_run( seed, (size_t) bytes );
                const int want = redirect ? FZ_XDP_REDIRECT : FZ_XDP_DROP;
                if ( verdict != want )
                {
                    fprintf( stderr, "seed generator: the %s seed's verdict is %d, not %d\n", name, verdict, want );
                    complete = false;
                    continue;
                }
                reached[family][redirect ? 1 : 0]++;
#else // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
                (void) redirect;
                reached[family][0] = reached[family][1] = 1;
#endif // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE

                if ( !fz_seed_write( dir, name, seed, bytes ) )
                {
                    complete = false;
                }
            }
        }
    }

    for ( int family = 0; family < 2; family++ )
    {
        if ( reached[family][0] == 0 || reached[family][1] == 0 )
        {
            fprintf( stderr, "seed generator: the %s seeds do not reach both REDIRECT and DROP\n", family ? "IPv6" : "IPv4" );
            complete = false;
        }
    }

#if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
    printf( "seed generator: %s\n", complete ? "every seed reaches the verdict its payload names" : "INCOMPLETE" );
#else // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
    printf( "seed generator: %s\n", complete ? "seeds written; the filter is not built here, so none was replayed" : "INCOMPLETE" );
#endif // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
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
        exit( fz_generate( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
#if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
    (void) fz_xdp_run( data, size );
#else // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
    (void) data;
    (void) size;
#endif // #if ROCKETNET_FUZZ_XDP_FILTER_ACTIVE
    return 0;
}
