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

#include "rocketnet_internal.h"
#include "rocketnet_message.h"
#include "rocketnet_read_write.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "rocketnet_counters.h"
#include "rocketnet_allocator.h"
#include "rocketnet_window_send.h"
#include "rocketnet_window_receive.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_channel.h"
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
    limits.reliable_messages_in_flight = 1024;
    limits.max_message_bytes = 32768;
    limits.receive_message_buffer_bytes = 65536;
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
    rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    for ( int i = 0; i < num_records; i++ )
        channels_check( rocketnet_message_writer_add( &writer, &records[i] ) == 1 );
    channels_check( rocketnet_message_writer_finish( &writer ) == expected_bytes );
    channels_check( memcmp( payload, expected, (size_t) expected_bytes ) == 0 );

    std::vector<uint8_t> exact( expected, expected + expected_bytes );
    std::vector<rocketnet_message_record_t> parsed;
    channels_check( channels_parse_all( exact.data(), expected_bytes, limits, parsed ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    channels_check( (int) parsed.size() == num_records );
    for ( int i = 0; i < num_records; i++ )
        channels_check( channels_record_equal( parsed[(size_t) i], records[i] ) );
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
        static const uint8_t expected[] = { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
                                            0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
        struct rocketnet_message_config_body_t body;
        body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
        body.enabled = 0xff;
        body.num_channels = 32;
        body.reliable_messages_in_flight = 1024;
        body.receive_message_buffer_bytes = 1048576;
        body.max_message_bytes = 1048576;
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
        narrow.reliable_messages_in_flight = 2;
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

    // 9. bad size: a reliable record in a payload over ROCKETNET_MAX_PACKET_PAYLOAD_BYTES; an unreliable one is fine there
    {
        std::vector<uint8_t> big = channels_bytes( { 0x03, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 } );
        const int length = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 12;
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

    alignas( 16 ) static uint8_t frame[64 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    const int payload_index = 32;
    uint8_t * payload = frame + payload_index;
    channels_check( ( (uintptr_t) payload & 15 ) == 0 );

    channels_check( rocketnet_message_single_capacity( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == ROCKETNET_MESSAGE_SINGLE_BYTES );
    channels_check( rocketnet_message_single_capacity( 1 ) == 0 );
    channels_check( rocketnet_message_single_capacity( 0 ) == 0 );

    // the application writes a packet-sized message in place at the root
    for ( int i = 0; i < ROCKETNET_MESSAGE_SINGLE_BYTES; i++ )
        payload[i] = (uint8_t) ( i ^ 0x5A );
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, ROCKETNET_MESSAGE_SINGLE_BYTES ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    channels_check( payload[ROCKETNET_MESSAGE_SINGLE_BYTES] == ROCKETNET_MESSAGE_FORM_SINGLE );

    // and the receiver reads it where it lies: the record's data is the payload root itself
    const struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 0x09 ) );
    struct rocketnet_message_reader_t reader;
    channels_check( rocketnet_message_reader_begin( &reader, payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    struct rocketnet_message_record_t record;
    channels_check( rocketnet_message_reader_next( &reader, &record ) == 1 );
    channels_check( record.data == payload );
    channels_check( record.bytes == ROCKETNET_MESSAGE_SINGLE_BYTES );
    channels_check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && !record.block && !record.control && record.channel == 0 );
    channels_check( rocketnet_message_reader_next( &reader, &record ) == 0 );

    // refusals: empty, and no room left for the form byte
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, 0 ) == 0 );
    channels_check( rocketnet_message_single_finish( payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == 0 );

    // a large SINGLE (the arena route) is legal: unreliable may ride fragments
    std::vector<uint8_t> large( 4097, 0x33 );
    channels_check( rocketnet_message_single_finish( large.data(), (int) large.size(), 4096 ) == 4097 );
    channels_check( rocketnet_message_check_payload( large.data(), (int) large.size(), &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );

    // SINGLE is UNRELIABLE: a receiver that does not admit code 0 refuses it
    const struct rocketnet_message_limits_t ordered_only = channels_limits( 32, 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    channels_check( rocketnet_message_check_payload( payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &ordered_only ) == ROCKETNET_MESSAGE_VIOLATION_TYPE_NOT_ENABLED );
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
        // build: either a packet of mixed records at most ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, or one
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
            // at most the receiver's max_message_bytes (check 9)
            std::vector<uint8_t> big( (size_t) channels_random_range( &random, 1200, limits.max_message_bytes ) );
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
            rocketnet_message_writer_begin( &writer, payload, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
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
            channels_check( length >= 4 && length <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
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
            const uint8_t original = mutated[(size_t) position];
            mutated[(size_t) position] = (uint8_t) ( original ^ (uint8_t) channels_random_range( &random, 1, 255 ) );
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
                channels_check_repack( mutated.data(), length, result, mutated[(size_t) ( length - 1 )] );
                mutations_parsed++;
            }
            mutated[(size_t) position] = original;
        }
    }

    channels_check( records_checked > iterations );
    channels_check( mutations_parsed > 0 );

    // the mutations reach every check a record alone can fail; BAD_SIZE needs
    // a reliable record past ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, which one byte cannot make, and
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

static void rn_test_log_function( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    (void) level;
    snprintf( rn_log_last, sizeof(rn_log_last), "%s", message );
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
    // section 14's formula, part by part, at the defaults: 6,574,080 bytes, no padding

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // test explicit headroom without a finite send budget
    check( config.num_channels == 32 );
    check( config.reliable_messages_in_flight == 1024 );
    check( config.receive_message_buffer_bytes == 1048576 );
    check( config.max_message_bytes == 1048576 );

    rn_region_layout_t layout;
    check( rn_region_layout( &config, &layout ) );
    check( layout.formula_bytes == 6680832 );
    check( layout.region_bytes == 6680832 );
    check( rocketnet_message_connection_bytes( &config ) == 6680832 );
    check( rocketnet_message_connection_bytes( NULL ) > 6680832 );

    const uint64_t C = 32, W = 1024, B = 1048576, Q = 1024, S = 4194304, P = 1024, Du = 1024, Ru = 16, Rq = 256;
    check( layout.bytes[RN_PART_CHANNEL_SEND] + layout.bytes[RN_PART_CHANNEL_RECEIVE] == 56 * C );
    check( layout.bytes[RN_PART_SEND_NUMBERS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVE_DESCRIPTORS] == 32 * W );
    check( layout.bytes[RN_PART_RECEIVED_BITS] == W / 8 );
    check( layout.bytes[RN_PART_HELD_INDEX] == 16 * 2 * W );
    check( layout.bytes[RN_PART_RECEIVE_RING] == B );
    check( layout.bytes[RN_PART_SEND_DESCRIPTORS] == 64 * Q );
    check( layout.bytes[RN_PART_SEND_STORE] == S );
    check( layout.bytes[RN_PART_SENT_PACKETS] + layout.bytes[RN_PART_SENT_INDEX] + layout.bytes[RN_PART_SENT_REFS] == 128 * P );
    check( layout.bytes[RN_PART_DELIVERIES] == 24 * ( W + Du ) );
    check( layout.bytes[RN_PART_SEQUENCED_HOLDS] == ( 16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) * Ru );
    check( layout.bytes[RN_PART_RECEIPTS] == 16 * Rq );
    check( layout.bytes[RN_PART_RESEND_FIFO] == 8 * W );
    check( layout.bytes[RN_PART_SEND_SPANS] == B );

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

    check( layout.store_chunks[RN_STORE_SMALL] == 10591 );
    check( layout.store_chunks[RN_STORE_MEDIUM] == 2709 );
    check( layout.store_chunks[RN_STORE_LARGE] == 1209 );
    check( layout.store_chunks[RN_STORE_LARGE] >= ( 1048576 + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES );

    // odd shapes pad, and the padding is all the region adds to the formula

    config.num_channels = 3;
    config.unreliable_messages_per_update = 1;
    config.sequenced_messages_held = 1;
    config.max_message_receipts = 17;
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

    config.sequenced_messages_held = 0;
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
        { "window not a power of two",              (int) offsetof( rocketnet_message_config_t, reliable_messages_in_flight ), 1000 },
        { "window below its minimum",               (int) offsetof( rocketnet_message_config_t, reliable_messages_in_flight ), 32 },
        { "ring not a multiple of 16",              (int) offsetof( rocketnet_message_config_t, receive_message_buffer_bytes ), 262144 + 8 },
        { "max message above the ring",             (int) offsetof( rocketnet_message_config_t, max_message_bytes ), ROCKETNET_MAX_MESSAGE_BYTES + 16 },
        { "store too small for a maximal block",    (int) offsetof( rocketnet_message_config_t, send_message_buffer_bytes ), 12288 },
        { "queue below its minimum",                (int) offsetof( rocketnet_message_config_t, send_queue_messages ), 15 },
        { "resend_delay_initial above resend_delay_max",              (int) offsetof( rocketnet_message_config_t, message_resend_delay_initial_ms ), 3000 },
        { "resend_delay_min above resend_delay_max",                  (int) offsetof( rocketnet_message_config_t, message_resend_delay_min_ms ), 3000 },
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

    // resend_delay_initial below resend_delay_min needs two fields: the default initial (2000 ms)
    // is above every single-field floor the range allows. Re-pinned for the
    // default initial value.
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 100;
    config.message_resend_delay_min_ms = 150;
    check( rocketnet_message_validate_config( &config ) == 0 );
    check( rocketnet_message_connection_bytes( &config ) == 0 );

    // ceil( max_message_bytes / F ) above W: a 64-message window holds at most 64 fragments

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 1;
    config.receive_message_buffer_bytes = 1 << 20;
    config.send_message_buffer_bytes = 1 << 20;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // the edge of the store rule: the large class holds exactly one maximal block

    rocketnet_message_default_config( &config );
    config.max_message_bytes = 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config.send_message_buffer_bytes = 12288;               // a third of 12288 holds three 1152 chunks with their links
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) == 1 );

    // section 14's lean profile is refused at the default max_message_bytes: its
    // large class holds 18 chunks, a 32768-byte block needs 29 (section 4)

    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 256;
    config.receive_message_buffer_bytes = 32768;
    config.send_message_buffer_bytes = 65536;
    config.send_queue_messages = 256;
    config.packets_in_flight = 256;                 // at least W (the sent-map rule of 14)
    config.unreliable_messages_per_update = 128;
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
    config.reliable_messages_in_flight = 64;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    config.send_message_buffer_bytes = 12288;
    config.send_queue_messages = 16;
    config.packets_in_flight = 64;                  // the window's 64: the sent map holds no fewer than W entries
    config.packets_per_update = 1;
    config.unreliable_messages_per_update = 1;
    config.sequenced_messages_held = 0;
    config.max_message_receipts = 16;
    config.message_resend_delay_min_ms = 1;
    config.message_resend_delay_initial_ms = 10;
    config.message_resend_delay_max_ms = 10;
    config.reliable_message_timeout_ms = 1000;
    check( rocketnet_message_validate_config( &config ) == 1 );
    check( rocketnet_message_connection_bytes( &config ) > 0 );

    // the sent map holds at least the window (14): a map one entry short of W
    // is refused, a map of W entries is taken

    rocketnet_message_default_config( &config );
    config.packets_in_flight = config.reliable_messages_in_flight - 1;
    check( rocketnet_message_validate_config( &config ) == 0 );
    config.packets_in_flight = config.reliable_messages_in_flight;
    check( rocketnet_message_validate_config( &config ) == 1 );

    check( rocketnet_message_validate_config( NULL ) == 1 );       // NULL is the defaults
}

static void test_channels_allocator_send_store()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.client_send_kbps = config.server_send_kbps = 0; // isolate headroom capacity
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

    std::vector<uint32_t> handles[RN_STORE_NUM_CLASSES];
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        const uint32_t total = rn_send_store_total_chunks( &store, c );
        handles[c].resize( total );
        for ( uint32_t i = 0; i < total; i++ )
        {
            handles[c][i] = rn_send_store_alloc( &store, class_bytes[c] );
            check( handles[c][i] != RN_CHUNK_NONE );
            uint8_t * data = rn_send_store_chunk_data( &store, handles[c][i] );
            check( data >= memory && data + class_bytes[c] <= memory + layout.bytes[RN_PART_SEND_STORE] );
            check( ( (uintptr_t) data & 15 ) == 0 );
            memset( data, (int) ( (uint32_t) c * 64 + ( i & 63 ) ), (size_t) class_bytes[c] );
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
                check( data[b] == (uint8_t) ( (uint32_t) c * 64 + ( i & 63 ) ) );
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
// not issue -- a class at or past RN_STORE_NUM_CLASSES, or an index the store
// has not issued -- reads nothing. A handle the store did issue still
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

    // an in-range chunk the lazy store has not issued reads nothing

    const uint32_t never_issued = ( (uint32_t) RN_STORE_SMALL << 30 ) | ( rn_send_store_total_chunks( &store, RN_STORE_SMALL ) - 1 );
    check( rn_send_store_chunk_data( &store, never_issued ) == NULL );
    check( rn_send_store_chunk_next( &store, never_issued ) == RN_CHUNK_NONE );

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
    config.client_send_kbps = config.server_send_kbps = 0; // test explicit headroom without a finite send budget
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
    check( rn_region_exhausted( region ) );

    // a free returns the descriptor and its chunk to the pools,
    // but the region is exhausted (sticky) so alloc is still refused
    rn_region_send_free( region, descriptors[5] );
    check( region->descriptors_free == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_free + 1 );
    check( rn_region_send_alloc( region, 10 ) == RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 1 );  // nothing taken

    // carve (reset) clears exhaustion, then alloc succeeds
    rn_region_carve( region );
    check( !rn_region_exhausted( region ) );
    const uint32_t d = rn_region_send_alloc( region, 10 );
    check( d != RN_DESCRIPTOR_NONE );
    check( region->descriptors_free == 15 );

    // a store refusal takes no descriptor, and is counted as itself

    // Carve reset the original 16 descriptors; only d is live now.
    rn_region_send_free( region, d );
    check( region->descriptors_free == 16 );
    check( rn_region_send_alloc( region, config.max_message_bytes + 1 ) == RN_DESCRIPTOR_NONE );   // too large is a refusal, never a store attempt
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == 0 );
    check( rn_region_send_alloc( region, 0 ) == RN_DESCRIPTOR_NONE );
    const uint32_t large = rn_send_store_free_chunks( &region->store, RN_STORE_LARGE );
    const int block_bytes = 131072; // exhaust the store before the 16 descriptors
    const uint32_t chunks = ( (uint32_t) block_bytes + ROCKETNET_MESSAGE_FRAGMENT_BYTES - 1 ) / ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t blocks = large / chunks;
    check( blocks < 16 );
    for ( uint32_t i = 0; i < blocks; i++ )
        check( rn_region_send_alloc( region, block_bytes ) != RN_DESCRIPTOR_NONE );
    check( rn_region_send_alloc( region, block_bytes ) == RN_DESCRIPTOR_NONE );
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
        check( rn_region_contains( region, region->sequenced_messages_held, sizeof(rn_sequenced_hold_t) * (uint64_t) region->holds ) );
        check( rn_region_contains( region, region->receipts, sizeof(rn_receipt_t) * (uint64_t) region->receipt_entries ) );
        check( rn_region_contains( region, region->resend_fifo, sizeof(rn_resend_link_t) * (uint64_t) region->window ) );
        check( !rn_region_contains( region, region->base + region->bytes, 1 ) );
        check( !rn_region_contains( region, region->base - 1, 1 ) );
        check( region->held_entries == 2 * region->window );
        check( region->sent_index_entries == 2 * region->packets );
        check( region->sent_ref_entries == 16 * region->packets );
        check( region->delivery_entries == region->window + (uint32_t) config.unreliable_messages_per_update );
    }

    // the receiver's ring bounds check

    rn_region_t * region = rn_arena_region( arena, 0 );
    check( rn_region_ring_span_valid( region, 0, 16 ) );
    check( rn_region_ring_span_valid( region, region->ring_bytes - 1152, 1152 ) );
    check( !rn_region_ring_span_valid( region, region->ring_bytes - 1152, 1153 ) );
    check( !rn_region_ring_span_valid( region, region->ring_bytes, 1 ) );
    check( !rn_region_ring_span_valid( region, 8, 16 ) );                  // placements are multiples of 16
    check( !rn_region_ring_span_valid( region, 0xFFFFFFFFFFFFFFF0ULL, 32 ) ); // no wrap past 2^64

    rn_arena_destroy( arena );
    check( rn_hook_mallocs == mallocs + 1 );
    check( rn_hook_frees == frees + 1 );

    // a refused config allocates nothing

    config.reliable_messages_in_flight = 1000;
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
    config.client_send_kbps = config.server_send_kbps = 0; // isolate headroom capacity

    static char text[4096];
    rn_trap_phase = "sizing";
    rn_trap_armed = 1;
    const int length = rocketnet_message_connection_sizing( &config, 1000, text, (int) sizeof(text) );
    rn_trap_armed = 0;
    check( length > 0 && length < (int) sizeof(text) );
    check( (int) strlen( text ) == length );
    check( strstr( text, "56C + 112.125W + B + 64Q + S + 128P + 24(W + Du) + 1432Ru + 16Rq" ) != NULL );
    check( strstr( text, "region bytes 6680832" ) != NULL );
    check( strstr( text, "formula bytes 6680832" ) != NULL );
    check( strstr( text, "1000 connections 6680832000" ) != NULL );

    // too small a buffer truncates and still reports the full length, as snprintf does

    char small[32];
    check( rocketnet_message_connection_sizing( &config, 1, small, (int) sizeof(small) ) == rocketnet_message_connection_sizing( &config, 1, NULL, 0 ) );
    check( strlen( small ) == sizeof(small) - 1 );

    // a refused config prints nothing and returns 0

    config.receive_message_buffer_bytes = 100;
    check( rocketnet_message_connection_sizing( &config, 1, text, (int) sizeof(text) ) == 0 );
    check( text[0] == '\0' );

    // the print form logs the same lines at info level and returns the region bytes

    rocketnet_message_default_config( &config );
    const enum rocketnet_log_level_t old_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    const int lines_before = rn_log_lines;
    check( rocketnet_message_print_connection_sizing( &config, 16 ) == rocketnet_message_connection_bytes( &config ) );
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
    // a descriptor at or past the lazy pool's watermark was never taken
    // (tla/RNLazyPool.tla): not in use, and its bytes are not state
    for ( uint32_t d = 0; d < region->descriptors_fresh; d++ )
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
            const uint64_t ds = store_now - store_full;
            const uint64_t dq = queue_now - queue_full;
            // A region already exhausted (sticky) refuses every send without
            // counting a pool counter; that is terminal for this send loop, so
            // stop here. Do not carve mid-send: carve clears the sticky flag and
            // frees every descriptor, so the next alloc would succeed, re-exhaust,
            // and carve again forever. The harness resets the region at the next
            // reconnect. A refusal that *becomes* exhausted is counted above and
            // is the normal one-pool refusal guarded below.
            if ( rn_region_exhausted( region ) && ( ds + dq ) == 0 )
                break;
            check( ds + dq == 1 );       // exactly one reason, counted once
            side->refused_store += ds;
            side->refused_queue += dq;
            break;
        }

        rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
        check( descriptor->bytes == (uint32_t) bytes );
        descriptor->serial = ++side->serial;
        descriptor->reliability = ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
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
    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

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
                rd->code = ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
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
            delivery->code = ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
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
                const uint32_t hold_slot = descriptor->serial % receiver->holds;
                rn_sequenced_hold_t * hold = &receiver->sequenced_messages_held[hold_slot];
                uint8_t * hold_data = receiver->sequenced_message_payloads[hold_slot].data;
                const uint32_t bytes = descriptor->bytes < (uint32_t) ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? descriptor->bytes : (uint32_t) ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
                memcpy( hold_data, delivery->data, bytes );
                hold->bytes = bytes;
                hold->channel = (uint16_t) c;
                hold->ordering = descriptor->ordering;
                hold->sequencing = 0;
                hold->used = 1;
                check( hold_data[0] == rn_pattern( descriptor->serial, 0 ) );
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
                receipt->status = ROCKETNET_RECEIPT_STATUS_ACKED;
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
    server_config.client_send_kbps = server_config.server_send_kbps = 0; // deliberate fixed-pool exhaustion
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
                rn_region_t * ends[2] = { rn_arena_region( server, s ), rn_arena_region( clients[s], 0 ) };
                // A region left exhausted from the previous epoch refuses every
                // send until reset (sticky). Reset it now -- the disconnect the
                // model's ExhaustionOnlyLeadsToDisconnect requires -- before the
                // reconnect's sends are queued.
                for ( int e = 0; e < 2; e++ )
                {
                    if ( rn_region_exhausted( ends[e] ) )
                    {
                        rn_region_carve( ends[e] );
                        rn_soak_reset( &sides[2 * s + e] );
                    }
                }
                rn_soak_send( &sides[2 * s] );
                rn_soak_send( &sides[2 * s + 1] );
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
        (void) client_store;
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
//     the SINGLE form (docs/CHANNELS.md 11.2): the codec reads a payload
//     that is one message with the form byte last as one code-0 record.
//     The send, receive-budget and drop behaviour of the unreliable path
//     is tested on the shipped path (test_unit_message_objects.cpp,
//     test_unit_channel_dispatch.cpp, functional/channel_unreliable_*.cpp).
// ----------------------------------------------------------------

static void channels_single_fill_pattern( uint8_t * data, int bytes, int salt )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = (uint8_t) ( i * 31 + salt );
}

static void channels_single_expect_one_record( const uint8_t * payload, int bytes, const uint8_t * message, int message_bytes )
{
    struct rocketnet_message_limits_t limits = channels_limits( 32, rocketnet_message_enabled_codes( 1 ) );
    struct rocketnet_message_reader_t reader;
    check( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    struct rocketnet_message_record_t record;
    check( rocketnet_message_reader_next( &reader, &record ) );
    check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE );
    check( record.bytes == message_bytes && memcmp( record.data, message, (size_t) message_bytes ) == 0 );
    check( !rocketnet_message_reader_next( &reader, &record ) );
}

static void test_channels_single_form_parses_as_one_code0_record()
{
    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    static uint8_t message[ROCKETNET_MESSAGE_SINGLE_BYTES];

    // a message of 300 bytes and the largest the form carries: the message at
    // the 16-aligned payload root, the form byte last
    const int sizes[2] = { 300, ROCKETNET_MESSAGE_SINGLE_BYTES };
    for ( int i = 0; i < 2; i++ )
    {
        memset( frame, 0, sizeof(frame) );
        channels_single_fill_pattern( message, sizes[i], 8 + i );
        memcpy( frame, message, (size_t) sizes[i] );
        frame[sizes[i]] = ROCKETNET_MESSAGE_FORM_SINGLE;
        channels_single_expect_one_record( frame, sizes[i] + 1, message, sizes[i] );
    }
}

// -----------------------------------------

// Layer 3 sender: small region fixtures exercise the real per-number table
// without an arena allocation. The red-first assertions stay as the rules'
// regression witnesses (CHANNELS 6.2, RNRing PlaceSingle/PlaceBlock/Ack,
// RNWindow NumberRecord/ReceiveAck).

struct channels_window_send_rig_t
{
    rn_region_t region;
    rn_send_number_t numbers[8];
    rocketnet_counters_t counters;
    rn_window_send_t send;
};

static void channels_window_send_setup( channels_window_send_rig_t * rig, uint32_t W, uint32_t B )
{
    memset( rig, 0, sizeof( *rig ) );
    rig->region.window = W;
    rig->region.ring_bytes = B;
    rig->region.max_message_bytes = B;
    rig->region.send_numbers = rig->numbers;
    rig->region.counters = &rig->counters;
    rn_window_send_init( &rig->send, &rig->region, 0 );
}

static void test_channels_window_send_numbers_late_never_at_send()
{
    channels_window_send_rig_t rig;
    channels_window_send_setup( &rig, 4, 64 );
    uint64_t n = UINT64_MAX, p = UINT64_MAX;
    check( rig.send.next == 0 );                    // send_message only queues
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 8, 0, &n, &p ) == RN_WINDOW_SEND_UNRELIABLE );
    check( rig.send.next == 0 );                    // no unreliable number
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 8, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( n == 0 && rig.send.next == 1 );          // first packet write takes it
    check( rig.numbers[0].number == 0 && rig.numbers[0].acked == 0 );
}

