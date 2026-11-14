/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server send pass's cursor at a capacity cut in every round
// (rn_message_object_send_packets; proofs/tla/RNServerSendFair.tla
// CursorAsFullWalk, RNSendRotation.tla Visit). The pass serves one frame per
// connection per round, the first round over every slot from the cursor and
// each later round over the last round's writers, and stops when the
// transport's send capacity is spent. The cursor it leaves is the one a walk
// over every connected slot would leave: past each slot visited, and at a cut
// the position the cut connection held before its visit, which for a later
// round is the position it held in the first round (for the first connected
// slot of a pass, past the last connected slot).
//
// A server message object of a few slots runs over a fake transport whose
// capacity is a number of frame starts per flush; some slots are never bound
// (their connection id is 0), so positions and indices differ. Every
// connection's sends are 1000-byte reliable messages, one frame each, and the
// frames of a flush are acknowledged at the next update. The test keeps its
// own model of the rule: per flush, the exact clients served, in order, and the
// cursor left, for cuts in round 1, 2 and 3 and passes that end uncut; and over
// many flushes at a capacity below the connected count, every connected client
// gets its turn. Time is an argument from 1000.0; nothing sleeps and nothing
// opens a socket.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "loopback_hub.h"

#define check rocketnet_unit_check

// A caller's misuse (an index outside the slots, a NULL address) asserts in a
// debug build and is refused in every build: the cases that make one count
// the asserts instead of stopping, and check the refusal.

static int g_gaps_asserts = 0;

static void gaps_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    g_gaps_asserts++;
}

struct gaps_assert_scope_t
{
    rocketnet_assert_function_t saved;

    gaps_assert_scope_t()
    {
        saved = rn_assert_function;
        g_gaps_asserts = 0;
        rocketnet_assert_function( gaps_assert_hook );
    }

    ~gaps_assert_scope_t()
    {
        rocketnet_assert_function( saved );
    }
};

#define CR_MAX_SLOTS     8
#define CR_PAYLOAD_INDEX 32
#define CR_MESSAGE_BYTES 1000
#define CR_T0            1000.0
#define CR_DT            ( 1.0 / 60.0 )

struct cr_transport_t
{
    uint64_t next_id;
    int capacity;                           // frame starts allowed per flush (send_capacity_available)
    int started;                            // frames started this flush
    int refused;                            // starts refused past the capacity
    int aborts;                             // frames aborted
    int bound[CR_MAX_SLOTS];                // 1: the slot holds a connection
    int disconnects;                        // link.disconnect calls
    int fail_finish;                        // 1: finish_packet refuses every frame (a send that fails)
    std::vector<int> acked_slot;            // the last flush's frames, acknowledged at this update
    std::vector<uint64_t> acked;
    std::vector<int> pending_slot;          // this flush's frames, in the order they were finished
    std::vector<uint64_t> pending;
    std::vector<int> inbound_slot;          // the payloads the next update presents, by client index
    std::vector<uint8_t *> inbound_data;
    std::vector<int> inbound_bytes;
    std::vector<std::vector<uint8_t>> sent;     // every finished frame's payload, in finish order
};

// start_packet hands back a 16-byte aligned frame. Kept out of the transport
// struct: an alignas member there pads the struct (MSVC C4324).
alignas( 16 ) static uint8_t cr_frame[CR_PAYLOAD_INDEX + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
static uint8_t cr_inbound_payload[8] = { 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE };

static void cr_update( void * transport, double time )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    (void) time;
    t->acked.swap( t->pending );
    t->acked_slot.swap( t->pending_slot );
    t->pending.clear();
    t->pending_slot.clear();
}

static int cr_payloads( void * transport, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    *client_index = t->inbound_slot.data();
    *payload_data = t->inbound_data.data();
    *payload_bytes = t->inbound_bytes.data();
    return (int) t->inbound_slot.size();
}

static int cr_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    *client_index = t->acked_slot.data();
    *packet_ids = t->acked.data();
    return (int) t->acked.size();
}

static int cr_max_payload_bytes( void * transport )
{
    (void) transport;
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

static uint8_t * cr_start_packet( void * transport, int client_index, int request_bytes, rocketnet_packet_context_t * context )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    memset( context, 0, sizeof( *context ) );
    check( request_bytes >= 1 && request_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    if ( t->started >= t->capacity )
    {
        t->refused++;                       // a transport out of frames refuses the start
        return NULL;
    }
    context->frame = cr_frame;
    context->payload_index = CR_PAYLOAD_INDEX;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->packet_id = ++t->next_id;
    context->client_index = client_index;
    t->started++;
    return cr_frame;
}

static int cr_finish_packet( void * transport, rocketnet_packet_context_t * context, int payload_bytes )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    if ( t->fail_finish )
    {
        return 0;
    }
    t->pending.push_back( context->packet_id );
    t->pending_slot.push_back( context->client_index );
    const uint8_t * payload = context->frame + context->payload_index;
    t->sent.push_back( std::vector<uint8_t>( payload, payload + payload_bytes ) );
    return ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_bytes;     // the seal path's wire bytes (rocketnet_send_budget.h)
}

static void cr_abort_packet( void * transport, rocketnet_packet_context_t * context )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    (void) context;
    t->aborts++;
}

static void cr_send_packets( void * transport )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    t->started = 0;
}

static int cr_send_capacity_available( void * transport )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    return t->capacity - t->started;
}

static uint64_t cr_connection( void * transport, int client_index )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    check( client_index >= 0 && client_index < CR_MAX_SLOTS );
    return t->bound[client_index] ? (uint64_t) client_index + 1 : 0;
}

static int cr_connected_count( void * transport )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    int count = 0;
    for ( int i = 0; i < CR_MAX_SLOTS; i++ )
    {
        count += t->bound[i] ? 1 : 0;
    }
    return count;
}

static void cr_disconnect( void * transport, int client_index )
{
    cr_transport_t * t = (cr_transport_t *) transport;
    (void) client_index;
    t->disconnects++;
}

static int cr_state( void * transport )
{
    (void) transport;
    return ROCKETNET_SERVER_TRANSPORT_RUNNING;
}

// ---- the model -------------------------------------------------------------------------
//
// One flush as the rule states it, on the test's own state: backlog[i] frames
// due on slot i, bound[i] the slots that hold a connection, cursor the slot the
// flush starts at. Returns the slots served in order and the cut's round
// (0: no cut), and leaves the cursor.

struct cr_model_t
{
    int slots;
    int bound[CR_MAX_SLOTS];
    int backlog[CR_MAX_SLOTS];
    int cursor;
};

