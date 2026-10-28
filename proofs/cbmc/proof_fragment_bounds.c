/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of rocketnet_reassembly_process_fragment.

    The leaf below is sdk/rocketnet_fragment.cpp (the header reader, the
    slot bit helpers, and process_fragment), compiled against
    sdk/rocketnet_fragment.h. rocketnet_assert is the release form: the
    runtime refusals are what hold, not the debug asserts. rocketnet_spam
    is ignored.

    One connection, one slot. max_payload_bytes is symbolic from 1 to 16.
    A fragment is symbolic and either outside the legal size window, or
    at most 8 bytes. The slot's stored stride and final size are 0 or at
    most 8, so every copy the accept path performs fits the arena. The
    claims: an illegal size or a zero payload id leaves the slot and the
    arena untouched; a reconstructed total above the payload maximum is
    refused, does not evict a different payload, and does not write the
    arena; an accepted fragment whose stride is known fits the maximum;
    an accepted final that leaves the stride at 0 and stays open is
    staged, and does not write the payload region. Completion clears
    the slot; a completed payload is between 1 and the maximum bytes.

    The stale check subtracts payload ids as uint32. This instance
    proves that subtraction only when the slot is empty or the wire id
    is at or above the occupant. The rollover half is not proved.
    Counts whose fragment payload is above 8 bytes, and payload maxima
    above 16, are not proved.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 64. The arena compare is 24 bytes and a cleared slot is
    56 bytes.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"

#define rocketnet_assert( c ) ( (void) 0 )
#define rocketnet_spam( ... ) ( (void) 0 )

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
#else
int nondet_int( void );
#endif // #ifndef __CPROVER__

#define PROOF_FRAG 8
#define PROOF_MAX  16
#define ARENA      ( PROOF_MAX + PROOF_FRAG )
#define TYPE_BUF   ( ROCKETNET_FRAGMENT_HEADER_BYTES + PROOF_FRAG )

/* sdk/rocketnet_fragment.cpp lines 27-38 */
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

/* sdk/rocketnet_fragment.cpp lines 181-197 */
static void rocketnet_reassembly_clear_slot( struct rocketnet_reassembly_slot_t * slot )
{
    memset( slot, 0, sizeof( struct rocketnet_reassembly_slot_t ) );
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

/* sdk/rocketnet_fragment.cpp lines 199-469 */
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
    uint8_t * slot_payload = set->arena + ( (size_t) connection_index * set->num_slots + (size_t) ( payload_id % (uint32_t) set->num_slots ) ) * set->slot_arena_bytes;
    uint8_t * slot_staging = slot_payload + set->max_payload_bytes;

    // a payload id older than the slot's occupant was already superseded;
    // accepting its bytes into the newer payload is the aliasing this
    // check exists to prevent. A slot holds only a short run of live ids:
    // the sender's id counter advances by one per fragmented payload, and
    // this slot recurs every num_slots ids, so the next honest id for the
    // slot is at most ROCKETNET_MAX_FRAGMENTS ahead of its occupant -- which
    // also covers the rollover past 0xFFFFFFFF. An id behind the occupant,
    // or far ahead of it, is stale. The far-ahead case is the one a plain
    // half-space serial comparison misses: an id just under 2^31 ahead
    // reads as "newer", so one hostile id evicts the occupant and every
    // honest id behind it then reads as old -- the slot poisoned for the
    // connection's life. Unsigned subtraction keeps the comparison defined.

    if ( slot->payload_id != 0 && payload_id != slot->payload_id &&
         payload_id - slot->payload_id > ROCKETNET_MAX_FRAGMENTS )
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
        {
            final_contribution = fragment_bytes;
        }
        else if ( same_payload && slot->final_bytes >= 0 )
        {
            final_contribution = slot->final_bytes;
        }

        if ( (int64_t) stride * ( fragment_count - 1 ) + final_contribution > (int64_t) set->max_payload_bytes )
        {
            rocketnet_spam( "reassembly refused payload. reconstructed payload too large" );
            counters[table->payload_too_large]++;

            if ( same_payload )
            {
                rocketnet_reassembly_clear_slot( slot );
            }

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
                memcpy( slot_payload + (size_t) stride * ( slot->fragment_count - 1 ), slot_staging, (size_t) slot->final_bytes );
                slot->final_staged = 0;
            }
        }

        memcpy( slot_payload + (size_t) slot->fragment_stride * fragment_index, fragment_data, (size_t) fragment_bytes );
    }
    else
    {
        slot->final_bytes = fragment_bytes;

        if ( slot->fragment_stride != 0 )
        {
            memcpy( slot_payload + (size_t) slot->fragment_stride * ( slot->fragment_count - 1 ), fragment_data, (size_t) fragment_bytes );
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

        if ( set->num_delivered < set->delivered_capacity )
        {
            uint8_t * delivered = set->delivered + (size_t) set->num_delivered * (size_t) set->max_payload_bytes;
            memcpy( delivered, slot_payload, (size_t) payload_bytes );
            set->num_delivered++;
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

        rocketnet_reassembly_clear_slot( slot );
    }

    return 1;
}
static int slot_unchanged( const struct rocketnet_reassembly_slot_t * slot,
                           const struct rocketnet_reassembly_slot_t * before )
{
    int same = slot->payload_id == before->payload_id && slot->fragment_count == before->fragment_count && slot->fragment_stride == before->fragment_stride && slot->final_bytes == before->final_bytes && slot->final_staged == before->final_staged && slot->num_received == before->num_received;

    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        if ( slot->received[i] != before->received[i] )
        {
            same = 0;
        }
    }

    return same;
}

