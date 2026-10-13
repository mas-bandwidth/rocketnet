/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The message layer's unit tests (docs/CHANNELS.md), one layer at a time.
    Layer 2, message records: the known-answer vectors of STANDARD.md
    _Messages_ both ways, one input per violation hitting exactly that
    violation, the zero-copy SINGLE form in place at a 16-aligned payload
    root, and the property test over pack and parse.
    Layer 1, the region: layout, validation, the send store, the descriptor
    pool, the arena, the sizing print, and ChannelsAllocatorNoLiveAllocation
    (docs/CHANNELS.md 4). That case arms the allocation trap for a soak of
    send and receive at full configured load. rocketnet_channels_prepare
    installs the hooks before init; the trap is quiet until a case arms it.

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

#include "rocketnet_counters.h"
#include "rocketnet_allocator.h"
#include "rocketnet_unreliable.h"
#include "rocketnet_network_simulator.h"

#include <stdarg.h>
#include <new>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif


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

// ----------------------------------------------------------------
//                          the allocation trap
// ----------------------------------------------------------------

// Armed for the soak. While armed, the first call to any allocation function
// ends the process with a line naming it. Three nets: the rocketnet allocator
// hooks (installed before init), the global operator new and delete, and on
// glibc without a sanitizer the C allocation functions themselves.

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__)
#define RN_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RN_TEST_SANITIZED 1
#endif
#endif

#if defined(__GLIBC__) && !defined(RN_TEST_SANITIZED) && !defined(__clang_analyzer__)
#define RN_TEST_TRAP_LIBC 1
#else
#define RN_TEST_TRAP_LIBC 0
#endif

static volatile int rn_trap_armed = 0;
static const char * volatile rn_trap_phase = "idle";

static void rn_trap_write( const char * text )
{
#if defined(_WIN32)
    fputs( text, stderr );
#else
    size_t length = strlen( text );
    while ( length > 0 )
    {
        const ssize_t written = write( 2, text, length );
        if ( written <= 0 )
            break;
        text += written;
        length -= (size_t) written;
    }
#endif
}

static void rn_trap_fire( const char * function, size_t bytes )
{
    rn_trap_armed = 0;
    char line[160];
    // snprintf of integers and strings does not allocate
    snprintf( line, sizeof(line), "live allocation: %s (%llu bytes) during %s\n", function, (unsigned long long) bytes, rn_trap_phase );
    rn_trap_write( line );
    _Exit( 1 );
}

static uint64_t rn_hook_mallocs = 0;
static uint64_t rn_hook_frees = 0;

static void * rn_test_malloc_hook( void * context, size_t bytes )
{
    (void) context;
    if ( rn_trap_armed )
        rn_trap_fire( "rocketnet_malloc", bytes );
    rn_hook_mallocs++;
    return malloc( bytes );
}

static void rn_test_free_hook( void * context, void * p )
{
    (void) context;
    if ( rn_trap_armed )
        rn_trap_fire( "rocketnet_free", 0 );
    rn_hook_frees++;
    free( p );
}

#if RN_TEST_TRAP_LIBC

extern "C" void * __libc_malloc( size_t bytes );
extern "C" void __libc_free( void * p );
extern "C" void * __libc_calloc( size_t count, size_t bytes );
extern "C" void * __libc_realloc( void * p, size_t bytes );
extern "C" void * __libc_memalign( size_t alignment, size_t bytes );

extern "C" void * malloc( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "malloc", bytes );
    return __libc_malloc( bytes );
}

extern "C" void free( void * p )
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "free", 0 );
    __libc_free( p );
}

extern "C" void * calloc( size_t count, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "calloc", count * bytes );
    return __libc_calloc( count, bytes );
}

extern "C" void * realloc( void * p, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "realloc", bytes );
    return __libc_realloc( p, bytes );
}

extern "C" void * memalign( size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "memalign", bytes );
    return __libc_memalign( alignment, bytes );
}

extern "C" void * aligned_alloc( size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "aligned_alloc", bytes );
    return __libc_memalign( alignment, bytes );
}

extern "C" int posix_memalign( void ** out, size_t alignment, size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "posix_memalign", bytes );
    void * p = __libc_memalign( alignment, bytes );
    if ( !p )
        return 12; // ENOMEM
    *out = p;
    return 0;
}

#endif // #if RN_TEST_TRAP_LIBC

void * operator new( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new", bytes );
    void * p = malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void * operator new[]( size_t bytes )
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new[]", bytes );
    void * p = malloc( bytes ? bytes : 1 );
    if ( !p )
        throw std::bad_alloc();
    return p;
}

void * operator new( size_t bytes, const std::nothrow_t & ) noexcept
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new", bytes );
    return malloc( bytes ? bytes : 1 );
}

void * operator new[]( size_t bytes, const std::nothrow_t & ) noexcept
{
    if ( rn_trap_armed )
        rn_trap_fire( "operator new[]", bytes );
    return malloc( bytes ? bytes : 1 );
}

void operator delete( void * p ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete", 0 );
    free( p );
}

void operator delete[]( void * p ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete[]", 0 );
    free( p );
}

void operator delete( void * p, size_t ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete", 0 );
    free( p );
}

void operator delete[]( void * p, size_t ) noexcept
{
    if ( rn_trap_armed && p )
        rn_trap_fire( "operator delete[]", 0 );
    free( p );
}

// ----------------------------------------------------------------
//                           test plumbing
// ----------------------------------------------------------------

// The library's log is captured, never printed: a passing run prints names only.

static char rn_log_last[1024];
static int rn_log_lines = 0;

static void rn_test_log_function( int level, const char * format, ... )
{
    (void) level;
    va_list args;
    va_start( args, format );
    vsnprintf( rn_log_last, sizeof(rn_log_last), format, args );
    va_end( args );
    rn_log_lines++;
}

#define check( condition )                                                                               \
    do                                                                                                   \
    {                                                                                                    \
        if ( !( condition ) )                                                                            \
        {                                                                                                \
            rn_trap_armed = 0;                                                                           \
            fprintf( stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition );              \
            if ( rn_log_last[0] )                                                                        \
                fprintf( stderr, "last log line: %s\n", rn_log_last );                                   \
            exit( 1 );                                                                                   \
        }                                                                                                \
    } while ( 0 )

static uint64_t rn_counter( const rocketnet_counters_t * counters, int counter )
{
    return rocketnet_counter_value( counters, counter );
}

static uint64_t rn_random_state = 0x9E3779B97F4A7C15ULL;

static uint32_t rn_random()
{
    // xorshift64*: no library call, so nothing on the soak's path can allocate
    rn_random_state ^= rn_random_state >> 12;
    rn_random_state ^= rn_random_state << 25;
    rn_random_state ^= rn_random_state >> 27;
    return (uint32_t) ( ( rn_random_state * 0x2545F4914F6CDD1DULL ) >> 32 );
}

static uint8_t rn_pattern( uint32_t serial, uint32_t offset )
{
    return (uint8_t) ( serial * 31u + offset * 7u + ( offset >> 8 ) );
}

// ----------------------------------------------------------------
//                              the cases
// ----------------------------------------------------------------

static void test_channels_allocator_layout()
{
    // section 14's formula, part by part, at the defaults: 449,024 bytes, no padding

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    check( config.num_channels == 32 );
    check( config.window_messages == 1024 );
    check( config.receive_ring_bytes == 65536 );

    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );
    check( layout.formula_bytes == 449024 );
    check( layout.region_bytes == 449024 );
    check( rocketnet_message_connection_bytes( &config ) == 449024 );
    check( rocketnet_message_connection_bytes( NULL ) == 449024 );

    const uint64_t C = 32, W = 1024, B = 65536, Q = 1024, S = 131072, P = 256, Du = 256, Ru = 16, Rq = 256;
    check( layout.bytes[RN_PART_CHANNEL_SEND] + layout.bytes[RN_PART_CHANNEL_RECEIVE] == 48 * C );
    check( layout.bytes[RN_PART_SEND_NUMBERS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVE_DESCRIPTORS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVED_BITS] == W / 8 );
    check( layout.bytes[RN_PART_HELD_INDEX] == 16 * 2 * W );
    check( layout.bytes[RN_PART_RECEIVE_RING] == B );
    check( layout.bytes[RN_PART_SEND_DESCRIPTORS] == 64 * Q );
    check( layout.bytes[RN_PART_SEND_STORE] == S );
    check( layout.bytes[RN_PART_SENT_PACKETS] + layout.bytes[RN_PART_SENT_INDEX] + layout.bytes[RN_PART_SENT_REFS] == 128 * P );
    check( layout.bytes[RN_PART_DELIVERIES] == 24 * ( W + Du ) );
    check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == 1208 * Ru );
    check( layout.bytes[RN_PART_RECEIPTS] == 16 * Rq );

    // every part 64-aligned, in order, disjoint, inside the region

    uint64_t end = 0;
    for ( int part = 0; part < RN_NUM_PARTS; part++ )
    {
        check( rn_region_part_name( part ) != NULL );
        check( layout.offset[part] % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        check( layout.offset[part] >= end );
        end = layout.offset[part] + layout.bytes[part];
    }
    check( end <= layout.region_bytes );
    check( layout.region_bytes % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );

    // the store's classes at the defaults, as section 4 records them: 64-aligned
    // thirds of S, each chunk with its u32 link inside its third

    check( layout.store_chunks[RN_STORE_SMALL] == 330 );
    check( layout.store_chunks[RN_STORE_MEDIUM] == 84 );
    check( layout.store_chunks[RN_STORE_LARGE] == 37 );
    check( layout.store_chunks[RN_STORE_LARGE] >= ( 32768 + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES );

    // odd shapes pad, and the padding is all the region adds to the formula

    config.num_channels = 3;
    config.unreliable_deliveries = 1;
    config.sequenced_holds = 1;
    config.receipt_ring = 17;
    check( rn_region_layout( &config, &layout ) );
    check( layout.region_bytes > layout.formula_bytes );
    check( layout.region_bytes - layout.formula_bytes < 64 * RN_NUM_PARTS );
    check( layout.region_bytes % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
    end = 0;
    for ( int part = 0; part < RN_NUM_PARTS; part++ )
    {
        check( layout.offset[part] % ROCKETNET_ALLOCATION_ALIGNMENT == 0 );
        check( layout.offset[part] >= end );
        end = layout.offset[part] + layout.bytes[part];
    }
    check( end <= layout.region_bytes );

    // no sequenced holds is legal and takes no bytes

    config.sequenced_holds = 0;
    check( rn_region_layout( &config, &layout ) );
    check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == 0 );
}

static void test_channels_allocator_validation()
{
    rocketnet_message_config_t config;

    // the defaults, the table's minimum where the cross rules allow it, and the maxima

    rocketnet_message_default_config( &config );
    check( rocketnet_message_validate_config( &config ) == 1 );

    struct rejected_t { const char * what; int field_offset; int value; };
    const rejected_t rejected[] =
    {
        { "num_channels 0",                         (int) offsetof( rocketnet_message_config_t, num_channels ), 0 },
        { "num_channels above the compile-time max", (int) offsetof( rocketnet_message_config_t, num_channels ), ROCKETNET_MAX_CHANNELS + 1 },
        { "window not a power of two",              (int) offsetof( rocketnet_message_config_t, window_messages ), 1000 },
        { "window below its minimum",               (int) offsetof( rocketnet_message_config_t, window_messages ), 32 },
        { "ring not a multiple of 16",              (int) offsetof( rocketnet_message_config_t, receive_ring_bytes ), 65536 + 8 },
        { "max message above the ring",             (int) offsetof( rocketnet_message_config_t, max_message_bytes ), 65536 + 16 },
        { "store too small for a maximal block",    (int) offsetof( rocketnet_message_config_t, send_buffer_bytes ), 12288 },
        { "queue below its minimum",                (int) offsetof( rocketnet_message_config_t, send_queue_messages ), 15 },
        { "rto_min above rto_max",                  (int) offsetof( rocketnet_message_config_t, rto_min_ms ), 3000 },
        { "rto_initial above rto_max",              (int) offsetof( rocketnet_message_config_t, rto_initial_ms ), 3000 },
        { "rto_initial below rto_min",              (int) offsetof( rocketnet_message_config_t, rto_initial_ms ), 20 },
    };

    for ( size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++ )
    {
        rocketnet_message_default_config( &config );
        memcpy( (uint8_t*) &config + rejected[i].field_offset, &rejected[i].value, sizeof(int) );
        const int lines_before = rn_log_lines;
        if ( rocketnet_message_validate_config( &config ) != 0 )
        {
            fprintf( stderr, "accepted: %s\n", rejected[i].what );
            exit( 1 );
        }
        // refused loudly, and the connection bytes are 0 for a refused config
        check( rn_log_lines > lines_before );
        check( rocketnet_message_connection_bytes( &config ) == 0 );
        rn_region_layout_t layout;
        check( !rn_region_layout( &config, &layout ) );
    }

    // ceil( max_message_bytes / F ) above W: a 64-message window holds at most 64 fragments

    rocketnet_message_default_config( &config );
    config.window_messages = 64;
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    config.receive_ring_bytes = 1 << 20;
    config.send_buffer_bytes = 1 << 20;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // the edge of the store rule: the large class holds exactly one maximal block

    rocketnet_message_default_config( &config );
    config.max_message_bytes = 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config.send_buffer_bytes = 12288;               // a third of 12288 holds three 1152 chunks with their links
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // section 14's lean profile is refused at the default max_message_bytes: its
    // large class holds 18 chunks, a 32768-byte block needs 29 (section 4)

    rocketnet_message_default_config( &config );
    config.window_messages = 256;
    config.receive_ring_bytes = 32768;
    config.send_buffer_bytes = 65536;
    config.send_queue_messages = 256;
    config.packets_in_flight = 64;
    config.unreliable_deliveries = 128;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 18 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );
    config.max_message_bytes = 18 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    check( rocketnet_message_validate_config( &config ) == 0 );

    // the maximum channel count is legal

    rocketnet_message_default_config( &config );
    config.num_channels = ROCKETNET_MAX_CHANNELS;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // the smallest legal region: every minimum the cross rules allow

    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.window_messages = 64;
    config.receive_ring_bytes = 4096;
    config.max_message_bytes = 1191;
    config.send_buffer_bytes = 12288;
    config.send_queue_messages = 16;
    config.packets_in_flight = 16;
    config.max_packets_per_update = 1;
    config.unreliable_deliveries = 1;
    config.sequenced_holds = 0;
    config.receipt_ring = 16;
    config.rto_min_ms = 1;
    config.rto_initial_ms = 10;
    config.rto_max_ms = 10;
    config.reliable_timeout_ms = 1000;
    check( rocketnet_message_validate_config( &config ) == 1 );
    check( rocketnet_message_connection_bytes( &config ) > 0 );

    check( rocketnet_message_validate_config( NULL ) == 1 );       // NULL is the defaults
}

