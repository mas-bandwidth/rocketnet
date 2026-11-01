/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The wire-facing parser fuzz harness. One libFuzzer binary covering
// the three surfaces that read bytes rocketnet did not write -- the packet
// prefilter, base64 decoding, and the config value parses -- because one
// binary is one compile of the sdk, and the per-commit budget is minutes.
//
// THE SELECTOR BYTE: input[0] & 3 chooses the surface, and the rest of the
// input is that surface's bytes. 0 and 1 = prefilter, 2 = base64,
// 3 = config. The committed corpus under tests/fuzz/corpus is named for the
// surface each file selects, so a regression input stays readable.
//
// PER-INPUT DETERMINISM IS A REQUIREMENT HERE, not a nicety: the per-commit
// gate is a corpus REPLAY, and a replay is only a gate if input N behaves
// the same whether it is replayed alone or after input N-1. No surface
// keeps state across a call.
//
// THE INPUT BOUNDS ARE THE TRANSPORT'S OWN. A parser is only obliged to
// survive what the transport can hand it. Fuzzing past that contract
// manufactures crashes that no packet can cause, so each surface caps its
// slice where the transport caps it, and the cap is named.

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_base64.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>

#include "../portable_crt.h"

static const uint8_t rocketnet_fuzz_filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// -----------------------------------------------------------------------
//  the types this harness's parsers read, and the counter each raises
// -----------------------------------------------------------------------
//
// Three surfaces read bytes here, and each branches on a type the wire
// carries: the packet type the prefilter hands back, the shape a base64
// string has, and the shape a config value has. A type with no seed is a branch the fuzzer never
// enters, so every type gets a counter and the seed generator asks the
// only question that matters -- does a seed reach this type at all --
// with a number that only goes up.
//
// Where the library counts the type itself (the prefilter's refusals)
// the library's counter is the one the
// generator reads, and the harness raises it as the parser runs. The
// types the library does not count (the packet types the prefilter
// returns, the base64 shapes, the config shapes) have no library row to
// raise, so the harness keeps the counter for them here.

enum rocketnet_fuzz_type_t
{
    ROCKETNET_FUZZ_TYPE_PREFILTER_REQUEST = 0,
    ROCKETNET_FUZZ_TYPE_PREFILTER_DENIED,
    ROCKETNET_FUZZ_TYPE_PREFILTER_CHALLENGE,
    ROCKETNET_FUZZ_TYPE_PREFILTER_RESPONSE,
    ROCKETNET_FUZZ_TYPE_PREFILTER_KEEP_ALIVE,
    ROCKETNET_FUZZ_TYPE_PREFILTER_PAYLOAD,
    ROCKETNET_FUZZ_TYPE_PREFILTER_DISCONNECT,
    ROCKETNET_FUZZ_TYPE_PREFILTER_COOKIE,
    ROCKETNET_FUZZ_TYPE_PREFILTER_ACK,
    ROCKETNET_FUZZ_TYPE_PREFILTER_RESERVED,
    ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_SHORT,
    ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_LARGE,
    ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_EPOCH,
    ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_VALUE,
    ROCKETNET_FUZZ_TYPE_BASE64_DECODED,
    ROCKETNET_FUZZ_TYPE_BASE64_OVER_CAPACITY,
    ROCKETNET_FUZZ_TYPE_BASE64_ENCODED,
    ROCKETNET_FUZZ_TYPE_CONFIG_INT_PARSED,
    ROCKETNET_FUZZ_TYPE_CONFIG_INT_REFUSED,
    ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_PARSED,
    ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_REFUSED,
    ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME,
    ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME_REFUSED,
    ROCKETNET_FUZZ_TYPE_COUNT
};

struct rocketnet_fuzz_type_info_t
{
    const char * name;            // the type's name, and its seed's name
    int library_counter;          // the library counter it raises, or -1
};

// the library counters the harness raises: the prefilter's four refusals

