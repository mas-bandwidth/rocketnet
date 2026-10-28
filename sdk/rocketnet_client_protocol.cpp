/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_client_protocol.h"
#include "rocketnet_platform.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet_internal.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_memory.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_seams.h"

#include <sodium.h>
#include <string.h>

void rocketnet_client_protocol_pick_server_address( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                    bool ipv6_available,
                                                    struct rocketnet_address_t * address );

const char * rocketnet_client_transport_state_name( int state )
{
    switch ( state )
    {
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED:         return "connect token expired";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN:         return "invalid connect token";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT:          return "connection timed out";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT: return "connection response timed out";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT:  return "connection request timed out";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT:      return "cookie request timed out";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED:             return "connection denied";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED:                  return "disconnected";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE:             return "requesting cookie";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST:    return "sending connection request";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE:   return "sending connection response";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED:                     return "connected";
        default:
            rocketnet_assert( 0 );
            return "???";
    }
}

// tla/RNClientIoDecrypt.tla RetireKeys: a terminal state runs the connection
// reset, whose retire_keys hook retires the I/O thread's open copy.

static void rocketnet_client_set_state( struct rocketnet_client_protocol_t * client, int state )
{
    if ( client->state == state )
        return;

    rocketnet_info( "client changed state from '%s' to '%s'", rocketnet_client_transport_state_name( client->state ), rocketnet_client_transport_state_name( state ) );

    client->state = state;

    // A terminal state ends the attempt: the derived session keys, the
    // token's keys and the ephemeral secret are no longer needed and MUST be
    // zeroed. TRANSPORT.md t7 rule 10: both sides MUST erase their own
    // ephemeral secret when no handshake that needs it remains, and MUST
    // erase the four keys and the token keys when the attempt that owns them
    // ends; t13.4 says the same for a client reset.
    // tla/ClientConnection.tla's KeysErasedWhenDown pins cKeys = NoKeys at
    // state <= 0. Every automatic terminal transition passes through here,
    // after the state is stored, so the reason, the state-change report and
    // any packets already written keep their order.

    if ( state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        rocketnet_client_connection_reset( client );
}

// The AEAD integrity limit, per TRANSPORT.md t14 Replay protection: one
// more sealed packet that did not open this attempt, across its handshake
// and session keys. On reaching ROCKETNET_INTEGRITY_LIMIT the attempt ends
// in disconnected with its keys and ephemeral secret zeroed -- the zeroing
// rocketnet_client_protocol_disconnect does at the end of every attempt,
// done here at once rather than left to the caller -- and
// INTEGRITY_LIMIT_REACHED emits once. Disconnected is terminal, so nothing
// counts past it. This is the read path, so the check runs in every build.

static void rocketnet_client_integrity_failure( struct rocketnet_client_protocol_t * client )
{
    client->integrity_failures++;

    if ( client->integrity_failures < ROCKETNET_INTEGRITY_LIMIT )
        return;

    rocketnet_debug( "client disconnected. integrity limit reached" );

    client->counters->values[ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED]++;
    rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
}

void rocketnet_client_protocol_zero_state( struct rocketnet_client_protocol_t * client )
{
    typedef rocketnet_client_protocol_t T;

    const rocketnet_span_t skip[] = {
        { offsetof( T, ack ) + offsetof( rocketnet_ack_endpoint_t, sent ), sizeof( rocketnet_ack_endpoint_t ) - offsetof( rocketnet_ack_endpoint_t, sent ) },
        { offsetof( T, acked_packet_ids ), sizeof( ( (T *) 0 )->acked_packet_ids ) },
    };

    static_assert( offsetof( T, acked_packet_ids ) > offsetof( T, ack ), "the skipped spans are in their order in the struct" );

    rocketnet_zero_except( client, sizeof( T ), skip, (int) ( sizeof( skip ) / sizeof( skip[0] ) ) );
}

void rocketnet_client_protocol_init( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters )
{
    rocketnet_assert( client );
    rocketnet_assert( counters );

    rocketnet_client_protocol_zero_state( client );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( client->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    client->send_rate = 10.0;
    client->counters = counters;
    client->state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
    client->client_index = -1;
    client->state_first_send_time = -1.0;
    client->next_fragment_payload_id = 1;
    rocketnet_ack_endpoint_reset( &client->ack );
}

// one rtt sample: first send in the current state to the first packet that
// answers it, folded as a running minimum. disarms until the next state's
// first send, so each handshake exchange contributes at most one sample.

static void rocketnet_client_rtt_sample( struct rocketnet_client_protocol_t * client, double current_time )
{
    if ( client->state_first_send_time < 0.0 )
        return;

    const double sample = current_time - client->state_first_send_time;

    client->state_first_send_time = -1.0;

    if ( sample < 0.0 )
        return;

    if ( client->rtt_estimate == 0.0 || sample < client->rtt_estimate )
        client->rtt_estimate = sample;
}

// the server address an accepted token selects for the attempt: the IPv6
// address when the token flags one and the host can open an IPv6 socket, else
// the IPv4 address when the token flags one, else the IPv6 address (which the
// transport refuses on a host with no IPv6). A pure function of its
// arguments (tla/ServerConnection.tla PickFamily).

void rocketnet_client_protocol_pick_server_address( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                    bool ipv6_available,
                                                    struct rocketnet_address_t * address )
{
    rocketnet_assert( signed_fields );
    rocketnet_assert( address );

    const uint8_t flags = signed_fields->address_flags;
    const bool use_ipv6 = ( flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) != 0 &&
                          ( ipv6_available || ( flags & ROCKETNET_ADDRESS_FLAG_IPV4 ) == 0 );

    memset( address, 0, sizeof( *address ) );

    if ( use_ipv6 )
    {
        address->type = ROCKETNET_ADDRESS_IPV6;

        const uint8_t * p = signed_fields->ipv6_address;

        for ( int i = 0; i < 8; i++ )
            address->data.ipv6[i] = rocketnet_read_uint16( &p );

        address->port = rocketnet_read_uint16( &p );
    }
    else
    {
        address->type = ROCKETNET_ADDRESS_IPV4;

        const uint8_t * p = signed_fields->ipv4_address;
        rocketnet_read_bytes( &p, address->data.ipv4, 4 );

        address->port = rocketnet_read_uint16( &p );
    }
}

// tla/ClientConnection.tla AppConnect; FreshAttemptFromFailure: a connect
// from any failure state carries nothing of the earlier attempt
// tla/ClientConnection.tla ClientCommitEarly: the reversed witness's commit to
// a family of its own choosing before any answer; the fixed client commits only
// on the first accepted answer.

int rocketnet_client_protocol_connect( struct rocketnet_client_protocol_t * client,
                                       const uint8_t * connect_token,
                                       double current_time,
                                       uint64_t current_timestamp )
{
    rocketnet_assert( client );
    rocketnet_assert( connect_token );
    rocketnet_assert( client->counters );

    // tla/ClientConnection.tla AppConnect permits only disconnected or failure states

    if ( client->state > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        return 0;

    // an accepted connect begins a fresh attempt from the start: fresh salt, fresh ephemeral, sequences at one. recovery is this same path.

    rocketnet_client_connection_reset( client );

    client->state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;

    rocketnet_read_connect_token( &client->token, connect_token );

    rocketnet_read_signed_connect_data( &client->signed_fields, client->token.signed_connect_data );

    // the pre-send token checks, from the standard (TRANSPORT.md t11 rule 2).
    // a timeout of zero or below is invalid at both ends: the server ignores
    // such a request, and a client that accepted the token would never time
    // out (the deadline check below reads only a positive timeout)

    uint8_t flags = client->signed_fields.address_flags;
    if ( flags == 0 || ( flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) != 0 ||
         client->signed_fields.create_timestamp > client->signed_fields.expire_timestamp ||
         client->signed_fields.timeout_seconds <= 0 ||
         client->signed_fields.version != ROCKETNET_WIRE_VERSION )
    {
        rocketnet_debug( "client connect failed. invalid connect token" );

        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN );
        return 0;
    }

    if ( client->signed_fields.expire_timestamp <= current_timestamp )
    {
        rocketnet_debug( "client connect failed. connect token expired" );

        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
        return 0;
    }

    // the server address, IPv6 preferred when both families are present.
    // the transport selects again with the host's IPv6 capability before it
    // binds (rocketnet_client_transport_create_internal).

    rocketnet_client_protocol_pick_server_address( &client->signed_fields, true, &client->server_address );

    // one salt and one ephemeral keypair per attempt, sequences at one and never reset within it

    randombytes_buf( client->connection_salt, sizeof( client->connection_salt ) );

    ROCKETNET_CRYPTO_TALLY( keypair );

    crypto_box_keypair( client->client_ephemeral_public_key, client->client_ephemeral_secret_key );

    client->send_sequence_handshake = 1;
#if ROCKETNET_DEV
    client->send_sequence_session = rocketnet_dev_session_sequence_start();     // 1 unless the DEV seam moves it
#else // #if ROCKETNET_DEV
    client->send_sequence_session = 1;
#endif // #if ROCKETNET_DEV

    client->attempt_start_time = current_time;
    client->last_receive_time = current_time;
    client->last_send_time = current_time - 1000.0;         // the first update sends immediately
    client->total_deadline_seconds = client->signed_fields.expire_timestamp - client->signed_fields.create_timestamp;
    client->timeout_seconds = client->signed_fields.timeout_seconds;

    rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    return 1;
}

// deadlines, in the standard's shape: each waiting state N dies to -(N+1)
// after the token's timeout with nothing received, and the whole handshake
// dies to connect token expired once the token's lifetime has elapsed

// tla/ClientConnection.tla Tick

static void rocketnet_client_check_deadlines( struct rocketnet_client_protocol_t * client, double current_time )
{
    if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        return;

    if ( client->state < ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
         current_time - client->attempt_start_time >= (double) client->total_deadline_seconds )
    {
        rocketnet_debug( "client connect failed. connect token expired during handshake" );

        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
        return;
    }

    if ( client->timeout_seconds > 0 &&
         current_time - client->last_receive_time >= (double) client->timeout_seconds )
    {
        int timed_out_state = -( client->state + 1 );

        rocketnet_debug( "client timed out in state '%s'", rocketnet_client_transport_state_name( client->state ) );

        rocketnet_client_set_state( client, timed_out_state );
    }
}

// Builds one sealed session packet: the sealed plaintext opens with the ack
// block built from this connection's receive state, and the per-packet type
// data follows it. Every session send passes through here or through the
// payload's in-place twin, so sending any packet sends a block, and sending
// a block resets the ack debt. The sent packet is recorded in the delivery
// bookkeeping -- these sequences carry no packet id and surface nothing.

static int rocketnet_client_write_session_packet( struct rocketnet_client_protocol_t * client,
                                                  uint8_t packet_type,
                                                  const uint8_t * type_data,
                                                  int type_data_bytes,
                                                  double current_time,
                                                  uint8_t * out_packet )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 8];

    rocketnet_assert( type_data_bytes <= 8 );

    // the block truncates to the budget's remaining room -- the capacity
    // arithmetic is what keeps the packet inside the send budget, whatever
    // the replay window's depth

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - type_data_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;

    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &client->accepted_session );

    if ( type_data_bytes > 0 )
        memcpy( plaintext + block_bytes, type_data, (size_t) type_data_bytes );

    uint64_t sequence = client->send_sequence_session;

    int bytes = rocketnet_write_sealed_packet( out_packet, packet_type,
                                               client->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + type_data_bytes,
                                               client->signed_fields.protocol_id, client->connection_salt,
                                               NULL, client->keys.session_client_to_server );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );

    client->send_sequence_session++;

    rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( &client->ack, sequence, bytes, 0, 0, 0, current_time ),
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );

    client->ack.ack_debt = 0;

    return bytes;
}

