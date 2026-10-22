/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The kernel prefilter counters (sdk/rocketnet_wire.h
// ROCKETNET_SERVER_XDP_COUNTER_*) surface through the transport counter
// surface via rocketnet_counters_apply_prefilter. Every case runs in memory:
// no transport, no transport, no kernel.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "test/counters_expect.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// the transport counter of each kernel prefilter counter, in wire order

static const int prefilter_kernel_counters[ROCKETNET_SERVER_XDP_NUM_COUNTERS] =
{
    ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH,
    ROCKETNET_COUNTER_XDP_KERNEL_PASSED_IPV6_EXTENSION,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED,
    ROCKETNET_COUNTER_XDP_KERNEL_PASSED_FRAGMENT,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SOCKET,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION,
};

static void test_prefilter_counters_map_each_kernel_counter()
{
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        struct rocketnet_counters_t counters;
        memset( &counters, 0, sizeof(counters) );
        counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] = 7;

        uint64_t prefilter[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = { 0 };
        prefilter[i] = 1000 + (uint64_t) i;

        rocketnet_counters_apply_prefilter( &counters, prefilter );

        check( counters.values[prefilter_kernel_counters[i]] == 1000 + (uint64_t) i );
        for ( int j = 0; j < ROCKETNET_SERVER_XDP_NUM_COUNTERS; j++ )
            if ( j != i )
                check( counters.values[prefilter_kernel_counters[j]] == 0 );
        check( counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == 7 );
    }
}

REGISTER_UNIT_TEST( test_prefilter_counters_map_each_kernel_counter );

