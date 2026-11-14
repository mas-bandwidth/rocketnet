/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_MSGX_SOCKET_TABLE_H
#define ROCKETNET_SDK_ROCKETNET_MSGX_SOCKET_TABLE_H

// The macOS batched server transport's per-client socket table
// (sdk/rocketnet_server_msgx_apple.cpp). macOS sends a datagram on a
// connected socket for about a sixth of what an unconnected socket costs per
// destination, so the transport keeps one socket per connected client, bound
// to the server's own port (SO_REUSEADDR|SO_REUSEPORT) and connected to the
// client. The kernel then delivers that client's datagrams to its socket and
// no longer to the shared one, so the receive worker reads every socket in
// the table (measured, docs and final record: an inbound datagram from a
// connected remote arrives only on its connected socket; a close returns it to
// the shared socket at once; a second socket connected to the same remote is
// refused EADDRINUSE).
//
// Pure: no platform call. The socket open and close are the caller's
// functions, so the unit tests and the trace check run the real table over a
// fake kernel. proofs/tla/RNMsgxSocketTable.tla models it at this layout.
//
// Threads:
// - main owns the address index, the slot states and every open and close.
//   It opens on the replay walk's mark (a connected client seen at an
//   address), retires on its forget (the client left or moved) and on a
//   worker's dead report, and closes a retired socket only after every
//   holder has been quiescent since the retire.
// - fill threads (strictly inside the fill phase, when main changes nothing)
//   look an address up and stamp the packet with the slot and generation.
// - holders (the transmit workers and the receive worker) read a slot's word
//   inside a pass. A pass is bracketed by holder_begin and holder_end; a
//   socket is closed only once every holder that was inside a pass at its
//   retire has ended that pass (quiescent-state reclamation), so a holder
//   never sends or reads on a descriptor that main closed and the kernel may
//   have handed to another client's new socket.
//
// The word of a slot is the generation in the high 32 bits and the
// descriptor plus one in the low 32 bits; zero low bits means no usable
// socket. A retire advances the generation, so a packet stamped before it
// resolves to no socket and leaves through the shared socket, never dropped.

#include <stdint.h>
#include <string.h>
#include <atomic>
#include <new>

#include "rocketnet_address.h"
#include "rocketnet_memory.h"

// the most slots a table may have: the transport's own limit on max_clients.
// A table is sized at init for the slots it is given (the server's configured
// max_clients), never for this limit (sdk/rocketnet_constants.h, "Per-server
// sizing")
#ifndef ROCKETNET_MSGX_TABLE_MAX_SLOTS
#define ROCKETNET_MSGX_TABLE_MAX_SLOTS                    1000
#endif // #ifndef ROCKETNET_MSGX_TABLE_MAX_SLOTS

// the holders: the transmit workers and the one receive worker
#define ROCKETNET_MSGX_TABLE_MAX_HOLDERS                    17

// the index is open addressing with linear probing over the smallest power of
// two at least twice the slots: a power of two so an entry is hash & mask, at
// most half full so a probe ends at an empty entry within a few steps. That
// is the index's only minimum: 2 entries at 1 slot, 16 at 8, 2048 at 1000
static inline int rocketnet_msgx_socket_table_index_entries( int capacity )
{
    int entries = 1;
    while ( entries < 2 * capacity )
    {
        entries <<= 1;
    }
    return entries;
}

#define ROCKETNET_MSGX_TABLE_INDEX_SIZE( capacity )      rocketnet_msgx_socket_table_index_entries( capacity )

static_assert( ROCKETNET_MSGX_TABLE_MAX_SLOTS <= 32767, "a slot fits the index's int16_t entries" );

// a socket a holder reported dead (ENOTCONN or EPIPE: an ICMP error
// disconnected it) is retired, and its address is not opened again for this
// many updates: a client whose path keeps refusing stays on the shared socket
// instead of a socket per update. About a second at 60 Hz. Chosen, not measured.
#define ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES          60