static void test_channels_allocator_send_store()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );

    uint8_t * memory = (uint8_t*) rocketnet_malloc( NULL, (size_t) layout.bytes[RN_PART_SEND_STORE] );
    check( memory );
    rn_send_store_t store;
    rn_send_store_init( &store, memory, layout.bytes[RN_PART_SEND_STORE] );

    const int class_bytes[RN_STORE_NUM_CLASSES] = { RN_STORE_SMALL_BYTES, RN_STORE_MEDIUM_BYTES, RN_STORE_LARGE_BYTES };
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        check( rn_send_store_total_chunks( &store, c ) == layout.store_chunks[c] );
        check( rn_send_store_free_chunks( &store, c ) == layout.store_chunks[c] );
    }

    // the smallest class that fits, by size

    check( rn_send_store_class_for( 1 ) == RN_STORE_SMALL );
    check( rn_send_store_class_for( 128 ) == RN_STORE_SMALL );
    check( rn_send_store_class_for( 129 ) == RN_STORE_MEDIUM );
    check( rn_send_store_class_for( 512 ) == RN_STORE_MEDIUM );
    check( rn_send_store_class_for( 513 ) == RN_STORE_LARGE );
    check( rn_send_store_class_for( 1152 ) == RN_STORE_LARGE );
    check( rn_send_store_class_for( 1153 ) == RN_STORE_LARGE );
    check( rn_send_store_chunks_needed( 1 ) == 1 );
    check( rn_send_store_chunks_needed( 1152 ) == 1 );
    check( rn_send_store_chunks_needed( 1153 ) == 2 );
    check( rn_send_store_chunks_needed( 32768 ) == 29 );

    // fill every class to refusal, with nothing else taken; the chunks are
    // distinct, inside the store and 16-aligned

    static uint32_t handles[RN_STORE_NUM_CLASSES][4096];
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint32_t total = rn_send_store_total_chunks( &store, c );
        check( total <= 4096 );
        for ( uint32_t i = 0; i < total; i++ )
        {
            handles[c][i] = rn_send_store_alloc( &store, class_bytes[c] );
            check( handles[c][i] != RN_CHUNK_NONE );
            uint8_t * data = rn_send_store_chunk_data( &store, handles[c][i] );
            check( data >= memory && data + class_bytes[c] <= memory + layout.bytes[RN_PART_SEND_STORE] );
            check( ( (uintptr_t) data & 15 ) == 0 );
            memset( data, (int) ( c * 64 + ( i & 63 ) ), (size_t) class_bytes[c] );
        }
        check( rn_send_store_free_chunks( &store, c ) == 0 );
        check( rn_send_store_alloc( &store, class_bytes[c] ) == RN_CHUNK_NONE );
        // no fallback into a larger class, and no other class moved
        for ( int other = 0; other < RN_STORE_NUM_CLASSES; other++ )
            if ( other > c )
                check( rn_send_store_free_chunks( &store, other ) == rn_send_store_total_chunks( &store, other ) );
    }

    // the bytes written are still there: no two handles share a chunk

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        for ( uint32_t i = 0; i < rn_send_store_total_chunks( &store, c ); i++ )
        {
            const uint8_t * data = rn_send_store_chunk_data( &store, handles[c][i] );
            for ( int b = 0; b < class_bytes[c]; b++ )
                check( data[b] == (uint8_t) ( c * 64 + ( i & 63 ) ) );
        }

    // free in random order, refill, and the counts hold at every step

    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint32_t total = rn_send_store_total_chunks( &store, c );
        for ( uint32_t i = total; i > 1; i-- )
        {
            const uint32_t j = rn_random() % i;
            const uint32_t t = handles[c][i - 1]; handles[c][i - 1] = handles[c][j]; handles[c][j] = t;
        }
        for ( uint32_t i = 0; i < total; i++ )
        {
            rn_send_store_free( &store, handles[c][i] );
            check( rn_send_store_free_chunks( &store, c ) == i + 1 );
        }
        for ( uint32_t i = 0; i < total; i++ )
            check( rn_send_store_alloc( &store, class_bytes[c] ) != RN_CHUNK_NONE );
        check( rn_send_store_alloc( &store, class_bytes[c] ) == RN_CHUNK_NONE );
    }

    // blocks: chained large chunks, all or none

    rn_send_store_init( &store, memory, layout.bytes[RN_PART_SEND_STORE] );
    const uint32_t large = rn_send_store_total_chunks( &store, RN_STORE_LARGE );
    const uint32_t block = rn_send_store_alloc( &store, 32768 );
    check( block != RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == large - 29 );
    int links = 0;
    for ( uint32_t chunk = block; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &store, chunk ) )
    {
        memset( rn_send_store_chunk_data( &store, chunk ), links, RN_STORE_LARGE_BYTES );
        links++;
    }
    check( links == 29 );

    // a block bigger than what is left takes nothing

    const uint32_t left = rn_send_store_free_chunks( &store, RN_STORE_LARGE );
    check( rn_send_store_alloc( &store, (int) ( left + 1 ) * RN_STORE_LARGE_BYTES ) == RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == left );
    const uint32_t rest = rn_send_store_alloc( &store, (int) left * RN_STORE_LARGE_BYTES );
    check( rest != RN_CHUNK_NONE );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == 0 );
    check( rn_send_store_alloc( &store, 513 ) == RN_CHUNK_NONE );

    // freeing a block frees its whole chain; the first block's bytes survived the second
    links = 0;
    for ( uint32_t chunk = block; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &store, chunk ) )
    {
        const uint8_t * data = rn_send_store_chunk_data( &store, chunk );
        check( data[0] == (uint8_t) links && data[RN_STORE_LARGE_BYTES - 1] == (uint8_t) links );
        links++;
    }
    rn_send_store_free( &store, block );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == 29 );
    rn_send_store_free( &store, rest );
    check( rn_send_store_free_chunks( &store, RN_STORE_LARGE ) == large );

    // a refused double free changes nothing

    const uint32_t once = rn_send_store_alloc( &store, 100 );
    check( once != RN_CHUNK_NONE );
    rn_send_store_free( &store, once );
    const uint32_t small_free = rn_send_store_free_chunks( &store, RN_STORE_SMALL );
    check( !rn_send_store_free_checked( &store, once ) );
    check( rn_send_store_free_chunks( &store, RN_STORE_SMALL ) == small_free );
    check( !rn_send_store_free_checked( &store, RN_CHUNK_NONE ) );

    rocketnet_free( NULL, memory );
}

// The store's accessors answer the checked free's rule
// (rocketnet_allocator.cpp, rn_send_store_free_checked): a handle the store did
// not issue -- a class at or past RN_STORE_NUM_CLASSES, or an index at or past
// the class's chunk count -- reads nothing. A handle the store did issue still
// answers its data and its next exactly as before.

static void test_channels_send_store_refuses_bad_handle()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );

    uint8_t * memory = (uint8_t*) rocketnet_malloc( NULL, (size_t) layout.bytes[RN_PART_SEND_STORE] );
    check( memory );
    rn_send_store_t store;
    rn_send_store_init( &store, memory, layout.bytes[RN_PART_SEND_STORE] );

    // a valid allocated handle still returns its data and its next as before

    const uint32_t valid = rn_send_store_alloc( &store, 100 );
    check( valid != RN_CHUNK_NONE );
    uint8_t * valid_data = rn_send_store_chunk_data( &store, valid );
    check( valid_data != NULL );
    check( valid_data >= memory && valid_data + 100 <= memory + layout.bytes[RN_PART_SEND_STORE] );
    memset( valid_data, 0xA5, 100 );
    check( valid_data[0] == 0xA5 && valid_data[99] == 0xA5 );
    check( rn_send_store_chunk_next( &store, valid ) == RN_CHUNK_NONE );

    // a real chain still walks whole

    const uint32_t block = rn_send_store_alloc( &store, 32768 );
    check( block != RN_CHUNK_NONE );
    int links = 0;
    for ( uint32_t chunk = block; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &store, chunk ) )
        links++;
    check( links == (int) rn_send_store_chunks_needed( 32768 ) );

    // a valid class with an index one past its chunk count reads nothing

    const uint32_t bad[] =
    {
        ( (uint32_t) RN_STORE_SMALL << 30 ) | rn_send_store_total_chunks( &store, RN_STORE_SMALL ),
        ( (uint32_t) RN_STORE_MEDIUM << 30 ) | rn_send_store_total_chunks( &store, RN_STORE_MEDIUM ),
        ( (uint32_t) RN_STORE_LARGE << 30 ) | rn_send_store_total_chunks( &store, RN_STORE_LARGE ),
        RN_CHUNK_NONE,
        0xC0000000u,
    };
    for ( int k = 0; k < (int) ( sizeof(bad) / sizeof(bad[0]) ); k++ )
    {
        check( rn_send_store_chunk_data( &store, bad[k] ) == NULL );
        check( rn_send_store_chunk_next( &store, bad[k] ) == RN_CHUNK_NONE );
    }

    rn_send_store_free( &store, block );
    rn_send_store_free( &store, valid );
    rocketnet_free( NULL, memory );
}