// tla/ClientConnection.tla ClientSend

void rocketnet_client_protocol_update( struct rocketnet_client_protocol_t * client,
                                       double current_time,
                                       uint8_t * out_packet,
                                       int * out_packet_bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( out_packet );
    rocketnet_assert( out_packet_bytes );

    *out_packet_bytes = 0;

    rocketnet_client_check_deadlines( client, current_time );

    if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        return;

    // tla/RNEndpoint.tla WriteAck: sends a packet that carries only the owed acks.
    // An owed ack goes out on this update, outside the scheduled send rate.
    // Type 8 does not add debt, so the ack cannot ping-pong.

    if ( current_time - client->last_send_time < 1.0 / client->send_rate )
    {
        if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             client->ack.ack_debt >= ROCKETNET_ACK_DEBT_THRESHOLD )
        {
            int bytes = rocketnet_client_write_session_packet( client, ROCKETNET_CONNECTION_ACK_PACKET,
                                                               NULL, 0, current_time, out_packet );
            if ( bytes > 0 )
            {
                *out_packet_bytes = bytes;

                rocketnet_spam( "client sent standalone ack" );
            }
        }

        return;
    }

    client->last_send_time = current_time;

    if ( client->state_first_send_time < 0.0 )
        client->state_first_send_time = current_time;

    switch ( client->state )
    {
        case ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE:
        case ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST:
        {
            // the kept cookie goes out on at least every second request: a
            // later cookie alternates with it and never displaces it, so a
            // forged cookie cannot take the kept one off the wire
            // (tla/ClientConnection.tla ClientPacket, HandshakeNeverRewinds)

            const uint8_t * return_cookie = client->has_cookie ? client->return_cookie : NULL;

            if ( client->has_latest_cookie )
            {
                if ( client->send_latest_cookie )
                    return_cookie = client->latest_cookie;

                client->send_latest_cookie = !client->send_latest_cookie;
            }

            rocketnet_write_connection_request_packet( out_packet, client->packet_filter,
                                                       return_cookie,
                                                       client->connection_salt,
                                                       client->client_ephemeral_public_key,
                                                       client->token.signed_connect_data,
                                                       client->token.signature );

            *out_packet_bytes = ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES;

            rocketnet_spam( "client sent connection request" );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE:
        {
            // the stored response, byte-identical every retransmission
            memcpy( out_packet, client->stored_response, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

            *out_packet_bytes = ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES;

            rocketnet_spam( "client sent connection response" );
            break;
        }

        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED:
        {
            uint8_t keep_alive_data[8];
            uint8_t * p = keep_alive_data;
            rocketnet_write_uint32( &p, (uint32_t) client->client_index );
            rocketnet_write_uint32( &p, (uint32_t) client->max_clients );

            int bytes = rocketnet_client_write_session_packet( client, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                               keep_alive_data, 8, current_time, out_packet );
            rocketnet_assert( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

            *out_packet_bytes = bytes;

            rocketnet_spam( "client sent keep alive" );
            break;
        }
    }
}

// the client-side per-type processing: state gate first, then the mechanics,
// each in its own early-return function

// tla/ClientConnection.tla ClientRecvCookie

static int rocketnet_client_process_cookie( struct rocketnet_client_protocol_t * client,
                                            const uint8_t * packet_data,
                                            int packet_bytes )
{
    uint64_t * counters = client->counters->values;

    counters[ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED]++;

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE &&
         client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
    {
        rocketnet_spam( "client ignored cookie. wrong state" );

        counters[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE]++;
        return 0;
    }

    if ( packet_bytes != ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored cookie. wrong size" );

        counters[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE]++;
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

    // the client trusts only the server: the salt rides in the clear, so
    // anyone who sees the request can echo it, and the client cannot check
    // the mac. a cookie therefore never refreshes the receive time and never
    // gives a round-trip sample; both come only from the challenge and later
    // packets that open under the attempt's keys (standard/TRANSPORT.md
    // t11.1 Cookie processing).
    //
    // for the same reason a cookie never displaces the one the attempt keeps.
    // the first accepted cookie is kept for the attempt; a later one replaces
    // only the latest cookie, which alternates with the kept one on the wire.
    // a forged cookie can cost a request, never the kept cookie: the handshake
    // moves backwards on no packet that does not open under the attempt's
    // keys (tla/ClientConnection.tla HandshakeNeverRewinds). a genuine fresh
    // cookie, after the kept one expired, still goes out on every second
    // request. nothing is sent because this arrived.

    const uint8_t * cookie_field = packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES;

    if ( !client->has_cookie )
    {
        memcpy( client->return_cookie, cookie_field, ROCKETNET_COOKIE_FIELD_BYTES );
        client->has_cookie = 1;
    }
    else
    {
        memcpy( client->latest_cookie, cookie_field, ROCKETNET_COOKIE_FIELD_BYTES );
        client->has_latest_cookie = 1;
    }

    counters[ROCKETNET_COUNTER_CLIENT_COOKIE_STORED]++;

    rocketnet_debug( "client stored cookie" );

    // leaving requesting cookie disarms the round-trip stamp without a
    // sample, so the challenge samples from the first request of sending
    // connection request, never from the first request of the attempt

    if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE )
    {
        client->state_first_send_time = -1.0;
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    }

    return 1;
}

// tla/ClientConnection.tla ClientRecvChallenge

static int rocketnet_client_process_challenge( struct rocketnet_client_protocol_t * client,
                                               const uint8_t * packet_data,
                                               int packet_bytes,
                                               double current_time )
{
    uint64_t * counters = client->counters->values;

    counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED]++;

    // TRANSPORT.md t10 step 6: the per-type size check runs before the state gate

    if ( packet_bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored challenge. wrong size" );

        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_SIZE]++;
        return 0;
    }

    // processed in sending connection request only: past it the client no longer sends requests, so a genuine retransmitted challenge has no question left to answer

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
    {
        rocketnet_spam( "client ignored challenge. wrong state" );

        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_WRONG_STATE]++;
        return 0;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->handshake_highest_opened );
    if ( sequence == 0 )
    {
        rocketnet_spam( "client ignored challenge. zero sequence" );

        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_ZERO_SEQUENCE]++;
        return 0;
    }

    // derive the handshake keys from the carried nonce into temporaries, committed only after the open succeeds: a forged nonce must never overwrite anything

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memcpy( session_nonce, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    struct rocketnet_connection_keys_t temporary_keys;
    rocketnet_derive_handshake_keys( &temporary_keys,
                                     client->token.client_to_server_key,
                                     client->token.server_to_client_key,
                                     client->connection_salt, session_nonce );

    uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES];
    uint8_t nonce_out[ROCKETNET_SESSION_NONCE_BYTES];
    int opened_bytes = rocketnet_open_sealed_packet( server_ephemeral_public_key, nonce_out, ROCKETNET_KEY_BYTES,
                                                     packet_data, packet_bytes,
                                                     ROCKETNET_CONNECTION_CHALLENGE_PACKET, sequence,
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     client->client_ephemeral_public_key,
                                                     temporary_keys.handshake_server_to_client );
    if ( opened_bytes != ROCKETNET_KEY_BYTES )
    {
        sodium_memzero( &temporary_keys, sizeof( temporary_keys ) );

        rocketnet_spam( "client ignored challenge. did not open" );

        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_DID_NOT_OPEN]++;

        rocketnet_client_integrity_failure( client );
        return 0;
    }

    // the full derivation, refusing a degenerate server ephemeral outright.
    // the attempt then ends at its deadline, the correct outcome for a server
    // offering a small order point.

    if ( !rocketnet_derive_connection_keys( &client->keys,
                                            client->token.client_to_server_key,
                                            client->token.server_to_client_key,
                                            client->connection_salt, session_nonce,
                                            client->client_ephemeral_secret_key, server_ephemeral_public_key,
                                            client->client_ephemeral_public_key, server_ephemeral_public_key ) )
    {
        sodium_memzero( &temporary_keys, sizeof( temporary_keys ) );

        rocketnet_debug( "client ignored challenge. server ephemeral key is a small order point" );

        counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_SMALL_ORDER_KEY]++;
        return 0;
    }

    sodium_memzero( &temporary_keys, sizeof( temporary_keys ) );

    // commit: the nonce, the keys, the erasure of the ephemeral secret -- from this moment the client holds only the four derived keys, exactly as the server does

    memcpy( client->session_nonce, session_nonce, ROCKETNET_SESSION_NONCE_BYTES );

    sodium_memzero( client->client_ephemeral_secret_key, sizeof( client->client_ephemeral_secret_key ) );

    // tla/RNClientIoDecrypt.tla PublishKeys: an I/O thread's open copy is published here, before the connection response below is handed off

    if ( client->publish_keys )
        client->publish_keys( client->keys_context, client );

    if ( sequence > client->handshake_highest_opened )
        client->handshake_highest_opened = sequence;

    client->last_receive_time = current_time;

    // the response is built once and retransmitted byte-identical: same key, same sequence, same bytes -- one ciphertext, never a second encryption

    const uint8_t response_padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    int response_bytes = rocketnet_write_sealed_packet( client->stored_response, ROCKETNET_CONNECTION_RESPONSE_PACKET,
                                                        client->packet_filter, client->send_sequence_handshake,
                                                        NULL, response_padding, ROCKETNET_RESPONSE_PADDING_BYTES,
                                                        client->signed_fields.protocol_id, client->connection_salt,
                                                        client->client_ephemeral_public_key,
                                                        client->keys.handshake_client_to_server );
    rocketnet_assert( response_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );

    (void) response_bytes;

    client->send_sequence_handshake++;

    counters[ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED]++;

    rocketnet_client_rtt_sample( client, current_time );

    rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    return 1;
}

