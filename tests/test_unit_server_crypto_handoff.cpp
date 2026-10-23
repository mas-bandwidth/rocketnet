/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server AEAD handoff (tla/RNServerCrypto.tla). Manual mode: this thread
// calls step, no worker thread, no wake, no socket, no clock. A sealed or
// opened job matches rocketnet_write_sealed_packet / rocketnet_open_sealed_packet
// byte for byte. take before step returns NULL. Three published banks refuse
// another claim; release of the oldest makes a bank free again.

#include "test_unit_registry.h"

#include "rocketnet_server_crypto.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"

#include <string.h>

#define check rocketnet_unit_check

static const uint64_t server_crypto_protocol_id = 0x1122334455667788ULL;

static void server_crypto_key( uint8_t * key, uint8_t * salt, uint8_t * filter )
{
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
        key[i] = (uint8_t) ( 0x40 + i );
    for ( int i = 0; i < ROCKETNET_CONNECTION_SALT_BYTES; i++ )
        salt[i] = (uint8_t) ( 0x90 + i );
    for ( int i = 0; i < ROCKETNET_FILTER_VALUE_BYTES; i++ )
        filter[i] = (uint8_t) ( 0xB0 + i );
}

static void server_crypto_fill_seal( struct rocketnet_server_crypto_job_t * job, const uint8_t * key, const uint8_t * salt,
                                     const uint8_t * filter, uint64_t sequence, const uint8_t * plain, int plain_bytes )
{
    job->kind = ROCKETNET_SERVER_CRYPTO_SEAL;
    job->packet_type = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    job->sequence = sequence;
    job->protocol_id = server_crypto_protocol_id;
    memcpy( job->key, key, ROCKETNET_KEY_BYTES );
    memcpy( job->salt, salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( job->filter, filter, ROCKETNET_FILTER_VALUE_BYTES );
    job->has_ephemeral = 0;
    job->has_session_nonce = 0;
    memcpy( job->plain, plain, (size_t) plain_bytes );
    job->plain_bytes = plain_bytes;
}

static int server_crypto_direct_seal( uint8_t * packet, const uint8_t * key, const uint8_t * salt, const uint8_t * filter,
                                      uint64_t sequence, const uint8_t * plain, int plain_bytes )
{
    return rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, filter, sequence,
                                          NULL, plain, plain_bytes, server_crypto_protocol_id, salt, NULL, key );
}

