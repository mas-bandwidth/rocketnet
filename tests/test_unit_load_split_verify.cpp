/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The LAN proof's per-stream delivery checker (tools/load_split_verify.h).
// Each case feeds a clean stream the checker must pass and a stream with one
// planted violation the checker must catch. Pure logic: no time, no sockets.

#include "test_unit_registry.h"

#include "../tools/load_split_verify.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static const uint64_t split_verify_test_run_id = 0x1234abcd5678ef01ULL;

static struct split_verify_payload_t split_verify_test_payload( uint32_t mode, uint32_t size_class, uint32_t sequence )
{
    struct split_verify_payload_t payload;
    payload.run_id = split_verify_test_run_id;
    payload.client = 7;
    payload.direction = SPLIT_VERIFY_CLIENT_TO_SERVER;
    payload.mode = mode;
    payload.size_class = size_class;
    payload.sequence = sequence;
    payload.length = size_class == SPLIT_VERIFY_SMALL ? 100u : 3000u;
    return payload;
}

// write, read back and deliver one message: the path the proof's receiver runs
static int split_verify_test_deliver( struct split_verify_stream_t * stream, uint32_t sequence )
{
    static uint8_t buffer[4096];
    const struct split_verify_payload_t sent = split_verify_test_payload( stream->mode, stream->size_class, sequence );
    const uint32_t bytes = split_verify_write_payload( buffer, sizeof( buffer ), &sent );
    check( bytes == sent.length );
    struct split_verify_payload_t received;
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_OK );
    return split_verify_stream_deliver( stream, &received );
}

static void test_split_verify_reliable_exactly_once()
{
    struct split_verify_stream_t stream;
    struct split_verify_stream_result_t result;

    // clean: RELIABLE delivers 1..5 in any order, each once; the sender sent 5
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE, SPLIT_VERIFY_SMALL );
    const uint32_t clean_order[] = { 2, 1, 4, 3, 5 };
    for ( uint32_t i = 0; i < 5; ++i )
        check( split_verify_test_deliver( &stream, clean_order[i] ) == 0 );
    check( split_verify_stream_final( &stream, 5, &result ) );
    check( result.missing == 0 && result.delivered == 5 );

    // planted: one message never delivered (a reliable gap fails, never counts as loss)
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, SPLIT_VERIFY_FRAGMENTED );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 3 ) == 0 );
    check( !split_verify_stream_final( &stream, 3, &result ) );
    check( result.missing == 1 && result.lost == 0 );

    // planted: one message delivered twice
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 2 ) == 0 );
    check( split_verify_test_deliver( &stream, 2 ) == 1 );
    check( !split_verify_stream_final( &stream, 2, &result ) );
    check( stream.duplicates == 1 && result.missing == 0 );

    // planted: a delivery the sender never sent
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE, SPLIT_VERIFY_FRAGMENTED );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 2 ) == 0 );
    check( !split_verify_stream_final( &stream, 1, &result ) );
    check( result.phantom == 1 && result.missing == 0 );

    // planted: a sender count past the window is reported, never wrapped
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE, SPLIT_VERIFY_SMALL );
    check( !split_verify_stream_final( &stream, SPLIT_VERIFY_MAX_SEQUENCE + 1, &result ) );
    check( result.window_exceeded );
}

static void test_split_verify_ordered_no_gaps()
{
    struct split_verify_stream_t stream;
    struct split_verify_stream_result_t result;

    // clean: 1..6 in order, fragmented RELIABLE_ORDERED_WITH_ACK_RECEIPT
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, SPLIT_VERIFY_FRAGMENTED );
    for ( uint32_t sequence = 1; sequence <= 6; ++sequence )
        check( split_verify_test_deliver( &stream, sequence ) == 0 );
    check( split_verify_stream_final( &stream, 6, &result ) );

    // planted: a gap (3 skipped, delivered later) breaks order at once
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 2 ) == 0 );
    check( split_verify_test_deliver( &stream, 4 ) == 1 );
    check( split_verify_test_deliver( &stream, 3 ) == 1 );
    check( stream.order_errors == 2 && stream.duplicates == 0 );
    check( !split_verify_stream_final( &stream, 4, &result ) );
    check( result.missing == 0 );

    // planted: the first delivery is not sequence 1
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_VERIFY_FRAGMENTED );
    check( split_verify_test_deliver( &stream, 2 ) == 1 );
    check( stream.order_errors == 1 );
}

