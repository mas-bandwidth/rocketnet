/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_replay_marks.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_internal.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_memory.h"
#include "rocketnet_xdp_replay_marks.h"

#include <string.h>

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
#define ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS( X ) \
    X( marked_value, uint32_t marked_value )                       \
    X( seen_address, struct rocketnet_address_t seen_address )     \
    X( has_mark, uint8_t has_mark )                                \
    X( seen, uint8_t seen )                                        \
    X( mark_owed, uint8_t mark_owed )

struct rocketnet_server_replay_slot_mark_t
{
#define ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS( ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE
};

// The seen slots are indexed by their address in an open-addressed table of
// two entries per slot, so a forget finds the other records of its address
// without a scan of every slot. The table follows the slots in their one
// allocation: each entry is a slot index + 1, zero empty, and the table is at
// most half full, since each slot is in it at most once.

#define ROCKETNET_SERVER_REPLAY_ADDRESS_ENTRIES_PER_SLOT 2

struct rocketnet_server_replay_marks_t
{
    void * context;
    int max_clients;
#if ROCKETNET_DEV
    uint32_t comparisons;               // every address table probe, for the unit cost cases
#endif // #if ROCKETNET_DEV
    struct rocketnet_server_replay_slot_mark_t * slots;
};

static uint64_t rocketnet_server_replay_marks_storage_bytes( int max_clients )
{
    return ( sizeof( struct rocketnet_server_replay_slot_mark_t ) + ROCKETNET_SERVER_REPLAY_ADDRESS_ENTRIES_PER_SLOT * sizeof( uint32_t ) ) * (uint64_t) max_clients;
}

uint64_t rocketnet_server_replay_marks_allocation_bytes( int max_clients )
{
    if ( max_clients < 1 )
        return 0;

    return rocketnet_memory_allocations( { sizeof( struct rocketnet_server_replay_marks_t ),
                                           rocketnet_server_replay_marks_storage_bytes( max_clients ) } );
}

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients )
{
    if ( max_clients < 1 )
        return NULL;

    struct rocketnet_server_replay_marks_t * marks = (struct rocketnet_server_replay_marks_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_replay_marks_t ) );
    if ( !marks )
        return NULL;
    memset( marks, 0, sizeof( struct rocketnet_server_replay_marks_t ) );

    marks->context = context;
    marks->max_clients = max_clients;
    marks->slots = (struct rocketnet_server_replay_slot_mark_t *) rocketnet_malloc( context, (size_t) rocketnet_server_replay_marks_storage_bytes( max_clients ) );

    if ( !marks->slots )
    {
        rocketnet_free( context, marks );
        return NULL;
    }

    memset( marks->slots, 0, (size_t) rocketnet_server_replay_marks_storage_bytes( max_clients ) );

    return marks;
}

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks )
{
    rocketnet_assert( marks );
    rocketnet_free( marks->context, marks->slots );
    rocketnet_free( marks->context, marks );
}

#if ROCKETNET_DEV
uint32_t rocketnet_server_replay_marks_comparisons_for_test( const struct rocketnet_server_replay_marks_t * marks )
{
    return marks->comparisons;
}
#endif // #if ROCKETNET_DEV

static uint32_t * rocketnet_server_replay_marks_address_table( struct rocketnet_server_replay_marks_t * marks )
{
    return (uint32_t *) ( marks->slots + marks->max_clients );
}

static uint32_t rocketnet_server_replay_marks_address_size( const struct rocketnet_server_replay_marks_t * marks )
{
    return (uint32_t) marks->max_clients * ROCKETNET_SERVER_REPLAY_ADDRESS_ENTRIES_PER_SLOT;
}

static uint32_t rocketnet_server_replay_marks_address_home( const struct rocketnet_server_replay_marks_t * marks, const struct rocketnet_address_t * address )
{
    uint32_t hash = 2166136261u;
    hash ^= address->type;
    hash *= 16777619u;
    hash ^= (uint8_t) address->port;
    hash *= 16777619u;
    hash ^= (uint8_t) ( address->port >> 8 );
    hash *= 16777619u;

    const uint8_t * bytes = (const uint8_t *) &address->data;
    const uint32_t count = address->type == ROCKETNET_ADDRESS_IPV4 ? 4 : address->type == ROCKETNET_ADDRESS_IPV6 ? 16
                                                                                                                 : 0;
    for ( uint32_t i = 0; i < count; i++ )
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash % rocketnet_server_replay_marks_address_size( marks );
}