// tla/ClientConnection.tla ClientRecvDenied

static int rocketnet_client_process_denied( struct rocketnet_client_protocol_t * client,
                                            const uint8_t * packet_data,
                                            int packet_bytes,
                                            double current_time )
{
    uint64_t * counters = client->counters->values;

    (void) current_time;

    counters[ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED]++;

    // TRANSPORT.md t10 step 6: the per-type size check runs before the state gate

    if ( packet_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
    {
        rocketnet_spam( "client ignored denied. wrong size" );

        counters[ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_SIZE]++;
        return 0;
    }

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST &&
         client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        rocketnet_spam( "client ignored denied. wrong state" );

        counters[ROCKETNET_COUNTER_CLIENT_DENIED_WRONG_STATE]++;
        return 0;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->handshake_highest_opened );
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

    if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        opened_bytes = rocketnet_open_sealed_packet( opened, nonce_out, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,
                                                     packet_data, packet_bytes,
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

        opened_bytes = rocketnet_open_sealed_packet( opened, nonce_out, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,
                                                     packet_data, packet_bytes,
                                                     ROCKETNET_CONNECTION_DENIED_PACKET, sequence,
                                                     client->signed_fields.protocol_id, client->connection_salt,
                                                     NULL, temporary_keys.handshake_server_to_client );

        sodium_memzero( &temporary_keys, sizeof( temporary_keys ) );
    }

    if ( opened_bytes != 0 )
    {
        rocketnet_spam( "client ignored denied. did not open" );

        counters[ROCKETNET_COUNTER_CLIENT_DENIED_DID_NOT_OPEN]++;

        rocketnet_client_integrity_failure( client );
        return 0;
    }

    counters[ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED]++;

    rocketnet_debug( "client connection denied by server" );

    rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );

    return 1;
}

