/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The path packet size probe of the handshake (sdk/rocketnet_client_protocol.cpp,
// sdk/rocketnet_server_protocol.cpp; standard/TRANSPORT.md t8.10 and t11.4),
// checked by proofs/tla/TraceRNPathProbe.tla (README-TraceCheck.md) against
// proofs/tla/RNPathProbe.tla with Timed = FALSE, the code's own timing.
//
//   tla_trace --layer path-probe --seed N --out DIR [--steps K]
//
// writes DIR/MCTracePathProbeSeed<seed>.tla and .cfg
// (MCTracePathProbePlantSeed<seed> from tla_trace_plant_path_probe).
//
// The walk drives a real client protocol and a real server protocol through a
// path of its own: no socket, no thread, no clock but the walk's. Before the
// first record it runs the handshake to the challenge at one instant (so the
// round-trip estimate is zero and every probe window is the code's
// ROCKETNET_CLIENT_PROBE_WINDOW_SENDS sends), on a client whose link MTU list
// gives four candidate sizes and a server whose list gives the same four or
// three of them. Then the path: a capacity each way, fixed for the walk; a
// packet above its direction's capacity is dropped when sent, and the rest
// are in flight in a set (two copies of one packet are one), delivered in any
// order, lost when the walk loses them. An attacker replays a client probe in
// flight, or sends a probe of any size from the client's own address that
// does not open (F) or from an address with no mapping (X).
//
// Records (tla_step), each the model action the call's own result names:
//   client_update   rocketnet_client_protocol_update, called only when
//                   rocketnet_client_protocol_send_ready says it sends:
//                   ClientSendProbe, ClientExpireNext (the probe counter
//                   CLIENT_PROBE_WINDOW_EXPIRED moved and a probe went out),
//                   ClientExpireFail (CLIENT_PROBE_FAILED moved, nothing out)
//                   or ClientSendResponse (a response went out), by the
//                   packet's type and the counters the call moved
//   client_timeout  the same call at the client's silence deadline, which
//                   the walk's clock jumps to: ClientTimeout when the call
//                   left the client in -4; the walk ends with Done after it
//   client_recv     rocketnet_client_protocol_process_packet of a packet in
//                   flight down: ClientRecvReply(r) or ClientRecvKeepAlive(r)
//   server_recv     rocketnet_server_protocol_process_packet of a packet in
//                   flight up: ServerRecvProbe(p) or ServerRecvResponse(p);
//                   its reply (SEND_PROBE) or keep-alive (SEND_KEEP_ALIVE)
//                   goes down at the size the server wrote
//   keep_alive      rocketnet_server_write_keep_alive for the client's slot:
//                   ServerSendKeepAlive
//   tick            the walk's clock forward one client send interval and
//                   rocketnet_server_protocol_update: Tick
//   server_update   rocketnet_server_protocol_update at the same time:
//                   ServerUpdate
//   lose_up, lose_down, replay, attack, done   the walk's path and attacker:
//                   LoseUp(p), LoseDown(r), Replay(p), AttackProbe(a, s), Done
//
// Every model variable is read from the code but the path's: phase from the
// client's state and probing flag, idx from probe_index, size from
// packet_bytes, sends from probe_sends, maySend from
// rocketnet_client_protocol_send_ready, mapping, slot and answered from the
// server's mapping of the client's address (its client index, packet_bytes
// and probe_update against the server's update_number). capUp, capDown, up,
// down, losses and attacks are the walk's path: an honest probe the server
// withholds (its counter CONNECTION_PROBE_WITHHELD moved) is a loss, as the
// model counts it. A packet in flight is written as the model's record: a
// reply's size is the size the server wrote, its p the size of the probe it
// answered; a response's size is the client's packet_bytes, which the
// server's slot then checks.
//
// The plant (ROCKETNET_PATH_PROBE_PLANT, in rocketnet_server_process_probe)
// never reads the mapping's per-update allowance, so a second probe in one
// update is answered. Its effect shows at the first server_recv that answers
// a probe while the code's own allowance for the update was already spent,
// which the clean code never does; the walk reports that record.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_PATH_PROBE_PLANT 1
#include "../sdk/rocketnet_server_protocol.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_token_tables.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_ack.h"
#include "../sdk/rocketnet_link_mtu.h"
#include "../sdk/rocketnet_test_vectors.h"
#include "tla_trace_record.h"

// the walk's clock: a send rate whose interval is exact in binary, so every
// tick makes the client's send due with no rounding
#define PP_SEND_RATE        64.0
#define PP_START_TIME       100.0
// the token's silence timeout, and a handshake deadline past any walk: 400
// ticks are 6.25 s, so no deadline but the walk's own jump ends the attempt
#define PP_TIMEOUT_SECONDS  10
#define PP_HANDSHAKE_DEADLINE 60.0

// the client's list: four candidate sizes over IPv4, 1452, 1372, 1252 and 548
static const int pp_client_mtus[] = { 1500, 1400, 1280, 576 };
// the server's: the same, or without 1400 (a probe of 1372 is refused)
static const int pp_server_mtus_short[] = { 1500, 1280, 576 };

enum pp_kind_t { PP_PROBE, PP_RESP, PP_REPLY, PP_KA };
enum pp_source_t { PP_C, PP_F, PP_X, PP_S };

struct pp_packet_t
{
    int kind;
    int src;
    int s;
    int p;
    int n;
    std::vector<uint8_t> bytes;
};

static bool pp_same( const pp_packet_t & a, const pp_packet_t & b )
{
    return a.kind == b.kind && a.src == b.src && a.s == b.s && a.p == b.p && a.n == b.n;
}

static std::string pp_record( const pp_packet_t & packet )
{
    static const char * kinds[] = { "probe", "resp", "reply", "ka" };
    static const char * sources[] = { "C", "F", "X", "S" };
    tla_record_t record;
    record.add( "k", tla_string( kinds[packet.kind] ) );
    record.add( "src", tla_string( sources[packet.src] ) );
    record.add( "s", tla_int( packet.s ) );
    record.add( "p", tla_int( packet.p ) );
    record.add( "n", tla_int( packet.n ) );
    return record.text();
}

static std::string pp_set( const std::vector<pp_packet_t> & packets )
{
    std::string text = "{";
    for ( size_t i = 0; i < packets.size(); i++ )
    {
        text += ( i > 0 ? ", " : "" ) + pp_record( packets[i] );
    }
    return text + "}";
}

static std::string pp_int_set( const std::vector<int> & values )
{
    std::string text = "{";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        text += ( i > 0 ? ", " : "" ) + tla_int( values[i] );
    }
    return text + "}";
}