static uint32_t rocketnet_server_replay_marks_address_step( const struct rocketnet_server_replay_marks_t * marks, uint32_t position )
{
    return position + 1 == rocketnet_server_replay_marks_address_size( marks ) ? 0 : position + 1;
}

// Empties table position hole and shifts the rest of its probe run back, so
// the table never holds a tombstone.

static void rocketnet_server_replay_marks_address_erase_at( struct rocketnet_server_replay_marks_t * marks, uint32_t hole )
{
    uint32_t * table = rocketnet_server_replay_marks_address_table( marks );
    const uint32_t size = rocketnet_server_replay_marks_address_size( marks );

    table[hole] = 0;
    uint32_t next = rocketnet_server_replay_marks_address_step( marks, hole );
    while ( table[next] != 0 )
    {
#if ROCKETNET_DEV
        marks->comparisons++;
#endif // #if ROCKETNET_DEV
        const uint32_t home = rocketnet_server_replay_marks_address_home( marks, &marks->slots[table[next] - 1].seen_address );
        if ( ( next + size - home ) % size >= ( next + size - hole ) % size )
        {
            table[hole] = table[next];
            table[next] = 0;
            hole = next;
        }
        next = rocketnet_server_replay_marks_address_step( marks, next );
    }
}

static void rocketnet_server_replay_marks_address_remove( struct rocketnet_server_replay_marks_t * marks, int index )
{
    if ( !marks->slots[index].seen )
        return;

    const uint32_t * table = rocketnet_server_replay_marks_address_table( marks );
    uint32_t position = rocketnet_server_replay_marks_address_home( marks, &marks->slots[index].seen_address );
    while ( table[position] != 0 )
    {
#if ROCKETNET_DEV
        marks->comparisons++;
#endif // #if ROCKETNET_DEV
        if ( table[position] == (uint32_t) index + 1 )
        {
            rocketnet_server_replay_marks_address_erase_at( marks, position );
            return;
        }
        position = rocketnet_server_replay_marks_address_step( marks, position );
    }
}

static void rocketnet_server_replay_marks_address_insert( struct rocketnet_server_replay_marks_t * marks, int index )
{
    uint32_t * table = rocketnet_server_replay_marks_address_table( marks );
    uint32_t position = rocketnet_server_replay_marks_address_home( marks, &marks->slots[index].seen_address );
    while ( table[position] != 0 )
    {
#if ROCKETNET_DEV
        marks->comparisons++;
#endif // #if ROCKETNET_DEV
        position = rocketnet_server_replay_marks_address_step( marks, position );
    }
    table[position] = (uint32_t) index + 1;
}

static void rocketnet_server_replay_marks_clear_slot( struct rocketnet_server_replay_marks_t * marks, int index )
{
    rocketnet_server_replay_marks_address_remove( marks, index );
    memset( &marks->slots[index], 0, sizeof( marks->slots[index] ) );
}

// proofs/tla/RNServerReplaySlots.tla Walk (MarkSupersedes, ForgetSupersedes): a
// queued forget or mark of an address clears every other slot's record of that
// address. Such a record is a refused forget left by a slot that is no longer
// there, or a mark the queued forget is about to delete. Kept, the record's
// later forget would delete the entry of the slot now at the address, or its
// slot, back at the address, would believe a mark another slot replaced is
// still in the kernel map (KernelHoldsMark). A slot cleared while connected is
// first seen again and marked in the same walk.
static void rocketnet_server_replay_marks_supersede( struct rocketnet_server_replay_marks_t * marks,
                                                     int keep,
                                                     const struct rocketnet_address_t * address )
{
    // a cleared record leaves its position, and the erase shifts the rest of
    // the probe run back into it, so that position is read again

    const uint32_t * table = rocketnet_server_replay_marks_address_table( marks );
    uint32_t position = rocketnet_server_replay_marks_address_home( marks, address );
    while ( table[position] != 0 )
    {
#if ROCKETNET_DEV
        marks->comparisons++;
#endif // #if ROCKETNET_DEV
        const int i = (int) table[position] - 1;
        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];
        if ( i != keep && rocketnet_address_equal( &slot->seen_address, address ) )
        {
            rocketnet_server_replay_marks_address_erase_at( marks, position );
            memset( slot, 0, sizeof( *slot ) );
            continue;
        }
        position = rocketnet_server_replay_marks_address_step( marks, position );
    }
}

