/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_replay_marks.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_internal.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_memory.h"

#include <string.h>

/*
    The declaration manifest: the struct is declared from this list, so a
    member exists only as an entry here. The RESET/KEEP inventory in
    rocketnet_connection_reset.h is compared with it by name, and a member with
    no decision is named in test_connection_reset_inventory_covers_every_field.
*/
#define ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS( X ) \
    X( has_mark, int has_mark )                                    \
    X( marked_value, uint32_t marked_value )                       \
    X( seen, int seen )                                            \
    X( seen_address, struct rocketnet_address_t seen_address )     \
    X( mark_owed, int mark_owed )                                  \
    X( skip_marked, int skip_marked )                              \
    X( skip_address, struct rocketnet_address_t skip_address )

struct rocketnet_server_replay_slot_mark_t
{
#define ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE( name, declaration ) declaration;
    ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS( ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE )
#undef ROCKETNET_SERVER_REPLAY_SLOT_MARK_LAYOUT_FIELDS_DECLARE
};

struct rocketnet_server_replay_marks_t
{
    void * context;
    int max_clients;
    bool supersede_owed;                // the last walk left a refused forget: two records may name one address
#if ROCKETNET_DEV
    uint64_t comparisons;               // every record a supersede scan reads, for the unit cost case
#endif // #if ROCKETNET_DEV
    struct rocketnet_server_replay_slot_mark_t * slots;
};

uint64_t rocketnet_server_replay_marks_allocation_bytes( int max_clients )
{
    if ( max_clients < 1 )
    {
        return 0;
    }

    return rocketnet_memory_allocations( { sizeof( struct rocketnet_server_replay_marks_t ),
                                           sizeof( struct rocketnet_server_replay_slot_mark_t ) * (uint64_t) max_clients } );
}

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients )
{
    if ( max_clients < 1 )
    {
        return NULL;
    }

    struct rocketnet_server_replay_marks_t * marks = (struct rocketnet_server_replay_marks_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_replay_marks_t ) );
    if ( !marks )
    {
        return NULL;
    }
    memset( marks, 0, sizeof( struct rocketnet_server_replay_marks_t ) );

    marks->context = context;
    marks->max_clients = max_clients;
    marks->slots = (struct rocketnet_server_replay_slot_mark_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_replay_slot_mark_t ) * (size_t) max_clients );

    if ( !marks->slots )
    {
        rocketnet_free( context, marks );
        return NULL;
    }

    memset( marks->slots, 0, sizeof( struct rocketnet_server_replay_slot_mark_t ) * (size_t) max_clients );

    return marks;
}

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks )
{
    rocketnet_assert( marks );
    rocketnet_free( marks->context, marks->slots );
    rocketnet_free( marks->context, marks );
}

#if ROCKETNET_DEV
uint64_t rocketnet_server_replay_marks_comparisons_for_test( const struct rocketnet_server_replay_marks_t * marks )
{
    return marks->comparisons;
}

bool rocketnet_server_replay_marks_slot_for_test( const struct rocketnet_server_replay_marks_t * marks, int index, bool * has_mark, uint32_t * marked_value,
                                                  bool * seen, struct rocketnet_address_t * seen_address, bool * mark_owed )
{
    if ( !marks || index < 0 || index >= marks->max_clients )
    {
        return false;
    }
    const struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[index];
    *has_mark = slot->has_mark != 0;
    *marked_value = slot->marked_value;
    *seen = slot->seen != 0;
    *seen_address = slot->seen_address;
    *mark_owed = slot->mark_owed != 0;
    return true;
}
#endif // #if ROCKETNET_DEV

// The slot's record of the address it was seen at, cleared; the skip mark
// (skip_marked, skip_address) is the other address the slot answers for and
// is cleared only by its own forget or a supersede of that address
// (proofs/tla/RNServerReplaySlots.tla ClearRec)
static void rocketnet_server_replay_marks_clear_record( struct rocketnet_server_replay_slot_mark_t * slot )
{
    slot->has_mark = 0;
    slot->marked_value = 0;
    slot->seen = 0;
    memset( &slot->seen_address, 0, sizeof( slot->seen_address ) );
    slot->mark_owed = 0;
}

static void rocketnet_server_replay_marks_clear_skip( struct rocketnet_server_replay_slot_mark_t * slot )
{
    slot->skip_marked = 0;
    memset( &slot->skip_address, 0, sizeof( slot->skip_address ) );
}

