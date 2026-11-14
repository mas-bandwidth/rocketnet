/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The red-zone walk (rocketnet_shadow_walk in sdk/rocketnet_memory.h), which
// rocketnet_red_zone_unpoison takes over a block at free in an ASan build,
// asks its run query once per run of shadow and asks about exactly the spans
// the walk before it asked about. The walk before it is kept here as the
// oracle: it queried at every span on Windows, skipped a run not committed,
// and asked about the span otherwise. Both walk the same made-up shadow, a
// list of runs each committed, not committed, or not known (a query that
// fails), and the spans each asks about must be the same, in the same order.
//
// No OS call, no sanitizer: the shadow is a table, so the cases run in every
// build.

#include "test_unit_registry.h"

#include "rocketnet_memory.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

#define WALK_SPAN ( (uintptr_t) ROCKETNET_RED_ZONE_SHADOW_PAGE_SPAN )
#define WALK_BASE ( (uintptr_t) 0x40000000 )
#define WALK_MAX_RUNS 64
#define WALK_MAX_SPANS 4096

enum walk_state_t
{
    WALK_COMMITTED,
    WALK_NOT_COMMITTED,
    WALK_NOT_KNOWN,
};

struct walk_run_t
{
    uintptr_t begin;
    uintptr_t end;
    walk_state_t state;
};

struct walk_shadow_t
{
    walk_run_t runs[WALK_MAX_RUNS];
    int num_runs;
    uintptr_t spans[WALK_MAX_SPANS][2];
    int num_spans;
    int queries;
};

static void walk_shadow_reset( walk_shadow_t * shadow )
{
    shadow->num_spans = 0;
    shadow->queries = 0;
}

static void walk_add_run( walk_shadow_t * shadow, uintptr_t begin, uintptr_t end, walk_state_t state )
{
    check( shadow->num_runs < WALK_MAX_RUNS );
    shadow->runs[shadow->num_runs].begin = begin;
    shadow->runs[shadow->num_runs].end = end;
    shadow->runs[shadow->num_runs].state = state;
    shadow->num_runs++;
}

// the run that holds at; an address outside every run is not known
static const walk_run_t * walk_find( const walk_shadow_t * shadow, uintptr_t at )
{
    for ( int i = 0; i < shadow->num_runs; i++ )
    {
        if ( at >= shadow->runs[i].begin && at < shadow->runs[i].end )
        {
            return &shadow->runs[i];
        }
    }
    return NULL;
}

static uintptr_t walk_run( void * context, uintptr_t at, bool * committed )
{
    walk_shadow_t * shadow = (walk_shadow_t *) context;
    shadow->queries++;
    const walk_run_t * run = walk_find( shadow, at );
    *committed = true;
    if ( !run || run->state == WALK_NOT_KNOWN )
    {
        return at;
    }
    *committed = run->state == WALK_COMMITTED;
    return run->end;
}

static void walk_span( void * context, uintptr_t begin, uintptr_t end )
{
    walk_shadow_t * shadow = (walk_shadow_t *) context;
    check( shadow->num_spans < WALK_MAX_SPANS );
    check( begin < end );
    shadow->spans[shadow->num_spans][0] = begin;
    shadow->spans[shadow->num_spans][1] = end;
    shadow->num_spans++;
}

// the walk before rocketnet_shadow_walk, as it was on Windows: at every span,
// the end of the run not committed that holds it, or at itself when the run
// is committed or the query fails
static void walk_oracle( walk_shadow_t * shadow, uintptr_t begin, uintptr_t end )
{
    uintptr_t at = begin;
    while ( at < end )
    {
        const walk_run_t * run = walk_find( shadow, at );
        shadow->queries++;
        const uintptr_t uncommitted_end = run && run->state == WALK_NOT_COMMITTED ? run->end : at;
        if ( uncommitted_end > at )
        {
            at = uncommitted_end < end ? uncommitted_end : end;
            continue;
        }
        const uintptr_t span_end = ( at | ( WALK_SPAN - 1 ) ) + 1;
        const uintptr_t next = span_end < end ? span_end : end;
        walk_span( shadow, at, next );
        at = next;
    }
}

