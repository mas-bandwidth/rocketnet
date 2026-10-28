/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Decrypt on the client I/O thread (tla/RNClientIoDecrypt.tla,
// docs/CLIENT-IO-DECRYPT.md If decrypt moves): the I/O step opens each session
// datagram before IoPublish with its own copy of the open key and its own
// replay window, and update consumes the records in arrival order. Single
// threaded and step-driven: the fake client transport, the step called on the
// test's thread in place of the I/O thread, a test clock from 1000.0, and
// session datagrams sealed by the test under a known key. No sockets, no real
// time.

#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static const uint64_t client_io_decrypt_protocol_id = 0x1122334455667788ULL;

struct client_io_decrypt_test_clock_t
{
    double now;
};

static double client_io_decrypt_test_time( void * context )
{
    return ( (client_io_decrypt_test_clock_t *) context )->now;
}

// One I/O core on the fake transport, and one client protocol in connected with
// the same known session key: what update holds after the challenge.
struct client_io_decrypt_fixture_t
{
    rocketnet_fake_client_transport_io_t fake;
    rocketnet_client_transport_io_t transport;
    rocketnet_client_io_rig_t * rig;
    client_io_decrypt_test_clock_t clock;
    rocketnet_client_io_t io;
    rocketnet_counters_t counters;
    rocketnet_client_protocol_t client;
    uint8_t key[ROCKETNET_KEY_BYTES];
    rocketnet_address_t server;
};

static void client_io_decrypt_setup( client_io_decrypt_fixture_t * f )
{
    memset( &f->fake, 0, sizeof( f->fake ) );
    f->transport = rocketnet_client_transport_io_t();

    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    check( rocketnet_client_transport_io_open( &rocketnet_fake_client_operations_io, &f->fake, &config, &bind, &f->transport ) );

    f->rig = rocketnet_client_io_rig_create();
    f->clock.now = 1000.0;
    rocketnet_client_io_rig_init( f->rig, &f->io, &f->transport, client_io_decrypt_test_time, &f->clock );

    memset( &f->counters, 0, sizeof( f->counters ) );
    rocketnet_client_protocol_init( &f->client, &f->counters );
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        f->key[i] = (uint8_t) ( 0x40 + i );
    }
    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
    {
        f->client.connection_salt[i] = (uint8_t) ( 0x90 + i );
    }
    memcpy( f->client.keys.session_server_to_client, f->key, ROCKETNET_KEY_BYTES );
    f->client.signed_fields.protocol_id = client_io_decrypt_protocol_id;
    f->client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    f->client.send_sequence_session = 1;

    rocketnet_address_load_ipv4( &f->server, 0x7f000001, 40000 );
    f->client.server_address = f->server;
}

static void client_io_decrypt_teardown( client_io_decrypt_fixture_t * f )
{
    rocketnet_client_transport_io_close( &f->transport );
    rocketnet_client_io_rig_destroy( f->rig );
}

// update's PublishKeys at the challenge, from the protocol's own fields

static void client_io_decrypt_publish( client_io_decrypt_fixture_t * f )
{
    rocketnet_client_io_publish_keys( &f->io, f->client.keys.session_server_to_client, f->client.signed_fields.protocol_id,
                                      f->client.connection_salt, f->client.packet_filter, &f->client.server_address );
}

// the plaintext of a payload: the minimum ack block and one payload byte

static int client_io_decrypt_plaintext( uint8_t * out, uint8_t payload_byte )
{
    memset( out, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    out[ROCKETNET_ACK_BLOCK_MIN_BYTES] = payload_byte;
    return ROCKETNET_ACK_BLOCK_MIN_BYTES + 1;
}

// seals a payload packet from the server under the known key and pushes it

static int client_io_decrypt_push_payload_from( client_io_decrypt_fixture_t * f, const rocketnet_address_t * from,
                                                uint64_t sequence, uint8_t payload_byte, bool forge )
{
    uint8_t plaintext[64];
    const int plaintext_bytes = client_io_decrypt_plaintext( plaintext, payload_byte );
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, f->client.packet_filter, sequence,
                                                     NULL, plaintext, plaintext_bytes, client_io_decrypt_protocol_id,
                                                     f->client.connection_salt, NULL, f->key );
    check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + plaintext_bytes );
    if ( forge )
        packet[ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES] ^= 0x01;   // one ciphertext byte flipped
    rocketnet_fake_client_push( &f->fake, from, packet, bytes );

    return bytes;
}

