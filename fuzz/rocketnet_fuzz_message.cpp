/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message record fuzz harness (docs/CHANNELS.md layer 2). It drives the
// decoder of sdk/rocketnet_message.cpp -- the first code above the transport
// that reads bytes the remote endpoint wrote -- and checks the layer's promises as an
// oracle, not just its survival:
//
//   - begin and check_payload agree, and name exactly one violation or none;
//   - a refused payload hands out no record (a record is whole or absent);
//   - an accepted payload's records are each whole, lie inside the body and
//     never on the form byte, tile the body exactly, are each a record a
//     correct sender may write, and pack back to the input byte for byte:
//     the codec is a bijection on valid payloads, so nothing in a payload is
//     ignored;
//   - an accepted CONFIG body of version 1 writes back to the same 28 bytes.
//
// Reading past the payload is caught by the sanitizers: libFuzzer hands the
// input in an allocation of exactly its size, and the payload is its tail.
//
// THE SELECTOR BYTE: input[0] & 3 chooses the receiver's limits, and the
// rest of the input is the payload.
//   0 = the first version: 32 channels, codes 0 and 3, the default W, B and
//       max_message_bytes
//   1 = every code, 1024 channels (WIDE in reach), the defaults otherwise
//   2 = every code, 65536 channels, W = 2, max_message_bytes 2305 and B 4096:
//       the block and ring bounds close enough for mutation to cross them
//   3 = the CONFIG body reader, on the rest of the input
// The committed corpus under fuzz/corpus_message is named for the shape each
// file holds, so a regression input stays readable.
//
// No state survives a call, so a corpus replay is a gate: input N behaves
// the same alone or after input N-1.
//
// Built as ci.yml builds the other targets, against the instrumented
// library (build-fuzz, fuzzer-no-link + asan + ubsan); the clang++ line is
// one command, wrapped here:
//
//   clang++ -g -O1 -std=c++17 -fsanitize=fuzzer,address,undefined
//     -fno-sanitize-recover=all -fno-omit-frame-pointer -Isdk
//     fuzz/rocketnet_fuzz_message.cpp build-fuzz/librocketnet.a
//     -lbpf -lxdp -lpthread -o bin/fuzz_message
//   ./bin/fuzz_message fuzz/corpus_message/*                     # the replay gate
//   ./bin/fuzz_message corpus fuzz/corpus_message -dict=fuzz/rocketnet.dict

#include "rocketnet_internal.h"
#include "rocketnet_message.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../tests/portable_crt.h"

// -----------------------------------------------------------------------
//  the types this harness's parser reads, and the counter each raises
// -----------------------------------------------------------------------
//
// The decoder names one violation per check of STANDARD.md _Messages_, and
// every record it accepts carries a code. Those are the types this parser
// reads: a violation with no seed is a check the fuzzer never enters, and
// a code with no seed is a reliability the fuzzer never decodes. Each type
// gets a counter here, so the seed generator can ask whether a seed reaches
// it at all. The library counts no message-layer counters of its own, so
// the harness keeps them.

enum rocketnet_fuzz_message_type_t
{
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_NONE = 0,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_FORM,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_TRUNCATED,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_FLAGS,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_LENGTH,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_CHANNEL,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_TYPE_NOT_ENABLED,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_BLOCK,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_RING,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_SIZE,
    ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_CONTROL,
    ROCKETNET_FUZZ_MESSAGE_CODE_UNRELIABLE,
    ROCKETNET_FUZZ_MESSAGE_CODE_UNRELIABLE_SEQUENCED,
    ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE,
    ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE_ORDERED,
    ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE_SEQUENCED,
    ROCKETNET_FUZZ_MESSAGE_TYPE_COUNT
};

struct rocketnet_fuzz_message_type_info_t
{
    const char * name;
};

