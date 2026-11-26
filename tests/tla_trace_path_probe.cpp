/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The path probe after connect (sdk/rocketnet_client_protocol.cpp,
// sdk/rocketnet_server_protocol.cpp; standard/TRANSPORT.md t8.13, t11.4 and
// t12), checked by proofs/tla/TraceRNPathProbe.tla (README-TraceCheck.md)
// against proofs/tla/RNPathProbe.tla with Timed = FALSE, the code's own timing.
//
//   tla_trace --layer path-probe --seed N --out DIR [--steps K]
//
// writes DIR/MCTracePathProbeSeed<seed>.tla and .cfg
// (MCTracePathProbePlantSeed<seed> from tla_trace_plant_path_probe).
//
// The walk drives a real client protocol and a real server protocol through a
// path of its own: no socket, no thread, no clock but the walk's. Before the
// first record it runs the handshake to connected at one instant, so the
// round-trip estimate is zero and the first pair interval is
// ROCKETNET_LINK_MTU_PROBE_FIRST_INTERVAL_SECONDS, and both ends start at P0,
// the smallest candidate. The seed's scenario picks the lists and the path: a
// capacity each way, fixed for the walk; a packet above its direction's
// capacity, or sent during a loss burst that takes it, is dropped when sent,
// and the rest are in flight in a set (two copies of one packet are one),
// delivered in any order or never. An attacker replays a client packet in
// flight, sends a probe from an address with no mapping (X) or from the
// client's own address sealed under a key that is not the session key (F),
// or sends the client a reply sealed under such a key.
//
// Records (tla_step), each one call and the model action its own result
// names:
//   client_resolve   rocketnet_client_protocol_probe_resolve, called when
//                    the pair is due and one is out: ClientResolvePair
//   client_rule_out  rocketnet_client_protocol_probe_rule_out, called when
//                    the pair is due, none is out and the candidate has its
//                    strikes: ClientRuleOut (1) or ClientFail (-1)
//   client_send      rocketnet_client_protocol_probe_send_pair, called when
//                    the pair is due, none is out and the candidate has
//                    fewer strikes: ClientSendPair
//                    (rocketnet_client_protocol_probe_update calls the three
//                    in this order, in one call; the walk calls them one a
//                    step, which the model allows, as the code does)
//   client_confirm   rocketnet_client_protocol_probe_update when its confirm
//                    is due: ClientSendConfirm (it wrote a confirm)
//   client_recv      rocketnet_client_protocol_process_packet of a packet in
//                    flight down: ClientRecvSmall, ClientRecvLarge,
//                    ClientRecvForged or ClientRecvConfirmEcho, by the
//                    packet the walk delivered
//   server_recv      rocketnet_server_protocol_process_packet of a packet in
//                    flight up: ServerRecvProbe, ServerRecvForged or
//                    ServerRecvConfirm, by the packet delivered; its reply
//                    (SEND_PROBE) goes down at the size the server wrote
//   tick             the walk's clock to the next pair's time and
//                    rocketnet_server_protocol_update: Tick
//   server_update    rocketnet_server_protocol_update at the same time:
//                    ServerUpdate
//   burst, replay, attack, attack_reply, done   the walk's path and
//                    attacker: LossBurst(k), Replay(p), AttackProbe(p),
//                    AttackReply(s), Done
//
// The clock is not a model variable: the walk moves it only in tick, which
// crosses the one instant the model sees, the pair's due time (due), and
// silently when it does not cross it (to a confirm's next send). A tick
// goes PP_TICK_LATE past the due time.
//
// Every model variable is read from the code but the path's and the
// attacker's (the client's from the copy taken at its -14 once it has
// failed, failed_client below): cP from rocketnet_client_protocol_packet_bytes, sP from the
// client's mapping (rocketnet_server_protocol_mapping_packet_bytes), phase
// from the client's state and probing flag, ruled the candidates before
// probe_index, strikes probe_strikes, out probe_pair_out, pairNo the parity
// of probe_pair, pairS the size of the large probe the client last wrote
// (P0 before it wrote one), smallSeen and largeSeen probe_pair_small and
// probe_pair_large, due the clock against probe_next_time, confirmS
// confirm_bytes, ansSmall and ansLarge the mapping's probe_update and
// probe_large_update against the server's update_number. capUp, capDown,
// burst, bursts, pairInBurst, burstStrikes, attacks, up and down are the
// walk's: pairInBurst and burstStrikes follow the model's loss assumption
// (a burst meets a pair sent in it or out when it starts; the strikes of
// the current row taken on pairs a burst met), and the walk starts a burst,
// or sends a pair into a burst of large packets, only while that allows. A
// packet in
// flight is the model's record: its kind, its source, its size (0 for the
// 34-byte small probe, confirm and echo, and the confirm's or echo's size
// field otherwise kept in s), the parity of its pair number and whether it
// is a replayed copy.
//
// The plant (ROCKETNET_PATH_PROBE_PLANT, in rocketnet_server_process_probe)
// spends the mapping's allowance before the open, so a packet forged at the
// client's address spends it. Its effect shows at the first server_recv of
// a forged packet after which an allowance of the update is spent, which the
// clean code never does (ServerRecvForged spends nothing); the walk reports
// that record.

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
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_test_vectors.h"
#include "tla_trace_record.h"

#define PP_SEND_RATE        64.0
#define PP_START_TIME       100.0
// how far past the due time a tick moves the clock: 1/1024 s
#define PP_TICK_LATE        ( 1.0 / 1024.0 )
// the token's silence timeout and the server's handshake deadline, past any
// walk: neither end times the connection out
#define PP_TIMEOUT_SECONDS  3600
#define PP_HANDSHAKE_DEADLINE 60.0
// the most loss bursts a walk starts, the model's MaxBursts
#define PP_MAX_BURSTS       3

// the lists: four candidates over IPv4, 1452, 1372, 1252 and 548; the
// server's without 1400 (a probe of 1372 is off its list); and two
// candidates, 1452 and 548, for the walk that fails at P0
static const int pp_mtus_four[] = { 1500, 1400, 1280, 576 };
static const int pp_mtus_short[] = { 1500, 1280, 576 };
static const int pp_mtus_two[] = { 1500, 576 };

enum pp_kind_t { PP_SMALL, PP_LARGE, PP_CONFIRM, PP_ECHO };
enum pp_source_t { PP_C, PP_F, PP_X, PP_S };

struct pp_packet_t
{
    int kind;
    int src;
    int s;
    int pn;
    int n;
    std::vector<uint8_t> bytes;
};

static bool pp_same( const pp_packet_t & a, const pp_packet_t & b )
{
    return a.kind == b.kind && a.src == b.src && a.s == b.s && a.pn == b.pn && a.n == b.n;
}

static int pp_wire( const pp_packet_t & packet )
{
    return packet.kind == PP_LARGE ? packet.s : 0;
}

static std::string pp_record( const pp_packet_t & packet )
{
    static const char * kinds[] = { "small", "large", "confirm", "echo" };
    static const char * sources[] = { "C", "F", "X", "S" };
    tla_record_t record;
    record.add( "k", tla_string( kinds[packet.kind] ) );
    record.add( "src", tla_string( sources[packet.src] ) );
    record.add( "s", tla_int( packet.s ) );
    record.add( "pn", tla_int( packet.pn ) );
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

static std::string pp_args( const std::string & value )
{
    return tla_sequence( std::vector<std::string>( 1, value ) );
}

static bool pp_holds( const std::vector<pp_packet_t> & flight, const pp_packet_t & packet )
{
    for ( size_t i = 0; i < flight.size(); i++ )
    {
        if ( pp_same( flight[i], packet ) )
        {
            return true;
        }
    }
    return false;
}

struct pp_walk_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_client_protocol_t client;
    // the client as it stood at its -14: the client's set state resets every
    // field of the connection (TRANSPORT.md t13.4), and the model's
    // ClientFail changes nothing but phase, so once failed every client
    // variable is read from this copy and phase from the client itself
    struct rocketnet_client_protocol_t failed_client;
    bool has_failed_client;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_address_t client_address;
    struct rocketnet_address_t other_address;
    double now;
    int cap_up;
    int cap_down;
    // the model's burst: 0 off, 1 "all", 2 "large"
    int burst;
    int bursts;
    // the model's pairInBurst and burstStrikes
    bool pair_in_burst;
    int burst_strikes;
    int attacks;
    int pair_bytes;
    std::vector<pp_packet_t> up;
    std::vector<pp_packet_t> down;
    uint64_t forged_sequence;
};

static const char * pp_burst_name( int burst )
{
    return burst == 1 ? "all" : ( burst == 2 ? "large" : "off" );
}

// the path at the send: a burst takes what it covers, and a packet above the
// direction's capacity never arrives; a packet already in flight is the one
// packet the set holds (the model's SendUp and SendDown)
static void pp_send( pp_walk_t * walk, std::vector<pp_packet_t> & flight, int capacity, const pp_packet_t & packet )
{
    const bool lost = walk->burst == 1 || ( walk->burst == 2 && pp_wire( packet ) > 0 );
    if ( lost || pp_wire( packet ) > capacity )
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

static bool pp_answered_small( pp_walk_t * walk )
{
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    return mapping && mapping->probe_update == walk->server.update_number + 1;
}

static bool pp_answered_large( pp_walk_t * walk )
{
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    return mapping && mapping->probe_large_update == walk->server.update_number + 1;
}

static const char * pp_phase( const pp_walk_t * walk )
{
    if ( walk->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_MTU_PROBE_FAILED )
    {
        return "failed";
    }
    if ( walk->client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        return "other";
    }
    return walk->client.probing ? "probing" : "done";
}

// the client the model variables are read from (failed_client, above)
static const struct rocketnet_client_protocol_t * pp_client( const pp_walk_t * walk )
{
    return walk->has_failed_client ? &walk->failed_client : &walk->client;
}

static bool pp_due( const pp_walk_t * walk )
{
    return walk->now >= pp_client( walk )->probe_next_time;
}

// the model's BurstMayMeet: a burst may still meet a pair that can strike
static bool pp_burst_may_meet( const pp_walk_t * walk )
{
    return walk->burst_strikes < ROCKETNET_LINK_MTU_PROBE_STRIKES - 1;
}

static tla_record_t pp_state( pp_walk_t * walk )
{
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    const struct rocketnet_client_protocol_t * client = pp_client( walk );
    std::vector<int> ruled;
    for ( int i = 0; i < client->probe_index; i++ )
    {
        ruled.push_back( client->probe_candidates[i] );
    }
    tla_record_t state;
    state.add( "capUp", tla_int( walk->cap_up ) );
    state.add( "capDown", tla_int( walk->cap_down ) );
    state.add( "cP", tla_int( rocketnet_client_protocol_packet_bytes( client ) ) );
    state.add( "sP", tla_int( mapping ? rocketnet_server_protocol_mapping_packet_bytes( mapping ) : 0 ) );
    state.add( "phase", tla_string( pp_phase( walk ) ) );
    state.add( "ruled", pp_int_set( ruled ) );
    state.add( "strikes", tla_int( client->probe_strikes ) );
    state.add( "out", tla_bool( client->probe_pair_out != 0 ) );
    state.add( "pairNo", tla_int( (int) ( client->probe_pair & 1 ) ) );
    state.add( "pairS", tla_int( walk->pair_bytes ) );
    state.add( "pairInBurst", tla_bool( walk->pair_in_burst ) );
    state.add( "smallSeen", tla_bool( client->probe_pair_small != 0 ) );
    state.add( "largeSeen", tla_bool( client->probe_pair_large != 0 ) );
    state.add( "due", tla_bool( pp_due( walk ) ) );
    state.add( "confirmS", tla_int( client->confirm_bytes ) );
    state.add( "ansSmall", tla_bool( pp_answered_small( walk ) ) );
    state.add( "ansLarge", tla_bool( pp_answered_large( walk ) ) );
    state.add( "burst", tla_string( pp_burst_name( walk->burst ) ) );
    state.add( "bursts", tla_int( walk->bursts ) );
    state.add( "burstStrikes", tla_int( walk->burst_strikes ) );
    state.add( "attacks", tla_int( walk->attacks ) );
    state.add( "up", pp_set( walk->up ) );
    state.add( "down", pp_set( walk->down ) );
    return state;
}

// the server and the client, the handshake run to connected at one instant
static bool pp_setup( pp_walk_t * walk, const int * client_mtus, int num_client_mtus, const int * server_mtus, int num_server_mtus )
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
    for ( int i = 0; i < num_client_mtus; i++ )
    {
        client->link_mtus[i] = client_mtus[i];
    }
    client->num_link_mtus = num_client_mtus;
    if ( !rocketnet_client_protocol_connect( client, token_data, walk->now, ROCKETNET_TEST_CREATE_TS ) )
    {
        return false;
    }

    // cookie request, cookie, request, challenge, response, keep-alive: the
    // client's update sends each at once (the advance's early send), so the
    // round trip is zero
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < 6 && client->state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; i++ )
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
    struct rocketnet_encryption_mapping_t * mapping = pp_mapping( walk );
    return client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && client->probing && client->rtt_estimate == 0.0 &&
           mapping != NULL && mapping->control->client_index != -1;
}