static void test_channels_allocator_descriptor_pool()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.send_queue_messages = 16;

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    // Q descriptors, then a counted refusal that takes no store chunk

    uint32_t descriptors[16];
    for ( int i = 0; i < 16; i++ )
    {
        descriptors[i] = rn_region_send_alloc( region, 10 );
        check( descriptors[i] != RN_DESCRIPTOR_NONE );
        check( descriptors[i] < 16 );
        for ( int j = 0; j < i; j++ )
            check( descriptors[j] != descriptors[i] );
        check( region->send_descriptors[descriptors[i]].bytes == 10 );
    }
    check( region->descriptors_free == 0 );
    const uint32_t small_free = rn_send_store_free_chunks( &region->store, RN_STORE_SMALL );
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) == 1 );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_free );

    // a free returns the descriptor and its chunk

    rn_region_send_free( region, descriptors[5] );
    check( region->descriptors_free == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_free + 1 );
    check( rn_region_send_alloc( region, 10 ) == descriptors[5] );

    // a store refusal takes no descriptor, and is counted as itself

    for ( int i = 0; i < 16; i++ )
        rn_region_send_free( region, descriptors[i] );
    check( region->descriptors_free == 16 );
    check( rn_region_send_alloc( region, config.max_message_bytes + 1 ) == RN_DESCRIPTOR_NONE );   // too large is a refusal, never a store attempt
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rn_region_send_alloc( region, 0 ) == RN_DESCRIPTOR_NONE );
    const uint32_t large = rn_send_store_free_chunks( &region->store, RN_STORE_LARGE );
    const uint32_t blocks = large / 29;
    for ( uint32_t i = 0; i < blocks; i++ )
        check( rn_region_send_alloc( region, 32768 ) != RN_DESCRIPTOR_NONE );
    check( rn_region_send_alloc( region, 32768 ) == RN_DESCRIPTOR_NONE );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 1 );
    check( region->descriptors_free == 16 - blocks );

    // a carve gives everything back

    rn_region_carve( region );
    check( region->descriptors_free == 16 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large );

    rn_arena_destroy( arena );
}

static void test_channels_allocator_arena()
{
    // one allocation per arena, at create; none at carve; one free at destroy

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    const uint64_t mallocs = rn_hook_mallocs;
    const uint64_t frees = rn_hook_frees;
    rn_arena_t * arena = rn_arena_create( NULL, &config, 16, &counters );
    check( arena );
    check( rn_hook_mallocs == mallocs + 1 );
    check( rn_arena_num_regions( arena ) == 16 );
    check( rn_arena_bytes( arena ) >= 16 * rocketnet_message_connection_bytes( &config ) );
    check( rn_arena_bytes( arena ) < 16 * rocketnet_message_connection_bytes( &config ) + 65536 );

    for ( int i = 0; i < 16; i++ )
    {
        rn_region_t * region = rn_arena_region( arena, i );
        check( region );
        check( region->bytes == rocketnet_message_connection_bytes( &config ) );
        check( ( (uintptr_t) region->base & ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) ) == 0 );
        if ( i > 0 )
            check( rn_arena_region( arena, i - 1 )->base + rn_arena_region( arena, i - 1 )->bytes <= region->base );
        rn_region_carve( region );

        // every typed carve-out lies inside its own region, and the ring is 16-aligned
        check( rn_region_contains( region, region->channel_send, sizeof(rn_channel_send_t) * (uint64_t) region->num_channels ) );
        check( rn_region_contains( region, region->channel_receive, sizeof(rn_channel_receive_t) * (uint64_t) region->num_channels ) );
        check( rn_region_contains( region, region->send_numbers, sizeof(rn_send_number_t) * (uint64_t) region->window ) );
        check( rn_region_contains( region, region->receive_descriptors, sizeof(rn_receive_descriptor_t) * (uint64_t) region->window ) );
        check( rn_region_contains( region, region->received_bits, region->window / 8 ) );
        check( rn_region_contains( region, region->held_index, sizeof(rn_held_entry_t) * (uint64_t) region->held_entries ) );
        check( rn_region_contains( region, region->receive_ring, region->ring_bytes ) );
        check( ( (uintptr_t) region->receive_ring & 15 ) == 0 );
        check( rn_region_contains( region, region->send_descriptors, sizeof(rn_send_descriptor_t) * (uint64_t) region->queue ) );
        check( rn_region_contains( region, region->sent_packets, sizeof(rn_sent_packet_t) * (uint64_t) region->packets ) );
        check( rn_region_contains( region, region->sent_index, sizeof(rn_sent_index_t) * (uint64_t) region->sent_index_entries ) );
        check( rn_region_contains( region, region->sent_refs, sizeof(uint32_t) * (uint64_t) region->sent_ref_entries ) );
        check( rn_region_contains( region, region->deliveries, sizeof(rn_delivery_t) * (uint64_t) region->delivery_entries ) );
        check( rn_region_contains( region, region->sequenced_holds, sizeof(rn_sequenced_hold_t) * (uint64_t) region->holds ) );
        check( rn_region_contains( region, region->receipts, sizeof(rn_receipt_t) * (uint64_t) region->receipt_entries ) );
        check( !rn_region_contains( region, region->base + region->bytes, 1 ) );
        check( !rn_region_contains( region, region->base - 1, 1 ) );
        check( region->held_entries == 2 * region->window );
        check( region->sent_index_entries == 2 * region->packets );
        check( region->sent_ref_entries == 16 * region->packets );
        check( region->delivery_entries == region->window + (uint32_t) config.unreliable_deliveries );
    }

    // the receiver's ring bounds check

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( rn_region_ring_span_valid( region, 0, 16 ) );
    check( rn_region_ring_span_valid( region, 65536 - 1152, 1152 ) );
    check( !rn_region_ring_span_valid( region, 65536 - 1152, 1153 ) );
    check( !rn_region_ring_span_valid( region, 65536, 1 ) );
    check( !rn_region_ring_span_valid( region, 8, 16 ) );                  // placements are multiples of 16
    check( !rn_region_ring_span_valid( region, 0xFFFFFFFFFFFFFFF0ULL, 32 ) ); // no wrap past 2^64

    rn_arena_destroy( arena );
    check( rn_hook_mallocs == mallocs + 1 );
    check( rn_hook_frees == frees + 1 );

    // a refused config allocates nothing

    config.window_messages = 1000;
    check( rn_arena_create( NULL, &config, 1, &counters ) == NULL );
    check( rn_hook_mallocs == mallocs + 1 );
    rocketnet_message_default_config( &config );
    check( rn_arena_create( NULL, &config, 0, &counters ) == NULL );
    check( rn_hook_mallocs == mallocs + 1 );
}

static void test_channels_allocator_sizing()
{
    // the formula and the region, printed before any connect; the buffer form allocates nothing

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    static char text[4096];
    rn_trap_phase = "sizing";
    rn_trap_armed = 1;
    const int length = rocketnet_message_connection_sizing( &config, 1000, text, (int) sizeof(text) );
    rn_trap_armed = 0;
    check( length > 0 && length < (int) sizeof(text) );
    check( (int) strlen( text ) == length );
    check( strstr( text, "48C + 96.125W + B + 64Q + S + 128P + 24(W + Du) + 1208Ru + 16Rq" ) != NULL );
    check( strstr( text, "region bytes 449024" ) != NULL );
    check( strstr( text, "formula bytes 449024" ) != NULL );
    check( strstr( text, "1000 connections 449024000" ) != NULL );

    // too small a buffer truncates and still reports the full length, as snprintf does

    char small[32];
    check( rocketnet_message_connection_sizing( &config, 1, small, (int) sizeof(small) ) == rocketnet_message_connection_sizing( &config, 1, NULL, 0 ) );
    check( strlen( small ) == sizeof(small) - 1 );

    // a refused config prints nothing and returns 0

    config.receive_ring_bytes = 100;
    check( rocketnet_message_connection_sizing( &config, 1, text, (int) sizeof(text) ) == 0 );
    check( text[0] == '\0' );

    // the print form logs the same lines at info level and returns the region bytes

    rocketnet_message_default_config( &config );
    const int old_level = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    const int lines_before = rn_log_lines;
    check( rocketnet_message_print_connection_sizing( &config, 16 ) == 449024 );
    rocketnet_log_level( old_level );
    check( rn_log_lines - lines_before >= 10 );
    check( strstr( rn_log_last, "16 connections" ) != NULL );
}

static void test_channels_allocator_trap_witness()
{
    // the trap fires: each net, armed in a child, ends it with exit 1

#if !defined(_WIN32)
    for ( int net = 0; net < 4; net++ )
    {
        if ( net == 2 && !RN_TEST_TRAP_LIBC )
            continue;
        fflush( stdout );
        fflush( stderr );
        const pid_t pid = fork();
        check( pid >= 0 );
        if ( pid == 0 )
        {
            // the child's trap line is the witness's expected output; keep the parent's stderr clean
            FILE * devnull = freopen( "/dev/null", "w", stderr );
            (void) devnull;
            rn_trap_phase = "witness";
            rn_trap_armed = 1;
            // volatile function pointers: a compiler may elide a malloc/free pair it can see
            void * (* volatile allocate)( size_t ) = malloc;
            void (* volatile release)( void * ) = free;
            switch ( net )
            {
                case 0: rocketnet_free( NULL, rocketnet_malloc( NULL, 8 ) ); break;
                case 1: { int * volatile p = new int( 1 ); delete p; } break;
                case 2: release( allocate( 8 ) ); break;
                case 3: { rocketnet_counters_t counters; rocketnet_message_config_t config; rocketnet_message_default_config( &config ); rn_arena_destroy( rn_arena_create( NULL, &config, 1, &counters ) ); } break;
            }
            _Exit( 0 );
        }
        int status = 0;
        check( waitpid( pid, &status, 0 ) == pid );
        check( WIFEXITED( status ) && WEXITSTATUS( status ) == 1 );
    }
#endif // #if !defined(_WIN32)
}

// ----------------------------------------------------------------
//                  the soak: send and receive at full load
// ----------------------------------------------------------------

// Layers 2 to 8 do not exist yet; this stands in for them by driving every
// carve-out of layer 1 to its configured capacity, the way they will
// (docs/CHANNELS.md 4, 6.1, 7.1, 9.1). Per tick, per connection, each way:
// sends until the descriptor pool or the store refuses, a write of every
// fragment from its store chunk into a frame with its number and sent-map
// entry, the receive side's descriptor, bitset, ring copy, held index,
// deliveries, holds and receipts, then acks of a random part and frees.
// The receive side checks every byte. Every refusal must equal its counter.
// A reconnect (a carve) every 20th tick. The trap is armed throughout.