// proofs/tla/RNServerReplaySlots.tla Walk: the forget pass clears a record only
// when the sink queued its forget, and the mark pass skips a slot whose record
// names another address (ForgetNeverLost)
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

    // proofs/tla/RNXdpReplay.tla Walk: first the forgets -- every slot that was seen
    // connected and is no longer connected, or whose mapping changed address,
    // whether or not it held a mark: the transport's forget drops the reply
    // MAC of that address too, and a delete of a missing key is harmless.

    int refused_forgets = 0;

    for ( int i = 0; i < marks->max_clients; i++ )
    {
        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];
        if ( !slot->seen )
            continue;

        int forget = 0;

        if ( !rocketnet_client_slots_connected( slots, i ) )
        {
            forget = 1;
        }
        else
        {
            const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
            if ( mapping_index < 0 )
            {
                forget = 1;
            }
            else
            {
                const struct rocketnet_address_t * address = rocketnet_encryption_map_address( map, mapping_index );
                if ( !rocketnet_address_equal( address, &slot->seen_address ) )
                    forget = 1;
            }
        }

        if ( forget )
        {
            // Queue acceptance transfers responsibility to the transport's
            // pending buffer. Until then this slot keeps the old identity.
            const struct rocketnet_address_t forgotten = slot->seen_address;
            if ( sink->forget( sink->context, &forgotten ) )
            {
                rocketnet_server_replay_marks_clear_slot( marks, i );
                rocketnet_server_replay_marks_supersede( marks, i, &forgotten );
            }
            else
            {
                refused_forgets++;
            }
        }
    }

    // then the marks -- every connected slot first seen (zero included),
    // whose highest_opened fell below its last mark, or whose highest_opened
    // advanced by the mark step. Every connected slot with a
    // mapping is seen at its address, marked or not, so that the forget pass
    // can drop the reply MAC of a slot that left before it ever marked.

    for ( int i = 0; i < marks->max_clients; i++ )
    {
        if ( !rocketnet_client_slots_connected( slots, i ) )
            continue;

        const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
        if ( mapping_index < 0 )
            continue;
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( map, mapping_index );
        const uint64_t highest_opened = mapping->replay_session.highest_opened;

        const struct rocketnet_address_t * address = rocketnet_encryption_map_address( map, mapping_index );

        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];

        // A refused forget for an old mapping must be retried before the
        // current mapping replaces its seen or marked identity.

        if ( slot->seen && !rocketnet_address_equal( address, &slot->seen_address ) )
            continue;

        // proofs/tla/RNXdpReplay.tla Marks, NewSessionNeverDroppedByOldMark: a slot
        // first seen, or whose highest_opened fell below its mark (a new
        // session in the slot), is marked even at zero, so the commit before
        // its first keep-alive leaves overwrites any older session's entry
        // for the address, whether or not that session's forget has landed.

        const bool first_seen = !slot->seen;
        const bool mark_owed = slot->mark_owed != 0;

        // a seen slot is here only at the address it was seen at, which the
        // address table already holds

        if ( first_seen )
        {
            slot->seen = 1;
            slot->seen_address = *address;
            rocketnet_server_replay_marks_address_insert( marks, i );
        }

        // A steady advance coalesces until it reaches the mark step. A fall
        // is a new session in the slot and is marked at once, as a first-seen
        // slot is, so its first keep-alive cannot meet the old session's entry.

        const uint32_t high_water = (uint32_t) highest_opened;
        const uint32_t last_marked = slot->has_mark ? slot->marked_value : 0;
        const bool decreased = !first_seen && high_water < last_marked;

        if ( !first_seen && !mark_owed && !decreased && high_water - last_marked < ROCKETNET_XDP_REPLAY_MARK_STEP )
            continue;

        // proofs/tla/RNXdpReplay.tla Walk, AdvanceOnQueueRefusal = FALSE: a mark the
        // sink cannot queue leaves the cache where it was and the slot owes
        // it, so the next walk takes it again (MarkOwedUntilApplied)

        if ( !sink->mark( sink->context, address, high_water ) )
        {
            slot->mark_owed = 1;
            continue;
        }

        slot->mark_owed = 0;
        slot->has_mark = 1;
        slot->marked_value = high_water;

        // only a forget refused in this walk leaves another slot's record
        // naming a connected slot's address: every other record was cleared
        // by its own queued forget or names the address its slot is at

        if ( refused_forgets > 0 )
            rocketnet_server_replay_marks_supersede( marks, i, address );
    }
}

