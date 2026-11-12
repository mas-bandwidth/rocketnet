/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Exhaustive check of the production rocketnet_ack_apply_run leaf over the
    finite state the ESBMC harness proof_ack_apply_run.c describes, at run
    lengths 1..ACK_RUN_BOUND (8), and more:

      * every ring position of the run's first slot (0..4095),
      * every run length 1..8,
      * every prior-ack pattern of the run (2^len),
      * every highest-sent offset highest - low in [len - 1, 4095],
      * two neighbourhoods: the rest of the ring empty (the harness's state),
        and every other slot of the bitmap words the run touches holding a
        live, unacknowledged record of the sequence the window gives it
        (production's state; the walk must mask these out and leave them),
      * three sequence blocks: low in [1, 4096) (the start-up regime, where
        the stats window is clamped at 1), low in [4096, 8192), and the last
        block whose run and highest still fit in 64 bits.

    Each case runs the sdk leaf natively (sdk/rocketnet_ack.cpp is included,
    as the ESBMC harness includes it) and checks every assertion of the ESBMC
    harness for every sequence of the run, plus: the neighbours' bits and
    records are unchanged and their share of the eligible-unacked total
    stays. After each ring position the whole endpoint is compared with an
    all-zero one, so a write outside the run and its neighbours is caught.

    What is not enumerated is the block number between the second and the
    last: for highest >= 4096 the leaf reads sequences only through % 4096,
    differences and comparisons with highest - 4095 and highest - 2048, all
    of which a shift by a multiple of 4096 preserves. That is an argument,
    not a check. Run lengths 9..4096 are not covered.

    Controls (each must fail; proofs/cbmc/Makefile):
      -DACK_NEGATIVE_CONTROL  the visit-count check negated when a newly
                              unacked record exists, as the ESBMC control;
      -DACK_RUN_PLANT_VACUOUS no case reaches the leaf: the coverage counters
                              (cases, wrapped runs, runs straddling the
                              loss-eligibility edge, the start-up regime,
                              filled neighbourhoods, newly acked records)
                              must report VACUOUS;
      the mutants             one-line bugs sed plants in a copy of the leaf.
    -DACK_RUN_STOP_ON_FAILURE ends the run at the first failure (controls).

    Usage: ack_run_exhaustive [threads]
    Exit: 0 VERIFICATION SUCCESSFUL, 1 VERIFICATION FAILED, 3 VACUOUS.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>

#ifndef ACK_RUN_BOUND
#define ACK_RUN_BOUND 8
#endif // #ifndef ACK_RUN_BOUND

#include "rocketnet_ack.cpp"

static const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;

struct counts_t
{
    uint64_t cases = 0;
    uint64_t failures = 0;
    uint64_t wrapped = 0;
    uint64_t eligible_edge = 0;
    uint64_t startup = 0;
    uint64_t neighbours = 0;
    uint64_t newly = 0;
};

static std::atomic<uint64_t> g_reported( 0 );
static std::atomic<bool> g_stop( false );

static void report( const char * what, uint64_t low, uint64_t len, uint64_t highest, uint64_t pattern, int fill )
{
#if defined( ACK_RUN_STOP_ON_FAILURE )
    g_stop.store( true, std::memory_order_relaxed );
#endif // #if defined( ACK_RUN_STOP_ON_FAILURE )
    // a negative control fails ~1e10 times: read before the atomic add, so the
    // threads do not contend on one cache line, and count in uint64_t, which
    // does not wrap
    if ( g_reported.load( std::memory_order_relaxed ) < 10 && g_reported.fetch_add( 1 ) < 10 )
    {
        printf( "FAILED %s: low=%llu len=%llu highest=%llu pattern=0x%llx neighbours=%d\n", what, (unsigned long long) low,
                (unsigned long long) len, (unsigned long long) highest, (unsigned long long) pattern, fill );
    }
}

static inline bool bit( const uint64_t * words, uint64_t slot )
{
    return ( ( words[slot >> 6] >> ( slot & 63 ) ) & 1ULL ) != 0;
}

static void run_block( uint64_t block, uint32_t c_begin, uint32_t c_end, counts_t * out )
{
    struct rocketnet_ack_endpoint_t * endpoint = (struct rocketnet_ack_endpoint_t *) calloc( 1, sizeof( *endpoint ) );
    struct rocketnet_ack_endpoint_t * zero = (struct rocketnet_ack_endpoint_t *) calloc( 1, sizeof( *zero ) );
    if ( !endpoint || !zero )
    {
        printf( "out of memory\n" );
        exit( 2 );
    }
    counts_t k;

    for ( uint32_t c = c_begin; c < c_end && !g_stop.load( std::memory_order_relaxed ); ++c )
    {
        const uint64_t low = block * N + c;
        if ( low < 1 )
        {
            continue;
        }
        for ( uint64_t len = 1; len <= ACK_RUN_BOUND; ++len )
        {
            const uint64_t high = low + len - 1;
            const uint64_t word_lo = ( low % N ) >> 6;
            const uint64_t word_hi = ( high % N ) >> 6;
            uint64_t run_mask[2] = { 0, 0 }; // run bits in word_lo, word_hi
            for ( uint64_t i = 0; i < len; ++i )
            {
                const uint64_t slot = ( low + i ) % N;
                run_mask[( slot >> 6 ) == word_lo ? 0 : 1] |= 1ULL << ( slot & 63 );
            }
            for ( uint64_t h = len - 1; h < N && !g_stop.load( std::memory_order_relaxed ); ++h )
            {
                const uint64_t highest = low + h;
                if ( highest < low )
                {
                    continue; // past 2^64
                }
                const uint64_t eligible_top = rocketnet_ack_stats_eligible_top( highest );
                for ( int fill = 0; fill < 2; ++fill )
                {
                    // the neighbours: every other slot of the run's words holds
                    // a live, unacknowledged record of the sequence the window
                    // [highest - 4095, highest] maps to it
                    uint64_t neighbour_words[2] = { 0, 0 };
                    uint64_t neighbour_eligible_unacked = 0;
                    if ( fill )
                    {
                        const uint64_t words[2] = { word_lo, word_hi };
                        for ( int w = 0; w < ( word_lo == word_hi ? 1 : 2 ); ++w )
                        {
                            for ( uint64_t b = 0; b < 64; ++b )
                            {
                                if ( ( run_mask[w] >> b ) & 1ULL )
                                {
                                    continue;
                                }
                                const uint64_t slot = words[w] * 64 + b;
                                const uint64_t back = ( highest % N + N - slot ) % N;
                                if ( back >= highest )
                                {
                                    continue; // that sequence would be below 1
                                }
                                const uint64_t sequence = highest - back;
                                endpoint->sent[slot].sequence = sequence;
                                neighbour_words[w] |= 1ULL << b;
                                if ( sequence <= eligible_top )
                                {
                                    neighbour_eligible_unacked++;
                                }
                            }
                            endpoint->sent_written[words[w]] |= neighbour_words[w];
                            endpoint->sent_held[words[w]] |= neighbour_words[w];
                            endpoint->sent_unacked[words[w]] |= neighbour_words[w];
                        }
                    }

                    for ( uint64_t pattern = 0; pattern < ( 1ULL << len ); ++pattern )
                    {
#if defined( ACK_RUN_PLANT_VACUOUS )
                        // the vacuity plant: no case reaches the leaf; the
                        // coverage counters must then report VACUOUS
                        continue;
#endif // #if defined( ACK_RUN_PLANT_VACUOUS )
                        uint64_t expected_new = 0;
                        uint64_t expected_largest = 0;
                        uint64_t eligible_unacked = 0;
                        bool any_eligible = false;
                        bool any_ineligible = false;

                        endpoint->highest_sequence = highest;
                        for ( uint64_t i = 0; i < len; ++i )
                        {
                            const uint64_t sequence = low + i;
                            const uint64_t slot = sequence % N;
                            const uint8_t already_acked = ( pattern >> i ) & 1U;
                            endpoint->sent[slot].sequence = sequence;
                            endpoint->sent[slot].acked = already_acked;
                            endpoint->sent_written[slot >> 6] |= 1ULL << ( slot & 63 );
                            endpoint->sent_held[slot >> 6] |= 1ULL << ( slot & 63 );
                            if ( sequence <= eligible_top )
                            {
                                any_eligible = true;
                            }
                            else
                            {
                                any_ineligible = true;
                            }
                            if ( !already_acked )
                            {
                                endpoint->sent_unacked[slot >> 6] |= 1ULL << ( slot & 63 );
                                expected_new++;
                                expected_largest = sequence;
                                if ( sequence <= eligible_top )
                                {
                                    eligible_unacked++;
                                }
                            }
                        }
                        endpoint->stats_eligible_unacked = (uint32_t) ( neighbour_eligible_unacked + eligible_unacked );
                        g_ack_visit_count = 0;

                        uint64_t acked_packet_ids[1] = { 0 };
                        int acked_count = 0;
                        uint64_t unknown_sequence_counter = 0;
                        uint64_t batch_full_counter = 0;
                        const int newly_acked = rocketnet_ack_apply_run( endpoint, low, high, -1.0, acked_packet_ids, 1, &acked_count,
                                                                         &unknown_sequence_counter, &batch_full_counter );

                        k.cases++;
                        k.newly += (uint64_t) newly_acked;
                        if ( low % N > high % N )
                        {
                            k.wrapped++;
                        }
                        if ( any_eligible && any_ineligible )
                        {
                            k.eligible_edge++;
                        }
                        if ( highest < N )
                        {
                            k.startup++;
                        }
                        if ( neighbour_words[0] | neighbour_words[1] )
                        {
                            k.neighbours++;
                        }

                        bool ok = true;
#define CHECK( condition, what )                                    \
    if ( !( condition ) )                                           \
    {                                                               \
        ok = false;                                                 \
        report( what, low, len, highest, pattern, fill );           \
    }
                        CHECK( (uint64_t) newly_acked == expected_new, "the production ACK run neither skips nor double-counts" );
                        CHECK( unknown_sequence_counter == 0, "every held sequence remains known" );
                        CHECK( batch_full_counter == 0, "records without packet ids never defer" );
                        CHECK( acked_count == 0, "records without packet ids surface no id" );
                        CHECK( endpoint->stats_eligible_unacked == neighbour_eligible_unacked,
                               "every eligible unacked record of the run, and only those, leaves the running total" );
#if defined( ACK_NEGATIVE_CONTROL )
                        if ( expected_new > 0 )
                        {
                            CHECK( g_ack_visit_count == 0, "negative control reaches the production ACK operation" );
                        }
#else
                        CHECK( g_ack_visit_count == expected_new, "the production walk visits each initially-unacked sequence once" );
#endif // #if defined( ACK_NEGATIVE_CONTROL )
                        CHECK( endpoint->largest_acked_sequence == expected_largest,
                               "largest_acked_sequence names the newest newly acked sequence" );
                        for ( uint64_t i = 0; i < len; ++i )
                        {
                            const uint64_t sequence = low + i;
                            const uint64_t slot = sequence % N;
                            CHECK( endpoint->sent[slot].sequence == sequence, "the ACK walk does not overwrite a live sent record" );
                            CHECK( endpoint->sent[slot].acked == 1, "every sequence in the run ends acked" );
                            CHECK( bit( endpoint->sent_written, slot ), "an ACK does not erase the written identity" );
                            CHECK( bit( endpoint->sent_held, slot ), "an ACK does not erase the held identity" );
                            CHECK( !bit( endpoint->sent_unacked, slot ), "an ACK clears the unacked identity" );
                        }
                        CHECK( ( endpoint->sent_unacked[word_lo] & ~run_mask[0] ) == neighbour_words[0] &&
                                   ( endpoint->sent_unacked[word_hi] & ~run_mask[word_lo == word_hi ? 0 : 1] ) ==
                                       neighbour_words[word_lo == word_hi ? 0 : 1],
                               "the ACK walk leaves the neighbours unacknowledged" );
#undef CHECK
                        if ( !ok )
                        {
                            k.failures++;
                        }

                        // restore: the run's records, bits and the counters
                        for ( uint64_t i = 0; i < len; ++i )
                        {
                            const uint64_t slot = ( low + i ) % N;
                            const uint64_t clear = ~( 1ULL << ( slot & 63 ) );
                            memset( &endpoint->sent[slot], 0, sizeof( endpoint->sent[slot] ) );
                            endpoint->sent_written[slot >> 6] &= clear;
                            endpoint->sent_held[slot >> 6] &= clear;
                            endpoint->sent_unacked[slot >> 6] &= clear;
                        }
                        endpoint->sent_unacked[word_lo] |= neighbour_words[0];
                        endpoint->sent_unacked[word_hi] |= neighbour_words[word_lo == word_hi ? 0 : 1];
                        endpoint->highest_sequence = 0;
                        endpoint->largest_acked_sequence = 0;
                        endpoint->stats_eligible_unacked = 0;
                        endpoint->stats_acked_count = 0;
                        endpoint->stats_acked_bytes = 0;
                        endpoint->stats_acked_low = 0;
                        endpoint->stats_acked_high = 0;
                        endpoint->stats_acked_low_time = 0.0;
                        endpoint->stats_acked_high_time = 0.0;
                        endpoint->keep_alive_acked = 0;
                    }

                    // the neighbours' records are untouched; then clear them
                    if ( fill )
                    {
                        const uint64_t words[2] = { word_lo, word_hi };
                        for ( int w = 0; w < ( word_lo == word_hi ? 1 : 2 ); ++w )
                        {
                            for ( uint64_t b = 0; b < 64; ++b )
                            {
                                if ( ( neighbour_words[w] >> b ) & 1ULL )
                                {
                                    const uint64_t slot = words[w] * 64 + b;
                                    if ( endpoint->sent[slot].acked != 0 )
                                    {
                                        report( "the ACK walk leaves the neighbours' records unacknowledged", low, len, highest, 0, fill );
                                        k.failures++;
                                    }
                                    memset( &endpoint->sent[slot], 0, sizeof( endpoint->sent[slot] ) );
                                }
                            }
                            endpoint->sent_written[words[w]] = 0;
                            endpoint->sent_held[words[w]] = 0;
                            endpoint->sent_unacked[words[w]] = 0;
                        }
                    }
                }
            }
        }
        if ( memcmp( endpoint, zero, sizeof( *endpoint ) ) != 0 )
        {
            report( "the leaf wrote state outside the run and its neighbours", low, 0, 0, 0, -1 );
            k.failures++;
            memset( endpoint, 0, sizeof( *endpoint ) );
        }
    }

    free( endpoint );
    free( zero );
    *out = k;
}

int main( int argc, char ** argv )
{
    int threads = argc > 1 ? atoi( argv[1] ) : 1;
    if ( threads < 1 )
    {
        threads = 1;
    }
    const uint64_t last_block = ( UINT64_MAX - 2 * N ) / N;
    const uint64_t blocks[3] = { 0, 1, last_block };

    counts_t total;
    for ( uint64_t b = 0; b < 3; ++b )
    {
        std::vector<counts_t> parts( (size_t) threads );
        std::vector<std::thread> workers;
        for ( int t = 0; t < threads; ++t )
        {
            const uint32_t begin = (uint32_t) ( N * (uint64_t) t / (uint64_t) threads );
            const uint32_t end = (uint32_t) ( N * (uint64_t) ( t + 1 ) / (uint64_t) threads );
            workers.emplace_back( run_block, blocks[b], begin, end, &parts[(size_t) t] );
        }
        for ( auto & w : workers )
        {
            w.join();
        }
        counts_t k;
        for ( const auto & p : parts )
        {
            k.cases += p.cases;
            k.failures += p.failures;
            k.wrapped += p.wrapped;
            k.eligible_edge += p.eligible_edge;
            k.startup += p.startup;
            k.neighbours += p.neighbours;
            k.newly += p.newly;
        }
        printf( "block %llu: cases=%llu failures=%llu wrapped=%llu eligible_edge=%llu startup=%llu neighbours=%llu newly_acked=%llu\n",
                (unsigned long long) blocks[b], (unsigned long long) k.cases, (unsigned long long) k.failures,
                (unsigned long long) k.wrapped, (unsigned long long) k.eligible_edge, (unsigned long long) k.startup,
                (unsigned long long) k.neighbours, (unsigned long long) k.newly );
        total.cases += k.cases;
        total.failures += k.failures;
        total.wrapped += k.wrapped;
        total.eligible_edge += k.eligible_edge;
        total.startup += k.startup;
        total.neighbours += k.neighbours;
        total.newly += k.newly;
    }

    printf( "total: cases=%llu failures=%llu wrapped=%llu eligible_edge=%llu startup=%llu neighbours=%llu newly_acked=%llu\n",
            (unsigned long long) total.cases, (unsigned long long) total.failures, (unsigned long long) total.wrapped,
            (unsigned long long) total.eligible_edge, (unsigned long long) total.startup, (unsigned long long) total.neighbours,
            (unsigned long long) total.newly );

    if ( total.failures != 0 )
    {
        printf( "VERIFICATION FAILED\n" );
        return 1;
    }
    if ( total.cases == 0 || total.wrapped == 0 || total.eligible_edge == 0 || total.startup == 0 || total.neighbours == 0 ||
         total.newly == 0 )
    {
        printf( "VACUOUS: a coverage counter is zero\n" );
        return 3;
    }
    printf( "VERIFICATION SUCCESSFUL\n" );
    return 0;
}