struct rn_soak_side_t
{
    rn_region_t * sender;
    rn_region_t * receiver;
    rocketnet_counters_t * sender_counters;
    const rocketnet_message_config_t * config;
    uint64_t next_number;           // the next message number this side gives
    uint64_t oldest_unacked;
    uint64_t receiver_next;         // R: the lowest number not yet received
    uint64_t ring_next;             // the sender's absolute ring position
    uint64_t next_packet_id;
    uint32_t sent_head, sent_count; // the sent-packet ring, oldest first
    uint32_t ref_next;
    uint32_t serial;
    uint32_t receipt_head, receipt_count;
    uint64_t refused_store, refused_queue;
    uint64_t sent_bytes, delivered_messages, delivered_bytes, acked_fragments;
};

static void rn_soak_check_accounting( const rn_region_t * region )
{
    // free + used = total, for the store's classes and the descriptor pool

    uint32_t used[RN_STORE_NUM_CLASSES] = { 0, 0, 0 };
    uint32_t descriptors_used = 0;
    for ( uint32_t d = 0; d < region->queue; d++ )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        if ( !descriptor->in_use )
            continue;
        descriptors_used++;
        for ( uint32_t chunk = descriptor->chunk; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &region->store, chunk ) )
            used[rn_send_store_chunk_class( chunk )]++;
    }
    check( descriptors_used + region->descriptors_free == region->queue );
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        check( used[c] + rn_send_store_free_chunks( &region->store, c ) == rn_send_store_total_chunks( &region->store, c ) );
}

static uint32_t rn_held_slot( const rn_region_t * region, uint64_t key )
{
    uint32_t slot = (uint32_t) ( ( key * 0x9E3779B97F4A7C15ULL ) >> 40 ) & ( region->held_entries - 1 );
    while ( region->held_index[slot].used && region->held_index[slot].key != key )
        slot = ( slot + 1 ) & ( region->held_entries - 1 );
    return slot;
}

static uint32_t rn_sent_index_slot( const rn_region_t * region, uint64_t packet_id )
{
    uint32_t slot = (uint32_t) ( packet_id * 2654435761u ) % region->sent_index_entries;
    while ( region->sent_index[slot].used && region->sent_index[slot].packet_id != packet_id )
        slot = ( slot + 1 ) % region->sent_index_entries;
    return slot;
}

static void rn_soak_reset( rn_soak_side_t * side )
{
    side->next_number = 0;
    side->oldest_unacked = 0;
    side->receiver_next = 0;
    side->ring_next = 0;
    side->sent_head = 0;
    side->sent_count = 0;
    side->ref_next = 0;
    side->receipt_head = 0;
    side->receipt_count = 0;
}

static void rn_soak_send( rn_soak_side_t * side )
{
    rn_region_t * region = side->sender;
    const rocketnet_message_config_t * config = side->config;

    // send until refused: mostly small, some medium, some fragment-sized, some blocks up to the maximum

    for ( ;; )
    {
        const uint32_t pick = rn_random() % 16;
        int bytes;
        if ( pick < 8 )
            bytes = 1 + (int) ( rn_random() % RN_STORE_SMALL_BYTES );
        else if ( pick < 12 )
            bytes = RN_STORE_SMALL_BYTES + 1 + (int) ( rn_random() % ( RN_STORE_MEDIUM_BYTES - RN_STORE_SMALL_BYTES ) );
        else if ( pick < 15 )
            bytes = RN_STORE_MEDIUM_BYTES + 1 + (int) ( rn_random() % ( RN_STORE_LARGE_BYTES - RN_STORE_MEDIUM_BYTES ) );
        else
            bytes = RN_STORE_LARGE_BYTES + 1 + (int) ( rn_random() % (uint32_t) ( config->max_message_bytes - RN_STORE_LARGE_BYTES ) );

        const uint64_t store_full = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
        const uint64_t queue_full = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );
        const uint32_t d = rn_region_send_alloc( region, bytes );
        if ( d == RN_DESCRIPTOR_NONE )
        {
            const uint64_t store_now = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
            const uint64_t queue_now = rn_counter( side->sender_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL );
            check( ( store_now - store_full ) + ( queue_now - queue_full ) == 1 );       // exactly one reason, counted once
            side->refused_store += store_now - store_full;
            side->refused_queue += queue_now - queue_full;
            break;
        }

        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        check( descriptor->bytes == (uint32_t) bytes );
        descriptor->serial = ++side->serial;
        descriptor->reliability = ROCKETNET_RELIABLE_ORDERED;
        descriptor->priority = (uint8_t) ( rn_random() % 4 );
        descriptor->channel = (uint16_t) ( rn_random() % region->num_channels );

        // copy the message into its chunks, as send_message will

        uint32_t offset = 0;
        for ( uint32_t chunk = descriptor->chunk; chunk != RN_CHUNK_NONE; chunk = rn_send_store_chunk_next( &region->store, chunk ) )
        {
            uint8_t * data = rn_send_store_chunk_data( &region->store, chunk );
            check( rn_region_contains( region, data, 1 ) );
            const uint32_t take = (uint32_t) bytes - offset < (uint32_t) RN_STORE_LARGE_BYTES ? (uint32_t) bytes - offset : (uint32_t) RN_STORE_LARGE_BYTES;
            for ( uint32_t b = 0; b < take; b++ )
                data[b] = rn_pattern( descriptor->serial, offset + b );
            offset += take;
        }
        check( offset == (uint32_t) bytes );

        // queue it on its channel's FIFO (layer 6's fields)

        rn_channel_send_t * channel = &region->channel_send[descriptor->channel];
        descriptor->next = RN_DESCRIPTOR_NONE;
        if ( channel->queue_tail == RN_DESCRIPTOR_NONE )
            channel->queue_head = d;
        else
            region->send_descriptors[channel->queue_tail].next = d;
        channel->queue_tail = d;
        channel->queued[descriptor->priority]++;
        descriptor->ordering = channel->ordered_write++;
        channel->sequenced_write = 0;
    }
}

static void rn_soak_write_and_receive( rn_soak_side_t * side, uint64_t tick )
{
    rn_region_t * region = side->sender;
    rn_region_t * receiver = side->receiver;
    uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];

    for ( uint32_t c = 0; c < region->num_channels; c++ )
    {
        rn_channel_send_t * channel = &region->channel_send[c];
        while ( channel->queue_head != RN_DESCRIPTOR_NONE )
        {
            const uint32_t d = channel->queue_head;
            rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            const uint32_t fragments = ( descriptor->bytes + RN_STORE_LARGE_BYTES - 1 ) / RN_STORE_LARGE_BYTES;
            const uint64_t span = (uint64_t) fragments * RN_STORE_LARGE_BYTES;

            // the window's two bounds (6.2): the count, and the ring bytes

            if ( side->next_number + fragments > side->oldest_unacked + region->window )
                return;
            uint64_t ring = side->ring_next;
            if ( ring / region->ring_bytes != ( ring + span - 1 ) / region->ring_bytes )
                ring = ( ring / region->ring_bytes + 1 ) * region->ring_bytes;
            if ( side->sent_count == region->packets || side->ref_next + fragments > region->sent_ref_entries )
                return;

            channel->queue_head = descriptor->next;
            if ( channel->queue_head == RN_DESCRIPTOR_NONE )
                channel->queue_tail = RN_DESCRIPTOR_NONE;
            channel->queued[descriptor->priority]--;
            descriptor->first_number = side->next_number;
            descriptor->fragments = (uint16_t) fragments;
            descriptor->fragments_acked = 0;
            side->ring_next = ring + span;
            side->sent_bytes += descriptor->bytes;

            // one packet per message: a sent-map entry, its index slot and its references

            const uint32_t entry_index = ( side->sent_head + side->sent_count ) % region->packets;
            rn_sent_packet_t * entry = &region->sent_packets[entry_index];
            entry->packet_id = ++side->next_packet_id;
            entry->send_time = (double) tick;
            entry->ref_first = side->ref_next;
            entry->ref_count = (uint16_t) fragments;
            entry->unreliable_receipts = 0;
            side->sent_count++;
            const uint32_t index_slot = rn_sent_index_slot( region, entry->packet_id );
            check( !region->sent_index[index_slot].used );
            region->sent_index[index_slot].packet_id = entry->packet_id;
            region->sent_index[index_slot].entry = entry_index;
            region->sent_index[index_slot].used = 1;

            uint32_t chunk = descriptor->chunk;
            for ( uint32_t f = 0; f < fragments; f++ )
            {
                const uint64_t number = side->next_number++;
                region->sent_refs[side->ref_next++] = (uint32_t) number;

                rn_send_number_t * slot = &region->send_numbers[number & ( region->window - 1 )];
                check( slot->descriptor == RN_DESCRIPTOR_NONE || slot->acked );     // a live number is never overwritten
                slot->number = number;
                slot->descriptor = d;
                slot->fragment = (uint16_t) f;
                slot->ring_start = ring + (uint64_t) f * RN_STORE_LARGE_BYTES;
                slot->ring_units = RN_STORE_LARGE_BYTES / 16;
                slot->last_send = (double) tick;
                slot->acked = 0;

                // the write: from the chunk straight into the frame

                const uint32_t offset = f * RN_STORE_LARGE_BYTES;
                const uint32_t bytes = descriptor->bytes - offset < (uint32_t) RN_STORE_LARGE_BYTES ? descriptor->bytes - offset : (uint32_t) RN_STORE_LARGE_BYTES;
                check( chunk != RN_CHUNK_NONE );
                memcpy( frame, rn_send_store_chunk_data( &region->store, chunk ), bytes );
                chunk = rn_send_store_chunk_next( &region->store, chunk );

                // the receive: the window check, the bitset, the descriptor, the ring copy

                check( number >= side->receiver_next && number < side->receiver_next + receiver->window );
                const uint32_t r = (uint32_t) ( number & ( receiver->window - 1 ) );
                uint64_t * word = &receiver->received_bits[r / 64];
                check( ( *word & ( 1ULL << ( r % 64 ) ) ) == 0 );
                *word |= 1ULL << ( r % 64 );
                rn_receive_descriptor_t * rd = &receiver->receive_descriptors[r];
                check( !rd->live || rd->number == number );
                rd->number = number;
                rd->live = 1;
                rd->code = ROCKETNET_RELIABLE_ORDERED;
                rd->channel = (uint16_t) c;
                rd->ordering = descriptor->ordering;
                rd->sequencing = 0;
                rd->ring = (uint32_t) ( slot->ring_start % receiver->ring_bytes );
                rd->bytes = bytes;
                rd->block_have = (uint16_t) ( f + 1 );
                rd->block_count = (uint16_t) fragments;
                check( rn_region_ring_span_valid( receiver, rd->ring, bytes ) );
                memcpy( receiver->receive_ring + rd->ring, frame, bytes );
                for ( uint32_t b = 0; b < bytes; b++ )
                    check( receiver->receive_ring[rd->ring + b] == rn_pattern( descriptor->serial, offset + b ) );

                // the held index: ordered messages behind their predecessor are held, then drained

                const uint64_t key = ( (uint64_t) c << 32 ) | descriptor->ordering;
                const uint32_t held = rn_held_slot( receiver, key );
                receiver->held_index[held].key = key;
                receiver->held_index[held].number_slot = r;
                receiver->held_index[held].used = 1;
            }

            // the message is whole at the receiver: deliver from the ring, release the hold

            rn_channel_receive_t * rc = &receiver->channel_receive[c];
            check( rc->ordered_read == descriptor->ordering );
            const uint64_t key = ( (uint64_t) c << 32 ) | descriptor->ordering;
            const uint32_t held = rn_held_slot( receiver, key );
            check( receiver->held_index[held].used );
            receiver->held_index[held].used = 0;
            receiver->held_index[held].key = 0;
            rc->ordered_read = descriptor->ordering + 1;
            rc->sequenced_watermark = 0;

            const uint32_t delivery_index = (uint32_t) ( side->delivered_messages % receiver->delivery_entries );
            rn_delivery_t * delivery = &receiver->deliveries[delivery_index];
            delivery->data = receiver->receive_ring + ( ring % receiver->ring_bytes );
            delivery->bytes = (int32_t) descriptor->bytes;
            delivery->channel = (uint16_t) c;
            delivery->code = ROCKETNET_RELIABLE_ORDERED;
            delivery->serial = descriptor->serial;
            check( rn_region_contains( receiver, delivery->data, descriptor->bytes ) );
            for ( uint32_t b = 0; b < descriptor->bytes; b += 97 )
                check( delivery->data[b] == rn_pattern( descriptor->serial, b ) );
            side->delivered_messages++;
            side->delivered_bytes += descriptor->bytes;

            // retire the receive slots and advance R

            for ( uint32_t f = 0; f < fragments; f++ )
            {
                const uint32_t r = (uint32_t) ( ( descriptor->first_number + f ) & ( receiver->window - 1 ) );
                receiver->receive_descriptors[r].live = 0;
            }
            for ( ;; )
            {
                const uint32_t r = (uint32_t) ( side->receiver_next & ( receiver->window - 1 ) );
                uint64_t * word = &receiver->received_bits[r / 64];
                if ( ( *word & ( 1ULL << ( r % 64 ) ) ) == 0 )
                    break;
                *word &= ~( 1ULL << ( r % 64 ) );
                side->receiver_next++;
            }

            // a sequenced hold and a receipt per message, round the rings

            if ( receiver->holds > 0 )
            {
                rn_sequenced_hold_t * hold = &receiver->sequenced_holds[descriptor->serial % receiver->holds];
                const uint32_t bytes = descriptor->bytes < (uint32_t) sizeof(hold->data) ? descriptor->bytes : (uint32_t) sizeof(hold->data);
                memcpy( hold->data, delivery->data, bytes );
                hold->bytes = bytes;
                hold->channel = (uint16_t) c;
                hold->ordering = descriptor->ordering;
                hold->sequencing = 0;
                hold->used = 1;
                check( hold->data[0] == rn_pattern( descriptor->serial, 0 ) );
            }
        }
    }
}

