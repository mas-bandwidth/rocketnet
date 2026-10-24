/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_fragment.h"
#include "rocketnet_constants.h"
#include "rocketnet_memory.h"
#include "rocketnet_read_write.h"
#include "rocketnet_platform.h"
#include "rocketnet_internal.h"

#include <string.h>

// The transport's reassembled payload maximum lies between one packet's
// payload and the wire's fragmented payload maximum.
ROCKETNET_STATIC_ASSERT( ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES &&
    ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES <= ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES,
    "the transport's payload maximum is within the fragment limits" );

void rocketnet_write_fragment_header( uint8_t * output, uint32_t payload_id, int fragment_index, int fragment_count )
{
    rocketnet_assert( output );
    rocketnet_assert( payload_id != 0 );
    rocketnet_assert( fragment_count >= 2 && fragment_count <= ROCKETNET_MAX_FRAGMENTS );
    rocketnet_assert( fragment_index >= 0 && fragment_index < fragment_count );

    uint8_t * p = output;
    rocketnet_write_uint32( &p, payload_id );
    rocketnet_write_uint8( &p, (uint8_t) fragment_index );
    rocketnet_write_uint8( &p, (uint8_t) ( fragment_count - 1 ) );
}

void rocketnet_read_fragment_header( const uint8_t * data, uint32_t * payload_id, int * fragment_index, int * fragment_count )
{
    rocketnet_assert( data );
    rocketnet_assert( payload_id );
    rocketnet_assert( fragment_index );
    rocketnet_assert( fragment_count );

    const uint8_t * p = data;
    *payload_id = rocketnet_read_uint32( &p );
    *fragment_index = rocketnet_read_uint8( &p );
    *fragment_count = rocketnet_read_uint8( &p ) + 1;
}

int rocketnet_fragment_plan( int payload_bytes, int * out_fragment_count, int * out_fragment_stride )
{
    rocketnet_assert( out_fragment_count );
    rocketnet_assert( out_fragment_stride );

    // fragmentation exists for payloads that do not fit one packet, and a
    // fragment count of one is invalid on the wire, so the count is at
    // least two -- a payload just over a single packet splits into two
    // near-halves. the remainder is never empty: stride = ceil(n/count)
    // makes the last fragment floor-sized, at least one byte for every
    // payload the fragment cap admits.

    if ( payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES || payload_bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
        return 0;

    int count = ( payload_bytes + ROCKETNET_MAX_FRAGMENT_BYTES - 1 ) / ROCKETNET_MAX_FRAGMENT_BYTES;
    if ( count < 2 )
        count = 2;

    int stride = ( payload_bytes + count - 1 ) / count;

    rocketnet_assert( count <= ROCKETNET_MAX_FRAGMENTS );
    rocketnet_assert( stride >= 1 && stride <= ROCKETNET_MAX_FRAGMENT_BYTES );
    rocketnet_assert( payload_bytes - stride * ( count - 1 ) >= 1 );
    rocketnet_assert( payload_bytes - stride * ( count - 1 ) <= stride );

    *out_fragment_count = count;
    *out_fragment_stride = stride;

    return 1;
}

// -----------------------------------------
//                              reassembly
// -----------------------------------------

// the delivered arena's bytes when every completion may be a maximum
// payload: capacity x the 16-aligned maximum (the packing stride bound)

static uint64_t rocketnet_reassembly_full_delivered_bytes(int max_payload_bytes, int delivered_capacity)
{
    if (max_payload_bytes < 1 || delivered_capacity < 1)
        return 0;
    return (uint64_t)delivered_capacity * (uint64_t)ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED(max_payload_bytes);
}

uint64_t rocketnet_reassembly_memory_required_delivered(int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity, uint64_t delivered_bytes)
{
    if (num_connections < 1 || num_slots < 1 || num_slots > ROCKETNET_MAX_FRAGMENTS ||
        max_payload_bytes < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES || max_payload_bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES || delivered_capacity < 1 ||
        delivered_bytes < (uint64_t)max_payload_bytes)
        return 0;
    uint64_t total = sizeof(rocketnet_reassembly_set_t);
    const uint64_t slots = (uint64_t)num_connections * (uint64_t)num_slots;
    if (!rocketnet_memory_add(total, slots, sizeof(rocketnet_reassembly_slot_t)) ||
        !rocketnet_memory_add(total, (uint64_t)num_connections, sizeof(uint32_t)) ||
        !rocketnet_memory_add(total, slots, (uint64_t)max_payload_bytes + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES) ||
        !rocketnet_memory_add(total, 1, delivered_bytes) || total > SIZE_MAX)
        return 0;
    return total;
}

uint64_t rocketnet_reassembly_memory_required(int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity)
{
    return rocketnet_reassembly_memory_required_delivered(num_connections, num_slots, max_payload_bytes, delivered_capacity,
        rocketnet_reassembly_full_delivered_bytes(max_payload_bytes, delivered_capacity));
}

bool rocketnet_reassembly_memory_allowed(int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity)
{
    return rocketnet_reassembly_memory_allowed_delivered(num_connections, num_slots, max_payload_bytes, delivered_capacity,
        rocketnet_reassembly_full_delivered_bytes(max_payload_bytes, delivered_capacity));
}

bool rocketnet_reassembly_memory_allowed_delivered(int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity, uint64_t delivered_bytes)
{
    const uint64_t bytes = rocketnet_reassembly_memory_required_delivered(num_connections, num_slots, max_payload_bytes, delivered_capacity, delivered_bytes);
    const uint64_t budget = (uint64_t)ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024;
    if (!bytes || bytes > budget)
    {
        rocketnet_error("reassembly geometry refused: requires %llu bytes, component budget %llu bytes",
            (unsigned long long)bytes, (unsigned long long)budget);
        return false;
    }
    return true;
}

// the completion bound (rocketnet_fragment.h; the delivered bound of
// tla/FragmentsAndAcks.tla, BatchFullNeverFires, finding D2): of the N
// packets one update drains, a payload begun earlier completes on one and
// a payload begun in the update on two, and at most connections x slots
// payloads are begun earlier

int rocketnet_server_reassembly_delivered_capacity( int max_clients, int num_slots )
{
    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS || num_slots < 1 || num_slots > ROCKETNET_MAX_FRAGMENTS )
        return 0;
    const int64_t drain = ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS;
    const int64_t begun_earlier = (int64_t) max_clients * (int64_t) num_slots;
    return (int) ( ( drain + ( begun_earlier < drain ? begun_earlier : drain ) ) / 2 );
}