// the session types share one shape: size floor, state gate, sequence,
// replay check, open, replay advance, ack block -- then their own action

struct rocketnet_client_session_counters_t
{
    int received;
    int wrong_state;
    int wrong_size;
    int zero_sequence;
    int already_received;
    int did_not_open;
    int bad_ack_block;
    int wrong_opened_size;
};

// A type's own check on its type data, run after the shared ack-block and
// size checks and before any commit, per the reading order: a packet the
// type refuses here keeps only its replay-window mark. Returns 1 to accept,
// 0 to refuse (the validator counts its own reason). NULL when the size
// check is the whole check.

typedef int ( *rocketnet_client_type_data_validator_t )( struct rocketnet_client_protocol_t * client,
                                                         const uint8_t * type_data,
                                                         int type_data_bytes );

// The state rung of every session type, as netcode.io's client: a keep-alive,
// disconnect or standalone ack is processed from sending connection response
// on, where the session keys are born; a payload or a fragment only once
// connected. The keep-alive that connects the client can arrive after the
// server's first payloads, and a payload delivered before connected let the
// application reply on a connection that could not yet send. The server's
// resend carries a reliable message again after the keep-alive
// (tla/ClientConnection.tla PayloadBeforeKeepAlive).

static bool rocketnet_client_session_state_accepts( const struct rocketnet_client_protocol_t * client, uint8_t packet_type )
{
    if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return true;
    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
        return false;

    return packet_type != ROCKETNET_CONNECTION_PAYLOAD_PACKET && packet_type != ROCKETNET_CONNECTION_FRAGMENT_PACKET;
}

// tla/ClientConnection.tla ClientRecvSession and tla/RNClientIoDecrypt.tla
// UpdateConsume: one session packet through the reading order -- wire
// floor, state, sequence, replay, open, replay advance, ack block, the type's
// size and own check, then the commit. The packet is either raw (opened
// here) or a record the I/O thread already worked, whose verdict takes the
// open's place at the same rung with the same counter. On success the type
// data's offset into opened lands in out_type_data_offset and its length is
// returned; -1 means refused, the reason counted.

