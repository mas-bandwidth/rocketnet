/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's real receive loop, stateful: rocketnet_server_transport_update
// on a long-lived message server with a connected client, fed datagrams the
// input names. Where rocketnet_fuzz_receive_structured drives the protocol
// layer's process_packet directly, this target goes in at the top -- the
// receive drain, the wire totals, the admission gate, the send-frame
// reservation, process_packet_capacity, the payload batch and the
// reassembled-payload hand-off -- and up through the message layer that reads
// what the transport delivers.
//
// INPUT: [config selector][record]...
//   selector bit 4:    start the records during the handshake instead of
//                      after it, so the request, cookie, challenge and
//                      response states are where the records land
// each record is [mode][u16 length][length bytes], one datagram (or one tick)
// per record, the yojimbo fuzz_connection shape:
//   mode 0: the bytes, raw, from the connected client's own address
//   mode 1: the bytes, raw, from a stranger the server has never seen
//   mode 2: sealed with the live keys: bytes[0] picks the packet type,
//           bytes[1] the sequence skip, the rest is the plaintext -- the
//           post-decrypt parse of every client -> server type
//   mode 3: a real client datagram from the wire, with the bytes XORed over
//           it from the prefilter on: valid envelopes, damaged contents
//   mode 4: time: bytes[0] ticks of clean traffic, so acks, keep-alives,
//           resends and deadlines move
// After every record the pair takes one step: the server receives, the client
// updates, and the real traffic between them is delivered cleanly, so the
// session stays live for the records that follow.
//
// ORACLES: the deterministic pair must connect (a seed replays the same
// handshake); nothing allocates after create; every payload the server hands
// up is within the receive payload bound; the server's counters obey their
// conservation law at the end.

#include "rocketnet_fuzz_pair.h"

static const uint8_t rocketnet_fuzz_c2s_types[] =
{
    ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
    ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET
};

static struct rocketnet_fuzz_pair_t rocketnet_fuzz_server_pair;

static void rocketnet_fuzz_server_step( struct rocketnet_fuzz_pair_t * pair )
{
    pair->time += ROCKETNET_FUZZ_DT;
    rocketnet_fuzz_pair_server_update( pair );

    // the server's payload batch this update: each payload is bounded
    const struct rocketnet_server_transport_t * transport = pair->transport;
    for ( int i = 0; i < transport->payloads.num_payloads; i++ )
        rocketnet_fuzz_pair_require( transport->payloads.payload_bytes[i] > 0 &&
                                     transport->payloads.payload_bytes[i] <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,
                                     "a delivered payload is within the payload bound" );

    rocketnet_fuzz_pair_client_update( pair );
    rocketnet_fuzz_pair_drain_messages( pair );
    rocketnet_fuzz_pair_send( pair );
    rocketnet_fuzz_pair_deliver_all( pair );
    rocketnet_fuzz_pair_check_no_allocation( pair );
}

static uint64_t rocketnet_fuzz_server_run( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;

    struct rocketnet_fuzz_cursor_t cursor = { data, size, 0 };
    const uint8_t selector = rocketnet_fuzz_u8( &cursor );

    struct rocketnet_fuzz_pair_t * pair = &rocketnet_fuzz_server_pair;
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

        switch ( mode % 5 )
        {
            case 0:
                if ( length > 0 )
                    rocketnet_fuzz_pair_inject_c2s( pair, true, record, length );
                break;

            case 1:
                if ( length > 0 )
                    rocketnet_fuzz_pair_inject_c2s( pair, false, record, length );
                break;

            case 2:
            {
                if ( length < 2 )
                    break;
                const uint8_t type = rocketnet_fuzz_c2s_types[record[0] % sizeof( rocketnet_fuzz_c2s_types )];
                const int bytes = rocketnet_fuzz_pair_seal_c2s( pair, type, record[1] % 64, record + 2, length - 2, packet );
                if ( bytes > 0 )
                    rocketnet_fuzz_pair_inject_c2s( pair, true, packet, bytes );
                break;
            }

            case 3:
            {
                // the client's next real datagram, taken off the wire and damaged
                pair->time += ROCKETNET_FUZZ_DT;
                rocketnet_fuzz_pair_client_update( pair );
                rocketnet_message_client_send_packets( pair->client );
                rocketnet_fuzz_pair_capture_c2s( pair );
                if ( pair->c2s.count > 0 )
                {
                    struct rocketnet_fuzz_datagram_t * d = &pair->c2s.packet[0];
                    for ( int i = 0; i < length && ROCKETNET_PREFILTER_BYTES + i < d->bytes; i++ )
                        d->data[ROCKETNET_PREFILTER_BYTES + i] ^= record[i];
                    rocketnet_fuzz_pair_deliver_c2s( pair, 0 );
                }
                break;
            }

            case 4:
            {
                const int ticks = length > 0 ? 1 + record[0] % 64 : 1;
                for ( int t = 0; t < ticks; t++ )
                    rocketnet_fuzz_server_step( pair );
                break;
            }
        }

        rocketnet_fuzz_server_step( pair );
    }

    char report[240];
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( &pair->transport->counters, report, sizeof( report ) ) != 0,
                                 "the server transport's counters are conserved" );

    rocketnet_fuzz_pair_check_no_allocation( pair );
    return rocketnet_fuzz_pair_fingerprint( pair );
}

ROCKETNET_FUZZ_PAIR_TARGET( rocketnet_fuzz_server_run )