// the completion bound (rocketnet_fragment.h; the delivered bound of
// tla/FragmentsAndAcks.tla, BatchFullNeverFires, finding D2), at the
// client's one connection and drain: of the N packets one update drains, a
// payload begun earlier completes on one and a payload begun in the update
// on two, and at most num_slots payloads are begun earlier

int rocketnet_client_reassembly_delivered_capacity( int num_slots )
{
    if ( num_slots < 1 || num_slots > ROCKETNET_MAX_FRAGMENTS )
        return 0;
    const int64_t drain = ROCKETNET_NUM_CLIENT_PACKETS;
    const int64_t begun_earlier = num_slots;
    return (int) ( ( drain + ( begun_earlier < drain ? begun_earlier : drain ) ) / 2 );
}

// the byte bound beside the count bound (rocketnet_fragment.h; finding D2
// of tla/README-FragmentsAndAcks.md): a payload completed in an update is
// made of fragments drained in that update -- one fragment per packet, at
// most ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES each -- plus the bytes its slot
// already held from earlier updates, at most one payload per slot and no
// more than the completions. Each completion pads to the 16-byte packing
// stride. The bound is the smaller of that and capacity x the aligned
// maximum payload, and both are true upper bounds.

uint64_t rocketnet_server_reassembly_delivered_bytes( int max_clients, int num_slots, int max_payload_bytes )
{
    const int capacity = rocketnet_server_reassembly_delivered_capacity( max_clients, num_slots );
    if ( capacity < 1 || max_payload_bytes < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES || max_payload_bytes > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
        return 0;
    const uint64_t by_count = rocketnet_reassembly_full_delivered_bytes( max_payload_bytes, capacity );
    const uint64_t begun_earlier = (uint64_t) max_clients * (uint64_t) num_slots;
    const uint64_t held = ( begun_earlier < (uint64_t) capacity ? begun_earlier : (uint64_t) capacity ) * (uint64_t) max_payload_bytes;
    const uint64_t by_drain = (uint64_t) ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS * (uint64_t) ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES +
                              held + (uint64_t) capacity * ( ROCKETNET_REASSEMBLY_DELIVERED_ALIGN - 1 );
    return by_drain < by_count ? by_drain : by_count;
}

struct rocketnet_reassembly_set_t * rocketnet_reassembly_set_create( void * context, int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity )
{
    return rocketnet_reassembly_set_create_delivered( context, num_connections, num_slots, max_payload_bytes, delivered_capacity,
                                                      rocketnet_reassembly_full_delivered_bytes( max_payload_bytes, delivered_capacity ) );
}

struct rocketnet_reassembly_set_t * rocketnet_reassembly_set_create_delivered( void * context, int num_connections, int num_slots, int max_payload_bytes, int delivered_capacity, uint64_t delivered_bytes )
{
    rocketnet_assert( num_connections > 0 );
    rocketnet_assert( num_slots > 0 && num_slots <= ROCKETNET_MAX_FRAGMENTS );
    rocketnet_assert( max_payload_bytes >= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rocketnet_assert( delivered_capacity > 0 );

    if (!rocketnet_reassembly_memory_allowed_delivered(num_connections, num_slots, max_payload_bytes, delivered_capacity, delivered_bytes))
        return NULL;

    struct rocketnet_reassembly_set_t * set = (struct rocketnet_reassembly_set_t*) rocketnet_malloc( context, sizeof(struct rocketnet_reassembly_set_t) );
    if ( !set )
        return NULL;

    memset( set, 0, sizeof(struct rocketnet_reassembly_set_t) );

    set->context = context;
    set->num_connections = num_connections;
    set->num_slots = num_slots;
    set->max_payload_bytes = max_payload_bytes;
    set->slot_arena_bytes = (size_t) max_payload_bytes + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;
    set->delivered_capacity = delivered_capacity;
    set->delivered_bytes = (size_t) delivered_bytes;

    // the per-connection stale-rule references ride at the tail of the slot
    // records' allocation, so the set keeps its allocation count

    const size_t slot_records_bytes = (size_t) num_connections * (size_t) num_slots * sizeof(struct rocketnet_reassembly_slot_t);
    set->slots = (struct rocketnet_reassembly_slot_t*) rocketnet_malloc( context, slot_records_bytes + (size_t) num_connections * sizeof(uint32_t) );
    set->arena = (uint8_t*) rocketnet_malloc( context, (size_t) num_connections * (size_t) num_slots * set->slot_arena_bytes );

    // The delivered mirror is this implementation's own addition beyond
    // the standard's arena arithmetic: the per-update home of completed
    // payloads, which is what lets a reassembly slot clear and reuse
    // immediately while the delivered bytes stay valid. The capacity is
    // the caller's completion bound per update (rocketnet_fragment.h); at
    // that bound BATCH_FULL is a true must-stay-zero invariant rather than
    // an operating condition. Completions pack by bytes at a 16-byte
    // stride, so the mirror is sized by the caller's byte bound, not
    // capacity x max payload. The reassembly budget is approximately
    // connections x slots x (max payload + staging) for the arena plus
    // delivered_bytes for this mirror; slot records ride along.

    set->delivered = (uint8_t*) rocketnet_malloc( context, set->delivered_bytes );

    if ( !set->slots || !set->arena || !set->delivered )
    {
        rocketnet_reassembly_set_destroy( set );
        return NULL;
    }

    memset( set->slots, 0, slot_records_bytes );
    set->newest_completed = (uint32_t*) ( (uint8_t*) set->slots + slot_records_bytes );
    memset( set->newest_completed, 0, (size_t) num_connections * sizeof(uint32_t) );

    return set;
}

void rocketnet_reassembly_set_destroy( struct rocketnet_reassembly_set_t * set )
{
    rocketnet_assert( set );
    if ( set->slots )
        rocketnet_free( set->context, set->slots );
    if ( set->arena )
        rocketnet_free( set->context, set->arena );
    if ( set->delivered )
        rocketnet_free( set->context, set->delivered );
    rocketnet_free( set->context, set );
}

void rocketnet_reassembly_set_reset_connection( struct rocketnet_reassembly_set_t * set, int connection_index )
{
    rocketnet_assert( set );
    rocketnet_assert( connection_index >= 0 && connection_index < set->num_connections );
    memset( set->slots + connection_index * set->num_slots, 0, (size_t) set->num_slots * sizeof(struct rocketnet_reassembly_slot_t) );
    set->newest_completed[connection_index] = 0;
}

// tla/FragmentsAndAcks.tla Tick: starts the receiver's update, the delivered count and arena back at zero
void rocketnet_reassembly_set_begin_update( struct rocketnet_reassembly_set_t * set )
{
    rocketnet_assert( set );
    set->num_delivered = 0;
    set->delivered_used = 0;
    set->completed_payload = NULL;
    set->completed_bytes = 0;
}

static void rocketnet_reassembly_clear_slot( struct rocketnet_reassembly_slot_t * slot )
{
    memset( slot, 0, sizeof(struct rocketnet_reassembly_slot_t) );
}

// a fragment bit, and setting it: num_received tracks the set bits

static int rocketnet_reassembly_bit( const struct rocketnet_reassembly_slot_t * slot, int index )
{
    return ( slot->received[index / 8] >> ( index % 8 ) ) & 1;
}

static void rocketnet_reassembly_set_bit( struct rocketnet_reassembly_slot_t * slot, int index )
{
    slot->received[index / 8] |= (uint8_t) ( 1 << ( index % 8 ) );
    slot->num_received++;
}

// tla/FragmentsAndAcks.tla, RecvData: the checks in the standard's order, then
// placement and delivery inside this call (DeliveredExact, DeliveredOnce,
// CompleteDelivers, ArenaBounded, CountPinned). The stale test is the rule
// StaleOnlyIfOlder asks for: an honest fragment newer than its slot's
// occupant is never refused (finding D1 in tla/README-FragmentsAndAcks.md)

int rocketnet_reassembly_process_fragment( struct rocketnet_reassembly_set_t * set,
                                           int connection_index,
                                           const uint8_t * type_data,
                                           int type_data_bytes,
                                           uint64_t * counters,
                                           const struct rocketnet_reassembly_counters_t * table )
{
    rocketnet_assert( set );
    rocketnet_assert( connection_index >= 0 && connection_index < set->num_connections );
    rocketnet_assert( type_data );
    rocketnet_assert( counters );
    rocketnet_assert( table );

    set->completed_payload = NULL;
    set->completed_bytes = 0;

    if ( type_data_bytes < ROCKETNET_FRAGMENT_HEADER_BYTES + 1 ||
         type_data_bytes > ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES )
    {
        rocketnet_spam( "reassembly refused fragment. invalid fragment size" );
        counters[table->size_mismatch]++;
        return 0;
    }

    uint32_t payload_id;
    int fragment_index;
    int fragment_count;
    rocketnet_read_fragment_header( type_data, &payload_id, &fragment_index, &fragment_count );

    const uint8_t * fragment_data = type_data + ROCKETNET_FRAGMENT_HEADER_BYTES;
    const int fragment_bytes = type_data_bytes - ROCKETNET_FRAGMENT_HEADER_BYTES;

    // the standard's checks, in order, each with its own counter. every
    // refusal is decided before anything mutates, so a refused fragment --
    // including a newer payload id whose first fragment is invalid --
    // leaves the slot exactly as it was.

    // a payload id of zero is what an uninitialized sender produces

    if ( payload_id == 0 )
    {
        rocketnet_spam( "reassembly refused fragment. zero payload id" );
        counters[table->zero_payload_id]++;
        return 0;
    }

    struct rocketnet_reassembly_slot_t * slot = &set->slots[connection_index * set->num_slots + (int) ( payload_id % (uint32_t) set->num_slots )];
    uint8_t * slot_payload = set->arena + ( (size_t) connection_index * (size_t) set->num_slots + (size_t) ( payload_id % (uint32_t) set->num_slots ) ) * set->slot_arena_bytes;
    uint8_t * slot_staging = slot_payload + set->max_payload_bytes;

    // the stale test, STANDARD.md _Fragmentation_, against two references.
    // Ids are compared as unsigned differences, so the rollover past
    // 0xFFFFFFFF is ordinary arithmetic.
    //
    // First the connection's newest completed payload id (0, the id before
    // the sender's first, until one completes): an honest id is within
    // ROCKETNET_REASSEMBLY_REACH of it -- ahead by the payloads lost whole
    // since, or behind as a late fragment -- and an id further away in both
    // directions is stale. That is the hostile id: one fragment with an id
    // near half the space ahead, which a plain serial comparison would read
    // as newer than every honest id, refuses here whether its slot is
    // occupied or empty, so it neither evicts nor holds a slot. Only a
    // completed payload moves the reference, never a lone fragment.
    //
    // Then the slot's occupant: an id 1..ROCKETNET_REASSEMBLY_BEHIND (256)
    // behind it was superseded -- accepting its bytes into the newer payload
    // is the aliasing this check exists to prevent. Any other id displaces
    // the occupant, counted abandoned. A newer id does so however many of
    // the slot's ids were lost whole in between; an id further behind the
    // occupant shows the occupant is no live predecessor -- a delayed
    // fragment that re-occupied a slot a completion emptied, or a bad id far
    // ahead, holds the slot only until the next live id of its series.
    //
    // The bound on one bad fragment with id B that no honest payload yet
    // carries, inside REACH: the ids of its slot's series that trail B by
    // 1..BEHIND refuse stale until the series reaches B, at most
    // BEHIND / num_slots payloads (32 at eight slots), the displaced occupant
    // among them when it is that close; payload B itself shares its slot
    // state. Further from the live series, B costs the one occupant it
    // displaces. The stale test before this rule had the same worst case.

    const uint32_t reference = set->newest_completed[connection_index];

    if ( payload_id - reference > ROCKETNET_REASSEMBLY_REACH && reference - payload_id > ROCKETNET_REASSEMBLY_REACH )
    {
        rocketnet_spam( "reassembly refused fragment. payload id far from the newest completed" );
        counters[table->stale]++;
        return 0;
    }

    if ( slot->payload_id != 0 && payload_id != slot->payload_id &&
         slot->payload_id - payload_id <= ROCKETNET_REASSEMBLY_BEHIND )
    {
        rocketnet_spam( "reassembly refused fragment. stale payload id" );
        counters[table->stale]++;
        return 0;
    }

    // a count of one is invalid: a payload that fits one packet is type 5.
    // the wire byte encodes count - 1, so counts above 256 cannot exist.

    if ( fragment_count == 1 )
    {
        rocketnet_spam( "reassembly refused fragment. invalid fragment count" );
        counters[table->count_invalid]++;
        return 0;
    }

    if ( fragment_index >= fragment_count )
    {
        rocketnet_spam( "reassembly refused fragment. fragment index out of range" );
        counters[table->index_out_of_range]++;
        return 0;
    }

    const int same_payload = slot->payload_id == payload_id;
    const int is_final = fragment_index == fragment_count - 1;

    // the first fragment of a payload id fixes its count; a later fragment
    // naming a different count refuses and keeps the slot as it was -- the
    // widened count after the fact is the classic memory-trashing attack
    // on reassembly, and the pin is the answer

    if ( same_payload && fragment_count != slot->fragment_count )
    {
        rocketnet_spam( "reassembly refused fragment. fragment count mismatch" );
        counters[table->count_mismatch]++;
        return 0;
    }

    // the size discipline. every fragment except the last is the same
    // size, fixed by the first non-final arrival; the final must not
    // exceed the stride once the stride is known. what this fragment would
    // pin is computed here, refused before anything mutates.

    const int stride = ( same_payload && slot->fragment_stride != 0 ) ? slot->fragment_stride
                     : ( !is_final ? fragment_bytes : 0 );                          // 0 = still unknown: a final arriving first

    if ( same_payload && slot->fragment_stride != 0 )
    {
        if ( !is_final && fragment_bytes != slot->fragment_stride )
        {
            rocketnet_spam( "reassembly refused fragment. fragment size mismatch" );
            counters[table->size_mismatch]++;
            return 0;
        }
        if ( is_final && fragment_bytes > slot->fragment_stride )
        {
            rocketnet_spam( "reassembly refused fragment. final fragment exceeds the stride" );
            counters[table->size_mismatch]++;
            return 0;
        }
    }

    // a staged final is checked against the stride at the moment the first
    // non-final fragment fixes it: exceeding it refuses the whole payload

    if ( same_payload && slot->fragment_stride == 0 && !is_final && slot->final_staged && slot->final_bytes > stride )
    {
        rocketnet_spam( "reassembly refused payload. staged final fragment exceeds the stride" );
        counters[table->size_mismatch]++;
        rocketnet_reassembly_clear_slot( slot );
        return 0;
    }

    // the reconstructed payload -- stride x (count - 1) plus the final
    // fragment -- must not exceed the configured maximum. the check runs
    // the moment both the stride and the count are known, never deferred
    // to completion, so an over-large payload is refused before it
    // occupies a slot's arena rather than after filling it. when the
    // final's size is not yet known it contributes its one-byte minimum;
    // the final's own arrival rechecks with the real size. a payload that
    // can never fit clears its slot -- waiting for eviction would only
    // convert this refusal into an abandonment later.

    if ( stride != 0 )
    {
        int64_t final_contribution = 1;
        if ( is_final )
            final_contribution = fragment_bytes;
        else if ( same_payload && slot->final_bytes >= 0 )
            final_contribution = slot->final_bytes;

        if ( (int64_t) stride * ( fragment_count - 1 ) + final_contribution > (int64_t) set->max_payload_bytes )
        {
            rocketnet_spam( "reassembly refused payload. reconstructed payload too large" );
            counters[table->payload_too_large]++;
            if ( same_payload )
                rocketnet_reassembly_clear_slot( slot );
            return 0;
        }
    }

    // a fragment index already held refuses as a duplicate. the sealed
    // packet's replay protection already refuses replayed packets, so this
    // fires only on a sender defect.

    if ( same_payload && rocketnet_reassembly_bit( slot, fragment_index ) )
    {
        rocketnet_spam( "reassembly refused fragment. duplicate fragment index" );
        counters[table->duplicate_index]++;
        return 0;
    }

    // every check passed: the fragment is accepted, and only now does
    // anything mutate. a newer payload id claiming an occupied slot evicts
    // the incomplete payload -- the loss-visibility counter, and the only
    // way a fragmented payload dies to loss.

    if ( !same_payload )
    {
        if ( slot->payload_id != 0 )
        {
            rocketnet_spam( "reassembly abandoned incomplete payload %u", slot->payload_id );
            counters[table->abandoned]++;
        }
        rocketnet_reassembly_clear_slot( slot );
        slot->payload_id = payload_id;
        slot->fragment_count = fragment_count;
        slot->final_bytes = -1;
    }

    if ( !is_final )
    {
        // the first non-final arrival fixes the stride, and places the
        // staged final if one waits -- the stride passed every check above

        if ( slot->fragment_stride == 0 )
        {
            slot->fragment_stride = stride;
            if ( slot->final_staged )
            {
                memcpy( slot_payload + (size_t) stride * (size_t) ( slot->fragment_count - 1 ), slot_staging, (size_t) slot->final_bytes );
                slot->final_staged = 0;
            }
        }

        memcpy( slot_payload + (size_t) slot->fragment_stride * (size_t) fragment_index, fragment_data, (size_t) fragment_bytes );
    }
    else
    {
        slot->final_bytes = fragment_bytes;

        if ( slot->fragment_stride != 0 )
        {
            memcpy( slot_payload + (size_t) slot->fragment_stride * (size_t) ( slot->fragment_count - 1 ), fragment_data, (size_t) fragment_bytes );
        }
        else
        {
            // only the final fragment can arrive before the stride is
            // known; it waits in this slot's staging buffer and is placed
            // when the first non-final fragment fixes the stride

            memcpy( slot_staging, fragment_data, (size_t) fragment_bytes );
            slot->final_staged = 1;
        }
    }

    rocketnet_reassembly_set_bit( slot, fragment_index );

    counters[table->accepted]++;

    if ( slot->num_received == slot->fragment_count )
    {
        // complete: fragment 0 is non-final, so the stride is known and
        // every fragment sits placed and contiguous. the payload is copied
        // out once -- the one copy fragmentation inherently costs -- so
        // the slot clears now and the delivered bytes stay valid until the
        // next update.

        rocketnet_assert( slot->fragment_stride != 0 );
        rocketnet_assert( !slot->final_staged );
        rocketnet_assert( slot->final_bytes >= 1 );

        const int payload_bytes = slot->fragment_stride * ( slot->fragment_count - 1 ) + slot->final_bytes;
        rocketnet_assert( payload_bytes <= set->max_payload_bytes );

        // the delivered arena packs by bytes at a 16-byte stride: the count
        // bound and the byte bound (rocketnet_fragment.h) both hold here

        const size_t delivered_offset = ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( set->delivered_used );

        if ( set->num_delivered < set->delivered_capacity && delivered_offset + (size_t) payload_bytes <= set->delivered_bytes )
        {
            uint8_t * delivered = set->delivered + delivered_offset;
            memcpy( delivered, slot_payload, (size_t) payload_bytes );
            set->num_delivered++;
            set->delivered_used = delivered_offset + (size_t) payload_bytes;
            set->completed_payload = delivered;
            set->completed_bytes = payload_bytes;
            rocketnet_spam( "reassembled payload %u (%d bytes)", slot->payload_id, payload_bytes );
        }
        else
        {
            rocketnet_spam( "reassembled payload %u dropped. delivery batch full", slot->payload_id );
            counters[table->batch_full]++;
        }

        counters[table->reassembled]++;

        // the stale rule's reference advances to the newest completed id,
        // serially: a completion older than it leaves it where it is

        const uint32_t advance = slot->payload_id - set->newest_completed[connection_index];
        if ( advance != 0 && advance < 0x80000000u )
            set->newest_completed[connection_index] = slot->payload_id;

        rocketnet_reassembly_clear_slot( slot );
    }

    return 1;
}

// -----------------------------------------
//                          the send arena
// -----------------------------------------

uint64_t rocketnet_send_arena_allocation_bytes(int slots, int bytes)
{
    if (slots < 1 || bytes < 1) return 0;
    return rocketnet_memory_allocations({sizeof(rocketnet_send_arena_t), (uint64_t)slots * (uint64_t)bytes});
}

struct rocketnet_send_arena_t * rocketnet_send_arena_create( void * context, int num_slots, int slot_bytes )
{
    rocketnet_assert( num_slots > 0 );
    rocketnet_assert( slot_bytes > 0 );

    struct rocketnet_send_arena_t * arena = (struct rocketnet_send_arena_t*) rocketnet_malloc( context, sizeof(struct rocketnet_send_arena_t) );
    if ( !arena )
        return NULL;

    memset( arena, 0, sizeof(struct rocketnet_send_arena_t) );

    arena->context = context;
    arena->num_slots = num_slots;
    arena->slot_bytes = slot_bytes;
    arena->data = (uint8_t*) rocketnet_malloc( context, (size_t) num_slots * (size_t) slot_bytes );
    if ( !arena->data )
    {
        rocketnet_free( context, arena );
        return NULL;
    }

    return arena;
}

void rocketnet_send_arena_destroy( struct rocketnet_send_arena_t * arena )
{
    rocketnet_assert( arena );
    if ( arena->data )
        rocketnet_free( arena->context, arena->data );
    rocketnet_free( arena->context, arena );
}

uint8_t * rocketnet_send_arena_acquire( struct rocketnet_send_arena_t * arena )
{
    rocketnet_assert( arena );

    uint64_t slot = rocketnet_platform_atomic_add_u64( &arena->num_used, 1 );
    if ( slot >= (uint64_t) arena->num_slots )
        return NULL;

    return arena->data + slot * (size_t) arena->slot_bytes;
}

int rocketnet_send_arena_owns( const struct rocketnet_send_arena_t * arena, const uint8_t * p )
{
    rocketnet_assert( arena );
    return p >= arena->data && p < arena->data + (size_t) arena->num_slots * (size_t) arena->slot_bytes;
}

void rocketnet_send_arena_reset( struct rocketnet_send_arena_t * arena )
{
    rocketnet_assert( arena );
    arena->num_used = 0;
}