static const struct rocketnet_fuzz_type_info_t rocketnet_fuzz_types[ROCKETNET_FUZZ_TYPE_COUNT] = {
    { "prefilter_request", -1 },
    { "prefilter_denied", -1 },
    { "prefilter_challenge", -1 },
    { "prefilter_response", -1 },
    { "prefilter_keep_alive", -1 },
    { "prefilter_payload", -1 },
    { "prefilter_disconnect", -1 },
    { "prefilter_cookie", -1 },
    { "prefilter_ack", -1 },
    { "prefilter_reserved_type", -1 },
    { "prefilter_too_short", ROCKETNET_COUNTER_PREFILTER_TOO_SHORT },
    { "prefilter_too_large", ROCKETNET_COUNTER_PREFILTER_TOO_LARGE },
    { "prefilter_wrong_epoch", ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH },
    { "prefilter_wrong_value", ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE },
    { "base64_decoded", -1 },
    { "base64_over_capacity", -1 },
    { "base64_encoded", -1 },
    { "config_int_parsed", -1 },
    { "config_int_refused", -1 },
    { "config_ladder_parsed", -1 },
    { "config_ladder_refused", -1 },
    { "config_env_name", -1 },
    { "config_env_name_refused", -1 }
};

// the library counters the harness raises, and the harness's own counters
// for the types the library does not count

static struct rocketnet_counters_t rocketnet_fuzz_counters;
static uint64_t rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_COUNT];

// the prefilter hands back the type byte it read: 0 through 8 are the
// wire's packet types and 9 through 255 are reserved, so the reserved
// types share one counter

static int rocketnet_fuzz_prefilter_type_index( int type_byte )
{
    if ( type_byte < 0 || type_byte > 255 )
        return -1;
    if ( type_byte < ROCKETNET_FIRST_RESERVED_PACKET_TYPE )
        return ROCKETNET_FUZZ_TYPE_PREFILTER_REQUEST + type_byte;

    return ROCKETNET_FUZZ_TYPE_PREFILTER_RESERVED;
}

static uint64_t rocketnet_fuzz_type_value( int type )
{
    const struct rocketnet_fuzz_type_info_t * t = &rocketnet_fuzz_types[type];
    if ( t->library_counter >= 0 )
        return rocketnet_counter_value( &rocketnet_fuzz_counters, t->library_counter );
    return rocketnet_fuzz_type_counters[type];
}

// what one input does, named so the seed generator can replay a seed
// through the target exactly as the fuzzer would

static void rocketnet_fuzz_run( const uint8_t * data, size_t size );

// -----------------------------------------------------------------------
//  0: the packet prefilter -- the first thing that touches a datagram
// -----------------------------------------------------------------------

static void rocketnet_fuzz_prefilter( const uint8_t * data, size_t bytes )
{
    // the platform receive path never presents more than the receive
    // ceiling, and the prefilter's own TOO_LARGE branch is the check for
    // anything at it or past it, so a slice one byte over the ceiling is
    // the largest input with meaning here

    if ( bytes > ROCKETNET_RECEIVE_CEILING + 1 )
    {
        bytes = ROCKETNET_RECEIVE_CEILING + 1;
    }

    // Give ASan a boundary at the declared slice, not at the end of the
    // larger libFuzzer input from which it was taken.

    std::unique_ptr<uint8_t[]> packet( new uint8_t[bytes] );
    memcpy( packet.get(), data, bytes );

    const int result = rocketnet_prefilter_check( packet.get(), (int) bytes, rocketnet_fuzz_filter_value );

    if ( result < 0 )
    {
        // every refusal must map to a counter: an unmapped code would be a
        // refusal nothing counts, which the standard forbids

        const int counter = rocketnet_prefilter_counter( result );
        if ( counter < 0 )
        {
            abort();
        }

        rocketnet_fuzz_counters.values[counter]++;

        if ( result == ROCKETNET_PREFILTER_TOO_SHORT )
        {
            rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_SHORT]++;
        }
        else if ( result == ROCKETNET_PREFILTER_TOO_LARGE )
        {
            rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_LARGE]++;
        }
        else if ( result == ROCKETNET_PREFILTER_WRONG_EPOCH )
        {
            rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_EPOCH]++;
        }
        else if ( result == ROCKETNET_PREFILTER_WRONG_VALUE )
        {
            rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_VALUE]++;
        }
        else
        {
            abort();
        }
    }
    else
    {
        // an accepted packet's type is a byte, and the prefilter is
        // deliberately type-blind: anything outside 0..255 is impossible

        if ( result > 255 )
        {
            abort();
        }

        const int index = rocketnet_fuzz_prefilter_type_index( result );
        if ( index < 0 )
        {
            abort();
        }

        rocketnet_fuzz_type_counters[index]++;
    }
}