#if ROCKETNET_DEV
static bool rocketnet_replay_witness_forget( void * context, const rocketnet_address_t * )
{
    ( *(int *) context )++;
    return true;
}

static bool rocketnet_replay_witness_mark( void *, const rocketnet_address_t *, uint32_t ) { return true; }

bool rocketnet_server_replay_marks_inventory_matches_for_test( char * failure, size_t failure_bytes )
{
#define MARK_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_server_replay_slot_mark_t, field, -1 ),
    static const rocketnet_reset_member_t members[] = { ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS( MARK_MEMBER ) };
#undef MARK_MEMBER
#define MARK_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_server_replay_slot_mark_t, field, #decision[0] == 'R' ),
    // The shared inventory in rocketnet_connection_reset.h lists the mark
    // record's fields; the walk's seen record and owed flag are three more
    // members, and their RESET decisions are listed here so that every member
    // of the struct has one and the inventory covers the whole struct.
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_SERVER_REPLAY_MARK_FIELDS( MARK_DECISION )
                                                              MARK_DECISION( RESET, seen )
                                                                  MARK_DECISION( RESET, seen_address )
                                                                      MARK_DECISION( RESET, mark_owed ) };
#undef MARK_DECISION
    return rocketnet_reset_inventory_matches( "rocketnet_server_replay_slot_mark_t", members, sizeof( members ) / sizeof( members[0] ),
                                              decisions, sizeof( decisions ) / sizeof( decisions[0] ), 0,
                                              sizeof( rocketnet_server_replay_slot_mark_t ),
                                              alignof( rocketnet_server_replay_slot_mark_t ), failure, failure_bytes );
}

bool rocketnet_server_replay_marks_reset_witness_for_test()
{
    rocketnet_server_replay_marks_t * marks = rocketnet_server_replay_marks_create( NULL, 1 );
    rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 1 );
    rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 1 );
    if ( !marks || !slots || !map )
    {
        if ( marks )
            rocketnet_server_replay_marks_destroy( marks );
        if ( slots )
            rocketnet_client_slots_destroy( slots );
        if ( map )
            rocketnet_encryption_map_destroy( map );
        return false;
    }

    rocketnet_address_t address;
    const bool parsed = rocketnet_address_parse( &address, "198.51.100.91:4091" );
    uint8_t user[ROCKETNET_USER_DATA_BYTES] = {};
    const int mapping = parsed ? rocketnet_encryption_map_add( map, &address, 1005.0 ) : -1;
    const uint64_t owner_generation = mapping >= 0 ? rocketnet_encryption_map_owner_generation( map, mapping ) : 1;
    const int index = mapping >= 0 ? rocketnet_client_slots_assign( slots, 1, mapping, owner_generation, user ) : -1;
    if ( index == 0 )
    {
        rocketnet_encryption_mapping_t * entry = rocketnet_encryption_map_entry( map, mapping );
        entry->control->client_index = index;
        entry->replay_session.highest_opened = 0xA5A5;
    }

    int forgets = 0;
    rocketnet_server_replay_sink_t sink = { &forgets, rocketnet_replay_witness_forget, rocketnet_replay_witness_mark };

    if ( index == 0 )
    {
        rocketnet_server_replay_marks_walk( marks, slots, map, &sink );
        rocketnet_client_slots_release( slots, index );
        rocketnet_encryption_map_remove_current( map, mapping );
        rocketnet_server_replay_marks_walk( marks, slots, map, &sink );
    }

    const uint8_t zero[sizeof( rocketnet_server_replay_slot_mark_t )] = {};
    const bool clean = index == 0 && forgets == 1 && memcmp( &marks->slots[0], zero, sizeof( zero ) ) == 0;
    rocketnet_server_replay_marks_destroy( marks );
    rocketnet_client_slots_destroy( slots );
    rocketnet_encryption_map_destroy( map );

    return clean;
}
#endif // #if ROCKETNET_DEV
