/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's per-client socket table
// (sdk/rocketnet_msgx_socket_table.h; model proofs/tla/RNMsgxSocketTable.tla
// and proofs/tla/RNMsgxAddressIndex.tla) over a fake kernel: open hands out
// the lowest free descriptor, refuses a second socket connected to the same
// peer (EADDRINUSE, as macOS does) and refuses when none is free (EMFILE);
// close frees it. Holders are stepped by hand on this thread. No socket, no
// clock, no thread.
//
// Each test pins one rule of the table: the open, forget and close lifecycle;
// the quarantine holding a retired socket open while a holder that was inside
// a pass at the retire is still in it, and releasing it once that pass ends;
// a stamp from before a retire resolving to no socket; a dead report retiring
// only the generation it names and starting the address's cooldown; the open
// budget deferring a mark to the next update; a refused open and a full table
// counted with no slot taken; the index against a reference over a seeded
// walk of 1000 slots with deletes; and close_all.

#include "test_unit_registry.h"

#include "rocketnet_address.h"
#include "rocketnet_msgx_socket_table.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSGX_TEST_FDS 2048

struct msgx_test_kernel_t
{
    int num_fds;
    bool open[MSGX_TEST_FDS];
    struct rocketnet_address_t peer[MSGX_TEST_FDS];
    int opens;
    int closes;
    int refused;
    int hooks;
    int hooked_fd;
};

static int msgx_test_open( void * user, const struct rocketnet_address_t * address )
{
    struct msgx_test_kernel_t * kernel = (struct msgx_test_kernel_t *) user;
    for ( int f = 0; f < kernel->num_fds; f++ )
    {
        if ( kernel->open[f] && rocketnet_address_equal( &kernel->peer[f], address ) )
        {
            kernel->refused++;
            return -EADDRINUSE;
        }
    }
    for ( int f = 0; f < kernel->num_fds; f++ )
    {
        if ( !kernel->open[f] )
        {
            kernel->open[f] = true;
            kernel->peer[f] = *address;
            kernel->opens++;
            return f;
        }
    }
    kernel->refused++;
    return -EMFILE;
}

static void msgx_test_close( void * user, int fd )
{
    struct msgx_test_kernel_t * kernel = (struct msgx_test_kernel_t *) user;
    rocketnet_unit_check( fd >= 0 && fd < kernel->num_fds );
    rocketnet_unit_check( kernel->open[fd] );
    kernel->open[fd] = false;
    kernel->closes++;
}

static void msgx_test_hook( void * user, int fd )
{
    struct msgx_test_kernel_t * kernel = (struct msgx_test_kernel_t *) user;
    kernel->hooks++;
    kernel->hooked_fd = fd;
}

static struct rocketnet_address_t msgx_test_address( int n )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = 10;
    address.data.ipv4[1] = (uint8_t) ( n >> 16 );
    address.data.ipv4[2] = (uint8_t) ( n >> 8 );
    address.data.ipv4[3] = 1;
    address.port = (uint16_t) ( 40000 + ( n & 0xFF ) );
    return address;
}

struct msgx_test_t
{
    struct msgx_test_kernel_t * kernel;
    struct rocketnet_msgx_socket_table_t * table;
};

static struct msgx_test_t msgx_test_create( int capacity, int num_holders, int opens_per_update, int num_fds )
{
    struct msgx_test_t test;
    test.kernel = new msgx_test_kernel_t();
    test.kernel->num_fds = num_fds;
    test.table = new rocketnet_msgx_socket_table_t();
    rocketnet_msgx_socket_table_init( test.table, capacity, num_holders, opens_per_update, msgx_test_open, msgx_test_close, test.kernel );
    return test;
}

static void msgx_test_destroy( struct msgx_test_t * test )
{
    delete test->table;
    delete test->kernel;
}

static uint64_t msgx_test_counter( const struct msgx_test_t & test, int counter )
{
    return test.table->counters[counter];
}