static int cr_model_flush( cr_model_t * m, int capacity, std::vector<int> * served )
{
    const int n = m->slots;
    const int start = m->cursor % n;
    served->clear();

    // the position each connected slot holds before its first-round visit:
    // past the connected slot visited before it, and for the first connected
    // slot of the pass, past the last (a walk that went round once already)

    int order[CR_MAX_SLOTS];
    int connected = 0;
    for ( int o = 0; o < n; o++ )
    {
        const int i = ( start + o ) % n;
        if ( m->bound[i] )
        {
            order[connected++] = i;
        }
    }
    if ( connected == 0 )
    {
        return 0;                                   // nothing connected: the cursor stays
    }
    const int last = order[connected - 1];
    int before[CR_MAX_SLOTS];
    for ( int k = 0; k < connected; k++ )
    {
        before[order[k]] = ( order[k > 0 ? k - 1 : connected - 1] + 1 ) % n;
    }

    int left = capacity;
    std::vector<int> writers;

    // round 1: every connected slot from the cursor; a cut leaves the cursor
    // past the connected slots visited so far (at the cursor if none)

    int cursor = start;
    for ( int o = 0; o < n; o++ )
    {
        const int i = ( start + o ) % n;
        if ( !m->bound[i] )
        {
            continue;
        }
        if ( left <= 0 )
        {
            m->cursor = cursor;
            return 1;
        }
        if ( m->backlog[i] > 0 )
        {
            m->backlog[i]--;
            left--;
            served->push_back( i );
            writers.push_back( i );
        }
        cursor = ( i + 1 ) % n;
    }

    // later rounds: the last round's writers in the same order; a cut leaves
    // the cut writer's first-round position

    for ( int round = 2; !writers.empty(); round++ )
    {
        std::vector<int> kept;
        for ( size_t k = 0; k < writers.size(); k++ )
        {
            const int i = writers[k];
            if ( left <= 0 )
            {
                m->cursor = before[i];
                return round;
            }
            if ( m->backlog[i] > 0 )
            {
                m->backlog[i]--;
                left--;
                served->push_back( i );
                kept.push_back( i );
            }
        }
        writers.swap( kept );
    }

    m->cursor = ( last + 1 ) % n;
    return 0;
}

// ---- the server ------------------------------------------------------------------------

struct cr_world_t
{
    cr_transport_t * t;
    rn_message_object_t * object;
    rocketnet_message_server_t * server;
    double time;
    cr_model_t model;
};

static void cr_world_create( cr_world_t * w, int slots, const int * bound, int packets_per_update = 64 )
{
    check( slots >= 2 && slots <= CR_MAX_SLOTS );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.reliable_messages_in_flight = 128;
    config.packets_in_flight = 128;
    config.receive_message_buffer_bytes = 4096;
    config.max_message_bytes = 4096;
    config.send_message_buffer_bytes = 131072;
    config.send_queue_messages = 128;
    config.packets_per_update = packets_per_update;
    config.unreliable_messages_per_update = 16;
    config.max_message_receipts = 16;
    config.sequenced_messages_held = 0;
    config.server_send_kbps = 0;                    // uncapped: only the transport's capacity cuts
    check( rocketnet_message_validate_config( &config ) );

    w->t = new cr_transport_t();
    w->t->next_id = 7000;
    w->t->capacity = 1000;
    memset( &w->model, 0, sizeof( w->model ) );
    w->model.slots = slots;
    for ( int i = 0; i < slots; i++ )
    {
        w->t->bound[i] = bound[i];
        w->model.bound[i] = bound[i];
    }

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = w->t;
    link.update = cr_update;
    link.payloads = cr_payloads;
    link.acked = cr_acked;
    link.max_payload_bytes = cr_max_payload_bytes;
    link.start_packet = cr_start_packet;
    link.finish_packet = cr_finish_packet;
    link.abort_packet = cr_abort_packet;
    link.send_packets = cr_send_packets;
    link.send_capacity_available = cr_send_capacity_available;
    link.connection = cr_connection;
    link.disconnect = cr_disconnect;
    link.state = cr_state;
    link.send_connected_count = cr_connected_count;

    w->object = rn_message_object_create( NULL, &link, 1, slots, &config, NULL, 0, 0, CR_T0 );
    check( w->object != NULL );
    w->server = (rocketnet_message_server_t *) w->object;
    w->time = CR_T0;

    // two warm flushes: every connection's CONFIG goes out, one frame each,
    // then is acknowledged; uncut, so the cursor ends past the last connected slot

    for ( int f = 0; f < 2; f++ )
    {
        check( rocketnet_message_server_update( w->server, w->time ) == ROCKETNET_UPDATE_OK );
        check( w->object->send_next_connection == w->model.cursor );
        for ( int i = 0; f == 0 && i < slots; i++ )
        {
            w->model.backlog[i] = bound[i] ? 1 : 0;
        }
        std::vector<int> served;
        check( cr_model_flush( &w->model, w->t->capacity, &served ) == 0 );
        rocketnet_message_server_send_packets( w->server );
        check( w->t->pending_slot == served );
        check( w->object->send_next_connection == w->model.cursor );
        w->time += CR_DT;
    }
    for ( int i = 0; i < slots; i++ )
    {
        check( ( rocketnet_message_server_client_connected( w->server, i ) != 0 ) == ( bound[i] != 0 ) );
    }
}

static void cr_world_destroy( cr_world_t * w )
{
    rocketnet_message_server_destroy( w->server );
    delete w->t;
}

// one update and flush: add[i] more frames due on slot i, the flush at the
// cursor the model holds and at `capacity`; the frames and the cursor the
// server leaves must be the model's. Returns the round of the cut.

