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

#include <filesystem>
#include <string>

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

// -----------------------------------------------------------------------
//  the seed generator (ROCKETNET_FUZZ_SEED_DIR=<dir>, rocketnet_fuzz_pair.h)
//
// For every client -> server type the server reads (request, response,
// keep-alive, payload, disconnect, ack, fragment): the real datagram, raw,
// from the client's address, and -- every type but the request -- its real
// plaintext sealed under the live keys. Each seed must raise its type's
// counter when replayed: the received counter for the raw seed, the
// accepted counter for the sealed one. With ROCKETNET_FUZZ_SEED_FROM=<the
// committed fuzz/corpus> it also wraps every prefilter_* datagram there as a
// stranger's and as the client's.
// -----------------------------------------------------------------------

struct rocketnet_fuzz_server_seed_type_t
{
    uint8_t type;
    int received;
    int accepted;           // -1: no sealed seed (the request is not sealed)
};

static const struct rocketnet_fuzz_server_seed_type_t rocketnet_fuzz_server_seed_types[] =
{
    { ROCKETNET_CONNECTION_REQUEST_PACKET,    ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED,  -1 },
    { ROCKETNET_CONNECTION_RESPONSE_PACKET,   ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED },
    { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED,          ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED },
    { ROCKETNET_CONNECTION_PAYLOAD_PACKET,    ROCKETNET_COUNTER_PAYLOAD_RECEIVED,             ROCKETNET_COUNTER_PAYLOAD_DELIVERED },
    { ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_COUNTER_DISCONNECT_RECEIVED,          ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED },
    { ROCKETNET_CONNECTION_ACK_PACKET,        ROCKETNET_COUNTER_ACK_RECEIVED,                 ROCKETNET_COUNTER_ACK_ACCEPTED },
    { ROCKETNET_CONNECTION_FRAGMENT_PACKET,   ROCKETNET_COUNTER_FRAGMENT_RECEIVED,            ROCKETNET_COUNTER_FRAGMENT_ACCEPTED },
};

static int rocketnet_fuzz_server_type_index( uint8_t type )
{
    for ( int i = 0; i < (int) sizeof( rocketnet_fuzz_c2s_types ); i++ )
        if ( rocketnet_fuzz_c2s_types[i] == type )
            return i;
    return -1;
}

static bool rocketnet_fuzz_server_generate( const char * dir )
{
    static struct rocketnet_fuzz_recording_t recording;
    static uint8_t seed[4 + 3 * 300 + ROCKETNET_RECEIVE_CEILING + 8];
    struct rocketnet_fuzz_pair_t * pair = &rocketnet_fuzz_server_pair;
    rocketnet_fuzz_pair_record( pair, &recording );

    bool complete = true;
    char name[256];
    for ( size_t k = 0; k < sizeof( rocketnet_fuzz_server_seed_types ) / sizeof( rocketnet_fuzz_server_seed_types[0] ); k++ )
    {
        const struct rocketnet_fuzz_server_seed_type_t * t = &rocketnet_fuzz_server_seed_types[k];
        const struct rocketnet_fuzz_recorded_t * r = &recording.c2s[t->type];
        const char * type_name = rocketnet_fuzz_packet_type_names[t->type];
        if ( r->bytes == 0 )
        {
            fprintf( stderr, "seed generator: no real %s was recorded\n", type_name );
            complete = false;
            continue;
        }
        const uint8_t selector = r->handshake ? 0x10 : 0x00;
        const int wait_from = r->handshake ? r->step - 3 : 0;
        const int wait_to = r->handshake ? r->step + 3 : 2;

        int bytes = rocketnet_fuzz_seed_place( seed, rocketnet_fuzz_server_run, rocketnet_fuzz_server_counters_of, t->received,
                                               selector, wait_from, wait_to, 0, r->data, r->bytes, NULL, 0 );
        if ( bytes > 0 )
        {
            snprintf( name, sizeof( name ), "raw_%s", type_name );
            complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
        }
        else
        {
            fprintf( stderr, "seed generator: the raw %s seed reaches no %s\n", type_name, rocketnet_counter_name( t->received ) );
            complete = false;
        }

        if ( t->accepted < 0 )
            continue;
        if ( r->plaintext_bytes < 0 )
        {
            fprintf( stderr, "seed generator: the recorded %s did not open\n", type_name );
            complete = false;
            continue;
        }
        const uint8_t head[2] = { (uint8_t) rocketnet_fuzz_server_type_index( t->type ), 0 };
        bytes = rocketnet_fuzz_seed_place( seed, rocketnet_fuzz_server_run, rocketnet_fuzz_server_counters_of, t->accepted,
                                           selector, wait_from, wait_to, 2, head, 2, r->plaintext, r->plaintext_bytes );
        if ( bytes > 0 )
        {
            snprintf( name, sizeof( name ), "sealed_%s", type_name );
            complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
        }
        else
        {
            fprintf( stderr, "seed generator: the sealed %s seed reaches no %s\n", type_name, rocketnet_counter_name( t->accepted ) );
            complete = false;
        }
    }

    // the committed wire corpus, each datagram from a stranger and from the client
    char from[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_FROM", from, sizeof( from ) ) )
    {
        int wrapped = 0;
        for ( const auto & entry : std::filesystem::directory_iterator( from ) )
        {
            const std::string file = entry.path().filename().string();
            if ( file.rfind( "prefilter_", 0 ) != 0 || !entry.is_regular_file() )
                continue;
            static uint8_t body[ROCKETNET_RECEIVE_CEILING + 2];
            FILE * f = rocketnet_test_fopen( entry.path().string().c_str(), "rb" );
            if ( !f )
                continue;
            const int got = (int) fread( body, 1, sizeof( body ), f );
            fclose( f );
            if ( got < 2 )
                continue;
            // the stateless target's selector byte comes first; the datagram is the rest
            const int datagram = got - 1 > ROCKETNET_RECEIVE_CEILING ? ROCKETNET_RECEIVE_CEILING : got - 1;
            for ( int source = 0; source < 2; source++ )
            {
                seed[0] = 0x00;
                const int bytes = 1 + rocketnet_fuzz_record_write( seed + 1, (uint8_t) ( source == 0 ? 1 : 0 ), body + 1, datagram, NULL, 0 );
                snprintf( name, sizeof( name ), "wire_%s_%s", source == 0 ? "stranger" : "client", file.c_str() + strlen( "prefilter_" ) );
                complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
                wrapped++;
            }
        }
        printf( "seed generator: wrapped %d committed wire seeds\n", wrapped );
    }

    printf( "seed generator: %s\n", complete ? "every client -> server type has a seed that reaches its reader" : "INCOMPLETE" );
    return complete;
}

ROCKETNET_FUZZ_PAIR_TARGET( rocketnet_fuzz_server_run, rocketnet_fuzz_server_generate )
