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

static void test_harden_chain_gso_slack_bounds()
{
    // the XDP down row sends below the kernel (kernel_out 0): its send_gap is
    // every datagram, not a GSO batch. Planted: one of the cell's no-port
    // drops (2 on the host, 1 a stray) and 1000 lost where no counter names
    // them. The slack is 0 and the 999 unnamed fail (the slack was the whole
    // 66599830 and passed them)

    harden_chain_t down = harden_chain_test_down();
    down.arrived_noports_strays = 1;
    down.library_received -= 1000;
    harden_chain_result_t r = harden_chain_judge( down, 0 );
    check( r.cell_noports == 1 );
    check( r.library_gap == 999 );
    check( r.gso_slack == 0 );
    check( !r.ok );

    // the up row's players send GSO batches through their kernel (send_gap
    // 148613). One named skb drop hides at most 63 more datagrams: a gap of 63
    // passes as gso_slack, 64 fails, and so does a loss of 999 (which the
    // whole send_gap passed)

    harden_chain_t up = harden_chain_test_up();
    up.arrived_noports_strays = 0;
    up.library_received -= 64;
    r = harden_chain_judge( up, 0 );
    check( r.cell_noports == 1 );
    check( r.library_gap == 63 );
    check( r.gso_slack == 63 );
    check( r.ok );

    up.library_received -= 1;
    r = harden_chain_judge( up, 0 );
    check( r.library_gap == 64 );
    check( !r.ok );

    up = harden_chain_test_up();
    up.arrived_noports_strays = 0;
    up.library_received -= 1000;
    r = harden_chain_judge( up, 0 );
    check( r.library_gap == 999 );
    check( !r.ok );

    // two named skb drops: up to 126
    up = harden_chain_test_up();
    up.arrived_noports = 3;
    up.arrived_noports_strays = 1;
    up.library_received -= 128;
    r = harden_chain_judge( up, 0 );
    check( r.library_gap == 126 && r.gso_slack == 126 && r.ok );

    // each enabling condition alone false, the 63 gap above fails: no
    // kernel counters on the sender's OS, and no send_gap (the kernel counted
    // every datagram, so nothing was batched)

    up = harden_chain_test_up();
    up.arrived_noports_strays = 0;
    up.library_received -= 64;
    up.sender_counters = false;
    r = harden_chain_judge( up, 0 );
    check( r.gso_slack == 0 && !r.ok );

    up = harden_chain_test_up();
    up.arrived_noports_strays = 0;
    up.library_received -= 64;
    up.kernel_out = up.library_sent;
    r = harden_chain_judge( up, 0 );
    check( r.send_gap == 0 && r.gso_slack == 0 && !r.ok );

    // the receiving device's overlap: a gap down to -overlap passes, one
    // below it fails

    harden_chain_t c;
    c.library_sent = 1000;
    c.kernel_out = 1000;
    c.arrived_in = 1000;
    c.library_received = 1000;
    c.arrived_device_dropped = 2;
    c.device_overlap = 2;
    r = harden_chain_judge( c, 0 );
    check( r.library_gap == -2 && r.ok );
    c.arrived_device_dropped = 3;
    r = harden_chain_judge( c, 0 );
    check( r.library_gap == -3 && !r.ok );
}

// The refusals before received, through the reconcile's own path: one map of
// the terms the roles print, read by harden_chain_up_from_terms (the chain the
// reconcile judges) and harden_handoff_gap_from_terms. The transport counter
// keys are the server's c2s_transport_ prefix and the library's own counter
// name, so a renamed counter reads 0 and fails here

typedef std::map<std::string, int64_t> harden_terms_t;

static std::string harden_transport_key( int counter )
{
    return std::string( "server.c2s_transport_" ) + rocketnet_counter_name( counter );
}

static harden_chain_t harden_chain_test_up( const harden_terms_t & terms )
{
    return harden_chain_up_from_terms( [&terms]( const char * key ) -> int64_t
                                       {
                                           harden_terms_t::const_iterator it = terms.find( key );
                                           return it == terms.end() ? 0 : it->second; } );
}

static int64_t harden_chain_test_handoff_gap( const harden_terms_t & terms )
{
    return harden_handoff_gap_from_terms( [&terms]( const char * key ) -> int64_t
                                          {
                                              harden_terms_t::const_iterator it = terms.find( key );
                                              return it == terms.end() ? 0 : it->second; } );
}

