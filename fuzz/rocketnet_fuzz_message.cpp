/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message record fuzz harness (docs/CHANNELS.md layer 2). It drives the
// decoder of sdk/rocketnet_message.cpp -- the first code above the transport
// that reads bytes a peer wrote -- and checks the layer's promises as an
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

    std::vector<rocketnet_message_record_t> records;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        records.push_back( record );
        rocketnet_fuzz_require( records.size() <= (size_t) bytes, "next terminates" );
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

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    // the codec allocates nothing and touches nothing else: no rocketnet_init

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 1 || size > 1 << 20 )
        return 0;

    const uint8_t * body = data + 1;
    const int body_size = (int) ( size - 1 );

    const int selector = data[0] & 3;
    if ( selector == 3 )
        rocketnet_fuzz_message_config( body, body_size );
    else
        rocketnet_fuzz_message_payload( body, body_size, selector );

    return 0;
}