static int cr_flush( cr_world_t * w, const int * add, int capacity )
{
    check( rocketnet_message_server_update( w->server, w->time ) == ROCKETNET_UPDATE_OK );

    uint8_t message[CR_MESSAGE_BYTES];
    memset( message, 0x5A, sizeof( message ) );
    for ( int i = 0; i < w->model.slots; i++ )
    {
        for ( int k = 0; k < add[i]; k++ )
        {
            check( w->model.bound[i] );
            check( rocketnet_message_server_send( w->server, i, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
            w->model.backlog[i]++;
        }
    }

    check( w->object->send_next_connection == w->model.cursor );

    std::vector<int> served;
    const int round = cr_model_flush( &w->model, capacity, &served );

    w->t->capacity = capacity;
    rocketnet_message_server_send_packets( w->server );

    if ( w->t->pending_slot != served || w->object->send_next_connection != w->model.cursor )
    {
        printf( "    capacity %d, cut in round %d: served", capacity, round );
        for ( size_t k = 0; k < w->t->pending_slot.size(); k++ )
        {
            printf( " %d", w->t->pending_slot[k] );
        }
        printf( " (the rule:" );
        for ( size_t k = 0; k < served.size(); k++ )
        {
            printf( " %d", served[k] );
        }
        printf( "), cursor %d (the rule: %d)\n", w->object->send_next_connection, w->model.cursor );
    }
    check( w->t->pending_slot == served );
    check( w->object->send_next_connection == w->model.cursor );
    check( w->t->aborts == 0 );
    check( w->t->refused == 0 );

    w->time += CR_DT;
    return round;
}

// ---- the cases -------------------------------------------------------------------------

// Hand-built cuts, one per round, at five slots with slots 0 and 3 unbound:
// connected 1, 2 and 4. The first connected slot of a pass from cursor 0 is 1,
// and the slot past the last connected (4) is 0, so the first writer's
// first-round position (0) is neither its own index nor 1.
static void test_server_send_cursor_cut_in_each_round()
{
    const int bound[5] = { 0, 1, 1, 0, 1 };
    const int none[5] = { 0, 0, 0, 0, 0 };

    // round 1: capacity 2, frames due on 1, 2 and 4: 1 and 2 are served, the
    // cut is at 4, before its visit: the cursor is 3 (past 2)

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 1, 1, 0, 1 };
        check( cr_flush( &w, add, 2 ) == 1 );
        check( w.t->pending_slot.size() == 2 && w.t->pending_slot[0] == 1 && w.t->pending_slot[1] == 2 );
        check( w.object->send_next_connection == 3 );

        // the next flush starts at 4, then wraps to 1 and 2 (nothing due):
        // uncut, the cursor ends past the last connected slot visited, 2
        check( cr_flush( &w, none, 2 ) == 0 );
        check( w.t->pending_slot.size() == 1 && w.t->pending_slot[0] == 4 );
        check( w.object->send_next_connection == 3 );
        cr_world_destroy( &w );
    }

    // round 2, cut at the pass's first writer: capacity 3, two frames due on 1,
    // one on 2 and one on 4. Round 1 serves 1, 2, 4; round 2 is cut at 1: the
    // cursor is 1's first-round position, past the last connected slot: 0

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 2, 1, 0, 1 };
        check( cr_flush( &w, add, 3 ) == 2 );
        check( w.object->send_next_connection == 0 );

        // the next flush serves 1's second frame first, and is cut at 2
        check( cr_flush( &w, none, 1 ) == 1 );
        check( w.t->pending_slot.size() == 1 && w.t->pending_slot[0] == 1 );
        check( w.object->send_next_connection == 2 );
        cr_world_destroy( &w );
    }

    // round 2, cut at a later writer: capacity 4, two frames due on 1, 2 and
    // 4. Round 1 serves 1, 2, 4; round 2 serves 1 and is cut at 2: the cursor
    // is 2's first-round position, past 1: 2

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 2, 2, 0, 2 };
        check( cr_flush( &w, add, 4 ) == 2 );
        check( w.object->send_next_connection == 2 );

        // the next flush: 2 first, then 4, cut at 1
        check( cr_flush( &w, none, 2 ) == 1 );
        check( w.t->pending_slot.size() == 2 && w.t->pending_slot[0] == 2 && w.t->pending_slot[1] == 4 );
        cr_world_destroy( &w );
    }

    // round 2, cut at the last writer: capacity 5, two frames due on 1, 2, 4:
    // the cursor is 4's first-round position, past 2: 3

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 2, 2, 0, 2 };
        check( cr_flush( &w, add, 5 ) == 2 );
        check( w.object->send_next_connection == 3 );
        cr_world_destroy( &w );
    }

    // round 3, the writer list closed up: capacity 6, three frames due on 2
    // and 4, one on 1. Round 1 serves 1, 2, 4; round 2 serves 2 and 4 (1 has
    // nothing left and leaves the list); round 3 serves 2 and is cut at 4: the
    // cursor is 4's first-round position, 3, carried when the list closed up

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 1, 3, 0, 3 };
        check( cr_flush( &w, add, 6 ) == 3 );
        check( w.object->send_next_connection == 3 );

        check( cr_flush( &w, none, 1 ) == 1 );
        check( w.t->pending_slot.size() == 1 && w.t->pending_slot[0] == 4 );
        check( w.object->send_next_connection == 0 );
        cr_world_destroy( &w );
    }

    // round 3, cut at the first writer after the list closed up: three frames
    // due on 2 and 4, one on 1, capacity 5: round 3 is cut at 2, whose
    // first-round position is past 1: 2

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 1, 3, 0, 3 };
        check( cr_flush( &w, add, 5 ) == 3 );
        check( w.object->send_next_connection == 2 );
        cr_world_destroy( &w );
    }

    // a capacity spent exactly by the last frame of a round: the cut comes at
    // the next round's first writer, never a frame past the capacity (the
    // transport here refuses a start past it)

    {
        cr_world_t w;
        cr_world_create( &w, 5, bound );
        const int add[5] = { 0, 3, 3, 0, 3 };
        check( cr_flush( &w, add, 6 ) == 3 );
        check( w.object->send_next_connection == 0 );
        cr_world_destroy( &w );
    }
}

REGISTER_UNIT_TEST( test_server_send_cursor_cut_in_each_round );

// The rule against the model over many flushes: two to six slots, every
// pattern of bound slots, frames added at random, capacities from 1 to more
// than the backlog. Every flush's frames and cursor match the model, and cuts
// land in rounds 1, 2 and 3 and later.
static void test_server_send_cursor_follows_the_rule_over_many_flushes()
{
    uint32_t seed = 0x5EED1234u;
    int cuts[5] = { 0, 0, 0, 0, 0 };
    for ( int slots = 2; slots <= 6; slots++ )
    {
        for ( int mask = 1; mask < ( 1 << slots ); mask++ )
        {
            int bound[CR_MAX_SLOTS] = {};
            for ( int i = 0; i < slots; i++ )
            {
                bound[i] = ( mask >> i ) & 1;
            }

            cr_world_t w;
            cr_world_create( &w, slots, bound );
            for ( int f = 0; f < 12; f++ )
            {
                int add[CR_MAX_SLOTS] = {};
                int due = 0;
                for ( int i = 0; i < slots; i++ )
                {
                    seed = seed * 1664525u + 1013904223u;
                    add[i] = bound[i] && w.model.backlog[i] < 6 ? (int) ( ( seed >> 24 ) % 4 ) : 0;
                    due += w.model.backlog[i] + add[i];
                }
                seed = seed * 1664525u + 1013904223u;
                const int capacity = 1 + (int) ( ( seed >> 20 ) % (uint32_t) ( due + 2 ) );
                const int round = cr_flush( &w, add, capacity );
                cuts[round < 4 ? round : 4]++;
            }
            cr_world_destroy( &w );
        }
    }
    printf( "    cuts by round: none %d, 1: %d, 2: %d, 3: %d, later: %d\n", cuts[0], cuts[1], cuts[2], cuts[3], cuts[4] );
    check( cuts[0] > 0 && cuts[1] > 0 && cuts[2] > 0 && cuts[3] > 0 && cuts[4] > 0 );
}