// a gso cell with a hostile role: the clients sent 1000, the hostile role 10
// (2 too short, 1 too large, 7 the workers' prefilter refused for another
// reason); the server's library received the clients' 1000. The workers count
// all 10 into the transport's PACKETS_RECEIVED and into their own totals
// (c2s_worker_refused), the 3 size refusals into PREFILTER_TOO_SHORT/TOO_LARGE
// as well
static harden_terms_t harden_terms_test_hostile_gso()
{
    harden_terms_t t;
    t["clients.c2s_packets_sent"] = 1000;
    t["hostile.sent"] = 10;
    t["clients.c2s_udp_out"] = 1010;
    t["server.c2s_udp_in"] = 1010;
    t["clients.c2s_kernel_read"] = 1;
    t["server.c2s_kernel_read"] = 1;
    t["server.c2s_packets_received"] = 1000;
    t["server.c2s_worker_refused"] = 10;
    t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1010;
    t[harden_transport_key( ROCKETNET_COUNTER_PREFILTER_TOO_SHORT )] = 2;
    t[harden_transport_key( ROCKETNET_COUNTER_PREFILTER_TOO_LARGE )] = 1;
    return t;
}

static void test_harden_chain_refused_hostile_worker_prefilter()
{
    harden_terms_t t = harden_terms_test_hostile_gso();
    harden_chain_t c = harden_chain_test_up( t );
    check( c.library_sent == 1010 );
    check( c.library_refused_before_received == 10 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.library_gap == 0 );
    check( r.receive_gap == 0 );
    check( harden_chain_test_handoff_gap( t ) == 0 );

    // the size counters alone (the term with a hostile role before R42) leave
    // the 7 other refusals unnamed: the chain fails a cell that lost nothing

    c.library_refused_before_received = 2 + 1;
    r = harden_chain_judge( c, 0 );
    check( !r.ok );
    check( r.library_gap == 7 );
}