static const struct rocketnet_fuzz_message_type_info_t rocketnet_fuzz_message_types[ROCKETNET_FUZZ_MESSAGE_TYPE_COUNT] =
{
    { "violation_none" },
    { "violation_bad_form" },
    { "violation_truncated" },
    { "violation_bad_flags" },
    { "violation_bad_length" },
    { "violation_bad_channel" },
    { "violation_type_not_enabled" },
    { "violation_bad_block" },
    { "violation_bad_ring" },
    { "violation_bad_size" },
    { "violation_bad_control" },
    { "code_unreliable" },
    { "code_unreliable_sequenced" },
    { "code_reliable" },
    { "code_reliable_ordered" },
    { "code_reliable_sequenced" }
};

static uint64_t rocketnet_fuzz_message_counters[ROCKETNET_FUZZ_MESSAGE_TYPE_COUNT];

// the violation a parse names, and the type it is
static int rocketnet_fuzz_message_violation_type( int violation )
{
    if ( violation < 0 || violation >= ROCKETNET_FUZZ_MESSAGE_TYPE_COUNT )
        return -1;
    return violation;
}

// the code a record carries, and the type it is
static int rocketnet_fuzz_message_code_type( int code )
{
    if ( code < 0 || code >= ROCKETNET_MESSAGE_NUM_CODES )
        return -1;
    return ROCKETNET_FUZZ_MESSAGE_CODE_UNRELIABLE + code;
}

// what one input does, named so the seed generator can replay a seed
// through the target exactly as the fuzzer would
static void rocketnet_fuzz_message_run( const uint8_t * data, size_t size );

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition ) return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

static struct rocketnet_message_limits_t rocketnet_fuzz_message_limits( int profile )
{
    struct rocketnet_message_limits_t limits;
    limits.num_channels = 32;
    limits.enabled_codes = rocketnet_message_enabled_codes( 0x09 );
    limits.reliable_messages_in_flight = 1024;
    limits.max_message_bytes = 32768;
    limits.receive_message_buffer_bytes = 65536;
    if ( profile == 1 )
    {
        limits.num_channels = 1024;
        limits.enabled_codes = 0x1F;
    }
    else if ( profile == 2 )
    {
        limits.num_channels = 65536;
        limits.enabled_codes = 0x1F;
        limits.reliable_messages_in_flight = 2;
        limits.max_message_bytes = 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
        limits.receive_message_buffer_bytes = 4096;
    }
    return limits;
}

