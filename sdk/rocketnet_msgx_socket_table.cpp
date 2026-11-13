/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_msgx_socket_table.h"

// The retire, out of line: the trace check's plant
// (ROCKETNET_MSGX_SOCKET_TABLE_PLANT) must be library code the plant binary
// compiles in place of the library's (proofs/tla/README-TraceCheck.md): only
// tests/tla_trace_msgx.cpp in the plant build defines it, and its traces must
// fail TraceConsistent. The rest of the table stays inline in its header.

// proofs/tla/RNMsgxSocketTable.tla Retire (RetireFirst, then the passcopy and
// the quarantine): the word loses its descriptor and advances its
// generation, the address leaves the index, and each holder's pass is
// copied. The descriptor stays open until the sweep sees every holder
// past its passcopy. The sweep's caller deregisters the descriptor from its
// receive poller before (retire_hook); forget's caller deregisters it after,
// from retired_fd. An event the receive holder takes in between resolves
// through the word, whose generation has already moved, so it is dropped.
void rocketnet_msgx_socket_table_retire( struct rocketnet_msgx_socket_table_t * table, int slot )
{
    struct rocketnet_msgx_socket_slot_t * entry = &table->slots[slot];
    rocketnet_assert( entry->state == ROCKETNET_MSGX_SLOT_OPEN );

    rocketnet_msgx_socket_table_index_remove( table, slot );
    entry->generation++;
    entry->word.store( rocketnet_msgx_word( entry->generation, -1 ), std::memory_order_seq_cst );
    for ( int h = 0; h < table->num_holders; h++ )
    {
        entry->passcopy[h] = table->pass[h].load( std::memory_order_seq_cst );
    }
#if defined( ROCKETNET_MSGX_SOCKET_TABLE_PLANT )
    // the trace check's plant: close at the retire, no quarantine
    table->close( table->user, entry->fd );
    entry->fd = -1;
    entry->state = ROCKETNET_MSGX_SLOT_FREE;
    table->free_slots[table->num_free++] = slot;
    table->counts.closed++;
    table->num_open--;
#else  // #if defined( ROCKETNET_MSGX_SOCKET_TABLE_PLANT )
    entry->state = ROCKETNET_MSGX_SLOT_RETIRED;
    table->retired[table->num_retired++] = slot;
    table->num_open--;
#endif // #if defined( ROCKETNET_MSGX_SOCKET_TABLE_PLANT )
}
