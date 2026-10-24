/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message layer stressed at a wide channel count: this one source builds
// bin/test_channels_1024 and bin/test_channels_10k (CMakeLists.txt), each with
// the SDK compiled again at ROCKETNET_MAX_CHANNELS = 1024 or 10000, the way
// test_wide is built at 512. Neither is part of ./bin/test or the per-push tier;
// .github/workflows/nightly-channels-stress.yml runs both on Linux, macOS and
// Windows.
//
// test_channels_stress_every_channel_delivers: a public client and server over
// the in-memory loopback transport, the client step-driven, both with the
// seeded link simulator on (latency, jitter that reorders, loss and
// duplication). Every channel up to ROCKETNET_MAX_CHANNELS is used, channel c
// carrying reliability c % 8, so every delivery mode covers an eighth of the
// channels. Each round both ends send one pattern-verified message on every
// channel. The receiver checks each message's bytes, its delivered channel
// (its own for ordered and sequenced modes, 0 for the others), its delivered
// reliability, at most once on every mode, in order on the ordered modes,
// newer-only on the sequenced modes; at the end every message of the reliable
// modes arrived exactly once, the newest of every reliable sequenced channel
// arrived, and every reliable receipt reads ACKED. Time is an argument from
// 1000.0; there is no socket and no real time. Message bytes are written into
// the library's own reservation (start, finish), and every table the test
// keeps is allocated before the first frame: nothing in the frame loop
// allocates, which the counting allocator checks.
//
// test_channels_stress_memory_is_planned: the public memory plan
// (rocketnet_client_required_memory, rocketnet_server_required_memory) is the
// block create takes, measured through the allocator; the server's bytes per
// connection is the plan's step from one client slot to two; and the plan is
// linear in the channel count: every 64 more channels add the same bytes, on
// the client and per server connection (each per-channel table is rounded up
// to the 64-byte allocation alignment, so the line is exact at that stride).

#include "test_unit_registry.h"
#include "unit_shutdown.h"
#include "token_string.h"

#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_memory.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_MAX_CHANNELS < 1024
#error "test_channels_stress is built with ROCKETNET_MAX_CHANNELS of 1024 or more"
#endif

#define STRESS_PROTOCOL                 0x5354524553534348ULL
#define STRESS_NUM_CHANNELS             ROCKETNET_MAX_CHANNELS
#define STRESS_NUM_MODES                8

// the bounded nightly size: rounds of one message per channel per direction
#if ROCKETNET_MAX_CHANNELS >= 10000
#define STRESS_ROUNDS                   8
#else
#define STRESS_ROUNDS                   32
#endif

#define STRESS_MESSAGES_PER_SIDE        ( STRESS_NUM_CHANNELS * STRESS_ROUNDS )
#define STRESS_HEADER_BYTES             8
#define STRESS_MAX_PATTERN_BYTES        24
#define STRESS_MAX_MESSAGE_BYTES        ( STRESS_HEADER_BYTES + STRESS_MAX_PATTERN_BYTES )
#define STRESS_FRAME_SECONDS            0.01
#define STRESS_DRAIN_FRAMES             3000
#define STRESS_RECEIPTS_PER_UPDATE      65536      // max_message_receipts

// the link simulator on both ends: one-way latency, jitter (which reorders),
// loss and duplication, from the transports' fixed simulator seeds
#define STRESS_LATENCY_MS               20
#define STRESS_JITTER_MS                10
#define STRESS_LOSS_PERCENT             5
#define STRESS_DUPLICATE_PERCENT        5

// ---- the counting allocator ------------------------------------------------

struct stress_allocation_count_t
{
    std::atomic<uint64_t> allocations;
    std::atomic<uint64_t> bytes;
};

static std::atomic<uint64_t> stress_allocations_total( 0 );

static void * stress_malloc( void * context, size_t bytes )
{
    stress_allocations_total++;
    stress_allocation_count_t * count = (stress_allocation_count_t *) context;
    if ( count )
    {
        count->allocations++;
        count->bytes += (uint64_t) bytes;
    }
    return malloc( bytes );
}

static void stress_free( void * context, void * p )
{
    (void) context;
    free( p );
}

static void stress_count_reset( stress_allocation_count_t * count )
{
    count->allocations = 0;
    count->bytes = 0;
}