static int rocketnet_client_open_session( struct rocketnet_client_protocol_t * client,
                                          uint8_t packet_type,
                                          const struct rocketnet_client_session_counters_t * type_counters,
                                          const uint8_t * packet_data,
                                          int packet_bytes,
                                          double current_time,
                                          uint8_t * opened,
                                          int opened_capacity,
                                          int * out_type_data_offset,
                                          rocketnet_client_type_data_validator_t type_data_validator )
{
    uint64_t * counters = client->counters->values;
    const int verdict = client->record_verdict;

    counters[type_counters->received]++;

    if ( packet_bytes < rocketnet_session_min_wire_bytes( packet_type ) || verdict == ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE )
    {
        rocketnet_spam( "client ignored session packet. below the type's wire floor" );
        counters[type_counters->wrong_size]++;
        return -1;
    }

    // session traffic exists only from sending connection response on: the
    // keys that open it are born at the challenge

    if ( !rocketnet_client_session_state_accepts( client, packet_type ) )
    {
        rocketnet_spam( "client ignored session packet. wrong state" );
        counters[type_counters->wrong_state]++;
        return -1;
    }

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), client->replay_session.highest_opened );

    // a record the I/O thread refused takes this thread's own cheap rungs
    // first, so its counter matches the packet opened here

    if ( verdict != ROCKETNET_CLIENT_IO_VERDICT_OPENED )
    {
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
    }

    int opened_bytes = -1;

    switch ( verdict )
    {
        case ROCKETNET_CLIENT_IO_VERDICT_RAW:
            opened_bytes = rocketnet_open_sealed_packet( opened, NULL, opened_capacity, packet_data, packet_bytes, packet_type,
                                                         sequence, client->signed_fields.protocol_id,
                                                         client->connection_salt, NULL,
                                                         client->keys.session_server_to_client );
            if ( opened_bytes < 0 )
            {
                rocketnet_spam( "client ignored session packet. did not open" );
                counters[type_counters->did_not_open]++;
                rocketnet_client_integrity_failure( client );
                return -1;
            }
            break;

        case ROCKETNET_CLIENT_IO_VERDICT_ZERO_SEQUENCE:
            rocketnet_spam( "client ignored session packet. zero sequence" );
            counters[type_counters->zero_sequence]++;
            return -1;

        case ROCKETNET_CLIENT_IO_VERDICT_ALREADY_RECEIVED:
            rocketnet_spam( "client ignored session packet. already received" );
            counters[type_counters->already_received]++;
            return -1;

        case ROCKETNET_CLIENT_IO_VERDICT_DID_NOT_OPEN:
            rocketnet_spam( "client ignored session packet. did not open" );
            counters[type_counters->did_not_open]++;
            rocketnet_client_integrity_failure( client );
            return -1;

        case ROCKETNET_CLIENT_IO_VERDICT_OPENED:

            // the I/O thread opened it in place behind the wire header. A
            // sequence that disagrees with the record is counted did not
            // open, with no integrity failure: the bytes were authentic.

            if ( sequence != client->record_sequence )
            {
                rocketnet_spam( "client ignored session packet. sequence disagrees with the I/O thread" );
                counters[type_counters->did_not_open]++;
                return -1;
            }

            if ( rocketnet_replay_already_received( &client->replay_session, sequence ) )
            {
                rocketnet_spam( "client ignored session packet. already received" );
                counters[type_counters->already_received]++;
                return -1;
            }

            opened_bytes = packet_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES;
            if ( opened_bytes < 0 || opened_bytes > opened_capacity )
            {
                rocketnet_spam( "client ignored session packet. opened size exceeds buffer capacity" );
                counters[type_counters->wrong_size]++;
                return -1;
            }

            memcpy( opened, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, (size_t) opened_bytes );
            break;

        default:
            rocketnet_assert( 0 );
            return -1;
    }

    // mark received and advance the high-water mark first, per the reading
    // order: no malformed or mis-sized packet has any effect beyond its
    // replay-window mark

    rocketnet_replay_advance( &client->replay_session, sequence );

    // the ack block at the front of the opened data, read against this end's
    // own highest sent session sequence (tla/AckBlockWrap.tla ReadBlock): a
    // malformed block refuses the whole packet

    struct rocketnet_ack_block_t block;
    const int block_bytes = rocketnet_ack_block_read( &block, opened, opened_bytes, client->send_sequence_session - 1 );
    if ( block_bytes == 0 )
    {
        rocketnet_spam( "client ignored session packet. bad ack block" );
        counters[type_counters->bad_ack_block]++;
        return -1;
    }

    const int type_data_bytes = opened_bytes - block_bytes;
    if ( !rocketnet_session_type_data_fits( packet_type, type_data_bytes ) )
    {
        rocketnet_spam( "client ignored session packet. wrong opened size" );
        counters[type_counters->wrong_opened_size]++;
        return -1;
    }

    if ( type_data_validator && !type_data_validator( client, opened + block_bytes, type_data_bytes ) )
        return -1;

    // every check passed: only now does the packet keep the connection
    // alive, feed the trackers, apply its block and add to the ack debt

    client->last_receive_time = current_time;
    client->received_session_sequence = sequence;

    // the first session packet accepted before the connection is confirmed
    // answers the connection response: the third handshake round trip

    if ( !client->rtt_session_sampled && client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        rocketnet_client_rtt_sample( client, current_time );
        client->rtt_session_sampled = 1;
    }

    if ( rocketnet_session_ack_eligible_on_open( packet_type ) )
        rocketnet_replay_advance( &client->accepted_session, sequence );

    rocketnet_ack_endpoint_record_received( &client->ack, sequence, packet_bytes, current_time );

    rocketnet_ack_endpoint_process_block( &client->ack, &block, current_time,
                                          client->acked_packet_ids, ROCKETNET_SENT_PACKET_BUFFER_SIZE, &client->num_acked,
                                          &counters[ROCKETNET_COUNTER_CLIENT_ACK_UNKNOWN_SEQUENCE],
                                          &counters[ROCKETNET_COUNTER_CLIENT_ACKED_BATCH_FULL] );

    if ( rocketnet_session_elicits_ack( packet_type ) )
        client->ack.ack_debt++;

    *out_type_data_offset = block_bytes;

    return type_data_bytes;
}

// The keep-alive's eight type bytes name the client index and the slot
// count. Checked here, as the open's type-data validator, so a keep-alive
// whose index or count is invalid is refused before the open commits:
// per the reading order it then keeps only its replay-window mark and does
// not refresh the idle timeout or feed the trackers. The refusal keeps its
// existing counter.

static int rocketnet_client_validate_keep_alive( struct rocketnet_client_protocol_t * client,
                                                 const uint8_t * type_data,
                                                 int type_data_bytes )
{
    (void) type_data_bytes;         // the open sized the eight type bytes

    const uint8_t * p = type_data;
    int client_index = (int) rocketnet_read_uint32( &p );
    int max_clients = (int) rocketnet_read_uint32( &p );

    if ( client_index < 0 || client_index >= max_clients || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_spam( "client rejected keep-alive. invalid index" );

        client->counters->values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE]++;
        return 0;
    }

    return 1;
}

