/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_client_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet.h"

#include <sodium.h>
#include <string.h>

const char * rocketnet_client_state_name( int state )
{
    switch ( state )
    {
        case ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED:          return "connect token expired";
        case ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN:          return "invalid connect token";
        case ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT:           return "connection timed out";
        case ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT:  return "connection response timed out";
        case ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT:   return "connection request timed out";
        case ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT:       return "cookie request timed out";
        case ROCKETNET_CLIENT_STATE_CONNECTION_DENIED:              return "connection denied";
        case ROCKETNET_CLIENT_STATE_DISCONNECTED:                   return "disconnected";
        case ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE:              return "requesting cookie";
        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST:     return "sending connection request";
        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE:    return "sending connection response";
        case ROCKETNET_CLIENT_STATE_CONNECTED:                      return "connected";
        default:
            rocketnet_assert( 0 );
            return "???";
    }
}

static void rocketnet_client_set_state( struct rocketnet_client_protocol_t * client, int state )
{
    if ( client->state == state )
        return;
    rocketnet_info( "client changed state from '%s' to '%s'", rocketnet_client_state_name( client->state ), rocketnet_client_state_name( state ) );
    client->state = state;
}

void rocketnet_client_protocol_init( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters )
{
    rocketnet_assert( client );
    rocketnet_assert( counters );
    memset( client, 0, sizeof(*client) );
    const uint8_t tag[ROCKETNET_TITLE_TAG_BYTES] = ROCKETNET_TEST_TITLE_TAG;
    memcpy( client->title_tag, tag, ROCKETNET_TITLE_TAG_BYTES );
    client->send_rate = 10.0;
    client->counters = counters;
    client->state = ROCKETNET_CLIENT_STATE_DISCONNECTED;
    client->client_index = -1;
}

int rocketnet_client_connect( struct rocketnet_client_protocol_t * client,
                              const uint8_t * connect_token,
                              double current_time,
                              uint64_t current_timestamp )
{
    rocketnet_assert( client );
    rocketnet_assert( connect_token );
    rocketnet_assert( client->counters );

    // a connect always begins a fresh attempt from the start: fresh salt,
    // fresh ephemeral, sequences at one. recovery is this same path.

    struct rocketnet_counters_t * counters = client->counters;
    uint8_t title_tag[ROCKETNET_TITLE_TAG_BYTES];
    memcpy( title_tag, client->title_tag, ROCKETNET_TITLE_TAG_BYTES );
    double send_rate = client->send_rate;

    memset( client, 0, sizeof(*client) );
    memcpy( client->title_tag, title_tag, ROCKETNET_TITLE_TAG_BYTES );
    client->send_rate = send_rate;
    client->counters = counters;
    client->client_index = -1;
    client->state = ROCKETNET_CLIENT_STATE_DISCONNECTED;

    rocketnet_read_connect_token( &client->token, connect_token );
    rocketnet_read_signed_connect_data( &client->signed_fields, client->token.signed_connect_data );

    // the pre-send token checks, from the standard

    uint8_t flags = client->signed_fields.address_flags;
    if ( flags == 0 || ( flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) != 0 ||
         client->signed_fields.create_timestamp > client->signed_fields.expire_timestamp ||
         client->signed_fields.version != ROCKETNET_VERSION )
    {
        rocketnet_debug( "client connect failed. invalid connect token" );
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
        return 0;
    }

    if ( client->signed_fields.expire_timestamp <= current_timestamp )
    {
        rocketnet_debug( "client connect failed. connect token expired" );
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
        return 0;
    }

    // the server address: IPv6 preferred when both families are present.
    // reachability belongs to the socket layer, which may override the pick.

    memset( &client->server_address, 0, sizeof(client->server_address) );
    if ( flags & ROCKETNET_ADDRESS_FLAG_IPV6 )
    {
        client->server_address.type = ROCKETNET_ADDRESS_IPV6;
        const uint8_t * p = client->signed_fields.ipv6_address;
        for ( int i = 0; i < 8; i++ )
            client->server_address.data.ipv6[i] = rocketnet_read_uint16( &p );
        client->server_address.port = rocketnet_read_uint16( &p );
    }
    else
    {
        client->server_address.type = ROCKETNET_ADDRESS_IPV4;
        const uint8_t * p = client->signed_fields.ipv4_address;
        rocketnet_read_bytes( &p, client->server_address.data.ipv4, 4 );
        client->server_address.port = rocketnet_read_uint16( &p );
    }

    // one salt and one ephemeral keypair per attempt, sequences at one and
    // never reset within it

    randombytes_buf( client->connection_salt, sizeof(client->connection_salt) );
    crypto_box_keypair( client->client_ephemeral_public_key, client->client_ephemeral_secret_key );
    client->send_sequence_handshake = 1;
    client->send_sequence_session = 1;

    client->attempt_start_time = current_time;
    client->last_receive_time = current_time;
    client->last_send_time = current_time - 1000.0;         // the first update sends immediately
    client->total_deadline_seconds = client->signed_fields.expire_timestamp - client->signed_fields.create_timestamp;
    client->timeout_seconds = client->signed_fields.timeout_seconds;

    rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE );

    return 1;
}