// the oracle and the walk ask about the same spans; returns the walk's queries
static int walk_compare( walk_shadow_t * shadow, uintptr_t begin, uintptr_t end )
{
    walk_shadow_reset( shadow );
    walk_oracle( shadow, begin, end );
    const int oracle_spans = shadow->num_spans;
    static uintptr_t oracle[WALK_MAX_SPANS][2];
    memcpy( oracle, shadow->spans, sizeof( oracle[0] ) * (size_t) oracle_spans );

    walk_shadow_reset( shadow );
    rocketnet_shadow_walk( begin, end, walk_run, walk_span, shadow );
    check( shadow->num_spans == oracle_spans );
    for ( int i = 0; i < oracle_spans && i < shadow->num_spans; i++ )
    {
        check( shadow->spans[i][0] == oracle[i][0] );
        check( shadow->spans[i][1] == oracle[i][1] );
    }
    return shadow->queries;
}

// a shadow of runs of the given lengths in spans from WALK_BASE, states in turn
static void walk_layout( walk_shadow_t * shadow, const int * lengths, const walk_state_t * states, int num_runs )
{
    shadow->num_runs = 0;
    uintptr_t at = WALK_BASE;
    for ( int i = 0; i < num_runs; i++ )
    {
        walk_add_run( shadow, at, at + (uintptr_t) lengths[i] * WALK_SPAN, states[i] );
        at += (uintptr_t) lengths[i] * WALK_SPAN;
    }
}

static walk_shadow_t g_walk_shadow;

// all committed: every span asked, one query for the whole block
static void test_red_zone_walk_all_committed()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    const int lengths[] = { 100 };
    const walk_state_t states[] = { WALK_COMMITTED };
    walk_layout( shadow, lengths, states, 1 );
    check( walk_compare( shadow, WALK_BASE + 4 * WALK_SPAN, WALK_BASE + 68 * WALK_SPAN ) == 1 );
    check( shadow->num_spans == 64 );
}

// none committed: nothing asked, one query
static void test_red_zone_walk_none_committed()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    const int lengths[] = { 100 };
    const walk_state_t states[] = { WALK_NOT_COMMITTED };
    walk_layout( shadow, lengths, states, 1 );
    check( walk_compare( shadow, WALK_BASE + 4 * WALK_SPAN, WALK_BASE + 68 * WALK_SPAN ) == 1 );
    check( shadow->num_spans == 0 );
}

// alternating runs of one span and of several spans: one query per run
static void test_red_zone_walk_alternating()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    int lengths[40];
    walk_state_t states[40];
    for ( int i = 0; i < 40; i++ )
    {
        lengths[i] = 1;
        states[i] = i % 2 ? WALK_NOT_COMMITTED : WALK_COMMITTED;
    }
    walk_layout( shadow, lengths, states, 40 );
    check( walk_compare( shadow, WALK_BASE, WALK_BASE + 40 * WALK_SPAN ) == 40 );
    check( shadow->num_spans == 20 );

    for ( int i = 0; i < 40; i++ )
    {
        lengths[i] = i % 2 ? 2 : 3;
    }
    walk_layout( shadow, lengths, states, 40 );
    check( walk_compare( shadow, WALK_BASE, WALK_BASE + 100 * WALK_SPAN ) == 40 );
    check( shadow->num_spans == 60 );
}

// a committed run that ends inside the block, then a run not committed, then
// committed again past the block's end
static void test_red_zone_walk_run_ends_mid_block()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    const int lengths[] = { 11, 30, 40 };
    const walk_state_t states[] = { WALK_COMMITTED, WALK_NOT_COMMITTED, WALK_COMMITTED };
    walk_layout( shadow, lengths, states, 3 );
    check( walk_compare( shadow, WALK_BASE + WALK_SPAN, WALK_BASE + 50 * WALK_SPAN ) == 3 );
    check( shadow->num_spans == 10 + 9 );
}