static void rocketnet_fuzz_message_payload( const uint8_t * payload, int bytes, int profile )
{
    const struct rocketnet_message_limits_t limits = rocketnet_fuzz_message_limits( profile );

    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    rocketnet_fuzz_require( violation >= ROCKETNET_MESSAGE_VIOLATION_NONE && violation < ROCKETNET_MESSAGE_NUM_VIOLATIONS, "one named violation" );
    rocketnet_fuzz_require( violation != ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL, "the payload checks never name the CONFIG body's violation" );
    rocketnet_fuzz_require( rocketnet_message_check_payload( payload, bytes, &limits ) == violation, "begin and check_payload agree" );

    const int violation_type = rocketnet_fuzz_message_violation_type( violation );
    rocketnet_fuzz_require( violation_type >= 0, "a named violation has a type" );
    rocketnet_fuzz_message_counters[violation_type]++;

    std::vector<rocketnet_message_record_t> records;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        records.push_back( record );
        rocketnet_fuzz_require( records.size() <= (size_t) bytes, "next terminates" );
        const int code_type = rocketnet_fuzz_message_code_type( record.code );
        rocketnet_fuzz_require( code_type >= 0, "a record carries a code the wire knows" );
        rocketnet_fuzz_message_counters[code_type]++;
    }

    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        rocketnet_fuzz_require( records.empty(), "a refused payload hands out no record" );
        return;
    }

    rocketnet_fuzz_require( !records.empty(), "an accepted payload holds at least one record" );
    const uint8_t form = payload[bytes - 1];

    // whole, inside the body, tiling it, and each one a correct sender could write

    const uint8_t * cursor = payload;
    for ( size_t i = 0; i < records.size(); i++ )
    {
        const struct rocketnet_message_record_t & r = records[i];
        rocketnet_fuzz_require( r.bytes >= 1, "a record has data" );
        rocketnet_fuzz_require( r.data >= payload && r.data + r.bytes <= payload + bytes - 1, "a record lies inside the body" );
        if ( form == ROCKETNET_MESSAGE_FORM_RECORDS )
        {
            rocketnet_fuzz_require( rocketnet_message_record_valid( &r ), "an accepted record is a valid record" );
            rocketnet_fuzz_require( r.data == cursor + rocketnet_message_record_header_bytes( &r ), "records follow one another" );
            cursor = r.data + r.bytes;
            if ( r.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE && r.code != ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED )
                rocketnet_fuzz_require( bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, "reliable records only in unfragmented payloads" );
        }
        else
        {
            rocketnet_fuzz_require( records.size() == 1 && r.data == payload && r.bytes == bytes - 1, "SINGLE is the payload, in place" );
            cursor = payload + bytes - 1;
        }
    }
    rocketnet_fuzz_require( cursor == payload + bytes - 1, "the records tile the body" );

    // pack back: the same bytes

    std::vector<uint8_t> repacked( (size_t) bytes );
    if ( form == ROCKETNET_MESSAGE_FORM_SINGLE )
    {
        memcpy( repacked.data(), records[0].data, (size_t) records[0].bytes );
        rocketnet_fuzz_require( rocketnet_message_single_finish( repacked.data(), bytes, records[0].bytes ) == bytes, "SINGLE repacks" );
    }
    else
    {
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, repacked.data(), bytes );
        for ( size_t i = 0; i < records.size(); i++ )
            rocketnet_fuzz_require( rocketnet_message_writer_add( &writer, &records[i] ) == 1, "every record repacks" );
        rocketnet_fuzz_require( rocketnet_message_writer_finish( &writer ) == bytes, "RECORDS repacks to its length" );
    }
    rocketnet_fuzz_require( memcmp( repacked.data(), payload, (size_t) bytes ) == 0, "pack(parse(x)) == x" );
}

static void rocketnet_fuzz_message_config( const uint8_t * data, int bytes )
{
    struct rocketnet_message_config_body_t body;
    const int violation = rocketnet_message_read_config_body( data, bytes, &body );
    rocketnet_fuzz_require( violation == ROCKETNET_MESSAGE_VIOLATION_NONE || violation == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL, "the CONFIG body names only its own violation" );
    const int violation_type = rocketnet_fuzz_message_violation_type( violation );
    rocketnet_fuzz_require( violation_type >= 0, "a named violation has a type" );
    rocketnet_fuzz_message_counters[violation_type]++;
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return;
    rocketnet_fuzz_require( bytes == ROCKETNET_MESSAGE_CONFIG_BYTES, "an accepted CONFIG body is 28 bytes" );
    if ( body.version == ROCKETNET_MESSAGE_CONFIG_VERSION )
    {
        uint8_t written[ROCKETNET_MESSAGE_CONFIG_BYTES];
        rocketnet_message_write_config_body( written, &body );
        rocketnet_fuzz_require( memcmp( written, data, ROCKETNET_MESSAGE_CONFIG_BYTES ) == 0, "a version 1 CONFIG body writes back to itself" );
    }
}

// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>
//
// One named seed per type: one per violation the decoder names, and one
// per code a record carries. Each seed is built from the wire's own
// layout -- the flags byte, the length, the fields the flags name, the
// data -- and every seed is replayed through the target before it is
// written. The generator refuses to finish unless every type the parser
// reads has a seed that raises that type's counter.
//
// The seeds are deterministic, so a regenerated corpus differs only when
// the code under it changed.
//
// ROCKETNET_FUZZ_SEED_SKIP names the types to withhold, comma separated.
// It is how the INCOMPLETE path is shown: a type whose seed is withheld
// has no seed that reaches it, so the generator exits 1 and says so.
// -----------------------------------------------------------------------