// open, stamp, resolve, forget, close after the sweep, and the slot reused
// with the next generation
static void test_rocketnet_msgx_socket_table_lifecycle()
{
    struct msgx_test_t test = msgx_test_create( 3, 2, 4, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    const int slot = rocketnet_msgx_socket_table_lookup( test.table, &a );
    rocketnet_unit_check( slot == 0 );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_OPEN );
    rocketnet_unit_check( test.table->slots[slot].fd == 0 );
    rocketnet_unit_check( test.kernel->open[0] );
    rocketnet_unit_check( test.table->num_open == 1 && test.table->num_free == 2 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPENED ) == 1 );

    // a second mark of an open address changes nothing
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( test.kernel->opens == 1 );

    uint32_t generation = 99;
    rocketnet_unit_check( rocketnet_msgx_socket_table_stamp( test.table, &a, &generation ) == slot );
    rocketnet_unit_check( generation == 0 );
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, generation ) == 0 );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );

    int retired_fd = -1;
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &a, &retired_fd ) );
    rocketnet_unit_check( retired_fd == 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_RETIRED );
    rocketnet_unit_check( rocketnet_msgx_word_fd( test.table->slots[slot].word.load() ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_word_generation( test.table->slots[slot].word.load() ) == 1 );
    rocketnet_unit_check( test.table->num_retired == 1 && test.table->num_open == 0 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_RETIRED_FORGET ) == 1 );
    rocketnet_unit_check( test.kernel->open[0] );

    // a forget of an address with no socket retires nothing
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &a, &retired_fd ) );
    rocketnet_unit_check( retired_fd == -1 );

    // every holder quiescent: the sweep closes it
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, msgx_test_hook, test.kernel ) == 1 );
    rocketnet_unit_check( !test.kernel->open[0] );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_FREE );
    rocketnet_unit_check( test.table->slots[slot].fd == -1 );
    rocketnet_unit_check( test.table->num_free == 3 && test.table->num_retired == 0 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_CLOSED ) == 1 );
    rocketnet_unit_check( test.kernel->hooks == 0 );

    // reopened: the same slot and descriptor, the next generation
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == slot );
    rocketnet_unit_check( test.table->slots[slot].fd == 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_stamp( test.table, &a, &generation ) == slot );
    rocketnet_unit_check( generation == 1 );

    msgx_test_destroy( &test );
}

// a retired socket stays open while a holder that was inside a pass at the
// retire is still in it; a holder that begins after the retire does not hold
// it; once the pass ends the sweep closes it
static void test_rocketnet_msgx_socket_table_quarantine()
{
    struct msgx_test_t test = msgx_test_create( 3, 3, 4, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    const struct rocketnet_address_t b = msgx_test_address( 2 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    const int slot = rocketnet_msgx_socket_table_lookup( test.table, &a );
    uint32_t generation = 0;
    rocketnet_unit_check( rocketnet_msgx_socket_table_stamp( test.table, &a, &generation ) == slot );

    // holder 0 resolves the descriptor inside its pass, then main forgets
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    const int fd = rocketnet_msgx_socket_table_resolve( test.table, slot, generation );
    rocketnet_unit_check( fd == 0 );
    int retired_fd = -1;
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &a, &retired_fd ) );
    rocketnet_unit_check( retired_fd == fd );

    // holder 1 begins after the retire: it sees the retired word, and does
    // not hold the quarantine
    rocketnet_msgx_socket_table_holder_begin( test.table, 1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, generation ) == -1 );

    // holder 0 is still in its pass: kept, the descriptor open, so a new
    // client cannot be handed it
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 0 );
    rocketnet_unit_check( test.kernel->open[fd] );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_RETIRED );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    const int slot_b = rocketnet_msgx_socket_table_lookup( test.table, &b );
    rocketnet_unit_check( slot_b >= 0 && slot_b != slot );
    rocketnet_unit_check( test.table->slots[slot_b].fd != fd );

    // holder 2 cycles a whole pass: no change
    rocketnet_msgx_socket_table_holder_begin( test.table, 2 );
    rocketnet_msgx_socket_table_holder_end( test.table, 2 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 0 );
    rocketnet_unit_check( test.kernel->open[fd] );

    // holder 0 ends its pass: closed, though holder 1 is still inside its own
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 1 );
    rocketnet_unit_check( !test.kernel->open[fd] );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_FREE );
    rocketnet_msgx_socket_table_holder_end( test.table, 1 );

    // a holder that begins a new pass after the snapshot does not hold a
    // retire it was inside a different pass for
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &b, &retired_fd ) );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 1 );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_unit_check( test.table->num_free == 3 );

    msgx_test_destroy( &test );
}