static int client_io_decrypt_push_payload( client_io_decrypt_fixture_t * f, uint64_t sequence, uint8_t payload_byte, bool forge )
{
    return client_io_decrypt_push_payload_from( f, &f->server, sequence, payload_byte, forge );
}

// update's consume of one exposed record (tla/RNClientIoDecrypt.tla UpdateConsume)

static int client_io_decrypt_consume( client_io_decrypt_fixture_t * f, rocketnet_client_receive_buffer_t * buffer, int i,
                                      uint8_t * payload, int * payload_bytes )
{
    return rocketnet_client_protocol_process_record( &f->client, buffer->packet_data[i], (int) buffer->packet_bytes[i],
                                                     buffer->verdict[i], buffer->sequence[i], f->clock.now,
                                                     payload, ROCKETNET_FRAME_SIZE, payload_bytes );
}

// IoTakeKeys, IoCheck, IoOpen before IoPublish: two sealed datagrams, one
// step. Both records carry the opened verdict, their sequence and the
// plaintext behind the wire header; the step ran both AEADs and update's
// consume runs none, and delivers both payloads.

static void test_client_io_decrypt_opens_before_publish()
{
    static client_io_decrypt_fixture_t f;
    client_io_decrypt_setup( &f );
    client_io_decrypt_publish( &f );

    const int bytes1 = client_io_decrypt_push_payload( &f, 1, 0xA1, false );
    const int bytes2 = client_io_decrypt_push_payload( &f, 2, 0xA2, false );

    const uint64_t opens_before_step = rocketnet_crypto_tally_get().aead_open;
    check( rocketnet_client_io_step( &f.io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( rocketnet_crypto_tally_get().aead_open - opens_before_step == 2 );

    rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &f.io );
    check( buffer );
    check( buffer->num_packets == 2 );

    const int wire_bytes[2] = { bytes1, bytes2 };

    for ( int i = 0; i < 2; i++ )
    {
        check( buffer->verdict[i] == ROCKETNET_CLIENT_IO_VERDICT_OPENED );
        check( buffer->sequence[i] == (uint64_t) ( i + 1 ) );
        check( (int) buffer->packet_bytes[i] == wire_bytes[i] );

        uint8_t plaintext[64];
        const int plaintext_bytes = client_io_decrypt_plaintext( plaintext, (uint8_t) ( 0xA1 + i ) );
        check( memcmp( buffer->packet_data[i] + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, plaintext, (size_t) plaintext_bytes ) == 0 );
    }

    const uint64_t opens_before_consume = rocketnet_crypto_tally_get().aead_open;

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t payload[ROCKETNET_FRAME_SIZE];
        int payload_bytes = 0;
        check( client_io_decrypt_consume( &f, buffer, i, payload, &payload_bytes ) == 1 );
        check( payload_bytes == 1 );
        check( payload[0] == (uint8_t) ( 0xA1 + i ) );
    }

    check( rocketnet_crypto_tally_get().aead_open == opens_before_consume );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 2 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN] == 0 );
    check( f.client.replay_session.highest_opened == 2 );
    check( f.io.open.replay.highest_opened == 2 );

    rocketnet_client_io_return( &f.io );
    client_io_decrypt_teardown( &f );
}
REGISTER_UNIT_TEST( test_client_io_decrypt_opens_before_publish );

// IoCheck's replay check on the I/O thread's window comes before the AEAD
// (CheapRefusalsFirst): the same sealed datagram twice in one step opens once,
// the copy's record is already_received, and update counts it once.