REGISTER_UNIT_TEST( test_server_send_cursor_follows_the_rule_over_many_flushes );

// No client starved: six slots, 1, 2, 4 and 5 connected, every connected
// client with more frames due than any flush can send, uneven backlogs, the
// capacity below the connected count and changing. Over the flushes every
// connected client is served, and no two counts differ by more than one per
// round of service.
static void test_server_send_cursor_starves_no_client()
{
    const int bound[6] = { 0, 1, 1, 0, 1, 1 };
    const int capacities[] = { 1, 2, 3, 1, 3, 2, 1, 1, 2, 3, 3, 1, 2, 2, 1, 3 };
    const int flushes = (int) ( sizeof( capacities ) / sizeof( capacities[0] ) );

    cr_world_t w;
    cr_world_create( &w, 6, bound );
    int served[6] = {};
    int frames = 0;
    for ( int f = 0; f < flushes; f++ )
    {
        int add[6] = {};
        for ( int i = 0; i < 6; i++ )
        {
            add[i] = bound[i] && w.model.backlog[i] < 4 + i ? 4 + i - w.model.backlog[i] : 0;
        }
        check( cr_flush( &w, add, capacities[f] ) == 1 );       // the capacity is below the connected count: every cut is in round 1
        for ( size_t k = 0; k < w.t->pending_slot.size(); k++ )
        {
            served[w.t->pending_slot[k]]++;
            frames++;
        }
    }

    // round robin over the connected clients: each has had frames / 4 turns, or one more

    for ( int i = 0; i < 6; i++ )
    {
        if ( bound[i] )
        {
            check( served[i] == frames / 4 || served[i] == frames / 4 + 1 );
        }
        else
        {
            check( served[i] == 0 );
        }
    }
    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_send_cursor_starves_no_client );