static void test_channels_window_send_count_bound_stalls_at_u_prime_plus_w()
{
    channels_window_send_rig_t rig;
    channels_window_send_setup( &rig, 4, 64 );
    uint64_t n, p;
    for ( uint64_t i = 0; i < 4; i++ )
    {
        check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
        check( n == i );
    }
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, &n, &p ) == RN_WINDOW_SEND_STALL );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, &n, &p ) == RN_WINDOW_SEND_STALL );
    check( rig.counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 1 );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 1, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( n == 4 );
}

static void test_channels_window_send_ring_pads_at_the_boundary()
{
    channels_window_send_rig_t rig;
    channels_window_send_setup( &rig, 8, 64 );
    uint64_t n, p;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( p == 0 );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 48, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( p == 64 && rig.send.ring_next == 112 ); // [32,80) would cross B
    check( rn_region_ring_span_valid( &rig.region, p % 64, 48 ) );
}

static void test_channels_window_send_floor_is_u_prime_not_u()
{
    channels_window_send_rig_t rig;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    channels_window_send_setup( &rig, 8, 2 * F );
    uint64_t n, p;
    check( rn_window_send_first_write_block( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F + 1, 0, &n, &p ) == 2 );
    check( n == 0 && p == 0 );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F - 16, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( rig.send.ring_next == 2 * F );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 0 );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 0, &n, &p ) == RN_WINDOW_SEND_STALL );
    check( rig.send.next == 3 );                    // floor 0 forbids [B,B+16)
}

static void test_channels_window_send_block_takes_consecutive_numbers_and_one_span()
{
    channels_window_send_rig_t rig;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    channels_window_send_setup( &rig, 8, 4096 );
    uint64_t n, p;
    check( rn_window_send_first_write_block( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F + 17, 5, &n, &p ) == 2 );
    check( n == 0 && p == 0 && rig.send.next == 2 );
    check( rig.numbers[0].fragment == 0 && rig.numbers[1].fragment == 1 );
    check( rig.numbers[0].ring_start == 0 && rig.numbers[1].ring_start == F );
    check( rig.numbers[0].ring_units == F / 16 && rig.numbers[1].ring_units == 2 );
    check( rig.send.ring_next == F + 32 );
    check( rn_window_send_mark_acked( &rig.send, 1 ) == 1 );
    check( rig.send.U == 0 );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U == 2 && rig.send.U_prime == 2 );
}

static void test_channels_window_send_stall_lets_unreliable_through()
{
    channels_window_send_rig_t rig;
    channels_window_send_setup( &rig, 1, 64 );
    uint64_t n, p;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, 0, &n, &p ) == RN_WINDOW_SEND_STALL );
    check( rig.send.stalled && rn_window_send_can_write_unreliable( &rig.send ) );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 8, 0, &n, &p ) == RN_WINDOW_SEND_UNRELIABLE );
    check( rig.send.next == 1 && rig.counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );
}

static void test_channels_window_send_any_message_fits_when_nothing_is_outstanding()
{
    channels_window_send_rig_t rig;
    channels_window_send_setup( &rig, 4, 64 );
    uint64_t n, p;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U_prime == rig.send.next );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, 0, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
    check( p == 64 && rig.send.ring_next == 128 );
}

// -----------------------------------------
// CHANNELS 6.5 and RNWindow Accept: each case uses the region's W=64 bitset
// and descriptor table. These checks were added before the receive module.
struct channels_window_receive_rig_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_window_receive_t receive;
};

static void channels_window_receive_setup( channels_window_receive_rig_t * rig, uint64_t first )
{
    memset( rig, 0, sizeof(*rig) );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.max_message_bytes = 32768;       // a block is at most W = 64 fragments (the cross rule of 14)
    rig->arena = rn_arena_create( NULL, &config, 1, &rig->counters );
    check( rig->arena );
    rig->region = rn_arena_region( rig->arena, 0 );
    rn_region_carve( rig->region );
    rn_window_receive_init( &rig->receive, rig->region, first );
}