static void rn_soak_ack( rn_soak_side_t * side, uint64_t tick, bool all )
{
    rn_region_t * region = side->sender;

    // ack the older half always, and a random part of the rest; free a message when every fragment is acked

    uint32_t acked_packets = 0;
    for ( uint32_t i = 0; i < side->sent_count; i++ )
    {
        rn_sent_packet_t * entry = &region->sent_packets[( side->sent_head + i ) % region->packets];
        if ( entry->packet_id == 0 )
            continue;
        if ( !all && entry->send_time >= (double) tick && ( rn_random() & 1 ) )
            continue;
        const uint32_t index_slot = rn_sent_index_slot( region, entry->packet_id );
        check( region->sent_index[index_slot].used && region->sent_index[index_slot].packet_id == entry->packet_id );
        for ( uint32_t r = 0; r < entry->ref_count; r++ )
        {
            const uint64_t number = region->sent_refs[entry->ref_first + r];
            rn_send_number_t * slot = &region->send_numbers[number & ( region->window - 1 )];
            check( (uint32_t) slot->number == (uint32_t) number && !slot->acked );
            slot->acked = 1;
            side->acked_fragments++;
            rn_send_descriptor_t * descriptor = &region->send_descriptors[slot->descriptor];
            check( descriptor->in_use );
            if ( ++descriptor->fragments_acked == descriptor->fragments )
            {
                if ( side->receipt_count == region->receipt_entries )
                {
                    side->receipt_head = ( side->receipt_head + 1 ) % region->receipt_entries;
                    side->receipt_count--;
                }
                rn_receipt_t * receipt = &region->receipts[( side->receipt_head + side->receipt_count ) % region->receipt_entries];
                receipt->serial = descriptor->serial;
                receipt->status = ROCKETNET_RECEIPT_ACKED;
                side->receipt_count++;
                rn_region_send_free( region, slot->descriptor );
            }
        }
        entry->packet_id = 0;
        acked_packets++;
    }

    // compact: pop acked entries off the head, rebuild the index and the reference ring from what remains

    for ( uint32_t s = 0; s < region->sent_index_entries; s++ )
        region->sent_index[s].used = 0;
    uint32_t kept = 0;
    uint32_t refs = 0;
    static uint32_t scratch_refs[1 << 16];
    for ( uint32_t i = 0; i < side->sent_count; i++ )
    {
        rn_sent_packet_t * entry = &region->sent_packets[( side->sent_head + i ) % region->packets];
        if ( entry->packet_id == 0 )
            continue;
        for ( uint32_t r = 0; r < entry->ref_count; r++ )
            scratch_refs[refs + r] = region->sent_refs[entry->ref_first + r];
        rn_sent_packet_t moved = *entry;
        moved.ref_first = refs;
        refs += moved.ref_count;
        const uint32_t to = ( side->sent_head + kept ) % region->packets;
        region->sent_packets[to] = moved;
        const uint32_t index_slot = rn_sent_index_slot( region, moved.packet_id );
        region->sent_index[index_slot].packet_id = moved.packet_id;
        region->sent_index[index_slot].entry = to;
        region->sent_index[index_slot].used = 1;
        kept++;
    }
    check( refs <= region->sent_ref_entries );
    memcpy( region->sent_refs, scratch_refs, refs * sizeof(uint32_t) );
    side->sent_count = kept;
    side->ref_next = refs;

    // advance the oldest unacked number

    while ( side->oldest_unacked < side->next_number && region->send_numbers[side->oldest_unacked & ( region->window - 1 )].acked )
        side->oldest_unacked++;
    (void) acked_packets;
}

static void test_channels_allocator_no_live_allocation()
{
    // A server arena of 16 slots at the defaults, and 16 client arenas, one
    // region each, as 16 client objects have. The clients share the server's
    // wire fields but run a short send queue, so between the two ends both
    // refusals are reached: at the defaults the store's 451 chunks run out
    // before the 1024 descriptors can (docs/CHANNELS.md 14), so the server
    // end is store-bound and the client end is queue-bound.

    const int num_slots = 16;
    rocketnet_message_config_t server_config;
    rocketnet_message_default_config( &server_config );
    rocketnet_message_config_t client_config = server_config;
    client_config.send_queue_messages = 64;

    static rocketnet_counters_t server_counters;
    static rocketnet_counters_t client_counters[16];
    memset( &server_counters, 0, sizeof(server_counters) );
    memset( client_counters, 0, sizeof(client_counters) );
    rn_arena_t * server = rn_arena_create( NULL, &server_config, num_slots, &server_counters );
    check( server );
    rn_arena_t * clients[16];
    for ( int s = 0; s < num_slots; s++ )
    {
        clients[s] = rn_arena_create( NULL, &client_config, 1, &client_counters[s] );
        check( clients[s] );
    }

    // even sides send server to client, odd sides client to server

    static rn_soak_side_t sides[2 * 16];
    memset( sides, 0, sizeof(sides) );
    for ( int s = 0; s < num_slots; s++ )
    {
        sides[2 * s].sender = rn_arena_region( server, s );
        sides[2 * s].receiver = rn_arena_region( clients[s], 0 );
        sides[2 * s].sender_counters = &server_counters;
        sides[2 * s].config = &server_config;
        sides[2 * s + 1].sender = rn_arena_region( clients[s], 0 );
        sides[2 * s + 1].receiver = rn_arena_region( server, s );
        sides[2 * s + 1].sender_counters = &client_counters[s];
        sides[2 * s + 1].config = &client_config;
    }

    const uint64_t hook_mallocs = rn_hook_mallocs;
    const uint64_t hook_frees = rn_hook_frees;

    rn_trap_phase = "connect";
    rn_trap_armed = 1;

    // connect: every region is carved, allocating nothing

    for ( int s = 0; s < num_slots; s++ )
    {
        rn_region_carve( rn_arena_region( server, s ) );
        rn_region_carve( rn_arena_region( clients[s], 0 ) );
    }

    const int ticks = 60;
    uint64_t time = 0;
    for ( int tick = 1; tick <= ticks; tick++ )
    {
        for ( int round = 0; round < 2; round++ )
        {
            time++;
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "send";
                rn_soak_send( &sides[i] );
            }
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "receive";
                rn_soak_write_and_receive( &sides[i], time );
                rn_soak_check_accounting( sides[i].sender );
            }
            for ( int i = 0; i < 2 * num_slots; i++ )
            {
                rn_trap_phase = "ack";
                rn_soak_ack( &sides[i], time, false );
                rn_soak_check_accounting( sides[i].sender );
            }
        }

        // a reconnect of every connection every 20th tick, with sends queued
        // and in flight: the carve takes everything back

        if ( tick % 20 == 0 )
        {
            rn_trap_phase = "reconnect";
            for ( int s = 0; s < num_slots; s++ )
            {
                rn_soak_send( &sides[2 * s] );
                rn_soak_send( &sides[2 * s + 1] );
                rn_region_t * ends[2] = { rn_arena_region( server, s ), rn_arena_region( clients[s], 0 ) };
                for ( int e = 0; e < 2; e++ )
                {
                    check( ends[e]->descriptors_free < ends[e]->queue );
                    rn_region_carve( ends[e] );
                    check( ends[e]->descriptors_free == ends[e]->queue );
                    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
                        check( rn_send_store_free_chunks( &ends[e]->store, c ) == rn_send_store_total_chunks( &ends[e]->store, c ) );
                    rn_soak_check_accounting( ends[e] );
                }
                rn_soak_reset( &sides[2 * s] );
                rn_soak_reset( &sides[2 * s + 1] );
            }
        }
    }

    // drain: write what is queued, ack everything, and every store and pool is whole again

    rn_trap_phase = "drain";
    for ( int pass = 0; pass < 64; pass++ )
    {
        time++;
        bool queued = false;
        for ( int i = 0; i < 2 * num_slots; i++ )
        {
            rn_soak_write_and_receive( &sides[i], time );
            rn_soak_ack( &sides[i], time, true );
            for ( uint32_t c = 0; c < sides[i].sender->num_channels; c++ )
                if ( sides[i].sender->channel_send[c].queue_head != RN_DESCRIPTOR_NONE )
                    queued = true;
        }
        if ( !queued )
            break;
    }
    for ( int i = 0; i < 2 * num_slots; i++ )
    {
        const rn_region_t * region = sides[i].sender;
        check( sides[i].sent_count == 0 );
        check( region->descriptors_free == region->queue );
        for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
            check( rn_send_store_free_chunks( &region->store, c ) == rn_send_store_total_chunks( &region->store, c ) );
    }

    rn_trap_armed = 0;
    rn_trap_phase = "idle";
    check( rn_hook_mallocs == hook_mallocs && rn_hook_frees == hook_frees );

    // every refusal is its own counter, on its own owner, and the load reached
    // capacity on both ends: the store on the server's, the queue on the clients'

    uint64_t server_store = 0, server_queue = 0, client_store = 0, client_queue = 0;
    for ( int s = 0; s < num_slots; s++ )
    {
        server_store += sides[2 * s].refused_store;
        server_queue += sides[2 * s].refused_queue;
        client_store += sides[2 * s + 1].refused_store;
        client_queue += sides[2 * s + 1].refused_queue;
        check( sides[2 * s + 1].refused_store == rn_counter( &client_counters[s], ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) );
        check( sides[2 * s + 1].refused_queue == rn_counter( &client_counters[s], ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) );
        check( sides[2 * s].delivered_messages > 0 && sides[2 * s + 1].delivered_messages > 0 );
        check( sides[2 * s].delivered_bytes == sides[2 * s].sent_bytes );
        check( sides[2 * s + 1].delivered_bytes == sides[2 * s + 1].sent_bytes );
    }
    check( server_store == rn_counter( &server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) );
    check( server_queue == rn_counter( &server_counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) );
    check( server_store > 0 && server_queue == 0 );
    check( client_queue > 0 );

    for ( int s = 0; s < num_slots; s++ )
        rn_arena_destroy( clients[s] );
    rn_arena_destroy( server );
}