// deadlines, in the standard's shape: each waiting state N dies to -(N+1)
// after the token's timeout with nothing received, and the whole handshake
// dies to connect token expired once the token's lifetime has elapsed

static void rocketnet_client_check_deadlines( struct rocketnet_client_protocol_t * client, double current_time )
{
    if ( client->state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
        return;

    if ( client->state < ROCKETNET_CLIENT_STATE_CONNECTED &&
         current_time - client->attempt_start_time >= (double) client->total_deadline_seconds )
    {
        rocketnet_debug( "client connect failed. connect token expired during handshake" );
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
        return;
    }

    if ( client->timeout_seconds > 0 &&
         current_time - client->last_receive_time >= (double) client->timeout_seconds )
    {
        int timed_out_state = -( client->state + 1 );
        rocketnet_debug( "client timed out in state '%s'", rocketnet_client_state_name( client->state ) );
        rocketnet_client_set_state( client, timed_out_state );
    }
}

void rocketnet_client_update( struct rocketnet_client_protocol_t * client,
                              double current_time,
                              uint8_t * out_packet,
                              int * out_packet_bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( out_packet );
    rocketnet_assert( out_packet_bytes );

    *out_packet_bytes = 0;

    rocketnet_client_check_deadlines( client, current_time );

    if ( client->state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
        return;

    // one sending rule: the scheduled rate, nothing sent because a packet
    // arrived, no immediate-send case for any transition

    if ( current_time - client->last_send_time < 1.0 / client->send_rate )
        return;

    client->last_send_time = current_time;

    switch ( client->state )
    {
        case ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE:
        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST:
        {
            rocketnet_write_connection_request_packet( out_packet, client->title_tag,
                                                       client->has_cookie ? client->return_cookie : NULL,
                                                       client->connection_salt,
                                                       client->client_ephemeral_public_key,
                                                       client->token.signed_connect_data,
                                                       client->token.signature );
            *out_packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;
            rocketnet_spam( "client sent connection request" );
            break;
        }

        case ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE:
        {
            // the stored response, byte-identical every retransmission
            memcpy( out_packet, client->stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
            *out_packet_bytes = ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES;
            rocketnet_spam( "client sent connection response" );
            break;
        }

        case ROCKETNET_CLIENT_STATE_CONNECTED:
        {
            uint8_t keep_alive_data[8];
            uint8_t * p = keep_alive_data;
            rocketnet_write_uint32( &p, (uint32_t) client->client_index );
            rocketnet_write_uint32( &p, (uint32_t) client->max_clients );
            int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                       client->title_tag, client->send_sequence_session,
                                                       NULL, keep_alive_data, 8,
                                                       client->signed_fields.protocol_id, client->connection_salt,
                                                       NULL, client->keys.session_client_to_server );
            rocketnet_assert( bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_WIRE_BYTES );
            client->send_sequence_session++;
            *out_packet_bytes = bytes;
            rocketnet_spam( "client sent keep alive" );
            break;
        }
    }
}

// the client-side per-type processing: state gate first, then the mechanics,
// each in its own early-return function

static int rocketnet_client_process_cookie( struct rocketnet_client_protocol_t * client,
                                            const uint8_t * packet_data,
                                            int packet_bytes,
                                            double current_time )
{
    uint64_t * counters = client->counters->values;

    counters[ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED]++;

    if ( client->state != ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE &&
         client->state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST )
    {
        rocketnet_spam( "client ignored cookie. wrong state" );
        counters[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE]++;
        return 0;
    }

    if ( packet_bytes != ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored cookie. wrong size" );
        counters[ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH]++;
        return 0;
    }

    // the echoed salt must match this attempt, or the packet is an off-path
    // injection and is discarded -- the standard's rule, and the reason the
    // salt rides in the clear

    if ( sodium_memcmp( packet_data + ROCKETNET_PREFILTER_BYTES, client->connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ) != 0 )
    {
        rocketnet_spam( "client ignored cookie. salt does not match this attempt" );
        counters[ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH]++;
        return 0;
    }

    // store the last 24 bytes -- expire and mac -- and carry them in every
    // subsequent request. a fresh cookie replaces a stale one. nothing is
    // sent because this arrived; the next scheduled request carries it.

    memcpy( client->return_cookie, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES, ROCKETNET_COOKIE_FIELD_BYTES );
    client->has_cookie = 1;
    client->last_receive_time = current_time;

    counters[ROCKETNET_COUNTER_CLIENT_COOKIE_STORED]++;
    rocketnet_debug( "client stored cookie" );

    if ( client->state == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE )
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );

    return 1;
}

static int rocketnet_client_process_challenge( struct rocketnet_client_protocol_t * client,
                                               const uint8_t * packet_data,
                                               int packet_bytes,
                                               double current_time )
{
    uint64_t * counters = client->counters->values;

    counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED]++;

    // processed in sending connection request only: past it the client no
    // longer sends requests, so a genuine retransmitted challenge has no
    // question left to answer

    if ( client->state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST )
    {
        rocketnet_spam( "client ignored challenge. wrong state" );
        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_STATE]++;
        return 0;
    }

    if ( packet_bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored challenge. wrong size" );
        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN]++;
        return 0;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->replay_handshake.highest_opened );
    if ( sequence == 0 )
    {
        rocketnet_spam( "client ignored challenge. zero sequence" );
        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_ZERO_SEQUENCE]++;
        return 0;
    }

    // derive the handshake keys from the carried nonce into temporaries,
    // committed only after the open succeeds: a forged nonce must never
    // overwrite anything

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t temporary_keys;
    rocketnet_derive_handshake_keys( &temporary_keys,
                                     client->token.client_to_server_key,
                                     client->token.server_to_client_key,
                                     client->connection_salt, session_nonce );

    uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES];
    uint8_t nonce_out[ROCKETNET_SESSION_NONCE_BYTES];
    int opened_bytes = rocketnet_open_sealed_packet( server_ephemeral_public_key, nonce_out,
                                                     packet_data, packet_bytes,
                                                     ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     client->client_ephemeral_public_key,
                                                     temporary_keys.handshake_server_to_client );
    if ( opened_bytes != ROCKETNET_KEY_BYTES )
    {
        sodium_memzero( &temporary_keys, sizeof(temporary_keys) );
        rocketnet_spam( "client ignored challenge. did not open" );
        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN]++;
        return 0;
    }

    // the full derivation, refusing a degenerate server ephemeral outright.
    // the attempt then ends at its deadline, the correct outcome for a peer
    // offering a small order point.

    if ( !rocketnet_derive_connection_keys( &client->keys,
                                            client->token.client_to_server_key,
                                            client->token.server_to_client_key,
                                            client->connection_salt, session_nonce,
                                            client->client_ephemeral_secret_key, server_ephemeral_public_key,
                                            client->client_ephemeral_public_key, server_ephemeral_public_key ) )
    {
        sodium_memzero( &temporary_keys, sizeof(temporary_keys) );
        rocketnet_debug( "client ignored challenge. server ephemeral key is a small order point" );
        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_SMALL_ORDER_KEY]++;
        return 0;
    }

    sodium_memzero( &temporary_keys, sizeof(temporary_keys) );

    // commit: the nonce, the keys, the erasure of the ephemeral secret --
    // from this moment the client holds only the four derived keys, exactly
    // as the server does

    memcpy( client->session_nonce, session_nonce, ROCKETNET_SESSION_NONCE_BYTES );
    sodium_memzero( client->client_ephemeral_secret_key, sizeof(client->client_ephemeral_secret_key) );

    if ( sequence > client->replay_handshake.highest_opened )
        client->replay_handshake.highest_opened = sequence;
    client->last_receive_time = current_time;

    // the response is built once and retransmitted byte-identical: same key,
    // same sequence, same bytes -- one ciphertext, never a second encryption

    const uint8_t response_padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    int response_bytes = rocketnet_write_sealed_packet( client->stored_response, ROCKETNET_CONNECTION_RESPONSE_PACKET,
                                                        client->title_tag, client->send_sequence_handshake,
                                                        NULL, response_padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                        client->signed_fields.protocol_id, client->connection_salt,
                                                        client->client_ephemeral_public_key,
                                                        client->keys.handshake_client_to_server );
    rocketnet_assert( response_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    (void) response_bytes;
    client->send_sequence_handshake++;

    counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED]++;
    rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE );

    return 1;
}