static void test_client_io_decrypt_replay_refused_before_open()
{
    static client_io_decrypt_fixture_t f;
    client_io_decrypt_setup( &f );
    client_io_decrypt_publish( &f );

    client_io_decrypt_push_payload( &f, 1, 0xB1, false );
    client_io_decrypt_push_payload( &f, 1, 0xB1, false );

    const uint64_t opens_before_step = rocketnet_crypto_tally_get().aead_open;
    check( rocketnet_client_io_step( &f.io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( rocketnet_crypto_tally_get().aead_open - opens_before_step == 1 );

    rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &f.io );
    check( buffer );
    check( buffer->num_packets == 2 );
    check( buffer->verdict[0] == ROCKETNET_CLIENT_IO_VERDICT_OPENED );
    check( buffer->verdict[1] == ROCKETNET_CLIENT_IO_VERDICT_ALREADY_RECEIVED );

    const uint64_t opens_before_consume = rocketnet_crypto_tally_get().aead_open;
    uint8_t payload[ROCKETNET_FRAME_SIZE];
    int payload_bytes = 0;
    check( client_io_decrypt_consume( &f, buffer, 0, payload, &payload_bytes ) == 1 );
    check( client_io_decrypt_consume( &f, buffer, 1, payload, &payload_bytes ) == 0 );
    check( rocketnet_crypto_tally_get().aead_open == opens_before_consume );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED] == 1 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 1 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED] == 2 );

    rocketnet_client_io_return( &f.io );
    client_io_decrypt_teardown( &f );
}
REGISTER_UNIT_TEST( test_client_io_decrypt_replay_refused_before_open );

// RetireKeys then IoDropKeys (NoOpenWithoutKeys): publish and take, retire,
// then a sealed datagram. The retire wakes the I/O thread; the step reads retired at
// the top of the drain, zeroes its copy and opens nothing: the record is raw.

static void test_client_io_decrypt_no_open_after_retire()
{
    static client_io_decrypt_fixture_t f;
    client_io_decrypt_setup( &f );
    client_io_decrypt_publish( &f );

    // IoTakeKeys as the I/O thread runs it at the top of a drain: the copy is live

    rocketnet_client_io_take_keys( &f.io );
    check( f.io.open.loaded_epoch == ROCKETNET_CLIENT_IO_EPOCH_LIVE );
    check( memcmp( f.io.open.session_server_to_client, f.key, ROCKETNET_KEY_BYTES ) == 0 );

    const int wakes_before = f.fake.wake_calls;
    rocketnet_client_io_retire_keys( &f.io );
    check( f.fake.wake_calls == wakes_before + 1 );

    client_io_decrypt_push_payload( &f, 1, 0xC1, false );

    const uint64_t opens_before_step = rocketnet_crypto_tally_get().aead_open;
    check( rocketnet_client_io_step( &f.io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( rocketnet_crypto_tally_get().aead_open == opens_before_step );

    rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &f.io );
    check( buffer );
    check( buffer->num_packets == 1 );
    check( buffer->verdict[0] == ROCKETNET_CLIENT_IO_VERDICT_RAW );
    check( buffer->sequence[0] == 0 );

    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        check( f.io.open.session_server_to_client[i] == 0 );
    }
    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
    {
        check( f.io.open.connection_salt[i] == 0 );
    }

    // a publish after the retire is refused: the copy stays zero

    client_io_decrypt_publish( &f );
    check( f.io.open.epoch.load() == ROCKETNET_CLIENT_IO_EPOCH_RETIRED );
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        check( f.io.open.session_server_to_client[i] == 0 );
    }

    rocketnet_client_io_return( &f.io );
    client_io_decrypt_teardown( &f );
}
REGISTER_UNIT_TEST( test_client_io_decrypt_no_open_after_retire );

// UpdateConsume counts every verdict in its arrival position (SingleWriter):
// genuine, forged (one byte flipped), genuine. The I/O thread's records are
// opened, did_not_open, opened; update counts did_not_open once, and the
// integrity failure runs after the first genuine record is accepted and before
// the third is consumed.