static void channels_window_receive_teardown( channels_window_receive_rig_t * rig )
{
    rn_arena_destroy( rig->arena );
}

static rocketnet_message_record_t channels_window_record( uint32_t number )
{
    static const uint8_t data = 0x31;
    rocketnet_message_record_t record = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE, &data, 1 );
    record.number = number & ROCKETNET_MESSAGE_U24_MASK;
    return record;
}

static void test_channels_window_receive_accepts_in_order()
{
    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    for ( uint32_t n = 0; n < 64; n++ )
    {
        rocketnet_message_record_t record = channels_window_record( n );
        uint32_t slot = RN_DESCRIPTOR_NONE;
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( slot == n );
        check( rig.receive.R == n + 1 );
        check( rig.region->receive_descriptors[slot].live );
        check( rig.region->receive_descriptors[slot].number == n );
        rn_window_receive_release( &rig.receive, n );
        check( !rig.region->receive_descriptors[slot].live );
    }
    channels_window_receive_teardown( &rig );
}

static void test_channels_window_receive_duplicate_is_counted_not_violation()
{
    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    rocketnet_message_record_t record = channels_window_record( 1 );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );
    record.number = 0;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rig.receive.R == 2 );
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );
    check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) == 2 );
    channels_window_receive_teardown( &rig );
}

static void test_channels_window_receive_outside_window_is_violation()
{
    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    rocketnet_message_record_t record = channels_window_record( 64 );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_VIOLATION );
    check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW );
    check( rig.receive.R == 0 );
    record.number = 0;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_VIOLATION );
    check( !rig.region->receive_descriptors[0].live );
    channels_window_receive_teardown( &rig );
}

static void test_channels_window_receive_slot_collision_is_violation()
{
    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    rocketnet_message_record_t record = channels_window_record( 0 );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    record.number = 64;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_VIOLATION );
    check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION );
    check( rig.region->receive_descriptors[0].number == 0 );
    check( rig.region->receive_descriptors[0].live );
    channels_window_receive_teardown( &rig );
}

static void test_channels_window_receive_reconstructs_across_the_24_bit_wrap()
{
    const uint64_t modulus = 1ULL << 24;
    for ( int lap = 1; lap <= 3; lap += 2 )
    {
        channels_window_receive_rig_t rig;
        const uint64_t first = modulus * (uint64_t) lap - 2;
        channels_window_receive_setup( &rig, first );
        uint32_t slot = RN_DESCRIPTOR_NONE;
        rocketnet_message_record_t record = channels_window_record( (uint32_t) ( first + 1 ) );
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( rig.region->receive_descriptors[slot].number == first + 1 );
        record.number = (uint32_t) ( first & ROCKETNET_MESSAGE_U24_MASK );
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( rig.receive.R == first + 2 );
        record.number = 0;
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
        check( rig.region->receive_descriptors[slot].number == first + 2 );
        record.number = (uint32_t) ( ( first + 1 ) & ROCKETNET_MESSAGE_U24_MASK );
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );
        channels_window_receive_teardown( &rig );
    }
}

static void test_channels_window_receive_advance_clears_only_what_it_passes()
{
    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    const uint32_t order[] = { 2, 4, 0, 1 };
    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_message_record_t record = channels_window_record( order[i] );
        uint32_t slot = RN_DESCRIPTOR_NONE;
        check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    }
    check( rig.receive.R == 3 );
    check( ( rig.region->received_bits[0] & 0x7ULL ) == 0 );
    check( ( rig.region->received_bits[0] & ( 1ULL << 4 ) ) != 0 );
    rocketnet_message_record_t record = channels_window_record( 3 );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( rig.receive.R == 5 );
    check( ( rig.region->received_bits[0] & 0x1FULL ) == 0 );
    channels_window_receive_teardown( &rig );
}

// CHANNELS R1/R8, 6.5 and RNWindow OpenPacket/Accept: a scripted sender
// resends only unaccepted numbers inside R+W. The simulator duplicates and
// reorders RECORDS payloads; the layer-2 reader feeds each parsed record to
// the window. Accepted records are consumed and released immediately.
static void test_channels_window_receive_over_the_lossy_simulator_leg( float loss_percent )
{
    const int total = 128;
    int accepted[total] = { 0 };
    int arrivals[total] = { 0 };
    int accepted_total = 0;
    int received_records = 0;
    int duplicate_results = 0;
    int drained = 0;

    channels_window_receive_rig_t rig;
    channels_window_receive_setup( &rig, 0 );
    rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create(
        NULL, 8192, 0x8EED5EEDULL + (uint64_t) loss_percent );
    check( wire );
    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1:40001" ) );
    rocketnet_network_simulator_config( wire, 0.02, 0.04, loss_percent, 30.0f );
    rocketnet_message_limits_t limits = channels_limits( 1, 1u << ROCKETNET_MESSAGE_CODE_RELIABLE );
    limits.reliable_messages_in_flight = 64;
    double now = 0.0;
    alignas( 16 ) uint8_t packet[128];
    alignas( 16 ) uint8_t received[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES * 2];

    rn_trap_phase = "receive-window-wire";
    rn_trap_armed = 1;
    for ( int tick = 0; tick < 2000 && drained < 16; tick++ )
    {
        now += 0.01;
        int oldest = 0;
        while ( oldest < total && accepted[oldest] )
            oldest++;
        check( (uint64_t) oldest == rig.receive.R );

        // At most 16 live candidates, all within R+W. The sender learns
        // acceptance through the test script, as an ack would in layer 3.
        if ( oldest < total )
        {
            const int end = oldest + 16 < total ? oldest + 16 : total;
            for ( int n = oldest; n < end; n++ )
            {
                if ( accepted[n] )
                    continue;
                const uint8_t body[2] = { (uint8_t) n, (uint8_t) ( n >> 8 ) };
                rocketnet_message_record_t record = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE, body, sizeof(body) );
                record.number = (uint32_t) n;
                rocketnet_message_writer_t writer;
                rocketnet_message_writer_begin( &writer, packet, sizeof(packet) );
                check( rocketnet_message_writer_add( &writer, &record ) == 1 );
                const int bytes = rocketnet_message_writer_finish( &writer );
                check( bytes > 0 );
                rocketnet_network_simulator_send( wire, &address, &address, packet, bytes, now );
            }
        }
        else
            drained++;

        for ( int bytes = rocketnet_network_simulator_receive( wire, &address, now, &address, received );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &address, now, &address, received ) )
        {
            rocketnet_message_reader_t reader;
            check( rocketnet_message_reader_begin( &reader, received, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
            rocketnet_message_record_t record;
            while ( rocketnet_message_reader_next( &reader, &record ) )
            {
                check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE );
                check( record.bytes == 2 );
                const int n = (int) record.number;
                check( n >= 0 && n < total );
                check( record.data[0] == (uint8_t) n && record.data[1] == (uint8_t) ( n >> 8 ) );
                arrivals[n]++;
                received_records++;
                uint32_t slot = RN_DESCRIPTOR_NONE;
                const int answer = rn_window_receive_accept( &rig.receive, &record, &slot );
                check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
                if ( answer == RN_WINDOW_RECEIVE_ACCEPT )
                {
                    accepted[n]++;
                    accepted_total++;
                    check( accepted[n] == 1 );
                    check( slot == (uint32_t) ( n % 64 ) );
                    rn_window_receive_release( &rig.receive, (uint64_t) n );
                }
                else
                {
                    check( answer == RN_WINDOW_RECEIVE_DUPLICATE );
                    duplicate_results++;
                }
            }
        }
    }
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( accepted_total == total );
    check( rig.receive.R == (uint64_t) total );
    int duplicate_arrivals = 0;
    for ( int n = 0; n < total; n++ )
    {
        check( accepted[n] == 1 );
        check( arrivals[n] >= 1 );
        duplicate_arrivals += arrivals[n] - 1;
    }
    check( duplicate_results == duplicate_arrivals );
    check( rn_counter( &rig.counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE ) == (uint64_t) duplicate_arrivals );
    check( received_records == accepted_total + duplicate_results );
    check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_network_simulator_destroy( wire );
    channels_window_receive_teardown( &rig );
}

static void test_channels_window_receive_over_the_lossy_simulator()
{
    test_channels_window_receive_over_the_lossy_simulator_leg( 0.0f );
    test_channels_window_receive_over_the_lossy_simulator_leg( 10.0f );
    test_channels_window_receive_over_the_lossy_simulator_leg( 50.0f );
}

// CHANNELS 6.2/6.3, RNRing LiveSpansDisjoint and RNWindow NumberRecord/Accept:
// a small layer-6 stand-in first-writes records through the real send module,
// retransmits them over the simulator, and gives an ACK one update after the
// receiver accepted and released each number (P2/P6). A shadow byte map checks
// every live ring span. All arrays are fixed before the trap is armed (R1/R4).
struct channels_window_exchange_record_t
{
    uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    uint32_t bytes;
    uint32_t ring;
    uint32_t ordering;
    uint16_t fragment;
    uint8_t block;
    uint8_t accepted;
    uint8_t released;
    uint8_t acked;
    int ack_due;
};

static void channels_window_exchange_write( rocketnet_network_simulator_t * wire,
                                             const rocketnet_address_t * address,
                                             const channels_window_exchange_record_t * item,
                                             uint64_t n, uint8_t * packet, int capacity, double now )
{
    rocketnet_message_record_t record = channels_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                          item->data, (int) item->bytes );
    record.number = (uint32_t) n;
    record.ring = item->ring;
    record.ordering = item->ordering;
    record.channel = 0;
    record.block = item->block;
    record.fragment = item->fragment;
    record.fragments = item->block ? 1 : 0;
    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, packet, capacity );
    check( rocketnet_message_writer_add( &writer, &record ) == 1 );
    const int bytes = rocketnet_message_writer_finish( &writer );
    check( bytes > 0 );
    rocketnet_network_simulator_send( wire, address, address, packet, bytes, now );
}

