/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The harden_load receive stream (tests/load/harden_stream.h): planted
// sequences on one unreliable stream, and the counts each must leave. No
// count goes below zero, and nothing is counted delivered twice. Pure
// bookkeeping: no time, no sockets.

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

REGISTER_UNIT_TEST( test_harden_stream_reorder_takes_back_lost );
REGISTER_UNIT_TEST( test_harden_stream_late_below_first_takes_back_head );
REGISTER_UNIT_TEST( test_harden_stream_past_window_moves_nothing );
REGISTER_UNIT_TEST( test_harden_stream_counts_never_wrap );
