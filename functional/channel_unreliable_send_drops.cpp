/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The two places the object write path (rn_message_connection_write) drops an
// unreliable send over the send budget, each over the deterministic simulated
// link with no loss: RNSendBudget Wait then Drop (a reliable packet the budget
// could not pay is waiting) and the unreliable builder out of budget. Owner
// rule R6: a reliable message is never dropped for budget, it waits; an
// unreliable send over budget is dropped, counted in
// ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET, and gone: it never
// arrives in a later update, late and stale. The counter shows the drop; the
// two cases assert the absence over simulated time.
#include "channel_harness.h"
#include "../tests/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>

#define DROPS_RELIABLE_BYTES        1000
#define DROPS_UNRELIABLE_BYTES      24
#define DROPS_BUILDER_BYTES         300
#define DROPS_TICK                  ( 1.0 / 60.0 )

// ROCKETNET_TEST_VERBOSE set: print each case's counts
static bool drops_verbose()
{
    char value[256];
    return rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", value, sizeof( value ) ) != 0;
}

static void drops_check( bool condition, const char * label )
{
    if ( condition )
        return;
    printf( "unreliable send drops failed: %s\n", label );
    fflush( stdout );
    exit( 1 );
}

static uint64_t drops_over_budget( const rocketnet_channel_harness_t * h )
{
    return h->message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET];
}

// A payload whose first byte is the message's tag and whose rest is a
// function of the tag, so a delivery names the send it came from.
static void drops_fill( uint8_t * bytes, int count, uint8_t tag )
{
    for ( int i = 0; i < count; i++ )
    {
        bytes[i] = (uint8_t) ( tag + i * 7 );
    }
}

static bool drops_matches( const rocketnet_channel_delivery_t * d, int count, uint8_t tag )
{
    if ( d->bytes != count )
        return false;
    for ( int i = 0; i < count; i++ )
        if ( d->data[i] != (uint8_t) ( tag + i * 7 ) )
            return false;

    return true;
}

// Pump a number of ticks, idle, so the bucket refills to its depth.
static void drops_idle( rocketnet_channel_harness_t * h, int ticks )
{
    for ( int i = 0; i < ticks; i++ )
    {
        rocketnet_channel_harness_pump( h, DROPS_TICK );
    }
}

void unreliable_dropped_while_reliable_waits_never_arrives()
{
    rocketnet_channel_harness_t h;
    drops_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    drops_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    // the client sends at 8 kbps (a bucket of about one maximum packet); the
    // server's acks are not the subject and go at the default

    drops_check( rocketnet_channel_harness_set_budgets( &h, 8, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS ) != 0, "budgets" );
    drops_idle( &h, 60 * 3 );
    rocketnet_channel_harness_read_counters( &h );

    // one update: two reliable messages that each fill a packet (the first
    // spends the bucket, the second waits) and then four unreliable messages

    uint8_t reliable[2][DROPS_RELIABLE_BYTES];
    drops_fill( reliable[0], DROPS_RELIABLE_BYTES, 0xA0 );
    drops_fill( reliable[1], DROPS_RELIABLE_BYTES, 0xB0 );

    uint8_t unreliable[4][DROPS_UNRELIABLE_BYTES];
    const uint64_t before = drops_over_budget( &h );
    const uint64_t budget_dropped_before = h.message_client->object.connections[0].budget.unreliable_dropped;
    const uint64_t budget_dropped_bytes_before = h.message_client->object.connections[0].budget.unreliable_dropped_bytes;

    drops_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, reliable[0], DROPS_RELIABLE_BYTES ) != 0, "send reliable 0" );
    drops_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, reliable[1], DROPS_RELIABLE_BYTES ) != 0, "send reliable 1" );

    for ( int i = 0; i < 4; i++ )
    {
        drops_fill( unreliable[i], DROPS_UNRELIABLE_BYTES, (uint8_t) ( 0x10 + i ) );
        drops_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, unreliable[i], DROPS_UNRELIABLE_BYTES ) != 0, "send unreliable" );
    }

    // the update that finds the reliable packet waiting counts every
    // unreliable message of the update as over budget

    rocketnet_channel_harness_pump( &h, DROPS_TICK );
    drops_check( drops_over_budget( &h ) == before + 4, "over budget counter rises by the unreliable count" );

    // the drop is the wait-then-drop shortcut: the unreliable sends are
    // dropped without being offered to the budget one by one (RNSendBudget
    // Wait), so the budget's own drop tally does not move. When the shortcut
    // is absent the builder offers them and the budget drops them instead:
    // the same counter and the same deliveries, a different path

    const rocketnet_send_budget_t * budget = &h.message_client->object.connections[0].budget;
    drops_check( budget->unreliable_dropped == budget_dropped_before, "unreliable sends reached the budget while a reliable packet waited" );
    drops_check( budget->unreliable_dropped_bytes == budget_dropped_bytes_before, "unreliable bytes reached the budget while a reliable packet waited" );

    // at least two seconds of simulated time: no unreliable message ever
    // arrives; the reliable ones arrive, in order, as the bucket refills

    int reliable_arrived = 0;
    int unreliable_arrived = 0;
    double elapsed = DROPS_TICK;
    for ( int tick = 0; tick < 60 * 30 && ( reliable_arrived < 2 || elapsed < 4.0 ); tick++ )
    {
        rocketnet_channel_harness_pump( &h, DROPS_TICK );
        elapsed += DROPS_TICK;

        rocketnet_channel_delivery_t d;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            if ( d.bytes == DROPS_UNRELIABLE_BYTES )
            {
                unreliable_arrived++;
            }
            else if ( reliable_arrived < 2 && drops_matches( &d, DROPS_RELIABLE_BYTES, (uint8_t) ( reliable_arrived == 0 ? 0xA0 : 0xB0 ) ) )
            {
                reliable_arrived++;
            }
            else
            {
                drops_check( false, "a delivery that is neither a sent reliable message nor an unreliable one" );
            }
        }
    }

    drops_check( elapsed >= 2.0, "simulated two seconds" );
    drops_check( unreliable_arrived == 0, "a dropped unreliable message arrived late" );
    drops_check( reliable_arrived == 2, "both reliable messages arrive, in order" );
    drops_check( drops_over_budget( &h ) >= before + 4, "counter does not fall" );
    drops_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    drops_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    if ( drops_verbose() )
        printf( "test_unreliable_dropped_while_reliable_waits_never_arrives: over_budget=%llu reliable_arrived=%d elapsed=%.2f\n",
                (unsigned long long) ( drops_over_budget( &h ) - before ), reliable_arrived, elapsed );
    rocketnet_channel_harness_destroy( &h );
}