static void test_channels_window_send_and_receive_over_the_lossy_simulator_leg( float loss_percent )
{
    const uint32_t W = 64, B = 4096, F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const int messages = 80, capacity = 128;
    rocketnet_counters_t counters = {};
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = W;
    config.receive_message_buffer_bytes = B;
    config.max_message_bytes = B;
    rn_arena_t * arena = rn_arena_create( NULL, &config, 2, &counters );
    check( arena );
    rn_region_t * source = rn_arena_region( arena, 0 );
    rn_region_t * target = rn_arena_region( arena, 1 );
    rn_region_carve( source );
    rn_region_carve( target );
    rn_window_send_t send;
    rn_window_receive_t receive;
    rn_window_send_init( &send, source, 0 );
    rn_window_receive_init( &receive, target, 0 );

    rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create(
        NULL, 8192, 0xE7C4A11ULL + (uint64_t) loss_percent );
    check( wire );
    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "127.0.0.1:40002" ) );
    rocketnet_network_simulator_config( wire, 0.02, 0.04, loss_percent, 30.0f );
    rocketnet_message_limits_t limits = channels_limits( 1, 1u << ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    limits.reliable_messages_in_flight = W;
    limits.max_message_bytes = B;
    limits.receive_message_buffer_bytes = B;

    channels_window_exchange_record_t records[capacity] = {};
    uint8_t live[B] = {};
    uint8_t reserved[B] = {};
    alignas( 16 ) uint8_t packet[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    alignas( 16 ) uint8_t received[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES * 2];
    uint64_t send_cursor = 0;
    int next_message = 0, accepted_total = 0, drained = 0;
    double now = 0.0;

    rn_trap_phase = "send-receive-window-wire";
    rn_trap_armed = 1;
    for ( int tick = 0; tick < 1500 && drained < 16; tick++ )
    {
        now += 0.01;
        uint64_t newly_numbered = UINT64_MAX;

        // Ack only numbers the receiver released in a prior update. This
        // oracle stands in for the next card's sent-packet map (P2/P6).
        for ( uint64_t n = 0; n < send.next; n++ )
        {
            if ( records[n].ack_due == tick && !records[n].acked )
            {
                check( records[n].released );
                check( rn_window_send_mark_acked( &send, n ) == 1 );
                records[n].acked = 1;
            }
        }

        // Number at most one new message per update, at its first write.
        // A block reserves both fragments and its whole span atomically.
        if ( next_message < messages )
        {
            const int block = next_message % 7 == 0;
            uint64_t first = 0, p = 0;
            uint32_t count = 0;
            if ( block )
                count = rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                          F + 17, RN_DESCRIPTOR_NONE, &first, &p );
            else if ( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                  16, RN_DESCRIPTOR_NONE, &first, &p ) == RN_WINDOW_SEND_NUMBERED )
                count = 1;
            if ( count )
            {
                check( first + count <= capacity );
                for ( uint32_t i = 0; i < count; i++ )
                {
                    channels_window_exchange_record_t * item = &records[first + i];
                    item->bytes = block ? ( i == 0 ? F : 17 ) : 16;
                    item->ring = (uint32_t) ( source->send_numbers[( first + i ) % W].ring_start % B / 16 );
                    item->ordering = (uint32_t) next_message;
                    item->fragment = (uint16_t) i;
                    item->block = (uint8_t) block;
                    for ( uint32_t j = 0; j < item->bytes; j++ )
                        item->data[j] = (uint8_t) ( ( first + i ) * 31 + j );
                }
                // Numbering and the first packet write are one operation in
                // this stand-in for layer 6 (CHANNELS 6.2, I3).
                newly_numbered = first;
                channels_window_exchange_write( wire, &address, &records[first], first,
                                                packet, sizeof(packet), now );
                next_message++;
            }
        }

        // Rewrites keep their original number and ring offset. Limit each
        // update's packets so the lossy link and receiver both make progress.
        const uint64_t numbered = send.next;
        // U' pins even an ACKed fragment of a partly ACKed block. Check
        // every sender reservation, including records the wire has not yet
        // delivered, against an independent byte shadow (RNRing LiveSpansDisjoint).
        memset( reserved, 0, sizeof(reserved) );
        for ( uint64_t n = send.U_prime; n < numbered; n++ )
        {
            const rn_send_number_t * entry = &source->send_numbers[n % W];
            check( entry->number == n );
            const uint32_t span = (uint32_t) entry->ring_units * 16;
            const uint32_t start = (uint32_t) ( entry->ring_start % B );
            check( rn_region_ring_span_valid( source, start, span ) );
            for ( uint32_t j = start; j < start + span; j++ )
            {
                check( reserved[j] == 0 );
                reserved[j] = (uint8_t) ( n + 1 );
            }
        }
        int sent = 0;
        for ( uint64_t scan = 0; scan < numbered && sent < 8; scan++ )
        {
            const uint64_t n = send_cursor++ % numbered;
            channels_window_exchange_record_t * item = &records[n];
            if ( item->acked || n == newly_numbered )
                continue;
            channels_window_exchange_write( wire, &address, item, n, packet, sizeof(packet), now );
            sent++;
        }

        // Layer 2 parses each RECORDS packet. The receive window accepts a
        // number once; our shadow set checks its whole live ring span.
        for ( int bytes = rocketnet_network_simulator_receive( wire, &address, now, &address, received );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &address, now, &address, received ) )
        {
            rocketnet_message_reader_t reader;
            check( rocketnet_message_reader_begin( &reader, received, bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
            rocketnet_message_record_t record;
            while ( rocketnet_message_reader_next( &reader, &record ) )
            {
                const uint64_t n = record.number;
                check( n < send.next && n < capacity );
                channels_window_exchange_record_t * item = &records[n];
                check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
                check( record.bytes == (int) item->bytes && record.ring == item->ring );
                check( memcmp( record.data, item->data, item->bytes ) == 0 );
                uint32_t slot = RN_DESCRIPTOR_NONE;
                const int answer = rn_window_receive_accept( &receive, &record, &slot );
                check( receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
                if ( answer == RN_WINDOW_RECEIVE_ACCEPT )
                {
                    check( !item->accepted && slot == n % W );
                    item->accepted = 1;
                    accepted_total++;
                    const uint32_t span = ( item->bytes + 15 ) & ~15u;
                    const uint32_t start = item->ring * 16;
                    check( rn_region_ring_span_valid( target, start, span ) );
                    for ( uint32_t j = start; j < start + span; j++ )
                    {
                        check( live[j] == 0 );
                        live[j] = (uint8_t) ( n + 1 );
                    }
                }
                else
                    check( answer == RN_WINDOW_RECEIVE_DUPLICATE && item->accepted );
            }
        }

        // Layer 5's stand-in holds a block until both fragments arrive;
        // singles and complete blocks release at this update's end.
        for ( uint64_t n = 0; n < send.next; n++ )
        {
            channels_window_exchange_record_t * item = &records[n];
            if ( !item->accepted || item->released )
                continue;
            if ( item->block )
            {
                const uint64_t first = n - item->fragment;
                if ( !records[first].accepted || !records[first + 1].accepted )
                    continue;
            }
            rn_window_receive_release( &receive, n );
            item->released = 1;
            item->ack_due = tick + 1;
            const uint32_t span = ( item->bytes + 15 ) & ~15u;
            const uint32_t start = item->ring * 16;
            for ( uint32_t j = start; j < start + span; j++ )
            {
                check( live[j] == (uint8_t) ( n + 1 ) );
                live[j] = 0;
            }
        }

        if ( next_message == messages && send.U == send.next )
            drained++;
        else
            drained = 0;
    }
    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( next_message == messages && drained == 16 );
    check( accepted_total == (int) send.next && receive.R == send.next );
    for ( uint64_t n = 0; n < send.next; n++ )
        check( records[n].accepted == 1 && records[n].released && records[n].acked );
    for ( uint32_t j = 0; j < B; j++ )
        check( live[j] == 0 );
    check( receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_network_simulator_destroy( wire );
    rn_arena_destroy( arena );
}

static void test_channels_window_send_and_receive_over_the_lossy_simulator()
{
    test_channels_window_send_and_receive_over_the_lossy_simulator_leg( 0.0f );
    test_channels_window_send_and_receive_over_the_lossy_simulator_leg( 10.0f );
    test_channels_window_send_and_receive_over_the_lossy_simulator_leg( 50.0f );
}

// ----------------------------------------------------------------
//      Layer 3: the sent-packet map, resend FIFO, RTT, resend delay (6.4)
// ----------------------------------------------------------------

static void test_channels_window_ack_of_an_unknown_id_is_ignored()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    const uint32_t d0 = rn_region_send_alloc( region, 64 );
    check( d0 != RN_DESCRIPTOR_NONE );
    uint64_t n0 = 0, p0 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, d0, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    check( n0 == 0 );
    rn_window_ack_commit_packet( &ack, 10, &n0, 1, 1.0 );

    check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    check( ack.resend_count == 1 );
    check( ack.has_rtt == 0 );
    check( ack.resend_delay == 2.0 );
    check( rn_window_ack_resend_delay( &ack ) == 2.0 );
    const uint32_t free_desc_before = region->descriptors_free;

    // Unknown packet_id (keep-alive or pure-unreliable ack, or 0): ignored completely.
    check( rn_window_ack_on_ack( &ack, 999, 1.05 ) == 0 );
    check( rn_window_ack_on_ack( &ack, 0, 1.05 ) == 0 );
    const uint64_t unknown_batch[3] = { 11, 42, 1000 };
    check( rn_window_ack_on_acks( &ack, unknown_batch, 3, 1.05 ) == 0 );

    check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    check( ack.resend_count == 1 );
    check( ack.has_rtt == 0 );
    check( ack.resend_delay == 2.0 );
    check( rn_window_ack_resend_delay( &ack ) == 2.0 );
    check( send.U == 0 && send.U_prime == 0 );
    check( region->descriptors_free == free_desc_before );

    // Valid ack of packet 10 at t = 1.08 samples RTT = 0.08s and advances U.
    check( rn_window_ack_on_ack( &ack, 10, 1.08 ) == 1 );
    check( ack.has_rtt == 1 );
    check( ack.srtt > 0.079999 && ack.srtt < 0.080001 );
    check( send.U == 1 && send.U_prime == 1 );
    check( rn_window_ack_entries_in_flight( &ack ) == 0 );

    // A duplicate ack of packet 10 is now unknown and ignored without altering RTT.
    const double srtt_after = ack.srtt;
    const double resend_delay_after = ack.resend_delay;
    check( rn_window_ack_on_ack( &ack, 10, 1.50 ) == 0 );
    check( ack.srtt == srtt_after && ack.resend_delay == resend_delay_after );

    rn_arena_destroy( arena );
}

static void test_channels_window_ack_frees_the_chunk_and_advances_u()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    const uint32_t small_total = rn_send_store_total_chunks( &region->store, RN_STORE_SMALL );
    const uint32_t large_total = rn_send_store_total_chunks( &region->store, RN_STORE_LARGE );
    const uint32_t desc_total = region->queue;

    // d0: 64-byte single message (n = 0)
    const uint32_t d0 = rn_region_send_alloc( region, 64 );
    // d1: 2500-byte 3-fragment block (n = 1, 2, 3)
    const uint32_t d1 = rn_region_send_alloc( region, 2500 );
    // d2: 64-byte single message (n = 4)
    const uint32_t d2 = rn_region_send_alloc( region, 64 );
    check( d0 != RN_DESCRIPTOR_NONE && d1 != RN_DESCRIPTOR_NONE && d2 != RN_DESCRIPTOR_NONE );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_total - 2 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large_total - 3 );
    check( region->descriptors_free == desc_total - 3 );

    uint64_t n0 = 0, p0 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, d0, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    uint64_t n1 = 0, p1 = 0;
    check( rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2500, d1, &n1, &p1 ) == 3 );
    uint64_t n4 = 0, p4 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 64, d2, &n4, &p4 ) == RN_WINDOW_SEND_NUMBERED );
    check( n0 == 0 && n1 == 1 && n4 == 4 );

    const uint64_t n2 = 2, n3 = 3;
    rn_window_ack_commit_packet( &ack, 1, &n0, 1, 1.0 );
    rn_window_ack_commit_packet( &ack, 2, &n1, 1, 1.0 );
    rn_window_ack_commit_packet( &ack, 3, &n2, 1, 1.0 );
    rn_window_ack_commit_packet( &ack, 4, &n3, 1, 1.0 );
    rn_window_ack_commit_packet( &ack, 5, &n4, 1, 1.0 );
    check( ack.resend_count == 5 );

    // Ack packet 3 (n = 2, middle fragment of block d1) out of order:
    // frees its one large chunk immediately, unlinks n = 2 from resend FIFO, keeps d1 in use, U stays 0.
    check( rn_window_ack_on_ack( &ack, 3, 1.05 ) == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large_total - 2 );
    check( region->send_descriptors[d1].in_use == 1 );
    check( region->descriptors_free == desc_total - 3 );
    check( ack.resend_count == 4 );
    check( send.U == 0 && send.U_prime == 0 );

    // Ack packet 1 (n = 0, single message d0):
    // frees its small chunk and descriptor d0, advances U and U' to 1.
    check( rn_window_ack_on_ack( &ack, 1, 1.06 ) == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_total - 1 );
    check( region->send_descriptors[d0].in_use == 0 );
    check( region->descriptors_free == desc_total - 2 );
    check( ack.resend_count == 3 );
    check( send.U == 1 && send.U_prime == 1 );

    // Ack packet 2 (n = 1, first fragment of block d1):
    // frees its large chunk, advances U past 1 and 2 to 3, but U' stops at 1 because n = 3 is still unacked!
    check( rn_window_ack_on_ack( &ack, 2, 1.07 ) == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large_total - 1 );
    check( region->send_descriptors[d1].in_use == 1 );
    check( ack.resend_count == 2 );
    check( send.U == 3 && send.U_prime == 1 );

    // Ack packet 4 (n = 3, last fragment of block d1):
    // frees its last large chunk and descriptor d1, advances U and U' to 4.
    check( rn_window_ack_on_ack( &ack, 4, 1.08 ) == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_LARGE ) == large_total );
    check( region->send_descriptors[d1].in_use == 0 );
    check( region->descriptors_free == desc_total - 1 );
    check( ack.resend_count == 1 );
    check( send.U == 4 && send.U_prime == 4 );

    // Ack packet 5 (n = 4, single message d2):
    // everything returned to the store and Q, U = U' = 5.
    check( rn_window_ack_on_ack( &ack, 5, 1.09 ) == 1 );
    check( rn_send_store_free_chunks( &region->store, RN_STORE_SMALL ) == small_total );
    check( region->descriptors_free == desc_total );
    check( ack.resend_count == 0 && ack.resend_head == RN_RESEND_NONE && ack.resend_tail == RN_RESEND_NONE );
    check( send.U == 5 && send.U_prime == 5 );

    rn_arena_destroy( arena );
}

static void test_channels_window_rtt_sample_is_unambiguous_across_resends()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 200;
    config.message_resend_delay_max_ms = 3000;                                   // so the resend delay formula shows unclamped
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );

    // Leg 1: original send in packet 1 at t = 1.00, resent in packet 2 at t = 1.25.
    // Late ack of packet 1 at t = 1.30 samples 1.30 - 1.00 = 0.30s.
    {
        rn_region_carve( region );
        rn_window_send_t send;
        rn_window_send_init( &send, region, 0 );
        rn_window_ack_t ack;
        rn_window_ack_init( &ack, &send, region, &config );

        uint64_t n0 = 0, p0 = 0;
        check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
        rn_window_ack_commit_packet( &ack, 1, &n0, 1, 1.00 );

        uint64_t next_pkt = 2;
        uint64_t resent[4];
        check( rn_window_ack_resend( &ack, 1.25, &next_pkt, resent, 4 ) == 1 );
        check( resent[0] == 0 && next_pkt == 3 );
        check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_RESENT ) == 1 );

        check( rn_window_ack_on_ack( &ack, 1, 1.30 ) == 1 );
        check( ack.has_rtt == 1 );
        check( ack.srtt > 0.299999 && ack.srtt < 0.300001 );
        check( ack.rttvar > 0.299999 && ack.rttvar < 0.300001 );
        check( ack.resend_delay > 1.829999 && ack.resend_delay < 1.830001 );     // 2 x 0.30 + 4 x 0.30 + 30 ms
        check( send.U == 1 && ack.resend_count == 0 );

        // Subsequent ack of packet 2 at t = 1.35 does not double-ack n0.
        check( rn_window_ack_on_ack( &ack, 2, 1.35 ) == 1 );
        check( send.U == 1 && ack.resend_count == 0 );
    }

    // Leg 2: original send in packet 10 at t = 2.00 (lost), resent in packet 11 at t = 2.25.
    // Ack of packet 11 at t = 2.33 samples 2.33 - 2.25 = 0.08s.
    {
        rn_region_carve( region );
        rn_window_send_t send;
        rn_window_send_init( &send, region, 0 );
        rn_window_ack_t ack;
        rn_window_ack_init( &ack, &send, region, &config );

        uint64_t n0 = 0, p0 = 0;
        check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
        rn_window_ack_commit_packet( &ack, 10, &n0, 1, 2.00 );

        uint64_t next_pkt = 11;
        uint64_t resent[4];
        check( rn_window_ack_resend( &ack, 2.25, &next_pkt, resent, 4 ) == 1 );
        check( resent[0] == 0 && next_pkt == 12 );

        check( rn_window_ack_on_ack( &ack, 11, 2.33 ) == 1 );
        check( ack.has_rtt == 1 );
        check( ack.srtt > 0.079999 && ack.srtt < 0.080001 );
        check( ack.rttvar > 0.079999 && ack.rttvar < 0.080001 );
        check( ack.resend_delay > 0.509999 && ack.resend_delay < 0.510001 );     // 2 x 0.08 + 4 x 0.08 + 30 ms
        check( send.U == 1 && ack.resend_count == 0 );
    }

    rn_arena_destroy( arena );
}