static void test_split_verify_sequenced_never_older()
{
    struct split_verify_stream_t stream;
    struct split_verify_stream_result_t result;

    // clean: UNRELIABLE_SEQUENCED skips forward (loss is counted, not failed)
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 4 ) == 0 );
    check( split_verify_test_deliver( &stream, 5 ) == 0 );
    check( split_verify_stream_final( &stream, 6, &result ) );
    check( result.lost == 3 && result.missing == 0 );

    // planted: a message older than the newest delivered
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 5 ) == 0 );
    check( split_verify_test_deliver( &stream, 3 ) == 1 );
    check( stream.stale_deliveries == 1 );
    check( !split_verify_stream_final( &stream, 5, &result ) );

    // planted: the same on RELIABLE_SEQUENCED, fragmented
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, SPLIT_VERIFY_FRAGMENTED );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 2 ) == 0 );
    check( split_verify_test_deliver( &stream, 1 ) == 1 );
    check( stream.stale_deliveries == 1 );
    check( !split_verify_stream_final( &stream, 2, &result ) );
}

static void test_split_verify_unreliable_no_duplicates()
{
    struct split_verify_stream_t stream;
    struct split_verify_stream_result_t result;

    // clean: UNRELIABLE out of order with loss; loss is counted, the stream passes
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_UNRELIABLE, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 3 ) == 0 );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_stream_final( &stream, 10, &result ) );
    check( result.lost == 8 && result.delivered == 2 );

    // planted: one duplicate on UNRELIABLE_WITH_ACK_RECEIPT
    split_verify_stream_init( &stream, 7, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 1 ) == 0 );
    check( split_verify_test_deliver( &stream, 1 ) == 1 );
    check( stream.duplicates == 1 && stream.delivered == 1 );
    check( !split_verify_stream_final( &stream, 1, &result ) );

    // planted: a payload that names another stream (another client) is an identity error
    split_verify_stream_init( &stream, 8, SPLIT_VERIFY_CLIENT_TO_SERVER, ROCKETNET_RELIABILITY_UNRELIABLE, SPLIT_VERIFY_SMALL );
    check( split_verify_test_deliver( &stream, 1 ) == 1 );
    check( stream.identity_errors == 1 && stream.delivered == 0 );
    check( !split_verify_stream_final( &stream, 1, &result ) );

    // an unreliable mode has no fragmented class: the writer refuses it
    static uint8_t buffer[4096];
    const struct split_verify_payload_t fragmented = split_verify_test_payload( ROCKETNET_RELIABILITY_UNRELIABLE, SPLIT_VERIFY_FRAGMENTED, 1 );
    check( split_verify_write_payload( buffer, sizeof( buffer ), &fragmented ) == 0 );
}

static void test_split_verify_receipts_match()
{
    static struct split_verify_ledger_t ledger;
    static struct split_verify_stream_t receiver[SPLIT_VERIFY_STREAMS_PER_CLIENT];
    struct split_verify_ledger_result_t result;

    const uint32_t direction = SPLIT_VERIFY_CLIENT_TO_SERVER;
    for ( uint32_t mode = 0; mode < SPLIT_VERIFY_NUM_MODES; ++mode )
        for ( uint32_t size_class = 0; size_class < SPLIT_VERIFY_NUM_SIZE_CLASSES; ++size_class )
            split_verify_stream_init( &receiver[split_verify_stream_index( direction, mode, size_class )], 7, direction, mode, size_class );
    struct split_verify_stream_t * reliable = &receiver[split_verify_stream_index( direction, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL )];
    struct split_verify_stream_t * unreliable = &receiver[split_verify_stream_index( direction, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL )];

    // clean: two reliable sends ACKED and received; one unreliable send ACKED
    // and received, one LOST; serials are not contiguous (other sends between)
    split_verify_ledger_init( &ledger, 7, direction );
    check( split_verify_ledger_send( &ledger, 10, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 1 ) == 0 );
    check( split_verify_ledger_send( &ledger, 12, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 1 ) == 0 );
    check( split_verify_ledger_send( &ledger, 15, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 2 ) == 0 );
    check( split_verify_ledger_send( &ledger, 16, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 2 ) == 0 );
    check( split_verify_test_deliver( reliable, 1 ) == 0 );
    check( split_verify_test_deliver( reliable, 2 ) == 0 );
    check( split_verify_test_deliver( unreliable, 1 ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 15, ROCKETNET_RECEIPT_STATUS_ACKED ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 10, ROCKETNET_RECEIPT_STATUS_ACKED ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 12, ROCKETNET_RECEIPT_STATUS_ACKED ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 16, ROCKETNET_RECEIPT_STATUS_LOSS ) == 0 );
    check( split_verify_ledger_final( &ledger, receiver, 0, &result ) );
    check( result.acked == 3 && result.lost == 1 );

    // planted: receipts_dropped is not zero
    check( !split_verify_ledger_final( &ledger, receiver, 1, &result ) );

    // planted: a second outcome for one send, and an outcome for no send
    check( split_verify_ledger_outcome( &ledger, 10, ROCKETNET_RECEIPT_STATUS_ACKED ) == 1 );
    check( split_verify_ledger_outcome( &ledger, 11, ROCKETNET_RECEIPT_STATUS_ACKED ) == 1 );
    check( ledger.repeated_outcomes == 1 && ledger.unknown_serials == 1 );
    check( !split_verify_ledger_final( &ledger, receiver, 0, &result ) );

    // planted: a reliable send reported LOSS
    split_verify_ledger_init( &ledger, 7, direction );
    check( split_verify_ledger_send( &ledger, 20, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, SPLIT_VERIFY_FRAGMENTED, 1 ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 20, ROCKETNET_RECEIPT_STATUS_LOSS ) == 1 );
    check( ledger.reliable_losses == 1 );
    check( !split_verify_ledger_final( &ledger, receiver, 0, &result ) );

    // planted: a send that never got an outcome
    split_verify_ledger_init( &ledger, 7, direction );
    check( split_verify_ledger_send( &ledger, 30, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 1 ) == 0 );
    check( !split_verify_ledger_final( &ledger, receiver, 0, &result ) );
    check( result.without_outcome == 1 );

    // planted: ACKED names a message the receiver never recorded (unreliable 2)
    split_verify_ledger_init( &ledger, 7, direction );
    check( split_verify_ledger_send( &ledger, 40, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 2 ) == 0 );
    check( split_verify_ledger_outcome( &ledger, 40, ROCKETNET_RECEIPT_STATUS_ACKED ) == 0 );
    check( !split_verify_ledger_final( &ledger, receiver, 0, &result ) );
    check( result.acked_unrecorded == 1 );

    // planted: a send in a mode with no receipt, and a serial out of order
    split_verify_ledger_init( &ledger, 7, direction );
    check( split_verify_ledger_send( &ledger, 50, ROCKETNET_RELIABILITY_RELIABLE, SPLIT_VERIFY_SMALL, 1 ) == 1 );
    check( split_verify_ledger_send( &ledger, 52, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 1 ) == 0 );
    check( split_verify_ledger_send( &ledger, 51, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, SPLIT_VERIFY_SMALL, 2 ) == 1 );
    check( ledger.serial_order_errors == 2 );
}