// ----------------------------------------------------------------
//            the UNRELIABLE path (docs/CHANNELS.md 5, 6.3, 6.4, 9.3,
//          11.2): the pure module over layer 1's region and layer 2's
//                                codec
// ----------------------------------------------------------------

// A small rig for the UNRELIABLE module: one region from one arena, a FIFO
// the caller owns (nothing here allocates; R1/R4), and a delivered list big
// enough for the unit cases. The trap is armed inside each case around the
// send, fill and receive calls so a live malloc or free on those paths
// aborts (R1; docs/CHANNELS.md 1).

struct channels_unreliable_rig_t
{
    rocketnet_message_config_t config;
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_unreliable_message_t queue[64];
    rn_unreliable_t unreliable;
    rn_delivery_t deliveries[64];
};

static void channels_unreliable_setup( channels_unreliable_rig_t * rig, int queue_capacity )
{
    memset( &rig->counters, 0, sizeof(rig->counters) );
    rocketnet_message_default_config( &rig->config );
    rig->arena = rn_arena_create( NULL, &rig->config, 1, &rig->counters );
    check( rig->arena );
    rig->region = rn_arena_region( rig->arena, 0 );
    rn_region_carve( rig->region );
    memset( rig->queue, 0, sizeof(rig->queue) );
    rn_unreliable_init( &rig->unreliable, rig->region, rig->queue, (uint32_t) queue_capacity );
    memset( rig->deliveries, 0, sizeof(rig->deliveries) );
}

static void channels_unreliable_teardown( channels_unreliable_rig_t * rig )
{
    rn_arena_destroy( rig->arena );
}

static void channels_unreliable_fill_pattern( uint8_t * data, int bytes, int salt )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 31 + salt );
}

// counts the code-0 records in a layer 2 payload, checking their bytes

static int channels_unreliable_parse( const uint8_t * payload, int bytes, int expected_code,
                                      const uint8_t * expect[], int expect_bytes[], int expect_count,
                                      uint8_t * body )
{
    struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 1 ) );
    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    struct rocketnet_message_record_t record;
    int count = 0;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        check( count < expect_count );
        check( record.code == expected_code );
        if ( body )
            body[count] = (uint8_t) record.bytes;
        if ( expect[count] )
            check( record.bytes == expect_bytes[count] && memcmp( record.data, expect[count], (size_t) record.bytes ) == 0 );
        count++;
    }
    check( count == expect_count );
    return count;
}