static int rocketnet_client_process_keep_alive( struct rocketnet_client_protocol_t * client,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE
    };

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = 0;
    int type_data_bytes = rocketnet_client_open_session( client, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                          &type_counters,
                                                          packet_data, packet_bytes,
                                                          current_time, opened, (int) sizeof( opened ), &type_data_offset,
                                                          rocketnet_client_validate_keep_alive );
    if ( type_data_bytes < 0 )
        return 0;

    client->counters->values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED]++;

    rocketnet_spam( "client received keep alive" );

    // the first keep-alive is how the client learns it is connected, and which slot it holds; the validator already accepted its index

    if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
    {
        const uint8_t * p = opened + type_data_offset;
        client->client_index = (int) rocketnet_read_uint32( &p );
        client->max_clients = (int) rocketnet_read_uint32( &p );
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
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
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE
    };

    // payloads are processed only once connected, as netcode.io's client:
    // see rocketnet_client_session_state_accepts

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = 0;
    int type_data_bytes = rocketnet_client_open_session( client, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                                          &type_counters,
                                                          packet_data, packet_bytes,
                                                          current_time, opened, (int) sizeof( opened ), &type_data_offset, NULL );
    if ( type_data_bytes < 0 )
        return 0;

    memcpy( out_payload, opened + type_data_offset, (size_t) type_data_bytes );

    *out_payload_bytes = type_data_bytes;

    if ( !client->defer_payload_acks )
        rocketnet_client_protocol_accept_payload( client, client->received_session_sequence );

    client->counters->values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED]++;

    rocketnet_spam( "client received payload (%d bytes)", type_data_bytes );

    return 1;
}

static int rocketnet_client_process_ack( struct rocketnet_client_protocol_t * client,
                                         const uint8_t * packet_data,
                                         int packet_bytes,
                                         double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_ACK_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_ACK_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_ACK_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_ACK_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_ACK_WRONG_OPENED_SIZE
    };

    // the standalone ack: the block is the packet. opened, replay-marked and reported like any session packet, and never eliciting one back.

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = 0;
    int type_data_bytes = rocketnet_client_open_session( client, ROCKETNET_CONNECTION_ACK_PACKET,
                                                          &type_counters,
                                                          packet_data, packet_bytes,
                                                          current_time, opened, (int) sizeof( opened ), &type_data_offset, NULL );
    if ( type_data_bytes < 0 )
        return 0;

    client->counters->values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED]++;

    rocketnet_spam( "client received standalone ack" );

    return 1;
}

// tla/ClientConnection.tla ClientFragmentReceive: a fragment is accepted only
// in connected, the one step that creates client reassembly state.

static int rocketnet_client_process_fragment( struct rocketnet_client_protocol_t * client,
                                              const uint8_t * packet_data,
                                              int packet_bytes,
                                              double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_FRAGMENT_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_OPENED_SIZE
    };

    // a fragment is accepted only on a connected slot, per TRANSPORT.md t16.2
    // and FragmentOnlyWhenConnected in
    // tla/ClientConnection.tla: in any other state it is refused here,
    // before the open and before any reassembly call. Payload types other
    // than 9 keep their states in the shared open below.

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        rocketnet_spam( "client ignored fragment. not connected" );

        client->counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED]++;
        client->counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_WRONG_STATE]++;
        return 0;
    }

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = 0;
    int type_data_bytes = rocketnet_client_open_session( client, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                                          &type_counters,
                                                          packet_data, packet_bytes,
                                                          current_time, opened, (int) sizeof( opened ), &type_data_offset, NULL );
    if ( type_data_bytes < 0 )
        return 0;

    if ( !client->reassembly )
    {
        // reassembly is created wherever fragments are processed; a client
        // built without it cannot reassemble, and the accepted count keeps
        // the arrival invariant honest in release
        rocketnet_assert( 0 );

        client->counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED]++;
        rocketnet_replay_advance( &client->accepted_session, client->received_session_sequence );

        client->ack.ack_debt++;
        return 1;
    }

    static const struct rocketnet_reassembly_counters_t fragment_counters = {
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_STALE,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_INVALID,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_COUNT_MISMATCH,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_SIZE_MISMATCH,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_DUPLICATE_INDEX,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED,
        ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL
    };

    const int accepted = rocketnet_reassembly_process_fragment( client->reassembly, 0,
                                                                opened + type_data_offset, type_data_bytes,
                                                                client->counters->values, &fragment_counters );

    // standard/TRANSPORT.md t15.3: a packet is ack-eligible only once its
    // type data passes validation, and a fragment's validation is
    // reassembly's. so the fragment's sequence is marked in accepted_session,
    // and adds to the ack debt, only when reassembly accepts it: a refused
    // fragment is never named in a block, so the server's aggregate never
    // completes on it (tla/FragmentsAndAcks.tla RecvData,
    // NamedOnlyIfAccepted). an accepted fragment that completes a payload
    // later dropped as batch full still acknowledges: reassembly accepted it.

    if ( accepted )
    {
        rocketnet_replay_advance( &client->accepted_session, client->received_session_sequence );

        client->ack.ack_debt++;
    }

    return accepted;
}

static int rocketnet_client_process_disconnect( struct rocketnet_client_protocol_t * client,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                double current_time )
{
    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_DISCONNECT_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_DISCONNECT_WRONG_OPENED_SIZE
    };

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = 0;
    int type_data_bytes = rocketnet_client_open_session( client, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                          &type_counters,
                                                          packet_data, packet_bytes,
                                                          current_time, opened, (int) sizeof( opened ), &type_data_offset, NULL );
    if ( type_data_bytes < 0 )
        return 0;

    client->counters->values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED]++;

    rocketnet_debug( "client disconnected by server" );

    rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    return 1;
}