static void test_harden_chain_refused_received_count_short_is_red()
{
    // the library's received count short of the transport's: the transport
    // counted 1010 in PACKETS_RECEIVED, the workers refused 10, the library
    // counted 999 received. The term is the workers' own count, so the chain
    // judges the received count: the missing one stays in the gap and the
    // chain fails, and so does the handoff. A term of PACKETS_RECEIVED less the
    // received count (4703c70f0) reads 11 refused here and passes

    harden_terms_t t = harden_terms_test_hostile_gso();
    t["server.c2s_packets_received"] = 999;
    harden_chain_t c = harden_chain_test_up( t );
    check( c.library_refused_before_received == 10 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
    check( r.receive_gap == 1 );
    check( harden_chain_test_handoff_gap( t ) == 1 );

    // a datagram a worker handed on and main never took: main counts both
    // PACKETS_RECEIVED and the received count, so both read one short. The
    // chain fails on the gap and the handoff balances

    t = harden_terms_test_hostile_gso();
    t["server.c2s_packets_received"] = 999;
    t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1009;
    r = harden_chain_judge( harden_chain_test_up( t ), 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
    check( harden_chain_test_handoff_gap( t ) == 0 );
    check( !harden_up_ok( r.ok, harden_chain_test_handoff_gap( t ) ) );

    // the transport counting one more in PACKETS_RECEIVED than main took or a
    // worker refused: the chain balances on the library's counts and the
    // handoff alone is red

    t = harden_terms_test_hostile_gso();
    t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1011;
    r = harden_chain_judge( harden_chain_test_up( t ), 0 );
    check( r.ok );
    check( harden_chain_test_handoff_gap( t ) == 1 );
    check( !harden_up_ok( r.ok, harden_chain_test_handoff_gap( t ) ) );
    check( harden_up_ok( r.ok, 0 ) );

    // and with no hostile role: the received count one short

    harden_terms_t clean;
    clean["clients.c2s_packets_sent"] = 1000;
    clean["clients.c2s_udp_out"] = 1000;
    clean["server.c2s_udp_in"] = 1000;
    clean["server.c2s_packets_received"] = 999;
    clean[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1000;
    r = harden_chain_judge( harden_chain_test_up( clean ), 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
}

static void test_harden_chain_refused_both_parts_io_uring()
{
    // io_uring, 7 hostile datagrams and no client traffic: 3 dropped for size on
    // the receive path (URING_RECEIVE_DROPPED_SIZE, outside PACKETS_RECEIVED), 4
    // refused by the workers' prefilter (inside it). The term is both parts; either
    // part alone (the two branches before R42) leaves a gap

    harden_terms_t t;
    t["hostile.sent"] = 7;
    t["clients.c2s_udp_out"] = 7;
    t["server.c2s_udp_in"] = 7;
    t["server.c2s_worker_refused"] = 4;
    t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 4;
    t[harden_transport_key( ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE )] = 3;
    harden_chain_t c = harden_chain_test_up( t );
    check( c.library_refused_before_received == 7 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.library_gap == 0 );
    check( harden_chain_test_handoff_gap( t ) == 0 );

    c.library_refused_before_received = 4;
    check( !harden_chain_judge( c, 0 ).ok );
    c.library_refused_before_received = 3;
    check( !harden_chain_judge( c, 0 ).ok );
}

static void test_harden_chain_refused_hostile_xdp()
{
    // xdp, --row hostile: the clients sent 1000, the hostile role 10. xdp has no
    // receive workers' totals; its kernel program dropped 5 for the filter and
    // 2 too small (XDP_KERNEL_DROPPED_*), and 3 frames were dropped before a
    // datagram was read (XDP_RECEIVE_FRAME_DROPPED), none of them in
    // PACKETS_RECEIVED; the library received the clients' 1000

    harden_terms_t t;
    t["clients.c2s_packets_sent"] = 1000;
    t["hostile.sent"] = 10;
    t["clients.c2s_udp_out"] = 1010;
    t["server.c2s_packets_received"] = 1000;
    t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 1000;
    t[harden_transport_key( ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER )] = 5;
    t[harden_transport_key( ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL )] = 2;
    t[harden_transport_key( ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED )] = 3;
    harden_chain_t c = harden_chain_test_up( t );
    check( c.library_sent == 1010 );
    check( c.library_refused_before_received == 10 );
    harden_chain_result_t r = harden_chain_judge( c, 0 );
    check( r.ok );
    check( r.library_gap == 0 );
    check( harden_chain_test_handoff_gap( t ) == 0 );

    // without the xdp terms (the term before this fix) the 10 are no named
    // drop and the chain fails a cell that lost nothing
    c.library_refused_before_received = 0;
    r = harden_chain_judge( c, 0 );
    check( !r.ok );
    check( r.library_gap == 10 );

    // a hostile datagram that no counter names still fails
    t["hostile.sent"] = 11;
    t["clients.c2s_udp_out"] = 1011;
    r = harden_chain_judge( harden_chain_test_up( t ), 0 );
    check( !r.ok );
    check( r.library_gap == 1 );
}

static void test_harden_chain_refused_from_terms_reads_the_counters()
{
    // every key the term reads: the workers' count and each outside size drop
    // counter by the library's own name. The prefilter's own size counters mix
    // both parts and are never read, and PACKETS_RECEIVED is never read into
    // the term (the received count is not subtracted from anything)

    const int outside[] = { ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE,
                            ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_SHORT,
                            ROCKETNET_COUNTER_IOCP_RECEIVE_TOO_LARGE,
                            ROCKETNET_COUNTER_RIO_RECEIVE_TOO_SHORT,
                            ROCKETNET_COUNTER_RIO_RECEIVE_TOO_LARGE,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SOCKET,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION,
                            ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SESSION,
                            ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED };
    for ( int i = 0; i < (int) ( sizeof( outside ) / sizeof( outside[0] ) ); i++ )
    {
        harden_terms_t t;
        t["server.c2s_worker_refused"] = 10;
        t["server.c2s_packets_received"] = 1000;
        t[harden_transport_key( ROCKETNET_COUNTER_PACKETS_RECEIVED )] = 5000;
        t[harden_transport_key( outside[i] )] = 3;
        t[harden_transport_key( ROCKETNET_COUNTER_PREFILTER_TOO_SHORT )] = 100;
        t[harden_transport_key( ROCKETNET_COUNTER_PREFILTER_TOO_LARGE )] = 100;
        check( harden_chain_test_up( t ).library_refused_before_received == 13 );
    }
}

REGISTER_UNIT_TEST( test_harden_chain_refused_from_terms_reads_the_counters );
REGISTER_UNIT_TEST( test_harden_chain_refused_hostile_worker_prefilter );
REGISTER_UNIT_TEST( test_harden_chain_refused_both_parts_io_uring );
REGISTER_UNIT_TEST( test_harden_chain_refused_hostile_xdp );
REGISTER_UNIT_TEST( test_harden_chain_refused_received_count_short_is_red );
REGISTER_UNIT_TEST( test_harden_chain_strays_named_pass );
REGISTER_UNIT_TEST( test_harden_chain_unnamed_strays_fail );
REGISTER_UNIT_TEST( test_harden_chain_planted_loss_fails );
REGISTER_UNIT_TEST( test_harden_chain_strays_cannot_hide_a_loss );
REGISTER_UNIT_TEST( test_harden_chain_cell_noports_and_strays_together );
REGISTER_UNIT_TEST( test_harden_chain_duplicates_row );
REGISTER_UNIT_TEST( test_harden_chain_gso_slack_bounds );
