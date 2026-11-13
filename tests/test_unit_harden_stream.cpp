/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load receive stream (tests/load/harden_stream.h): planted
// sequences on one unreliable stream, and the counts each must leave. No
// count goes below zero, and nothing is counted delivered twice. A reliable
// stream starts at 0 on each connection, and an earlier connection's message
// is an error, never a new stream. Pure bookkeeping: no time, no sockets.

#include "test_unit_registry.h"

#include "load/harden_stream.h"

#include <stdint.h>

#define check rocketnet_unit_check

static void harden_stream_test_take( harden_stream_t * stream, harden_verify_t * v, uint32_t connection, uint32_t sequence )
{
    harden_message_t m;
    m.kind = HARDEN_KIND_BURST;
    m.client_id = 1;
    m.connection = connection;
    m.sequence = sequence;
    m.bytes = 0;
    harden_stream_take( stream, &m, false, v );
}

static void test_harden_stream_reorder_takes_back_lost()
{
    // 0, 2, 1: the gap at 1 is counted lost, then taken back when it arrives

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take( &s, &v, 1, 0 );
    harden_stream_test_take( &s, &v, 1, 2 );
    check( v.lost[HARDEN_KIND_BURST] == 1 );
    harden_stream_test_take( &s, &v, 1, 1 );
    check( v.lost[HARDEN_KIND_BURST] == 0 );
    check( v.delivered[HARDEN_KIND_BURST] == 3 );
    check( v.reordered[HARDEN_KIND_BURST] == 1 );
    check( v.errors == 0 );

    // its second arrival is a duplicate

    harden_stream_test_take( &s, &v, 1, 1 );
    check( v.duplicates == 1 );
    check( v.errors == 1 );
    check( v.delivered[HARDEN_KIND_BURST] == 3 );
}

static void test_harden_stream_late_below_first_takes_back_head()
{
    // the connection's first message taken is 5: 0..4 are counted in head.
    // 3 arrives late: it is taken back from head, and lost (never given)
    // stays 0, not 2^64 - 1

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take( &s, &v, 1, 5 );
    check( v.head[HARDEN_KIND_BURST] == 5 );
    harden_stream_test_take( &s, &v, 1, 3 );
    check( v.lost[HARDEN_KIND_BURST] == 0 );
    check( v.head[HARDEN_KIND_BURST] == 4 );
    check( v.delivered[HARDEN_KIND_BURST] == 2 );
    check( v.reordered[HARDEN_KIND_BURST] == 1 );
    check( v.errors == 0 );
}

static void test_harden_stream_past_window_moves_nothing()
{
    // 0 delivered, then 2000: 1..1999 counted lost. 0 again, older than the
    // window, may be a duplicate: it is named late_past_window, and neither
    // takes a lost back nor counts delivered

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take( &s, &v, 1, 0 );
    harden_stream_test_take( &s, &v, 1, 2000 );
    check( v.lost[HARDEN_KIND_BURST] == 1999 );
    harden_stream_test_take( &s, &v, 1, 0 );
    check( v.late_past_window[HARDEN_KIND_BURST] == 1 );
    check( v.lost[HARDEN_KIND_BURST] == 1999 );
    check( v.delivered[HARDEN_KIND_BURST] == 2 );
    check( v.delivered[HARDEN_KIND_BURST] <= 2001 - v.lost[HARDEN_KIND_BURST] );

    // the same below the connection's first: first 5000, then 10. lost was
    // never given (0), and stays 0

    harden_stream_t t;
    harden_verify_t w;
    harden_stream_test_take( &t, &w, 1, 5000 );
    harden_stream_test_take( &t, &w, 1, 10 );
    check( w.late_past_window[HARDEN_KIND_BURST] == 1 );
    check( w.lost[HARDEN_KIND_BURST] == 0 );
    check( w.head[HARDEN_KIND_BURST] == 5000 );
    check( w.delivered[HARDEN_KIND_BURST] == 1 );
}

static void test_harden_stream_counts_never_wrap()
{
    // a connection whose first message is 1500, then every earlier sequence
    // in reverse: the 1023 within the window (477..1499, expected being 1501)
    // are taken back from head, the 477 older are past the window. No count
    // goes below zero, delivered never exceeds what was sent

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take( &s, &v, 7, 1500 );
    for ( uint32_t i = 1500; i > 0; i-- )
    {
        harden_stream_test_take( &s, &v, 7, i - 1 );
    }
    check( v.lost[HARDEN_KIND_BURST] == 0 );
    check( v.head[HARDEN_KIND_BURST] == 477 );
    check( v.late_past_window[HARDEN_KIND_BURST] == 477 );
    check( v.delivered[HARDEN_KIND_BURST] == 1 + 1023 );
    check( v.delivered[HARDEN_KIND_BURST] <= 1501 );
    check( v.errors == 0 );
}