int rocketnet_client_protocol_process_packet( struct rocketnet_client_protocol_t * client,
                                              const uint8_t * packet_data,
                                              int packet_bytes,
                                              double current_time,
                                              uint8_t * out_payload,
                                              int out_payload_capacity,
                                              int * out_payload_bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( packet_data );
    rocketnet_assert( out_payload );
    rocketnet_assert( out_payload_bytes );

    uint64_t * counters = client->counters->values;

    *out_payload_bytes = 0;

    // the caller's buffer must hold the largest ordinary payload: a smaller
    // one is refused here, once per call and before the packet is read, so an
    // overrun cannot happen and the protocol state is left untouched. the
    // transport reports the same condition from its own cursor.

    if ( out_payload_capacity < ROCKETNET_RECEIVE_PAYLOAD_BYTES )
    {
        rocketnet_spam( "client dropped packet. output capacity too small" );

        counters[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL]++;
        return 0;
    }

    client->received_session_sequence = 0;

    if ( client->reassembly )
    {
        client->reassembly->completed_payload = NULL;
        client->reassembly->completed_bytes = 0;
    }

    counters[ROCKETNET_COUNTER_PACKETS_RECEIVED]++;

    int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, client->packet_filter );
    if ( packet_type < 0 )
    {
        rocketnet_spam( "client dropped packet. prefilter" );

        counters[rocketnet_prefilter_counter( packet_type )]++;
        return 0;
    }

    // types 10 through 255 are reserved and ignored by a conforming receiver

    if ( packet_type > ROCKETNET_CONNECTION_FRAGMENT_PACKET )
    {
        rocketnet_spam( "client dropped packet. reserved type" );

        counters[ROCKETNET_COUNTER_RESERVED_TYPE]++;
        return 0;
    }

    // a client never processes what only a client sends -- request, response, and its own session sends arriving back are junk

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
            return rocketnet_client_process_cookie( client, packet_data, packet_bytes );
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
        case ROCKETNET_CONNECTION_ACK_PACKET:
            return rocketnet_client_process_ack( client, packet_data, packet_bytes, current_time );
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:
            return rocketnet_client_process_fragment( client, packet_data, packet_bytes, current_time );
    }

    return 0;
}

// The fill-phase pair behind the client's acquire/commit send surface --
// the same phase contract as the server's: update on the main thread,
// start and seal from any game thread strictly between updates, the only
// shared words a sequence add and a timestamp store.

// tla/RNClientIoDecrypt.tla UpdateConsume
// tla/RNClientIoDecrypt.tla IoTakeKeys, IoDropKeys and IoKeepKeys: the I/O
// thread reads the epoch at the top of a drain and takes, zeroes or keeps its
// open key copy.
// tla/RNClientIoDecrypt.tla IoReceive, IoCheck and IoOpen: the I/O thread
// drains datagrams into its buffer, runs the cheap replay check and the AEAD,
// tagging each record opened, replay, forged or raw.
// tla/RNClientIoDecrypt.tla IoPublish, IoOwe, IoWait, IoWake and IoClearWake:
// the I/O thread publishes its filled buffer or records the owed publish, waits
// on the wake, and burns a wake it does not need.
// tla/RNClientIoDecrypt.tla Expose, Return and Done: update exposes the ready
// buffer at its start, returns it at its end, and the behaviour ends drained.
// tla/RNClientIoDecrypt.tla Tag: each step records its thread as the writer,
// which the SingleWriter invariant reads.

int rocketnet_client_protocol_process_record( struct rocketnet_client_protocol_t * client,
                                              const uint8_t * packet_data,
                                              int packet_bytes,
                                              uint8_t verdict,
                                              uint64_t sequence,
                                              double current_time,
                                              uint8_t * out_payload,
                                              int out_payload_capacity,
                                              int * out_payload_bytes )
{
    rocketnet_assert( client );

    if ( verdict == ROCKETNET_CLIENT_IO_VERDICT_RAW )
        return rocketnet_client_protocol_process_packet( client, packet_data, packet_bytes, current_time,
                                                         out_payload, out_payload_capacity, out_payload_bytes );

    client->record_verdict = verdict;
    client->record_sequence = sequence;

    const int result = rocketnet_client_protocol_process_packet( client, packet_data, packet_bytes, current_time,
                                                                 out_payload, out_payload_capacity, out_payload_bytes );

    client->record_verdict = ROCKETNET_CLIENT_IO_VERDICT_RAW;
    client->record_sequence = 0;

    return result;
}

int rocketnet_client_protocol_start_payload( struct rocketnet_client_protocol_t * client, uint64_t * sequence )
{
    rocketnet_assert( client );
    rocketnet_assert( sequence );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    *sequence = rocketnet_platform_atomic_add_u64( &client->send_sequence_session, 1 );

    return 1;
}

// tla/RNClientIoDecrypt.tla FillAckBuild: a fill thread builds the ack block
// from a receive window only update writes, and writes nothing shared.

int rocketnet_client_protocol_seal_payload_frame( struct rocketnet_client_protocol_t * client,
                                                  uint64_t sequence,
                                                  uint64_t packet_id,
                                                  uint8_t * wire_start,
                                                  int payload_bytes,
                                                  double current_time )
{
    rocketnet_assert( client );
    rocketnet_assert( wire_start );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return 0;

    // the application's bytes come first: the block truncates to the budget's remaining room, oldest entries dropped first. the 7-byte minimum always fits.

    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;

    int ack_block_bytes = rocketnet_ack_block_build( ack_block, block_capacity, &client->accepted_session );

    int wire_bytes = rocketnet_write_sealed_payload_in_place( wire_start, client->packet_filter, sequence,
                                                              ack_block, ack_block_bytes,
                                                              payload_bytes, client->signed_fields.protocol_id,
                                                              client->connection_salt,
                                                              client->keys.session_client_to_server );
    if ( wire_bytes == 0 )
        return 0;

    rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( &client->ack, sequence, wire_bytes, packet_id, 1, 0, current_time ),
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );

    rocketnet_platform_atomic_store_u64( &client->ack.ack_debt, 0 );        // fill-phase threads reset concurrently

    rocketnet_platform_atomic_store_double( &client->last_send_time, current_time );

    return wire_bytes;
}

#if ROCKETNET_DEV
int rocketnet_client_protocol_write_payload( struct rocketnet_client_protocol_t * client,
                                             const uint8_t * payload_data,
                                             int payload_bytes,
                                             double current_time,
                                             uint8_t * out_packet )
{
    rocketnet_assert( client );
    rocketnet_assert( payload_data );
    rocketnet_assert( out_packet );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return 0;

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;

    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &client->accepted_session );
    memcpy( plaintext + block_bytes, payload_data, (size_t) payload_bytes );

    uint64_t sequence = client->send_sequence_session;

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                               client->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + payload_bytes,
                                               client->signed_fields.protocol_id, client->connection_salt,
                                               NULL, client->keys.session_client_to_server );
    if ( bytes == 0 )
        return 0;

    client->send_sequence_session++;

    rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( &client->ack, sequence, bytes, 0, 0, 0, current_time ),
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );

    client->last_send_time = current_time;
    client->ack.ack_debt = 0;

    rocketnet_spam( "client sent payload (%d bytes)", payload_bytes );

    return bytes;
}
#endif // #if ROCKETNET_DEV

