/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client's real receive loop, stateful: the step-driven client transport
// draining its loopback endpoint inside rocketnet_client_transport_update (the
// client io step, the source check, the prefilter, the handshake and session
// reading order, reassembly) and the message client above it, on a client
// connected to a real server. Every datagram the input names arrives from the
// server's real address, so it passes the client's source check and lands in
// the parsers.
//
// INPUT: [config selector][record]...
//   selector bit 4:    start the records during the handshake instead of
//                      after it: the cookie, challenge and denied paths
// each record is [mode][u16 length][length bytes]:
//   mode 0: the bytes, raw, from the server's address
//   mode 1: sealed with the live keys: bytes[0] picks the packet type,
//           bytes[1] the sequence skip, the rest is the plaintext -- the
//           post-decrypt parse of every server -> client type, the
//           challenge and the denied included (they need the mapping the
//           server makes at the request, so they open only once it exists)
//   mode 2: an unsealed cookie for the client's own salt: bytes[0..7] the
//           expire timestamp, bytes[8..23] the mac
//   mode 3: a real server datagram from the wire, with the bytes XORed over
//           it from the prefilter on
//   mode 4: time: bytes[0] ticks of clean traffic
// After every record the pair takes one step with the real traffic between
// the two delivered cleanly.
//
// ORACLES: the deterministic pair connects; nothing allocates after create;
// every message the client delivers is within max_message_bytes; the
// client's counters obey their conservation law at the end.

#include "rocketnet_fuzz_pair.h"

static const uint8_t rocketnet_fuzz_s2c_types[] =
{
    ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
    ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
    ROCKETNET_CONNECTION_DENIED_PACKET
};

static struct rocketnet_fuzz_pair_t rocketnet_fuzz_client_pair;

static void rocketnet_fuzz_client_step( struct rocketnet_fuzz_pair_t * pair )
{
    pair->time += ROCKETNET_FUZZ_DT;
    rocketnet_fuzz_pair_server_update( pair );
    rocketnet_fuzz_pair_client_update( pair );

    const uint8_t * data = NULL;
    int bytes = 0;
    int channel = 0;
    while ( rocketnet_message_client_receive( pair->client, &channel, &data, &bytes ) )
    {
        rocketnet_fuzz_pair_require( bytes >= 0 && bytes <= ROCKETNET_FUZZ_MAX_MESSAGE_BYTES, "a delivered message is within max_message_bytes" );
        rocketnet_fuzz_pair_require( channel >= 0 && channel < ROCKETNET_FUZZ_NUM_CHANNELS, "a delivered message names a configured channel" );
        rocketnet_message_client_release( pair->client, data );
    }

    rocketnet_fuzz_pair_drain_messages( pair );
    rocketnet_fuzz_pair_send( pair );
    rocketnet_fuzz_pair_deliver_all( pair );
    rocketnet_fuzz_pair_check_no_allocation( pair );
}

static uint64_t rocketnet_fuzz_client_run( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;

    struct rocketnet_fuzz_cursor_t cursor = { data, size, 0 };
    const uint8_t selector = rocketnet_fuzz_u8( &cursor );

    struct rocketnet_fuzz_pair_t * pair = &rocketnet_fuzz_client_pair;
    rocketnet_fuzz_pair_acquire( pair, ( selector & 0x10 ) == 0 );

    static uint8_t record[ROCKETNET_RECEIVE_CEILING];
    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int records = 0; records < 512 && rocketnet_fuzz_more( &cursor ); records++ )
    {
        const uint8_t mode = rocketnet_fuzz_u8( &cursor );
        int length = (int) rocketnet_fuzz_u16( &cursor );
        if ( length > ROCKETNET_RECEIVE_CEILING )
            length = ROCKETNET_RECEIVE_CEILING;
        const size_t available = cursor.size - cursor.pos;
        if ( (size_t) length > available )
            length = (int) available;
        memcpy( record, cursor.data + cursor.pos, (size_t) length );
        cursor.pos += (size_t) length;

        // the client's address is learned from its first datagram; in the
        // handshake-start case one quiet step lets it send that first
        if ( !pair->client_known )
            rocketnet_fuzz_client_step( pair );

        switch ( mode % 5 )
        {
            case 0:
                if ( length > 0 )
                    rocketnet_fuzz_pair_inject_s2c( pair, record, length );
                break;

            case 1:
            {
                if ( length < 2 )
                    break;
                const uint8_t type = rocketnet_fuzz_s2c_types[record[0] % sizeof( rocketnet_fuzz_s2c_types )];
                const int bytes = rocketnet_fuzz_pair_seal_s2c( pair, type, record[1] % 64, record + 2, length - 2, packet );
                if ( bytes > 0 )
                    rocketnet_fuzz_pair_inject_s2c( pair, packet, bytes );
                break;
            }

            case 2:
            {
                if ( !pair->salt_known )
                    break;
                uint64_t expire = 0;
                for ( int i = 0; i < 8 && i < length; i++ )
                    expire |= (uint64_t) record[i] << ( 8 * i );
                uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
                memset( mac, 0, sizeof( mac ) );
                for ( int i = 0; i < ROCKETNET_COOKIE_MAC_BYTES && 8 + i < length; i++ )
                    mac[i] = record[8 + i];
                rocketnet_write_connection_cookie_packet( packet, pair->transport->protocol.packet_filter, pair->client_salt, expire, mac );
                rocketnet_fuzz_pair_inject_s2c( pair, packet, ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
                break;
            }

            case 3:
            {
                // the server's next real datagram, taken off the wire and damaged
                pair->time += ROCKETNET_FUZZ_DT;
                rocketnet_fuzz_pair_server_update( pair );
                rocketnet_message_server_send_packets( pair->server );
                if ( pair->s2c.count > 0 )
                {
                    struct rocketnet_fuzz_datagram_t * d = &pair->s2c.packet[0];
                    for ( int i = 0; i < length && ROCKETNET_PREFILTER_BYTES + i < d->bytes; i++ )
                        d->data[ROCKETNET_PREFILTER_BYTES + i] ^= record[i];
                    rocketnet_fuzz_pair_deliver_s2c( pair, 0 );
                }
                break;
            }

            case 4:
            {
                const int ticks = length > 0 ? 1 + record[0] % 64 : 1;
                for ( int t = 0; t < ticks; t++ )
                    rocketnet_fuzz_client_step( pair );
                break;
            }
        }

        rocketnet_fuzz_client_step( pair );
    }

    char report[240];
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( rocketnet_message_client_counters( pair->client ), report, sizeof( report ) ) != 0,
                                 "the client's counters are conserved" );
    rocketnet_fuzz_pair_check_no_allocation( pair );
    return rocketnet_fuzz_pair_fingerprint( pair );
}

ROCKETNET_FUZZ_PAIR_TARGET( rocketnet_fuzz_client_run )