#define ROCKETNET_FUZZ_MESSAGE_SEED_MAX 4096

// one record: the flags byte, the length, then the fields the flags name
// in the standard's order (number, ring, sequencing, ordering, channel,
// and the block's fragment and fragments), then the data

static int rocketnet_fuzz_message_record( uint8_t * out, int at, uint8_t flags, int code, int length,
                                          uint16_t channel, uint16_t ring, uint16_t fragment, uint16_t fragments )
{
    const bool block = ( flags & ROCKETNET_MESSAGE_FLAG_BLOCK ) != 0;
    const bool wide = ( flags & ROCKETNET_MESSAGE_FLAG_WIDE ) != 0;
    int p = at;
    out[p++] = flags;
    out[p++] = (uint8_t) ( length & 0xFF );
    out[p++] = (uint8_t) ( ( length >> 8 ) & 0xFF );
    if ( code == 2 || code == 3 || code == 4 )
    {
        out[p++] = 1; out[p++] = 0; out[p++] = 0;                    // the number
    }
    if ( code == 3 || code == 4 || block )
    {
        out[p++] = (uint8_t) ( ring & 0xFF );
        out[p++] = (uint8_t) ( ring >> 8 );
    }
    if ( code == 1 || code == 4 )
    {
        out[p++] = 2; out[p++] = 0; out[p++] = 0;                    // the sequencing
    }
    if ( code == 1 || code == 3 || code == 4 )
    {
        out[p++] = 3; out[p++] = 0; out[p++] = 0;                    // the ordering
        if ( wide )
        {
            out[p++] = (uint8_t) ( channel & 0xFF );
            out[p++] = (uint8_t) ( channel >> 8 );
        }
        else
            out[p++] = (uint8_t) channel;
    }
    if ( block )
    {
        out[p++] = (uint8_t) ( fragment & 0xFF );
        out[p++] = (uint8_t) ( fragment >> 8 );
        out[p++] = (uint8_t) ( fragments & 0xFF );
        out[p++] = (uint8_t) ( fragments >> 8 );
    }
    for ( int i = 0; i < length; i++ )
        out[p++] = 0xAA;
    return p;
}

