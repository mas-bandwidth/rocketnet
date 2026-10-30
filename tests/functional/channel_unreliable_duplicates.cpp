/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNChannelTypes!Dispatch for code 0 over a link that duplicates half of its
// packets. A plain UNRELIABLE record takes no number and has no window check
// of its own (standard/MESSAGES.md m7.4), so its at-most-once delivery rests on
// the transport replay window alone. This case asserts the property where a
// user sees it, at the channel delivery, and that the duplicates really rode
// the wire so the case cannot pass on a link that never duplicated.
#include "channel_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUP_KINDS               3       // code 0, code 1, one reliable type
#define DUP_MESSAGES            400     // per kind per direction (ids 0..399)
#define DUP_PAYLOAD_BYTES       16
#define DUP_PER_TICK            4       // code 0 sends per update
#define DUP_SEND_TICKS          ( DUP_MESSAGES / DUP_PER_TICK )
#define DUP_SETTLE_TICKS        600     // ten seconds of test time after the last send

static const int dup_codes[DUP_KINDS] = { ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_RELIABLE };
static const char * dup_names[DUP_KINDS] = { "unreliable", "unreliable_sequenced", "reliable" };

static void dup_check( bool condition, const char * label )
{
    if ( condition )
        return;
    printf( "unreliable at most once failed: %s\n", label );
    fflush( stdout );
    exit( 1 );
}

static void dup_fail_twice( int direction, int kind, uint32_t id )
{
    printf( "unreliable at most once failed: message delivered twice: %s received kind=%s id=%u again\n",
            direction ? "client" : "server", dup_names[kind], id );
    fflush( stdout );
    exit( 1 );
}

// payload: kind byte, three bytes of id, then a pattern derived from kind, direction and id
static uint8_t dup_pattern( int direction, int kind, uint32_t id, int i )
{
    return (uint8_t) ( id * 31u + (uint32_t) i * 17u + (uint32_t) kind * 101u + (uint32_t) direction * 7u );
}

static void dup_make( uint8_t * bytes, int direction, int kind, uint32_t id )
{
    bytes[0] = (uint8_t) kind;
    bytes[1] = (uint8_t) id;
    bytes[2] = (uint8_t) ( id >> 8 );
    bytes[3] = (uint8_t) ( id >> 16 );

    for ( int i = 4; i < DUP_PAYLOAD_BYTES; i++ )
    {
        bytes[i] = dup_pattern( direction, kind, id, i );
    }
}

typedef struct dup_seen_t
{
    uint8_t seen[2][DUP_KINDS][DUP_MESSAGES];   // [direction][kind][id]; direction 0 = to server, 1 = to client
    uint32_t delivered[2][DUP_KINDS];
} dup_seen_t;

static void dup_receive( dup_seen_t * s, int direction, const rocketnet_channel_delivery_t * d )
{
    dup_check( d->bytes == DUP_PAYLOAD_BYTES, "payload length" );

    const int kind = d->data[0];
    dup_check( kind >= 0 && kind < DUP_KINDS, "payload kind" );

    const uint32_t id = (uint32_t) d->data[1] | ( (uint32_t) d->data[2] << 8 ) | ( (uint32_t) d->data[3] << 16 );
    dup_check( id < DUP_MESSAGES, "payload id" );

    for ( int i = 4; i < DUP_PAYLOAD_BYTES; i++ )
    {
        dup_check( d->data[i] == dup_pattern( direction, kind, id, i ), "payload intact" );
    }

    if ( s->seen[direction][kind][id] )
    {
        dup_fail_twice( direction, kind, id );
    }

    s->seen[direction][kind][id] = 1;
    s->delivered[direction][kind]++;
}

static void dup_leg( float loss_percent, const char * label )
{
    rocketnet_channel_harness_t h;
    dup_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    dup_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.02, 0.03, loss_percent, 50.0f );

    static dup_seen_t s;
    memset( &s, 0, sizeof( s ) );

    uint32_t next_id[DUP_KINDS] = { 0, 0, 0 };

    for ( int tick = 0; tick < DUP_SEND_TICKS + DUP_SETTLE_TICKS; tick++ )
    {
        if ( tick < DUP_SEND_TICKS )
        {
            for ( int n = 0; n < DUP_PER_TICK; n++ )
            {
                uint8_t bytes[DUP_PAYLOAD_BYTES];
                const uint32_t id = next_id[0]++;
                dup_make( bytes, 0, 0, id );
                dup_check( rocketnet_channel_harness_client_send( &h, 0, dup_codes[0], bytes, DUP_PAYLOAD_BYTES ) != 0, "client send code 0" );
                dup_make( bytes, 1, 0, id );
                dup_check( rocketnet_channel_harness_server_send( &h, 0, dup_codes[0], bytes, DUP_PAYLOAD_BYTES ) != 0, "server send code 0" );
            }

            // controls: one code 1 and one reliable message per update each way

            for ( int kind = 1; kind < DUP_KINDS; kind++ )
            {
                uint8_t bytes[DUP_PAYLOAD_BYTES];
                const uint32_t id = next_id[kind]++;
                dup_make( bytes, 0, kind, id );
                dup_check( rocketnet_channel_harness_client_send( &h, 0, dup_codes[kind], bytes, DUP_PAYLOAD_BYTES ) != 0, "client send control" );
                dup_make( bytes, 1, kind, id );
                dup_check( rocketnet_channel_harness_server_send( &h, 0, dup_codes[kind], bytes, DUP_PAYLOAD_BYTES ) != 0, "server send control" );
            }
        }

        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

        rocketnet_channel_delivery_t d;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            dup_receive( &s, 0, &d );
        }

        while ( rocketnet_channel_harness_client_collect( &h, 0, &d ) )
        {
            dup_receive( &s, 1, &d );
        }
    }

    rocketnet_channel_harness_read_counters( &h );
    dup_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    dup_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );

    // the hazard was present: duplicated payload packets reached each receiver
    // and the replay window refused them

    const uint64_t dup_to_server = h.server_counters.values[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED];
    const uint64_t dup_to_client = h.client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED];
    dup_check( dup_to_server > 0, "no duplicate reached the server" );
    dup_check( dup_to_client > 0, "no duplicate reached the client" );

    // the property needs messages to have flowed

    for ( int direction = 0; direction < 2; direction++ )
    {
        dup_check( s.delivered[direction][0] >= ( loss_percent > 0.0f ? DUP_MESSAGES / 2u : (uint32_t) DUP_MESSAGES ), "code 0 deliveries" );
        dup_check( s.delivered[direction][1] > 0, "code 1 deliveries" );
        dup_check( s.delivered[direction][2] > 0, "reliable deliveries" );

        if ( loss_percent == 0.0f )
        {
            dup_check( s.delivered[direction][2] == (uint32_t) DUP_SEND_TICKS, "reliable control all delivered on the lossless leg" );
        }
    }

    printf( "test_unreliable_at_most_once_over_duplicating_link %s: dup_to_server=%llu dup_to_client=%llu"
            " code0=%u/%u code1=%u/%u reliable=%u/%u seed=0x5EED5EED5EED5EED\n",
            label, (unsigned long long) dup_to_server, (unsigned long long) dup_to_client,
            s.delivered[0][0], s.delivered[1][0], s.delivered[0][1], s.delivered[1][1],
            s.delivered[0][2], s.delivered[1][2] );
    rocketnet_channel_harness_destroy( &h );
}

void unreliable_at_most_once_over_duplicating_link()
{
    dup_leg( 0.0f, "lossless" );
    dup_leg( 10.0f, "loss10" );
}