// a packet stamped before a retire resolves to no socket (the shared one),
// also after the slot is reopened for another client on the same descriptor
static void test_rocketnet_msgx_socket_table_stale_stamp()
{
    struct msgx_test_t test = msgx_test_create( 3, 1, 4, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    const struct rocketnet_address_t b = msgx_test_address( 2 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    uint32_t stale = 0;
    const int slot = rocketnet_msgx_socket_table_stamp( test.table, &a, &stale );
    rocketnet_unit_check( slot == 0 );

    int retired_fd = -1;
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &a, &retired_fd ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, stale ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 1 );

    // b takes the slot and the descriptor a had
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) == slot );
    rocketnet_unit_check( test.table->slots[slot].fd == retired_fd );
    uint32_t fresh = 0;
    rocketnet_unit_check( rocketnet_msgx_socket_table_stamp( test.table, &b, &fresh ) == slot );
    rocketnet_unit_check( fresh != stale );

    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, stale ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, fresh ) == retired_fd );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, -1, 0 ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, 3, fresh ) == -1 );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );

    // an address with no socket stamps the shared socket
    const struct rocketnet_address_t c = msgx_test_address( 3 );
    uint32_t none = 7;
    rocketnet_unit_check( rocketnet_msgx_socket_table_stamp( test.table, &c, &none ) == -1 );
    rocketnet_unit_check( none == 0 );

    msgx_test_destroy( &test );
}

// a dead report retires the slot only at the generation it names, calls the
// retire hook with the descriptor first, and cools the address down for
// ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES updates
static void test_rocketnet_msgx_socket_table_dead_report()
{
    struct msgx_test_t test = msgx_test_create( 3, 2, 4, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    rocketnet_msgx_socket_table_begin_update( test.table, 100 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    uint32_t generation = 0;
    const int slot = rocketnet_msgx_socket_table_stamp( test.table, &a, &generation );
    rocketnet_unit_check( slot >= 0 );
    const int fd = test.table->slots[slot].fd;

    // a report about an older generation of the slot is ignored
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_msgx_socket_table_report_dead( test.table, slot, generation + 5 );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_unit_check( test.table->dead_reports.load() == 1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, msgx_test_hook, test.kernel ) == 0 );
    rocketnet_unit_check( test.table->dead_reports.load() == 0 );
    rocketnet_unit_check( test.table->slots[slot].dead_report.load() == 0 );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_OPEN );
    rocketnet_unit_check( test.kernel->hooks == 0 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_RETIRED_DEAD ) == 0 );

    // reports out of range are ignored
    rocketnet_msgx_socket_table_report_dead( test.table, -1, generation );
    rocketnet_msgx_socket_table_report_dead( test.table, 3, generation );
    rocketnet_unit_check( test.table->dead_reports.load() == 0 );

    // two holders report the open generation: one report is taken
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    rocketnet_msgx_socket_table_holder_begin( test.table, 1 );
    rocketnet_msgx_socket_table_report_dead( test.table, slot, generation );
    rocketnet_msgx_socket_table_report_dead( test.table, slot, generation );
    rocketnet_unit_check( test.table->dead_reports.load() == 1 );
    rocketnet_msgx_socket_table_holder_end( test.table, 1 );

    // holder 0 is still inside: retired, hooked, kept open
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, msgx_test_hook, test.kernel ) == 0 );
    rocketnet_unit_check( test.kernel->hooks == 1 && test.kernel->hooked_fd == fd );
    rocketnet_unit_check( test.table->slots[slot].state == ROCKETNET_MSGX_SLOT_RETIRED );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot, generation ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_RETIRED_DEAD ) == 1 );
    rocketnet_unit_check( test.kernel->open[fd] );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, msgx_test_hook, test.kernel ) == 1 );
    rocketnet_unit_check( !test.kernel->open[fd] );

    // the cooldown: refused and counted for the updates it lasts, then open
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_COOLDOWN ) == 1 );
    rocketnet_msgx_socket_table_begin_update( test.table, 100 + ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES - 1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_COOLDOWN ) == 2 );

    // another address is not cooled down
    const struct rocketnet_address_t b = msgx_test_address( 2 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) >= 0 );

    rocketnet_msgx_socket_table_begin_update( test.table, 100 + ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    const int reopened = rocketnet_msgx_socket_table_lookup( test.table, &a );
    rocketnet_unit_check( reopened >= 0 );
    rocketnet_unit_check( test.table->slots[reopened].generation == ( reopened == slot ? generation + 1 : 0 ) );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_COOLDOWN ) == 2 );

    msgx_test_destroy( &test );
}

