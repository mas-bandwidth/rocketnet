/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The message layer's unit tests (docs/CHANNELS.md), one layer at a time.
    Layer 2, message records: the known-answer vectors of STANDARD.md
    _Messages_ both ways, one input per violation hitting exactly that
    violation, the zero-copy SINGLE form in place at a 16-aligned payload
    root, and the property test over pack and parse -- encode then decode is
    the identity, a parse that succeeds packs back to the same bytes, and no
    parse reads outside its payload, over every prefix and every one-byte
    mutation of each generated payload.

    These tests run inside ./bin/test. Each is registered under its suite
    name (test_*, the shape tools/check_test_environment.py accepts) and an
    alias, the name a card's TEST line uses: ./bin/test <alias> runs that
    one test alone.
*/

#include "rocketnet.h"
#include "rocketnet_message.h"
#include "rocketnet_read_write.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define channels_check( condition )                                                         \
do                                                                                          \
{                                                                                           \
    if ( !(condition) )                                                                     \
    {                                                                                       \
        printf( "check failed: ( %s ), function %s, file %s, line %d\n",                    \
            #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );     \
        fflush( stdout );                                                                   \
        exit( 1 );                                                                          \
    }                                                                                       \
} while(0)

// a fixed-seed generator: every run, and every replay of a failure, is the same run

struct channels_random_t
{
    uint64_t state;
};

static uint32_t channels_random( channels_random_t * random )
{
    uint64_t x = random->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    random->state = x;
    return (uint32_t) ( x >> 32 );
}

static int channels_random_range( channels_random_t * random, int lo, int hi )
{
    return lo + (int) ( channels_random( random ) % (uint32_t) ( hi - lo + 1 ) );
}

static int channels_round16( int bytes )
{
    return ( bytes + 15 ) & ~15;
}

static struct rocketnet_message_limits_t channels_limits( int num_channels, uint32_t enabled_codes )
{
    struct rocketnet_message_limits_t limits;
    limits.num_channels = num_channels;
    limits.enabled_codes = enabled_codes;
    limits.window_messages = 1024;
    limits.max_message_bytes = 32768;
    limits.receive_ring_bytes = 65536;
    return limits;
}

static struct rocketnet_message_record_t channels_record( int code, const uint8_t * data, int bytes )
{
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = code;
    record.data = data;
    record.bytes = bytes;
    return record;
}

static bool channels_record_equal( const struct rocketnet_message_record_t & a, const struct rocketnet_message_record_t & b )
{
    return a.code == b.code && ( a.block != 0 ) == ( b.block != 0 ) && ( a.control != 0 ) == ( b.control != 0 )
        && a.number == b.number && a.ring == b.ring && a.sequencing == b.sequencing && a.ordering == b.ordering
        && a.channel == b.channel && a.fragment == b.fragment && a.fragments == b.fragments
        && a.bytes == b.bytes && memcmp( a.data, b.data, (size_t) a.bytes ) == 0;
}

static int channels_parse_all( const uint8_t * payload, int bytes, const struct rocketnet_message_limits_t * limits,
                               std::vector<rocketnet_message_record_t> & records )
{
    records.clear();
    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, limits );
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
        records.push_back( record );
    if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        channels_check( records.empty() );
    return violation;
}

// -----------------------------------------
//                  the known-answer vectors
// -----------------------------------------