static int rocketnet_client_process_denied( struct rocketnet_client_protocol_t * client,
                                            const uint8_t * packet_data,
                                            int packet_bytes,
                                            double current_time )
{
    uint64_t * counters = client->counters->values;

    (void) current_time;

    counters[ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED]++;

    if ( client->state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST &&
         client->state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        rocketnet_spam( "client ignored denied. wrong state" );
        counters[ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_STATE]++;
        return 0;
    }

    if ( packet_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored denied. wrong size" );
        counters[ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN]++;
        return 0;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->replay_handshake.highest_opened );
    if ( sequence == 0 )
    {
        rocketnet_spam( "client ignored denied. zero sequence" );
        counters[ROCKETNET_COUNTER_CLIENT_DENIED_ZERO_SEQUENCE]++;
        return 0;
    }

    // a client that holds handshake keys opens with them and ignores the
    // carried nonce; one that does not derives temporaries from it. either
    // way an off-path attacker cannot deny: without the token keys, nothing
    // it sends opens.

    uint8_t opened[ROCKETNET_CONNECTION_DENIED_WIRE_BYTES];
    uint8_t nonce_out[ROCKETNET_SESSION_NONCE_BYTES];
    int opened_bytes;

    if ( client->state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        opened_bytes = rocketnet_open_sealed_packet( opened, nonce_out, packet_data, packet_bytes,
                                                     ROCKETNET_CONNECTION_DENIED_PACKET, sequence,
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     NULL, client->keys.handshake_server_to_client );
    }
    else
    {
        uint8_t carried_nonce[ROCKETNET_SESSION_NONCE_BYTES];
        memcpy( carried_nonce, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );
        struct rocketnet_connection_keys_t temporary_keys;
        rocketnet_derive_handshake_keys( &temporary_keys,
                                         client->token.client_to_server_key,
                                         client->token.server_to_client_key,
                                         client->connection_salt, carried_nonce );
        opened_bytes = rocketnet_open_sealed_packet( opened, nonce_out, packet_data, packet_bytes,
                                                     ROCKETNET_CONNECTION_DENIED_PACKET, sequence,
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     NULL, temporary_keys.handshake_server_to_client );
        sodium_memzero( &temporary_keys, sizeof(temporary_keys) );
    }

    if ( opened_bytes != 0 )
    {
        rocketnet_spam( "client ignored denied. did not open" );
        counters[ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN]++;
        return 0;
    }

    counters[ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED]++;
    rocketnet_debug( "client connection denied by server" );
    rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );

    return 1;
}