// -----------------------------------------------------------------------
//  2: base64 -- how every key and secret reaches a config
// -----------------------------------------------------------------------

static void rocketnet_fuzz_base64( const uint8_t * data, size_t bytes )
{
    // the config engine's BYTES fields are base64 of at most a key's worth
    // of bytes; 4096 is far past every field and keeps the harness fast

    if ( bytes > 4096 )
    {
        bytes = 4096;
    }

    std::unique_ptr<uint8_t[]> input( new uint8_t[bytes] );
    memcpy( input.get(), data, bytes );
    data = input.get();

    char text[4097];
    memcpy( text, data, bytes );
    text[bytes] = '\0';

    uint8_t decoded[4097];
    const int wide_bytes = rocketnet_base64_decode_data( text, decoded, sizeof( decoded ) );
    if ( wide_bytes > 0 )
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_BASE64_DECODED]++;
    }

    // THE REAL CALLER'S GEOMETRY, and the reason this second decode exists.
    // rocketnet_config_apply_env hands this decoder a value straight out of
    // getenv -- any length at all -- against an output buffer that is the
    // field's own size, a key's 32 bytes. A decode into a buffer that can
    // never be too small exercises everything EXCEPT the check that stands
    // between an over-long env var and the config struct behind it, so the
    // capacity refusal above would be unreachable from this harness without
    // this call.

    uint8_t key_field[ROCKETNET_KEY_BYTES];
    const int key_bytes = rocketnet_base64_decode_data( text, key_field, sizeof( key_field ) );
    if ( key_bytes < 0 || key_bytes > (int) sizeof( key_field ) )
    {
        abort();
    }

    if ( wide_bytes > (int) sizeof( key_field ) )
    {
        rocketnet_fuzz_require( key_bytes == 0, "base64 overlong field must refuse" );
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_BASE64_OVER_CAPACITY]++;
    }

    if ( wide_bytes > 0 && wide_bytes <= (int) sizeof( key_field ) )
        rocketnet_fuzz_require( key_bytes == wide_bytes && memcmp( key_field, decoded, (size_t) wide_bytes ) == 0,
                                "base64 fitting field must preserve all bytes" );

    // ONE SPELLING PER KEY, as an oracle rather than a table of examples:
    // whatever these bytes are, encoding then decoding them must return
    // exactly them. A decoder that accepted a second spelling would still
    // pass this; a decoder that lost a byte could not.

    char encoded[8192];
    const int encoded_length = rocketnet_base64_encode_data( data, bytes, encoded, sizeof( encoded ) );
    if ( encoded_length > 0 )
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_BASE64_ENCODED]++;
    }

    char small_encoded[8];
    const int small_encoded_length = rocketnet_base64_encode_data( data, bytes, small_encoded, sizeof( small_encoded ) );
    if ( encoded_length >= (int) sizeof( small_encoded ) )
    {
        rocketnet_fuzz_require( small_encoded_length == -1, "base64 encode must refuse insufficient capacity" );
    }

    if ( encoded_length > 0 && encoded_length < (int) sizeof( small_encoded ) )
        rocketnet_fuzz_require( small_encoded_length == encoded_length && memcmp( small_encoded, encoded, (size_t) encoded_length + 1 ) == 0,
                                "base64 fitting encode must preserve bytes and terminator" );

    if ( encoded_length > 0 )
    {
        uint8_t round_trip[4097];
        const int decoded_length = rocketnet_base64_decode_data( encoded, round_trip, sizeof( round_trip ) );
        if ( decoded_length != (int) bytes )
        {
            abort();
        }

        if ( bytes > 0 && memcmp( round_trip, data, bytes ) != 0 )
        {
            abort();
        }
    }
}