#define ROCKETNET_MSGX_SLOT_FREE                             0
#define ROCKETNET_MSGX_SLOT_OPEN                             1
#define ROCKETNET_MSGX_SLOT_RETIRED                          2

// what the table did, main only: plain fields the transport's stats and the
// tests read; none of them is a public counter row (the transport's public
// rows come from its send failures and receive drop totals)
struct rocketnet_msgx_table_counts_t
{
    uint64_t opened;                    // sockets opened and connected
    uint64_t closed;                    // sockets closed after their quarantine
    uint64_t open_failed;               // an open the kernel refused (descriptor exhaustion, EADDRINUSE, ...): the client stays on the shared socket
    uint64_t open_deferred;             // a mark past this update's open budget: the walk owes it and takes it again next update
    uint64_t table_full;                // a mark with every slot open or still in quarantine
    uint64_t retired_forget;            // retired on the walk's forget
    uint64_t retired_dead;              // retired on a holder's dead report
    uint64_t cooldown;                  // a mark refused inside a dead address's cooldown
};

struct rocketnet_msgx_socket_slot_t
{
    // read by holders and fill threads, written by main
    std::atomic<uint64_t> word;

    // set by a holder (the descriptor refused as disconnected), taken by
    // main: zero for none, else the generation it reports plus one, in one
    // word so a report is never read half written
    std::atomic<uint64_t> dead_report;

    // main only
    int state;
    int fd;
    uint32_t generation;
    struct rocketnet_address_t address;
    uint64_t passcopy[ROCKETNET_MSGX_TABLE_MAX_HOLDERS];   // each holder's pass at the retire
};

// the address of a dead socket and the update it may open again: a small
// direct-mapped table, main only; a later dead address that maps to a used
// entry takes it
#define ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES               64

// A table and its arrays are one block: the table, then slots, free_slots
// and retired of capacity entries each and the index of
// ROCKETNET_MSGX_TABLE_INDEX_SIZE( capacity ) entries, carved after it
// (rocketnet_msgx_socket_table_layout). The caller allocates
// rocketnet_msgx_socket_table_bytes( capacity ) and init wires the arrays.
struct rocketnet_msgx_socket_table_t
{
    int capacity;                       // slots, at most ROCKETNET_MSGX_TABLE_MAX_SLOTS
    int index_size;                     // ROCKETNET_MSGX_TABLE_INDEX_SIZE( capacity ), a power of two
    int num_holders;
    int opens_per_update;               // the open budget of one update
    int opens_left;                     // main: what is left of it this update
    uint64_t update;                    // main: the update number begin_update sets

    // the open and close the table calls: the platform's socket, bind and
    // connect, or a fake. open returns a descriptor >= 0, or a negative errno
    int ( *open )( void * user, const struct rocketnet_address_t * address );
    void ( *close )( void * user, int fd );
    void * user;

    std::atomic<uint64_t> pass[ROCKETNET_MSGX_TABLE_MAX_HOLDERS];  // even: quiescent; odd: inside a pass
    std::atomic<int> dead_reports;      // holders count their dead reports here; main's sweep reads it

    int * free_slots;                   // main: a stack of free slots (capacity)
    int num_free;
    int * retired;                      // main: the slots in quarantine, oldest first (capacity)
    int num_retired;
    int num_open;

    int16_t * index;                    // slot of an open address, -1 empty (index_size)

    struct rocketnet_address_t cooldown_address[ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES];
    uint64_t cooldown_until[ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES];

    struct rocketnet_msgx_table_counts_t counts;

    struct rocketnet_msgx_socket_slot_t * slots;     // capacity
};

// the carves of a table for capacity slots, offsets from the block's base
struct rocketnet_msgx_socket_table_layout_t
{
    uint64_t slots;
    uint64_t free_slots;
    uint64_t retired;
    uint64_t index;
    uint64_t bytes;
};