struct pp_walk_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t other_address;
    double now;
    int cap_up;
    int cap_down;
    std::vector<pp_packet_t> up;
    std::vector<pp_packet_t> down;
    int losses;
    int attacks;
    uint64_t forged_sequence;
};

// the path: a packet above the direction's capacity never arrives, and a
// packet already in flight is the one packet the set holds (the model's set)
static void pp_send( std::vector<pp_packet_t> & flight, int capacity, const pp_packet_t & packet )
{
    const int wire = ( packet.kind == PP_RESP || packet.kind == PP_KA ) ? 0 : packet.s;
    if ( wire > capacity )
    {
        return;
    }
    for ( size_t i = 0; i < flight.size(); i++ )
    {
        if ( pp_same( flight[i], packet ) )
        {
            flight[i].bytes = packet.bytes;
            return;
        }
    }
    flight.push_back( packet );
}

static struct rocketnet_encryption_mapping_t * pp_mapping( pp_walk_t * walk )
{
    const int index = rocketnet_encryption_map_find( walk->server.encryption_map, &walk->client_address );
    return index < 0 ? NULL : rocketnet_encryption_map_entry( walk->server.encryption_map, index );
}

static bool pp_answered( pp_walk_t * walk )
{
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    return mapping && mapping->probe_update == walk->server.update_number + 1;
}

static const char * pp_phase( const pp_walk_t * walk )
{
    switch ( walk->client.state )
    {
        case ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE: return walk->client.probing ? "probe" : "response";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED:                   return "connected";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_MTU_PROBE_FAILED:            return "failed";
        case ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT: return "timedout";
        default:                                                          return "other";
    }
}

static bool pp_may_send( const pp_walk_t * walk )
{
    return rocketnet_client_protocol_send_ready( &walk->client, walk->now );
}