// past the update's open budget a mark is deferred (false, counted, nothing
// opened); the next update opens it
static void test_rocketnet_msgx_socket_table_open_budget()
{
    struct msgx_test_t test = msgx_test_create( 3, 1, 2, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    const struct rocketnet_address_t b = msgx_test_address( 2 );
    const struct rocketnet_address_t c = msgx_test_address( 3 );
    rocketnet_msgx_socket_table_begin_update( test.table, 1 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( test.table->opens_left == 0 );
    rocketnet_unit_check( !rocketnet_msgx_socket_table_mark( test.table, &c ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &c ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPEN_DEFERRED ) == 1 );
    rocketnet_unit_check( test.kernel->opens == 2 );

    // an open address costs no budget
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );

    rocketnet_msgx_socket_table_begin_update( test.table, 2 );
    rocketnet_unit_check( test.table->opens_left == 2 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &c ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &c ) >= 0 );
    rocketnet_unit_check( test.table->opens_left == 1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPENED ) == 3 );

    msgx_test_destroy( &test );
}

// a refused open (descriptor exhaustion, or a peer that still has a socket in
// quarantine) is counted and takes no slot; a full table is counted
static void test_rocketnet_msgx_socket_table_refused_and_full()
{
    struct msgx_test_t test = msgx_test_create( 2, 1, 8, 1 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    const struct rocketnet_address_t b = msgx_test_address( 2 );
    const struct rocketnet_address_t c = msgx_test_address( 3 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );

    // one descriptor in the kernel: exhausted
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPEN_FAILED ) == 1 );
    rocketnet_unit_check( test.table->num_free == 1 && test.table->num_open == 1 );
    rocketnet_unit_check( test.table->opens_left == 6 );

    // a in quarantine (holder inside): its peer's new socket is refused
    test.kernel->num_fds = 4;
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    int retired_fd = -1;
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &a, &retired_fd ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPEN_FAILED ) == 2 );
    rocketnet_unit_check( test.table->num_free == 1 && test.table->num_retired == 1 );

    // b takes the last slot; with the other in quarantine, c finds it full
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) >= 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &c ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &c ) == -1 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_TABLE_FULL ) == 1 );

    // every open call is opened or counted refused
    rocketnet_unit_check( (uint64_t) ( test.kernel->opens + test.kernel->refused ) ==
                          msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPENED ) +
                              msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPEN_FAILED ) );

    // the quarantine ends: a's socket is closed and its slot free again
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) >= 0 );

    msgx_test_destroy( &test );
}

// splitmix64, so a failure reproduces
static uint64_t msgx_test_random( uint64_t * state )
{
    uint64_t z = ( *state += 0x9E3779B97F4A7C15ULL );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
    return z ^ ( z >> 31 );
}

#define MSGX_TEST_ADDRESSES 1500

// the index against a reference over a seeded walk of marks and forgets on
// 1000 slots: every open address is found at its slot, every other is not,
// and the slots and the free stack stay whole
static void test_rocketnet_msgx_socket_table_index_reference()
{
    struct msgx_test_t test = msgx_test_create( ROCKETNET_MSGX_TABLE_MAX_SLOTS, 1, ROCKETNET_MSGX_TABLE_MAX_SLOTS, MSGX_TEST_FDS );
    static struct rocketnet_address_t addresses[MSGX_TEST_ADDRESSES];
    static int reference[MSGX_TEST_ADDRESSES];     // the slot open at the address, or -1
    for ( int i = 0; i < MSGX_TEST_ADDRESSES; i++ )
    {
        addresses[i] = msgx_test_address( i );
        reference[i] = -1;
    }
    int num_open = 0;
    int removes_with_shift = 0;
    uint64_t random = 0x6d73677874626c31ULL;
    for ( int step = 0; step < 200000; step++ )
    {
        const int i = (int) ( msgx_test_random( &random ) % MSGX_TEST_ADDRESSES );
        // phases of 20000 steps: filling (marks win, the table fills and
        // marks find it full) and draining, so runs grow and are deleted
        const bool filling = ( step / 20000 ) % 2 == 0;
        const bool mark = (int) ( msgx_test_random( &random ) % 100 ) < ( filling ? 85 : 30 );
        if ( mark )
        {
            const bool full = reference[i] < 0 && num_open == ROCKETNET_MSGX_TABLE_MAX_SLOTS;
            rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &addresses[i] ) );
            if ( reference[i] < 0 && !full )
            {
                reference[i] = rocketnet_msgx_socket_table_lookup( test.table, &addresses[i] );
                rocketnet_unit_check( reference[i] >= 0 );
                num_open++;
            }
        }
        else
        {
            const uint32_t home = rocketnet_msgx_address_hash( &addresses[i] ) & ( ROCKETNET_MSGX_TABLE_INDEX_SIZE - 1 );
            const bool next_used = test.table->index[( home + 1 ) & ( ROCKETNET_MSGX_TABLE_INDEX_SIZE - 1 )] >= 0;
            int retired_fd = -1;
            rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &addresses[i], &retired_fd ) );
            rocketnet_unit_check( ( retired_fd >= 0 ) == ( reference[i] >= 0 ) );
            if ( reference[i] >= 0 )
            {
                removes_with_shift += next_used ? 1 : 0;
                reference[i] = -1;
                num_open--;
            }
            rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == ( retired_fd >= 0 ? 1 : 0 ) );
        }
        rocketnet_msgx_socket_table_begin_update( test.table, (uint64_t) step + 1 );

        // the address touched, every step; every address, every 1000 steps
        rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &addresses[i] ) == reference[i] );
        if ( step % 1000 == 999 )
        {
            int found = 0;
            for ( int j = 0; j < MSGX_TEST_ADDRESSES; j++ )
            {
                rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &addresses[j] ) == reference[j] );
                found += reference[j] >= 0 ? 1 : 0;
            }
            int entries = 0;
            for ( int e = 0; e < ROCKETNET_MSGX_TABLE_INDEX_SIZE; e++ )
            {
                entries += test.table->index[e] >= 0 ? 1 : 0;
            }
            rocketnet_unit_check( found == num_open && entries == num_open );
            rocketnet_unit_check( test.table->num_open == num_open );
            rocketnet_unit_check( test.table->num_open + test.table->num_free + test.table->num_retired == ROCKETNET_MSGX_TABLE_MAX_SLOTS );
        }
    }
    // the walk filled the table and its deletes moved runs
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_TABLE_FULL ) > 0 );
    rocketnet_unit_check( removes_with_shift > 1000 );
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_OPENED ) ==
                          msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_CLOSED ) + (uint64_t) num_open );

    msgx_test_destroy( &test );
}