// a block not aligned to a span at either end, over every layout above
static void test_red_zone_walk_unaligned_block()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    const int lengths[] = { 3, 1, 2, 5, 1, 1, 4 };
    const walk_state_t states[] = { WALK_COMMITTED, WALK_NOT_COMMITTED, WALK_COMMITTED, WALK_NOT_COMMITTED,
                                    WALK_COMMITTED, WALK_NOT_COMMITTED, WALK_COMMITTED };
    walk_layout( shadow, lengths, states, 7 );
    const uintptr_t begins[] = { WALK_BASE + 100, WALK_BASE + WALK_SPAN - 1, WALK_BASE + 3 * WALK_SPAN + 7 };
    const uintptr_t ends[] = { WALK_BASE + 16 * WALK_SPAN + 777, WALK_BASE + 4 * WALK_SPAN + 1, WALK_BASE + 3 * WALK_SPAN + 9 };
    for ( int b = 0; b < 3; b++ )
    {
        for ( int e = 0; e < 3; e++ )
        {
            if ( begins[b] < ends[e] )
            {
                (void) walk_compare( shadow, begins[b], ends[e] );
            }
        }
    }
    (void) walk_compare( shadow, WALK_BASE + 100, WALK_BASE + 16 * WALK_SPAN + 777 );
    check( shadow->spans[0][0] == WALK_BASE + 100 );
    check( shadow->spans[0][1] == WALK_BASE + WALK_SPAN );
    check( shadow->spans[shadow->num_spans - 1][1] == WALK_BASE + 16 * WALK_SPAN + 777 );
}

// a query that fails asks about one span, then queries again
static void test_red_zone_walk_query_not_known()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    const int lengths[] = { 2, 3, 2, 3 };
    const walk_state_t states[] = { WALK_COMMITTED, WALK_NOT_KNOWN, WALK_NOT_COMMITTED, WALK_NOT_KNOWN };
    walk_layout( shadow, lengths, states, 4 );
    check( walk_compare( shadow, WALK_BASE, WALK_BASE + 12 * WALK_SPAN ) == 1 + 3 + 1 + 3 + 2 );
    check( shadow->num_spans == 2 + 3 + 3 + 2 );
}

// a build with no query (every build but Windows): the run is the whole
// address space and every span is asked, as before
static uintptr_t walk_run_everything( void *, uintptr_t, bool * committed )
{
    *committed = true;
    return UINTPTR_MAX;
}

static void test_red_zone_walk_no_query()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    walk_shadow_reset( shadow );
    rocketnet_shadow_walk( WALK_BASE + 5, WALK_BASE + 9 * WALK_SPAN + 3, walk_run_everything, walk_span, shadow );
    check( shadow->num_spans == 10 );
    uintptr_t at = WALK_BASE + 5;
    for ( int i = 0; i < shadow->num_spans; i++ )
    {
        check( shadow->spans[i][0] == at );
        at = shadow->spans[i][1];
    }
    check( at == WALK_BASE + 9 * WALK_SPAN + 3 );
}

// runs of made-up lengths, some ending inside a span, in every state, and
// blocks with made-up ends over them: the same spans every time
static void test_red_zone_walk_made_up_layouts()
{
    walk_shadow_t * shadow = &g_walk_shadow;
    uint32_t seed = 0x2545f491u;
    for ( int layout = 0; layout < 2000; layout++ )
    {
        shadow->num_runs = 0;
        uintptr_t at = WALK_BASE;
        const int num_runs = 1 + (int) ( seed % 12 );
        for ( int i = 0; i < num_runs; i++ )
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const uintptr_t length = ( seed % 4 == 0 ) ? 1 + seed % ( 3 * WALK_SPAN ) : ( 1 + seed % 5 ) * WALK_SPAN;
            walk_add_run( shadow, at, at + length, (walk_state_t) ( ( seed >> 8 ) % 3 ) );
            at += length;
        }
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const uintptr_t extent = at - WALK_BASE;
        const uintptr_t begin = WALK_BASE + seed % extent;
        const uintptr_t end = begin + 1 + ( seed >> 7 ) % ( at + WALK_SPAN - begin );
        (void) walk_compare( shadow, begin, end );
    }
}

REGISTER_UNIT_TEST( test_red_zone_walk_all_committed );
REGISTER_UNIT_TEST( test_red_zone_walk_alternating );
REGISTER_UNIT_TEST( test_red_zone_walk_made_up_layouts );
REGISTER_UNIT_TEST( test_red_zone_walk_no_query );
REGISTER_UNIT_TEST( test_red_zone_walk_none_committed );
REGISTER_UNIT_TEST( test_red_zone_walk_query_not_known );
REGISTER_UNIT_TEST( test_red_zone_walk_run_ends_mid_block );
REGISTER_UNIT_TEST( test_red_zone_walk_unaligned_block );