// the session types share one shape: state gate, sequence, replay check,
// open, replay advance -- then their own action

struct rocketnet_client_session_counters_t
{
    int received;
    int wrong_state;
    int zero_sequence;
    int already_received;
    int did_not_open;
};

static int rocketnet_client_open_session_packet( struct rocketnet_client_protocol_t * client,
                                                 uint8_t packet_type,
                                                 const struct rocketnet_client_session_counters_t * type_counters,
                                                 const uint8_t * packet_data,
                                                 int packet_bytes,
                                                 double current_time,
                                                 uint8_t * opened,
                                                 uint64_t * out_sequence )
{
    uint64_t * counters = client->counters->values;

    counters[type_counters->received]++;

    // session traffic exists only from sending connection response on: the
    // keys that open it are born at the challenge

    if ( client->state != ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE &&
         client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        rocketnet_spam( "client ignored session packet. wrong state" );
        counters[type_counters->wrong_state]++;
        return -1;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->replay_session.highest_opened );
    if ( sequence == 0 )
    {
        rocketnet_spam( "client ignored session packet. zero sequence" );
        counters[type_counters->zero_sequence]++;
        return -1;
    }

    if ( rocketnet_replay_already_received( &client->replay_session, sequence ) )
    {
        rocketnet_spam( "client ignored session packet. already received" );
        counters[type_counters->already_received]++;
        return -1;
    }

    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, packet_data, packet_bytes, packet_type,
                                                     sequence, client->signed_fields.protocol_id,
                                                     client->connection_salt, NULL,
                                                     client->keys.session_server_to_client );
    if ( opened_bytes < 0 )
    {
        rocketnet_spam( "client ignored session packet. did not open" );
        counters[type_counters->did_not_open]++;
        return -1;
    }

    rocketnet_replay_advance( &client->replay_session, sequence );
    client->last_receive_time = current_time;
    *out_sequence = sequence;

    return opened_bytes;
}