inline bool rocketnet_msgx_socket_table_layout( int capacity, struct rocketnet_msgx_socket_table_layout_t * layout )
{
    memset( layout, 0, sizeof( *layout ) );
    if ( capacity < 1 || capacity > ROCKETNET_MSGX_TABLE_MAX_SLOTS )
    {
        return false;
    }
    const uint64_t slots = (uint64_t) capacity;
    rocketnet_carve_t carve = { 0, false };
    rocketnet_carve( carve, 1, sizeof( struct rocketnet_msgx_socket_table_t ), alignof( struct rocketnet_msgx_socket_table_t ) );
    layout->slots = rocketnet_carve( carve, slots, sizeof( struct rocketnet_msgx_socket_slot_t ), alignof( struct rocketnet_msgx_socket_slot_t ) );
    layout->free_slots = rocketnet_carve( carve, slots, sizeof( int ), alignof( int ) );
    layout->retired = rocketnet_carve( carve, slots, sizeof( int ), alignof( int ) );
    layout->index = rocketnet_carve( carve, (uint64_t) ROCKETNET_MSGX_TABLE_INDEX_SIZE( capacity ), sizeof( int16_t ), alignof( int16_t ) );
    layout->bytes = carve.bytes;
    return !carve.failed;
}

// the bytes of a table for capacity slots (its block), 0 out of range
inline uint64_t rocketnet_msgx_socket_table_bytes( int capacity )
{
    struct rocketnet_msgx_socket_table_layout_t layout;
    return rocketnet_msgx_socket_table_layout( capacity, &layout ) ? layout.bytes : 0;
}

static inline uint64_t rocketnet_msgx_word( uint32_t generation, int fd )
{
    return ( (uint64_t) generation << 32 ) | (uint64_t) (uint32_t) ( fd + 1 );
}

static inline int rocketnet_msgx_word_fd( uint64_t word )
{
    return (int) (uint32_t) ( word & 0xFFFFFFFFULL ) - 1;
}

static inline uint32_t rocketnet_msgx_word_generation( uint64_t word )
{
    return (uint32_t) ( word >> 32 );
}

static inline uint32_t rocketnet_msgx_address_hash( const struct rocketnet_address_t * address )
{
    uint32_t hash = 2166136261u;
    const int words = address->type == ROCKETNET_ADDRESS_IPV6 ? 8 : 2;
    for ( int i = 0; i < words; i++ )
    {
        hash = ( hash ^ address->data.ipv6[i] ) * 16777619u;
    }
    hash = ( hash ^ address->port ) * 16777619u;
    return hash ^ ( hash >> 15 );
}