// -----------------------------------------------------------------------
//  3: the config value parses -- the transport ladder and the integer parse
// -----------------------------------------------------------------------

static void rocketnet_fuzz_config( const uint8_t * data, size_t bytes )
{
    // an environment variable's value; the platform's own getenv surface is
    // the bound, and nothing near it is interesting

    if ( bytes > 1024 )
    {
        bytes = 1024;
    }

    char value[1025];
    memcpy( value, data, bytes );
    value[bytes] = '\0';

    int parsed = 0;
    if ( rocketnet_config_parse_int_value( value, &parsed ) )
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_INT_PARSED]++;
    }
    else
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_INT_REFUSED]++;
    }

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );

    struct rocketnet_transport_ladder_t ladder;
    memset( &ladder, 0, sizeof( ladder ) );

    const bool ladder_parsed = rocketnet_transport_ladder_parse( value, available, num_available, &ladder );
    if ( ladder_parsed )
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_PARSED]++;
    }
    else
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_REFUSED]++;
    }

    if ( ladder_parsed )
    {
        // a ladder that parsed is a ladder the transport will walk: every
        // candidate must be a real transport index and the list must fit the
        // array it was written into

        if ( ladder.num_candidates < 1 || ladder.num_candidates > ROCKETNET_MAX_TRANSPORT_CANDIDATES )
        {
            abort();
        }

        if ( ladder.num_skipped < 0 || ladder.num_skipped > ROCKETNET_MAX_TRANSPORT_CANDIDATES )
        {
            abort();
        }

        for ( int i = 0; i < ladder.num_candidates; i++ )
        {
            if ( ladder.candidates[i] < 0 || ladder.candidates[i] >= ROCKETNET_NUM_TRANSPORTS )
            {
                abort();
            }

            if ( rocketnet_transport_name( ladder.candidates[i] ) == NULL )
            {
                abort();
            }
        }
    }

    char env_name[256];
    if ( rocketnet_config_env_var_name( value, env_name, sizeof( env_name ) ) )
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME]++;
    }
    else
    {
        rocketnet_fuzz_type_counters[ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME_REFUSED]++;
    }
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
//
// One named seed per type, each built from the wire's own constants: a
// prefilter header of the type's packet, a base64 string of the shape the decoder reads, a
// config value of the shape the parser reads. Every seed is replayed
// through the target before it is written, and the generator refuses to
// finish unless every type the parsers read has a seed that raises that
// type's counter: a type with no seed is a branch the fuzzer never
// enters.
//
// The seeds are deterministic, so a regenerated corpus differs only when
// the code under it changed.
//
// ROCKETNET_FUZZ_SEED_SKIP names the types to withhold, comma separated.
// It is how the INCOMPLETE path is shown: a type whose seed is withheld
// has no seed that reaches it, so the generator exits 1 and says so.
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_SEED_MAX 2048

static int rocketnet_fuzz_seed_prefilter( uint8_t * out, int type_byte, int epoch_byte, int filter_wrong, int bytes )
{
    if ( bytes < 1 || bytes > ROCKETNET_FUZZ_SEED_MAX )
        return 0;
    out[0] = 0;                                     // the selector: the prefilter
    out[1] = (uint8_t) type_byte;
    out[2] = (uint8_t) epoch_byte;
    out[3] = filter_wrong ? 0x00 : rocketnet_fuzz_filter_value[0];
    out[4] = filter_wrong ? 0x00 : rocketnet_fuzz_filter_value[1];
    out[5] = filter_wrong ? 0x00 : rocketnet_fuzz_filter_value[2];
    out[6] = filter_wrong ? 0x00 : rocketnet_fuzz_filter_value[3];

    for ( int i = ROCKETNET_PREFILTER_BYTES + 1; i < bytes; i++ )
    {
        out[i] = 0x00;
    }

    return 1 + bytes;
}