static void channels_check_vector( const uint8_t * expected, int expected_bytes,
                                   const struct rocketnet_message_record_t * records, int num_records,
                                   const struct rocketnet_message_limits_t * limits )
{
    uint8_t payload[2048];
    memset( payload, 0xCD, sizeof( payload ) );
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PAYLOAD_BYTES );
    for ( int i = 0; i < num_records; i++ )
        channels_check( rocketnet_message_writer_add( &writer, &records[i] ) == 1 );
    channels_check( rocketnet_message_writer_finish( &writer ) == expected_bytes );
    channels_check( memcmp( payload, expected, (size_t) expected_bytes ) == 0 );

    std::vector<uint8_t> exact( expected, expected + expected_bytes );
    std::vector<rocketnet_message_record_t> parsed;
    channels_check( channels_parse_all( exact.data(), expected_bytes, limits, parsed ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    channels_check( (int) parsed.size() == num_records );
    for ( int i = 0; i < num_records; i++ )
        channels_check( channels_record_equal( parsed[i], records[i] ) );
}

static void channels_test_vectors()
{
    const struct rocketnet_message_limits_t limits = channels_limits( 1024, rocketnet_message_enabled_codes( 0x09 ) );
    static const uint8_t hi[2] = { 0x68, 0x69 };

    // SINGLE, message "hi"

    {
        static const uint8_t expected[] = { 0x68, 0x69, 0x00 };
        uint8_t payload[16];
        memcpy( payload, hi, 2 );
        channels_check( rocketnet_message_single_finish( payload, sizeof( payload ), 2 ) == 3 );
        channels_check( memcmp( payload, expected, 3 ) == 0 );
        std::vector<rocketnet_message_record_t> parsed;
        channels_check( channels_parse_all( expected, 3, &limits, parsed ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        channels_check( parsed.size() == 1 );
        channels_check( parsed[0].code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && parsed[0].bytes == 2 && parsed[0].data == expected );
    }

    struct rocketnet_message_record_t unreliable = channels_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, hi, 2 );

    struct rocketnet_message_record_t ordered = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, hi, 2 );
    ordered.number = 5;
    ordered.ring = 0;
    ordered.ordering = 0;
    ordered.channel = 3;

    // RECORDS, one UNRELIABLE "hi"

    {
        static const uint8_t expected[] = { 0x00, 0x02, 0x00, 0x68, 0x69, 0x01 };
        channels_check_vector( expected, sizeof( expected ), &unreliable, 1, &limits );
    }

    // RECORDS, RELIABLE_ORDERED "hi", number 5, ring 0, ordering 0, channel 3

    {
        static const uint8_t expected[] = { 0x03, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x68, 0x69, 0x01 };
        channels_check_vector( expected, sizeof( expected ), &ordered, 1, &limits );
    }

    // RECORDS, the two records above in one packet

    {
        static const uint8_t expected[] = { 0x00, 0x02, 0x00, 0x68, 0x69,
                                             0x03, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x68, 0x69, 0x01 };
        struct rocketnet_message_record_t both[2] = { unreliable, ordered };
        channels_check_vector( expected, sizeof( expected ), both, 2, &limits );
    }

    // RECORDS, fragment 1 of 3 of a RELIABLE_ORDERED block, number 9, ring 72,
    // ordering 7, channel 300 (WIDE), 1152 data bytes

    {
        static uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
        for ( int i = 0; i < ROCKETNET_MESSAGE_FRAGMENT_BYTES; i++ )
            data[i] = (uint8_t) ( i * 7 + 1 );
        static const uint8_t header[] = { 0x1b, 0x80, 0x04, 0x09, 0x00, 0x00, 0x48, 0x00, 0x07, 0x00, 0x00, 0x2c, 0x01, 0x01, 0x00, 0x02, 0x00 };
        std::vector<uint8_t> expected( sizeof( header ) + ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1 );
        memcpy( expected.data(), header, sizeof( header ) );
        memcpy( expected.data() + sizeof( header ), data, ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        expected.back() = 0x01;
        struct rocketnet_message_record_t fragment = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, data, ROCKETNET_MESSAGE_FRAGMENT_BYTES );
        fragment.block = 1;
        fragment.number = 9;
        fragment.ring = 72;
        fragment.ordering = 7;
        fragment.channel = 300;
        fragment.fragment = 1;
        fragment.fragments = 2;
        channels_check( rocketnet_message_record_header_bytes( &fragment ) == 17 );
        channels_check_vector( expected.data(), (int) expected.size(), &fragment, 1, &limits );
    }

    // CONFIG, defaults, number 0

    {
        static const uint8_t expected[] = { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x09, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
                                            0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
        struct rocketnet_message_config_body_t body;
        body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
        body.enabled = 0x09;
        body.num_channels = 32;
        body.window_messages = 1024;
        body.receive_ring_bytes = 65536;
        body.max_message_bytes = 32768;
        body.fragment_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        uint8_t config[ROCKETNET_MESSAGE_CONFIG_BYTES];
        rocketnet_message_write_config_body( config, &body );
        struct rocketnet_message_record_t record = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE, config, ROCKETNET_MESSAGE_CONFIG_BYTES );
        record.control = 1;
        channels_check_vector( expected, sizeof( expected ), &record, 1, &limits );

        struct rocketnet_message_config_body_t read;
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &read ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        channels_check( memcmp( &read, &body, sizeof( body ) ) == 0 );
    }
}

// -----------------------------------------
//          one input per violation, exactly
// -----------------------------------------

static void channels_check_violation( const std::vector<uint8_t> & payload, const struct rocketnet_message_limits_t * limits, int expected )
{
    std::vector<uint8_t> exact( payload );
    std::vector<rocketnet_message_record_t> parsed;
    const int violation = channels_parse_all( exact.data(), (int) exact.size(), limits, parsed );
    if ( violation != expected )
        printf( "expected violation %s, got %s\n", rocketnet_message_violation_name( expected ), rocketnet_message_violation_name( violation ) );
    channels_check( violation == expected );
    channels_check( rocketnet_message_check_payload( exact.data(), (int) exact.size(), limits ) == expected );
}

static std::vector<uint8_t> channels_bytes( std::initializer_list<int> bytes )
{
    std::vector<uint8_t> result;
    for ( int b : bytes )
        result.push_back( (uint8_t) b );
    return result;
}

static void channels_test_violations()
{
    const struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 0x09 ) );
    const struct rocketnet_message_limits_t all = channels_limits( 32, 0x1F );

    // 1. bad form: too short, or a form byte other than 0 and 1
    channels_check_violation( channels_bytes( { 0x00 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FORM );
    channels_check_violation( channels_bytes( { 0x68, 0x02 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FORM );
    channels_check_violation( channels_bytes( { 0x68, 0xFF } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FORM );

    // 2. truncated: under 3 bytes left, or the header's fields past the body
    channels_check_violation( channels_bytes( { 0x00, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_TRUNCATED );
    channels_check_violation( channels_bytes( { 0x00, 0x01, 0x00, 0x41, 0x00, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_TRUNCATED );
    channels_check_violation( channels_bytes( { 0x03, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_TRUNCATED );

    // 3. bad flags: codes 5 to 7, reserved bits, BLOCK on 0 or 1, WIDE on 0 or 2, CONTROL off code 2 or with BLOCK
    channels_check_violation( channels_bytes( { 0x05, 0x01, 0x00, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x40, 0x01, 0x00, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x08, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x10, 0x01, 0x00, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x12, 0x01, 0x00, 0, 0, 0, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x23, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );
    channels_check_violation( channels_bytes( { 0x2A, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );

    // 4. bad length: zero, or past the body
    channels_check_violation( channels_bytes( { 0x00, 0x00, 0x00, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH );
    channels_check_violation( channels_bytes( { 0x00, 0x02, 0x00, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_LENGTH );

    // 5. bad channel: at or over the runtime count, or WIDE below 256
    channels_check_violation( channels_bytes( { 0x03, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
    {
        const struct rocketnet_message_limits_t wide = channels_limits( 1024, 0x1F );
        channels_check_violation( channels_bytes( { 0x13, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x00, 0x41, 0x01 } ), &wide, ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
        channels_check_violation( channels_bytes( { 0x13, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x04, 0x41, 0x01 } ), &wide, ROCKETNET_MESSAGE_VIOLATION_BAD_CHANNEL );
    }

    // 6. type not enabled: code 2 as application data under the {0, 3} mask; CONTROL is exempt
    channels_check_violation( channels_bytes( { 0x02, 0x01, 0x00, 0, 0, 0, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED );
    channels_check_violation( channels_bytes( { 0x41, 0x00 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_NONE );
    {
        const struct rocketnet_message_limits_t ordered_only = channels_limits( 32, 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
        channels_check_violation( channels_bytes( { 0x41, 0x00 } ), &ordered_only, ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED );
    }

    // 7. bad block: a count of one, an index past the count, more fragments than W or than the
    // largest message, a short non-last fragment, a long last fragment
    {
        // code 2 BLOCK: flags 0x0A, length, number 3, ring 2, fragment 2, fragments 2
        std::vector<uint8_t> one = channels_bytes( { 0x0A, 0x01, 0x00, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x00, 0x41, 0x01 } );
        channels_check_violation( one, &all, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        std::vector<uint8_t> past = channels_bytes( { 0x0A, 0x01, 0x00, 0, 0, 0, 0, 0, 0x02, 0x00, 0x01, 0x00, 0x41, 0x01 } );
        channels_check_violation( past, &all, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        struct rocketnet_message_limits_t narrow = all;
        narrow.window_messages = 2;
        // the last of three fragments, at ring 144 so its block starts at 0
        std::vector<uint8_t> over_window = channels_bytes( { 0x0A, 0x01, 0x00, 0, 0, 0, 0x90, 0x00, 0x02, 0x00, 0x02, 0x00, 0x41, 0x01 } );
        channels_check_violation( over_window, &narrow, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        struct rocketnet_message_limits_t small = all;
        small.max_message_bytes = 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        channels_check_violation( over_window, &small, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        small.max_message_bytes = 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
        channels_check_violation( over_window, &small, ROCKETNET_MESSAGE_VIOLATION_NONE );
        std::vector<uint8_t> short_fragment = channels_bytes( { 0x0A, 0x01, 0x00, 0, 0, 0, 0, 0, 0x00, 0x00, 0x01, 0x00, 0x41, 0x01 } );
        channels_check_violation( short_fragment, &all, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
        std::vector<uint8_t> long_last = channels_bytes( { 0x0A, 0x81, 0x04, 0, 0, 0, 0, 0, 0x01, 0x00, 0x01, 0x00 } );
        long_last.insert( long_last.end(), ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1, 0x41 );
        long_last.push_back( 0x01 );
        channels_check_violation( long_last, &all, ROCKETNET_MESSAGE_VIOLATION_BAD_BLOCK );
    }

    // 8. bad ring: a span past B, and a block whose base would sit below the ring
    {
        // code 3, length 17 at ring unit 4095 of a 65536 ring: 65520 + 32 > 65536
        std::vector<uint8_t> past = channels_bytes( { 0x03, 0x11, 0x00, 0, 0, 0, 0xFF, 0x0F, 0, 0, 0, 0x00 } );
        past.insert( past.end(), 17, 0x41 );
        past.push_back( 0x01 );
        channels_check_violation( past, &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_RING );
        // the same at length 16 fits exactly
        std::vector<uint8_t> fits = channels_bytes( { 0x03, 0x10, 0x00, 0, 0, 0, 0xFF, 0x0F, 0, 0, 0, 0x00 } );
        fits.insert( fits.end(), 16, 0x41 );
        fits.push_back( 0x01 );
        channels_check_violation( fits, &limits, ROCKETNET_MESSAGE_VIOLATION_NONE );
        // fragment 1 at ring 71: its block would start one unit below 0
        std::vector<uint8_t> below = channels_bytes( { 0x0A, 0x01, 0x00, 0, 0, 0, 71, 0, 0x01, 0x00, 0x01, 0x00, 0x41, 0x01 } );
        channels_check_violation( below, &all, ROCKETNET_MESSAGE_VIOLATION_BAD_RING );
    }

    // 9. bad size: a reliable record in a payload over 1192 bytes; an unreliable one is fine there
    {
        std::vector<uint8_t> big = channels_bytes( { 0x03, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 } );
        const int length = ROCKETNET_MAX_PAYLOAD_BYTES - 12;
        big[1] = (uint8_t) ( length & 0xFF );
        big[2] = (uint8_t) ( length >> 8 );
        big.insert( big.end(), length, 0x41 );
        big.push_back( 0x01 );
        channels_check_violation( big, &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_SIZE );
        big.erase( big.begin() + 12 );
        big[1] = (uint8_t) ( ( length - 1 ) & 0xFF );
        channels_check_violation( big, &limits, ROCKETNET_MESSAGE_VIOLATION_NONE );
        std::vector<uint8_t> unreliable = channels_bytes( { 0x00, 0x00, 0x10 } );
        unreliable.insert( unreliable.end(), 0x1000, 0x41 );
        unreliable.push_back( 0x01 );
        channels_check_violation( unreliable, &limits, ROCKETNET_MESSAGE_VIOLATION_NONE );
    }

    // the first failure wins: a bad first record hides nothing, and records before it are never handed out
    channels_check_violation( channels_bytes( { 0x00, 0x01, 0x00, 0x41, 0x05, 0x01, 0x00, 0x41, 0x01 } ), &limits, ROCKETNET_MESSAGE_VIOLATION_BAD_FLAGS );

    // 12. bad control: a CONFIG body of the wrong length, an unknown kind, nonzero reserved bytes in version 1
    {
        struct rocketnet_message_config_body_t body;
        memset( &body, 0, sizeof( body ) );
        body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
        uint8_t config[ROCKETNET_MESSAGE_CONFIG_BYTES + 1];
        rocketnet_message_write_config_body( config, &body );
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES - 1, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL );
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES + 1, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL );
        config[0] = 2;
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL );
        config[0] = ROCKETNET_MESSAGE_CONTROL_CONFIG;
        config[25] = 1;
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_BAD_CONTROL );
        config[1] = 2;
        channels_check( rocketnet_message_read_config_body( config, ROCKETNET_MESSAGE_CONFIG_BYTES, &body ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        channels_check( body.version == 2 );
    }

    for ( int i = 0; i < ROCKETNET_MESSAGE_NUM_VIOLATIONS; i++ )
        channels_check( rocketnet_message_violation_name( i ) != NULL );
}

// -----------------------------------------
//                    SINGLE, zero copy
// -----------------------------------------

static void channels_test_single_zero_copy()
{
    // a frame with the payload root at a 16-aligned payload_index, as start_packet hands back

    alignas( 16 ) static uint8_t frame[64 + ROCKETNET_MAX_PAYLOAD_BYTES];
    const int payload_index = 32;
    uint8_t * payload = frame + payload_index;
    channels_check( ( (uintptr_t) payload & 15 ) == 0 );

    channels_check( rocketnet_message_single_capacity( ROCKETNET_MAX_PAYLOAD_BYTES ) == ROCKETNET_MESSAGE_SINGLE_BYTES );
    channels_check( rocketnet_message_single_capacity( 1 ) == 0 );
    channels_check( rocketnet_message_single_capacity( 0 ) == 0 );

    // the application writes a packet-sized message in place at the root
    for ( int i = 0; i < ROCKETNET_MESSAGE_SINGLE_BYTES; i++ )
        payload[i] = (uint8_t) ( i ^ 0x5A );
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MESSAGE_SINGLE_BYTES ) == ROCKETNET_MAX_PAYLOAD_BYTES );
    channels_check( payload[ROCKETNET_MESSAGE_SINGLE_BYTES] == ROCKETNET_MESSAGE_FORM_SINGLE );

    // and the receiver reads it where it lies: the record's data is the payload root itself
    const struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 0x09 ) );
    struct rocketnet_message_reader_t reader;
    channels_check( rocketnet_message_reader_begin( &reader, payload, ROCKETNET_MAX_PAYLOAD_BYTES, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    struct rocketnet_message_record_t record;
    channels_check( rocketnet_message_reader_next( &reader, &record ) == 1 );
    channels_check( record.data == payload );
    channels_check( record.bytes == ROCKETNET_MESSAGE_SINGLE_BYTES );
    channels_check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && !record.block && !record.control && record.channel == 0 );
    channels_check( rocketnet_message_reader_next( &reader, &record ) == 0 );

    // refusals: empty, and no room left for the form byte
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PAYLOAD_BYTES, 0 ) == 0 );
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MAX_PAYLOAD_BYTES ) == 0 );

    // a large SINGLE (the arena route) is legal: unreliable may ride fragments
    std::vector<uint8_t> large( 4097, 0x33 );
    channels_check( rocketnet_message_single_finish( large.data(), (int) large.size(), 4096 ) == 4097 );
    channels_check( rocketnet_message_check_payload( large.data(), (int) large.size(), &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    // SINGLE is UNRELIABLE: a receiver that does not admit code 0 refuses it
    const struct rocketnet_message_limits_t ordered_only = channels_limits( 32, 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    channels_check( rocketnet_message_check_payload( payload, ROCKETNET_MAX_PAYLOAD_BYTES, &ordered_only ) == ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED );
}

// -----------------------------------------
//             the property test, pack/parse
// -----------------------------------------

static const int channels_num_channels = 1024;

// a random record a correct sender could write under channels_limits( 1024, 0x1F ),
// with data from pool, at most max_bytes long including its header

static bool channels_random_record( channels_random_t * random, const uint8_t * pool, int max_bytes, bool reliable_allowed,
                                    struct rocketnet_message_record_t * record )
{
    memset( record, 0, sizeof( *record ) );
    record->code = channels_random_range( random, 0, reliable_allowed ? 4 : 1 );
    const bool has_channel = record->code == 1 || record->code == 3 || record->code == 4;
    if ( has_channel )
        record->channel = (uint32_t) ( channels_random_range( random, 0, 3 ) == 0 ? channels_random_range( random, 256, channels_num_channels - 1 ) : channels_random_range( random, 0, 255 ) );
    if ( record->code >= 2 )
        record->number = channels_random( random ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( record->code == 1 || record->code == 4 )
        record->sequencing = channels_random( random ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( has_channel )
        record->ordering = channels_random( random ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( record->code == 2 && channels_random_range( random, 0, 7 ) == 0 )
        record->control = 1;
    if ( record->code >= 2 && !record->control && channels_random_range( random, 0, 3 ) == 0 )
    {
        record->block = 1;
        record->fragments = (uint32_t) channels_random_range( random, 1, 27 );     // 28 * 1152 <= 32768 + 1151
        record->fragment = (uint32_t) channels_random_range( random, 0, (int) record->fragments );
    }

    const int header = rocketnet_message_record_header_bytes( record );
    int bytes;
    if ( record->block )
    {
        const bool last = record->fragment == record->fragments;
        bytes = last ? channels_random_range( random, 1, ROCKETNET_MESSAGE_FRAGMENT_BYTES ) : ROCKETNET_MESSAGE_FRAGMENT_BYTES;
        if ( header + bytes > max_bytes )
            return false;
        // place the block's base so its whole span is inside a 65536 ring
        const int stride_units = ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16;
        const int span = (int) record->fragments * ROCKETNET_MESSAGE_FRAGMENT_BYTES + ( last ? channels_round16( bytes ) : 16 );
        const int base = channels_random_range( random, 0, ( 65536 - span ) / 16 );
        record->ring = (uint32_t) ( base + (int) record->fragment * stride_units );
    }
    else
    {
        if ( max_bytes - header < 1 )
            return false;
        const int cap = max_bytes - header < 1300 ? max_bytes - header : 1300;
        bytes = channels_random_range( random, 0, 3 ) == 0 ? channels_random_range( random, 1, cap ) : channels_random_range( random, 1, cap < 40 ? cap : 40 );
        if ( record->code == 3 || record->code == 4 )
            record->ring = (uint32_t) channels_random_range( random, 0, ( 65536 - channels_round16( bytes ) ) / 16 );
    }
    record->bytes = bytes;
    record->data = pool + channels_random_range( random, 0, 2048 );
    return true;
}

static void channels_check_records_inside( const uint8_t * payload, int bytes, const std::vector<rocketnet_message_record_t> & records )
{
    // every record is whole and lies inside the body, never on the form byte
    for ( size_t i = 0; i < records.size(); i++ )
    {
        channels_check( records[i].bytes >= 1 );
        channels_check( records[i].data >= payload );
        channels_check( records[i].data + records[i].bytes <= payload + bytes - 1 );
    }
}

static void channels_check_repack( const uint8_t * payload, int bytes, const std::vector<rocketnet_message_record_t> & records, int form )
{
    // a parse that succeeds packs back to the same bytes: the codec is a bijection on valid payloads
    std::vector<uint8_t> repacked( (size_t) bytes + 64, 0xEE );
    if ( form == ROCKETNET_MESSAGE_FORM_SINGLE )
    {
        channels_check( records.size() == 1 );
        memcpy( repacked.data(), records[0].data, (size_t) records[0].bytes );
        channels_check( rocketnet_message_single_finish( repacked.data(), bytes, records[0].bytes ) == bytes );
    }
    else
    {
        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, repacked.data(), bytes );
        for ( size_t i = 0; i < records.size(); i++ )
        {
            channels_check( rocketnet_message_record_valid( &records[i] ) );
            channels_check( rocketnet_message_writer_add( &writer, &records[i] ) == 1 );
        }
        channels_check( rocketnet_message_writer_finish( &writer ) == bytes );
    }
    channels_check( memcmp( repacked.data(), payload, (size_t) bytes ) == 0 );
}

// parse the same L bytes followed by different tails: a reader that never
// looks past L reports the same thing whatever follows

static int channels_parse_with_tails( const uint8_t * bytes, int length, const struct rocketnet_message_limits_t * limits,
                                      std::vector<rocketnet_message_record_t> & records )
{
    // exact size first, so a sanitizer build sees any overread as a heap overflow
    std::vector<uint8_t> exact( bytes, bytes + length );
    const int violation = channels_parse_all( exact.data(), length, limits, records );
    channels_check( rocketnet_message_check_payload( exact.data(), length, limits ) == violation );

    static const uint8_t tails[3] = { 0x00, 0xFF, 0x01 };
    for ( int t = 0; t < 3; t++ )
    {
        std::vector<uint8_t> padded( (size_t) length + 64, tails[t] );
        if ( length > 0 )
            memcpy( padded.data(), bytes, (size_t) length );
        std::vector<rocketnet_message_record_t> again;
        channels_check( channels_parse_all( padded.data(), length, limits, again ) == violation );
        channels_check( again.size() == records.size() );
        for ( size_t i = 0; i < again.size(); i++ )
        {
            channels_check( again[i].data - padded.data() == records[i].data - exact.data() );
            channels_check( channels_record_equal( again[i], records[i] ) );
        }
    }

    // hand the records back pointing into the caller's bytes
    for ( size_t i = 0; i < records.size(); i++ )
        records[i].data = bytes + ( records[i].data - exact.data() );
    return violation;
}

static void channels_test_round_trip_and_never_overreads()
{
    const struct rocketnet_message_limits_t limits = channels_limits( channels_num_channels, 0x1F );
    const struct rocketnet_message_limits_t first_version_limits = channels_limits( 32, rocketnet_message_enabled_codes( 0x09 ) );

    channels_random_t random;
    random.state = 0x9E3779B97F4A7C15ULL;

    static uint8_t pool[4096];
    for ( int i = 0; i < (int) sizeof( pool ); i++ )
        pool[i] = (uint8_t) channels_random( &random );

    alignas( 16 ) static uint8_t payload[65536 + 64];

    const int iterations = 4000;
    int records_checked = 0;
    int mutations_parsed = 0;
    int violations_seen[ROCKETNET_MESSAGE_NUM_VIOLATIONS] = { 0 };

    for ( int iteration = 0; iteration < iterations; iteration++ )
    {
        // build: either a packet of mixed records at most 1192 bytes, or one
        // large unreliable record alone (L2.3), or a SINGLE message

        std::vector<rocketnet_message_record_t> sent;
        int length = 0;
        int form = ROCKETNET_MESSAGE_FORM_RECORDS;
        const int shape = channels_random_range( &random, 0, 9 );

        if ( shape == 0 )
        {
            form = ROCKETNET_MESSAGE_FORM_SINGLE;
            const int bytes = channels_random_range( &random, 1, channels_random_range( &random, 0, 1 ) ? ROCKETNET_MESSAGE_SINGLE_BYTES : 3000 );
            for ( int i = 0; i < bytes; i++ )
                payload[i] = pool[( i * 31 + iteration ) & 4095];
            length = rocketnet_message_single_finish( payload, sizeof( payload ), bytes );
            channels_check( length == bytes + 1 );
            struct rocketnet_message_record_t record = channels_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, payload, bytes );
            sent.push_back( record );
        }
        else if ( shape == 1 )
        {
            struct rocketnet_message_writer_t writer;
            rocketnet_message_writer_begin( &writer, payload, sizeof( payload ) );
            struct rocketnet_message_record_t record;
            std::vector<uint8_t> big( (size_t) channels_random_range( &random, 1200, ROCKETNET_MESSAGE_MAX_RECORD_BYTES ) );
            for ( size_t i = 0; i < big.size(); i++ )
                big[i] = pool[( i * 13 + (size_t) iteration ) & 4095];
            record = channels_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, big.data(), (int) big.size() );
            if ( channels_random_range( &random, 0, 1 ) )
            {
                record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
                record.channel = (uint32_t) channels_random_range( &random, 0, channels_num_channels - 1 );
                record.sequencing = channels_random( &random ) & ROCKETNET_MESSAGE_U24_MASK;
                record.ordering = channels_random( &random ) & ROCKETNET_MESSAGE_U24_MASK;
            }
            channels_check( rocketnet_message_record_valid( &record ) );
            channels_check( rocketnet_message_writer_add( &writer, &record ) == 1 );
            length = rocketnet_message_writer_finish( &writer );
            // the parse below points into payload; keep the sent data alive by comparing now
            std::vector<rocketnet_message_record_t> parsed;
            channels_check( channels_parse_with_tails( payload, length, &limits, parsed ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
            channels_check( parsed.size() == 1 && channels_record_equal( parsed[0], record ) );
            channels_check_repack( payload, length, parsed, form );
            records_checked++;
            continue;
        }
        else
        {
            struct rocketnet_message_writer_t writer;
            rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PAYLOAD_BYTES );
            const int wanted = channels_random_range( &random, 1, 24 );
            for ( int i = 0; i < wanted; i++ )
            {
                struct rocketnet_message_record_t record;
                const int space = rocketnet_message_writer_space( &writer );
                if ( !channels_random_record( &random, pool, space, true, &record ) )
                    break;
                channels_check( rocketnet_message_record_valid( &record ) );
                channels_check( rocketnet_message_writer_add( &writer, &record ) == 1 );
                sent.push_back( record );
            }
            if ( sent.empty() )
            {
                channels_check( rocketnet_message_writer_finish( &writer ) == 0 );
                continue;
            }
            // a record that does not fit is refused whole and leaves the payload unchanged
            {
                struct rocketnet_message_record_t too_big = channels_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, pool, rocketnet_message_writer_space( &writer ) - 2 );
                if ( too_big.bytes >= 1 )
                {
                    const int before = writer.bytes;
                    channels_check( rocketnet_message_writer_add( &writer, &too_big ) == 0 );
                    channels_check( writer.bytes == before );
                }
            }
            length = rocketnet_message_writer_finish( &writer );
            channels_check( length >= 4 && length <= ROCKETNET_MAX_PAYLOAD_BYTES );
        }

        // decode: the identity, in place

        std::vector<rocketnet_message_record_t> parsed;
        channels_check( channels_parse_with_tails( payload, length, &limits, parsed ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
        channels_check( parsed.size() == sent.size() );
        for ( size_t i = 0; i < sent.size(); i++ )
            channels_check( channels_record_equal( parsed[i], sent[i] ) );
        channels_check_records_inside( payload, length, parsed );
        channels_check_repack( payload, length, parsed, form );
        records_checked += (int) parsed.size();

        // every proper prefix: refused, or whole records inside the prefix

        const int prefix_step = length > 64 ? length / 48 : 1;
        for ( int prefix = 0; prefix < length; prefix += prefix_step )
        {
            std::vector<rocketnet_message_record_t> cut;
            const int violation = channels_parse_with_tails( payload, prefix, &limits, cut );
            if ( violation == ROCKETNET_MESSAGE_VIOLATION_NONE )
            {
                channels_check_records_inside( payload, prefix, cut );
                channels_check_repack( payload, prefix, cut, payload[prefix - 1] );
            }
        }

        // one-byte mutations: refused with one violation, or a payload that packs back to itself

        std::vector<uint8_t> mutated( payload, payload + length );
        const int mutations = length < 48 ? length : 48;
        for ( int m = 0; m < mutations; m++ )
        {
            const int position = length <= 48 ? m : channels_random_range( &random, 0, length - 1 );
            const uint8_t original = mutated[position];
            mutated[position] = (uint8_t) ( original ^ (uint8_t) channels_random_range( &random, 1, 255 ) );
            std::vector<rocketnet_message_record_t> result;
            const int violation = channels_parse_with_tails( mutated.data(), length, &limits, result );
            channels_check( violation >= ROCKETNET_MESSAGE_VIOLATION_NONE && violation < ROCKETNET_MESSAGE_NUM_VIOLATIONS );
            violations_seen[violation]++;

            // and under the first version's limits: codes 0 and 3, 32 channels
            std::vector<rocketnet_message_record_t> first_version;
            const int narrow = channels_parse_with_tails( mutated.data(), length, &first_version_limits, first_version );
            violations_seen[narrow]++;
            if ( narrow == ROCKETNET_MESSAGE_VIOLATION_NONE )
                channels_check_records_inside( mutated.data(), length, first_version );
            if ( violation == ROCKETNET_MESSAGE_VIOLATION_NONE )
            {
                channels_check_records_inside( mutated.data(), length, result );
                channels_check_repack( mutated.data(), length, result, mutated[length - 1] );
                mutations_parsed++;
            }
            mutated[position] = original;
        }
    }

    channels_check( records_checked > iterations );
    channels_check( mutations_parsed > 0 );

    // the mutations reach every check a record alone can fail; BAD_SIZE needs
    // a reliable record past 1192 bytes, which one byte cannot make, and
    // BAD_CONTROL is the CONFIG body's
    for ( int v = ROCKETNET_MESSAGE_VIOLATION_BAD_FORM; v <= ROCKETNET_MESSAGE_VIOLATION_BAD_RING; v++ )
    {
        if ( violations_seen[v] == 0 )
            printf( "no mutation reached %s\n", rocketnet_message_violation_name( v ) );
        channels_check( violations_seen[v] > 0 );
    }
}

// -----------------------------------------
//                              registration
// -----------------------------------------

struct channels_test_t
{
    const char * name;
    const char * alias;
    void ( *function )();
};

static const struct channels_test_t channels_tests[] =
{
    { "test_channels_message_record_vectors", "ChannelsMessageRecordVectors", channels_test_vectors },
    { "test_channels_message_record_violations", "ChannelsMessageRecordViolations", channels_test_violations },
    { "test_channels_message_single_zero_copy", "ChannelsMessageSingleZeroCopy", channels_test_single_zero_copy },
    { "test_channels_message_record_round_trip_and_never_overreads", "ChannelsMessageRecordRoundTripAndNeverOverreads", channels_test_round_trip_and_never_overreads },
};

int rocketnet_channels_test_count()
{
    return (int) ( sizeof( channels_tests ) / sizeof( channels_tests[0] ) );
}

const char * rocketnet_channels_test_name( int index )
{
    return channels_tests[index].name;
}

// the suite name for a suite name or an alias, or NULL

const char * rocketnet_channels_test_resolve( const char * name )
{
    for ( int i = 0; i < rocketnet_channels_test_count(); i++ )
    {
        if ( strcmp( channels_tests[i].name, name ) == 0 || strcmp( channels_tests[i].alias, name ) == 0 )
            return channels_tests[i].name;
    }
    return NULL;
}

// 0 when it ran (a failed check exits), 2 when no such test

int rocketnet_channels_run_named( const char * name )
{
    for ( int i = 0; i < rocketnet_channels_test_count(); i++ )
    {
        if ( strcmp( channels_tests[i].name, name ) != 0 )
            continue;
        printf( "%s\n", name );
        fflush( stdout );
        channels_tests[i].function();
        return 0;
    }
    return 2;
}