// ---- the round reserve of an in-place unreliable start --------------------------------
//
// An in-place unreliable start (the packed build at sdk/rocketnet_message_server.cpp's
// `frame_available`, the whole frame at its `send_capacity_available <=
// send_connections` test) takes a transport frame only when more frames are
// left than one round of the rotating pass needs (one per connected client):
// application call order cannot take another client's turn. At capacity equal
// to the connected count the start takes no frame and the message goes to the
// region; one more and it takes a frame. Four slots, three connected.
static void test_server_in_place_start_leaves_a_round_for_the_others()
{
    const int bound[4] = { 1, 1, 0, 1 };
    uint8_t message[CR_MESSAGE_BYTES];
    memset( message, 0x5A, sizeof( message ) );

    for ( int extra = 0; extra < 2; extra++ )
    {
        cr_world_t w;
        cr_world_create( &w, 4, bound );
        check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

        const int capacity = 3 + extra;
        w.t->capacity = capacity;
        w.t->started = 0;

        uint8_t * p = rocketnet_message_server_start( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
        check( p != NULL );
        if ( extra == 0 )
        {
            // exactly one round left: no frame, the region holds the message

            check( w.t->started == 0 );
            check( w.object->connections[0].open.in_place != 1 && w.object->connections[0].open.in_place != 2 );
        }
        else
        {
            // one frame more than a round: the packed build opens a frame

            check( w.t->started == 1 );
            check( w.object->connections[0].open.in_place == 2 );
        }
        memset( p, 0x33, 100 );
        rocketnet_message_server_finish( w.server, 0, 100 );

        // the pass: every connected client has a reliable frame due, and each
        // gets it; client 0's unreliable message rides its frame, or its own

        for ( int i = 0; i < 4; i++ )
        {
            if ( bound[i] )
            {
                check( rocketnet_message_server_send( w.server, i, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
            }
        }
        rocketnet_message_server_send_packets( w.server );
        int served[4] = {};
        for ( size_t k = 0; k < w.t->pending_slot.size(); k++ )
        {
            served[w.t->pending_slot[k]]++;
        }
        check( (int) w.t->pending_slot.size() == capacity );
        check( served[0] == 1 + extra && served[1] == 1 && served[2] == 0 && served[3] == 1 );
        check( w.t->refused == 0 && w.t->aborts == 0 );

        cr_world_destroy( &w );
    }
}

REGISTER_UNIT_TEST( test_server_in_place_start_leaves_a_round_for_the_others );

// ---- every call that takes a client index, at -1 and at max_clients --------------------
//
// The first index past the array (max_clients) and -1 are refused by every call,
// with the refusal value and nothing changed: no frame started, no slot
// touched, no disconnect; a transport entry tagged with either is counted
// stale and applied to no slot (rn_message_admit).

int rn_message_server_send_fits( struct rocketnet_message_server_t * server, int client_index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );

static void test_server_calls_refuse_the_index_past_the_slots()
{
    const int bound[4] = { 1, 1, 1, 1 };
    cr_world_t w;
    cr_world_create( &w, 4, bound );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

    uint8_t message[16];
    memset( message, 0x11, sizeof( message ) );
    const int indices[2] = { -1, 4 };
    for ( int n = 0; n < 2; n++ )
    {
        const int index = indices[n];
        gaps_assert_scope_t asserts;

        check( rocketnet_message_server_start( w.server, index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) == NULL );
        check( rocketnet_message_server_start( w.server, index, 0, ROCKETNET_RELIABILITY_RELIABLE, 16 ) == NULL );
        check( rn_message_open_count( w.object ) == 0 && w.object->connections[0].open.in_place == 0 );
        check( rocketnet_message_server_finish( w.server, index, 16 ) == 0 );
        rocketnet_message_server_abort( w.server, index );
        check( rocketnet_message_server_send( w.server, index, 0, ROCKETNET_RELIABILITY_RELIABLE, message, 16 ) == 0 );
        check( w.t->started == 0 );

        rocketnet_message_receipt_t receipts[4];
        check( rocketnet_message_server_receipts( w.server, index, receipts, 4 ) == 0 );

        rocketnet_conditions_t conditions;
        memset( &conditions, 0x77, sizeof( conditions ) );
        check( !rocketnet_message_server_conditions( w.server, index, &conditions ) );

        check( rocketnet_message_server_set_send_kbps( w.server, index, 64 ) == 0 );
        check( rocketnet_message_server_disconnect_reason( w.server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( rocketnet_message_server_client_connected( w.server, index ) == 0 );

        uint64_t free_bytes = 7;
        uint64_t pool_bytes = 7;
        uint32_t free_messages = 7;
        check( rn_message_server_send_fits( w.server, index, 16, &free_bytes, &pool_bytes, &free_messages ) == 0 );
        check( free_bytes == 7 && pool_bytes == 7 && free_messages == 7 );

        rn_message_object_kick( w.object, index, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
        check( w.t->disconnects == 0 );
    }

    // transport entries tagged -1 and max_clients: counted stale, applied nowhere

    const uint64_t stale = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY];
    w.time += CR_DT;
    w.t->inbound_slot.assign( { -1, 4, -1, 4 } );
    w.t->inbound_data.assign( 4, cr_inbound_payload );
    w.t->inbound_bytes.assign( 4, (int) sizeof( cr_inbound_payload ) );
    {
        gaps_assert_scope_t asserts;
        check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );
    }
    w.t->inbound_slot.clear();
    w.t->inbound_data.clear();
    w.t->inbound_bytes.clear();
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY] == stale + 4 );

    for ( int i = 0; i < 4; i++ )
    {
        check( rocketnet_message_server_client_connected( w.server, i ) == 1 );
        check( rocketnet_message_server_disconnect_reason( w.server, i ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }
    check( w.t->disconnects == 0 );

    // the lowest index is a client like any other: its conditions and its receipts are read

    rocketnet_conditions_t conditions;
    check( rocketnet_message_server_conditions( w.server, 0, &conditions ) );

    // a NULL out pointer for a connected client: asserted where asserts are on, refused, nothing written

    {
        gaps_assert_scope_t asserts;
        check( !rocketnet_message_server_conditions( w.server, 0, NULL ) );
        check( g_gaps_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    }

    const uint32_t serial = rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, message, 16 );
    check( serial != 0 );
    rocketnet_message_server_send_packets( w.server );
    check( w.t->pending_slot.size() == 1 && w.t->pending_slot[0] == 0 );
    w.time += CR_DT;
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_receipt_t receipts[4];
    check( rocketnet_message_server_receipts( w.server, 0, receipts, 4 ) == 1 );
    check( receipts[0].serial == serial && receipts[0].status == ROCKETNET_RECEIPT_STATUS_ACKED );

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_calls_refuse_the_index_past_the_slots );

// ---- the ack drain takes a run at a time and counts each stale entry --------------------
//
// rn_message_object_update hands each run of acked ids with one slot and one
// connection id to the window in one call. Runs interleave here (slot 0, 1,
// 0, 1), stale runs sit between them (-1 and max_clients, two and three
// entries long), and an entry tagged for a slot with no binding: every stale
// entry is counted once, and every live id is acknowledged, as one admit per
// id did.

static void test_server_ack_drain_takes_runs_and_counts_each_stale_entry()
{
    const int bound[4] = { 1, 1, 1, 0 };
    cr_world_t w;
    cr_world_create( &w, 4, bound );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

    uint8_t message[CR_MESSAGE_BYTES];
    memset( message, 0x3C, sizeof( message ) );
    for ( int slot = 0; slot < 2; slot++ )
    {
        for ( int k = 0; k < 3; k++ )
        {
            check( rocketnet_message_server_send( w.server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
        }
    }
    rocketnet_message_server_send_packets( w.server );

    std::vector<uint64_t> ids[2];
    for ( size_t k = 0; k < w.t->pending.size(); k++ )
    {
        const int slot = w.t->pending_slot[k];
        check( slot == 0 || slot == 1 );
        ids[slot].push_back( w.t->pending[k] );
    }
    check( ids[0].size() == 3 && ids[1].size() == 3 );

    const uint64_t u0 = w.object->connections[0].ordered.send.U;
    const uint64_t u1 = w.object->connections[1].ordered.send.U;
    const uint64_t stale = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY];

    w.t->pending_slot.assign( { -1, -1, 0, 0, 4, 4, 4, 1, 0, 3, 1, 1 } );
    w.t->pending.assign( { 1, 2, ids[0][0], ids[0][1], 3, 4, 5, ids[1][0], ids[0][2], 6, ids[1][1], ids[1][2] } );

    w.time += CR_DT;
    {
        gaps_assert_scope_t asserts;
        check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );
    }

    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY] == stale + 6 );
    check( w.object->connections[0].ordered.send.U == u0 + 3 );
    check( w.object->connections[1].ordered.send.U == u1 + 3 );
    for ( int i = 0; i < 3; i++ )
    {
        check( ( rocketnet_message_server_client_connected( w.server, i ) != 0 ) == ( bound[i] != 0 ) );
    }

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_ack_drain_takes_runs_and_counts_each_stale_entry );

// ---- the drain: the batch's connection ids read once equal the per-entry tag ----------
//
// Two worlds take the same sends and the same acked batch. One link tags the
// batch entry by entry (acked_connection), the other hands the whole array at
// once (acked_connection_ids); both answer 0 past the tagged count, as the
// server's link does. A wrong tag inside a run of one slot splits it, and the
// untagged tail is stale. Every counter and every window's U must match.

static std::vector<uint64_t> g_drain_tags;
static int g_drain_tag_count = 0;

static uint64_t drain_tag_entry( void *, int k )
{
    return k >= 0 && k < g_drain_tag_count ? g_drain_tags[(size_t) k] : 0;
}

static int drain_tag_ids( void *, const uint64_t ** ids )
{
    *ids = g_drain_tags.data();
    return g_drain_tag_count;
}

static void drain_tagged_world( cr_world_t * w, const int * bound, bool whole_array, uint64_t * before )
{
    cr_world_create( w, 4, bound );
    check( rocketnet_message_server_update( w->server, w->time ) == ROCKETNET_UPDATE_OK );

    uint8_t message[CR_MESSAGE_BYTES];
    memset( message, 0x3C, sizeof( message ) );
    for ( int slot = 0; slot < 2; slot++ )
    {
        for ( int k = 0; k < 4; k++ )
        {
            check( rocketnet_message_server_send( w->server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE, message, (int) sizeof( message ) ) != 0 );
        }
    }
    rocketnet_message_server_send_packets( w->server );

    std::vector<uint64_t> ids[2];
    for ( size_t k = 0; k < w->t->pending.size(); k++ )
    {
        const int slot = w->t->pending_slot[k];
        check( slot == 0 || slot == 1 );
        ids[slot].push_back( w->t->pending[k] );
    }
    check( ids[0].size() == 4 && ids[1].size() == 4 );

    const uint64_t c0 = w->object->connections[0].hot->connection_id;
    const uint64_t c1 = w->object->connections[1].hot->connection_id;
    check( c0 != 0 && c1 != 0 && c0 != 77 && c1 != 77 );

    w->t->pending_slot.assign( { -1, 0, 0, 1, 0, 1, 1, 4, 0, 1 } );
    w->t->pending.assign( { 1, ids[0][0], ids[0][1], ids[1][0], ids[0][2], ids[1][1], ids[1][2], 9, ids[0][3], ids[1][3] } );
    g_drain_tags.assign( { 9, c0, c0, 77, c0, c1, c1, 5, c0, c1 } );
    g_drain_tag_count = 9;   // the last entry is past the tagged count: tag 0, stale

    before[0] = w->object->connections[0].ordered.send.U;
    before[1] = w->object->connections[1].ordered.send.U;
    before[2] = w->object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY];

    w->object->link.acked_connection = drain_tag_entry;
    w->object->link.acked_connection_ids = whole_array ? drain_tag_ids : NULL;

    w->time += CR_DT;
    gaps_assert_scope_t asserts;
    check( rocketnet_message_server_update( w->server, w->time ) == ROCKETNET_UPDATE_OK );
}

static void test_server_ack_drain_whole_array_matches_per_entry_tags()
{
    const int bound[4] = { 1, 1, 1, 0 };
    cr_world_t a;
    cr_world_t b;
    uint64_t before_a[3];
    uint64_t before_b[3];
    drain_tagged_world( &a, bound, false, before_a );
    drain_tagged_world( &b, bound, true, before_b );
    check( before_a[0] == before_b[0] && before_a[1] == before_b[1] && before_a[2] == before_b[2] );

    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; c++ )
    {
        check( a.object->counters.values[c] == b.object->counters.values[c] );
    }
    for ( int i = 0; i < 3; i++ )
    {
        check( a.object->connections[i].ordered.send.U == b.object->connections[i].ordered.send.U );
    }

    // slot 0's four ids are all tagged c0: U moves by 4; slot 1's first id
    // carries the wrong tag and its last is untagged, so two are applied.
    // Stale: -1, slot 1's wrong tag, slot 4, slot 1's untagged tail.

    check( b.object->connections[0].ordered.send.U == before_b[0] + 4 );
    check( b.object->connections[1].ordered.send.U == before_b[1] );
    check( b.object->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY] == before_b[2] + 4 );

    cr_world_destroy( &a );
    cr_world_destroy( &b );
}