void unreliable_dropped_when_builder_out_of_budget_never_arrives()
{
    rocketnet_channel_harness_t h;
    drops_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    drops_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    drops_check( rocketnet_channel_harness_set_budgets( &h, 8, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS ) != 0, "budgets" );
    drops_idle( &h, 60 * 3 );
    rocketnet_channel_harness_read_counters( &h );

    // unreliable messages only, more bytes in one update than the bucket pays

    const int sent = 12;
    uint8_t message[DROPS_BUILDER_BYTES];
    const uint64_t before = drops_over_budget( &h );
    for ( int i = 0; i < sent; i++ )
    {
        drops_fill( message, DROPS_BUILDER_BYTES, (uint8_t) ( 0x20 + i ) );
        drops_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, message, DROPS_BUILDER_BYTES ) != 0, "send" );
    }

    // the update's packets carry the messages that fit, the earliest ones, and
    // the rest are counted over budget in this update

    rocketnet_channel_harness_pump( &h, DROPS_TICK );

    const uint64_t over_budget_after_write = drops_over_budget( &h );

    rocketnet_channel_harness_pump( &h, DROPS_TICK );      // the wire delivers at the next update

    int arrived = 0;
    rocketnet_channel_delivery_t d;
    while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
    {
        drops_check( arrived < sent && drops_matches( &d, DROPS_BUILDER_BYTES, (uint8_t) ( 0x20 + arrived ) ), "an arrival is the next of the earliest sends" );
        arrived++;
    }

    drops_check( arrived >= 1, "the messages that fit arrive in the update" );
    drops_check( arrived < sent, "some messages are over budget" );
    drops_check( over_budget_after_write == before + (uint64_t) ( sent - arrived ), "over budget counter equals the messages that did not fit" );

    // at least two seconds: none of the dropped messages ever arrives

    double elapsed = 0.0;

    for ( int tick = 0; tick < 60 * 3; tick++ )
    {
        rocketnet_channel_harness_pump( &h, DROPS_TICK );
        elapsed += DROPS_TICK;
        drops_check( !rocketnet_channel_harness_server_collect( &h, 0, &d ), "a dropped unreliable message arrived late" );
    }

    drops_check( elapsed >= 2.0, "simulated two seconds" );
    drops_check( drops_over_budget( &h ) == before + (uint64_t) ( sent - arrived ), "no further drops once idle" );
    drops_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    drops_check( rocketnet_channel_allocator_calls == h.allocations_after_create, "allocation after create" );
    if ( drops_verbose() )
        printf( "test_unreliable_dropped_when_builder_out_of_budget_never_arrives: sent=%d arrived=%d over_budget=%llu elapsed=%.2f\n",
                sent, arrived, (unsigned long long) ( drops_over_budget( &h ) - before ), elapsed );
    rocketnet_channel_harness_destroy( &h );
}