static void test_server_crypto_handoff_seal_and_open_match()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    server_crypto_key( key, salt, filter );

    uint8_t plain[8];
    for ( int i = 0; i < 8; i++ )
        plain[i] = (uint8_t) ( 0x10 + i );

    uint8_t direct[ROCKETNET_RECEIVE_CEILING];
    const int direct_bytes = server_crypto_direct_seal( direct, key, salt, filter, 1, plain, 8 );
    check( direct_bytes > 0 );

    struct rocketnet_server_crypto_t * crypto = rocketnet_server_crypto_create( NULL, 1 );
    check( crypto );

    int count = -1;
    int bank = -1;
    check( rocketnet_server_crypto_take( crypto, &count, &bank ) == NULL );

    struct rocketnet_server_crypto_job_t * job = rocketnet_server_crypto_claim( crypto );
    check( job );
    server_crypto_fill_seal( job, key, salt, filter, 1, plain, 8 );
    memset( key, 0x5A, sizeof( key ) );
    check( rocketnet_server_crypto_publish( crypto ) );
    check( rocketnet_server_crypto_take( crypto, &count, &bank ) == NULL );
    check( rocketnet_server_crypto_step( crypto ) );

    struct rocketnet_server_crypto_job_t * done = rocketnet_server_crypto_take( crypto, &count, &bank );
    check( done );
    check( count == 1 );
    check( done[0].result == direct_bytes );
    check( memcmp( done[0].packet, direct, (size_t) direct_bytes ) == 0 );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint8_t open_key[ROCKETNET_KEY_BYTES];
    server_crypto_key( open_key, salt, filter );
    const int opened_direct = rocketnet_open_sealed_packet( opened, NULL, ROCKETNET_RECEIVE_CEILING, direct, direct_bytes,
                                                            ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1, server_crypto_protocol_id,
                                                            salt, NULL, open_key );
    check( opened_direct == 8 );
    check( memcmp( opened, plain, 8 ) == 0 );

    check( rocketnet_server_crypto_release( crypto, bank ) );

    job = rocketnet_server_crypto_claim( crypto );
    check( job );
    job->kind = ROCKETNET_SERVER_CRYPTO_OPEN;
    job->packet_type = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    job->sequence = 1;
    job->protocol_id = server_crypto_protocol_id;
    memcpy( job->key, open_key, ROCKETNET_KEY_BYTES );
    memcpy( job->salt, salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( job->packet, direct, (size_t) direct_bytes );
    job->packet_bytes = direct_bytes;
    check( rocketnet_server_crypto_publish( crypto ) );
    check( rocketnet_server_crypto_step( crypto ) );
    done = rocketnet_server_crypto_take( crypto, &count, &bank );
    check( done );
    check( done[0].result == 8 );
    check( memcmp( done[0].plain, plain, 8 ) == 0 );
    check( rocketnet_server_crypto_release( crypto, bank ) );

    job = rocketnet_server_crypto_claim( crypto );
    check( job );
    job->kind = ROCKETNET_SERVER_CRYPTO_OPEN;
    job->packet_type = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    job->sequence = 1;
    job->protocol_id = server_crypto_protocol_id;
    memcpy( job->key, open_key, ROCKETNET_KEY_BYTES );
    memcpy( job->salt, salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( job->packet, direct, (size_t) direct_bytes );
    job->packet[direct_bytes - 1] ^= 0x01;
    job->packet_bytes = direct_bytes;
    check( rocketnet_server_crypto_publish( crypto ) );
    check( rocketnet_server_crypto_step( crypto ) );
    done = rocketnet_server_crypto_take( crypto, &count, &bank );
    check( done );
    check( done[0].result < 0 );
    check( rocketnet_server_crypto_release( crypto, bank ) );

    rocketnet_server_crypto_destroy( crypto );
    rocketnet_server_crypto_destroy( NULL );
}

static void test_server_crypto_handoff_in_place_matches()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    server_crypto_key( key, salt, filter );

    const int payload_at = ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
    const int payload_bytes = 4;
    uint8_t ack[ROCKETNET_ACK_BLOCK_MIN_BYTES];
    memset( ack, 0x11, sizeof( ack ) );

    uint8_t direct[ROCKETNET_RECEIVE_CEILING];
    uint8_t via[ROCKETNET_RECEIVE_CEILING];
    memset( direct, 0xA5, sizeof( direct ) );
    for ( int i = 0; i < payload_bytes; i++ )
        direct[payload_at + i] = (uint8_t) ( 0x21 + i );
    memcpy( via, direct, sizeof( via ) );

    const int direct_bytes = rocketnet_write_sealed_payload_in_place( direct, filter, 7, ack, ROCKETNET_ACK_BLOCK_MIN_BYTES,
                                                                      payload_bytes, server_crypto_protocol_id, salt, key );
    check( direct_bytes > 0 );

    struct rocketnet_server_crypto_t * crypto = rocketnet_server_crypto_create( NULL, 1 );
    check( crypto );
    struct rocketnet_server_crypto_job_t * job = rocketnet_server_crypto_claim( crypto );
    check( job );
    job->kind = ROCKETNET_SERVER_CRYPTO_SEAL_IN_PLACE;
    job->sequence = 7;
    job->protocol_id = server_crypto_protocol_id;
    memcpy( job->key, key, ROCKETNET_KEY_BYTES );
    memcpy( job->salt, salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( job->filter, filter, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( job->ack, ack, sizeof( ack ) );
    job->ack_bytes = ROCKETNET_ACK_BLOCK_MIN_BYTES;
    job->plain_bytes = payload_bytes;
    job->inplace = via;
    check( rocketnet_server_crypto_publish( crypto ) );
    check( rocketnet_server_crypto_step( crypto ) );

    int count = 0;
    int bank = -1;
    struct rocketnet_server_crypto_job_t * done = rocketnet_server_crypto_take( crypto, &count, &bank );
    check( done );
    check( done[0].result == direct_bytes );
    check( memcmp( via, direct, (size_t) direct_bytes ) == 0 );
    check( rocketnet_server_crypto_release( crypto, bank ) );
    rocketnet_server_crypto_destroy( crypto );
}

static void test_server_crypto_handoff_banks_refuse_and_stay_in_order()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    server_crypto_key( key, salt, filter );

    struct rocketnet_server_crypto_t * crypto = rocketnet_server_crypto_create( NULL, 1 );
    check( crypto );
    check( rocketnet_server_crypto_publish( crypto ) == false );

    for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
    {
        struct rocketnet_server_crypto_job_t * job = rocketnet_server_crypto_claim( crypto );
        check( job );
        uint8_t plain[1];
        plain[0] = (uint8_t) ( 0x30 + b );
        server_crypto_fill_seal( job, key, salt, filter, (uint64_t) ( b + 1 ), plain, 1 );
        check( rocketnet_server_crypto_publish( crypto ) );
    }
    check( rocketnet_server_crypto_claim( crypto ) == NULL );

    for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
    {
        check( rocketnet_server_crypto_step( crypto ) );
        int count = 0;
        int bank = -1;
        struct rocketnet_server_crypto_job_t * done = rocketnet_server_crypto_take( crypto, &count, &bank );
        check( done );
        check( count == 1 );
        check( done[0].plain[0] == (uint8_t) ( 0x30 + b ) );
        check( done[0].result > 0 );
        check( rocketnet_server_crypto_release( crypto, bank ) );
        check( rocketnet_server_crypto_release( crypto, bank ) == false );
    }

    struct rocketnet_server_crypto_job_t * again = rocketnet_server_crypto_claim( crypto );
    check( again );
    check( rocketnet_server_crypto_busy_skips( crypto ) > 0 );
    rocketnet_server_crypto_destroy( crypto );
}

static void test_server_crypto_handoff_job_cap_refuses()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    server_crypto_key( key, salt, filter );

    struct rocketnet_server_crypto_t * crypto = rocketnet_server_crypto_create( NULL, 1 );
    check( crypto );
    for ( int i = 0; i < ROCKETNET_SERVER_CRYPTO_JOBS; i++ )
    {
        struct rocketnet_server_crypto_job_t * job = rocketnet_server_crypto_claim( crypto );
        check( job );
        uint8_t plain[1];
        plain[0] = (uint8_t) i;
        server_crypto_fill_seal( job, key, salt, filter, 1, plain, 1 );
    }
    check( rocketnet_server_crypto_claim( crypto ) == NULL );
    check( rocketnet_server_crypto_publish( crypto ) );
    check( rocketnet_server_crypto_step( crypto ) );

    int count = 0;
    int bank = -1;
    struct rocketnet_server_crypto_job_t * done = rocketnet_server_crypto_take( crypto, &count, &bank );
    check( done );
    check( count == ROCKETNET_SERVER_CRYPTO_JOBS );
    check( done[0].plain[0] == 0 );
    check( done[ROCKETNET_SERVER_CRYPTO_JOBS - 1].plain[0] == (uint8_t) ( ROCKETNET_SERVER_CRYPTO_JOBS - 1 ) );
    check( done[0].result > 0 );
    check( rocketnet_server_crypto_release( crypto, bank ) );
    rocketnet_server_crypto_destroy( crypto );
}

REGISTER_UNIT_TEST( test_server_crypto_handoff_seal_and_open_match );
REGISTER_UNIT_TEST( test_server_crypto_handoff_in_place_matches );
REGISTER_UNIT_TEST( test_server_crypto_handoff_banks_refuse_and_stay_in_order );
REGISTER_UNIT_TEST( test_server_crypto_handoff_job_cap_refuses );