// proofs/tla/RNServerReplaySlots.tla Walk (MarkSupersedes, ForgetSupersedes): a
// queued forget or mark of an address clears every other slot's record of that
// address, and its skip mark of it. Such a record is a refused forget left by
// a slot that is no longer there, or a mark the queued forget is about to
// delete. Kept, the record's later forget would delete the entry of the slot
// now at the address, or its slot, back at the address, would believe a mark
// another slot replaced is still in the kernel map (KernelHoldsMark). A slot
// cleared while connected is first seen again and marked in the same walk.
static void rocketnet_server_replay_marks_supersede( struct rocketnet_server_replay_marks_t * marks,
                                                     int keep,
                                                     const struct rocketnet_address_t * address )
{
    for ( int i = 0; i < marks->max_clients; i++ )
    {
#if ROCKETNET_DEV
        marks->comparisons++;
#endif // #if ROCKETNET_DEV
        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];
        if ( i == keep )
        {
            continue;
        }
        if ( slot->seen && rocketnet_address_equal( &slot->seen_address, address ) )
        {
            rocketnet_server_replay_marks_clear_record( slot );
        }
        if ( slot->skip_marked && rocketnet_address_equal( &slot->skip_address, address ) )
        {
            rocketnet_server_replay_marks_clear_skip( slot );
        }
    }
}

// the address slot i is connected at now, NULL when it is not connected or
// has no mapping
static const struct rocketnet_address_t * rocketnet_server_replay_marks_slot_address( const struct rocketnet_client_slots_t * slots,
                                                                                      struct rocketnet_encryption_map_t * map, int i )
{
    if ( !rocketnet_client_slots_connected( slots, i ) )
    {
        return NULL;
    }
    const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
    if ( mapping_index < 0 )
    {
        return NULL;
    }
    return rocketnet_encryption_map_address( map, mapping_index );
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
        if ( !slot->seen && !slot->skip_marked )
        {
            continue;
        }

        const struct rocketnet_address_t * current = rocketnet_server_replay_marks_slot_address( slots, map, i );

        if ( slot->seen && ( !current || !rocketnet_address_equal( current, &slot->seen_address ) ) )
        {
            // Queue acceptance transfers responsibility to the transport's
            // pending buffer. Until then this slot keeps the old identity.
            const struct rocketnet_address_t forgotten = slot->seen_address;
            if ( sink->forget( sink->context, &forgotten ) )
            {
                rocketnet_server_replay_marks_clear_record( slot );

                // proofs/tla/RNServerReplaySlots.tla Walk (supOwed,
                // DistinctUnlessOwed): two records name one address only
                // after a walk that left a refused forget. Any other walk
                // ends with every record at its own connected slot's address,
                // and those are distinct, so there is nothing to clear and
                // the scan of every slot is skipped: a mass disconnect reads
                // each slot once.

                if ( marks->supersede_owed )
                {
                    rocketnet_server_replay_marks_supersede( marks, i, &forgotten );
                }
            }
            else
            {
                refused_forgets++;
            }
        }

        // proofs/tla/RNServerReplaySlots.tla Walk (G, RecordSkip,
        // KernelEntryOwned): the address the mark pass marked while the
        // slot was skipped is forgotten once the slot has left it, kept on a
        // refusal like the record's own forget, so its kernel entry always
        // has an owner