static tla_record_t pp_state( pp_walk_t * walk )
{
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    tla_record_t state;
    state.add( "capUp", tla_int( walk->cap_up ) );
    state.add( "capDown", tla_int( walk->cap_down ) );
    state.add( "phase", tla_string( pp_phase( walk ) ) );
    state.add( "idx", tla_int( walk->client.probe_index + 1 ) );
    state.add( "size", tla_int( walk->client.packet_bytes ) );
    state.add( "sends", tla_int( walk->client.probe_sends ) );
    state.add( "maySend", tla_bool( pp_may_send( walk ) ) );
    state.add( "mapping", tla_string( !mapping ? "none" : ( mapping->control->client_index == -1 ? "halfopen" : "connected" ) ) );
    state.add( "slot", tla_int( mapping ? mapping->packet_bytes : -1 ) );
    state.add( "answered", tla_bool( pp_answered( walk ) ) );
    state.add( "up", pp_set( walk->up ) );
    state.add( "down", pp_set( walk->down ) );
    state.add( "losses", tla_int( walk->losses ) );
    state.add( "attacks", tla_int( walk->attacks ) );
    return state;
}

// the server and the client, the handshake run to the challenge at one instant
static bool pp_setup( pp_walk_t * walk, const int * server_mtus, int num_server_mtus )
{
    struct rocketnet_server_protocol_t * server = &walk->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    if ( !rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) )
    {
        return false;
    }
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = PP_HANDSHAKE_DEADLINE;
    server->send_rate = PP_SEND_RATE;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &walk->server_counters;
    for ( int i = 0; i < num_server_mtus; i++ )
    {
        server->link_mtus[i] = server_mtus[i];
    }
    server->num_link_mtus = num_server_mtus;
    if ( !server->encryption_map || !server->used_token_history || !server->client_slots || !server->acks )
    {
        return false;
    }
    if ( !rocketnet_address_parse( &walk->client_address, "198.51.100.7:56789" ) ||
         !rocketnet_address_parse( &walk->other_address, "198.51.100.8:56789" ) )
    {
        return false;
    }

    walk->now = PP_START_TIME;
    rocketnet_server_protocol_update( server, walk->now );

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = PP_TIMEOUT_SECONDS;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) )
    {
        return false;
    }

    struct rocketnet_client_protocol_t * client = &walk->client;
    rocketnet_client_protocol_init( client, &walk->client_counters );
    client->send_rate = PP_SEND_RATE;
    for ( int i = 0; i < (int) ( sizeof( pp_client_mtus ) / sizeof( pp_client_mtus[0] ) ); i++ )
    {
        client->link_mtus[i] = pp_client_mtus[i];
    }
    client->num_link_mtus = (int) ( sizeof( pp_client_mtus ) / sizeof( pp_client_mtus[0] ) );
    if ( !rocketnet_client_protocol_connect( client, token_data, walk->now, ROCKETNET_TEST_CREATE_TS ) )
    {
        return false;
    }

    // cookie request, cookie, request, challenge: the client's update sends
    // each at once (the advance's early send), so the round trip is zero
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < 4 && client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE; i++ )
    {
        int packet_bytes = 0;
        rocketnet_client_protocol_update( client, walk->now, packet, &packet_bytes );
        if ( packet_bytes <= 0 )
        {
            return false;
        }
        int response_bytes = 0;
        int payload_bytes = 0;
        int payload_client_index = -1;
        rocketnet_server_protocol_process_packet( server, &walk->client_address, packet, packet_bytes, ROCKETNET_TEST_CREATE_TS, walk->now,
                                                  response, &response_bytes, payload, &payload_bytes, &payload_client_index );
        if ( response_bytes <= 0 )
        {
            return false;
        }
        rocketnet_client_protocol_process_packet( client, response, response_bytes, walk->now, payload, (int) sizeof( payload ), &payload_bytes );
    }
    return client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE && client->probing &&
           client->rtt_estimate == 0.0 && pp_mapping( walk ) != NULL;
}

static void pp_teardown( pp_walk_t * walk )
{
    rocketnet_encryption_map_destroy( walk->server.encryption_map );
    rocketnet_used_token_history_destroy( walk->server.used_token_history );
    rocketnet_client_slots_destroy( walk->server.client_slots );
    rocketnet_server_acks_destroy( walk->server.acks );
}