REGISTER_UNIT_TEST( test_server_ack_drain_whole_array_matches_per_entry_tags );

// ---- create: the status written, and the default send budget ---------------------------
//
// rocketnet_message_server_create_internal over the loopback transport (no
// socket): each refusal writes its own status over the caller's value, and a
// success writes OK. A per-client budget of 0 takes the message config's
// server_send_kbps, a capped budget, not unlimited; a config field of 0 is
// unlimited. ROCKETNET_SEND_BUDGET_MAX_KBPS is in range and one more is not.

static void ms_server_transport_config( rocketnet_server_transport_config_t * server_config )
{
    uint8_t backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret_key[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret_key[RN_TEST_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair( backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( server_public_key, server_secret_key ) == ROCKETNET_STATUS_OK );

    rocketnet_server_transport_default_config_current( server_config );
    server_config->protocol_id = 0x6d7367617073ULL;
    server_config->max_clients = 2;
    memcpy( server_config->backend_public_key, backend_public_key, sizeof( server_config->backend_public_key ) );
    memcpy( server_config->server_public_key, server_public_key, sizeof( server_config->server_public_key ) );
    memcpy( server_config->server_secret_key, server_secret_key, sizeof( server_config->server_secret_key ) );
    snprintf( server_config->transport, sizeof( server_config->transport ), "%s", "loopback" );
    server_config->loopback_hub = rocketnet_test_loopback_hub();
    memset( server_config->packet_filter, 0x5A, sizeof( server_config->packet_filter ) );
}

static void test_message_server_create_writes_each_status()
{
    rocketnet_quiet( 1 );
    rocketnet_server_transport_config_t server_config;
    ms_server_transport_config( &server_config );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.server_send_kbps = 256;

    int status = ROCKETNET_TEST_STATUS_UNSET;
    gaps_assert_scope_t asserts;

    // no address

    check( rocketnet_message_server_create_internal( NULL, NULL, &server_config, &config, NULL, 0, 0, &status, 0 ) == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );

    // a budget one past the maximum (the maximum itself sizes no block either:
    // its 100 ms of depth is past what a layout holds, so required_memory is 0
    // at both and the create refuses both with the same status)

    status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, &status, 0 ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( rocketnet_message_server_required_memory( &server_config, &config, (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS + 1 ) == 0 );
    status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS, &status, 0 ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // a caller block too short for the layout

    alignas( 64 ) static uint8_t small_block[4096];
    check( rocketnet_message_server_required_memory( &server_config, &config, 0 ) > sizeof( small_block ) );
    status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, small_block, sizeof( small_block ), 0, &status, 0 ) == NULL );
    check( status == ROCKETNET_STATUS_OUT_OF_MEMORY );

    // a message config with no required memory

    rocketnet_message_config_t bad = config;
    bad.num_channels = 0;
    status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &bad, NULL, 0, 0, &status, 0 ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // success: OK, and a budget of 0 takes the config's 256 kbps (no assert from here on)

    g_gaps_asserts = 0;
    status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_message_server_t * server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, 0, &status, 0 );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    if ( server )
    {
        check( server->object.default_kbps == 256 );
        rocketnet_message_server_destroy( server );
    }

    // a positive budget overrides the field; a field of 0 with a budget of 0 is unlimited

    server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, 512, &status, 0 );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    if ( server )
    {
        check( server->object.default_kbps == 512 );
        rocketnet_message_server_destroy( server );
    }

    config.server_send_kbps = 0;
    server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, 0, &status, 0 );
    check( server != NULL && status == ROCKETNET_STATUS_OK );
    if ( server )
    {
        check( server->object.default_kbps <= 0 );
        rocketnet_message_server_destroy( server );
    }
    check( g_gaps_asserts == 0 );
}