static void test_split_verify_payload_checksum()
{
    static uint8_t buffer[4096];
    struct split_verify_payload_t received;

    // clean: a small and a fragmented payload read back field for field
    const uint32_t classes[] = { SPLIT_VERIFY_SMALL, SPLIT_VERIFY_FRAGMENTED };
    for ( uint32_t c = 0; c < 2; ++c )
    {
        const struct split_verify_payload_t sent = split_verify_test_payload( ROCKETNET_RELIABILITY_RELIABLE_ORDERED, classes[c], 42 );
        const uint32_t bytes = split_verify_write_payload( buffer, sizeof( buffer ), &sent );
        check( bytes == sent.length );
        memset( &received, 0, sizeof( received ) );
        check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_OK );
        check( received.run_id == sent.run_id && received.client == sent.client && received.direction == sent.direction &&
               received.mode == sent.mode && received.size_class == sent.size_class &&
               received.sequence == sent.sequence && received.length == sent.length );
    }

    const struct split_verify_payload_t sent = split_verify_test_payload( ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_VERIFY_FRAGMENTED, 42 );
    const uint32_t bytes = split_verify_write_payload( buffer, sizeof( buffer ), &sent );

    // planted: one flipped body bit, one flipped sequence bit
    buffer[2000] ^= 0x10;
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_CHECKSUM );
    buffer[2000] ^= 0x10;
    buffer[12] ^= 0x01;
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_CHECKSUM );
    buffer[12] ^= 0x01;
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_OK );

    // planted: a truncated payload, another run, a bad magic
    check( split_verify_read_payload( buffer, bytes - 1, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_LENGTH );
    check( split_verify_read_payload( buffer, 16, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_SHORT );
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id + 1, &received ) == SPLIT_VERIFY_READ_RUN_ID );
    buffer[0] ^= 0xff;
    check( split_verify_read_payload( buffer, bytes, split_verify_test_run_id, &received ) == SPLIT_VERIFY_READ_MAGIC );
    buffer[0] ^= 0xff;

    // planted: a small class whose length is past one packet
    struct split_verify_payload_t oversize = split_verify_test_payload( ROCKETNET_RELIABILITY_RELIABLE, SPLIT_VERIFY_SMALL, 1 );
    oversize.length = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1;
    check( split_verify_write_payload( buffer, sizeof( buffer ), &oversize ) == 0 );
}

REGISTER_UNIT_TEST( test_split_verify_reliable_exactly_once );
REGISTER_UNIT_TEST( test_split_verify_ordered_no_gaps );
REGISTER_UNIT_TEST( test_split_verify_sequenced_never_older );
REGISTER_UNIT_TEST( test_split_verify_unreliable_no_duplicates );
REGISTER_UNIT_TEST( test_split_verify_receipts_match );
REGISTER_UNIT_TEST( test_split_verify_payload_checksum );