// ---- the configs -------------------------------------------------------------

// the message config both ends share: every channel, a reliable window and
// send queue that hold the reliable rounds in flight under loss, and no send
// budget, so a refusal or a drop is a defect and never the budget
static void stress_message_config( int * num_channels, int * reliable_messages_in_flight, int * send_message_buffer_bytes,
                                   int * send_queue_messages, int * packets_in_flight, int * packets_per_update,
                                   int * unreliable_messages_per_update, int * max_message_receipts, int channels )
{
    *num_channels = channels;
    *reliable_messages_in_flight = 65536;
    *send_message_buffer_bytes = 32 * 1024 * 1024;
    *send_queue_messages = 65536;
    *packets_in_flight = 16384;
    *packets_per_update = 1024;
    *unreliable_messages_per_update = 65536;
    *max_message_receipts = STRESS_RECEIPTS_PER_UPDATE;
}

static void stress_client_config( rocketnet_client_test_config_t * config, int channels, double (*clock)( void * ) )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = clock;
    config->send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    stress_message_config( &config->num_channels, &config->reliable_messages_in_flight, &config->send_message_buffer_bytes,
                           &config->send_queue_messages, &config->packets_in_flight, &config->packets_per_update,
                           &config->unreliable_messages_per_update, &config->max_message_receipts, channels );
}

static void stress_server_config( rocketnet_server_config_t * config, const uint8_t * backend_public_key, int max_clients, int channels )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = STRESS_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, backend_public_key, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
    config->per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    stress_message_config( &config->num_channels, &config->reliable_messages_in_flight, &config->send_message_buffer_bytes,
                           &config->send_queue_messages, &config->packets_in_flight, &config->packets_per_update,
                           &config->unreliable_messages_per_update, &config->max_message_receipts, channels );
}

// ---- the message pattern -----------------------------------------------------

// channel c carries reliability c % 8
static enum rocketnet_reliability_t stress_mode( int channel )
{
    return (enum rocketnet_reliability_t) ( channel % STRESS_NUM_MODES );
}