REGISTER_UNIT_TEST( test_message_server_create_writes_each_status );

// A create that fails on an allocation reports it: with the allocator
// refusing from the k-th call on, for every k until the create succeeds, a
// NULL server always comes with a status other than OK (the transport's
// refusal, or OUT_OF_MEMORY when the transport opened and the object's block
// was refused), never OK.

static int ms_allocations = 0;
static int ms_allocations_allowed = 0;

static void * ms_malloc( void * context, size_t bytes )
{
    (void) context;
    if ( ms_allocations >= ms_allocations_allowed )
    {
        return NULL;
    }
    ms_allocations++;
    return malloc( bytes );
}

static void ms_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static void test_message_server_create_reports_a_refused_allocation()
{
    rocketnet_quiet( 1 );
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );

    bool created = false;
    int refused_after_transport = 0;
    for ( int allowed = 0; allowed < 256 && !created; allowed++ )
    {
        check( rocketnet_test_loopback_hub_release() );
        rocketnet_term();
        ms_allocations = 0;
        ms_allocations_allowed = 1 << 30;                           // the hub and the keys allocate freely
        check( rocketnet_init( NULL, ms_malloc, ms_free ) == ROCKETNET_STATUS_OK );
        rocketnet_server_transport_config_t server_config;
        ms_server_transport_config( &server_config );
        ms_allocations = 0;
        ms_allocations_allowed = allowed;

        int status = ROCKETNET_TEST_STATUS_UNSET;
        rocketnet_message_server_t * server = rocketnet_message_server_create_internal( NULL, "127.0.0.1:0", &server_config, &config, NULL, 0, 0, &status, 0 );
        if ( server )
        {
            check( status == ROCKETNET_STATUS_OK );
            rocketnet_message_server_destroy( server );
            created = true;
        }
        else
        {
            check( status != ROCKETNET_STATUS_OK && status != (int) ROCKETNET_TEST_STATUS_UNSET );
            refused_after_transport += status == ROCKETNET_STATUS_OUT_OF_MEMORY ? 1 : 0;
        }
    }
    check( created );
    check( refused_after_transport > 0 );

    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_message_server_create_reports_a_refused_allocation );

// ---- abort on the region path returns the room ------------------------------------------
//
// An unreliable start the in-place path cannot take (the transport has no
// frame to spare beyond the clients' round) takes a send descriptor and a
// store chunk in the region (rn_endpoint_start); the public abort gives both
// back (rn_endpoint_abort). Started and aborted many more times than the region
// has descriptors, for each unreliable type: every start is given room, the
// region's free descriptors and chunks are back where they were after each
// abort, nothing is refused, and the connection goes on.

static uint32_t cr_store_free_chunks( const rn_region_t * region )
{
    uint32_t free_chunks = 0;
    for ( int c = 0; c < RN_STORE_NUM_CLASSES; c++ )
    {
        free_chunks += region->store.classes[c].free_count + ( region->store.classes[c].num_chunks - region->store.classes[c].fresh );
    }
    return free_chunks;
}

static void test_server_abort_on_the_region_path_returns_the_room()
{
    const int bound[2] = { 1, 0 };
    cr_world_t w;
    cr_world_create( &w, 2, bound );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

    // the transport has no frame to spare beyond the round it keeps for the
    // connected clients, so no start is written in place

    w.t->capacity = w.t->started;

    const rn_region_t * region = w.object->connections[0].region;
    const uint32_t descriptors_free = region->descriptors_free;
    const uint32_t chunks_free = cr_store_free_chunks( region );
    const uint64_t queue_full = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL];
    const uint64_t store_full = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL];
    const uint64_t exhausted = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED];
    check( descriptors_free > 0 );

    const int types[3] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT };
    const int rounds = 3 * (int) descriptors_free + 3;
    int given = 0;
    for ( int n = 0; n < rounds; n++ )
    {
        uint8_t * p = rocketnet_message_server_start( w.server, 0, 0, types[n % 3], 100 );
        check( p != NULL );
        check( w.object->connections[0].open.in_place == 0 );                               // the region path, not a frame
        check( region->descriptors_free == descriptors_free - 1 );
        if ( p )
        {
            given++;
            memset( p, 0x22, 100 );
        }
        rocketnet_message_server_abort( w.server, 0 );
        check( region->descriptors_free == descriptors_free );
        check( cr_store_free_chunks( region ) == chunks_free );
        check( rn_message_open_count( w.object ) == 0 );
    }
    check( given == rounds );
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == queue_full );
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL] == store_full );
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED] == exhausted );

    // the reliable starts (rn_channels_cancel) give their room back the same
    // way, and so does an unreliable start over one packet, promoted to the
    // reliable path (its pending type stays unreliable)

    const int reliable[4] = { ROCKETNET_RELIABILITY_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_UNRELIABLE };
    for ( int n = 0; n < rounds; n++ )
    {
        uint8_t * p = rocketnet_message_server_start( w.server, 0, 0, reliable[n % 4], n % 4 == 3 ? 2000 : 100 );
        check( p != NULL );
        rocketnet_message_server_abort( w.server, 0 );
        check( region->descriptors_free == descriptors_free );
        check( cr_store_free_chunks( region ) == chunks_free );
    }
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL] == queue_full );

    // an in-place start holds no descriptor, and its abort frees none (a
    // debug build asserts on a free of no descriptor)

    w.t->capacity = w.t->started + 4;
    {
        gaps_assert_scope_t asserts;
        for ( int n = 0; n < 3; n++ )
        {
            check( rocketnet_message_server_start( w.server, 0, 0, types[n], 100 ) != NULL );
            check( w.object->connections[0].open.in_place != 0 );
            check( region->descriptors_free == descriptors_free );
            rocketnet_message_server_abort( w.server, 0 );
            check( region->descriptors_free == descriptors_free );
        }
        check( g_gaps_asserts == 0 );
    }
    w.t->capacity = w.t->started;

    // the connection goes on: a message sent and finished on the same path
    // is queued, and nothing ends the connection

    uint8_t message[16];
    memset( message, 0x21, sizeof( message ) );
    check( rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, (int) sizeof( message ) ) != 0 );
    check( region->descriptors_free == descriptors_free - 1 );
    w.t->capacity = w.t->started + 4;
    rocketnet_message_server_send_packets( w.server );
    w.time += CR_DT;
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_server_client_connected( w.server, 0 ) == 1 );
    check( rocketnet_message_server_disconnect_reason( w.server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( w.t->disconnects == 0 );

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_abort_on_the_region_path_returns_the_room );

// A frame whose send fails ends every receipt it carried with one LOSS, and
// no other: sixteen receipted messages written in place fill one frame's
// receipt list (RN_MESSAGE_RECEIPT_SERIALS_MAX), the transport refuses the
// frame, and the receipts are exactly those sixteen serials, each LOSS once.
static void test_server_failed_frame_loses_each_receipt_once()
{
    const int bound[2] = { 1, 0 };
    cr_world_t w;
    cr_world_create( &w, 2, bound );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

    const uint64_t lost = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS];
    w.t->fail_finish = 1;

    uint8_t message[8];
    memset( message, 0x33, sizeof( message ) );
    uint32_t serials[RN_MESSAGE_RECEIPT_SERIALS_MAX];
    for ( int n = 0; n < RN_MESSAGE_RECEIPT_SERIALS_MAX; n++ )
    {
        serials[n] = rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, message, (int) sizeof( message ) );
        check( serials[n] != 0 );
    }
    check( w.object->connections[0].unreliable_build.receipt_count == RN_MESSAGE_RECEIPT_SERIALS_MAX );

    rocketnet_message_server_send_packets( w.server );
    w.t->fail_finish = 0;
    w.time += CR_DT;
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );     // this update's outcomes are the last flush's

    rocketnet_message_receipt_t out[2 * RN_MESSAGE_RECEIPT_SERIALS_MAX];
    const int count = rocketnet_message_server_receipts( w.server, 0, out, 2 * RN_MESSAGE_RECEIPT_SERIALS_MAX );
    check( count == RN_MESSAGE_RECEIPT_SERIALS_MAX );
    for ( int n = 0; n < count && n < RN_MESSAGE_RECEIPT_SERIALS_MAX; n++ )
    {
        check( out[n].serial == serials[n] );
        check( out[n].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    }
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS] == lost + RN_MESSAGE_RECEIPT_SERIALS_MAX );

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_failed_frame_loses_each_receipt_once );

