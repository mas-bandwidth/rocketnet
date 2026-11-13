/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load reconcile's chain (tests/load/harden_chain.h): a table of
// rows, each a clean row the chain must pass or a row with one planted
// violation the chain must fail. The clean rows are the X710 pair's XDP rows
// of 2026-11-13 (main 59890655d, rm-xdp-peak-noload-q2), whose players' and
// server's hosts each took a stray datagram from the internet at a closed port
// on their public address. Pure arithmetic: no time, no sockets.

#include "test_unit_registry.h"

#include "load/harden_chain.h"

#include <stdint.h>

#define check rocketnet_unit_check

// the XDP pair's server to clients direction: the server's XDP transport
// reads no kernel send counters (kernel_out 0), the players' kernel took every
// datagram; the players' host counted 2 no-port drops, both strays

static harden_chain_t harden_chain_test_down()
{
    harden_chain_t c;
    c.library_sent = 66599830;
    c.kernel_out = 0;
    c.qdisc_sent = 4;
    c.arrived_in = 66599830;
    c.arrived_noports = 2;
    c.arrived_noports_strays = 2;
    c.library_received = 66599830;
    return c;
}

// the clients to server direction: the server's XDP program takes the cell's
// datagrams before its kernel (arrived_in 0); its host counted 1 stray

static harden_chain_t harden_chain_test_up()
{
    harden_chain_t c;
    c.library_sent = 4588459;
    c.kernel_out = 4439846;
    c.arrived_in = 0;
    c.arrived_noports = 1;
    c.arrived_noports_strays = 1;
    c.library_received = 4588459;
    return c;
}

static void test_harden_chain_strays_named_pass()
{
    // clean: every datagram sent was received, and each no-port drop is a stray

    harden_chain_result_t down = harden_chain_judge( harden_chain_test_down(), 0 );
    check( down.ok );
    check( down.strays_valid );
    check( down.cell_noports == 0 );
    check( down.library_gap == 0 );

    harden_chain_result_t up = harden_chain_judge( harden_chain_test_up(), 0 );
    check( up.ok );
    check( up.cell_noports == 0 );
    check( up.library_gap == 0 );
}

static void test_harden_chain_unnamed_strays_fail()
{
    // the same rows with no stray counter read (the chain before strays were
    // named): the host's NoPorts names drops the library never had, the gap is
    // below zero, and the chain fails. It does not pass a negative gap

    harden_chain_t down = harden_chain_test_down();
    down.arrived_noports_strays = 0;
    harden_chain_result_t r = harden_chain_judge( down, 0 );
    check( !r.ok );
    check( r.library_gap == -2 );

    harden_chain_t up = harden_chain_test_up();
    up.arrived_noports_strays = 0;
    r = harden_chain_judge( up, 0 );
    check( !r.ok );
    check( r.library_gap == -1 );
}

static void test_harden_chain_planted_loss_fails()
{
    // planted: 66 of the server's datagrams lost where no counter names them
    // (dropped in the players' firewall), strays named as in the clean row

    harden_chain_t down = harden_chain_test_down();
    down.library_received -= 66;
    down.arrived_in -= 66;
    harden_chain_result_t r = harden_chain_judge( down, 0 );
    check( !r.ok );
    check( r.library_gap == 66 );
    check( r.named_drops == 0 );

    // and one lost: a single unnamed datagram fails

    down = harden_chain_test_down();
    down.library_received -= 1;
    r = harden_chain_judge( down, 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
}

static void test_harden_chain_strays_cannot_hide_a_loss()
{
    // planted: a loss the stray counter claims as well. Strays above the host's
    // NoPorts are a broken counter: not applied, and the chain fails

    harden_chain_t down = harden_chain_test_down();
    down.library_received -= 3;
    down.arrived_noports_strays = 5;
    harden_chain_result_t r = harden_chain_judge( down, 0 );
    check( !r.strays_valid );
    check( !r.ok );

    // a negative stray count is broken the same way

    down = harden_chain_test_down();
    down.arrived_noports_strays = -1;
    r = harden_chain_judge( down, 0 );
    check( !r.strays_valid );
    check( !r.ok );

    // planted: the cell's own no-port drops (a client's socket closed while the
    // server still sent to it) counted as strays: the library's lost datagrams
    // are then named by nothing, and the chain fails

    harden_chain_t cell;
    cell.library_sent = 1000;
    cell.kernel_out = 1000;
    cell.arrived_in = 997;
    cell.arrived_noports = 3;
    cell.library_received = 997;
    cell.arrived_noports_strays = 0;
    r = harden_chain_judge( cell, 0 );
    check( r.ok );
    check( r.cell_noports == 3 );
    check( r.library_gap == 0 );

    cell.arrived_noports_strays = 3;
    r = harden_chain_judge( cell, 0 );
    check( r.strays_valid );
    check( !r.ok );
    check( r.library_gap == 3 );
}

static void test_harden_chain_cell_noports_and_strays_together()
{
    // 3 of the cell's datagrams at a closed port and 2 strays: 5 no-port drops
    // on the host, 3 named as the cell's, the equation closes

    harden_chain_t c;
    c.library_sent = 1000;
    c.kernel_out = 1000;
    c.arrived_in = 997;
    c.arrived_noports = 5;
    c.arrived_noports_strays = 2;
    c.library_received = 997;
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.cell_noports == 3 );
    check( r.named_drops == 3 );
    check( r.library_gap == 0 );
    check( r.path_gap == 0 );
}

static void test_harden_chain_duplicates_row()
{
    // a duplicate row passes a gap at or below zero, and fails a loss

    harden_chain_t c;
    c.library_sent = 1000;
    c.kernel_out = 1000;
    c.arrived_in = 1010;
    c.library_received = 1010;
    check( harden_chain_judge( c, 1 ).ok );
    check( !harden_chain_judge( c, 0 ).ok );

    c.arrived_in = 990;
    c.library_received = 990;
    check( !harden_chain_judge( c, 1 ).ok );
}

REGISTER_UNIT_TEST( test_harden_chain_strays_named_pass );
REGISTER_UNIT_TEST( test_harden_chain_unnamed_strays_fail );
REGISTER_UNIT_TEST( test_harden_chain_planted_loss_fails );
REGISTER_UNIT_TEST( test_harden_chain_strays_cannot_hide_a_loss );
REGISTER_UNIT_TEST( test_harden_chain_cell_noports_and_strays_together );
REGISTER_UNIT_TEST( test_harden_chain_duplicates_row );