/* Occupied slots are incomplete, num_received is the popcount, bits at
   or above fragment_count are clear, and a known stride's final bit
   matches final_bytes. Completion clears a slot, so a pre-state that
   already counts every fragment is not a state this function produces. */

static int proof_slot_bits_hold( const struct rocketnet_reassembly_slot_t * slot )
{
    if ( slot->payload_id == 0 )
        return 1;
    if ( slot->fragment_count < 2 || slot->num_received < 0 || slot->num_received >= slot->fragment_count )
        return 0;

    int bits = 0;

    for ( int i = 0; i < ROCKETNET_MAX_FRAGMENTS / 8; i++ )
    {
        const int base = i * 8;
        const uint8_t byte = slot->received[i];
        if ( base >= slot->fragment_count )
        {
            if ( byte != 0 )
                return 0;
        }
        else if ( base + 8 > slot->fragment_count )
        {
            const int keep = slot->fragment_count - base;
            if ( keep < 1 || keep > 7 )
                return 0;

            const unsigned mask = ( 1u << keep ) - 1u;
            if ( ( byte & (uint8_t) ~mask ) != 0 )
                return 0;
        }

        uint8_t rest = byte;

        for ( int k = 0; k < 8; k++ )
        {
            bits += rest & 1;
            rest = (uint8_t) ( rest >> 1 );
        }
    }

    if ( bits != slot->num_received )
        return 0;

    if ( slot->fragment_stride != 0 )
    {
        const int fin = slot->fragment_count - 1;
        const int bit = ( slot->received[fin / 8] >> ( fin % 8 ) ) & 1;

        if ( slot->final_bytes == -1 )
        {
            if ( bit || slot->final_staged )
                return 0;
        }
        else if ( !bit || slot->final_bytes > slot->fragment_stride )
            return 0;
    }

    return 1;
}

static int arena_unchanged( const uint8_t * arena, const uint8_t * before )
{
    int same = 1;

    for ( int i = 0; i < ARENA; i++ )
    {
        if ( arena[i] != before[i] )
        {
            same = 0;
        }
    }

    return same;
}