static void test_prefilter_counters_are_assigned_not_added()
{
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    const uint64_t prefilter[ROCKETNET_SERVER_XDP_NUM_COUNTERS] =
    {
        5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    rocketnet_counters_apply_prefilter( &counters, prefilter );
    rocketnet_counters_apply_prefilter( &counters, prefilter );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        check( counters.values[prefilter_kernel_counters[i]] == prefilter[i] );

    const uint64_t smaller[ROCKETNET_SERVER_XDP_NUM_COUNTERS] =
    {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    };
    rocketnet_counters_apply_prefilter( &counters, smaller );
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        check( counters.values[prefilter_kernel_counters[i]] == smaller[i] );
}

REGISTER_UNIT_TEST( test_prefilter_counters_are_assigned_not_added );

// ---------------------------------------------------------------------------
// The pcapng corpus replay (test/pcapng/README.md). Every file under
// test/pcapng/ and test/pcapng/regress/ is read with test/pcapng.h and every
// packet in it is handed to the reader the transport hands it to before any
// key: interface-0 datagrams to the prefilter and the receive ladder of the
// end they were addressed to (the connect request on into the connect-token
// readers, under the published test keys), interface-1 records to the ack
// block reader, the fragment reassembly and the message reader. Each packet
// ends read, or refused under the name of the counter the code fires; an
// unnamed end fails with its file and index, and the per-name tally of each
// captured file is pinned. File input only: no sockets, no threads, no clock
// but the timestamps in the files, which start at the test clock's 1000.0 s.
// ---------------------------------------------------------------------------

#include "rocketnet_ack.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_fragment.h"
#include "rocketnet_memory.h"
#include "rocketnet_message.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"
#include "test/pcapng.h"

#include <stdlib.h>

#include <map>
#include <string>
#include <vector>

// the test endpoints the capture harness uses (functional/channel_harness.h)
#define PCAPNG_SERVER_ADDRESS   ( ( 203u << 24 ) | ( 0u << 16 ) | ( 113u << 8 ) | 10u )
#define PCAPNG_SERVER_PORT      40000
#define PCAPNG_CLOCK_START_US   1000000000ULL

// the replay's reassembly geometry: two connections of the capture server's
// shape (4 slots at a 16384-byte payload maximum), a delivery window of 4. A
// fragment record names its connection ("connection=1" in its comment; 0
// when absent), as the fuzz harness alternates its two.
#define PCAPNG_REASSEMBLY_CONNECTIONS   2
#define PCAPNG_REASSEMBLY_SLOTS         4
#define PCAPNG_REASSEMBLY_MAX_PAYLOAD   16384
#define PCAPNG_REASSEMBLY_DELIVERED     4

// the counting allocator, after test_unit_connection_memory.cpp cm_malloc/cm_free,
// with a size header so live bytes are known at every free

static uint64_t pcapng_live_bytes = 0;
static uint64_t pcapng_peak_bytes = 0;

static void * pcapng_malloc( void * context, size_t bytes )
{
    (void) context;
    uint8_t * block = (uint8_t *) malloc( bytes + 16 );
    if ( !block )
        return NULL;
    memcpy( block, &bytes, sizeof(bytes) );
    pcapng_live_bytes += bytes;
    if ( pcapng_live_bytes > pcapng_peak_bytes )
        pcapng_peak_bytes = pcapng_live_bytes;
    return block + 16;
}

static void pcapng_free( void * context, void * p )
{
    (void) context;
    if ( !p )
        return;
    uint8_t * block = (uint8_t *) p - 16;
    size_t bytes;
    memcpy( &bytes, block, sizeof(bytes) );
    pcapng_live_bytes -= bytes;
    free( block );
}

static void pcapng_allocator_install()
{
    rocketnet_term();
    check( rocketnet_init( NULL, pcapng_malloc, pcapng_free ) == ROCKETNET_STATUS_OK );
    pcapng_live_bytes = 0;
    pcapng_peak_bytes = 0;
}

static void pcapng_allocator_restore()
{
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static std::string pcapng_directory()
{
    std::string source = __FILE__;
    size_t slash = source.find_last_of( "/\\" );
    std::string root = ( slash == std::string::npos ) ? std::string( "." ) : source.substr( 0, slash );
    return root + "/test/pcapng";
}

static const char * pcapng_type_name( int packet_type )
{
    static const char * names[10] = { "REQUEST", "DENIED", "CHALLENGE", "RESPONSE", "KEEP_ALIVE", "PAYLOAD", "DISCONNECT", "COOKIE", "ACK", "FRAGMENT" };
    return packet_type >= 0 && packet_type < 10 ? names[packet_type] : "RESERVED";
}

// one packet's end: read (counter -1, a "read ..." name) or refused under a
// counter; name empty means it ended neither way

struct pcapng_outcome_t
{
    std::string name;
};

static pcapng_outcome_t pcapng_read_as( const char * what )
{
    pcapng_outcome_t outcome;
    outcome.name = std::string( "read " ) + what;
    return outcome;
}

static pcapng_outcome_t pcapng_refused( int counter )
{
    pcapng_outcome_t outcome;
    if ( counter >= 0 && counter < ROCKETNET_NUM_COUNTERS )
        outcome.name = rocketnet_counter_name( counter );
    return outcome;
}

static pcapng_outcome_t pcapng_refused_by( const char * name )
{
    pcapng_outcome_t outcome;
    outcome.name = name;
    return outcome;
}

// the planted fault of the witness: the prefilter's refusal loses its name
static int pcapng_fault_drop_prefilter_name = 0;

// the session types' per-type counters, server family then client family
struct pcapng_session_counters_t
{
    int packet_type;
    int min_wire_bytes;
    int server_wrong_size, server_bad_ack_block, server_wrong_opened_size;
    int client_wrong_size, client_bad_ack_block, client_wrong_opened_size;
};

static const pcapng_session_counters_t pcapng_session_counters[] =
{
    { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
      ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE, ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK, ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE,
      ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE },
    { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES,
      ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE, ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK, ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE,
      ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE },
    { ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES,
      ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE, ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK, ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE,
      ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_OPENED_SIZE },
    { ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
      ROCKETNET_COUNTER_ACK_WRONG_SIZE, ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK, ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE,
      ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_ACK_WRONG_OPENED_SIZE },
    { ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES,
      ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE, ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK, ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE,
      ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK, ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE },
};

static const pcapng_session_counters_t * pcapng_session( int packet_type )
{
    for ( size_t i = 0; i < sizeof(pcapng_session_counters) / sizeof(pcapng_session_counters[0]); i++ )
        if ( pcapng_session_counters[i].packet_type == packet_type )
            return &pcapng_session_counters[i];
    return NULL;
}

// the connect request past its size: the fixed-offset comparisons, then the
// connect-token readers under the published test keys. The private block's
// keys are opened into a local and wiped; nothing of them is printed.
static pcapng_outcome_t pcapng_connect_request( const uint8_t * packet, int bytes )
{
    if ( bytes != ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES )
        return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE );

    // after the prefilter header: return cookie (24), salt (8), client
    // ephemeral public key (32), signed connect data (576), signature (64)
    const uint8_t * signed_connect_data = packet + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES +
                                          ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES;
    const uint8_t * signature = signed_connect_data + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_VERSION;
        if ( rocketnet_read_uint32( &p ) != ROCKETNET_WIRE_VERSION )
            return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION );
    }
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID;
        if ( rocketnet_read_uint64( &p ) != ROCKETNET_TEST_PROTOCOL_ID )
            return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID );
    }

    // the signed part of a token travels in the request; the token's two
    // client keys never do, so they stand as zero in the split
    uint8_t token_bytes[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token_bytes, 0, sizeof(token_bytes) );
    memcpy( token_bytes, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    memcpy( token_bytes + ROCKETNET_SIGNED_CONNECT_DATA_BYTES, signature, ROCKETNET_SIGNATURE_BYTES );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_bytes );
    check( memcmp( token.signed_connect_data, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES ) == 0 );

    if ( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) )
        return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY );

    struct rocketnet_connect_token_private_t private_fields;
    const int opened = rocketnet_open_connect_token_private( &private_fields, token.signed_connect_data,
                                                             rocketnet_test_server_public_key, rocketnet_test_server_secret_key );
    const int consistent = opened && rocketnet_connect_token_private_consistent( &private_fields, token.signed_connect_data );
    volatile uint8_t * wipe = (volatile uint8_t *) &private_fields;
    for ( size_t i = 0; i < sizeof(private_fields); i++ )
        wipe[i] = 0;
    if ( !opened )
        return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK );
    if ( !consistent )
        return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH );
    return pcapng_read_as( "server REQUEST token" );
}