// The fragmented send path. The payload id and the sequence block are each
// one atomic add, so any number of fill threads split payloads
// concurrently; the fragments themselves need no ordering or contiguity --
// each is an ordinary sealed session packet with its own sequence.

uint32_t rocketnet_client_protocol_next_fragment_payload_id( struct rocketnet_client_protocol_t * client )
{
    rocketnet_assert( client );

    // the counter is 64-bit so the fold to the wire's u32 can skip zero forever -- zero is what an uninitialized sender produces, and this sender is initialized

    uint64_t raw = rocketnet_platform_atomic_add_u64( &client->next_fragment_payload_id, 1 );
    return (uint32_t) ( 1 + ( raw - 1 ) % 0xFFFFFFFFULL );
}

int rocketnet_client_protocol_start_fragments( struct rocketnet_client_protocol_t * client, int fragment_count, uint64_t * base_sequence )
{
    rocketnet_assert( client );
    rocketnet_assert( base_sequence );
    rocketnet_assert( fragment_count >= 1 );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    *base_sequence = rocketnet_platform_atomic_add_u64( &client->send_sequence_session, (uint64_t) fragment_count );

    return 1;
}

int rocketnet_client_protocol_seal_fragment( struct rocketnet_client_protocol_t * client,
                                             uint64_t sequence,
                                             uint32_t payload_id,
                                             int fragment_index,
                                             int fragment_count,
                                             const uint8_t * fragment_data,
                                             int fragment_bytes,
                                             uint64_t aggregate_id,
                                             double current_time,
                                             uint8_t * out_packet )
{
    rocketnet_assert( client );
    rocketnet_assert( fragment_data );
    rocketnet_assert( out_packet );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    if ( payload_id == 0 ||
         fragment_count < 2 || fragment_count > ROCKETNET_MAX_FRAGMENTS ||
         fragment_index < 0 || fragment_index >= fragment_count ||
         fragment_bytes < 1 || fragment_bytes > ROCKETNET_MAX_FRAGMENT_BYTES )
        return 0;

    // the fragment bytes come first: the stride is chosen against the
    // minimum block, so a current block with ranges truncates to the
    // remaining room. the 7-byte minimum always fits.

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_FRAGMENT_BYTES];

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_FRAGMENT_HEADER_BYTES - fragment_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;

    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &client->accepted_session );

    rocketnet_write_fragment_header( plaintext + block_bytes, payload_id, fragment_index, fragment_count );

    memcpy( plaintext + block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES, fragment_data, (size_t) fragment_bytes );

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                               client->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                                               client->signed_fields.protocol_id, client->connection_salt,
                                               NULL, client->keys.session_client_to_server );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );

    rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( &client->ack, sequence, bytes, 0, 0, aggregate_id, current_time ),
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                     &client->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );

    rocketnet_platform_atomic_store_u64( &client->ack.ack_debt, 0 );        // fill-phase threads reset concurrently

    rocketnet_platform_atomic_store_double( &client->last_send_time, current_time );

    return bytes;
}

#if ROCKETNET_DEV
int rocketnet_client_protocol_write_fragments( struct rocketnet_client_protocol_t * client,
                                               const uint8_t * payload_data,
                                               int payload_bytes,
                                               double current_time,
                                               uint8_t * out_packets,
                                               int * out_packet_bytes,
                                               int max_packets )
{
    rocketnet_assert( client );
    rocketnet_assert( payload_data );
    rocketnet_assert( out_packets );
    rocketnet_assert( out_packet_bytes );

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return 0;

    int fragment_count, fragment_stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &fragment_stride ) )
        return 0;

    if ( fragment_count > max_packets )
        return 0;

    uint32_t payload_id = rocketnet_client_protocol_next_fragment_payload_id( client );

    uint64_t base_sequence;
    if ( !rocketnet_client_protocol_start_fragments( client, fragment_count, &base_sequence ) )
        return 0;

    for ( int i = 0; i < fragment_count; i++ )
    {
        const int fragment_bytes = i < fragment_count - 1 ? fragment_stride : payload_bytes - fragment_stride * ( fragment_count - 1 );
        int wire_bytes = rocketnet_client_protocol_seal_fragment( client, base_sequence + (uint64_t) i, payload_id,
                                                                  i, fragment_count,
                                                                  payload_data + i * fragment_stride, fragment_bytes,
                                                                  0, current_time,
                                                                  out_packets + i * ROCKETNET_SEND_BUDGET );
        rocketnet_assert( wire_bytes > 0 );         // connected, and every argument in range by the plan

        out_packet_bytes[i] = wire_bytes;
    }

    rocketnet_spam( "client sent fragmented payload %u (%d bytes in %d fragments)", payload_id, payload_bytes, fragment_count );

    return fragment_count;
}
#endif // #if ROCKETNET_DEV

// tla/ClientConnection.tla AppDisconnect
// tla/ClientConnection.tla ClientDiscCopy: seals each copy of the disconnect
// burst once; the burst is never retransmitted.

void rocketnet_client_protocol_disconnect( struct rocketnet_client_protocol_t * client,
                                           uint8_t * out_packets,
                                           int * out_packet_count,
                                           int * out_packet_bytes )
{
    rocketnet_assert( client );
    rocketnet_assert( out_packets );
    rocketnet_assert( out_packet_count );
    rocketnet_assert( out_packet_bytes );

    *out_packet_count = 0;
    *out_packet_bytes = 0;

    // before the session keys exist there is nothing to seal: the state simply resets, and the server's half-open mapping dies at its deadline

    if ( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE ||
         client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        // each packet of the burst opens with the same ack block -- the receive state does not move between them -- so the burst shares one wire size

        int burst_bytes = 0;

        for ( int i = 0; i < ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS; i++ )
        {
            int bytes = rocketnet_client_write_session_packet( client, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                               NULL, 0, client->last_send_time,
                                                               out_packets + i * ROCKETNET_SEND_BUDGET );
            rocketnet_assert( bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
            rocketnet_assert( i == 0 || bytes == burst_bytes );

            burst_bytes = bytes;
        }

        *out_packet_count = ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS;
        *out_packet_bytes = burst_bytes;

        rocketnet_debug( "client sent %d redundant disconnect packets", ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS );
    }

    if ( client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        rocketnet_client_set_state( client, ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    else
        rocketnet_client_connection_reset( client );
}

void rocketnet_client_protocol_accept_payload( struct rocketnet_client_protocol_t * client, uint64_t sequence )
{
    if ( !sequence || client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
         rocketnet_replay_already_received( &client->accepted_session, sequence ) )
        return;

    rocketnet_replay_advance( &client->accepted_session, sequence );

    client->ack.ack_debt++;
}