static int rocketnet_fuzz_seed_build( int type, uint8_t * out, int capacity )
{
    if ( capacity < ROCKETNET_FUZZ_SEED_MAX )
        return 0;

    switch ( type )
    {
        // 0: the prefilter's packet types, in the wire's order, and the
        // first reserved type above them
        case ROCKETNET_FUZZ_TYPE_PREFILTER_REQUEST:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_DENIED:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_CHALLENGE:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_RESPONSE:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_KEEP_ALIVE:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_PAYLOAD:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_DISCONNECT:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_COOKIE:
        case ROCKETNET_FUZZ_TYPE_PREFILTER_ACK:
            return rocketnet_fuzz_seed_prefilter( out, type - ROCKETNET_FUZZ_TYPE_PREFILTER_REQUEST,
                                                  ROCKETNET_FILTER_EPOCH_UNKEYED, 0, ROCKETNET_PREFILTER_BYTES );
        case ROCKETNET_FUZZ_TYPE_PREFILTER_RESERVED:
            return rocketnet_fuzz_seed_prefilter( out, ROCKETNET_FIRST_RESERVED_PACKET_TYPE, ROCKETNET_FILTER_EPOCH_UNKEYED, 0, ROCKETNET_PREFILTER_BYTES );

        // the prefilter's refusals: one byte short of the header, one byte
        // past the receive ceiling, an epoch this build does not hold, and
        // a filter value that is not the one
        case ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_SHORT:
            return rocketnet_fuzz_seed_prefilter( out, 0, ROCKETNET_FILTER_EPOCH_UNKEYED, 0, ROCKETNET_PREFILTER_BYTES - 1 );
        case ROCKETNET_FUZZ_TYPE_PREFILTER_TOO_LARGE:
            return rocketnet_fuzz_seed_prefilter( out, 0, ROCKETNET_FILTER_EPOCH_UNKEYED, 0, ROCKETNET_RECEIVE_CEILING + 1 );
        case ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_EPOCH:
            return rocketnet_fuzz_seed_prefilter( out, 0, ROCKETNET_FILTER_EPOCH_UNKEYED + 1, 0, ROCKETNET_PREFILTER_BYTES );
        case ROCKETNET_FUZZ_TYPE_PREFILTER_WRONG_VALUE:
            return rocketnet_fuzz_seed_prefilter( out, 0, ROCKETNET_FILTER_EPOCH_UNKEYED, 1, ROCKETNET_PREFILTER_BYTES );

        // 2: the base64 shapes: a string that decodes, one that decodes
        // past a key's worth of bytes, and one the decoder refuses
        case ROCKETNET_FUZZ_TYPE_BASE64_DECODED:
        {
            out[0] = 2;
            memcpy( out + 1, "QUFB", 4 );
            return 5;
        }
        case ROCKETNET_FUZZ_TYPE_BASE64_OVER_CAPACITY:
        {
            // 44 characters: 33 bytes, one byte past a key

            out[0] = 2;
            memset( out + 1, 'A', 44 );
            return 45;
        }
        case ROCKETNET_FUZZ_TYPE_BASE64_ENCODED:
        {
            out[0] = 2;
            out[1] = '!';
            return 2;
        }

        // 3: the config shapes
        case ROCKETNET_FUZZ_TYPE_CONFIG_INT_PARSED:
        {
            out[0] = 3;
            memcpy( out + 1, "1452", 4 );
            return 5;
        }
        case ROCKETNET_FUZZ_TYPE_CONFIG_INT_REFUSED:
        {
            out[0] = 3;
            memcpy( out + 1, "12x", 3 );
            return 4;
        }
        case ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_PARSED:
        {
            out[0] = 3;
            memcpy( out + 1, "portable", 8 );
            return 9;
        }
        case ROCKETNET_FUZZ_TYPE_CONFIG_LADDER_REFUSED:
        {
            out[0] = 3;
            memcpy( out + 1, "rocket", 6 );
            return 7;
        }
        case ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME:
        {
            out[0] = 3;
            memcpy( out + 1, "server_transport", 16 );
            return 17;
        }
        case ROCKETNET_FUZZ_TYPE_CONFIG_ENV_NAME_REFUSED:
        {
            // longer than the env var name buffer holds with its prefix

            out[0] = 3;
            memset( out + 1, 'a', 250 );
            return 251;
        }
        default:
            break;
    }

    return 0;
}

