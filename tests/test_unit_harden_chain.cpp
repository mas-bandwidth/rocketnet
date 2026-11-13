/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load reconcile's chain (tests/load/harden_chain.h): a table of
// rows, each a clean row the chain must pass or a row with one planted
// violation the chain must fail. The clean rows are the X710 pair's XDP rows
// of 2026-11-13 (main 59890655d, rm-xdp-peak-noload-q2), whose players' and
// server's hosts each took a stray datagram from the internet at a closed port
// on their public address. Pure arithmetic: no time, no sockets. The refused
// term's rows read the counter names from the library.

#include "test_unit_registry.h"

#include "load/harden_chain.h"
#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#include <map>
#include <stdint.h>
#include <string>

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

// the refusals before received, with a hostile role: the clients sent 1000,
// the hostile role 10 (2 too short, 1 too large, 7 the gso workers' prefilter
// refused for another reason); the server's library received the clients'
// 1000. The workers count all 10 into the transport's PACKETS_RECEIVED, the 3
// size refusals into PREFILTER_TOO_SHORT/TOO_LARGE as well
static harden_chain_t harden_chain_test_hostile_gso()
{
    harden_chain_t c;
    c.library_sent = 1010;
    c.kernel_out = 1010;
    c.arrived_in = 1010;
    c.library_received = 1000;
    return c;
}

static void test_harden_chain_refused_hostile_worker_prefilter()
{
    harden_chain_t c = harden_chain_test_hostile_gso();
    c.library_refused_before_received = harden_refused_before_received( 1010, 1000, 0 );
    check( c.library_refused_before_received == 10 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.library_gap == 0 );
    check( r.receive_gap == 0 );

    // the size counters alone (the term with a hostile role before this fix)
    // leave the 7 other refusals unnamed: the chain fails a cell that lost nothing

    c.library_refused_before_received = 2 + 1;
    r = harden_chain_judge( c, 0 );
    check( !r.ok );
    check( r.library_gap == 7 );

    // a datagram lost after the workers counted it is still caught

    c = harden_chain_test_hostile_gso();
    c.library_received = 999;
    c.library_refused_before_received = harden_refused_before_received( 1010, 1000, 0 );
    r = harden_chain_judge( c, 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
}

static void test_harden_chain_refused_both_parts_io_uring()
{
    // io_uring, 7 hostile datagrams and no client traffic: 3 dropped for size on
    // the receive path (URING_RECEIVE_DROPPED_SIZE, outside PACKETS_RECEIVED), 4
    // refused by the workers' prefilter (inside it). The term is both parts; either
    // part alone (the two branches before this fix) leaves a gap

    harden_chain_t c;
    c.library_sent = 7;
    c.kernel_out = 7;
    c.arrived_in = 7;
    c.library_received = 0;
    c.library_refused_before_received = harden_refused_before_received( 4, 0, 3 );
    check( c.library_refused_before_received == 7 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.library_gap == 0 );

    c.library_refused_before_received = 4;
    check( !harden_chain_judge( c, 0 ).ok );
    c.library_refused_before_received = 3;
    check( !harden_chain_judge( c, 0 ).ok );

    // gso counts its size drops inside PACKETS_RECEIVED: the same 7 are the
    // difference alone, with no outside part

    check( harden_refused_before_received( 7, 0, 0 ) == 7 );
}

static void test_harden_chain_refused_negative_difference_is_red()
{
    // the library counting more received than the transport is a counter
    // defect: the term is not clamped, and the chain fails

    harden_chain_t c;
    c.library_sent = 100;
    c.kernel_out = 100;
    c.arrived_in = 100;
    c.library_received = 100;
    c.library_refused_before_received = harden_refused_before_received( 95, 100, 0 );
    check( c.library_refused_before_received == -5 );
    check( !harden_chain_judge( c, 0 ).ok );
}

static void test_harden_chain_refused_from_terms_reads_the_counters()
{
    // the reconcile's term from the printed terms: each key is the server's
    // c2s_transport_ prefix and the library's own counter name, so a misspelled
    // or renamed counter reads 0 and fails here

    const int outside[] = { ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE, ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT, ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE,
                            ROCKETNET_COUNTER_RIO_RECEIVE_TOO_SHORT, ROCKETNET_COUNTER_RIO_RECEIVE_TOO_LARGE };
    for ( int i = 0; i < (int) ( sizeof( outside ) / sizeof( outside[0] ) ); i++ )
    {
        std::map<std::string, int64_t> terms;
        terms[std::string( "server.c2s_transport_" ) + rocketnet_counter_name( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1010;
        terms["server.c2s_packets_received"] = 1000;
        terms[std::string( "server.c2s_transport_" ) + rocketnet_counter_name( outside[i] )] = 3;
        // the prefilter's own size counters mix both parts: never read
        terms[std::string( "server.c2s_transport_" ) + rocketnet_counter_name( ROCKETNET_COUNTER_PREFILTER_TOO_SHORT )] = 100;
        terms[std::string( "server.c2s_transport_" ) + rocketnet_counter_name( ROCKETNET_COUNTER_PREFILTER_TOO_LARGE )] = 100;
        const int64_t refused = harden_refused_from_terms( [&terms]( const char * key ) -> int64_t
                                                           {
                                                               std::map<std::string, int64_t>::const_iterator it = terms.find( key );
                                                               return it == terms.end() ? 0 : it->second; } );
        check( refused == 13 );
    }
}

REGISTER_UNIT_TEST( test_harden_chain_refused_from_terms_reads_the_counters );
REGISTER_UNIT_TEST( test_harden_chain_refused_hostile_worker_prefilter );
REGISTER_UNIT_TEST( test_harden_chain_refused_both_parts_io_uring );
REGISTER_UNIT_TEST( test_harden_chain_refused_negative_difference_is_red );
REGISTER_UNIT_TEST( test_harden_chain_strays_named_pass );
REGISTER_UNIT_TEST( test_harden_chain_unnamed_strays_fail );
REGISTER_UNIT_TEST( test_harden_chain_planted_loss_fails );
REGISTER_UNIT_TEST( test_harden_chain_strays_cannot_hide_a_loss );
REGISTER_UNIT_TEST( test_harden_chain_cell_noports_and_strays_together );
REGISTER_UNIT_TEST( test_harden_chain_duplicates_row );