static void harden_stream_test_take_reliable( harden_stream_t * stream, harden_verify_t * v, uint64_t client_id, uint32_t connection, uint32_t sequence )
{
    harden_message_t m;
    m.kind = HARDEN_KIND_RELIABLE;
    m.client_id = client_id;
    m.connection = connection;
    m.sequence = sequence;
    m.bytes = 0;
    harden_stream_take( stream, &m, true, v );
}

static void test_harden_stream_reliable_starts_at_zero()
{
    // a reliable ordered stream delivers its connection's messages in order
    // from the first: 0, 1, 2 on one connection is clean

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take_reliable( &s, &v, 1, 1, 0 );
    harden_stream_test_take_reliable( &s, &v, 1, 1, 1 );
    harden_stream_test_take_reliable( &s, &v, 1, 1, 2 );
    check( v.errors == 0 );
    check( v.delivered[HARDEN_KIND_RELIABLE] == 3 );

    // planted: the connection's first two lost, the first delivered is 2. The
    // missing head is a gap and an error, not a count in head

    harden_stream_t t;
    harden_verify_t w;
    harden_stream_test_take_reliable( &t, &w, 1, 1, 2 );
    check( w.reliable_gaps == 1 );
    check( w.errors == 1 );

    // the next connection (a churner back) starts again at 0: clean

    harden_stream_test_take_reliable( &s, &v, 1, 2, 0 );
    harden_stream_test_take_reliable( &s, &v, 1, 2, 1 );
    check( v.errors == 0 );
    check( v.delivered[HARDEN_KIND_RELIABLE] == 5 );
}

static void test_harden_stream_old_connection_is_an_error()
{
    // client 1 on connection 5, then on connection 6 (it reconnected). A
    // message of connection 5 arriving now is an old session's body: an
    // error, and the stream stays on connection 6, so 6's next message is
    // in order

    harden_stream_t s;
    harden_verify_t v;
    harden_stream_test_take_reliable( &s, &v, 1, 5, 0 );
    harden_stream_test_take_reliable( &s, &v, 1, 5, 1 );
    harden_stream_test_take_reliable( &s, &v, 1, 6, 0 );
    check( v.errors == 0 );
    harden_stream_test_take_reliable( &s, &v, 1, 5, 2 );
    check( v.old_connection == 1 );
    check( v.errors == 1 );
    check( s.connection == 6 );
    harden_stream_test_take_reliable( &s, &v, 1, 6, 1 );
    check( v.errors == 1 );
    check( v.reliable_gaps == 0 && v.back == 0 );
    check( v.delivered[HARDEN_KIND_RELIABLE] == 4 );

    // on an unreliable stream the same: connection 5's late message is an
    // error, never a new stream

    harden_stream_t u;
    harden_verify_t x;
    harden_stream_test_take( &u, &x, 6, 0 );
    harden_stream_test_take( &u, &x, 5, 9 );
    check( x.old_connection == 1 );
    check( x.errors == 1 );
    check( x.head[HARDEN_KIND_BURST] == 0 );

    // another client taking a server slot's stream is a new owner, whatever
    // its connection number: a new stream, no error

    harden_stream_t o;
    harden_verify_t y;
    harden_stream_test_take_reliable( &o, &y, 1, 7, 0 );
    harden_stream_test_take_reliable( &o, &y, 2, 3, 0 );
    harden_stream_test_take_reliable( &o, &y, 2, 3, 1 );
    check( y.errors == 0 );
    check( o.client_id == 2 && o.connection == 3 );

    // and with the same connection number (each client counts its own
    // connects, so two clients' first connections are both 1): client 2 in
    // client 1's old slot starts a new stream at 0, not a step back

    harden_stream_t p;
    harden_verify_t z;
    harden_stream_test_take_reliable( &p, &z, 1, 1, 0 );
    harden_stream_test_take_reliable( &p, &z, 1, 1, 1 );
    harden_stream_test_take_reliable( &p, &z, 2, 1, 0 );
    check( z.errors == 0 && z.back == 0 );
    check( p.client_id == 2 );
}

REGISTER_UNIT_TEST( test_harden_stream_reliable_starts_at_zero );
REGISTER_UNIT_TEST( test_harden_stream_old_connection_is_an_error );
REGISTER_UNIT_TEST( test_harden_stream_reorder_takes_back_lost );
REGISTER_UNIT_TEST( test_harden_stream_late_below_first_takes_back_head );
REGISTER_UNIT_TEST( test_harden_stream_past_window_moves_nothing );
REGISTER_UNIT_TEST( test_harden_stream_counts_never_wrap );
