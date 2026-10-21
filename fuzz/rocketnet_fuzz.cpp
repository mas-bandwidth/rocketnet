/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The wire-facing parser fuzz harness (#111). One libFuzzer binary covering
// the four surfaces that read bytes rocketnet did not write -- the packet
// prefilter, the fragment header and its reassembly, base64 decoding, and
// the config value parses -- because one binary is one compile of the sdk,
// and the per-commit budget is minutes.
//
// THE SELECTOR BYTE: input[0] & 3 chooses the surface, and the rest of the
// input is that surface's bytes. 0 = prefilter, 1 = fragment, 2 = base64,
// 3 = config. The committed corpus under fuzz/corpus is named for the
// surface each file selects, so a regression input stays readable.
//
// PER-INPUT DETERMINISM IS A REQUIREMENT HERE, not a nicety: the per-commit
// gate is a corpus REPLAY, and a replay is only a gate if input N behaves
// the same whether it is replayed alone or after input N-1. So no state
// survives a call: the reassembly set -- the one surface with memory -- is
// reset at the top of every input, and the multi-fragment sequences that
// reassembly needs are built INSIDE one input from its length-prefixed
// records rather than across inputs.
//
// THE INPUT BOUNDS ARE THE TRANSPORT'S OWN. A parser is only obliged to
// survive what the transport can hand it, and the transport never hands a
// reassembly more than the receive ceiling allows. Fuzzing past that
// contract manufactures crashes that no packet can cause, so each surface
// caps its slice where the transport caps it, and the cap is named.

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_fragment.h"
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

static const uint8_t rocketnet_fuzz_filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition ) return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// the counter table the reassembly engine fires into: the server's family,
// the same one the suite's fragment tests use

static const struct rocketnet_reassembly_counters_t rocketnet_fuzz_fragment_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

static struct rocketnet_reassembly_set_t * rocketnet_fuzz_reassembly = NULL;

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
        bytes = ROCKETNET_RECEIVE_CEILING + 1;

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
            abort();
    }
    else
    {
        // an accepted packet's type is a byte, and the prefilter is
        // deliberately type-blind: anything outside 0..255 is impossible
        if ( result > 255 )
            abort();
    }
}

// -----------------------------------------------------------------------
//  1: the fragment header and reassembly
// -----------------------------------------------------------------------