static void test_channels_unreliable_small_messages_share_one_frame()
{
    // many small unreliable messages pack into one RECORDS payload (9.3 step
    // 2) as code-0 records with the layer 2 writer, and each chunk is freed
    // the moment it is written (9.3 step 3)

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t a[8], b[32], c[64];
    channels_unreliable_fill_pattern( a, sizeof(a), 1 );
    channels_unreliable_fill_pattern( b, sizeof(b), 2 );
    channels_unreliable_fill_pattern( c, sizeof(c), 3 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, a, sizeof(a), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, b, sizeof(b), 0, 1 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, c, sizeof(c), 0, 2 ) == 1 );

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    memset( frame, 0, sizeof(frame) );
    const int length = rn_unreliable_fill( &rig.unreliable, frame, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( length > 0 && length <= ROCKETNET_MAX_PAYLOAD_BYTES );

    // one packet carries all three; the FIFO is empty and the store is whole
    // again (each chunk freed as written, R5 / 9.3 step 3)

    check( rig.unreliable.count == 0 );
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
        check( rn_send_store_free_chunks( &rig.region->store, cl ) == rn_send_store_total_chunks( &rig.region->store, cl ) );

    const uint8_t * expect[3] = { a, b, c };
    int expect_bytes[3] = { (int)sizeof(a), (int)sizeof(b), (int)sizeof(c) };
    uint8_t body[3];
    check( channels_unreliable_parse( frame, length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect, expect_bytes, 3, body ) == 3 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_record_never_split_across_packets()
{
    // a record that does not fit ends the packet and stays queued; it is
    // never split across packets (9.3 step 2). The head that fills the frame
    // completely is written whole, the next waits for its own packet.

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t big[1188], rest[100];
    channels_unreliable_fill_pattern( big, sizeof(big), 4 );
    channels_unreliable_fill_pattern( rest, sizeof(rest), 5 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, big, sizeof(big), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, rest, sizeof(rest), 0, 0 ) == 1 );

    // 1188 + 3 record bytes fill the 1192 frame to the last byte; the 100-byte
    // tail cannot share it, so it waits whole

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int length = rn_unreliable_fill( &rig.unreliable, frame, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( length == ROCKETNET_MAX_PAYLOAD_BYTES );
    const uint8_t * expect[1] = { big };
    int expect_bytes[1] = { (int)sizeof(big) };
    check( channels_unreliable_parse( frame, length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect, expect_bytes, 1, NULL ) == 1 );
    check( rig.unreliable.count == 1 );

    // the tail travels whole in its own packet

    alignas( 16 ) static uint8_t tail[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int tail_length = rn_unreliable_fill( &rig.unreliable, tail, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( tail_length > 0 );
    const uint8_t * expect_tail[1] = { rest };
    int expect_tail_bytes[1] = { (int)sizeof(rest) };
    check( channels_unreliable_parse( tail, tail_length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect_tail, expect_tail_bytes, 1, NULL ) == 1 );
    check( rig.unreliable.count == 0 );

    rn_trap_armed = 0;
    rn_trap_phase = "idle";
    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_large_message_travels_alone()
{
    // a message longer than one packet is planned alone in its own large
    // payload (L2.3; 9.3 step 1), capped by the record's u16 length; the
    // small tail behind it waits for its own packet

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t large[4000], small[50];
    channels_unreliable_fill_pattern( large, sizeof(large), 6 );
    channels_unreliable_fill_pattern( small, sizeof(small), 7 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, large, sizeof(large), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, small, sizeof(small), 0, 0 ) == 1 );

    check( rn_unreliable_next_packet_bytes( &rig.unreliable ) == (int) sizeof(large) + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 );

    alignas( 16 ) static uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES * 8];
    const int length = rn_unreliable_fill( &rig.unreliable, payload, (int) sizeof(large) + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 );
    check( length == (int) sizeof(large) + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 );
    const uint8_t * expect[1] = { large };
    int expect_bytes[1] = { (int)sizeof(large) };
    check( channels_unreliable_parse( payload, length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect, expect_bytes, 1, NULL ) == 1 );

    // the small message stayed behind, whole

    check( rig.unreliable.count == 1 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";
    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_single_zero_copy_at_payload_root()
{
    // the zero-copy SINGLE form (11.2): the message is written at the
    // 16-aligned payload root, costs one byte (the form byte, last), and
    // touches no module state

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    memset( frame, 0, sizeof(frame) );

    check( rn_unreliable_single_capacity( ROCKETNET_MAX_PAYLOAD_BYTES ) == ROCKETNET_MESSAGE_SINGLE_BYTES );
    check( rn_unreliable_single_begin( frame, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MESSAGE_SINGLE_BYTES ) == frame );
    check( rn_unreliable_single_begin( frame, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MESSAGE_SINGLE_BYTES + 1 ) == NULL );
    check( rn_unreliable_single_begin( frame, 0, 10 ) == NULL );

    uint8_t message[300];
    channels_unreliable_fill_pattern( message, sizeof(message), 8 );

    // write at the root, finish writes the form byte last, one byte of cost

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-zero-copy";
    uint8_t * root = rn_unreliable_single_begin( frame, ROCKETNET_MAX_PAYLOAD_BYTES, (int) sizeof(message) );
    check( root == frame && ( ( (uintptr_t) root & 15 ) == 0 ) );
    memcpy( root, message, sizeof(message) );
    const int length = rn_unreliable_single_finish( frame, ROCKETNET_MAX_PAYLOAD_BYTES, (int) sizeof(message) );
    check( length == (int) sizeof(message) + 1 );
    check( frame[length - 1] == ROCKETNET_MESSAGE_FORM_SINGLE );

    const uint8_t * expect[1] = { message };
    int expect_bytes[1] = { (int)sizeof(message) };
    check( channels_unreliable_parse( frame, length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect, expect_bytes, 1, NULL ) == 1 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( rn_unreliable_single_finish( frame, ROCKETNET_MAX_PAYLOAD_BYTES, ROCKETNET_MAX_PAYLOAD_BYTES ) == 0 );
    check( rn_unreliable_single_finish( frame, ROCKETNET_MAX_PAYLOAD_BYTES, 0 ) == 0 );
}

static void test_channels_unreliable_store_full_drops_and_counts()
{
    // when the send store has no chunk of the class the message needs, the
    // send is dropped and counted, never grown (R1: unreliable data is
    // dropped and counted; docs/CHANNELS.md 1)

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );
    rig.config.send_buffer_bytes = 12288;
    rig.config.max_message_bytes = 1191;      // the minimum, so the store's large class holds a whole maximal block (14)
    rn_arena_destroy( rig.arena );
    rig.arena = rn_arena_create( NULL, &rig.config, 1, &rig.counters );
    check( rig.arena );
    rig.region = rn_arena_region( rig.arena, 0 );
    rn_region_carve( rig.region );
    rn_unreliable_init( &rig.unreliable, rig.region, rig.queue, 64 );

    const uint32_t small_chunks = rn_send_store_total_chunks( &rig.region->store, RN_STORE_SMALL );

    uint8_t message[64];
    channels_unreliable_fill_pattern( message, sizeof(message), 9 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    int accepted = 0;
    for ( uint32_t i = 0; i < small_chunks + 4; i++ )
        accepted += rn_unreliable_send( &rig.unreliable, message, sizeof(message), 0, 0 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( accepted == (int) small_chunks );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 4 );
    check( rig.unreliable.count == small_chunks );

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_delivery_budget_drops_and_counts()
{
    // when the delivered list (Du) is full the rest are dropped and counted
    // (6.3 step 4), which unreliable semantics allow

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    // a RECORDS payload of five code-0 records

    alignas( 16 ) static uint8_t payload[128];
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof(payload) );
    for ( int i = 0; i < 5; i++ )
    {
        struct rocketnet_message_record_t record;
        memset( &record, 0, sizeof(record) );
        record.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        record.data = (const uint8_t*) "abc";
        record.bytes = 3;
        check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    }
    const int length = rocketnet_message_writer_finish( &writer );
    check( length > 0 );

    struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 1 ) );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-receive";
    int out_count = 0;
    const int violation = rn_unreliable_receive( &rig.unreliable, payload, length, &limits, rig.deliveries, 2, &out_count );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( out_count == 2 );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL ) == 3 );
    for ( int i = 0; i < 2; i++ )
        check( rig.deliveries[i].bytes == 3 && rig.deliveries[i].code == ROCKETNET_MESSAGE_CODE_UNRELIABLE );

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_failed_send_drops_and_counts()
{
    // a packet whose finish_packet fails drops the unreliable records it
    // carried and counts each (6.4, Failed send): the chunks were already
    // freed at write, so only the count changes

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t a[16], b[16], c[16];
    channels_unreliable_fill_pattern( a, sizeof(a), 11 );
    channels_unreliable_fill_pattern( b, sizeof(b), 12 );
    channels_unreliable_fill_pattern( c, sizeof(c), 13 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, a, sizeof(a), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, b, sizeof(b), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, c, sizeof(c), 0, 0 ) == 1 );

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int length = rn_unreliable_fill( &rig.unreliable, frame, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( length > 0 );
    check( rig.unreliable.count == 0 );

    // the caller's finish_packet refused: drop and count the records it carried

    rn_unreliable_packet_failed( &rig.unreliable, 3 );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 3 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_chunk_freed_when_written()
{
    // the send store holds a message only until its record is written; the
    // chunk is freed at that moment (9.3 step 3, R5), across the store's
    // classes

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t small[64], medium[400];
    channels_unreliable_fill_pattern( small, sizeof(small), 14 );
    channels_unreliable_fill_pattern( medium, sizeof(medium), 15 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, small, sizeof(small), 0, 0 ) == 1 );
    check( rn_unreliable_send( &rig.unreliable, medium, sizeof(medium), 0, 0 ) == 1 );

    uint32_t used_before[RN_STORE_NUM_CLASSES];
    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
        used_before[cl] = rn_send_store_total_chunks( &rig.region->store, cl )
            - rn_send_store_free_chunks( &rig.region->store, cl );
    check( used_before[RN_STORE_SMALL] == 1 && used_before[RN_STORE_MEDIUM] == 1 );

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int length = rn_unreliable_fill( &rig.unreliable, frame, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( length > 0 );

    // both chunks are back the moment the records are written

    for ( int cl = 0; cl < RN_STORE_NUM_CLASSES; cl++ )
        check( rn_send_store_free_chunks( &rig.region->store, cl ) == rn_send_store_total_chunks( &rig.region->store, cl ) );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_block_not_corrupted_across_fragmented_store()
{
    // a block (longer than one large chunk) travels alone, its record copied
    // chunk-chain by chunk-chain (L2.3), never contiguously across a chunk
    // boundary. Filling one block frees chunks 0 and 1, threading the class
    // free list as 1 -> 0, so the next block lands on non-adjacent chunks;
    // both blocks must come out byte-identical to what was sent.

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );

    uint8_t big1[1188], big2[1188];
    channels_unreliable_fill_pattern( big1, sizeof(big1), 16 );
    channels_unreliable_fill_pattern( big2, sizeof(big2), 17 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, big1, sizeof(big1), 0, 0 ) == 1 );

    alignas( 16 ) static uint8_t frame1[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int len1 = rn_unreliable_fill( &rig.unreliable, frame1, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( len1 == ROCKETNET_MAX_PAYLOAD_BYTES );
    const uint8_t * expect1[1] = { big1 };
    int expect1_bytes[1] = { (int)sizeof(big1) };
    check( channels_unreliable_parse( frame1, len1, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect1, expect1_bytes, 1, NULL ) == 1 );

    // the second block now sits on chunks 1 then 0 (the free list reversed)

    check( rn_unreliable_send( &rig.unreliable, big2, sizeof(big2), 0, 0 ) == 1 );
    alignas( 16 ) static uint8_t frame2[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int len2 = rn_unreliable_fill( &rig.unreliable, frame2, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( len2 == ROCKETNET_MAX_PAYLOAD_BYTES );
    const uint8_t * expect2[1] = { big2 };
    int expect2_bytes[1] = { (int)sizeof(big2) };
    check( channels_unreliable_parse( frame2, len2, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect2, expect2_bytes, 1, NULL ) == 1 );

    check( rig.unreliable.count == 0 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    channels_unreliable_teardown( &rig );
}

static void test_channels_unreliable_message_past_u16_length_refused()
{
    // a message longer than the record's u16 length can never be carried by an
    // unreliable record: the send refuses and counts it (SEND_FAILED) instead of
    // enqueuing it, so the FIFO never wedges on a message it can never send
    // (R1: unreliable data is dropped and counted, never stuck)

    channels_unreliable_rig_t rig;
    channels_unreliable_setup( &rig, 16 );
    rig.config.max_message_bytes = 65536;       // allowed by the config, past the u16 cap
    rig.config.send_buffer_bytes = 1 << 20;     // the store's large class must hold that whole block (14)
    rn_arena_destroy( rig.arena );
    rig.arena = rn_arena_create( NULL, &rig.config, 1, &rig.counters );
    check( rig.arena );
    rig.region = rn_arena_region( rig.arena, 0 );
    rn_region_carve( rig.region );
    rn_unreliable_init( &rig.unreliable, rig.region, rig.queue, 16 );

    static uint8_t too_large[65536];
    channels_unreliable_fill_pattern( too_large, sizeof(too_large), 18 );
    uint8_t small[50];
    channels_unreliable_fill_pattern( small, sizeof(small), 19 );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-send";
    check( rn_unreliable_send( &rig.unreliable, too_large, (int) sizeof(too_large), 0, 0 ) == 0 );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == 1 );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rig.unreliable.count == 0 );

    // the FIFO is not wedged: a valid message behind it still sends and delivers

    check( rn_unreliable_send( &rig.unreliable, small, sizeof(small), 0, 0 ) == 1 );
    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    const int length = rn_unreliable_fill( &rig.unreliable, frame, ROCKETNET_MAX_PAYLOAD_BYTES );
    check( length > 0 );
    const uint8_t * expect[1] = { small };
    int expect_bytes[1] = { (int)sizeof(small) };
    check( channels_unreliable_parse( frame, length, ROCKETNET_MESSAGE_CODE_UNRELIABLE, expect, expect_bytes, 1, NULL ) == 1 );
    check( rig.unreliable.count == 0 );
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    channels_unreliable_teardown( &rig );
}

// the lossy-simulator walk: a sender pushes small, packet-sized (zero-copy)
// and larger-than-a-packet unreliable messages through
// rocketnet_network_simulator at a loss and jitter, and the receiver parses
// with layer 2's reader and delivers through rn_unreliable_receive. Every
// delivered message is byte-identical to one sent; at 0 percent loss every
// queued message is delivered, sent equals delivered plus lost plus counted
// drops (R8), and no live allocation happens anywhere on the send or receive
// path (R1).

struct channels_unreliable_wire_t
{
    rocketnet_network_simulator_t * wire;
    rocketnet_address_t address;
    double time;
};

struct channels_unreliable_sent_t
{
    uint8_t data[4096];
    int bytes;
    int used;
};

static struct channels_unreliable_sent_t channels_unreliable_sent[4096];
static int channels_unreliable_sent_count = 0;

static void channels_unreliable_register_sent( const uint8_t * data, int bytes )
{
    check( channels_unreliable_sent_count < 4096 && bytes > 0 && bytes <= 4096 );
    memcpy( channels_unreliable_sent[channels_unreliable_sent_count].data, data, (size_t) bytes );
    channels_unreliable_sent[channels_unreliable_sent_count].bytes = bytes;
    channels_unreliable_sent[channels_unreliable_sent_count].used = 1;
    channels_unreliable_sent_count++;
}

// marks one sent message delivered; returns 1 when a byte-identical sent
// message was found (and consumed), 0 when the delivery matched nothing

static int channels_unreliable_delivered( const uint8_t * data, int bytes )
{
    for ( int i = 0; i < channels_unreliable_sent_count; i++ )
    {
        if ( !channels_unreliable_sent[i].used )
            continue;
        if ( channels_unreliable_sent[i].bytes != bytes )
            continue;
        if ( memcmp( channels_unreliable_sent[i].data, data, (size_t) bytes ) != 0 )
            continue;
        channels_unreliable_sent[i].used = 0;
        return 1;
    }
    return 0;
}

// counts the code-0 records in a layer 2 payload (SINGLE or RECORDS), the
// wire-side measurement of how many messages a frame carried -- independent
// of the delivery registry, so wire loss is never its complement

static int channels_unreliable_count_code0( const uint8_t * payload, int bytes )
{
    struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 1 ) );
    struct rocketnet_message_reader_t reader;
    const int violation = rocketnet_message_reader_begin( &reader, payload, bytes, &limits );
    check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    int count = 0;
    struct rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE );
        count++;
    }
    return count;
}

static void test_channels_unreliable_over_the_lossy_simulator( float loss_percent )
{
    channels_unreliable_sent_count = 0;

    // R8 conservation is measured on independent sides: attempted at the send
    // calls, refused at the module's refusal (cross-checked against its
    // store-full counter), delivered at the receive loop (each byte-verified
    // against the sent registry), the FIFO remainder as the module's own
    // count, wire loss as the gap between the code-0 records parsed into the
    // frames this test sent and parsed out of the frames it received, and the
    // receiver's full-budget drops as the module's counter. None is the
    // complement of another, so the final equality is a real check.

    int attempted = 0;
    int refused = 0;
    int delivered_count = 0;
    int sent_messages = 0;
    int received_messages = 0;
    int packets_sent = 0;
    int packets_received = 0;

    struct channels_unreliable_rig_t sender, receiver;
    channels_unreliable_setup( &sender, 16 );
    channels_unreliable_setup( &receiver, 16 );

    struct channels_unreliable_wire_t wire = { 0 };
    wire.wire = rocketnet_network_simulator_create( NULL, 8192, 0xC0FFEE + (uint64_t)( loss_percent * 10.0f ) );
    check( wire.wire );
    wire.time = 0.0;
    // a real address: an unset (NONE) address never equals another, itself
    // included (rocketnet_address_equal), so the simulator would match nothing
    check( rocketnet_address_parse( &wire.address, "127.0.0.1:40000" ) );
    rocketnet_network_simulator_config( wire.wire, 0.02, 0.02, loss_percent, 0.0f );

    rn_trap_armed = 1;
    rn_trap_phase = "unreliable-wire";
    channels_random_t random;
    random.state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)( loss_percent * 10.0f );

    // the zero-copy path writes straight into a frame: the message at the
    // 16-aligned root, the form byte last. The receive buffer is a frame of
    // its own, sized for the large RECORDS payloads this walk also sends.

    alignas( 16 ) static uint8_t zc_frame[ROCKETNET_MAX_PAYLOAD_BYTES];
    alignas( 16 ) static uint8_t rx_frame[ROCKETNET_MAX_PAYLOAD_BYTES * 2];
    struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 1 ) );

    uint8_t small[80], big[1400];       // 1400 > ROCKETNET_MAX_PAYLOAD_BYTES, still inside the wire's receive ceiling (1426)

    for ( uint64_t tick = 1; tick <= 120; tick++ )
    {
        wire.time += 1.0 / 60.0;

        // small messages to aggregate

        for ( int i = 0; i < 4; i++ )
        {
            const int bytes = channels_random_range( &random, 1, 80 );
            channels_unreliable_fill_pattern( small, bytes, (int) ( tick * 7 + i ) );
            attempted++;
            if ( rn_unreliable_send( &sender.unreliable, small, bytes, 0, 0 ) )
                channels_unreliable_register_sent( small, bytes );
            else
                refused++;
        }

        // a larger-than-a-packet message now and then

        if ( tick % 5 == 0 )
        {
            channels_unreliable_fill_pattern( big, sizeof(big), (int) tick );
            attempted++;
            if ( rn_unreliable_send( &sender.unreliable, big, sizeof(big), 0, 0 ) )
                channels_unreliable_register_sent( big, sizeof(big) );
            else
                refused++;
        }

        // a zero-copy packet-sized message now and then

        if ( tick % 11 == 0 )
        {
            const int request = 1000;
            uint8_t * root = rn_unreliable_single_begin( zc_frame, ROCKETNET_MAX_PAYLOAD_BYTES, request );
            check( root == zc_frame );
            channels_unreliable_fill_pattern( root, request, (int) ( tick * 13 ) );
            const int zc_length = rn_unreliable_single_finish( zc_frame, ROCKETNET_MAX_PAYLOAD_BYTES, request );
            check( zc_length == request + 1 );
            attempted++;
            channels_unreliable_register_sent( root, request );
            check( channels_unreliable_count_code0( zc_frame, zc_length ) == 1 );
            sent_messages++;
            packets_sent++;
            rocketnet_network_simulator_send( wire.wire, &wire.address, &wire.address, zc_frame, zc_length, wire.time );
        }

        // write up to the configured packets per update, each into one frame

        for ( int p = 0; p < 4; p++ )
        {
            const int plan = rn_unreliable_next_packet_bytes( &sender.unreliable );
            if ( plan == 0 )
                break;
            alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES * 2];
            const int length = rn_unreliable_fill( &sender.unreliable, frame, plan );
            if ( length == 0 )
                break;
            sent_messages += channels_unreliable_count_code0( frame, length );
            packets_sent++;
            rocketnet_network_simulator_send( wire.wire, &wire.address, &wire.address, frame, length, wire.time );
        }

        // drain what is due on the receiver

        for ( int got = rocketnet_network_simulator_receive( wire.wire, &wire.address, wire.time, &wire.address, rx_frame );
              got > 0;
              got = rocketnet_network_simulator_receive( wire.wire, &wire.address, wire.time, &wire.address, rx_frame ) )
        {
            packets_received++;
            received_messages += channels_unreliable_count_code0( rx_frame, got );
            int out_count = 0;
            const int violation = rn_unreliable_receive( &receiver.unreliable, rx_frame, got, &limits,
                                                         receiver.deliveries, 64, &out_count );
            check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
            for ( int d = 0; d < out_count; d++ )
            {
                check( channels_unreliable_delivered( receiver.deliveries[d].data, receiver.deliveries[d].bytes ) == 1 );
                delivered_count++;
            }
        }
    }

    // drain: write what is left queued and keep the wire moving until every
    // packet is delivered or lost

    for ( int pass = 0; pass < 600; pass++ )
    {
        wire.time += 1.0 / 60.0;
        for ( int p = 0; p < 4; p++ )
        {
            const int plan = rn_unreliable_next_packet_bytes( &sender.unreliable );
            if ( plan == 0 )
                break;
            alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PAYLOAD_BYTES * 2];
            const int length = rn_unreliable_fill( &sender.unreliable, frame, plan );
            if ( length == 0 )
                break;
            sent_messages += channels_unreliable_count_code0( frame, length );
            packets_sent++;
            rocketnet_network_simulator_send( wire.wire, &wire.address, &wire.address, frame, length, wire.time );
        }
        int got = rocketnet_network_simulator_receive( wire.wire, &wire.address, wire.time, &wire.address, rx_frame );
        while ( got > 0 )
        {
            packets_received++;
            received_messages += channels_unreliable_count_code0( rx_frame, got );
            int out_count = 0;
            const int violation = rn_unreliable_receive( &receiver.unreliable, rx_frame, got, &limits,
                                                         receiver.deliveries, 64, &out_count );
            check( violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
            for ( int d = 0; d < out_count; d++ )
            {
                check( channels_unreliable_delivered( receiver.deliveries[d].data, receiver.deliveries[d].bytes ) == 1 );
                delivered_count++;
            }
            got = rocketnet_network_simulator_receive( wire.wire, &wire.address, wire.time, &wire.address, rx_frame );
        }
    }

    // R8: every send attempt is refused, still in the FIFO, delivered, dropped
    // for a full delivery budget, or lost on the wire. Each term is measured
    // on its own side and none is the complement of another: refused is
    // cross-checked against the module's store-full counter; delivered is
    // counted on the receive side and each delivery byte-verified against the
    // sent registry; the FIFO remainder is the module's own count; wire loss
    // is the gap between the code-0 records parsed into the frames this test
    // sent and parsed out of the frames it received (never
    // accepted - delivered); and the budget drops are the module's counter.

    const uint64_t store_full = rn_counter( &sender.counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL );
    check( store_full == (uint64_t) refused );

    const uint64_t delivery_full = rn_counter( &receiver.counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );

    const int fifo_remaining = (int) sender.unreliable.count;
    check( fifo_remaining == 0 );

    const int wire_lost = sent_messages - received_messages;
    check( wire_lost >= 0 );

    // every code-0 record the receiver parsed was delivered or dropped for a
    // full budget (the module counts exactly those two outcomes)
    check( (uint64_t) received_messages == (uint64_t) delivered_count + delivery_full );

    check( (uint64_t) attempted == (uint64_t) refused + (uint64_t) fifo_remaining
                                    + (uint64_t) delivered_count + delivery_full + (uint64_t) wire_lost );

    // the wire's own packet accounting, independent of the message registry
    check( packets_received <= packets_sent );

    if ( loss_percent == 0.0f )
    {
        check( refused == 0 );
        check( delivered_count == attempted );
        check( delivery_full == 0 );
        check( wire_lost == 0 );
        check( packets_received == packets_sent );
        check( sender.unreliable.count == 0 );
    }
    else
    {
        check( packets_received < packets_sent );
        check( wire_lost > 0 );
    }

    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    rocketnet_network_simulator_destroy( wire.wire );
    channels_unreliable_teardown( &sender );
    channels_unreliable_teardown( &receiver );
}