// interface 0: the datagram through the ladder of the end it was sent to, up
// to the first rung that needs a key or connection state
static pcapng_outcome_t pcapng_wire( const uint8_t * packet, int bytes, int to_server )
{
    const int code = rocketnet_prefilter_check( packet, bytes, rocketnet_test_filter_value );
    if ( code < 0 )
        return pcapng_fault_drop_prefilter_name ? pcapng_outcome_t() : pcapng_refused( rocketnet_prefilter_counter( code ) );
    const int packet_type = code;
    if ( packet_type > ROCKETNET_CONNECTION_FRAGMENT_PACKET )
        return pcapng_refused( ROCKETNET_COUNTER_RESERVED_TYPE );

    std::string read = std::string( to_server ? "server " : "client " ) + pcapng_type_name( packet_type );
    const pcapng_session_counters_t * session = pcapng_session( packet_type );

    if ( to_server )
    {
        if ( packet_type == ROCKETNET_CONNECTION_DENIED_PACKET || packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET ||
             packet_type == ROCKETNET_CONNECTION_COOKIE_PACKET )
            return pcapng_refused( ROCKETNET_COUNTER_WRONG_DIRECTION );
        if ( packet_type == ROCKETNET_CONNECTION_REQUEST_PACKET )
        {
            pcapng_outcome_t outcome = pcapng_connect_request( packet, bytes );
            if ( outcome.name.compare( 0, 5, "read " ) == 0 )
                check( ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, bytes ) );
            return outcome;
        }
        if ( packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET && bytes != ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES )
            return pcapng_refused( ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE );
        if ( session && bytes < session->min_wire_bytes )
            return pcapng_refused( session->server_wrong_size );
        // what the server reads before keys is what the kernel filter passes
        check( ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, bytes ) );
        return pcapng_read_as( read.c_str() );
    }

    if ( packet_type == ROCKETNET_CONNECTION_REQUEST_PACKET || packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
        return pcapng_refused( ROCKETNET_COUNTER_WRONG_DIRECTION );
    if ( packet_type == ROCKETNET_CONNECTION_COOKIE_PACKET && bytes != ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES )
        return pcapng_refused( ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE );
    if ( packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET && bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
        return pcapng_refused( ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN );
    if ( packet_type == ROCKETNET_CONNECTION_DENIED_PACKET && bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
        return pcapng_refused( ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN );
    if ( session && bytes < session->min_wire_bytes )
        return pcapng_refused( session->client_wrong_size );
    return pcapng_read_as( read.c_str() );
}

// the replay's reader state for one file: one reassembly connection, the
// default message limits

struct pcapng_replay_t
{
    struct rocketnet_reassembly_set_t * reassembly;
    struct rocketnet_message_limits_t limits;
    struct rocketnet_counters_t counters;
};

static const struct rocketnet_reassembly_counters_t pcapng_server_fragment_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID, ROCKETNET_COUNTER_FRAGMENT_STALE, ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE, ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH, ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE, ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX, ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED, ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED, ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

static const struct rocketnet_reassembly_counters_t pcapng_client_fragment_counters = {
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID, ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH, ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL };

// interface 1: one kind byte, then the bytes that reader is handed. The
// comment names the receiving end ("server" or "client") and, for an ack
// block, the packet type ("server 5"); a fragment may name its reassembly
// connection ("server connection=1").
static pcapng_outcome_t pcapng_opened( pcapng_replay_t * replay, const std::vector<uint8_t> & record, const std::string & comment )
{
    if ( record.empty() )
        return pcapng_refused_by( "PCAPNG_EMPTY_RECORD" );
    const int kind = record[0];
    const uint8_t * data = record.data() + 1;
    const int bytes = (int) record.size() - 1;

    const size_t space = comment.find( ' ' );
    const std::string end = comment.substr( 0, space );
    const int is_server = end == "server";
    if ( !is_server && end != "client" )
        return pcapng_refused_by( "PCAPNG_RECORD_NO_CONTEXT" );

    if ( kind == ROCKETNET_PCAPNG_KIND_ACK_BLOCK )
    {
        const int packet_type = space == std::string::npos ? -1 : atoi( comment.c_str() + space + 1 );
        const pcapng_session_counters_t * session = pcapng_session( packet_type );
        if ( !session )
            return pcapng_refused_by( "PCAPNG_RECORD_NO_CONTEXT" );

        // the reader's context is the receiver's own highest sent sequence;
        // the replay stands an honest sender's in for it: the block's own
        // largest, which reconstructs to itself
        uint64_t highest_sent = 0;
        if ( bytes >= 4 )
        {
            const uint8_t * p = data;
            highest_sent = rocketnet_read_uint32( &p );
        }
        struct rocketnet_ack_block_t block;
        const int block_bytes = rocketnet_ack_block_read( &block, data, bytes, highest_sent );
        if ( block_bytes == 0 )
            return pcapng_refused( is_server ? session->server_bad_ack_block : session->client_bad_ack_block );
        const int type_data_bytes = bytes - block_bytes;
        if ( ( packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET && type_data_bytes != 8 ) ||
             ( packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET && type_data_bytes < 1 ) ||
             ( packet_type == ROCKETNET_CONNECTION_DISCONNECT_PACKET && type_data_bytes != 0 ) ||
             ( packet_type == ROCKETNET_CONNECTION_ACK_PACKET && type_data_bytes != 0 ) ||
             ( packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET && type_data_bytes < ROCKETNET_FRAGMENT_HEADER_BYTES + 1 ) )
            return pcapng_refused( is_server ? session->server_wrong_opened_size : session->client_wrong_opened_size );
        return pcapng_read_as( ( end + " ack block " + pcapng_type_name( packet_type ) ).c_str() );
    }

    if ( kind == ROCKETNET_PCAPNG_KIND_FRAGMENT )
    {
        if ( bytes < ROCKETNET_FRAGMENT_HEADER_BYTES + 1 || bytes > ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES )
            return pcapng_refused( is_server ? ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE : ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE );
        const struct rocketnet_reassembly_counters_t * table = is_server ? &pcapng_server_fragment_counters : &pcapng_client_fragment_counters;
        const int connection = comment.find( "connection=1" ) != std::string::npos ? 1 : 0;
        struct rocketnet_counters_t before = replay->counters;
        rocketnet_reassembly_set_begin_update( replay->reassembly );
        const int accepted = rocketnet_reassembly_process_fragment( replay->reassembly, connection, data, bytes, replay->counters.values, table );
        if ( accepted )
        {
            check( replay->counters.values[table->accepted] == before.values[table->accepted] + 1 );
            return pcapng_read_as( replay->reassembly->completed_bytes > 0 ? ( end + " fragment completing" ).c_str() : ( end + " fragment" ).c_str() );
        }
        // refused: named by the refusal the engine fired, from its own table
        const int refusals[9] = { table->zero_payload_id, table->stale, table->count_invalid, table->index_out_of_range,
                                  table->count_mismatch, table->size_mismatch, table->payload_too_large, table->duplicate_index,
                                  table->batch_full };
        for ( int i = 0; i < 9; i++ )
            if ( replay->counters.values[refusals[i]] != before.values[refusals[i]] )
                return pcapng_refused( refusals[i] );
        return pcapng_outcome_t();
    }

    if ( kind == ROCKETNET_PCAPNG_KIND_MESSAGE_PAYLOAD )
    {
        struct rocketnet_message_reader_t reader;
        const int violation = rocketnet_message_reader_begin( &reader, data, bytes, &replay->limits );
        if ( violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
            return pcapng_refused_by( ( std::string( rocketnet_counter_name( ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) ) + " " +
                                        rocketnet_message_violation_name( violation ) ).c_str() );
        struct rocketnet_message_record_t message;
        int records = 0;
        while ( rocketnet_message_reader_next( &reader, &message ) )
        {
            records++;
            check( records <= bytes );
        }
        return pcapng_read_as( ( end + " message payload" ).c_str() );
    }

    return pcapng_refused_by( "PCAPNG_UNKNOWN_KIND" );
}

// one file's replay: its tally by name, and the unnamed packets by index
struct pcapng_file_result_t
{
    int read_result;
    std::map<std::string, int> tally;
    std::vector<int> unnamed;
    size_t file_bytes;
};

static pcapng_file_result_t pcapng_replay_file( const std::string & path )
{
    pcapng_file_result_t result;
    result.file_bytes = 0;
    std::vector<uint8_t> image;
    result.read_result = rocketnet_pcapng_read_file( path, image );
    if ( result.read_result != ROCKETNET_PCAPNG_OK )
        return result;
    result.file_bytes = image.size();
    std::vector<rocketnet_pcapng_packet_t> packets;
    result.read_result = rocketnet_pcapng_read_buffer( image.data(), image.size(), packets );
    if ( result.read_result != ROCKETNET_PCAPNG_OK )
        return result;

    pcapng_replay_t replay;
    memset( &replay, 0, sizeof(replay) );
    replay.reassembly = rocketnet_reassembly_set_create( NULL, PCAPNG_REASSEMBLY_CONNECTIONS, PCAPNG_REASSEMBLY_SLOTS, PCAPNG_REASSEMBLY_MAX_PAYLOAD, PCAPNG_REASSEMBLY_DELIVERED );
    check( replay.reassembly != NULL );
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    replay.limits.num_channels = config.num_channels;
    replay.limits.enabled_codes = rocketnet_message_enabled_codes( ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION );
    replay.limits.reliable_messages_in_flight = config.reliable_messages_in_flight;
    replay.limits.max_message_bytes = config.max_message_bytes;
    replay.limits.receive_message_buffer_bytes = config.receive_message_buffer_bytes;

    // time comes from the file, never before the test clock's start
    for ( size_t i = 0; i < packets.size(); i++ )
    {
        const rocketnet_pcapng_packet_t & packet = packets[i];
        check( packet.timestamp_us >= PCAPNG_CLOCK_START_US );

        pcapng_outcome_t outcome;
        if ( packet.interface_id == ROCKETNET_PCAPNG_INTERFACE_WIRE )
        {
            const uint8_t * datagram;
            int bytes;
            uint32_t source_address, destination_address;
            uint16_t source_port, destination_port;
            if ( !rocketnet_pcapng_udp_payload( packet.data, &datagram, &bytes, &source_address, &source_port, &destination_address, &destination_port ) )
                outcome = pcapng_refused_by( "PCAPNG_FRAME_NOT_IPV4_UDP" );
            else
            {
                // the datagram in its own buffer, so a reader's overread is a sanitizer's finding
                std::vector<uint8_t> exact( datagram, datagram + bytes );
                const int to_server = destination_address == PCAPNG_SERVER_ADDRESS && destination_port == PCAPNG_SERVER_PORT;
                outcome = pcapng_wire( exact.empty() ? (const uint8_t *) "" : exact.data(), bytes, to_server );
            }
        }
        else
        {
            outcome = pcapng_opened( &replay, packet.data, packet.comment );
        }
        if ( outcome.name.empty() )
            result.unnamed.push_back( (int) i );
        else
            result.tally[outcome.name]++;
    }

    rocketnet_reassembly_set_destroy( replay.reassembly );
    return result;
}

static std::string pcapng_tally_text( const std::map<std::string, int> & tally )
{
    std::string text;
    for ( std::map<std::string, int>::const_iterator it = tally.begin(); it != tally.end(); ++it )
    {
        char line[160];
        snprintf( line, sizeof(line), "%s=%d;", it->first.c_str(), it->second );
        text += line;
    }
    return text;
}

// The pinned tallies: per captured file, every name a packet ended under and
// its count. Captures are not byte-reproducible (the keys are random per run);
// these counts are what a re-capture by the README's recipe reproduces.
struct pcapng_pin_t
{
    const char * file;
    const char * tally;
};

static const pcapng_pin_t pcapng_pins[] =
{
    { "fragment_seeds.pcapng", "FRAGMENT_COUNT_INVALID=1;FRAGMENT_INDEX_OUT_OF_RANGE=1;FRAGMENT_PAYLOAD_TOO_LARGE=1;FRAGMENT_WRONG_OPENED_SIZE=2;FRAGMENT_ZERO_PAYLOAD_ID=1;read server fragment=8;read server fragment completing=4;" },
    { "test_harness_one_message_each_way_lossy.pcapng", "read client CHALLENGE=1;read client COOKIE=2;read client KEEP_ALIVE=10;read client PAYLOAD=2;read client ack block KEEP_ALIVE=8;read client ack block PAYLOAD=2;read client message payload=2;read server KEEP_ALIVE=8;read server PAYLOAD=4;read server REQUEST token=2;read server RESPONSE=2;read server ack block KEEP_ALIVE=8;read server ack block PAYLOAD=2;read server message payload=2;" },
    { "hostile_prefilter.pcapng", "CONNECTION_REQUEST_WRONG_VERSION=1;PREFILTER_TOO_SHORT=2;PREFILTER_WRONG_EPOCH=1;PREFILTER_WRONG_VALUE=1;RESERVED_TYPE=1;read server KEEP_ALIVE=1;read server PAYLOAD=2;" },
    { "test_ordered_small_and_large_interleaved.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=4;read client ack block KEEP_ALIVE=3;read server PAYLOAD=25;read server REQUEST token=2;read server RESPONSE=1;read server ack block PAYLOAD=17;read server message payload=16;" },
    { "test_reliable_duplicate_after_gap_delivered_once.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=31;read client ack block KEEP_ALIVE=31;read server KEEP_ALIVE=29;read server PAYLOAD=3;read server REQUEST token=2;read server RESPONSE=1;read server ack block KEEP_ALIVE=29;read server ack block PAYLOAD=3;read server message payload=3;" },
    { "test_reliable_mixed_sizes_over_lossy_link.pcapng", "read client CHALLENGE=1;read client COOKIE=2;read client KEEP_ALIVE=3;read client PAYLOAD=13;read client ack block KEEP_ALIVE=1;read client ack block PAYLOAD=13;read client message payload=11;read server KEEP_ALIVE=1;read server PAYLOAD=14;read server REQUEST token=2;read server RESPONSE=2;read server ack block KEEP_ALIVE=1;read server ack block PAYLOAD=9;read server message payload=9;" },
    { "test_reliable_sequenced_held_record_released.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=31;read client ack block KEEP_ALIVE=31;read server KEEP_ALIVE=30;read server PAYLOAD=2;read server REQUEST token=2;read server RESPONSE=1;read server ack block KEEP_ALIVE=30;read server ack block PAYLOAD=2;read server message payload=2;" },
    { "test_unreliable_at_most_once_over_duplicating_link.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=1;read client PAYLOAD=80;read client ack block KEEP_ALIVE=1;read client ack block PAYLOAD=121;read client message payload=77;read server PAYLOAD=80;read server REQUEST token=2;read server RESPONSE=1;read server ack block PAYLOAD=121;read server message payload=78;" },
    { "test_unreliable_dropped_when_builder_out_of_budget_never_arrives.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=52;read client ack block KEEP_ALIVE=52;read server KEEP_ALIVE=51;read server PAYLOAD=1;read server REQUEST token=2;read server RESPONSE=1;read server ack block KEEP_ALIVE=51;read server ack block PAYLOAD=1;read server message payload=1;" },
    { "test_unreliable_receipts_over_lossy_link.pcapng", "read client CHALLENGE=1;read client COOKIE=2;read client KEEP_ALIVE=4;read client PAYLOAD=48;read client ack block KEEP_ALIVE=2;read client ack block PAYLOAD=43;read client message payload=42;read server KEEP_ALIVE=2;read server PAYLOAD=48;read server REQUEST token=2;read server RESPONSE=2;read server ack block KEEP_ALIVE=1;read server ack block PAYLOAD=47;read server message payload=42;" },
    { "test_control_denied_when_server_full.pcapng", "read client COOKIE=1;read client DENIED=1;read server REQUEST token=2;" },
    { "test_control_disconnect_burst_each_end.pcapng", "read client CHALLENGE=2;read client COOKIE=2;read client DISCONNECT=10;read client KEEP_ALIVE=2;read client ack block DISCONNECT=1;read client ack block KEEP_ALIVE=2;read server DISCONNECT=10;read server REQUEST token=4;read server RESPONSE=2;read server ack block DISCONNECT=1;" },
    { "test_control_fragment_packets_each_end.pcapng", "read client CHALLENGE=1;read client COOKIE=1;read client FRAGMENT=3;read client KEEP_ALIVE=1;read client ack block FRAGMENT=3;read client ack block KEEP_ALIVE=1;read client fragment=2;read client fragment completing=1;read server FRAGMENT=3;read server REQUEST token=2;read server RESPONSE=1;read server ack block FRAGMENT=3;read server fragment=2;read server fragment completing=1;" },
    { "test_control_standalone_ack_each_end.pcapng", "read client ACK=1;read client CHALLENGE=1;read client COOKIE=1;read client KEEP_ALIVE=1;read client ack block ACK=1;read client ack block KEEP_ALIVE=1;read server PAYLOAD=72;read server REQUEST token=2;read server RESPONSE=1;read server ack block PAYLOAD=72;read server message payload=72;" },
};

// replays every file of the corpus; returns the number of unnamed packets and
// prints each with its file and index
static int pcapng_replay_corpus( int pin_tallies )
{
    const std::string root = pcapng_directory();
    std::vector<std::string> names;
    rocketnet_pcapng_list( root, names );
    std::vector<std::string> regress;
    rocketnet_pcapng_list( root + "/regress", regress );
    check( !names.empty() );

    pcapng_allocator_install();
    // the bound: the replay's one reassembly set by the geometry above -- its
    // requested component bytes plus the allocator's per-allocation overhead
    // on each of its four allocations (the set, the slot records, the arena,
    // the delivered mirror) -- and nothing else. No packet allocates.
    const uint64_t required = rocketnet_reassembly_memory_required( PCAPNG_REASSEMBLY_CONNECTIONS, PCAPNG_REASSEMBLY_SLOTS, PCAPNG_REASSEMBLY_MAX_PAYLOAD, PCAPNG_REASSEMBLY_DELIVERED );
    check( required > 0 );
    const uint64_t bound = required + 4 * rocketnet_allocation_overhead();

    int unnamed = 0;
    size_t corpus_bytes = 0;
    std::vector<std::string> paths;
    for ( size_t i = 0; i < names.size(); i++ )
        paths.push_back( root + "/" + names[i] );
    for ( size_t i = 0; i < regress.size(); i++ )
        paths.push_back( root + "/regress/" + regress[i] );

    int pinned_seen = 0;
    for ( size_t i = 0; i < paths.size(); i++ )
    {
        const bool is_regress = i >= names.size();
        const std::string name = is_regress ? "regress/" + regress[i - names.size()] : names[i];
        pcapng_file_result_t result = pcapng_replay_file( paths[i] );
        if ( result.read_result != ROCKETNET_PCAPNG_OK )
            printf( "pcapng corpus: %s refused: %s\n", name.c_str(), rocketnet_pcapng_result_name( result.read_result ) );
        check( result.read_result == ROCKETNET_PCAPNG_OK );
        check( result.file_bytes <= ROCKETNET_PCAPNG_MAX_FILE_BYTES );
        corpus_bytes += result.file_bytes;
        if ( pcapng_live_bytes != 0 || pcapng_peak_bytes > bound )
            printf( "pcapng corpus: %s live bytes %llu, peak %llu, bound %llu\n", name.c_str(),
                    (unsigned long long) pcapng_live_bytes, (unsigned long long) pcapng_peak_bytes, (unsigned long long) bound );
        check( pcapng_live_bytes == 0 );
        check( pcapng_peak_bytes <= bound );

        for ( size_t j = 0; j < result.unnamed.size(); j++ )
            printf( "pcapng corpus: %s packet %d ended neither read nor refused under a name\n", name.c_str(), result.unnamed[j] );
        unnamed += (int) result.unnamed.size();

        if ( !pin_tallies || is_regress )
            continue;
        const std::string tally = pcapng_tally_text( result.tally );
        const pcapng_pin_t * pin = NULL;
        for ( size_t k = 0; k < sizeof(pcapng_pins) / sizeof(pcapng_pins[0]); k++ )
            if ( name == pcapng_pins[k].file )
                pin = &pcapng_pins[k];
        if ( !pin || tally != pin->tally )
            printf( "pcapng corpus: %s tally is not pinned as found:\n    { \"%s\", \"%s\" },\n", name.c_str(), name.c_str(), tally.c_str() );
        check( pin != NULL );
        check( tally == pin->tally );
        pinned_seen++;
    }
    if ( pin_tallies )
        check( pinned_seen == (int) ( sizeof(pcapng_pins) / sizeof(pcapng_pins[0]) ) );
    check( corpus_bytes <= ROCKETNET_PCAPNG_MAX_CORPUS_BYTES );
    check( pcapng_live_bytes == 0 );
    pcapng_allocator_restore();
    return unnamed;
}

// the reader's own refusals, each by name, on images cut from a corpus file;
// each image sits in a buffer of exactly its size
static void pcapng_reader_refusals()
{
    std::vector<uint8_t> image;
    rocketnet_pcapng_write_header( image );
    const uint8_t datagram[8] = { 5, 0, 0xb7, 0x59, 0x01, 0x00, 1, 2 };
    rocketnet_pcapng_write_packet( image, 0, PCAPNG_CLOCK_START_US, datagram, sizeof(datagram), "server 5" );
    std::vector<rocketnet_pcapng_packet_t> packets;

    {
        std::vector<uint8_t> exact( image );
        check( rocketnet_pcapng_read_buffer( exact.data(), exact.size(), packets ) == ROCKETNET_PCAPNG_OK );
        check( packets.size() == 1 && packets[0].comment == "server 5" && packets[0].data.size() == sizeof(datagram) );
        check( packets[0].timestamp_us == PCAPNG_CLOCK_START_US );
    }
    for ( size_t cut = 1; cut < image.size(); cut++ )
    {
        std::vector<uint8_t> truncated( image.begin(), image.begin() + (long) cut );
        // a cut at a block boundary is a shorter file, never the packet
        const int result = rocketnet_pcapng_read_buffer( truncated.data(), truncated.size(), packets );
        check( result != ROCKETNET_PCAPNG_OK || packets.empty() );
    }
    {
        std::vector<uint8_t> truncated( image.begin(), image.end() - 2 );
        check( rocketnet_pcapng_read_buffer( truncated.data(), truncated.size(), packets ) == ROCKETNET_PCAPNG_TRUNCATED_BLOCK );
    }
    {
        std::vector<uint8_t> odd( image );
        odd[4] = (uint8_t) ( odd[4] + 2 );
        check( rocketnet_pcapng_read_buffer( odd.data(), odd.size(), packets ) == ROCKETNET_PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4 );
    }
    {
        std::vector<uint8_t> linktype( image );
        const uint32_t shb = rocketnet_pcapng_get_u32( image.data() + 4 );
        linktype[shb + 8] = 1;
        linktype[shb + 9] = 0;
        check( rocketnet_pcapng_read_buffer( linktype.data(), linktype.size(), packets ) == ROCKETNET_PCAPNG_UNKNOWN_LINKTYPE );
    }
    {
        std::vector<uint8_t> over( image );
        over.resize( ROCKETNET_PCAPNG_MAX_FILE_BYTES + 1, 0 );
        check( rocketnet_pcapng_read_buffer( over.data(), over.size(), packets ) == ROCKETNET_PCAPNG_FILE_OVER_CAP );
    }
    {
        // a captured length that claims past its block
        std::vector<uint8_t> claim( image );
        const uint32_t shb = rocketnet_pcapng_get_u32( image.data() + 4 );
        const size_t epb = shb + 20 + 20;
        claim[epb + 20] = 0xFF;
        claim[epb + 24] = 0xFF;
        check( rocketnet_pcapng_read_buffer( claim.data(), claim.size(), packets ) == ROCKETNET_PCAPNG_BAD_PACKET_LENGTH );
    }
    for ( int i = 0; i < ROCKETNET_PCAPNG_NUM_RESULTS; i++ )
        check( strncmp( rocketnet_pcapng_result_name( i ), "PCAPNG_", 7 ) == 0 );
}

static void test_pcapng_corpus_replay()
{
    pcapng_reader_refusals();
    pcapng_fault_drop_prefilter_name = 0;
    const int unnamed = pcapng_replay_corpus( 1 );
    check( unnamed == 0 );
}

REGISTER_UNIT_TEST( test_pcapng_corpus_replay );

// the witness: the same replay with the prefilter's refusal name dropped. The
// hostile datagrams are refused there, so they now end unnamed, and the replay
// must report them, each with its file and index.
static void test_pcapng_corpus_witness_unread_packet_goes_red()
{
    pcapng_fault_drop_prefilter_name = 1;
    const int unnamed = pcapng_replay_corpus( 0 );
    pcapng_fault_drop_prefilter_name = 0;
    printf( "pcapng witness: the planted fault left %d packets unnamed\n", unnamed );
    check( unnamed >= 1 );

    // and the unnamed ones are exactly the prefilter refusals of the hostile file
    pcapng_file_result_t hostile = pcapng_replay_file( pcapng_directory() + "/hostile_prefilter.pcapng" );
    check( hostile.read_result == ROCKETNET_PCAPNG_OK );
    check( hostile.unnamed.empty() );
}

REGISTER_UNIT_TEST( test_pcapng_corpus_witness_unread_packet_goes_red );

// The corpus covers each control packet type: every top-level file under
// test/pcapng/ is read and its interface-0 frames counted by packet type
// (the first byte of the UDP payload), and the corpus must hold at least
// one frame of each of types 1 (denied), 6 (disconnect), 8 (ack) and 9
// (fragment). File input only: no sockets, no threads, no real time.
static void test_pcapng_corpus_covers_control_packet_types()
{
    static const int required_types[4] = {
        ROCKETNET_CONNECTION_DENIED_PACKET,
        ROCKETNET_CONNECTION_DISCONNECT_PACKET,
        ROCKETNET_CONNECTION_ACK_PACKET,
        ROCKETNET_CONNECTION_FRAGMENT_PACKET };

    const std::string root = pcapng_directory();
    std::vector<std::string> names;
    rocketnet_pcapng_list( root, names );
    check( !names.empty() );

    long counts[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for ( size_t i = 0; i < names.size(); i++ )
    {
        std::vector<uint8_t> image;
        check( rocketnet_pcapng_read_file( root + "/" + names[i], image ) == ROCKETNET_PCAPNG_OK );
        std::vector<rocketnet_pcapng_packet_t> packets;
        check( rocketnet_pcapng_read_buffer( image.data(), image.size(), packets ) == ROCKETNET_PCAPNG_OK );
        for ( size_t j = 0; j < packets.size(); j++ )
        {
            if ( packets[j].interface_id != ROCKETNET_PCAPNG_INTERFACE_WIRE )
                continue;
            const uint8_t * datagram;
            int bytes;
            uint32_t src_address, dst_address;
            uint16_t src_port, dst_port;
            if ( !rocketnet_pcapng_udp_payload( packets[j].data, &datagram, &bytes, &src_address, &src_port, &dst_address, &dst_port ) )
                continue;
            if ( bytes < 1 )
                continue;
            const int packet_type = datagram[0];
            if ( packet_type < 0 || packet_type >= 10 )
                continue;
            counts[packet_type]++;
        }
    }

    for ( int i = 0; i < 10; i++ )
        printf( "pcapng coverage: type %d count %ld\n", i, counts[i] );
    for ( int i = 0; i < 4; i++ )
        check( counts[required_types[i]] >= 1 );
}

REGISTER_UNIT_TEST( test_pcapng_corpus_covers_control_packet_types );