static void pp_teardown( pp_walk_t * walk )
{
    rocketnet_encryption_map_destroy( walk->server.encryption_map );
    rocketnet_used_token_history_destroy( walk->server.used_token_history );
    rocketnet_client_slots_destroy( walk->server.client_slots );
    rocketnet_server_acks_destroy( walk->server.acks );
}

// a probe sealed under a key that is not the session key, wire_bytes long, on
// a nonzero sequence: what F and X send the server and F sends the client
static std::vector<uint8_t> pp_forged_bytes( pp_walk_t * walk, int wire_bytes )
{
    static const uint8_t zeros[ROCKETNET_CONNECTION_PROBE_MAX_WIRE_BYTES] = { 0 };
    static const uint8_t no_key[ROCKETNET_KEY_BYTES] = { 0 };
    static const uint8_t no_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 0 };
    std::vector<uint8_t> bytes( (size_t) wire_bytes );
    const int written = rocketnet_write_sealed_packet( bytes.data(), ROCKETNET_CONNECTION_PROBE_PACKET, walk->server.packet_filter,
                                                       walk->forged_sequence++, NULL, zeros, wire_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES,
                                                       walk->server.protocol_id, no_salt, NULL, no_key );
    if ( written != wire_bytes )
    {
        fprintf( stderr, "tla_trace path-probe: a forged probe of %d bytes wrote %d\n", wire_bytes, written );
        tla_value_errors()++;
    }
    return bytes;
}