// a probe that does not open: sealed under a key nobody holds
static pp_packet_t pp_forged_probe( pp_walk_t * walk, int src, int s )
{
    static const uint8_t zeros[ROCKETNET_CONNECTION_PROBE_MAX_WIRE_BYTES] = { 0 };
    static const uint8_t no_key[ROCKETNET_KEY_BYTES] = { 0 };
    static const uint8_t no_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 0 };
    pp_packet_t packet = { PP_PROBE, src, s, 0, 0, std::vector<uint8_t>( (size_t) s ) };
    const int bytes = rocketnet_write_sealed_packet( packet.bytes.data(), ROCKETNET_CONNECTION_PROBE_PACKET, walk->server.packet_filter,
                                                     walk->forged_sequence++, NULL, zeros, s - ROCKETNET_PACKET_OVERHEAD_BYTES,
                                                     walk->server.protocol_id, no_salt, NULL, no_key );
    if ( bytes != s )
    {
        fprintf( stderr, "tla_trace path-probe: a forged probe of %d bytes wrote %d\n", s, bytes );
        tla_value_errors()++;
    }
    return packet;
}

static int trace_path_probe( uint64_t seed, int steps, const char * out )
{
    pp_walk_t * walk = new pp_walk_t();
    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    // the seed's scenario: 0 a path that carries every size, 1 one whose
    // capacities are two candidates (the walk goes down), 2 one that
    // carries none (-14), 3 any path, ended by the silence timeout
    const int scenario = (int) ( seed % 4 );
    const bool short_server = ( ( seed / 4 ) % 2 ) == 1;
    const int num_server_mtus = short_server ? 3 : 4;
    const bool ok = pp_setup( walk, short_server ? pp_server_mtus_short : pp_client_mtus, num_server_mtus );
    if ( !ok )
    {
        fprintf( stderr, "tla_trace path-probe: the handshake did not reach probing\n" );
        pp_teardown( walk );
        delete walk;
        return 1;
    }

    // the model's constants, from the code: the candidates the client's own
    // list gives its server's family, the server's, the window, the ceiling
    std::vector<int> client_set( (size_t) walk->client.num_probe_candidates );
    for ( int i = 0; i < walk->client.num_probe_candidates; i++ )
    {
        client_set[(size_t) i] = walk->client.probe_candidates[i];
    }
    int server_candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES];
    const int num_server_candidates = rocketnet_link_mtu_candidates( walk->server.link_mtus, walk->server.num_link_mtus,
                                                                     walk->client_address.type, server_candidates );
    std::vector<int> server_list( server_candidates, server_candidates + num_server_candidates );
    std::vector<int> sizes = client_set;
    for ( size_t i = 0; i < server_list.size(); i++ )
    {
        bool seen = false;
        for ( size_t j = 0; j < sizes.size(); j++ )
        {
            seen = seen || sizes[j] == server_list[i];
        }
        if ( !seen )
        {
            sizes.push_back( server_list[i] );
        }
    }

    const int smallest = client_set.back();
    switch ( scenario )
    {
        case 0:
            walk->cap_up = ROCKETNET_MAX_PACKET_BYTES;
            walk->cap_down = ROCKETNET_MAX_PACKET_BYTES;
            break;
        case 1:
            walk->cap_up = client_set[1 + (size_t) random.below( (int) client_set.size() - 1 )];
            walk->cap_down = client_set[1 + (size_t) random.below( (int) client_set.size() - 1 )];
            break;
        case 2:
            walk->cap_up = smallest - 1 - random.below( 8 );
            walk->cap_down = random.below( 2 ) ? ROCKETNET_MAX_PACKET_BYTES : smallest - 1;
            break;
        default:
            walk->cap_up = client_set[(size_t) random.below( (int) client_set.size() )];
            walk->cap_down = client_set[(size_t) random.below( (int) client_set.size() )];
            break;
    }

    // the late reply: a walk on a path that carries every size holds its
    // deliveries to the client back early on, so replies arrive after their
    // windows and the first one delivered may be an earlier window's
    const int hold_down_until = scenario == 0 ? 30 : 0;
    const int timeout_at = scenario == 3 ? 12 + random.below( 40 ) : -1;
    walk->forged_sequence = 1;

    records.push_back( tla_step( "init", "init", "<<>>", pp_state( walk ) ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    bool failed = false;

    for ( int step = 0; step < steps && !failed; step++ )
    {
        const std::string phase = pp_phase( walk );
        const bool attempting = phase == "probe" || phase == "response";
        const bool terminal = phase == "failed" || phase == "timedout";
        const bool may_send = pp_may_send( walk );
        const bool answered = pp_answered( walk );
        struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
        const bool connected_mapping = mapping && mapping->control->client_index != -1;

        if ( phase == "timedout" )
        {
            report.act( "Done" );
            records.push_back( tla_step( "done", "Done", "<<>>", pp_state( walk ) ) );
            continue;
        }

        if ( timeout_at >= 0 && step >= timeout_at && attempting && may_send )
        {
            const double deadline = walk->client.last_receive_time + (double) walk->client.timeout_seconds;
            walk->now = deadline > walk->now ? deadline : walk->now;
            int packet_bytes = 0;
            rocketnet_client_protocol_update( &walk->client, walk->now, packet, &packet_bytes );
            const char * act = ( walk->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT && packet_bytes == 0 ) ? "ClientTimeout" : "NotATimeout";
            report.act( act );
            records.push_back( tla_step( "client_timeout", act, "<<>>", pp_state( walk ) ) );
            continue;
        }

        // the choices this step has, by weight
        enum { OP_CLIENT_UPDATE, OP_TICK, OP_SERVER_UPDATE, OP_SERVER_RECV, OP_CLIENT_RECV, OP_KEEP_ALIVE,
               OP_LOSE_UP, OP_LOSE_DOWN, OP_REPLAY, OP_ATTACK, OP_DONE, OP_COUNT };
        int weights[OP_COUNT] = { 0 };
        weights[OP_CLIENT_UPDATE] = attempting && may_send ? 6 : 0;
        weights[OP_TICK] = ( !may_send || answered ) ? 4 : 0;
        weights[OP_SERVER_UPDATE] = answered ? 1 : 0;
        weights[OP_SERVER_RECV] = !walk->up.empty() ? 5 : 0;
        weights[OP_CLIENT_RECV] = !walk->down.empty() ? ( step < hold_down_until ? 0 : 5 ) : 0;
        weights[OP_KEEP_ALIVE] = connected_mapping ? 1 : 0;
        bool honest_up = false;
        for ( size_t i = 0; i < walk->up.size(); i++ )
        {
            honest_up = honest_up || ( walk->up[i].src == PP_C && walk->up[i].n == 0 );
        }
        weights[OP_LOSE_UP] = honest_up ? ( attempting ? 2 : 1 ) : 0;
        weights[OP_LOSE_DOWN] = !walk->down.empty() ? 1 : 0;
        weights[OP_REPLAY] = honest_up ? ( attempting ? 4 : 1 ) : 0;
        // the attacker keeps at most two probes in flight, so the client's
        // own packets are not lost in a crowd
        int forged_up = 0;
        for ( size_t i = 0; i < walk->up.size(); i++ )
        {
            forged_up += walk->up[i].src != PP_C ? 1 : 0;
        }
        weights[OP_ATTACK] = forged_up < 2 ? 1 : 0;
        weights[OP_DONE] = terminal ? 1 : 0;
        int total = 0;
        for ( int i = 0; i < OP_COUNT; i++ )
        {
            total += weights[i];
        }
        int pick = random.below( total );
        int op = 0;
        while ( pick >= weights[op] )
        {
            pick -= weights[op];
            op++;
        }

        switch ( op )
        {
            case OP_CLIENT_UPDATE:
            {
                const uint64_t expired = walk->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED];
                const uint64_t probe_failed = walk->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_FAILED];
                int packet_bytes = 0;
                rocketnet_client_protocol_update( &walk->client, walk->now, packet, &packet_bytes );
                const int type = packet_bytes > 0 ? rocketnet_prefilter_check( packet, packet_bytes, walk->client.packet_filter ) : -1;
                const char * act = "Unknown";
                if ( walk->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_FAILED] != probe_failed && packet_bytes == 0 )
                {
                    act = "ClientExpireFail";
                }
                else if ( type == ROCKETNET_CONNECTION_PROBE_PACKET )
                {
                    act = walk->client_counters.values[ROCKETNET_COUNTER_CLIENT_PROBE_WINDOW_EXPIRED] != expired ? "ClientExpireNext" : "ClientSendProbe";
                    pp_packet_t sent = { PP_PROBE, PP_C, packet_bytes, 0, 0, std::vector<uint8_t>( packet, packet + packet_bytes ) };
                    pp_send( walk->up, walk->cap_up, sent );
                }
                else if ( type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
                {
                    act = "ClientSendResponse";
                    pp_packet_t sent = { PP_RESP, PP_C, walk->client.packet_bytes, 0, 0, std::vector<uint8_t>( packet, packet + packet_bytes ) };
                    pp_send( walk->up, walk->cap_up, sent );
                }
                report.act( act );
                records.push_back( tla_step( "client_update", act, "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_TICK:
            case OP_SERVER_UPDATE:
            {
                if ( op == OP_TICK )
                {
                    walk->now += 1.0 / PP_SEND_RATE;
                }
                rocketnet_server_protocol_update( &walk->server, walk->now );
                const char * act = op == OP_TICK ? "Tick" : "ServerUpdate";
                report.act( act );
                records.push_back( tla_step( op == OP_TICK ? "tick" : "server_update", act, "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_SERVER_RECV:
            {
                const size_t i = (size_t) random.below( (int) walk->up.size() );
                const pp_packet_t delivered = walk->up[i];
                walk->up.erase( walk->up.begin() + (long) i );
                const uint64_t withheld = walk->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WITHHELD];
                int response_bytes = 0;
                int payload_bytes = 0;
                int payload_client_index = -1;
                const int result = rocketnet_server_protocol_process_packet( &walk->server, delivered.src == PP_X ? &walk->other_address : &walk->client_address,
                                                                             delivered.bytes.data(), (int) delivered.bytes.size(), ROCKETNET_TEST_CREATE_TS, walk->now,
                                                                             response, &response_bytes, payload, &payload_bytes, &payload_client_index );
                if ( result == ROCKETNET_SERVER_PROCESS_SEND_PROBE && response_bytes > 0 )
                {
                    pp_packet_t reply = { PP_REPLY, PP_S, response_bytes, delivered.s, 0, std::vector<uint8_t>( response, response + response_bytes ) };
                    pp_send( walk->down, walk->cap_down, reply );
                }
                else if ( result == ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE && response_bytes > 0 )
                {
                    pp_packet_t keep_alive = { PP_KA, PP_S, 0, 0, 0, std::vector<uint8_t>( response, response + response_bytes ) };
                    pp_send( walk->down, walk->cap_down, keep_alive );
                }
                else if ( result != ROCKETNET_SERVER_PROCESS_IGNORED )
                {
                    fprintf( stderr, "tla_trace path-probe: the server's result %d is outside the model\n", result );
                    failed = true;
                }
                // an honest probe the allowance withheld is the model's loss
                if ( delivered.src == PP_C && delivered.n == 0 &&
                     walk->server_counters.values[ROCKETNET_COUNTER_CONNECTION_PROBE_WITHHELD] != withheld )
                {
                    walk->losses++;
                }
                const char * act = delivered.kind == PP_PROBE ? "ServerRecvProbe" : "ServerRecvResponse";
                report.act( act );
                records.push_back( tla_step( "server_recv", act, tla_sequence( std::vector<std::string>( 1, pp_record( delivered ) ) ), pp_state( walk ) ) );
                // the planted effect: a probe answered in an update whose one
                // reply the code had already spent
                if ( answered && result == ROCKETNET_SERVER_PROCESS_SEND_PROBE )
                {
                    report.planted( (int) records.size() );
                }
                break;
            }

            case OP_CLIENT_RECV:
            {
                const size_t i = (size_t) random.below( (int) walk->down.size() );
                const pp_packet_t delivered = walk->down[i];
                walk->down.erase( walk->down.begin() + (long) i );
                int payload_bytes = 0;
                rocketnet_client_protocol_process_packet( &walk->client, delivered.bytes.data(), (int) delivered.bytes.size(), walk->now,
                                                          payload, (int) sizeof( payload ), &payload_bytes );
                const char * act = delivered.kind == PP_REPLY ? "ClientRecvReply" : "ClientRecvKeepAlive";
                report.act( act );
                records.push_back( tla_step( "client_recv", act, tla_sequence( std::vector<std::string>( 1, pp_record( delivered ) ) ), pp_state( walk ) ) );
                break;
            }

            case OP_KEEP_ALIVE:
            {
                const int bytes = rocketnet_server_write_keep_alive( &walk->server, mapping->control->client_index, walk->now, response );
                if ( bytes <= 0 )
                {
                    fprintf( stderr, "tla_trace path-probe: the connected slot wrote no keep-alive\n" );
                    failed = true;
                    break;
                }
                pp_packet_t keep_alive = { PP_KA, PP_S, 0, 0, 0, std::vector<uint8_t>( response, response + bytes ) };
                pp_send( walk->down, walk->cap_down, keep_alive );
                report.act( "ServerSendKeepAlive" );
                records.push_back( tla_step( "keep_alive", "ServerSendKeepAlive", "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_LOSE_UP:
            case OP_REPLAY:
            {
                std::vector<size_t> honest;
                for ( size_t i = 0; i < walk->up.size(); i++ )
                {
                    if ( walk->up[i].src == PP_C && walk->up[i].n == 0 && ( op == OP_LOSE_UP || walk->up[i].kind == PP_PROBE ) )
                    {
                        honest.push_back( i );
                    }
                }
                if ( honest.empty() )
                {
                    step--;
                    break;
                }
                const size_t i = honest[(size_t) random.below( (int) honest.size() )];
                const pp_packet_t chosen = walk->up[i];
                if ( op == OP_LOSE_UP )
                {
                    walk->up.erase( walk->up.begin() + (long) i );
                    walk->losses++;
                    report.act( "LoseUp" );
                    records.push_back( tla_step( "lose_up", "LoseUp", tla_sequence( std::vector<std::string>( 1, pp_record( chosen ) ) ), pp_state( walk ) ) );
                    break;
                }
                pp_packet_t copy = chosen;
                copy.n = 1;
                bool present = false;
                for ( size_t j = 0; j < walk->up.size(); j++ )
                {
                    present = present || pp_same( walk->up[j], copy );
                }
                if ( present )
                {
                    step--;
                    break;
                }
                walk->up.push_back( copy );
                walk->attacks++;
                report.act( "Replay" );
                records.push_back( tla_step( "replay", "Replay", tla_sequence( std::vector<std::string>( 1, pp_record( chosen ) ) ), pp_state( walk ) ) );
                break;
            }

            case OP_LOSE_DOWN:
            {
                const size_t i = (size_t) random.below( (int) walk->down.size() );
                const pp_packet_t chosen = walk->down[i];
                walk->down.erase( walk->down.begin() + (long) i );
                walk->losses++;
                report.act( "LoseDown" );
                records.push_back( tla_step( "lose_down", "LoseDown", tla_sequence( std::vector<std::string>( 1, pp_record( chosen ) ) ), pp_state( walk ) ) );
                break;
            }

            case OP_ATTACK:
            {
                const int src = random.below( 2 ) ? PP_F : PP_X;
                const int s = sizes[(size_t) random.below( (int) sizes.size() )];
                const pp_packet_t forged = pp_forged_probe( walk, src, s );
                bool present = false;
                for ( size_t j = 0; j < walk->up.size(); j++ )
                {
                    present = present || pp_same( walk->up[j], forged );
                }
                if ( present )
                {
                    step--;
                    break;
                }
                walk->up.push_back( forged );
                walk->attacks++;
                report.act( "AttackProbe" );
                std::vector<std::string> args;
                args.push_back( tla_string( src == PP_F ? "F" : "X" ) );
                args.push_back( tla_int( s ) );
                records.push_back( tla_step( "attack", "AttackProbe", tla_sequence( args ), pp_state( walk ) ) );
                break;
            }

            default:
            {
                report.act( "Done" );
                records.push_back( tla_step( "done", "Done", "<<>>", pp_state( walk ) ) );
                break;
            }
        }
    }

    // the capacities the walk chose are the only ones the model starts from;
    // the loss and attack budgets are past anything the walk can spend
    std::string constants;
    char line[256];
    snprintf( line, sizeof( line ),
              "    Ceiling = %d\n    CapsUp = {%d}\n    CapsDown = {%d}\n    W = %d\n    MaxLoss = %d\n    AttackBudget = %d\n"
              "    Timed = FALSE\n    Broken = \"none\"\n",
              ROCKETNET_MAX_PACKET_BYTES, walk->cap_up, walk->cap_down, ROCKETNET_CLIENT_PROBE_WINDOW_SENDS, steps, steps );
    constants += line;
    constants += "    ClientSet = " + pp_int_set( client_set ) + "\n";
    constants += "    ServerList = " + pp_int_set( server_list ) + "\n";

    pp_teardown( walk );
    delete walk;

    if ( failed )
    {
        return 1;
    }

    report.print();

    return tla_write_trace( out, "MCTracePathProbe", seed, "TraceRNPathProbe", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "path-probe", trace_path_probe );