static bool stress_mode_reliable( enum rocketnet_reliability_t mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE || mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static bool stress_mode_ordered( enum rocketnet_reliability_t mode )
{
    return mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED || mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static bool stress_mode_sequenced( enum rocketnet_reliability_t mode )
{
    return mode == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED;
}

static bool stress_mode_has_receipt( enum rocketnet_reliability_t mode )
{
    return mode == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT || mode == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           mode == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

// receipt modes arrive without the receipt flag (rocketnet_message_batch_t)
static enum rocketnet_reliability_t stress_mode_delivered( enum rocketnet_reliability_t mode )
{
    switch ( mode )
    {
        case ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT:       return ROCKETNET_RELIABILITY_UNRELIABLE;
        case ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT:         return ROCKETNET_RELIABILITY_RELIABLE;
        case ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT: return ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
        default:                                                      return mode;
    }
}

// ordered and sequenced modes are delivered on their own channel, the others on 0
static int stress_channel_delivered( int channel )
{
    const enum rocketnet_reliability_t mode = stress_mode( channel );
    return stress_mode_ordered( mode ) || stress_mode_sequenced( mode ) ? channel : 0;
}

static int stress_pattern_bytes( int channel, int round )
{
    return ( channel * 7 + round * 3 ) % ( STRESS_MAX_PATTERN_BYTES + 1 );
}

static uint8_t stress_pattern_byte( int side, int channel, int round, int index )
{
    uint32_t x = (uint32_t) side * 0x9E3779B9u ^ (uint32_t) channel * 0x85EBCA6Bu ^ (uint32_t) round * 0xC2B2AE35u ^ (uint32_t) index * 0x27D4EB2Fu;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 13;
    return (uint8_t) ( x & 0xFF );
}

// header: channel (u16), mode (u8), side (u8), round (u32), little endian
static int stress_write_message( uint8_t * out, int side, int channel, int round )
{
    out[0] = (uint8_t) ( channel & 0xFF );
    out[1] = (uint8_t) ( ( channel >> 8 ) & 0xFF );
    out[2] = (uint8_t) stress_mode( channel );
    out[3] = (uint8_t) side;
    out[4] = (uint8_t) ( round & 0xFF );
    out[5] = (uint8_t) ( ( round >> 8 ) & 0xFF );
    out[6] = (uint8_t) ( ( round >> 16 ) & 0xFF );
    out[7] = (uint8_t) ( ( round >> 24 ) & 0xFF );
    const int pattern_bytes = stress_pattern_bytes( channel, round );
    for ( int i = 0; i < pattern_bytes; i++ )
        out[STRESS_HEADER_BYTES + i] = stress_pattern_byte( side, channel, round, i );
    return STRESS_HEADER_BYTES + pattern_bytes;
}

// ---- one end's books -----------------------------------------------------------

struct stress_receipt_send_t
{
    uint32_t serial;
    int mode;
};

struct stress_side_t
{
    int side;                                   // 0 the client, 1 the server

    // what this end received from the other, per channel and round
    uint8_t * received;                         // [STRESS_NUM_CHANNELS * STRESS_ROUNDS], times seen
    int * last_round;                           // [STRESS_NUM_CHANNELS], -1 before the first
    uint64_t messages_received;

    // the receipt sends this end made, in send order (serials increase)
    stress_receipt_send_t * receipt_sends;      // [STRESS_MESSAGES_PER_SIDE]
    int num_receipt_sends;
    int reliable_receipts_acked;
    int unreliable_receipts_resolved;
    struct rocketnet_receipt_t * receipt_buffer;    // [STRESS_RECEIPTS_PER_UPDATE]
};

static void stress_side_create( stress_side_t * side, int which )
{
    memset( side, 0, sizeof( *side ) );
    side->side = which;
    side->received = (uint8_t *) calloc( (size_t) STRESS_MESSAGES_PER_SIDE, 1 );
    side->last_round = (int *) malloc( sizeof( int ) * (size_t) STRESS_NUM_CHANNELS );
    side->receipt_sends = (stress_receipt_send_t *) malloc( sizeof( stress_receipt_send_t ) * (size_t) STRESS_MESSAGES_PER_SIDE );
    side->receipt_buffer = (struct rocketnet_receipt_t *) malloc( sizeof( struct rocketnet_receipt_t ) * STRESS_RECEIPTS_PER_UPDATE );
    check( side->received && side->last_round && side->receipt_sends && side->receipt_buffer );
    for ( int c = 0; c < STRESS_NUM_CHANNELS; c++ )
        side->last_round[c] = -1;
}

static void stress_side_destroy( stress_side_t * side )
{
    free( side->received );
    free( side->last_round );
    free( side->receipt_sends );
    free( side->receipt_buffer );
}

// one delivered message: its bytes, channel, reliability and place in its channel
static void stress_side_receive_one( stress_side_t * side, int channel_delivered, enum rocketnet_reliability_t reliability,
                                     const uint8_t * data, int bytes )
{
    check( bytes >= STRESS_HEADER_BYTES && bytes <= STRESS_MAX_MESSAGE_BYTES );
    const int channel = (int) data[0] | ( (int) data[1] << 8 );
    const int mode = (int) data[2];
    const int sender = (int) data[3];
    const int round = (int) ( (uint32_t) data[4] | ( (uint32_t) data[5] << 8 ) | ( (uint32_t) data[6] << 16 ) | ( (uint32_t) data[7] << 24 ) );
    check( channel >= 0 && channel < STRESS_NUM_CHANNELS );
    check( round >= 0 && round < STRESS_ROUNDS );
    check( sender == 1 - side->side );
    check( mode == (int) stress_mode( channel ) );
    check( channel_delivered == stress_channel_delivered( channel ) );
    check( reliability == stress_mode_delivered( stress_mode( channel ) ) );
    check( bytes == STRESS_HEADER_BYTES + stress_pattern_bytes( channel, round ) );
    for ( int i = 0; i < bytes - STRESS_HEADER_BYTES; i++ )
        check( data[STRESS_HEADER_BYTES + i] == stress_pattern_byte( sender, channel, round, i ) );

    // at most once on every mode
    uint8_t * seen = &side->received[channel * STRESS_ROUNDS + round];
    check( *seen == 0 );
    *seen = 1;

    const enum rocketnet_reliability_t sent_mode = stress_mode( channel );
    if ( stress_mode_ordered( sent_mode ) )
        check( round == side->last_round[channel] + 1 );    // in order, no gap
    else if ( stress_mode_sequenced( sent_mode ) )
        check( round > side->last_round[channel] );         // newer only
    if ( round > side->last_round[channel] )
        side->last_round[channel] = round;
    side->messages_received++;
}

static void stress_side_receive_batch( stress_side_t * side, const struct rocketnet_message_batch_t * batch, bool server )
{
    for ( int i = 0; i < batch->num_messages; i++ )
    {
        check( batch->client_index[i] == 0 );
        (void) server;
        stress_side_receive_one( side, batch->channel[i], batch->reliability[i], batch->data[i], batch->bytes[i] );
    }
}

static void stress_side_record_send( stress_side_t * side, int channel, uint32_t serial )
{
    check( serial != 0 );
    const enum rocketnet_reliability_t mode = stress_mode( channel );
    if ( !stress_mode_has_receipt( mode ) )
        return;
    check( side->num_receipt_sends < STRESS_MESSAGES_PER_SIDE );
    if ( side->num_receipt_sends > 0 )
        check( serial > side->receipt_sends[side->num_receipt_sends - 1].serial );
    side->receipt_sends[side->num_receipt_sends].serial = serial;
    side->receipt_sends[side->num_receipt_sends].mode = (int) mode;
    side->num_receipt_sends++;
}

static void stress_side_receipts( stress_side_t * side, const struct rocketnet_receipt_t * receipts, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        // find the send by serial: the sends are in serial order
        int lo = 0;
        int hi = side->num_receipt_sends - 1;
        int found = -1;
        while ( lo <= hi )
        {
            const int mid = lo + ( hi - lo ) / 2;
            if ( side->receipt_sends[mid].serial == receipts[i].serial )
            {
                found = mid;
                break;
            }
            if ( side->receipt_sends[mid].serial < receipts[i].serial )
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        check( found >= 0 );
        check( side->receipt_sends[found].mode >= 0 );
        const enum rocketnet_reliability_t mode = (enum rocketnet_reliability_t) side->receipt_sends[found].mode;
        side->receipt_sends[found].mode = -1;           // one outcome per send
        if ( mode == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT )
        {
            check( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED || receipts[i].status == ROCKETNET_RECEIPT_STATUS_LOSS );
            side->unreliable_receipts_resolved++;
        }
        else
        {
            check( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            side->reliable_receipts_acked++;
        }
    }
}

// ---- the world -----------------------------------------------------------------

static double stress_now = 1000.0;
static double stress_clock( void * ) { return stress_now; }

struct stress_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    stress_allocation_count_t server_count;
    stress_allocation_count_t client_count;
    stress_side_t client_side;
    stress_side_t server_side;
    double time;
    int frames;
};

static void stress_world_frame( stress_world_t * world )
{
    stress_now = world->time;

    rocketnet_client_update( world->client, world->time );
    for ( const struct rocketnet_message_batch_t * batch = rocketnet_client_receive_messages( world->client ); batch != NULL;
          batch = rocketnet_client_receive_messages( world->client ) )
        stress_side_receive_batch( &world->client_side, batch, false );
    // receipts stay until the next update and a read does not consume them:
    // one read per update, into a buffer that holds the whole queue
    const int client_receipts = rocketnet_client_receipts( world->client, world->client_side.receipt_buffer, STRESS_RECEIPTS_PER_UPDATE );
    check( client_receipts >= 0 && client_receipts <= STRESS_RECEIPTS_PER_UPDATE );
    stress_side_receipts( &world->client_side, world->client_side.receipt_buffer, client_receipts );

    rocketnet_server_update( world->server, world->time );
    for ( const struct rocketnet_message_batch_t * batch = rocketnet_server_receive_messages( world->server ); batch != NULL;
          batch = rocketnet_server_receive_messages( world->server ) )
        stress_side_receive_batch( &world->server_side, batch, true );
    const int server_receipts = rocketnet_server_receipts( world->server, 0, world->server_side.receipt_buffer, STRESS_RECEIPTS_PER_UPDATE );
    check( server_receipts >= 0 && server_receipts <= STRESS_RECEIPTS_PER_UPDATE );
    stress_side_receipts( &world->server_side, world->server_side.receipt_buffer, server_receipts );
}

static void stress_world_send_packets( stress_world_t * world )
{
    rocketnet_client_send_packets( world->client );
    rocketnet_server_send_packets( world->server );
    world->time += STRESS_FRAME_SECONDS;
    world->frames++;
}

// the world comes value-initialized (new stress_world_t()): every field zero
static void stress_world_create( stress_world_t * world )
{
    world->time = 1000.0;
    stress_now = world->time;
    rocketnet_quiet( true );
    rocketnet_public_link_simulator_test_development( 1 );

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    stress_server_config( &server_config, world->backend_public, 1, STRESS_NUM_CHANNELS );
    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    world->server = rocketnet_server_create( &world->server_count, "127.0.0.1:0", NULL, &server_config, &status );
    check( world->server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_server_network_simulator( world->server, STRESS_LATENCY_MS, STRESS_JITTER_MS, STRESS_LOSS_PERCENT, STRESS_DUPLICATE_PERCENT );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    stress_client_config( &client_config, STRESS_NUM_CHANNELS, stress_clock );
    status = ROCKETNET_TEST_STATUS_UNSET;
    world->client = rocketnet_client_create( &world->client_count, &client_config, &status );
    check( world->client != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_client_network_simulator( world->client, STRESS_LATENCY_MS, STRESS_JITTER_MS, STRESS_LOSS_PERCENT, STRESS_DUPLICATE_PERCENT );

    stress_side_create( &world->client_side, 0 );
    stress_side_create( &world->server_side, 1 );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world->backend_secret, world->server, STRESS_PROTOCOL, addresses, 1, 0x5157E55ULL, 1000,
                                        1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( world->client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 2000 && !( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
                                    rocketnet_server_client_connected( world->server, 0 ) ); i++ )
    {
        stress_world_frame( world );
        stress_world_send_packets( world );
    }
    check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( world->client ) == 0 );
    check( rocketnet_server_client_connected( world->server, 0 ) );
}

static void stress_world_destroy( stress_world_t * world )
{
    rn_test_client_stop_and_destroy( world->client );
    rn_test_server_stop_and_destroy( world->server );
    stress_side_destroy( &world->client_side );
    stress_side_destroy( &world->server_side );
    rocketnet_public_link_simulator_test_development( 0 );
}

// one round: one message on every channel, each way
static void stress_world_send_round( stress_world_t * world, int round )
{
    for ( int c = 0; c < STRESS_NUM_CHANNELS; c++ )
    {
        const enum rocketnet_reliability_t mode = stress_mode( c );
        const int max_bytes = STRESS_HEADER_BYTES + stress_pattern_bytes( c, round );

        check( rocketnet_client_can_send_message( world->client, c, mode, max_bytes ) );
        uint8_t * out = rocketnet_client_start_message( world->client, c, mode, max_bytes );
        check( out != NULL );
        check( stress_write_message( out, 0, c, round ) == max_bytes );
        stress_side_record_send( &world->client_side, c, rocketnet_client_finish_message( world->client, max_bytes ) );

        check( rocketnet_server_can_send_message( world->server, 0, c, mode, max_bytes ) );
        out = rocketnet_server_start_message( world->server, 0, c, mode, max_bytes );
        check( out != NULL );
        check( stress_write_message( out, 1, c, round ) == max_bytes );
        stress_side_record_send( &world->server_side, c, rocketnet_server_finish_message( world->server, max_bytes ) );
    }
}

// every reliable message of the other end arrived, and the newest of every
// reliable sequenced channel
static bool stress_side_complete( const stress_side_t * side )
{
    for ( int c = 0; c < STRESS_NUM_CHANNELS; c++ )
    {
        const enum rocketnet_reliability_t mode = stress_mode( c );
        if ( !stress_mode_reliable( mode ) )
            continue;
        if ( mode == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED )
        {
            if ( side->last_round[c] != STRESS_ROUNDS - 1 )
                return false;
            continue;
        }
        for ( int r = 0; r < STRESS_ROUNDS; r++ )
            if ( side->received[c * STRESS_ROUNDS + r] != 1 )
                return false;
    }
    return true;
}

static int stress_reliable_receipt_sends( const stress_side_t * side )
{
    (void) side;
    int count = 0;
    for ( int c = 0; c < STRESS_NUM_CHANNELS; c++ )
    {
        const enum rocketnet_reliability_t mode = stress_mode( c );
        if ( stress_mode_has_receipt( mode ) && stress_mode_reliable( mode ) )
            count += STRESS_ROUNDS;
    }
    return count;
}

static void test_channels_stress_every_channel_delivers()
{
    stress_world_t * world = new stress_world_t();
    stress_world_create( world );

    // from here every allocation would be the frame loop's
    const uint64_t allocations_before = stress_allocations_total.load();

    for ( int round = 0; round < STRESS_ROUNDS; round++ )
    {
        stress_world_frame( world );
        stress_world_send_round( world, round );
        stress_world_send_packets( world );
        check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( world->server, 0 ) );
    }
    const int send_frames = world->frames;
    const int reliable_receipts = stress_reliable_receipt_sends( &world->client_side );
    for ( int i = 0; i < STRESS_DRAIN_FRAMES; i++ )
    {
        if ( stress_side_complete( &world->client_side ) && stress_side_complete( &world->server_side ) &&
             world->client_side.reliable_receipts_acked == reliable_receipts &&
             world->server_side.reliable_receipts_acked == reliable_receipts )
            break;
        stress_world_frame( world );
        stress_world_send_packets( world );
        check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( world->server, 0 ) );
    }

    const uint64_t loop_allocations = stress_allocations_total.load() - allocations_before;

    check( stress_side_complete( &world->client_side ) );
    check( stress_side_complete( &world->server_side ) );
    check( world->client_side.reliable_receipts_acked == reliable_receipts );
    check( world->server_side.reliable_receipts_acked == reliable_receipts );
    check( loop_allocations == 0 );

    struct rocketnet_client_stats_t client_stats;
    struct rocketnet_server_stats_t server_stats;
    rocketnet_client_stats( world->client, &client_stats );
    rocketnet_server_stats( world->server, &server_stats );
    check( client_stats.messages_dropped_over_budget == 0 );
    check( server_stats.messages_dropped_over_budget == 0 );
    check( client_stats.messages_dropped_not_connected == 0 );
    check( server_stats.messages_dropped_not_connected == 0 );
    check( client_stats.receipts_dropped == 0 );
    check( server_stats.receipts_dropped == 0 );
    check( client_stats.api_misuses == 0 );
    check( server_stats.api_misuses == 0 );
    // the link simulator was on: something was lost and resent
    check( client_stats.messages_resent > 0 );
    check( server_stats.messages_resent > 0 );

    printf( "channels %d rounds %d messages %d each way: client received %llu, server received %llu; "
            "%d send frames, %d frames in all (%.2f s simulated); resent client %llu server %llu; "
            "packets sent client %llu server %llu\n",
            STRESS_NUM_CHANNELS, STRESS_ROUNDS, STRESS_MESSAGES_PER_SIDE,
            (unsigned long long) world->client_side.messages_received, (unsigned long long) world->server_side.messages_received,
            send_frames, world->frames, world->time - 1000.0,
            (unsigned long long) client_stats.messages_resent, (unsigned long long) server_stats.messages_resent,
            (unsigned long long) client_stats.packets_sent, (unsigned long long) server_stats.packets_sent );

    stress_world_destroy( world );
    delete world;
    printf( "test_channels_stress_every_channel_delivers\n" );
}

static size_t stress_client_plan( int channels )
{
    rocketnet_client_test_config_t config;
    stress_client_config( &config, channels, stress_clock );
    return rocketnet_client_required_memory( &config );
}

static size_t stress_server_plan( int max_clients, int channels )
{
    uint8_t key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    memset( key, 0x11, sizeof( key ) );
    rocketnet_server_config_t config;
    stress_server_config( &config, key, max_clients, channels );
    return rocketnet_server_required_memory( &config );
}

static void test_channels_stress_memory_is_planned()
{
    rocketnet_quiet( true );
    const uint64_t overhead = rocketnet_allocation_overhead();

    // the client: create takes one block, the plan's bytes
    const size_t client_plan = stress_client_plan( STRESS_NUM_CHANNELS );
    check( client_plan > 0 );
    {
        stress_allocation_count_t count;
        stress_count_reset( &count );
        rocketnet_client_test_config_t config;
        stress_client_config( &config, STRESS_NUM_CHANNELS, stress_clock );
        rocketnet_client_t * client = rocketnet_client_create( &count, &config, NULL );
        check( client != NULL );
        check( count.allocations.load() == 1 );
        check( count.bytes.load() == (uint64_t) client_plan + overhead );
        rocketnet_client_destroy( client );
    }

    // the server: create takes one block, the plan's bytes, at one slot and at two
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0xA5 ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );
    size_t server_plans[2];
    for ( int max_clients = 1; max_clients <= 2; max_clients++ )
    {
        server_plans[max_clients - 1] = stress_server_plan( max_clients, STRESS_NUM_CHANNELS );
        check( server_plans[max_clients - 1] > 0 );
        stress_allocation_count_t count;
        stress_count_reset( &count );
        rocketnet_server_config_t config;
        stress_server_config( &config, backend_public, max_clients, STRESS_NUM_CHANNELS );
        rocketnet_server_t * server = rocketnet_server_create( &count, "127.0.0.1:0", NULL, &config, NULL );
        check( server != NULL );
        check( count.allocations.load() == 1 );
        check( count.bytes.load() == (uint64_t) server_plans[max_clients - 1] + overhead );
        rocketnet_server_destroy( server );
    }
    check( server_plans[1] > server_plans[0] );
    const size_t server_connection_bytes = server_plans[1] - server_plans[0];

    // linear in the channel count: each per-channel table is rounded up to
    // the allocation alignment, so every 64 more channels add the same bytes
    // exactly (64 channels of any table are whole multiples of the alignment),
    // and the plan never falls as the count rises
    const int stride = ROCKETNET_ALLOCATION_ALIGNMENT;
    const size_t client_step = stress_client_plan( 1 + stride ) - stress_client_plan( 1 );
    const size_t connection_step = ( stress_server_plan( 2, 1 + stride ) - stress_server_plan( 1, 1 + stride ) ) -
                                   ( stress_server_plan( 2, 1 ) - stress_server_plan( 1, 1 ) );
    check( client_step > 0 && connection_step > 0 );
    size_t previous = 0;
    for ( int channels = 1; channels <= STRESS_NUM_CHANNELS; channels++ )
    {
        const size_t plan = stress_client_plan( channels );
        check( plan >= previous );
        previous = plan;
        if ( channels + stride <= STRESS_NUM_CHANNELS )
            check( stress_client_plan( channels + stride ) - plan == client_step );
    }
    static const int server_counts[] = { 1, 2, 3, 63, 64, 65, 255, 256, 257, STRESS_NUM_CHANNELS / 2, STRESS_NUM_CHANNELS - 65, STRESS_NUM_CHANNELS - 64 };
    for ( size_t i = 0; i < sizeof( server_counts ) / sizeof( server_counts[0] ); i++ )
    {
        const int channels = server_counts[i];
        const size_t connection = stress_server_plan( 2, channels ) - stress_server_plan( 1, channels );
        const size_t connection_next = stress_server_plan( 2, channels + stride ) - stress_server_plan( 1, channels + stride );
        check( connection_next - connection == connection_step );
    }
    const int top = STRESS_NUM_CHANNELS - stride;
    check( ( stress_server_plan( 2, STRESS_NUM_CHANNELS ) - stress_server_plan( 1, STRESS_NUM_CHANNELS ) ) -
           ( stress_server_plan( 2, top ) - stress_server_plan( 1, top ) ) == connection_step );
    const double client_per_channel = (double) client_step / stride;
    const double connection_per_channel = (double) connection_step / stride;

    printf( "channels %d: client plan %llu bytes (%.2f per channel), server bytes per connection %llu (%.2f per channel)\n",
            STRESS_NUM_CHANNELS, (unsigned long long) client_plan, client_per_channel,
            (unsigned long long) server_connection_bytes, connection_per_channel );
    printf( "test_channels_stress_memory_is_planned\n" );
}

int main( int argc, char ** argv )
{
    (void) argc;
    (void) argv;
    check( rocketnet_init( NULL, stress_malloc, stress_free ) == ROCKETNET_STATUS_OK );
    test_channels_stress_memory_is_planned();
    test_channels_stress_every_channel_delivers();
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    return 0;
}