        if ( slot->skip_marked && ( !current || !rocketnet_address_equal( current, &slot->skip_address ) ) )
        {
            const struct rocketnet_address_t forgotten = slot->skip_address;
            if ( sink->forget( sink->context, &forgotten ) )
            {
                rocketnet_server_replay_marks_clear_skip( slot );
                if ( marks->supersede_owed )
                {
                    rocketnet_server_replay_marks_supersede( marks, i, &forgotten );
                }
            }
            else
            {
                refused_forgets++;
            }
        }
    }

    // then the marks -- every connected slot whose highest_opened differs
    // from what it last marked (none counting as zero), and every slot first
    // seen, zero included. Every connected slot with a
    // mapping is seen at its address, marked or not, so that the forget pass
    // can drop the reply MAC of a slot that left before it ever marked.

    for ( int i = 0; i < marks->max_clients; i++ )
    {
        if ( !rocketnet_client_slots_connected( slots, i ) )
        {
            continue;
        }

        const int mapping_index = rocketnet_client_slots_mapping_index( slots, i );
        if ( mapping_index < 0 )
        {
            continue;
        }
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( map, mapping_index );
        const uint64_t highest_opened = mapping->replay_session.highest_opened;

        const struct rocketnet_address_t * address = rocketnet_encryption_map_address( map, mapping_index );

        struct rocketnet_server_replay_slot_mark_t * slot = &marks->slots[i];

        // A refused forget for an old mapping must be retried before the
        // current mapping replaces its seen or marked identity. The new
        // address is still marked, at zero, without taking the
        // record (proofs/tla/RNServerReplaySlots.tla MarkWhileSkipped,
        // GreetedSlotHasEntry): its session is greeted after this update's
        // commit, and a session packet from a source with no entry drops in
        // the kernel (DROPPED_NO_SESSION) while an older session's stale entry
        // drops it as replayed. A zero mark the sink refuses holds the
        // greeting as any other does (rocketnet_xdp_replay_sink_mark). The
        // record names the old address until its forget is queued, so the
        // mark is asked for again each walk until then; a queued one
        // supersedes another slot's record of the address as below, and is
        // recorded in the slot's skip mark, which the forget pass forgets
        // once the slot leaves the address (RecordSkip, KernelEntryOwned).
        // A slot whose skip mark still names an address it has left is not
        // marked at all: the skip mark keeps its owner (KeepSkipOwner). With
        // the XDP sink no skip mark is ever queued (the zero mark waits for
        // the full forget array), so this is a sink without that rule's case
        // (msgx), whose session then waits unmarked for its forgets.

        if ( slot->seen && !rocketnet_address_equal( address, &slot->seen_address ) )
        {
            if ( slot->skip_marked && !rocketnet_address_equal( address, &slot->skip_address ) )
            {
                continue;
            }

            // the skip follows this slot's own forget refused in this walk,
            // so a queued mark always supersedes. It is asked at zero: the
            // session is not yet greeted, and a zero mark is the one the XDP
            // sink refuses while its forget array is full, holding every
            // greeting, whatever the session opened before its greeting
            // (ZeroMarkWaitsForForgets); the slot's own mark at its
            // highest_opened follows once its record takes the address.
            if ( sink->mark( sink->context, address, 0 ) )
            {
                rocketnet_server_replay_marks_supersede( marks, i, address );
                slot->skip_marked = 1;
                slot->skip_address = *address;
            }
            continue;
        }

        // proofs/tla/RNXdpReplay.tla Marks, NewSessionNeverDroppedByOldMark: a slot
        // first seen, or whose highest_opened fell below its mark (a new
        // session in the slot), is marked even at zero, so the commit before
        // its first keep-alive leaves overwrites any older session's entry
        // for the address, whether or not that session's forget has landed.

        const bool first_seen = !slot->seen;
        const bool mark_owed = slot->mark_owed != 0;

        slot->seen = 1;
        slot->seen_address = *address;

        // a skip mark of this address is the record's now (SeenAt)
        if ( slot->skip_marked && rocketnet_address_equal( &slot->skip_address, address ) )
        {
            rocketnet_server_replay_marks_clear_skip( slot );
        }

        const uint32_t high_water = (uint32_t) highest_opened;
        const uint32_t last_marked = slot->has_mark ? slot->marked_value : 0;

        if ( !first_seen && !mark_owed && high_water == last_marked )
        {
            continue;
        }

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
        {
            rocketnet_server_replay_marks_supersede( marks, i, address );
        }
    }

    // proofs/tla/RNServerReplaySlots.tla Walk (supOwed'): a forget refused in
    // this walk may leave its record beside a newer one of the same address,
    // so the next walk's forget pass owes the scan

    marks->supersede_owed = refused_forgets > 0;
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
    // record's fields; the walk's seen record, owed flag and skip mark are
    // five more members, and their RESET decisions are listed here so that
    // every member of the struct has one and the inventory covers the whole
    // struct.
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_SERVER_REPLAY_MARK_FIELDS( MARK_DECISION )
                                                              MARK_DECISION( RESET, seen )
                                                                  MARK_DECISION( RESET, seen_address )
                                                                      MARK_DECISION( RESET, mark_owed )
                                                                          MARK_DECISION( RESET, skip_marked )
                                                                              MARK_DECISION( RESET, skip_address ) };
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
        {
            rocketnet_server_replay_marks_destroy( marks );
        }
        if ( slots )
        {
            rocketnet_client_slots_destroy( slots );
        }
        if ( map )
        {
            rocketnet_encryption_map_destroy( map );
        }
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
