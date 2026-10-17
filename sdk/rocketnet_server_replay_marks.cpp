/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_replay_marks.h"
#include "rocketnet.h"

#include <string.h>

struct rocketnet_server_replay_slot_mark_t
{
    int has_mark;
    uint32_t marked_value;
    struct rocketnet_address_t marked_address;
};

struct rocketnet_server_replay_marks_t
{
    void * context;
    int max_clients;
    struct rocketnet_server_replay_slot_mark_t * slots;
};

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients )
{
    if ( max_clients < 1 )
        return NULL;

    struct rocketnet_server_replay_marks_t * marks = (struct rocketnet_server_replay_marks_t *) rocketnet_malloc( context, sizeof(struct rocketnet_server_replay_marks_t) );
    if ( !marks )
        return NULL;
    memset( marks, 0, sizeof(struct rocketnet_server_replay_marks_t) );

    marks->context = context;
    marks->max_clients = max_clients;
    marks->slots = (struct rocketnet_server_replay_slot_mark_t *) rocketnet_malloc( context, sizeof(struct rocketnet_server_replay_slot_mark_t) * (size_t) max_clients );

    if ( !marks->slots )
    {
        rocketnet_free( context, marks );
        return NULL;
    }

    memset( marks->slots, 0, sizeof(struct rocketnet_server_replay_slot_mark_t) * (size_t) max_clients );

    return marks;
}

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks )
{
    rocketnet_assert( marks );
    rocketnet_free( marks->context, marks->slots );
    rocketnet_free( marks->context, marks );
}

void rocketnet_server_replay_marks_walk( struct rocketnet_server_replay_marks_t * marks,
                                         const struct rocketnet_client_slots_t * slots,
                                         struct rocketnet_encryption_map_t * map,
                                         const struct rocketnet_server_replay_sink_t * sink )
{
    rocketnet_assert( marks );
    rocketnet_assert( slots );
    rocketnet_assert( map );
    rocketnet_assert( sink );
    rocketnet_assert( sink->forget );
    rocketnet_assert( sink->mark );

    // tla/RNXdpReplay.tla Walk: first the forgets -- every slot that held a
    // mark and is no longer connected, or whose mapping changed address.

    for ( int i = 0; i < marks->max_clients; i++ )
    {
        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];
        if ( !slot->has_mark )
            continue;

        int forget = 0;

        if ( !rocketnet_client_slots_connected( slots, i ) )
        {
            forget = 1;
        }
        else
        {
            const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
            const struct rocketnet_address_t * address = rocketnet_encryption_map_address( map, mapping_index );
            if ( !rocketnet_address_equal( address, &slot->marked_address ) )
                forget = 1;
        }

        if ( forget )
        {
            sink->forget( sink->context, &slot->marked_address );
            slot->has_mark = 0;
        }
    }

    // then the marks -- every connected slot whose highest_opened is nonzero
    // and differs from what it last marked.

    for ( int i = 0; i < marks->max_clients; i++ )
    {
        if ( !rocketnet_client_slots_connected( slots, i ) )
            continue;

        const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( map, mapping_index );
        const uint64_t highest_opened = mapping->replay_session.highest_opened;
        if ( highest_opened == 0 )
            continue;

        const uint32_t high_water = (uint32_t) highest_opened;

        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];
        if ( slot->has_mark && slot->marked_value == high_water )
            continue;

        const struct rocketnet_address_t * address = rocketnet_encryption_map_address( map, mapping_index );
        sink->mark( sink->context, address, high_water );
        slot->has_mark = 1;
        slot->marked_value = high_water;
        slot->marked_address = *address;
    }
}