static void test_channels_window_eviction_keeps_numbers_resendable()
{
    // The map holds at least W entries (packets_in_flight >= reliable_messages_in_flight,
    // tla/RNSentMap.tla), so W numbers sent one to a packet never evict one
    // another. What the map does evict is a packet whose numbers have since been
    // resent: here 20 numbers are written four times, a resend delay apart,
    // and the fourth pass evicts the first pass's oldest 16 entries.
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.packets_in_flight = 64;                  // P = W = 64, 16P = 1024
    config.max_message_bytes = 64 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config.message_resend_delay_initial_ms = 200;
    check( rocketnet_message_validate_config( &config ) == 1 );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    // Send 20 packets (packet_id 1..20, carrying n = 0..19) at t = 1.00.
    for ( uint32_t i = 0; i < 20; i++ )
    {
        uint64_t n = 0, p = 0;
        check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
        check( n == i );
        rn_window_ack_commit_packet( &ack, (uint64_t) ( i + 1 ), &n, 1, 1.00 );
    }
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );

    // Nothing is acked. Three resend passes at t = 1.25, 1.50 and 1.75 write
    // packets 100..159; the third fills the map past 64 and evicts packets 1..16.
    uint64_t next_pkt = 100;
    uint64_t resent[32];
    const double pass_time[3] = { 1.25, 1.50, 1.75 };
    for ( int pass = 0; pass < 3; pass++ )
    {
        const uint32_t count = rn_window_ack_resend( &ack, pass_time[pass], &next_pkt, resent, 32 );
        check( count == 20 );
        for ( uint32_t i = 0; i < 20; i++ )
            check( resent[i] == i );
    }
    check( next_pkt == 160 );
    check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 16 );
    check( rn_window_ack_entries_in_flight( &ack ) == 64 );
    check( ack.resend_count == 20 );

    // Evicted packet_ids 1..4 are no longer in sent_index: a late ack of one is ignored.
    for ( uint64_t id = 1; id <= 4; id++ )
        check( rn_window_ack_on_ack( &ack, id, 1.80 ) == 0 );
    check( send.U == 0 );

    // The numbers stayed resendable: acking the last pass (140..159) brings U to 20.
    for ( uint64_t id = 140; id < 160; id++ )
        check( rn_window_ack_on_ack( &ack, id, 1.80 ) == 1 );
    check( send.U == 20 && send.U_prime == 20 && ack.resend_count == 0 );

    rn_arena_destroy( arena );
}

static void test_channels_window_resend_delay_never_backs_off()
{
    // tla/RNResendTiming.tla: every resend waits the adaptive resend delay, which does
    // not back off; an ack updates the samples and therefore the resend delay
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 250;
    config.message_resend_delay_max_ms = 2000;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    const double t0 = 1000.0;
    uint64_t n0 = 0, p0 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &ack, 1, &n0, 1, t0 );
    check( ack.resend_delay == 0.25 && rn_window_ack_resend_delay( &ack ) == 0.25 );     // configured initial resend delay

    uint64_t next_pkt = 2;
    uint64_t out[4];

    // four passes, each due 250 ms after the last: the delay does not grow
    double last = t0;
    for ( int pass = 0; pass < 4; pass++ )
    {
        check( rn_window_ack_resend( &ack, last + 0.2499, &next_pkt, out, 4 ) == 0 );
        check( rn_window_ack_resend( &ack, last + 0.2501, &next_pkt, out, 4 ) == 1 );
        check( out[0] == n0 );
        last += 0.2501;
        check( rn_window_ack_resend_delay( &ack ) == 0.25 ); // no fixed resend interval cap
    }

    // an ack of the latest send takes a sample and the delay follows the resend delay
    check( rn_window_ack_on_ack( &ack, next_pkt - 1, last + 0.04 ) == 1 );
    check( ack.has_rtt == 1 );
    check( rn_window_ack_resend_delay( &ack ) == ack.resend_delay );

    rn_arena_destroy( arena );
}

static void test_channels_window_resend_walk_stops_at_the_first_not_due()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 200;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    const double send_times[5] = { 1.00, 1.05, 1.20, 1.25, 1.30 };
    for ( uint32_t i = 0; i < 5; i++ )
    {
        uint64_t n = 0, p = 0;
        check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n, &p ) == RN_WINDOW_SEND_NUMBERED );
        rn_window_ack_commit_packet( &ack, (uint64_t) ( i + 1 ), &n, 1, send_times[i] );
    }
    check( ack.resend_head == 0 && ack.resend_tail == 4 && ack.resend_count == 5 );

    // At t = 1.26 (resend_delay = 0.20, backoff = 1):
    // n = 0 (due 1.20) and n = 1 (due 1.25) are due; n = 2 (due 1.40) is not due.
    uint64_t next_pkt = 10;
    uint64_t resent[8];
    const uint32_t count = rn_window_ack_resend( &ack, 1.26, &next_pkt, resent, 8 );
    check( count == 2 );
    check( resent[0] == 0 && resent[1] == 1 );
    check( ack.resend_head == 2 && ack.resend_tail == 1 && ack.resend_count == 5 );
    check( region->send_numbers[0].last_send == 1.26 );
    check( region->send_numbers[1].last_send == 1.26 );
    check( region->send_numbers[2].last_send == 1.20 );

    // Calling resend again at t = 1.26 sees head (n = 2) is not due and stops immediately.
    check( rn_window_ack_resend( &ack, 1.26, &next_pkt, resent, 8 ) == 0 );
    check( ack.resend_head == 2 );

    rn_arena_destroy( arena );
}

static void test_channels_window_failed_send_marks_due_now()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 200;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    // n = 0 sent normally in packet 1 at t = 1.00 (due at 1.20).
    uint64_t n0 = 0, p0 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &ack, 1, &n0, 1, 1.00 );

    // n = 1 and n = 2 numbered for a packet at t = 1.02 whose finish_packet refuses.
    uint64_t n1 = 0, p1 = 0, n2 = 0, p2 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, RN_DESCRIPTOR_NONE, &n1, &p1 ) == RN_WINDOW_SEND_NUMBERED );
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 48, RN_DESCRIPTOR_NONE, &n2, &p2 ) == RN_WINDOW_SEND_NUMBERED );
    check( n1 == 1 && p1 == 16 && n2 == 2 && p2 == 48 );

    const uint64_t failed[2] = { n1, n2 };
    rn_window_ack_packet_failed( &ack, failed, 2, 1.02 );

    // Not entered in the sent-packet map; n = 1, 2 keep their numbers and ring offsets,
    // and sit at the head of the resend FIFO ahead of n = 0.
    check( rn_window_ack_entries_in_flight( &ack ) == 1 );
    check( region->send_numbers[1].number == 1 && region->send_numbers[1].ring_start == 16 );
    check( region->send_numbers[2].number == 2 && region->send_numbers[2].ring_start == 48 );
    check( ack.resend_count == 3 );
    check( ack.resend_head == 1 );
    check( region->resend_fifo[1].next == 2 );
    check( region->resend_fifo[2].next == 0 );
    check( ack.resend_tail == 0 );

    // At t = 1.02 (no resend delay wait), resend immediately emits n = 1 and n = 2, and stops at n = 0.
    uint64_t next_pkt = 2;
    uint64_t out[4];
    check( rn_window_ack_resend( &ack, 1.02, &next_pkt, out, 4 ) == 2 );
    check( out[0] == 1 && out[1] == 2 );
    check( rn_window_ack_resend_delay( &ack ) == 0.2 );        // a failed send leaves the configured initial resend delay alone
    check( ack.resend_head == 0 && ack.resend_tail == 2 );
    check( rn_window_ack_entries_in_flight( &ack ) == 3 );

    rn_arena_destroy( arena );
}