static int trace_path_probe( uint64_t seed, int steps, const char * out )
{
    pp_walk_t * walk = new pp_walk_t();
    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    // the seed's scenario:
    //   0 a path that carries every size, both ends at 1452 one pair after
    //     connect; then the attacker, replays, bursts and the confirm
    //   1 a path whose capacity up is 1372: 1452 is ruled out by its
    //     strikes and the walk raises to 1372
    //   2 two candidates and a path that carries no large probe: both are
    //     ruled out, P0 last, and the client fails -14
    //   3 any capacities of the candidates and a server list without 1400,
    //     with the attacker and bursts throughout
    const int scenario = (int) ( seed % 4 );
    const int * client_mtus = scenario == 2 ? pp_mtus_two : pp_mtus_four;
    const int num_client_mtus = scenario == 2 ? 2 : 4;
    const int * server_mtus = scenario == 3 ? pp_mtus_short : client_mtus;
    const int num_server_mtus = scenario == 3 ? 3 : num_client_mtus;
    if ( !pp_setup( walk, client_mtus, num_client_mtus, server_mtus, num_server_mtus ) )
    {
        fprintf( stderr, "tla_trace path-probe: the handshake did not reach connected and probing\n" );
        pp_teardown( walk );
        delete walk;
        return 1;
    }

    // the model's constants, from the code: the candidates the client's own
    // list gives its server's family, and the server's
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
    walk->pair_bytes = smallest;
    switch ( scenario )
    {
        case 0:
            walk->cap_up = ROCKETNET_MAX_PACKET_BYTES;
            walk->cap_down = ROCKETNET_MAX_PACKET_BYTES;
            break;
        case 1:
            walk->cap_up = client_set[1];
            walk->cap_down = ROCKETNET_MAX_PACKET_BYTES;
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

    // the noise each scenario carries: the rule-out and failing walks need
    // their strikes in a row inside the walk, so they carry little
    const bool noisy = scenario == 0 || scenario == 3;
    walk->forged_sequence = 1;

    records.push_back( tla_step( "init", "init", "<<>>", pp_state( walk ) ) );

    uint8_t small[ROCKETNET_RECEIVE_CEILING];
    uint8_t large[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    bool failed = false;

    for ( int step = 0; step < steps && !failed; step++ )
    {
        const std::string phase = pp_phase( walk );
        const bool probing = phase == "probing";
        const bool terminal = phase == "done" || phase == "failed";
        const bool due = pp_due( walk );
        const bool confirming = walk->client.confirm_bytes > 0 && phase == "done";
        const bool confirm_due = confirming && walk->now >= walk->client.confirm_next_time;
        const bool answered = pp_answered_small( walk ) || pp_answered_large( walk );

        // the next instant the model sees: the due time, a tick moving the
        // clock PP_TICK_LATE past it
        const double next_time = walk->client.probe_next_time + PP_TICK_LATE;
        const bool clock_event = !due;

        // the loss assumption: a pair goes into a burst of large packets,
        // and a burst starts on a pair that is out and can still strike,
        // only while a burst may meet another strike
        const bool sends_next = !walk->client.probe_pair_out && walk->client.probe_strikes < ROCKETNET_LINK_MTU_PROBE_STRIKES;
        const bool pair_allowed = !( sends_next && walk->burst == 2 ) || pp_burst_may_meet( walk );
        const bool pair_out = walk->client.probe_pair_out != 0;
        const bool burst_all_allowed = !( pair_out && walk->client.probe_pair_small ) || pp_burst_may_meet( walk );
        const bool burst_large_allowed = !pair_out || pp_burst_may_meet( walk );

        bool pair_in_flight = false;
        bool honest_up = false;
        int forged_up = 0;
        for ( size_t i = 0; i < walk->up.size(); i++ )
        {
            pair_in_flight = pair_in_flight || ( walk->up[i].src == PP_C && walk->up[i].kind != PP_CONFIRM );
            honest_up = honest_up || ( walk->up[i].src == PP_C && walk->up[i].n == 0 );
            forged_up += walk->up[i].src != PP_C ? 1 : 0;
        }
        for ( size_t i = 0; i < walk->down.size(); i++ )
        {
            pair_in_flight = pair_in_flight || ( walk->down[i].src == PP_S && walk->down[i].kind != PP_ECHO );
        }

        enum { OP_CLIENT_PAIR, OP_CONFIRM, OP_CLOCK, OP_SERVER_UPDATE, OP_SERVER_RECV, OP_CLIENT_RECV,
               OP_BURST_START, OP_BURST_END, OP_REPLAY, OP_ATTACK, OP_ATTACK_REPLY, OP_SILENT, OP_DONE, OP_COUNT };
        int weights[OP_COUNT] = { 0 };
        weights[OP_CLIENT_PAIR] = probing && due && pair_allowed ? 8 : 0;
        weights[OP_CONFIRM] = confirm_due ? 4 : 0;
        // the clock moves when the pair is in, or now and then without it
        weights[OP_CLOCK] = clock_event && phase != "failed" ? ( pair_in_flight ? 1 : 8 ) : 0;
        weights[OP_SERVER_UPDATE] = answered ? 1 : 0;
        weights[OP_SERVER_RECV] = !walk->up.empty() ? 8 : 0;
        weights[OP_CLIENT_RECV] = !walk->down.empty() && phase != "failed" ? 8 : 0;
        weights[OP_BURST_START] = noisy && walk->burst == 0 && walk->bursts < PP_MAX_BURSTS && ( burst_all_allowed || burst_large_allowed ) ? 1 : 0;
        weights[OP_BURST_END] = walk->burst != 0 ? 3 : 0;
        weights[OP_REPLAY] = honest_up ? ( noisy ? 2 : 1 ) : 0;
        weights[OP_ATTACK] = forged_up < 2 && walk->attacks < steps ? ( noisy ? 2 : 0 ) : 0;
        weights[OP_ATTACK_REPLY] = walk->attacks < steps ? ( noisy ? 1 : 0 ) : 0;
        // the clock to the confirm's next send, crossing nothing the model sees
        weights[OP_SILENT] = confirming && !confirm_due && ( !clock_event || walk->client.confirm_next_time < next_time ) ? 3 : 0;
        weights[OP_DONE] = terminal ? 1 : 0;
        int total = 0;
        for ( int i = 0; i < OP_COUNT; i++ )
        {
            total += weights[i];
        }
        if ( total == 0 )
        {
            fprintf( stderr, "tla_trace path-probe: no step is open in phase %s\n", phase.c_str() );
            failed = true;
            break;
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
            case OP_CLIENT_PAIR:
            {
                // the step the code's probe update takes next: resolve the
                // pair that is out, else rule out a candidate with its
                // strikes, else send the next pair
                if ( walk->client.probe_pair_out )
                {
                    const int strikes_before = walk->client.probe_strikes;
                    const bool small_before = walk->client.probe_pair_small != 0;
                    const int resolved = rocketnet_client_protocol_probe_resolve( &walk->client );
                    if ( walk->client.probe_strikes > strikes_before )
                    {
                        walk->burst_strikes += walk->pair_in_burst ? 1 : 0;
                    }
                    else if ( !small_before )
                    {
                        walk->burst_strikes = 0;
                    }
                    const char * act = resolved == 1 ? "ClientResolvePair" : "NotAResolve";
                    report.act( act );
                    records.push_back( tla_step( "client_resolve", act, "<<>>", pp_state( walk ) ) );
                    break;
                }
                if ( walk->client.probe_strikes >= ROCKETNET_LINK_MTU_PROBE_STRIKES )
                {
                    // the client as it stands before a -14 resets it
                    // (failed_client, above)
                    memcpy( &walk->failed_client, &walk->client, sizeof( walk->client ) );
                    const int ruled = rocketnet_client_protocol_probe_rule_out( &walk->client );
                    const char * act = ruled == 1 ? "ClientRuleOut" : ( ruled == -1 ? "ClientFail" : "NotARuleOut" );
                    report.act( act );
                    walk->has_failed_client = ruled == -1;
                    walk->burst_strikes = ruled == 1 ? 0 : walk->burst_strikes;
                    records.push_back( tla_step( "client_rule_out", act, "<<>>", pp_state( walk ) ) );
                    break;
                }
                int small_bytes = 0;
                int large_bytes = 0;
                rocketnet_client_protocol_probe_send_pair( &walk->client, walk->now, small, &small_bytes, large, &large_bytes );
                const char * act = small_bytes == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES && large_bytes > 0 ? "ClientSendPair" : "NotAPair";
                // the model drops what is left of the pair two back, which
                // shares this pair number's parity, both ways
                const int pn = (int) ( walk->client.probe_pair & 1 );
                for ( size_t i = walk->up.size(); i-- > 0; )
                {
                    if ( walk->up[i].src == PP_C && ( walk->up[i].kind == PP_SMALL || walk->up[i].kind == PP_LARGE ) && walk->up[i].pn == pn )
                    {
                        walk->up.erase( walk->up.begin() + (long) i );
                    }
                }
                for ( size_t i = walk->down.size(); i-- > 0; )
                {
                    if ( walk->down[i].src == PP_S && ( walk->down[i].kind == PP_SMALL || walk->down[i].kind == PP_LARGE ) && walk->down[i].pn == pn )
                    {
                        walk->down.erase( walk->down.begin() + (long) i );
                    }
                }
                walk->pair_bytes = large_bytes;
                walk->pair_in_burst = walk->burst != 0;
                pp_packet_t sent_small = { PP_SMALL, PP_C, 0, pn, 0, std::vector<uint8_t>( small, small + small_bytes ) };
                pp_packet_t sent_large = { PP_LARGE, PP_C, large_bytes, pn, 0, std::vector<uint8_t>( large, large + large_bytes ) };
                pp_send( walk, walk->up, walk->cap_up, sent_small );
                pp_send( walk, walk->up, walk->cap_up, sent_large );
                report.act( act );
                records.push_back( tla_step( "client_send", act, "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_CONFIRM:
            {
                int small_bytes = 0;
                int large_bytes = 0;
                const int confirm = walk->client.confirm_bytes;
                rocketnet_client_protocol_probe_update( &walk->client, walk->now, small, &small_bytes, large, &large_bytes );
                const char * act = small_bytes == ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES && large_bytes == 0 ? "ClientSendConfirm" : "NotAConfirm";
                pp_packet_t sent = { PP_CONFIRM, PP_C, confirm, 0, 0, std::vector<uint8_t>( small, small + small_bytes ) };
                pp_send( walk, walk->up, walk->cap_up, sent );
                report.act( act );
                records.push_back( tla_step( "client_confirm", act, "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_CLOCK:
            {
                walk->now = next_time;
                rocketnet_server_protocol_update( &walk->server, walk->now );
                report.act( "Tick" );
                records.push_back( tla_step( "tick", "Tick", "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_SILENT:
            {
                walk->now = walk->client.confirm_next_time;
                step--;
                break;
            }

            case OP_SERVER_UPDATE:
            {
                rocketnet_server_protocol_update( &walk->server, walk->now );
                report.act( "ServerUpdate" );
                records.push_back( tla_step( "server_update", "ServerUpdate", "<<>>", pp_state( walk ) ) );
                break;
            }

            case OP_SERVER_RECV:
            {
                const size_t i = (size_t) random.below( (int) walk->up.size() );
                const pp_packet_t delivered = walk->up[i];
                walk->up.erase( walk->up.begin() + (long) i );
                const bool small_before = pp_answered_small( walk );
                const bool large_before = pp_answered_large( walk );
                int response_bytes = 0;
                int payload_bytes = 0;
                int payload_client_index = -1;
                const int result = rocketnet_server_protocol_process_packet( &walk->server, delivered.src == PP_X ? &walk->other_address : &walk->client_address,
                                                                             delivered.bytes.data(), (int) delivered.bytes.size(), ROCKETNET_TEST_CREATE_TS, walk->now,
                                                                             response, &response_bytes, payload, &payload_bytes, &payload_client_index );
                if ( result == ROCKETNET_SERVER_PROCESS_SEND_PROBE && response_bytes > 0 )
                {
                    const int kind = delivered.kind == PP_CONFIRM ? PP_ECHO : delivered.kind;
                    const int s = delivered.kind == PP_SMALL ? 0 : delivered.s;
                    if ( pp_wire( delivered ) != ( kind == PP_LARGE ? response_bytes : 0 ) ||
                         ( kind != PP_LARGE && response_bytes != ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES ) )
                    {
                        fprintf( stderr, "tla_trace path-probe: the server answered %d bytes to %s\n", response_bytes, pp_record( delivered ).c_str() );
                        failed = true;
                    }
                    pp_packet_t reply = { kind, PP_S, s, delivered.pn, 0, std::vector<uint8_t>( response, response + response_bytes ) };
                    pp_send( walk, walk->down, walk->cap_down, reply );
                }
                else if ( result != ROCKETNET_SERVER_PROCESS_IGNORED )
                {
                    fprintf( stderr, "tla_trace path-probe: the server's result %d is outside the model\n", result );
                    failed = true;
                }
                const char * act = delivered.src == PP_F ? "ServerRecvForged" : ( delivered.kind == PP_CONFIRM ? "ServerRecvConfirm" : "ServerRecvProbe" );
                report.act( act );
                records.push_back( tla_step( "server_recv", act, pp_args( pp_record( delivered ) ), pp_state( walk ) ) );
                // the planted effect: a forged packet that spent an allowance
                if ( delivered.src == PP_F && ( ( !small_before && pp_answered_small( walk ) ) || ( !large_before && pp_answered_large( walk ) ) ) )
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
                const char * act = delivered.src == PP_F ? "ClientRecvForged"
                                 : ( delivered.kind == PP_SMALL ? "ClientRecvSmall"
                                 : ( delivered.kind == PP_LARGE ? "ClientRecvLarge" : "ClientRecvConfirmEcho" ) );
                report.act( act );
                records.push_back( tla_step( "client_recv", act, pp_args( pp_record( delivered ) ), pp_state( walk ) ) );
                break;
            }

            case OP_BURST_START:
            case OP_BURST_END:
            {
                int kind = op == OP_BURST_END ? 0 : 1 + random.below( 2 );
                if ( kind == 1 && !burst_all_allowed )
                {
                    kind = 2;
                }
                else if ( kind == 2 && !burst_large_allowed )
                {
                    kind = 1;
                }
                if ( kind != 0 )
                {
                    walk->pair_in_burst = pair_out || walk->pair_in_burst;
                    // the burst takes what is in flight that it covers
                    for ( size_t j = walk->up.size(); j-- > 0; )
                    {
                        if ( kind == 1 || pp_wire( walk->up[j] ) > 0 )
                        {
                            walk->up.erase( walk->up.begin() + (long) j );
                        }
                    }
                    for ( size_t j = walk->down.size(); j-- > 0; )
                    {
                        if ( kind == 1 || pp_wire( walk->down[j] ) > 0 )
                        {
                            walk->down.erase( walk->down.begin() + (long) j );
                        }
                    }
                    walk->bursts++;
                }
                walk->burst = kind;
                report.act( "LossBurst" );
                records.push_back( tla_step( "burst", "LossBurst", pp_args( tla_string( pp_burst_name( kind ) ) ), pp_state( walk ) ) );
                break;
            }

            case OP_REPLAY:
            {
                std::vector<size_t> honest;
                for ( size_t j = 0; j < walk->up.size(); j++ )
                {
                    pp_packet_t copy = walk->up[j];
                    copy.n = 1;
                    if ( walk->up[j].src == PP_C && walk->up[j].n == 0 && !pp_holds( walk->up, copy ) )
                    {
                        honest.push_back( j );
                    }
                }
                if ( honest.empty() )
                {
                    step--;
                    break;
                }
                const pp_packet_t chosen = walk->up[honest[(size_t) random.below( (int) honest.size() )]];
                pp_packet_t copy = chosen;
                copy.n = 1;
                walk->up.push_back( copy );
                walk->attacks++;
                report.act( "Replay" );
                records.push_back( tla_step( "replay", "Replay", pp_args( pp_record( chosen ) ), pp_state( walk ) ) );
                break;
            }

            case OP_ATTACK:
            {
                // X: a small or large probe from an address with no mapping;
                // F: a small, large or confirm at the client's address
                const int src = random.below( 2 ) ? PP_F : PP_X;
                const int kind = random.below( src == PP_F ? 3 : 2 );
                const int s = kind == PP_SMALL ? 0 : sizes[(size_t) random.below( (int) sizes.size() )];
                pp_packet_t forged = { kind, src, s, 0, 0, std::vector<uint8_t>() };
                if ( pp_holds( walk->up, forged ) )
                {
                    step--;
                    break;
                }
                forged.bytes = pp_forged_bytes( walk, kind == PP_LARGE ? s : ROCKETNET_CONNECTION_PROBE_SMALL_WIRE_BYTES );
                walk->up.push_back( forged );
                walk->attacks++;
                report.act( "AttackProbe" );
                records.push_back( tla_step( "attack", "AttackProbe", pp_args( pp_record( forged ) ), pp_state( walk ) ) );
                break;
            }

            case OP_ATTACK_REPLY:
            {
                const int s = sizes[(size_t) random.below( (int) sizes.size() )];
                pp_packet_t forged = { PP_LARGE, PP_F, s, 0, 0, std::vector<uint8_t>() };
                if ( pp_holds( walk->down, forged ) )
                {
                    step--;
                    break;
                }
                forged.bytes = pp_forged_bytes( walk, s );
                walk->down.push_back( forged );
                walk->attacks++;
                report.act( "AttackReply" );
                records.push_back( tla_step( "attack_reply", "AttackReply", pp_args( tla_int( s ) ), pp_state( walk ) ) );
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
    // the burst window and the attack budget are past anything the walk spends
    std::string constants;
    char line[256];
    snprintf( line, sizeof( line ),
              "    CapsUp = {%d}\n    CapsDown = {%d}\n    K = %d\n    MaxBursts = %d\n    AttackBudget = %d\n"
              "    Forger = FALSE\n    Timed = FALSE\n    Broken = \"none\"\n",
              walk->cap_up, walk->cap_down, ROCKETNET_LINK_MTU_PROBE_STRIKES, PP_MAX_BURSTS, steps );
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