static int rocketnet_client_process_keep_alive( struct rocketnet_client_protocol_t * client,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN };

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint64_t sequence = 0;
    int opened_bytes = rocketnet_client_open_session_packet( client, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                             &type_counters, packet_data, packet_bytes,
                                                             current_time, opened, &sequence );
    if ( opened_bytes < 0 )
        return 0;

    if ( opened_bytes != 8 )
    {
        rocketnet_spam( "client ignored keep alive. wrong size" );
        client->counters->values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN]++;
        return 0;
    }

    const uint8_t * p = opened;
    int client_index = (int) rocketnet_read_uint32( &p );
    int max_clients = (int) rocketnet_read_uint32( &p );

    client->counters->values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED]++;
    rocketnet_spam( "client received keep alive" );

    // the first keep-alive is how the client learns it is connected, and
    // which slot it holds

    if ( client->state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        client->client_index = client_index;
        client->max_clients = max_clients;
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_CONNECTED );
    }

    return 1;
}

static int rocketnet_client_process_payload( struct rocketnet_client_protocol_t * client,
                                             const uint8_t * packet_data,
                                             int packet_bytes,
                                             double current_time,
                                             uint8_t * out_payload,
                                             int * out_payload_bytes )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN };

    // payloads are for an established connection; one arriving in sending
    // connection response means the first keep-alive is still in flight, and
    // the periodic keep-alives repair that -- refuse it as wrong state
    // without touching the replay window

    if ( client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        client->counters->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED]++;
        rocketnet_spam( "client ignored payload. wrong state" );
        client->counters->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE]++;
        return 0;
    }

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint64_t sequence = 0;
    int opened_bytes = rocketnet_client_open_session_packet( client, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                             &type_counters, packet_data, packet_bytes,
                                                             current_time, opened, &sequence );
    if ( opened_bytes < 0 )
        return 0;

    if ( opened_bytes < 1 )
    {
        rocketnet_spam( "client ignored payload. empty" );
        client->counters->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN]++;
        return 0;
    }

    memcpy( out_payload, opened, (size_t) opened_bytes );
    *out_payload_bytes = opened_bytes;

    client->counters->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED]++;
    rocketnet_spam( "client received payload (%d bytes)", opened_bytes );

    return 1;
}

static int rocketnet_client_process_disconnect( struct rocketnet_client_protocol_t * client,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN };

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint64_t sequence = 0;
    int opened_bytes = rocketnet_client_open_session_packet( client, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                             &type_counters, packet_data, packet_bytes,
                                                             current_time, opened, &sequence );
    if ( opened_bytes < 0 )
        return 0;

    if ( opened_bytes != 0 )
    {
        rocketnet_spam( "client ignored disconnect. wrong size" );
        client->counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN]++;
        return 0;
    }

    client->counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED]++;
    rocketnet_debug( "client disconnected by server" );
    rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_DISCONNECTED );

    return 1;
}

