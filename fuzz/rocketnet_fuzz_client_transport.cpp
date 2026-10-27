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
//   mode 1: sealed with the live keys: bytes[0] picks the session type,
//           bytes[1] the sequence skip, the rest is the plaintext -- the
//           post-decrypt parse of every server -> client session type.
//           The handshake types (challenge, denied) are not sealed by this
//           target: see fuzz/README.md, "Exclusions".
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

#include <filesystem>
#include <string>

static const uint8_t rocketnet_fuzz_s2c_types[] = {
    ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
    ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET
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

    // one more step: what the last record sent the client's way is read
    rocketnet_fuzz_client_step( pair );

    char report[240];
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( rocketnet_fuzz_pair_client_counters( pair ), report, sizeof( report ) ) != 0,
                                 "the client transport's counters are conserved" );
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( rocketnet_message_client_counters( pair->client ), report, sizeof( report ) ) != 0,
                                 "the message client's counters are conserved" );
    rocketnet_fuzz_pair_check_no_allocation( pair );
    return rocketnet_fuzz_pair_fingerprint( pair );
}

// -----------------------------------------------------------------------
//  the seed generator (ROCKETNET_FUZZ_SEED_DIR=<dir>, rocketnet_fuzz_pair.h)
//
// For every server -> client type the client reads (denied, challenge,
// keep-alive, payload, disconnect, cookie, ack, fragment): the real datagram,
// raw, from the server's address; its real plaintext sealed under the live
// keys; and for the cookie, its real expire and mac through the cookie mode.
// Each seed must raise its type's counter when replayed: the received
// counter for raw and cookie seeds, the accepted counter for sealed ones.
//
// EXCLUDED, by name, so the check stays exact for everything else: the
// sealed denied and sealed challenge seeds are not generated, and the server
// never denies this pair, so the denied has no seed at all. Not generated:
// blocked by a safety classifier 2026-10-21; Glenn decides (fuzz/README.md,
// "Exclusions").
// -----------------------------------------------------------------------

struct rocketnet_fuzz_client_seed_type_t
{
    uint8_t type;
    int received;
    int accepted;           // -1: the type is not sealed
};

static const struct rocketnet_fuzz_client_seed_type_t rocketnet_fuzz_client_seed_types[] = {
    { ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED, ROCKETNET_COUNTER_CLIENT_DENIED_ACCEPTED },
    { ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED, ROCKETNET_COUNTER_CLIENT_CHALLENGE_ACCEPTED },
    { ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ACCEPTED },
    { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED },
    { ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_CLIENT_DISCONNECT_ACCEPTED },
    { ROCKETNET_CONNECTION_COOKIE_PACKET, ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED, -1 },
    { ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED, ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED },
    { ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_CLIENT_FRAGMENT_ACCEPTED },
};

static bool rocketnet_fuzz_client_excluded( uint8_t type )
{
    return type == ROCKETNET_CONNECTION_DENIED_PACKET || type == ROCKETNET_CONNECTION_CHALLENGE_PACKET;
}

static int rocketnet_fuzz_client_type_index( uint8_t type )
{
    for ( int i = 0; i < (int) sizeof( rocketnet_fuzz_s2c_types ); i++ )
        if ( rocketnet_fuzz_s2c_types[i] == type )
            return i;
    return -1;
}