// where a run of `run` bytes of `marker` first appears on the wire this
// world sent: frame index * 4096 + offset, or -1

static int cr_wire_position( const cr_transport_t * t, uint8_t marker, int run )
{
    for ( size_t f = 0; f < t->sent.size(); f++ )
    {
        const std::vector<uint8_t> & frame = t->sent[f];
        int length = 0;
        for ( size_t i = 0; i < frame.size(); i++ )
        {
            length = frame[i] == marker ? length + 1 : 0;
            if ( length == run )
            {
                return (int) f * 4096 + (int) i + 1 - run;
            }
        }
    }
    return -1;
}

// RNOutgoingBuild StartInPlace: an in-place unreliable send never goes out
// ahead of an unreliable message already queued. One message waits in the
// connection's queue (the transport had no frame to spare when it was
// sent); with frames to spare again, the next unreliable start takes the
// queued path too, and on the wire the first message comes first.
static void test_server_in_place_send_never_overtakes_a_queued_one()
{
    const int bound[2] = { 1, 0 };
    cr_world_t w;
    cr_world_create( &w, 2, bound );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );

    w.t->capacity = w.t->started;
    uint8_t first[100];
    memset( first, 0xA1, sizeof( first ) );
    check( rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, first, (int) sizeof( first ) ) != 0 );
    check( w.object->connections[0].unreliable_count == 1 );
    const uint64_t fallback = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK];
    w.t->sent.clear();

    w.t->capacity = w.t->started + 16;
    uint8_t * p = rocketnet_message_server_start( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100 );
    check( p != NULL );
    check( w.object->connections[0].open.in_place == 0 );                                   // queued behind the first
    if ( p )
    {
        memset( p, 0xB2, 100 );
        check( rocketnet_message_server_finish( w.server, 0, 100 ) != 0 );
    }
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK] == fallback + 1 );

    rocketnet_message_server_send_packets( w.server );
    const int a = cr_wire_position( w.t, 0xA1, 100 );
    const int b = cr_wire_position( w.t, 0xB2, 100 );
    check( a >= 0 );
    check( b >= 0 );
    check( a < b );

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_in_place_send_never_overtakes_a_queued_one );

// RNOutgoingBuild ReliableSentWithinOneUpdate: while reliable work is due one
// packet of the update's allowance stays for it. At two packets an update,
// with a reliable message waiting, one whole unreliable frame goes in place;
// the second unreliable start finds the last packet kept and takes the queued
// path, and the reliable message goes out in this update's flush.
static void test_server_in_place_keeps_a_packet_for_due_reliable_work()
{
    const int bound[2] = { 1, 0 };
    cr_world_t w;
    cr_world_create( &w, 2, bound, 2 );
    check( rocketnet_message_server_update( w.server, w.time ) == ROCKETNET_UPDATE_OK );
    const uint64_t fallback = w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK];
    w.t->sent.clear();

    uint8_t reliable[100];
    memset( reliable, 0xC3, sizeof( reliable ) );
    check( rocketnet_message_server_send( w.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, reliable, (int) sizeof( reliable ) ) != 0 );

    const int bytes = 1400;
    uint8_t * p = rocketnet_message_server_start( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
    check( p != NULL );
    check( w.object->connections[0].open.in_place != 0 );                                   // the first packet of two
    if ( p )
    {
        memset( p, 0xD4, (size_t) bytes );
        check( rocketnet_message_server_finish( w.server, 0, bytes ) != 0 );
    }
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK] == fallback );

    p = rocketnet_message_server_start( w.server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
    check( p != NULL );
    check( w.object->connections[0].open.in_place == 0 );                                   // the last packet is the reliable message's
    if ( p )
    {
        memset( p, 0xE5, (size_t) bytes );
        check( rocketnet_message_server_finish( w.server, 0, bytes ) != 0 );
    }
    check( w.object->counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK] == fallback + 1 );

    rocketnet_message_server_send_packets( w.server );
    check( w.t->sent.size() == 2 );
    check( cr_wire_position( w.t, 0xD4, 64 ) >= 0 );
    check( cr_wire_position( w.t, 0xC3, 64 ) >= 0 );                    // the reliable message went out in this update
    check( cr_wire_position( w.t, 0xE5, 64 ) == -1 );                   // the queued one waits: the allowance is spent

    cr_world_destroy( &w );
}

REGISTER_UNIT_TEST( test_server_in_place_keeps_a_packet_for_due_reliable_work );