// the table starts with every slot free and every holder quiescent. block
// holds rocketnet_msgx_socket_table_bytes( capacity ) bytes, aligned as
// rocketnet_malloc aligns; init constructs the table at its start, wires its
// arrays after it and returns it
inline struct rocketnet_msgx_socket_table_t * rocketnet_msgx_socket_table_init( void * block, int capacity, int num_holders, int opens_per_update,
                                                                                int ( *open )( void *, const struct rocketnet_address_t * ),
                                                                                void ( *close )( void *, int ), void * user )
{
    rocketnet_assert( block );
    rocketnet_assert( capacity >= 1 && capacity <= ROCKETNET_MSGX_TABLE_MAX_SLOTS );
    rocketnet_assert( num_holders >= 1 && num_holders <= ROCKETNET_MSGX_TABLE_MAX_HOLDERS );
    rocketnet_assert( opens_per_update >= 1 );

    struct rocketnet_msgx_socket_table_layout_t layout;
    const bool laid_out = rocketnet_msgx_socket_table_layout( capacity, &layout );
    rocketnet_assert( laid_out );
    (void) laid_out;
    uint8_t * base = (uint8_t *) block;
    struct rocketnet_msgx_socket_table_t * table = new ( block ) rocketnet_msgx_socket_table_t();
    table->slots = (struct rocketnet_msgx_socket_slot_t *) ( base + layout.slots );
    table->free_slots = (int *) ( base + layout.free_slots );
    table->retired = (int *) ( base + layout.retired );
    table->index = (int16_t *) ( base + layout.index );
    table->index_size = ROCKETNET_MSGX_TABLE_INDEX_SIZE( capacity );

    table->capacity = capacity;
    table->num_holders = num_holders;
    table->opens_per_update = opens_per_update;
    table->opens_left = opens_per_update;
    table->update = 0;
    table->open = open;
    table->close = close;
    table->user = user;
    for ( int h = 0; h < ROCKETNET_MSGX_TABLE_MAX_HOLDERS; h++ )
    {
        table->pass[h].store( 0, std::memory_order_relaxed );
    }
    table->dead_reports.store( 0, std::memory_order_relaxed );

    // the stack pops slot 0 first
    table->num_free = capacity;
    for ( int i = 0; i < capacity; i++ )
    {
        table->free_slots[i] = capacity - 1 - i;
    }
    for ( int i = 0; i < capacity; i++ )
    {
        table->retired[i] = 0;
    }
    table->num_retired = 0;
    table->num_open = 0;
    for ( int i = 0; i < table->index_size; i++ )
    {
        table->index[i] = -1;
    }
    memset( table->cooldown_address, 0, sizeof( table->cooldown_address ) );
    memset( table->cooldown_until, 0, sizeof( table->cooldown_until ) );
    memset( &table->counts, 0, sizeof( table->counts ) );
    for ( int i = 0; i < capacity; i++ )
    {
        struct rocketnet_msgx_socket_slot_t * slot = new ( &table->slots[i] ) rocketnet_msgx_socket_slot_t();
        slot->word.store( 0, std::memory_order_relaxed );
        slot->dead_report.store( 0, std::memory_order_relaxed );
        slot->state = ROCKETNET_MSGX_SLOT_FREE;
        slot->fd = -1;
        slot->generation = 0;
        memset( &slot->address, 0, sizeof( slot->address ) );
        memset( slot->passcopy, 0, sizeof( slot->passcopy ) );
    }
    return table;
}

// ---------------------------------------------------------------------------
// the address index (main writes; fill threads read inside the fill phase)
// ---------------------------------------------------------------------------

// the slot open at address, or -1
inline int rocketnet_msgx_socket_table_lookup( const struct rocketnet_msgx_socket_table_t * table, const struct rocketnet_address_t * address )
{
    const uint32_t mask = (uint32_t) table->index_size - 1;
    uint32_t entry = rocketnet_msgx_address_hash( address ) & mask;
    for ( int probes = 0; probes < table->index_size; probes++ )
    {
        const int slot = table->index[entry];
        if ( slot < 0 )
        {
            return -1;
        }
        if ( rocketnet_address_equal( &table->slots[slot].address, address ) )
        {
            return slot;
        }
        entry = ( entry + 1 ) & mask;
    }
    return -1;
}

// proofs/tla/RNMsgxAddressIndex.tla Insert: linear probing from the
// address's home to the first empty entry.
inline void rocketnet_msgx_socket_table_index_insert( struct rocketnet_msgx_socket_table_t * table, int slot )
{
    const uint32_t mask = (uint32_t) table->index_size - 1;
    uint32_t entry = rocketnet_msgx_address_hash( &table->slots[slot].address ) & mask;
    while ( table->index[entry] >= 0 )
    {
        entry = ( entry + 1 ) & mask;
    }
    table->index[entry] = (int16_t) slot;
}