// close_all closes every socket open or in quarantine, frees every slot,
// empties the index, and advances every generation so no stamp resolves
static void test_rocketnet_msgx_socket_table_close_all()
{
    struct msgx_test_t test = msgx_test_create( 3, 1, 4, 8 );
    const struct rocketnet_address_t a = msgx_test_address( 1 );
    const struct rocketnet_address_t b = msgx_test_address( 2 );
    const struct rocketnet_address_t c = msgx_test_address( 3 );

    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &a ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &c ) );
    uint32_t generation_a = 0;
    uint32_t generation_c = 0;
    const int slot_a = rocketnet_msgx_socket_table_stamp( test.table, &a, &generation_a );
    const int slot_c = rocketnet_msgx_socket_table_stamp( test.table, &c, &generation_c );

    // c retired while a holder is inside: in quarantine at the close
    rocketnet_msgx_socket_table_holder_begin( test.table, 0 );
    int retired_fd = -1;
    rocketnet_unit_check( rocketnet_msgx_socket_table_forget( test.table, &c, &retired_fd ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_sweep( test.table, NULL, NULL ) == 0 );
    rocketnet_msgx_socket_table_holder_end( test.table, 0 );

    rocketnet_msgx_socket_table_close_all( test.table );
    rocketnet_unit_check( test.kernel->closes == 3 );
    for ( int f = 0; f < 8; f++ )
    {
        rocketnet_unit_check( !test.kernel->open[f] );
    }
    rocketnet_unit_check( msgx_test_counter( test, ROCKETNET_MSGX_TABLE_COUNTER_CLOSED ) == 3 );
    rocketnet_unit_check( test.table->num_free == 3 && test.table->num_open == 0 && test.table->num_retired == 0 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &a ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) == -1 );
    for ( int s = 0; s < 3; s++ )
    {
        rocketnet_unit_check( test.table->slots[s].state == ROCKETNET_MSGX_SLOT_FREE );
        rocketnet_unit_check( test.table->slots[s].fd == -1 );
        rocketnet_unit_check( rocketnet_msgx_word_fd( test.table->slots[s].word.load() ) == -1 );
    }
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot_a, generation_a ) == -1 );
    rocketnet_unit_check( rocketnet_msgx_socket_table_resolve( test.table, slot_c, generation_c ) == -1 );

    // the free stack pops slot 0 first again
    rocketnet_unit_check( rocketnet_msgx_socket_table_mark( test.table, &b ) );
    rocketnet_unit_check( rocketnet_msgx_socket_table_lookup( test.table, &b ) == 0 );

    msgx_test_destroy( &test );
}

REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_lifecycle );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_quarantine );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_stale_stamp );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_dead_report );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_open_budget );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_refused_and_full );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_index_reference );
REGISTER_UNIT_TEST( test_rocketnet_msgx_socket_table_close_all );