static void test_client_io_decrypt_verdicts_counted_in_order()
{
    static client_io_decrypt_fixture_t f;
    client_io_decrypt_setup( &f );
    client_io_decrypt_publish( &f );

    client_io_decrypt_push_payload( &f, 1, 0xD1, false );
    client_io_decrypt_push_payload( &f, 2, 0xD2, true );
    client_io_decrypt_push_payload( &f, 3, 0xD3, false );

    const uint64_t opens_before_step = rocketnet_crypto_tally_get().aead_open;
    check( rocketnet_client_io_step( &f.io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( rocketnet_crypto_tally_get().aead_open - opens_before_step == 3 );

    rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &f.io );
    check( buffer );
    check( buffer->num_packets == 3 );
    check( buffer->verdict[0] == ROCKETNET_CLIENT_IO_VERDICT_OPENED );
    check( buffer->verdict[1] == ROCKETNET_CLIENT_IO_VERDICT_DID_NOT_OPEN );
    check( buffer->verdict[2] == ROCKETNET_CLIENT_IO_VERDICT_OPENED );

    // the I/O thread counted nothing

    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED] == 0 );
    check( f.client.integrity_failures == 0 );

    uint8_t payload[ROCKETNET_FRAME_SIZE];
    int payload_bytes = 0;

    client_io_decrypt_consume( &f, buffer, 0, payload, &payload_bytes );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 1 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN] == 0 );
    check( f.client.integrity_failures == 0 );

    client_io_decrypt_consume( &f, buffer, 1, payload, &payload_bytes );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] == 1 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN] == 1 );
    check( f.client.integrity_failures == 1 );

    const uint64_t delivered_before_third = f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED];
    const uint64_t wrong_state_before_third = f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE];
    client_io_decrypt_consume( &f, buffer, 2, payload, &payload_bytes );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN] == 1 );
    check( f.client.integrity_failures == 1 );

    // the third is accepted, or refused by the state check if the limit ended
    // the session: exactly one of the two moved

    check( ( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] - delivered_before_third ) +
               ( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE] - wrong_state_before_third ) ==
           1 );
    check( f.counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED] == 3 );

    rocketnet_client_io_return( &f.io );
    client_io_decrypt_teardown( &f );
}
REGISTER_UNIT_TEST( test_client_io_decrypt_verdicts_counted_in_order );

// A copy of a genuine datagram from any source but the dialed server stays raw
// and leaves the I/O thread's window alone: update drops it as junk, so the
// genuine copy that follows from the server still opens and is delivered.

static void test_client_io_decrypt_other_source_stays_raw()
{
    static client_io_decrypt_fixture_t f;
    client_io_decrypt_setup( &f );
    client_io_decrypt_publish( &f );

    rocketnet_address_t other = {};
    rocketnet_address_load_ipv4( &other, 0x7f000002, 40000 );
    client_io_decrypt_push_payload_from( &f, &other, 1, 0xE1, false );
    client_io_decrypt_push_payload( &f, 1, 0xE1, false );

    const uint64_t opens_before_step = rocketnet_crypto_tally_get().aead_open;
    check( rocketnet_client_io_step( &f.io, false ) == ROCKETNET_CLIENT_IO_WORKED );
    check( rocketnet_crypto_tally_get().aead_open - opens_before_step == 1 );

    rocketnet_client_receive_buffer_t * buffer = rocketnet_client_io_expose( &f.io );
    check( buffer );
    check( buffer->num_packets == 2 );
    check( buffer->verdict[0] == ROCKETNET_CLIENT_IO_VERDICT_RAW );
    check( buffer->verdict[1] == ROCKETNET_CLIENT_IO_VERDICT_OPENED );
    check( buffer->sequence[1] == 1 );

    uint8_t payload[ROCKETNET_FRAME_SIZE];
    int payload_bytes = 0;
    check( client_io_decrypt_consume( &f, buffer, 1, payload, &payload_bytes ) == 1 );
    check( payload_bytes == 1 && payload[0] == 0xE1 );

    rocketnet_client_io_return( &f.io );
    client_io_decrypt_teardown( &f );
}
REGISTER_UNIT_TEST( test_client_io_decrypt_other_source_stays_raw );