static void test_channels_unreliable_over_the_lossy_simulator_entry()
{
    // 0, 10 and 50 percent loss with reordering; allocation is trapped inside
    // each leg (R1, docs/CHANNELS.md 1)

    test_channels_unreliable_over_the_lossy_simulator( 0.0f );
    test_channels_unreliable_over_the_lossy_simulator( 10.0f );
    test_channels_unreliable_over_the_lossy_simulator( 50.0f );
}

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
    { "test_channels_allocator_layout", "ChannelsAllocatorLayout", test_channels_allocator_layout },
    { "test_channels_allocator_validation", "ChannelsAllocatorValidation", test_channels_allocator_validation },
    { "test_channels_allocator_send_store", "ChannelsAllocatorSendStore", test_channels_allocator_send_store },
    { "test_channels_send_store_refuses_bad_handle", "ChannelsSendStoreRefusesBadHandle", test_channels_send_store_refuses_bad_handle },
    { "test_channels_allocator_descriptor_pool", "ChannelsAllocatorDescriptorPool", test_channels_allocator_descriptor_pool },
    { "test_channels_allocator_arena", "ChannelsAllocatorArena", test_channels_allocator_arena },
    { "test_channels_allocator_sizing", "ChannelsAllocatorSizing", test_channels_allocator_sizing },
    { "test_channels_allocator_trap_witness", "ChannelsAllocatorTrapWitness", test_channels_allocator_trap_witness },
    { "test_channels_allocator_no_live_allocation", "ChannelsAllocatorNoLiveAllocation", test_channels_allocator_no_live_allocation },
    { "test_channels_unreliable_small_messages_share_one_frame", "ChannelsUnreliableSmallMessagesShareOneFrame", test_channels_unreliable_small_messages_share_one_frame },
    { "test_channels_unreliable_record_never_split_across_packets", "ChannelsUnreliableRecordNeverSplitAcrossPackets", test_channels_unreliable_record_never_split_across_packets },
    { "test_channels_unreliable_large_message_travels_alone", "ChannelsUnreliableLargeMessageTravelsAlone", test_channels_unreliable_large_message_travels_alone },
    { "test_channels_unreliable_single_zero_copy_at_payload_root", "ChannelsUnreliableSingleZeroCopyAtPayloadRoot", test_channels_unreliable_single_zero_copy_at_payload_root },
    { "test_channels_unreliable_store_full_drops_and_counts", "ChannelsUnreliableStoreFullDropsAndCounts", test_channels_unreliable_store_full_drops_and_counts },
    { "test_channels_unreliable_delivery_budget_drops_and_counts", "ChannelsUnreliableDeliveryBudgetDropsAndCounts", test_channels_unreliable_delivery_budget_drops_and_counts },
    { "test_channels_unreliable_failed_send_drops_and_counts", "ChannelsUnreliableFailedSendDropsAndCounts", test_channels_unreliable_failed_send_drops_and_counts },
    { "test_channels_unreliable_chunk_freed_when_written", "ChannelsUnreliableChunkFreedWhenWritten", test_channels_unreliable_chunk_freed_when_written },
    { "test_channels_unreliable_block_not_corrupted_across_fragmented_store", "ChannelsUnreliableBlockNotCorruptedAcrossFragmentedStore", test_channels_unreliable_block_not_corrupted_across_fragmented_store },
    { "test_channels_unreliable_message_past_u16_length_refused", "ChannelsUnreliableMessagePastU16LengthRefused", test_channels_unreliable_message_past_u16_length_refused },
    { "test_channels_unreliable_over_the_lossy_simulator", "ChannelsUnreliableOverTheLossySimulator", test_channels_unreliable_over_the_lossy_simulator_entry },
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

// Allocator cases replace this process's allocation functions. Replacement is
// refused once the library is initialized (rocketnet.cpp), so the hooks go in
// before rocketnet_init. rocketnet_unit_prepare then installs the suite's log
// capture; the allocator cases want their own, which counts refusals, so that
// one is installed again after prepare. Other cases are left alone.

static int rocketnet_channels_is_allocator_test( const char * name )
{
    if ( !name )
        return 0;
    for ( int i = 0; i < rocketnet_channels_test_count(); i++ )
    {
        if ( strcmp( channels_tests[i].name, name ) != 0 && strcmp( channels_tests[i].alias, name ) != 0 )
            continue;
        return strncmp( channels_tests[i].name, "test_channels_allocator_", 24 ) == 0;
    }
    return 0;
}

void rocketnet_channels_prepare( const char * name )
{
    if ( !rocketnet_channels_is_allocator_test( name ) )
        return;
    rocketnet_allocator( rn_test_malloc_hook, rn_test_free_hook );
}

void rocketnet_channels_prepare_log( const char * name )
{
    if ( !rocketnet_channels_is_allocator_test( name ) )
        return;
    rocketnet_log_function( rn_test_log_function );
}