int rocketnet_client_process_packet( struct rocketnet_client_protocol_t * client,
                                     const uint8_t * packet_data,
                                     int packet_bytes,
                                     double current_time,
                                     uint8_t * out_payload,
                                     int * out_payload_bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( packet_data );
    rocketnet_assert( out_payload );
    rocketnet_assert( out_payload_bytes );

    uint64_t * counters = client->counters->values;

    *out_payload_bytes = 0;

    counters[ROCKETNET_COUNTER_PACKETS_RECEIVED]++;

    int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, client->title_tag );
    if ( packet_type < 0 )
    {
        rocketnet_spam( "client dropped packet. prefilter" );
        counters[ROCKETNET_COUNTER_PREFILTER_DROPPED]++;
        return 0;
    }

    if ( packet_type > ROCKETNET_CONNECTION_COOKIE_PACKET )
    {
        rocketnet_spam( "client dropped packet. reserved type" );
        counters[ROCKETNET_COUNTER_RESERVED_TYPE]++;
        return 0;
    }

    // a client never processes what only a client sends -- request,
    // response, and its own session sends arriving back are junk

    if ( packet_type == ROCKETNET_CONNECTION_REQUEST_PACKET ||
         packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
    {
        rocketnet_spam( "client dropped packet. wrong direction" );
        counters[ROCKETNET_COUNTER_WRONG_DIRECTION]++;
        return 0;
    }

    switch ( packet_type )
    {
        case ROCKETNET_CONNECTION_COOKIE_PACKET:
            return rocketnet_client_process_cookie( client, packet_data, packet_bytes, current_time );
        case ROCKETNET_CONNECTION_CHALLENGE_PACKET:
            return rocketnet_client_process_challenge( client, packet_data, packet_bytes, current_time );
        case ROCKETNET_CONNECTION_DENIED_PACKET:
            return rocketnet_client_process_denied( client, packet_data, packet_bytes, current_time );
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET:
            return rocketnet_client_process_keep_alive( client, packet_data, packet_bytes, current_time );
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:
            return rocketnet_client_process_payload( client, packet_data, packet_bytes, current_time, out_payload, out_payload_bytes );
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET:
            return rocketnet_client_process_disconnect( client, packet_data, packet_bytes, current_time );
    }

    return 0;
}

int rocketnet_client_write_payload( struct rocketnet_client_protocol_t * client,
                                    const uint8_t * payload_data,
                                    int payload_bytes,
                                    double current_time,
                                    uint8_t * out_packet )
{
    rocketnet_assert( client );
    rocketnet_assert( payload_data );
    rocketnet_assert( out_packet );

    if ( client->state != ROCKETNET_CLIENT_STATE_CONNECTED )
        return 0;

    client->last_send_time = current_time;

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_RECEIVE_CEILING - ROCKETNET_PACKET_OVERHEAD_BYTES )
        return 0;

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                               client->title_tag, client->send_sequence_session,
                                               NULL, payload_data, payload_bytes,
                                               client->signed_fields.protocol_id, client->connection_salt,
                                               NULL, client->keys.session_client_to_server );
    if ( bytes == 0 )
        return 0;

    client->send_sequence_session++;
    rocketnet_spam( "client sent payload (%d bytes)", payload_bytes );

    return bytes;
}

void rocketnet_client_disconnect( struct rocketnet_client_protocol_t * client,
                                  uint8_t * out_packets,
                                  int * out_packet_count )
{
    rocketnet_assert( client );
    rocketnet_assert( out_packets );
    rocketnet_assert( out_packet_count );

    *out_packet_count = 0;

    // before the session keys exist there is nothing to seal: the state
    // simply resets, and the server's half-open mapping dies at its deadline

    if ( client->state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE ||
         client->state == ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        for ( int i = 0; i < ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS; i++ )
        {
            int bytes = rocketnet_write_sealed_packet( out_packets + i * ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES,
                                                       ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                       client->title_tag, client->send_sequence_session,
                                                       NULL, NULL, 0,
                                                       client->signed_fields.protocol_id, client->connection_salt,
                                                       NULL, client->keys.session_client_to_server );
            rocketnet_assert( bytes == ROCKETNET_CONNECTION_DISCONNECT_WIRE_BYTES );
            (void) bytes;
            client->send_sequence_session++;
        }
        *out_packet_count = ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS;
        rocketnet_debug( "client sent %d redundant disconnect packets", ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    }

    if ( client->state != ROCKETNET_CLIENT_STATE_DISCONNECTED )
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_STATE_DISCONNECTED );

    sodium_memzero( &client->keys, sizeof(client->keys) );
    sodium_memzero( client->client_ephemeral_secret_key, sizeof(client->client_ephemeral_secret_key) );
}