static void rocketnet_fuzz_fragment( const uint8_t * data, size_t bytes )
{
    if ( rocketnet_fuzz_reassembly == NULL )
        return;

    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof( counters_storage ) );

    // no state crosses an input: both connections start empty and the
    // delivery window opens here

    rocketnet_reassembly_set_reset_connection( rocketnet_fuzz_reassembly, 0 );
    rocketnet_reassembly_set_reset_connection( rocketnet_fuzz_reassembly, 1 );
    rocketnet_reassembly_set_begin_update( rocketnet_fuzz_reassembly );

    // the input is a sequence of records: one length byte pair, then that
    // many bytes of fragment type data, delivered alternately to the two
    // connections. a payload takes at least two fragments, so one record
    // per input would never reach the completion path.

    const int max_type_data = ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;

    size_t offset = 0;
    int connection = 0;

    while ( offset + 2 <= bytes )
    {
        size_t record = (size_t) data[offset] | ( (size_t) data[offset + 1] << 8 );
        offset += 2;

        if ( record > bytes - offset )
            record = bytes - offset;

        // the transport hands the engine what came out of one seal, and a
        // seal is bounded by the receive ceiling. THE FLOOR IS THE READING
        // ORDER'S: the engine documents -- and asserts -- that its caller
        // has already applied the standard's opened-size check, so a
        // record below the header plus one byte is not an input the
        // transport can produce, and feeding it here would manufacture a
        // crash no packet can cause.
        int record_bytes = (int) record;
        if ( record_bytes > max_type_data )
            record_bytes = max_type_data;

        if ( record_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1 )
        {
            std::unique_ptr<uint8_t[]> fragment( new uint8_t[record_bytes] );
            memcpy( fragment.get(), data + offset, record_bytes );
            rocketnet_reassembly_process_fragment( rocketnet_fuzz_reassembly,
                                                   connection,
                                                   fragment.get(),
                                                   record_bytes,
                                                   counters_storage.values,
                                                   &rocketnet_fuzz_fragment_counters );
        }

        offset += record;
        connection = connection ? 0 : 1;
    }

    // the header reader on its own, and the split plan the sender computes
    // from a payload size the caller chose

    if ( bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES )
    {
        uint8_t header[ROCKETNET_FRAGMENT_HEADER_BYTES];
        memcpy( header, data, sizeof(header) );
        uint32_t payload_id = 0;
        int fragment_index = 0;
        int fragment_count = 0;
        rocketnet_read_fragment_header( header, &payload_id, &fragment_index, &fragment_count );

        int plan_count = 0;
        int plan_stride = 0;
        const int payload_bytes = (int) ( payload_id % ( ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 4096 ) );
        if ( rocketnet_fragment_plan( payload_bytes, &plan_count, &plan_stride ) )
        {
            // a plan is a promise about the split: every fragment but the
            // last is exactly the stride, the last is between one byte and
            // the stride, and the pieces sum to the payload
            if ( plan_count < 2 || plan_count > ROCKETNET_MAX_FRAGMENTS )
                abort();
            if ( plan_stride < 1 || plan_stride > ROCKETNET_MAX_FRAGMENT_BYTES )
                abort();
            const int last = payload_bytes - plan_stride * ( plan_count - 1 );
            if ( last < 1 || last > plan_stride )
                abort();
        }
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
        bytes = 4096;

    std::unique_ptr<uint8_t[]> input( new uint8_t[bytes] );
    memcpy( input.get(), data, bytes );
    data = input.get();

    char text[4097];
    memcpy( text, data, bytes );
    text[bytes] = '\0';

    uint8_t decoded[4097];
    const int wide_bytes = rocketnet_base64_decode_data( text, decoded, sizeof( decoded ) );

    char decoded_string[4097];
    rocketnet_base64_decode_string( text, decoded_string, sizeof( decoded_string ) );

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
        abort();
    if ( wide_bytes > (int) sizeof( key_field ) )
        rocketnet_fuzz_require( key_bytes == 0, "base64 overlong field must refuse" );
    if ( wide_bytes > 0 && wide_bytes <= (int) sizeof( key_field ) )
        rocketnet_fuzz_require( key_bytes == wide_bytes && memcmp( key_field, decoded, wide_bytes ) == 0,
                               "base64 fitting field must preserve all bytes" );

    char small_string[8];
    const int small_string_bytes = rocketnet_base64_decode_string( text, small_string, sizeof( small_string ) );
    if ( wide_bytes >= (int) sizeof( small_string ) )
        rocketnet_fuzz_require( small_string_bytes == 0, "base64 string must reserve its terminator" );
    if ( wide_bytes > 0 && wide_bytes < (int) sizeof( small_string ) )
        rocketnet_fuzz_require( small_string_bytes == wide_bytes && memcmp( small_string, decoded, wide_bytes ) == 0 && small_string[wide_bytes] == '\0',
                               "base64 fitting string must preserve bytes and terminator" );

    // ONE SPELLING PER KEY, as an oracle rather than a table of examples:
    // whatever these bytes are, encoding then decoding them must return
    // exactly them. A decoder that accepted a second spelling would still
    // pass this; a decoder that lost a byte could not.

    char encoded[8192];
    const int encoded_length = rocketnet_base64_encode_data( data, bytes, encoded, sizeof( encoded ) );
    char small_encoded[8];
    const int small_encoded_length = rocketnet_base64_encode_data( data, bytes, small_encoded, sizeof( small_encoded ) );
    if ( encoded_length >= (int) sizeof( small_encoded ) )
        rocketnet_fuzz_require( small_encoded_length == -1, "base64 encode must refuse insufficient capacity" );
    if ( encoded_length > 0 && encoded_length < (int) sizeof( small_encoded ) )
        rocketnet_fuzz_require( small_encoded_length == encoded_length && memcmp( small_encoded, encoded, encoded_length + 1 ) == 0,
                               "base64 fitting encode must preserve bytes and terminator" );
    if ( encoded_length > 0 )
    {
        uint8_t round_trip[4097];
        const int decoded_length = rocketnet_base64_decode_data( encoded, round_trip, sizeof( round_trip ) );
        if ( decoded_length != (int) bytes )
            abort();
        if ( bytes > 0 && memcmp( round_trip, data, bytes ) != 0 )
            abort();
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
        bytes = 1024;

    char value[1025];
    memcpy( value, data, bytes );
    value[bytes] = '\0';

    int parsed = 0;
    rocketnet_config_parse_int_value( value, &parsed );

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );

    struct rocketnet_transport_ladder_t ladder;
    memset( &ladder, 0, sizeof( ladder ) );

    if ( rocketnet_transport_ladder_parse( value, available, num_available, &ladder ) )
    {
        // a ladder that parsed is a ladder the transport will walk: every
        // candidate must be a real transport index and the list must fit the
        // array it was written into
        if ( ladder.num_candidates < 1 || ladder.num_candidates > ROCKETNET_MAX_TRANSPORT_CANDIDATES )
            abort();
        if ( ladder.num_skipped < 0 || ladder.num_skipped > ROCKETNET_MAX_TRANSPORT_CANDIDATES )
            abort();
        for ( int i = 0; i < ladder.num_candidates; i++ )
        {
            if ( ladder.candidates[i] < 0 || ladder.candidates[i] >= ROCKETNET_NUM_TRANSPORTS )
                abort();
            if ( rocketnet_transport_name( ladder.candidates[i] ) == NULL )
                abort();
        }
    }

    char env_name[256];
    rocketnet_config_env_var_name( value, env_name, sizeof( env_name ) );
}

// -----------------------------------------------------------------------

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    // the parsers under test allocate through rocketnet_malloc and touch
    // nothing else: no rocketnet_init, so no platform threads and no
    // sodium, and the harness stays as deterministic as the parsers are

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    rocketnet_fuzz_reassembly = rocketnet_reassembly_set_create( NULL, 2, 4, 8192, 2 * 4 );
    if ( rocketnet_fuzz_reassembly == NULL )
        abort();

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;

    const uint8_t * body = data + 1;
    const size_t body_size = size - 1;

    switch ( data[0] & 3 )
    {
        case 0: rocketnet_fuzz_prefilter( body, body_size ); break;
        case 1: rocketnet_fuzz_fragment( body, body_size );  break;
        case 2: rocketnet_fuzz_base64( body, body_size );    break;
        case 3: rocketnet_fuzz_config( body, body_size );    break;
    }

    return 0;
}