// proofs/tla/RNMsgxAddressIndex.tla Remove: linear probing's
// backward-shift delete: no tombstones, so a lookup always
// ends at the first empty entry. An entry after the hole moves into it when
// its home is not cyclically inside (hole, entry].
inline void rocketnet_msgx_socket_table_index_remove( struct rocketnet_msgx_socket_table_t * table, int slot )
{
    const uint32_t mask = (uint32_t) table->index_size - 1;
    uint32_t hole = rocketnet_msgx_address_hash( &table->slots[slot].address ) & mask;
    while ( table->index[hole] != slot )
    {
        rocketnet_assert( table->index[hole] >= 0 );
        hole = ( hole + 1 ) & mask;
    }
    uint32_t entry = hole;
    for ( ;; )
    {
        entry = ( entry + 1 ) & mask;
        const int moving = table->index[entry];
        if ( moving < 0 )
        {
            break;
        }
        const uint32_t home = rocketnet_msgx_address_hash( &table->slots[moving].address ) & mask;
        const bool home_in_range = hole <= entry ? ( home > hole && home <= entry ) : ( home > hole || home <= entry );
        if ( !home_in_range )
        {
            table->index[hole] = (int16_t) moving;
            hole = entry;
        }
    }
    table->index[hole] = -1;
}

// ---------------------------------------------------------------------------
// fill threads and holders
// ---------------------------------------------------------------------------

// proofs/tla/RNMsgxSocketTable.tla Stamp: a fill thread's stamp for a
// packet to address: the slot and its current
// generation, or slot -1 for the shared socket. Fill phase only.
inline int rocketnet_msgx_socket_table_stamp( const struct rocketnet_msgx_socket_table_t * table, const struct rocketnet_address_t * address, uint32_t * generation )
{
    const int slot = rocketnet_msgx_socket_table_lookup( table, address );
    if ( slot < 0 )
    {
        *generation = 0;
        return -1;
    }
    *generation = rocketnet_msgx_word_generation( table->slots[slot].word.load( std::memory_order_relaxed ) );
    return slot;
}

// proofs/tla/RNMsgxSocketTable.tla HolderBegin and HolderEnd: a pass is odd. seq_cst, so a
// retire's word store and its passcopy of this counter are ordered against
// the pass's own word loads.
inline void rocketnet_msgx_socket_table_holder_begin( struct rocketnet_msgx_socket_table_t * table, int holder )
{
    rocketnet_assert( holder >= 0 && holder < table->num_holders );
    rocketnet_assert( ( table->pass[holder].load( std::memory_order_relaxed ) & 1 ) == 0 );
    table->pass[holder].fetch_add( 1, std::memory_order_seq_cst );
}

inline void rocketnet_msgx_socket_table_holder_end( struct rocketnet_msgx_socket_table_t * table, int holder )
{
    rocketnet_assert( holder >= 0 && holder < table->num_holders );
    rocketnet_assert( ( table->pass[holder].load( std::memory_order_relaxed ) & 1 ) == 1 );
    table->pass[holder].fetch_add( 1, std::memory_order_seq_cst );
}

// proofs/tla/RNMsgxSocketTable.tla TxResolve and RxResolve: inside a pass, the descriptor of a stamped
// packet's socket, or -1 when the slot was retired since the stamp (the
// packet then leaves through the shared socket)
inline int rocketnet_msgx_socket_table_resolve( const struct rocketnet_msgx_socket_table_t * table, int slot, uint32_t generation )
{
    if ( slot < 0 || slot >= table->capacity )
    {
        return -1;
    }
    const uint64_t word = table->slots[slot].word.load( std::memory_order_seq_cst );
    if ( rocketnet_msgx_word_generation( word ) != generation )
    {
        return -1;
    }
    return rocketnet_msgx_word_fd( word );
}