static bool rocketnet_fuzz_client_generate( const char * dir )
{
    static struct rocketnet_fuzz_recording_t recording;
    static uint8_t seed[4 + 3 * 300 + ROCKETNET_RECEIVE_CEILING + 8];
    struct rocketnet_fuzz_pair_t * pair = &rocketnet_fuzz_client_pair;
    rocketnet_fuzz_pair_record( pair, &recording );

    bool complete = true;
    char name[256];
    for ( size_t k = 0; k < sizeof( rocketnet_fuzz_client_seed_types ) / sizeof( rocketnet_fuzz_client_seed_types[0] ); k++ )
    {
        const struct rocketnet_fuzz_client_seed_type_t * t = &rocketnet_fuzz_client_seed_types[k];
        const struct rocketnet_fuzz_recorded_t * r = &recording.s2c[t->type];
        const char * type_name = rocketnet_fuzz_packet_type_names[t->type];
        const bool denied = t->type == ROCKETNET_CONNECTION_DENIED_PACKET;
        if ( denied )
        {
            printf( "seed generator: excluded: denied (not generated: blocked by a safety classifier 2026-10-21, Glenn decides)\n" );
            continue;
        }
        if ( r->bytes == 0 )
        {
            fprintf( stderr, "seed generator: no real %s was recorded\n", type_name );
            complete = false;
            continue;
        }
        const bool handshake = r->handshake;
        const int step = r->step;
        const uint8_t selector = handshake ? 0x10 : 0x00;
        const int wait_from = handshake ? step - 4 : 0;
        const int wait_to = handshake ? step + 4 : 2;

        {
            const int bytes = rocketnet_fuzz_seed_place( seed, rocketnet_fuzz_client_run, rocketnet_fuzz_pair_client_counters, t->received,
                                                         selector, wait_from, wait_to, 0, r->data, r->bytes, NULL, 0 );
            snprintf( name, sizeof( name ), "raw_%s", type_name );
            if ( bytes > 0 )
                complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
            else
            {
                fprintf( stderr, "seed generator: the raw %s seed reaches no %s\n", type_name, rocketnet_counter_name( t->received ) );
                complete = false;
            }
        }

        if ( t->type == ROCKETNET_CONNECTION_COOKIE_PACKET )
        {
            // the cookie mode carries the real expire timestamp and mac
            const int at = ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES;
            const int bytes = rocketnet_fuzz_seed_place( seed, rocketnet_fuzz_client_run, rocketnet_fuzz_pair_client_counters, t->received,
                                                         selector, wait_from, wait_to, 2, r->data + at, 8 + ROCKETNET_COOKIE_MAC_BYTES, NULL, 0 );
            if ( bytes > 0 )
                complete = rocketnet_fuzz_seed_write( dir, "cookie_fields", seed, bytes ) && complete;
            else
            {
                fprintf( stderr, "seed generator: the cookie-mode seed reaches no %s\n", rocketnet_counter_name( t->received ) );
                complete = false;
            }
        }

        if ( t->accepted < 0 )
            continue;
        if ( rocketnet_fuzz_client_excluded( t->type ) )
        {
            printf( "seed generator: excluded: sealed %s (not generated: blocked by a safety classifier 2026-10-21, Glenn decides)\n", type_name );
            continue;
        }
        if ( r->plaintext_bytes < 0 )
        {
            fprintf( stderr, "seed generator: the recorded %s did not open\n", type_name );
            complete = false;
            continue;
        }
        const uint8_t head[2] = { (uint8_t) rocketnet_fuzz_client_type_index( t->type ), 0 };
        const int bytes = rocketnet_fuzz_seed_place( seed, rocketnet_fuzz_client_run, rocketnet_fuzz_pair_client_counters, t->accepted,
                                                     selector, wait_from, wait_to, 1, head, 2,
                                                     r->plaintext, r->plaintext_bytes );
        snprintf( name, sizeof( name ), "sealed_%s", type_name );
        if ( bytes > 0 )
            complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
        else
        {
            fprintf( stderr, "seed generator: the sealed %s seed reaches no %s\n", type_name, rocketnet_counter_name( t->accepted ) );
            complete = false;
        }
    }

    // the committed wire corpus, each datagram from the server's address
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
            const int datagram = got - 1 > ROCKETNET_RECEIVE_CEILING ? ROCKETNET_RECEIVE_CEILING : got - 1;
            seed[0] = 0x00;
            const int bytes = 1 + rocketnet_fuzz_record_write( seed + 1, 0, body + 1, datagram, NULL, 0 );
            snprintf( name, sizeof( name ), "wire_%s", file.c_str() + strlen( "prefilter_" ) );
            complete = rocketnet_fuzz_seed_write( dir, name, seed, bytes ) && complete;
            wrapped++;
        }
        printf( "seed generator: wrapped %d committed wire seeds\n", wrapped );
    }

    printf( "seed generator: %s\n", complete ? "every server -> client type outside the exclusion list has a seed that reaches its reader" : "INCOMPLETE" );
    return complete;
}

ROCKETNET_FUZZ_PAIR_TARGET( rocketnet_fuzz_client_run, rocketnet_fuzz_client_generate )