static void test_channels_window_reliable_timeout_enters_the_error_state()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 250;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_region_carve( region );

    rn_window_send_t send;
    rn_window_send_init( &send, region, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, region, &config );

    // First send of n = 0 at t = 5.00 (reliable_timeout = 10.0s -> expires at t = 15.00).
    uint64_t n0 = 0, p0 = 0;
    check( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &ack, 1, &n0, 1, 5.00 );

    // Resend across several resend delay passes so last_send advances while first_send stays 5.00.
    uint64_t next_pkt = 2;
    uint64_t out[4];
    check( rn_window_ack_resend( &ack, 5.25, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 5.75, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 6.75, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 8.75, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 10.75, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 12.75, &next_pkt, out, 4 ) == 1 );
    check( rn_window_ack_resend( &ack, 14.75, &next_pkt, out, 4 ) == 1 );
    check( region->send_numbers[0].last_send == 14.75 );

    // Just before 15.00: still alive.
    check( rn_window_ack_check_timeout( &ack, 14.999 ) == 0 );
    check( ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

    // At t = 15.00 (now - first_send == 10.0 == reliable_timeout): enters error state and stops resending.
    check( rn_window_ack_resend( &ack, 15.00, &next_pkt, out, 4 ) == 0 );
    check( ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( rn_window_ack_check_timeout( &ack, 15.00 ) == 1 );

    rn_arena_destroy( arena );
}

struct channels_lossy_soak_record_t
{
    uint32_t bytes;
    uint32_t ring;
    uint32_t ordering;
    uint16_t fragment;
    uint16_t fragments;
    uint8_t block;
    uint8_t accepted;
    uint8_t released;
    uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
};

static const uint8_t * channels_lossy_soak_fragment_chunk_data( const rn_region_t * region, const rn_send_number_t * slot )
{
    check( slot->descriptor != RN_DESCRIPTOR_NONE && slot->descriptor < region->queue );
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[slot->descriptor];
    check( descriptor->in_use && descriptor->chunk != RN_CHUNK_NONE );

    uint32_t chain_index = 0;
    if ( descriptor->fragments > 1 )
    {
        for ( uint16_t j = 0; j < slot->fragment; j++ )
        {
            const uint64_t prev_num = slot->number - (uint64_t) ( slot->fragment - j );
            const rn_send_number_t * prev_slot = &region->send_numbers[prev_num & ( region->window - 1 )];
            if ( prev_slot->descriptor == slot->descriptor && prev_slot->number == prev_num && !prev_slot->acked )
                chain_index++;
        }
    }

    uint32_t chunk = descriptor->chunk;
    for ( uint32_t step = 0; step < chain_index; step++ )
    {
        chunk = rn_send_store_chunk_next( &region->store, chunk );
        check( chunk != RN_CHUNK_NONE );
    }
    return rn_send_store_chunk_data( &region->store, chunk );
}

static void channels_lossy_soak_send_wire_packet( rocketnet_network_simulator_t * wire,
                                                  const rocketnet_address_t * from,
                                                  const rocketnet_address_t * to,
                                                  const rn_region_t * sender_region,
                                                  const channels_lossy_soak_record_t * records,
                                                  uint64_t packet_id,
                                                  const uint64_t * numbers,
                                                  uint32_t count,
                                                  double now )
{
    // Wire frame format inside the simulator: 8-byte little-endian packet_id
    // prefix followed by a Layer 2 RECORDS payload.
    uint8_t frame[8 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * p = frame;
    rocketnet_write_uint64( &p, packet_id );

    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, frame + 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    for ( uint32_t i = 0; i < count; i++ )
    {
        const uint64_t n = numbers[i];
        const rn_send_number_t * slot = &sender_region->send_numbers[n & ( sender_region->window - 1 )];
        check( slot->number == n && !slot->acked );
        const channels_lossy_soak_record_t * item = &records[n];
        const uint8_t * chunk_bytes = channels_lossy_soak_fragment_chunk_data( sender_region, slot );
        check( memcmp( chunk_bytes, item->data, item->bytes ) == 0 );

        rocketnet_message_record_t rec;
        memset( &rec, 0, sizeof(rec) );
        rec.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
        rec.block = item->block;
        rec.number = (uint32_t) ( n & ROCKETNET_MESSAGE_U24_MASK );
        rec.ring = item->ring;
        rec.ordering = item->ordering & ROCKETNET_MESSAGE_U24_MASK;
        rec.channel = 0;
        rec.fragment = item->fragment;
        rec.fragments = item->block ? ( item->fragments - 1 ) : 0;
        rec.data = chunk_bytes;
        rec.bytes = (int) item->bytes;
        check( rocketnet_message_writer_add( &writer, &rec ) == 1 );
    }
    const int payload_bytes = rocketnet_message_writer_finish( &writer );
    check( payload_bytes > 0 );
    rocketnet_network_simulator_send( wire, from, to, frame, 8 + payload_bytes, now );
}

static void test_channels_window_lossy_soak_leg( float loss_percent )
{
    const uint32_t W = 64;
    const uint32_t P = 64;                          // at least W (the sent-map rule of 14)
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t B = 8 * F;
    const int total_messages = 140;
    const uint32_t max_records = 512;

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 4;
    config.reliable_messages_in_flight = (int) W;
    config.receive_message_buffer_bytes = (int) B;
    config.max_message_bytes = (int) ( 3 * F );
    config.send_message_buffer_bytes = 65536;
    config.send_queue_messages = 64;
    config.packets_in_flight = (int) P;
    config.message_resend_delay_min_ms = 20;
    config.message_resend_delay_initial_ms = 50;
    config.message_resend_delay_max_ms = 400;
    config.reliable_message_timeout_ms = 60000;

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 2, &counters );
    check( arena );
    rn_region_t * source = rn_arena_region( arena, 0 );
    rn_region_t * target = rn_arena_region( arena, 1 );
    rn_region_carve( source );
    rn_region_carve( target );

    rn_window_send_t send;
    rn_window_send_init( &send, source, 0 );
    rn_window_ack_t ack;
    rn_window_ack_init( &ack, &send, source, &config );
    rn_window_receive_t receive;
    rn_window_receive_init( &receive, target, 0 );

    rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create(
        NULL, 8192, 0x5EED1234ULL + (uint64_t) loss_percent );
    check( wire );
    rocketnet_network_simulator_config( wire, 0.02, 0.015, loss_percent, 25.0f );

    rocketnet_address_t sender_addr, receiver_addr;
    check( rocketnet_address_parse( &sender_addr, "127.0.0.1:41000" ) );
    check( rocketnet_address_parse( &receiver_addr, "127.0.0.1:41001" ) );

    rocketnet_message_limits_t limits;
    limits.num_channels = config.num_channels;
    limits.enabled_codes = rocketnet_message_enabled_codes( 0x09 );
    limits.reliable_messages_in_flight = (int) W;
    limits.max_message_bytes = config.max_message_bytes;
    limits.receive_message_buffer_bytes = (int) B;

    static channels_lossy_soak_record_t records[max_records];
    memset( records, 0, sizeof(records) );
    uint8_t live[8 * ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    memset( live, 0, sizeof(live) );
    uint8_t received[8 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    uint64_t next_packet_id = 1;
    int next_message = 0;
    int accepted_total = 0;
    int drained = 0;
    double now = 1.0;

    rn_trap_phase = "window_lossy_soak";
    rn_trap_armed = 1;

    for ( int tick = 0; tick < 20000 && drained < 16; tick++ )
    {
        now += 0.005;

        // 1. Sender receives transport ACK frames from the simulator and feeds
        //    rn_window_ack_on_acks.
        rocketnet_address_t from_addr;
        for ( int bytes = rocketnet_network_simulator_receive( wire, &sender_addr, now, &from_addr, received );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &sender_addr, now, &from_addr, received ) )
        {
            check( bytes >= 4 && ( ( bytes - 4 ) % 8 ) == 0 );
            const uint8_t * q = received;
            const uint32_t ack_count = rocketnet_read_uint32( &q );
            check( bytes == (int) ( 4 + 8 * ack_count ) && ack_count <= 64 );
            uint64_t ack_ids[64];
            for ( uint32_t i = 0; i < ack_count; i++ )
                ack_ids[i] = rocketnet_read_uint64( &q );
            rn_window_ack_on_acks( &ack, ack_ids, ack_count, now );
        }

        // 2. Resend due numbers from the resend FIFO, one packet per tick: the
        //    per-update packet limit the writers keep (packets_per_update),
        //    so the packets written in one RTT stay under P and acks land on
        //    live sent-map entries (standard/MESSAGES.md m9.1). With no
        //    back-off an unbounded resend walk at a small P evicts every entry
        //    before its ack returns, and the soak ends in RELIABLE_TIMEOUT.
        uint64_t resent_numbers[32];
        const uint32_t resent_count = rn_window_ack_resend( &ack, now, &next_packet_id, resent_numbers, 1 );
        check( ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
        for ( uint32_t i = 0; i < resent_count; i++ )
        {
            // rn_window_ack_resend assigned packet_ids [next_packet_id - resent_count + i]
            const uint64_t pkt_id = next_packet_id - resent_count + i;
            const uint64_t num = resent_numbers[i];
            channels_lossy_soak_send_wire_packet( wire, &sender_addr, &receiver_addr, source,
                                                  records, pkt_id, &num, 1, now );
        }

        // 3. Number and send up to 2 new messages per tick (mixing small, medium,
        //    and multi-fragment blocks). Every 23rd message simulates a
        //    finish_packet failure so failed-send recovery is exercised too.
        for ( int step = 0; step < 2 && next_message < total_messages; step++ )
        {
            const int is_block = ( next_message % 6 == 0 );
            const int msg_bytes = is_block ? (int) ( 2 * F + 32 ) : ( ( next_message % 3 == 0 ) ? 256 : 48 );

            const uint32_t d = rn_region_send_alloc( source, msg_bytes );
            if ( d == RN_DESCRIPTOR_NONE )
            {
                // If the region is exhausted, reset it (carve) and retry this message.
                if ( rn_region_exhausted( source ) )
                {
                    rn_region_carve( source );
                    // Also need to reset the window send/ack state since the region was carved.
                    rn_window_send_init( &send, source, 0 );
                    rn_window_ack_init( &ack, &send, source, &config );
                    step--;  // retry this message
                    continue;
                }
                break;
            }

            uint64_t first = 0, ring_pos = 0;
            uint32_t count = 0;
            if ( is_block )
            {
                count = rn_window_send_first_write_block( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                          (uint32_t) msg_bytes, d, &first, &ring_pos );
            }
            else if ( rn_window_send_first_write( &send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED,
                                                  (uint32_t) msg_bytes, d, &first, &ring_pos ) == RN_WINDOW_SEND_NUMBERED )
            {
                count = 1;
            }

            if ( count == 0 )
            {
                rn_region_send_free( source, d );
                break;
            }

            check( first + count <= max_records );
            rn_send_descriptor_t * descriptor = &source->send_descriptors[d];
            descriptor->first_number = first;
            descriptor->fragments = (uint16_t) count;
            descriptor->fragments_acked = 0;

            uint32_t chunk = descriptor->chunk;
            uint64_t nums[4];
            for ( uint32_t i = 0; i < count; i++ )
            {
                const uint64_t n = first + i;
                nums[i] = n;
                const uint32_t frag_bytes = is_block ? ( i + 1 < count ? F : ( (uint32_t) msg_bytes - i * F ) ) : (uint32_t) msg_bytes;
                channels_lossy_soak_record_t * item = &records[n];
                item->bytes = frag_bytes;
                item->ring = (uint32_t) ( ( source->send_numbers[n % W].ring_start % B ) / 16 );
                item->ordering = (uint32_t) next_message;
                item->fragment = (uint16_t) i;
                item->fragments = (uint16_t) count;
                item->block = (uint8_t) is_block;
                for ( uint32_t b = 0; b < frag_bytes; b++ )
                    item->data[b] = (uint8_t) ( n * 37u + b * 13u + 1u );

                check( chunk != RN_CHUNK_NONE );
                memcpy( rn_send_store_chunk_data( &source->store, chunk ), item->data, frag_bytes );
                chunk = rn_send_store_chunk_next( &source->store, chunk );
            }

            if ( next_message % 23 == 11 )
            {
                rn_window_ack_packet_failed( &ack, nums, count, now );
            }
            else
            {
                for ( uint32_t i = 0; i < count; i++ )
                {
                    const uint64_t pkt_id = next_packet_id++;
                    rn_window_ack_commit_packet( &ack, pkt_id, &nums[i], 1, now );
                    channels_lossy_soak_send_wire_packet( wire, &sender_addr, &receiver_addr, source,
                                                          records, pkt_id, &nums[i], 1, now );
                }
            }
            next_message++;
        }

        // 4. Assert TablesBounded on the sent-packet map at every tick.
        check( rn_window_ack_entries_in_flight( &ack ) <= P );
        check( ack.pkt_tail - ack.pkt_head <= P );
        check( rn_window_ack_refs_in_flight( &ack ) <= 16 * P );
        check( ack.resend_count <= W );
        rn_soak_check_accounting( source );

        // 5. Receiver opens incoming packets, parses Layer 2 records, runs
        //    rn_window_receive_accept, and sends back a simulated transport ACK
        //    frame listing the received packet_ids (plus an occasional unknown
        //    keep-alive sequence id to exercise unknown-ack ignoring).
        uint64_t ack_out[64];
        uint32_t ack_out_count = 0;
        for ( int bytes = rocketnet_network_simulator_receive( wire, &receiver_addr, now, &from_addr, received );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &receiver_addr, now, &from_addr, received ) )
        {
            check( bytes > 8 );
            const uint8_t * q = received;
            const uint64_t pkt_id = rocketnet_read_uint64( &q );
            if ( ack_out_count < 62 )
                ack_out[ack_out_count++] = pkt_id;

            rocketnet_message_reader_t reader;
            check( rocketnet_message_reader_begin( &reader, received + 8, bytes - 8, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
            rocketnet_message_record_t record;
            while ( rocketnet_message_reader_next( &reader, &record ) )
            {
                const uint64_t n = record.number;
                check( n < send.next && n < max_records );
                channels_lossy_soak_record_t * item = &records[n];
                check( record.code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
                check( record.bytes == (int) item->bytes && record.ring == item->ring );
                check( memcmp( record.data, item->data, item->bytes ) == 0 );

                uint32_t slot = RN_DESCRIPTOR_NONE;
                const int answer = rn_window_receive_accept( &receive, &record, &slot );
                check( receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
                if ( answer == RN_WINDOW_RECEIVE_ACCEPT )
                {
                    check( !item->accepted && slot == n % W );
                    item->accepted = 1;
                    accepted_total++;
                    const uint32_t span = ( item->bytes + 15 ) & ~15u;
                    const uint32_t start = item->ring * 16;
                    check( rn_region_ring_span_valid( target, start, span ) );
                    for ( uint32_t j = start; j < start + span; j++ )
                    {
                        check( live[j] == 0 );
                        live[j] = (uint8_t) ( ( n & 0x7F ) + 1 );
                    }
                }
                else
                {
                    check( answer == RN_WINDOW_RECEIVE_DUPLICATE && item->accepted );
                }
            }
        }

        if ( ack_out_count > 0 )
        {
            // Piggyback an unknown transport sequence (keep-alive) on every 5th ACK frame.
            if ( ( tick % 5 == 0 ) && ack_out_count < 64 )
                ack_out[ack_out_count++] = 0x10000000 + (uint64_t) tick;

            uint8_t ack_frame[4 + 64 * 8];
            uint8_t * p = ack_frame;
            rocketnet_write_uint32( &p, ack_out_count );
            for ( uint32_t i = 0; i < ack_out_count; i++ )
                rocketnet_write_uint64( &p, ack_out[i] );
            rocketnet_network_simulator_send( wire, &receiver_addr, &sender_addr,
                                              ack_frame, (int) ( 4 + 8 * ack_out_count ), now );
        }

        // 6. Release completed singles and blocks at the receiver.
        for ( uint64_t n = 0; n < send.next; n++ )
        {
            channels_lossy_soak_record_t * item = &records[n];
            if ( !item->accepted || item->released )
                continue;
            if ( item->block )
            {
                const uint64_t first = n - item->fragment;
                bool all_have = true;
                for ( uint16_t f = 0; f < item->fragments; f++ )
                {
                    if ( !records[first + f].accepted )
                    {
                        all_have = false;
                        break;
                    }
                }
                if ( !all_have )
                    continue;
            }
            rn_window_receive_release( &receive, n );
            item->released = 1;
            const uint32_t span = ( item->bytes + 15 ) & ~15u;
            const uint32_t start = item->ring * 16;
            for ( uint32_t j = start; j < start + span; j++ )
            {
                check( live[j] == (uint8_t) ( ( n & 0x7F ) + 1 ) );
                live[j] = 0;
            }
        }

        if ( next_message == total_messages && send.U == send.next && send.U_prime == send.next && receive.R == send.next )
            drained++;
        else
            drained = 0;
    }

    rn_trap_armed = 0;
    rn_trap_phase = "idle";

    check( next_message == total_messages && drained == 16 );
    check( accepted_total == (int) send.next );
    check( receive.R == send.next && send.U == send.next && send.U_prime == send.next );
    check( ack.resend_count == 0 && ack.resend_head == RN_RESEND_NONE && ack.resend_tail == RN_RESEND_NONE );
    check( ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( source->descriptors_free == source->queue );
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
        check( rn_send_store_free_chunks( &source->store, c ) == rn_send_store_total_chunks( &source->store, c ) );
    for ( uint64_t n = 0; n < send.next; n++ )
        check( records[n].accepted == 1 && records[n].released == 1 && source->send_numbers[n % W].acked == 1 );
    for ( uint32_t j = 0; j < B; j++ )
        check( live[j] == 0 );
    if ( loss_percent >= 50.0f )
    {
        check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );
        check( rn_counter( &counters, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) > 0 );
    }

    rocketnet_network_simulator_destroy( wire );
    rn_arena_destroy( arena );
}

// The same lossy soak on the walk the product runs: two rn_ordered_t over the
// simulated wire, messages through rn_channels_start and rn_channels_finish,
// packets from rn_ordered_write (its resend FIFO walk, sdk/rocketnet_channel.cpp),
// transport ack ids fed to rn_ordered_acks. rn_window_ack_resend is not called
// here: this leg goes red when the product's walk stops resending a sent number.
static void test_channels_window_lossy_soak_product_leg( float loss_percent )
{
    const int total_messages = 140;
    const int num_channels = 4;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = num_channels;
    config.reliable_messages_in_flight = 64;
    config.receive_message_buffer_bytes = (int) ( 128 * F );
    config.max_message_bytes = (int) ( 3 * F );
    config.send_message_buffer_bytes = 262144;
    config.send_queue_messages = 256;
    config.packets_in_flight = 64;                  // at least W (the sent-map rule of 14)
    config.message_resend_delay_min_ms = 20;
    config.message_resend_delay_initial_ms = 50;
    config.message_resend_delay_max_ms = 400;
    config.reliable_message_timeout_ms = 60000;

    rocketnet_counters_t counters[2];
    memset( counters, 0, sizeof(counters) );
    rn_arena_t * arenas[2];
    static rn_ordered_t nodes[2];
    for ( int i = 0; i < 2; i++ )
    {
        arenas[i] = rn_arena_create( NULL, &config, 1, &counters[i] );
        check( arenas[i] );
        rn_region_t * region = rn_arena_region( arenas[i], 0 );
        rn_region_carve( region );
        memset( &nodes[i], 0, sizeof(nodes[i]) );
        check( rn_ordered_init( &nodes[i], region, &config, NULL, 4, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    }
    rn_ordered_t * sender = &nodes[0];
    rn_ordered_t * receiver = &nodes[1];

    rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create(
        NULL, 8192, 0x5EED1234ULL + (uint64_t) loss_percent );
    check( wire );
    rocketnet_network_simulator_config( wire, 0.02, 0.015, loss_percent, 25.0f );

    rocketnet_address_t sender_addr, receiver_addr;
    check( rocketnet_address_parse( &sender_addr, "127.0.0.1:41000" ) );
    check( rocketnet_address_parse( &receiver_addr, "127.0.0.1:41001" ) );

    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
        pointers[i] = frames[i];

    uint8_t wire_bytes[8 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t next_packet_id = 1;
    int packets_written = 0;
    int next_message = 0;
    int delivered_total = 0;
    uint64_t last_delivered[ROCKETNET_MAX_CHANNELS];
    memset( last_delivered, 0, sizeof(last_delivered) );
    int drained = 0;
    double now = 1000.0;

    for ( int tick = 0; tick < 20000 && drained < 16; tick++ )
    {
        now += 0.005;

        // 1. the sender takes the transport's ack ids
        rocketnet_address_t from_addr;
        for ( int bytes = rocketnet_network_simulator_receive( wire, &sender_addr, now, &from_addr, wire_bytes );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &sender_addr, now, &from_addr, wire_bytes ) )
        {
            check( bytes >= 4 && ( ( bytes - 4 ) % 8 ) == 0 );
            const uint8_t * q = wire_bytes;
            const uint32_t ack_count = rocketnet_read_uint32( &q );
            check( bytes == (int) ( 4 + 8 * ack_count ) && ack_count <= 64 );
            uint64_t ack_ids[64];
            for ( uint32_t i = 0; i < ack_count; i++ )
                ack_ids[i] = rocketnet_read_uint64( &q );
            rn_ordered_acks( sender, ack_ids, ack_count, now );
        }

        // 2. up to 2 new messages per tick, small and multi-chunk, over the channels
        for ( int step = 0; step < 2 && next_message < total_messages; step++ )
        {
            const bool is_block = ( next_message % 6 == 0 );
            const uint32_t msg_bytes = is_block ? 2 * F + 32 : ( ( next_message % 3 == 0 ) ? 256 : 48 );
            const uint32_t channel = (uint32_t) ( next_message % num_channels );
            uint8_t * p = rn_channels_start( &sender->channels, channel, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, msg_bytes );
            if ( !p )
                break;
            const uint64_t id = (uint64_t) next_message + 1;
            memcpy( p, &id, 8 );
            for ( uint32_t b = 8; b < msg_bytes; b++ )
                p[b] = (uint8_t) ( id * 37u + b * 13u + 1u );
            check( rn_channels_finish( &sender->channels, msg_bytes ) );
            next_message++;
        }

        // 3. the product's write: its resend FIFO walk and its new numbers, one
        //    packet per tick, so the packets written in one RTT stay under P and
        //    acks land on live sent-map entries (standard/MESSAGES.md m9.1).
        //    With no back-off (6.4, RNResendTiming) a write of every due
        //    number at a small P evicts each entry before its ack returns, and the
        //    soak ends in RELIABLE_TIMEOUT. Every 23rd packet is a failed send
        //    (resent first next update, 6.4).
        const int written = rn_ordered_write( sender, now, pointers, 1 );
        check( sender->ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
        for ( int p = 0; p < written; p++ )
        {
            if ( packets_written++ % 23 == 11 )
            {
                rn_ordered_packet_failed( sender, p, now );
                continue;
            }
            const uint64_t packet_id = next_packet_id++;
            const int payload_bytes = (int) sender->builder.packet[p].payload_bytes;
            rn_ordered_commit( sender, p, packet_id, now );
            uint8_t * w = wire_bytes;
            rocketnet_write_uint64( &w, packet_id );
            memcpy( w, frames[p], (size_t) payload_bytes );
            rocketnet_network_simulator_send( wire, &sender_addr, &receiver_addr, wire_bytes, 8 + payload_bytes, now );
        }

        // 4. the receiver opens each packet, drains and checks its deliveries,
        //    and sends back the transport's ack ids
        uint64_t ack_out[64];
        uint32_t ack_out_count = 0;
        for ( int bytes = rocketnet_network_simulator_receive( wire, &receiver_addr, now, &from_addr, wire_bytes );
              bytes > 0;
              bytes = rocketnet_network_simulator_receive( wire, &receiver_addr, now, &from_addr, wire_bytes ) )
        {
            check( bytes > 8 );
            const uint8_t * q = wire_bytes;
            const uint64_t packet_id = rocketnet_read_uint64( &q );
            if ( ack_out_count < 64 )
                ack_out[ack_out_count++] = packet_id;
            check( rn_ordered_receive( receiver, wire_bytes + 8, bytes - 8 ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
            const uint32_t n = rn_ordered_deliveries( receiver );
            for ( uint32_t i = 0; i < n; i++ )
            {
                const rn_delivery_t & delivery = receiver->channels.region->deliveries[i];
                check( delivery.bytes >= 48 );
                uint64_t id;
                memcpy( &id, delivery.data, 8 );
                check( id >= 1 && id <= (uint64_t) total_messages && delivery.channel == ( id - 1 ) % (uint64_t) num_channels );
                for ( int b = 8; b < delivery.bytes; b++ )
                    check( delivery.data[b] == (uint8_t) ( id * 37u + (uint32_t) b * 13u + 1u ) );
                check( id > last_delivered[delivery.channel] );     // ordered, once
                last_delivered[delivery.channel] = id;
                delivered_total++;
            }
            rn_ordered_end_update( receiver );
        }
        if ( ack_out_count > 0 )
        {
            uint8_t ack_frame[4 + 64 * 8];
            uint8_t * w = ack_frame;
            rocketnet_write_uint32( &w, ack_out_count );
            for ( uint32_t i = 0; i < ack_out_count; i++ )
                rocketnet_write_uint64( &w, ack_out[i] );
            rocketnet_network_simulator_send( wire, &receiver_addr, &sender_addr, ack_frame, (int) ( 4 + 8 * ack_out_count ), now );
        }

        if ( next_message == total_messages && delivered_total == total_messages && sender->ack.resend_count == 0 )
            drained++;
        else
            drained = 0;
    }

    check( next_message == total_messages && drained == 16 );
    check( delivered_total == total_messages );
    check( sender->send.U == sender->send.next && sender->ack.resend_head == RN_RESEND_NONE );
    check( rn_ordered_decide( sender ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_ordered_decide( receiver ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( sender->channels.region->descriptors_free == sender->channels.region->queue );
    if ( loss_percent >= 50.0f )
        check( rn_counter( &counters[0], ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );

    rocketnet_network_simulator_destroy( wire );
    rn_arena_destroy( arenas[0] );
    rn_arena_destroy( arenas[1] );
}

static void test_channels_window_lossy_soak()
{
    test_channels_window_lossy_soak_leg( 0.0f );
    test_channels_window_lossy_soak_leg( 10.0f );
    test_channels_window_lossy_soak_leg( 50.0f );
    test_channels_window_lossy_soak_product_leg( 0.0f );
    test_channels_window_lossy_soak_product_leg( 10.0f );
    test_channels_window_lossy_soak_product_leg( 50.0f );
}

// -----------------------------------------
// Reversed-witness replays (CHANNELS 15, tla/README-RNWindow.md, tla/README-RNRing.md).
// Each test drives the concrete Layer 3 modules through the exact TLC witness
// trace with the guarding rule in place, and fails on the named invariant
// when that one-line rule is removed on the bench.

struct channels_window_witness_rig_t
{
    rn_region_t region;
    rn_send_number_t send_numbers[8];
    double send_first_sends[8];
    rn_sent_packet_t sent_packets[8];
    rn_sent_index_t sent_index[16];
    uint32_t sent_refs[128];
    rn_resend_link_t resend_fifo[8];
    uint64_t received_bits[1];
    rn_receive_descriptor_t receive_descriptors[8];
    rocketnet_counters_t counters;
    rn_window_send_t send;
    rn_window_ack_t ack;
    rn_window_receive_t receive;
};

static void channels_window_witness_setup( channels_window_witness_rig_t * rig, uint32_t W, uint32_t B )
{
    memset( rig, 0, sizeof( *rig ) );
    rig->region.window = W;
    rig->region.ring_bytes = B;
    rig->region.max_message_bytes = B;
    rig->region.packets = 8;
    rig->region.sent_index_entries = 16;
    rig->region.sent_ref_entries = 128;
    rig->region.send_numbers = rig->send_numbers;
    rig->region.send_first_sends = rig->send_first_sends;
    rig->region.sent_packets = rig->sent_packets;
    rig->region.sent_index = rig->sent_index;
    rig->region.sent_refs = rig->sent_refs;
    rig->region.resend_fifo = rig->resend_fifo;
    rig->region.received_bits = rig->received_bits;
    rig->region.receive_descriptors = rig->receive_descriptors;
    rig->region.counters = &rig->counters;
    for ( uint32_t i = 0; i < 8; i++ )
    {
        rig->resend_fifo[i].prev = RN_RESEND_NONE;
        rig->resend_fifo[i].next = RN_RESEND_NONE;
    }
    rn_window_send_init( &rig->send, &rig->region, 0 );
    rn_window_ack_init( &rig->ack, &rig->send, &rig->region, NULL );
    rn_window_receive_init( &rig->receive, &rig->region, 0 );
}

// RNWindow Witness 1 (MCRNWindowBrokenBlockUPrime.cfg, KeepBlockUPrime = FALSE):
// W = 2, block {1, 2}. Single 0 is numbered, sent, opened, delivered, released
// at EndUpdate, and acked (U = U' = 1). Block {1, 2} takes numbers 1 and 2;
// fragment 1 arrives, is accepted at slot 1, and stays held (live) waiting for
// fragment 2. When the ack for fragment 1 arrives, U advances to 2, but U'
// must stay at 1 (the block's first number). If U' moved to U = 2, single 3
// would be numbered (3 < 2 + 2) and collide with held fragment 1 at slot 3 % 2 = 1.
static void test_channels_window_witness_u_prime_holds_at_a_partial_block()
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    channels_window_witness_rig_t rig;
    channels_window_witness_setup( &rig, 2, 4 * F );

    // Step 1: number single 0, send in packet 1, receiver accepts and releases at EndUpdate, ack lands.
    uint64_t n0 = UINT64_MAX, p0 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    check( n0 == 0 && p0 == 0 );
    rn_window_ack_commit_packet( &rig.ack, 1, &n0, 1, 0.01 );

    rocketnet_message_record_t rec0 = channels_window_record( 0 );
    uint32_t slot0 = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &rec0, &slot0 ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( slot0 == 0 && rig.receive.R == 1 );
    rn_window_receive_release( &rig.receive, 0 ); // EndUpdate releases delivered 0
    check( !rig.receive_descriptors[0].live );

    check( rn_window_ack_on_ack( &rig.ack, 1, 0.02 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 1 );

    // Step 2: number 2-fragment block {1, 2}; send fragment 1 in packet 2;
    // receiver accepts fragment 1 at slot 1 and holds it (live = 1) because 2 is missing;
    // ack for packet 2 lands at sender.
    uint64_t n1 = UINT64_MAX, p1 = UINT64_MAX;
    check( rn_window_send_first_write_block( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F + 16, RN_DESCRIPTOR_NONE, &n1, &p1 ) == 2 );
    check( n1 == 1 && rig.send.next == 3 );
    const uint64_t frag1 = 1;
    rn_window_ack_commit_packet( &rig.ack, 2, &frag1, 1, 0.03 );

    rocketnet_message_record_t rec1 = channels_window_record( 1 );
    uint32_t slot1 = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &rec1, &slot1 ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( slot1 == 1 && rig.receive.R == 2 );
    check( rig.receive_descriptors[1].live == 1 && rig.receive_descriptors[1].number == 1 );

    check( rn_window_ack_on_ack( &rig.ack, 2, 0.04 ) == 1 );

    // Step 3: sender tries to first-write single 3 while fragment 2 is still unacked.
    uint64_t n3 = UINT64_MAX, p3 = UINT64_MAX;
    const int step3 = rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n3, &p3 );
    if ( step3 == RN_WINDOW_SEND_NUMBERED )
    {
        rocketnet_message_record_t rec3 = channels_window_record( (uint32_t) n3 );
        uint32_t slot3 = RN_DESCRIPTOR_NONE;
        rn_window_receive_accept( &rig.receive, &rec3, &slot3 );
        check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    }
    check( step3 == RN_WINDOW_SEND_STALL );
    check( rig.send.U == 2 && rig.send.U_prime == 1 );

    // Once fragment 2 arrives, completes the block, is released and acked, 3 is admitted cleanly.
    const uint64_t frag2 = 2;
    rn_window_ack_commit_packet( &rig.ack, 3, &frag2, 1, 0.05 );
    rocketnet_message_record_t rec2 = channels_window_record( 2 );
    uint32_t slot2 = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &rec2, &slot2 ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( slot2 == 0 && rig.receive.R == 3 );
    rn_window_receive_release( &rig.receive, 1 );
    rn_window_receive_release( &rig.receive, 2 );
    check( rn_window_ack_on_ack( &rig.ack, 3, 0.06 ) == 1 );
    check( rig.send.U == 3 && rig.send.U_prime == 3 );

    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n3, &p3 ) == RN_WINDOW_SEND_NUMBERED );
    check( n3 == 3 );
    rocketnet_message_record_t rec3_ok = channels_window_record( 3 );
    uint32_t slot3_ok = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &rec3_ok, &slot3_ok ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( slot3_ok == 1 && rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rn_window_receive_release( &rig.receive, 3 );
}

// RNWindow Witness 2 (MCRNWindowBrokenSameUpdateReply.cfg, RequireFixBatch = FALSE):
// W = 2, single 0 and block {1, 2}. In update j, single 0 is opened and
// delivered, so receive_descriptors[0] stays live for the duration of update j
// until EndUpdate releases it. By P6, the ack for 0's packet and the sender's
// reply carrying fragment 2 (slot 2 % 2 = 0) cannot be opened by the receiver
// until update j + 1, after EndUpdate has released 0. If the transport stand-in
// drops P6 (keep_p6 = 0) and opens the reply in update j before EndUpdate,
// record 2 collides with still-live record 0 at slot 0 (NoSlotCollision).
static void test_channels_window_witness_reply_opens_next_update()
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    channels_window_witness_rig_t rig;
    channels_window_witness_setup( &rig, 2, 4 * F );
    const int keep_p6 = 1; // P6 (CHANNELS 2, RNWindow RequireFixBatch): reply opens in update j+1 after EndUpdate

    // Sender numbers single 0 and sends it in packet 1.
    uint64_t n0 = UINT64_MAX, p0 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, RN_DESCRIPTOR_NONE, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    check( n0 == 0 );
    rn_window_ack_commit_packet( &rig.ack, 1, &n0, 1, 0.01 );

    // Receiver update j: open packet 1 (record 0 is delivered, so slot 0 is live until EndUpdate).
    rocketnet_message_record_t rec0 = channels_window_record( 0 );
    uint32_t slot0 = RN_DESCRIPTOR_NONE;
    check( rn_window_receive_accept( &rig.receive, &rec0, &slot0 ) == RN_WINDOW_RECEIVE_ACCEPT );
    check( slot0 == 0 && rig.receive_descriptors[0].live == 1 && rig.receive_descriptors[0].number == 0 );

    if ( keep_p6 )
    {
        // EndUpdate of update j runs before any reply to update j's ack can be opened in update j+1.
        rn_window_receive_release( &rig.receive, 0 );
    }

    // Sender receives the ack for packet 1, numbers block {1, 2}, and sends fragment 2 in packet 2.
    check( rn_window_ack_on_ack( &rig.ack, 1, 0.02 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 1 );
    uint64_t n1 = UINT64_MAX, p1 = UINT64_MAX;
    check( rn_window_send_first_write_block( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F + 16, RN_DESCRIPTOR_NONE, &n1, &p1 ) == 2 );
    check( n1 == 1 && rig.send.next == 3 );
    const uint64_t frag2 = 2;
    rn_window_ack_commit_packet( &rig.ack, 2, &frag2, 1, 0.03 );

    // Receiver opens the reply carrying fragment 2 (slot 2 % 2 = 0).
    rocketnet_message_record_t rec2 = channels_window_record( 2 );
    uint32_t slot2 = RN_DESCRIPTOR_NONE;
    const int answer2 = rn_window_receive_accept( &rig.receive, &rec2, &slot2 );
    check( rig.receive.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    check( answer2 == RN_WINDOW_RECEIVE_ACCEPT && slot2 == 0 );

    if ( !keep_p6 )
        rn_window_receive_release( &rig.receive, 0 );
    rn_window_receive_release( &rig.receive, 2 );
}

// RNRing Witness 1 (MCRNRingBrokenFloorAtU.cfg, FloorAtUPrime = FALSE):
// B = 4*F, W = 8. 2-fragment block {0, 1} takes [0, 2*F), single 2 takes
// [2*F, 3*F), single 3 takes [3*F, 4*F), so ring_next = 4*F = B. Receiver
// opens fragment 0, whose span [0, F) stays live in the ring while fragment 1
// is missing. Sender receives Ack(0), so U = 1 (ring_start = F) and U' = 0
// (ring_start = 0). When single 4 (16 bytes) wants a span at p = 4*F, the
// floor at ring_start(U') = 0 stalls it (4*F + 16 - 0 > 4*F). If the floor
// used ring_start(U) = F instead, single 4 would be placed at p = 4*F (mod B = 0)
// and overwrite live fragment 0's bytes [0, 16) (LiveSpansDisjoint).
static void test_channels_window_witness_floor_is_u_prime()
{
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t B = 4 * F;
    channels_window_witness_rig_t rig;
    channels_window_witness_setup( &rig, 8, B );
    uint8_t live[4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    memset( live, 0, sizeof( live ) );

    // Steps 1-6: PlaceBlock {0, 1} at [0, 2*F), PlaceSingle 2 at [2*F, 3*F), PlaceSingle 3 at [3*F, 4*F).
    uint64_t n0 = UINT64_MAX, p0 = UINT64_MAX;
    check( rn_window_send_first_write_block( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2 * F, 0, &n0, &p0 ) == 2 );
    check( n0 == 0 && p0 == 0 && rig.send.ring_next == 2 * F );

    uint64_t n2 = UINT64_MAX, p2 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F, 0, &n2, &p2 ) == RN_WINDOW_SEND_NUMBERED );
    check( n2 == 2 && p2 == 2 * F && rig.send.ring_next == 3 * F );

    uint64_t n3 = UINT64_MAX, p3 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, F, 0, &n3, &p3 ) == RN_WINDOW_SEND_NUMBERED );
    check( n3 == 3 && p3 == 3 * F && rig.send.ring_next == B );

    // Step 7: Open(0) at the receiver — fragment 0 claims ring span [0, F) and stays live waiting for fragment 1.
    for ( uint32_t j = 0; j < F; j++ )
        live[j] = 1;

    // Step 8: Ack(0) at the sender — U = 1 (ring_start = F), U' = 0 (ring_start = 0).
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 0 );

    // Step 9: WantSingle(16) / PlaceSingle for record 4.
    uint64_t n4 = UINT64_MAX, p4 = UINT64_MAX;
    const int step9 = rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, 0, &n4, &p4 );
    if ( step9 == RN_WINDOW_SEND_NUMBERED )
    {
        const uint32_t start4 = (uint32_t) ( p4 % B );
        for ( uint32_t j = start4; j < start4 + 16; j++ )
            check( live[j] == 0 ); // LiveSpansDisjoint
    }
    check( step9 == RN_WINDOW_SEND_STALL );

    // Once fragment 1 arrives, block {0, 1} releases [0, 2*F) and Ack(1) advances U' = 2, admitting 4 cleanly.
    for ( uint32_t j = 0; j < F; j++ )
        live[j] = 0;
    check( rn_window_send_mark_acked( &rig.send, 1 ) == 1 );
    check( rig.send.U == 2 && rig.send.U_prime == 2 );
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, 0, &n4, &p4 ) == RN_WINDOW_SEND_NUMBERED );
    check( n4 == 4 && p4 == B );
    const uint32_t start4_ok = (uint32_t) ( p4 % B );
    check( start4_ok == 0 );
    for ( uint32_t j = start4_ok; j < start4_ok + 16; j++ )
        check( live[j] == 0 );
}

// RNRing Witness 2 (MCRNRingBrokenNoPadding.cfg, PadAtBoundary = FALSE):
// B = 64, W = 4. Single 0 of 32 bytes is placed at [0, 32), opened, released,
// and acked (U = U' = 1, ring_next = 32). Single 1 of 48 bytes then asks for a
// span: [32, 80) would cross the ring boundary B = 64, so padding advances p
// to 64 and places single 1 at [64, 112), which modulo B = 64 is [0, 48) inside
// the ring (SpanInsideRing). Without boundary padding, p stays 32 and [32, 80)
// overhangs B = 64, failing SpanInsideRing (rn_region_ring_span_valid).
static void test_channels_window_witness_padding_at_the_boundary()
{
    const uint32_t B = 64;
    channels_window_witness_rig_t rig;
    channels_window_witness_setup( &rig, 4, B );

    // Steps 1-4: WantSingle(32) -> PlaceSingle(0) at [0, 32) -> Open(0) -> Release(0) -> Ack(0).
    uint64_t n0 = UINT64_MAX, p0 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 32, 0, &n0, &p0 ) == RN_WINDOW_SEND_NUMBERED );
    check( n0 == 0 && p0 == 0 && rig.send.ring_next == 32 );
    check( rn_region_ring_span_valid( &rig.region, (uint32_t) ( p0 % B ), 32 ) == 1 );
    check( rn_window_send_mark_acked( &rig.send, 0 ) == 1 );
    check( rig.send.U == 1 && rig.send.U_prime == 1 );

    // Steps 5-6: WantSingle(48) -> PlaceSingle(1) -> Open(1) checks SpanInsideRing.
    uint64_t n1 = UINT64_MAX, p1 = UINT64_MAX;
    check( rn_window_send_first_write( &rig.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 48, 0, &n1, &p1 ) == RN_WINDOW_SEND_NUMBERED );
    const uint32_t ring_offset = (uint32_t) ( p1 % B );
    check( rn_region_ring_span_valid( &rig.region, ring_offset, 48 ) == 1 ); // SpanInsideRing
    check( p1 == 64 && ring_offset == 0 && rig.send.ring_next == 112 );
}

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
    { "test_channels_single_form_parses_as_one_code0_record", "ChannelsSingleFormParsesAsOneCode0Record", test_channels_single_form_parses_as_one_code0_record },
    { "test_channels_window_send_numbers_late_never_at_send", "ChannelsWindowSendNumbersLateNeverAtSend", test_channels_window_send_numbers_late_never_at_send },
    { "test_channels_window_send_count_bound_stalls_at_u_prime_plus_w", "ChannelsWindowSendCountBoundStallsAtUPrimePlusW", test_channels_window_send_count_bound_stalls_at_u_prime_plus_w },
    { "test_channels_window_send_ring_pads_at_the_boundary", "ChannelsWindowSendRingPadsAtTheBoundary", test_channels_window_send_ring_pads_at_the_boundary },
    { "test_channels_window_send_floor_is_u_prime_not_u", "ChannelsWindowSendFloorIsUPrimeNotU", test_channels_window_send_floor_is_u_prime_not_u },
    { "test_channels_window_send_block_takes_consecutive_numbers_and_one_span", "ChannelsWindowSendBlockTakesConsecutiveNumbersAndOneSpan", test_channels_window_send_block_takes_consecutive_numbers_and_one_span },
    { "test_channels_window_send_stall_lets_unreliable_through", "ChannelsWindowSendStallLetsUnreliableThrough", test_channels_window_send_stall_lets_unreliable_through },
    { "test_channels_window_send_any_message_fits_when_nothing_is_outstanding", "ChannelsWindowSendAnyMessageFitsWhenNothingIsOutstanding", test_channels_window_send_any_message_fits_when_nothing_is_outstanding },
    { "test_channels_window_receive_accepts_in_order", "ChannelsWindowReceiveAcceptsInOrder", test_channels_window_receive_accepts_in_order },
    { "test_channels_window_receive_duplicate_is_counted_not_violation", "ChannelsWindowReceiveDuplicateIsCountedNotViolation", test_channels_window_receive_duplicate_is_counted_not_violation },
    { "test_channels_window_receive_outside_window_is_violation", "ChannelsWindowReceiveOutsideWindowIsViolation", test_channels_window_receive_outside_window_is_violation },
    { "test_channels_window_receive_slot_collision_is_violation", "ChannelsWindowReceiveSlotCollisionIsViolation", test_channels_window_receive_slot_collision_is_violation },
    { "test_channels_window_receive_reconstructs_across_the_24_bit_wrap", "ChannelsWindowReceiveReconstructsAcrossThe24BitWrap", test_channels_window_receive_reconstructs_across_the_24_bit_wrap },
    { "test_channels_window_receive_advance_clears_only_what_it_passes", "ChannelsWindowReceiveAdvanceClearsOnlyWhatItPasses", test_channels_window_receive_advance_clears_only_what_it_passes },
    { "test_channels_window_receive_over_the_lossy_simulator", "ChannelsWindowReceiveOverTheLossySimulator", test_channels_window_receive_over_the_lossy_simulator },
    { "test_channels_window_send_and_receive_over_the_lossy_simulator", "ChannelsWindowSendAndReceiveOverTheLossySimulator", test_channels_window_send_and_receive_over_the_lossy_simulator },
    { "test_channels_window_ack_of_an_unknown_id_is_ignored", "ChannelsWindowAckOfAnUnknownIdIsIgnored", test_channels_window_ack_of_an_unknown_id_is_ignored },
    { "test_channels_window_ack_frees_the_chunk_and_advances_u", "ChannelsWindowAckFreesTheChunkAndAdvancesU", test_channels_window_ack_frees_the_chunk_and_advances_u },
    { "test_channels_window_rtt_sample_is_unambiguous_across_resends", "ChannelsWindowRttSampleIsUnambiguousAcrossResends", test_channels_window_rtt_sample_is_unambiguous_across_resends },
    { "test_channels_window_eviction_keeps_numbers_resendable", "ChannelsWindowEvictionKeepsNumbersResendable", test_channels_window_eviction_keeps_numbers_resendable },
    { "test_channels_window_resend_delay_never_backs_off", "ChannelsWindowResendDelayNeverBacksOff", test_channels_window_resend_delay_never_backs_off },
    { "test_channels_window_resend_walk_stops_at_the_first_not_due", "ChannelsWindowResendWalkStopsAtTheFirstNotDue", test_channels_window_resend_walk_stops_at_the_first_not_due },
    { "test_channels_window_failed_send_marks_due_now", "ChannelsWindowFailedSendMarksDueNow", test_channels_window_failed_send_marks_due_now },
    { "test_channels_window_reliable_timeout_enters_the_error_state", "ChannelsWindowReliableTimeoutEntersTheErrorState", test_channels_window_reliable_timeout_enters_the_error_state },
    { "test_channels_window_lossy_soak", "ChannelsWindowLossySoak", test_channels_window_lossy_soak },
    { "test_channels_window_witness_u_prime_holds_at_a_partial_block", "ChannelsWindowWitnessUPrimeHoldsAtAPartialBlock", test_channels_window_witness_u_prime_holds_at_a_partial_block },
    { "test_channels_window_witness_reply_opens_next_update", "ChannelsWindowWitnessReplyOpensNextUpdate", test_channels_window_witness_reply_opens_next_update },
    { "test_channels_window_witness_floor_is_u_prime", "ChannelsWindowWitnessFloorIsUPrime", test_channels_window_witness_floor_is_u_prime },
    { "test_channels_window_witness_padding_at_the_boundary", "ChannelsWindowWitnessPaddingAtTheBoundary", test_channels_window_witness_padding_at_the_boundary },
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

// Allocator cases replace this process's allocation functions. The hooks go
// to rocketnet_init, which takes the allocator. rocketnet_unit_prepare then installs the suite's log
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
        return strncmp( channels_tests[i].name, "test_channels_allocator_", 24 ) == 0 ||
            strcmp( channels_tests[i].name, "test_channels_window_send_and_receive_over_the_lossy_simulator" ) == 0 ||
            strcmp( channels_tests[i].name, "test_channels_window_lossy_soak" ) == 0;
    }
    return 0;
}

void rocketnet_channels_prepare( const char * name, void * (**malloc_function)( void * context, size_t bytes ), void (**free_function)( void * context, void * p ) )
{
    *malloc_function = NULL;
    *free_function = NULL;
    if ( !rocketnet_channels_is_allocator_test( name ) )
        return;
    *malloc_function = rn_test_malloc_hook;
    *free_function = rn_test_free_hook;
}

void rocketnet_channels_prepare_log( const char * name )
{
    if ( !rocketnet_channels_is_allocator_test( name ) )
        return;
    rocketnet_log_function( rn_test_log_function, NULL );
}