// proofs/tla/RNMsgxSocketTable.tla ReportDead: inside a pass, a holder whose send or receive
// on the slot's socket answered disconnected asks main to retire it. Only
// the generation it resolved is retired: a report about an older socket of
// the slot is ignored.
inline void rocketnet_msgx_socket_table_report_dead( struct rocketnet_msgx_socket_table_t * table, int slot, uint32_t generation )
{
    if ( slot < 0 || slot >= table->capacity )
    {
        return;
    }
    struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
    uint64_t expected = 0;
    if ( entry->dead_report.compare_exchange_strong( expected, (uint64_t) generation + 1, std::memory_order_acq_rel ) )
    {
        table->dead_reports.fetch_add( 1, std::memory_order_release );
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// proofs/tla/RNMsgxSocketTable.tla BeginUpdate: the update's open budget.
inline void rocketnet_msgx_socket_table_begin_update( struct rocketnet_msgx_socket_table_t * table, uint64_t update )
{
    table->update = update;
    table->opens_left = table->opens_per_update;
}

inline int rocketnet_msgx_socket_table_cooldown_entry( const struct rocketnet_address_t * address )
{
    return (int) ( rocketnet_msgx_address_hash( address ) % ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES );
}

// proofs/tla/RNMsgxSocketTable.tla Retire: the word loses its descriptor and advances its
// generation, the address leaves the index, and each holder's pass is
// copied. The descriptor stays open until the sweep sees every holder
// past its passcopy. The sweep's caller deregisters the descriptor from its
// receive poller before (retire_hook); forget's caller deregisters it after,
// from retired_fd. An event the receive holder takes in between resolves
// through the word, whose generation has already moved, so it is dropped.
// (out of line, sdk/rocketnet_msgx_socket_table.cpp: the trace check's plant is library code)
void rocketnet_msgx_socket_table_retire( struct rocketnet_msgx_socket_table_t * table, int slot );

// proofs/tla/RNMsgxSocketTable.tla Mark: the replay walk saw a connected client at address.
// Opens its socket when it has none, within the update's open budget. False
// only when the open is deferred (the walk owes the mark and takes it again
// next update); a refused open or a full table is counted and answers true,
// the client staying on the shared socket.
inline bool rocketnet_msgx_socket_table_mark( struct rocketnet_msgx_socket_table_t * table, const struct rocketnet_address_t * address )
{
    if ( rocketnet_msgx_socket_table_lookup( table, address ) >= 0 )
    {
        return true;
    }

    const int cooldown = rocketnet_msgx_socket_table_cooldown_entry( address );
    if ( table->cooldown_until[cooldown] > table->update && rocketnet_address_equal( &table->cooldown_address[cooldown], address ) )
    {
        table->counts.cooldown++;
        return true;
    }

    if ( table->num_free == 0 )
    {
        table->counts.table_full++;
        return true;
    }

    if ( table->opens_left <= 0 )
    {
        table->counts.open_deferred++;
        return false;
    }
    table->opens_left--;

    const int fd = table->open( table->user, address );
    if ( fd < 0 )
    {
        table->counts.open_failed++;
        return true;
    }

    const int slot = table->free_slots[--table->num_free];
    struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
    rocketnet_assert( entry->state == ROCKETNET_MSGX_SLOT_FREE );
    entry->state = ROCKETNET_MSGX_SLOT_OPEN;
    entry->fd = fd;
    entry->address = *address;
    entry->word.store( rocketnet_msgx_word( entry->generation, fd ), std::memory_order_seq_cst );
    rocketnet_msgx_socket_table_index_insert( table, slot );
    table->num_open++;
    table->counts.opened++;
    return true;
}

// proofs/tla/RNMsgxSocketTable.tla Forget and ForgetCall (the whole call, the trace check's action): the client at address left or moved. Retires its
// socket, if it has one. Always true: nothing is owed.
inline bool rocketnet_msgx_socket_table_forget( struct rocketnet_msgx_socket_table_t * table, const struct rocketnet_address_t * address, int * retired_fd )
{
    *retired_fd = -1;
    const int slot = rocketnet_msgx_socket_table_lookup( table, address );
    if ( slot < 0 )
    {
        return true;
    }
    *retired_fd = table->slots[slot].fd;
    rocketnet_msgx_socket_table_retire( table, slot );
    table->counts.retired_forget++;
    return true;
}

// a retired slot's quarantine is over when every holder's pass has moved
// since the passcopy, or the holder was quiescent at it
inline bool rocketnet_msgx_socket_table_quiescent( const struct rocketnet_msgx_socket_table_t * table, int slot )
{
    const struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
    for ( int h = 0; h < table->num_holders; h++ )
    {
        const uint64_t passcopy = entry->passcopy[h];
        if ( ( passcopy & 1 ) != 0 && table->pass[h].load( std::memory_order_seq_cst ) == passcopy )
        {
            return false;
        }
    }
    return true;
}

// proofs/tla/RNMsgxSocketTable.tla SweepBegin, RetireDead and Close, and SweepCall (the whole sweep, the trace check's action): main's sweep, once per update.
// First every slot a holder reported dead (at the generation it reported) is
// retired and its address cooled down; retire_hook is called with each such
// descriptor before its retire (the caller deregisters it from its poller).
// Then every retired slot whose quarantine is over is closed and freed, with
// any dead report still in it cleared. Returns the sockets closed.
inline int rocketnet_msgx_socket_table_sweep( struct rocketnet_msgx_socket_table_t * table, void ( *retire_hook )( void *, int ), void * hook_user )
{
    if ( table->dead_reports.exchange( 0, std::memory_order_acq_rel ) > 0 )
    {
        for ( int slot = 0; slot < table->capacity; slot++ )
        {
            struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
            const uint64_t report = entry->dead_report.exchange( 0, std::memory_order_acq_rel );
            if ( report == 0 )
            {
                continue;
            }
            if ( entry->state != ROCKETNET_MSGX_SLOT_OPEN || (uint32_t) ( report - 1 ) != entry->generation )
            {
                continue;
            }
            const int cooldown = rocketnet_msgx_socket_table_cooldown_entry( &entry->address );
            table->cooldown_address[cooldown] = entry->address;
            table->cooldown_until[cooldown] = table->update + ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES;
            if ( retire_hook )
            {
                retire_hook( hook_user, entry->fd );
            }
            rocketnet_msgx_socket_table_retire( table, slot );
            table->counts.retired_dead++;
        }
    }

    int closed = 0;
    int kept = 0;
    for ( int i = 0; i < table->num_retired; i++ )
    {
        const int slot = table->retired[i];
        struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
        if ( !rocketnet_msgx_socket_table_quiescent( table, slot ) )
        {
            table->retired[kept++] = slot;
            continue;
        }
        table->close( table->user, entry->fd );
        entry->fd = -1;
        entry->state = ROCKETNET_MSGX_SLOT_FREE;
        // a holder may have reported the retired generation after this
        // sweep's dead scan, before its pass ended; left here it would block
        // the report of the slot's next socket (report_dead takes an empty
        // slot only). Every holder is past that pass, and a later pass cannot
        // resolve the retired generation, so no report of it is still coming
        entry->dead_report.store( 0, std::memory_order_relaxed );
        table->free_slots[table->num_free++] = slot;
        table->counts.closed++;
        closed++;
    }
    table->num_retired = kept;
    return closed;
}

// destroy: every holder has stopped, so every socket open or retired is
// closed now
inline void rocketnet_msgx_socket_table_close_all( struct rocketnet_msgx_socket_table_t * table )
{
    for ( int slot = 0; slot < table->capacity; slot++ )
    {
        struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
        if ( entry->state != ROCKETNET_MSGX_SLOT_FREE && entry->fd >= 0 )
        {
            table->close( table->user, entry->fd );
            table->counts.closed++;
        }
        entry->fd = -1;
        entry->state = ROCKETNET_MSGX_SLOT_FREE;
        entry->word.store( rocketnet_msgx_word( ++entry->generation, -1 ), std::memory_order_relaxed );
    }
    table->num_open = 0;
    table->num_retired = 0;
    table->num_free = table->capacity;
    for ( int i = 0; i < table->capacity; i++ )
    {
        table->free_slots[i] = table->capacity - 1 - i;
    }
    for ( int i = 0; i < table->index_size; i++ )
    {
        table->index[i] = -1;
    }
}

#endif // ROCKETNET_SDK_ROCKETNET_MSGX_SOCKET_TABLE_H