int main( void )
{
    struct rocketnet_reassembly_slot_t slot;
    __CPROVER_assume( slot.fragment_stride == 0 || ( slot.fragment_stride >= 1 && slot.fragment_stride <= PROOF_FRAG ) );
    __CPROVER_assume( slot.final_bytes == -1 || ( slot.final_bytes >= 1 && slot.final_bytes <= PROOF_FRAG ) );
    __CPROVER_assume( slot.final_staged == 0 || slot.final_staged == 1 );
    __CPROVER_assume( !slot.final_staged || ( slot.final_bytes >= 1 && slot.final_bytes <= PROOF_FRAG ) );
    __CPROVER_assume( slot.fragment_count >= 0 && slot.fragment_count <= ROCKETNET_MAX_FRAGMENTS );
    __CPROVER_assume( slot.num_received >= 0 && slot.num_received <= ROCKETNET_MAX_FRAGMENTS );
    /* An occupied slot whose stride is still 0 holds only its staged final,
       and that final's bit is set. One more fragment cannot complete while
       the stride is unknown: the final is a duplicate, and a non-final
       fixes the stride before the completion check. */
    __CPROVER_assume( slot.fragment_stride != 0 || slot.num_received <= 1 );
    __CPROVER_assume( slot.payload_id == 0 || slot.fragment_stride != 0 || slot.final_staged );
    __CPROVER_assume( !slot.final_staged || slot.num_received == 1 );
    {
        const int fin = slot.fragment_count - 1;
        __CPROVER_assume( !slot.final_staged || ( fin >= 0 && fin < ROCKETNET_MAX_FRAGMENTS && ( ( slot.received[fin / 8] >> ( fin % 8 ) ) & 1 ) ) );
    }
    __CPROVER_assume( proof_slot_bits_hold( &slot ) );

    uint8_t arena[ARENA];
    uint8_t delivered[PROOF_MAX];
    uint8_t type_data[TYPE_BUF];

    /* The stale check subtracts the ids as uint32. A payload id below the
       occupant wraps, which is defined and is how rollover stays ordered;
       --unsigned-overflow-check rejects that wrap. This instance keeps the
       difference representable: an empty slot, or a wire id at or above
       the occupant. The rollover half is not proved. */
    {
        const uint32_t wire_id = (uint32_t) type_data[0] | ( (uint32_t) type_data[1] << 8 ) | ( (uint32_t) type_data[2] << 16 ) | ( (uint32_t) type_data[3] << 24 );
        __CPROVER_assume( slot.payload_id == 0 || wire_id >= slot.payload_id );
    }

    int type_data_bytes = nondet_int();
    __CPROVER_assume( type_data_bytes < ROCKETNET_FRAGMENT_HEADER_BYTES + 1 || type_data_bytes > ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES || type_data_bytes <= ROCKETNET_FRAGMENT_HEADER_BYTES + PROOF_FRAG );

    int max_payload = nondet_int();
    __CPROVER_assume( max_payload >= 1 && max_payload <= PROOF_MAX );

    uint64_t counters[16];

    for ( int i = 0; i < 16; i++ )
    {
        counters[i] = 0;
    }

    struct rocketnet_reassembly_counters_t table;
    table.zero_payload_id = 0;
    table.stale = 1;
    table.count_invalid = 2;
    table.index_out_of_range = 3;
    table.count_mismatch = 4;
    table.size_mismatch = 5;
    table.payload_too_large = 6;
    table.duplicate_index = 7;
    table.accepted = 8;
    table.reassembled = 9;
    table.abandoned = 10;
    table.batch_full = 11;

    struct rocketnet_reassembly_set_t set;
    memset( &set, 0, sizeof( set ) );
    set.num_connections = 1;
    set.num_slots = 1;
    set.max_payload_bytes = max_payload;
    set.slot_arena_bytes = ARENA;
    set.slots = &slot;
    set.arena = arena;
    set.delivered_capacity = 1;
    set.num_delivered = 0;
    set.delivered = delivered;

    struct rocketnet_reassembly_slot_t before = slot;
    uint8_t arena_before[ARENA];

    for ( int i = 0; i < ARENA; i++ )
    {
        arena_before[i] = arena[i];
    }

    int rc = rocketnet_reassembly_process_fragment( &set, 0, type_data, type_data_bytes, counters, &table );

    const int in_size = type_data_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1 && type_data_bytes <= ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES;
    uint32_t payload_id = 0;
    int fragment_index = 0;
    int fragment_count = 1;
    int fragment_bytes = 0;
    int stride = 0;
    int exceeds = 0;
    int is_final = 0;
    if ( in_size )
    {
        const uint8_t * p = type_data;
        payload_id = rocketnet_read_uint32( &p );
        fragment_index = rocketnet_read_uint8( &p );
        fragment_count = rocketnet_read_uint8( &p ) + 1;
        fragment_bytes = type_data_bytes - ROCKETNET_FRAGMENT_HEADER_BYTES;

        is_final = fragment_index == fragment_count - 1;

        const int same = before.payload_id == payload_id;
        stride = ( same && before.fragment_stride != 0 ) ? before.fragment_stride
                                                         : ( !is_final ? fragment_bytes : 0 );

        if ( stride != 0 )
        {
            long long final_contribution = 1;

            if ( is_final )
            {
                final_contribution = fragment_bytes;
            }
            else if ( same && before.final_bytes >= 0 )
            {
                final_contribution = before.final_bytes;
            }

            const long long total = (long long) stride * ( fragment_count - 1 ) + final_contribution;
            exceeds = total > max_payload;
        }
    }

    if ( !in_size || payload_id == 0 )
    {
        __CPROVER_assert( rc == 0, "illegal size or zero payload id is refused" );
        __CPROVER_assert( slot_unchanged( &slot, &before ), "illegal size or zero payload id leaves the slot" );
        __CPROVER_assert( arena_unchanged( arena, arena_before ), "illegal size or zero payload id leaves the arena" );
    }

    if ( in_size && payload_id != 0 && exceeds && before.payload_id != payload_id )
    {
        __CPROVER_assert( rc == 0, "oversize fragment is refused" );
        __CPROVER_assert( slot.payload_id == before.payload_id, "oversize fragment does not evict the slot" );
        __CPROVER_assert( slot_unchanged( &slot, &before ), "oversize fragment leaves the slot" );
        __CPROVER_assert( arena_unchanged( arena, arena_before ), "oversize fragment does not allocate payload bytes" );
    }

    if ( rc == 1 && in_size && stride != 0 )
    {
        __CPROVER_assert( !exceeds, "accepted fragment does not exceed the payload maximum" );
    }

    /* Completion clears the slot, so a zero stride afterwards is the clear,
       not a staged final. The staging claims are the accepts that stay open. */
    const int open = rc == 1 && counters[table.reassembled] == 0;

    if ( open && in_size && is_final && stride == 0 )
    {
        __CPROVER_assert( slot.fragment_stride == 0, "stride 0 accept keeps the stride unknown" );
        __CPROVER_assert( slot.final_staged == 1, "stride 0 stages the final fragment" );

        for ( int i = 0; i < PROOF_MAX; i++ )
        {
            if ( i < max_payload )
            {
                __CPROVER_assert( arena[i] == arena_before[i], "stride 0 does not write the payload region" );
            }
        }
    }

    if ( open && in_size && !is_final )
    {
        __CPROVER_assert( slot.fragment_stride >= 1, "a non-final accept fixes a stride" );
        __CPROVER_assert( (long long) fragment_index * stride + fragment_bytes <= max_payload,
                          "placed fragment stays inside the payload maximum" );
    }

    if ( rc == 1 && counters[table.reassembled] == 1 )
    {
        __CPROVER_assert( slot.payload_id == 0, "completion clears the slot" );
        __CPROVER_assert( set.completed_bytes >= 1 && set.completed_bytes <= max_payload,
                          "completed payload fits the maximum" );
    }

    return 0;
}