static int rocketnet_fuzz_message_seed_build( int type, uint8_t * out, int capacity )
{
    if ( capacity < ROCKETNET_FUZZ_MESSAGE_SEED_MAX )
        return 0;

    switch ( type )
    {
        // an accepted payload: one record, in the RECORDS form
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_NONE:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x00, 0, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // the form byte is not one the decoder knows
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_FORM:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x00, 0, 1, 0, 0, 0, 0 );
            out[p++] = 0x02;
            return p;
        }

        // the body holds less than a record's fixed part
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_TRUNCATED:
        {
            out[0] = 0;
            out[1] = 0x00;
            out[2] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return 3;
        }

        // a reserved flag bit
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_FLAGS:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x40, 0, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a record that claims no data
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_LENGTH:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x00, 0, 0, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a channel past the first version's count
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_CHANNEL:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x03, 3, 1, 32, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a code the first version does not enable
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_TYPE_NOT_ENABLED:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x01, 1, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a block of one fragment
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_BLOCK:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x0B, 3, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a ring past the receiver's buffer
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_RING:
        {
            out[0] = 0;
            int p = rocketnet_fuzz_message_record( out, 1, 0x03, 3, 1, 0, 0xFFFF, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a record longer than this end's max_message_bytes
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_SIZE:
        {
            out[0] = 2;
            int p = rocketnet_fuzz_message_record( out, 1, 0x02, 2, 3000, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }

        // a CONFIG body of another kind
        case ROCKETNET_FUZZ_MESSAGE_VIOLATION_BAD_CONTROL:
        {
            out[0] = 3;
            memset( out + 1, 0, ROCKETNET_MESSAGE_CONFIG_BYTES );
            out[1] = 0x02;
            return 1 + ROCKETNET_MESSAGE_CONFIG_BYTES;
        }

        // one record of each code, in the profile that enables them all
        case ROCKETNET_FUZZ_MESSAGE_CODE_UNRELIABLE:
        {
            out[0] = 1;
            int p = rocketnet_fuzz_message_record( out, 1, 0x00, 0, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }
        case ROCKETNET_FUZZ_MESSAGE_CODE_UNRELIABLE_SEQUENCED:
        {
            out[0] = 1;
            int p = rocketnet_fuzz_message_record( out, 1, 0x01, 1, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }
        case ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE:
        {
            out[0] = 1;
            int p = rocketnet_fuzz_message_record( out, 1, 0x02, 2, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }
        case ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE_ORDERED:
        {
            out[0] = 1;
            int p = rocketnet_fuzz_message_record( out, 1, 0x03, 3, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }
        case ROCKETNET_FUZZ_MESSAGE_CODE_RELIABLE_SEQUENCED:
        {
            out[0] = 1;
            int p = rocketnet_fuzz_message_record( out, 1, 0x04, 4, 1, 0, 0, 0, 0 );
            out[p++] = ROCKETNET_MESSAGE_FORM_RECORDS;
            return p;
        }
        default:
            break;
    }
    return 0;
}

static bool rocketnet_fuzz_message_type_withheld( const char * list, const char * name )
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

static bool rocketnet_fuzz_message_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
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
        fprintf( stderr, "seed generator: cannot write %s\n", path );
    return ok;
}

static bool rocketnet_fuzz_message_generate( const char * dir )
{
    char skip[256];
    skip[0] = '\0';
    rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_SKIP", skip, sizeof( skip ) );

    static uint8_t seed[ROCKETNET_FUZZ_MESSAGE_SEED_MAX];
    bool complete = true;
    for ( int type = 0; type < ROCKETNET_FUZZ_MESSAGE_TYPE_COUNT; type++ )
    {
        const char * name = rocketnet_fuzz_message_types[type].name;
        if ( rocketnet_fuzz_message_type_withheld( skip, name ) )
        {
            fprintf( stderr, "seed generator: withholding %s\n", name );
            complete = false;
            continue;
        }

        const int bytes = rocketnet_fuzz_message_seed_build( type, seed, (int) sizeof( seed ) );
        if ( bytes <= 0 )
        {
            fprintf( stderr, "seed generator: no seed for %s\n", name );
            complete = false;
            continue;
        }

        // the seed is replayed through the target before it is written:
        // a seed that reaches no counter is no seed at all
        const uint64_t before = rocketnet_fuzz_message_counters[type];
        rocketnet_fuzz_message_run( seed, (size_t) bytes );
        if ( rocketnet_fuzz_message_counters[type] <= before )
        {
            fprintf( stderr, "seed generator: the %s seed reaches no %s\n", name, name );
            complete = false;
            continue;
        }

        if ( !rocketnet_fuzz_message_seed_write( dir, name, seed, bytes ) )
            complete = false;
    }

    printf( "seed generator: %s\n", complete ? "every type the parser reads has a seed that reaches it" : "INCOMPLETE" );
    return complete;
}

// -----------------------------------------------------------------------

static void rocketnet_fuzz_message_run( const uint8_t * data, size_t size )
{
    if ( size < 1 || size > 1 << 20 )
        return;

    const uint8_t * body = data + 1;
    const int body_size = (int) ( size - 1 );

    const int selector = data[0] & 3;
    if ( selector == 3 )
        rocketnet_fuzz_message_config( body, body_size );
    else
        rocketnet_fuzz_message_payload( body, body_size, selector );
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    // the codec allocates nothing and touches nothing else: no rocketnet_init

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    char seed_dir[1024];
    const int have_dir = rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) );
    if ( have_dir && seed_dir[0] != '\0' )
        exit( rocketnet_fuzz_message_generate( seed_dir ) ? 0 : 1 );

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    rocketnet_fuzz_message_run( data, size );
    return 0;
}