static bool rocketnet_fuzz_type_withheld( const char * list, const char * name )
{
    if ( !list || !list[0] )
        return false;

    const size_t length = strlen( name );
    const char * at = list;
    while ( *at )
    {
        const char * end = strchr( at, ',' );
        const size_t span = end ? (size_t) ( end - at ) : strlen( at );
        if ( span == length && memcmp( at, name, length ) == 0 )
            return true;
        if ( !end )
            break;
        at = end + 1;
    }

    return false;
}

static bool rocketnet_fuzz_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
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

static bool rocketnet_fuzz_generate( const char * dir )
{
    char skip[256];
    skip[0] = '\0';
    rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_SKIP", skip, sizeof( skip ) );

    static uint8_t seed[ROCKETNET_FUZZ_SEED_MAX];
    bool complete = true;

    for ( int type = 0; type < ROCKETNET_FUZZ_TYPE_COUNT; type++ )
    {
        const char * name = rocketnet_fuzz_types[type].name;
        if ( rocketnet_fuzz_type_withheld( skip, name ) )
        {
            fprintf( stderr, "seed generator: withholding %s\n", name );
            complete = false;
            continue;
        }

        const int bytes = rocketnet_fuzz_seed_build( type, seed, (int) sizeof( seed ) );
        if ( bytes <= 0 )
        {
            fprintf( stderr, "seed generator: no seed for %s\n", name );
            complete = false;
            continue;
        }

        // the seed is replayed through the target before it is written:
        // a seed that reaches no counter is no seed at all

        const uint64_t before = rocketnet_fuzz_type_value( type );

        rocketnet_fuzz_run( seed, (size_t) bytes );
        if ( rocketnet_fuzz_type_value( type ) <= before )
        {
            fprintf( stderr, "seed generator: the %s seed reaches no %s\n", name, name );
            complete = false;
            continue;
        }

        if ( !rocketnet_fuzz_seed_write( dir, name, seed, bytes ) )
        {
            complete = false;
        }
    }

    printf( "seed generator: %s\n", complete ? "every type the parsers read has a seed that reaches it" : "INCOMPLETE" );
    return complete;
}

// -----------------------------------------------------------------------

static void rocketnet_fuzz_run( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return;

    const uint8_t * body = data + 1;
    const size_t body_size = size - 1;

    switch ( data[0] & 3 )
    {
        case 0:
        case 1: rocketnet_fuzz_prefilter( body, body_size ); break;
        case 2: rocketnet_fuzz_base64( body, body_size ); break;
        case 3: rocketnet_fuzz_config( body, body_size ); break;
    }
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    // the parsers under test allocate through rocketnet_malloc and touch
    // nothing else: no rocketnet_init, so no platform threads and no
    // sodium, and the harness stays as deterministic as the parsers are

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    char seed_dir[1024];
    const int have_dir = rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) );
    if ( have_dir && seed_dir[0] != '\0' )
    {
        exit( rocketnet_fuzz_generate( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    rocketnet_fuzz_run( data, size );
    return 0;
}
