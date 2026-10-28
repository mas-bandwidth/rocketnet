/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_network_simulator.h"

#if ROCKETNET_DEV
#include "rocketnet_protocol.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>

/*
    THE SELECTION RULE, WHICH IS THE WIRE'S BEHAVIOUR AND DOES NOT CHANGE

    receive( to, now ) delivers the packet in the LOWEST OCCUPIED SLOT INDEX
    that is due ( delivery_time <= now ) and addressed to `to`. send() places
    a packet in the LOWEST FREE SLOT INDEX.

    Those two sentences are the whole observable contract, and they were the
    contract before this file carried an index too: the original receive()
    scanned the packet array from slot 0 and returned the first slot passing
    those three predicates, and the original insert scanned from slot 0 and
    took the first free one. Slot-index order is not delivery-time order and
    it is not FIFO -- it is an artifact of the array walk -- but it is the
    order every soak and every functional test has run against, so it is
    preserved exactly rather than improved. A faster instrument that delivers
    a different packet sequence is a different instrument, and the numbers
    taken through it are not comparable with the numbers taken before it.

    What changed (#99) is only the COST of finding that slot. receive() used
    to walk all `capacity` slots on every call: at soak_sim's shape that is
    a 4096-entry scan nine times per iteration, most of them finding nothing,
    and it held 38.6% of soak cycles on the test box -- harness, not product.

    The index: each destination shard owns a flat bitmap of occupied physical
    slots and a summary bitmap over its nonempty words. The shard is a hash of
    the destination address, so all slots addressed to a given destination
    live in exactly one shard. Walking summary words, occupancy words and bits
    from low to high visits every candidate in the same ascending physical-slot
    order the full array walk did. The address predicate still runs because a
    shard may hold more than one destination. Same visits, same order, same
    winner -- no pointer ownership and no insertion walk.

    Two further aids, both exact, and neither one constant-cost -- free_hint's
    scan is O(the gap it skips) and the bound's refresh is O(the entries it
    walked). They are cheap in every shape this file meets, which is not the
    same claim:

      - free_hint: the lowest slot index that may be free. Every slot below it
        is occupied, so insert's scan starts there instead of at 0 and still
        finds the globally lowest free slot.

      - shard_min_delivery_time: a LOWER BOUND on the earliest delivery time
        in the shard. If that bound is in the future nothing in the shard can
        be due and receive() returns 0 without walking at all -- which is the
        common case, because the soak's drain loop calls receive() until it
        comes back empty. The bound is lowered on insert and refreshed to the
        exact minimum by any walk that ends up proving nothing was due, which
        is the walk that had to look at everything anyway. Removals leave it
        stale-low, which costs a walk and can never hide a due packet.

    THE FAILURE STATE

    An instrument with no failure state is not an instrument, and an index is
    exactly the kind of machinery that can go quietly wrong -- a lost link
    does not crash, it silently withholds packets, and a soak that silently
    loses packets reports a hostile wire that nobody configured.

    So the index carries an audit ( rocketnet_network_simulator_audit ): with
    it on, every receive() re-runs the ORIGINAL full-array scan as a reference
    and compares the slot the two selections chose, then walks the whole index
    checking it against the packet arrays. Any divergence prints the specifics
    and aborts the process. It is deliberately slow -- it is proof mode, not
    soak mode -- and it does not depend on rocketnet_assert, which compiles
    out under NDEBUG, because a Release soak is exactly where the audit has
    to be able to fire.

    And the audit has its own negative control: rocketnet_network_simulator_
    inject_index_fault() deliberately drops one occupied slot out of the
    index, so the audit can be watched failing on demand rather than trusted.
    scripts/soak-sim-selftest runs both legs. That injection is ONE fault of
    several the audit catches -- a cold reader has since driven it red with a
    mis-ordered link, a wrong shard assignment, a shard bound set one
    nanosecond too high, and three others, four of which leave no trace in the
    soak's own output -- so it is the negative control, not the boundary of
    what is checked.
*/

// tla/Link.tla: Send, Deliver, Drop, Duplicate, Reorder, ReverseWindow, Delay, Age;
// the drop-every-Nth schedule is Link!Send with Pattern "everynth", the window
// reversal is Link!ReverseWindow (rocketnet_network_simulator_flush_window)
#define ROCKETNET_SIMULATOR_NO_SLOT (-1)
#define ROCKETNET_SIMULATOR_SETTING_PENDING UINT64_C(1)
#define ROCKETNET_SIMULATOR_SETTING_ACTIVE UINT64_C(2)
#define ROCKETNET_SIMULATOR_MAX_REVERSE_WINDOW 64
#define ROCKETNET_SIMULATOR_SHARDS 64

struct rocketnet_simulator_window_entry_t
{
    struct rocketnet_address_t from;
    struct rocketnet_address_t to;
    double delivery_time;
    int packet_bytes;
    uint8_t packet_data[ROCKETNET_RECEIVE_CEILING];
};

// The setting of one link, as the config record the caller hands over and
// the owner takes: latency and jitter in seconds, loss and duplicate as
// percentages.
struct rocketnet_simulator_config_t
{
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;
};

struct rocketnet_network_simulator_t
{
    void * context;
    int capacity;
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;
    // The setting handed over by the caller and the flag that says one is
    // waiting: the caller writes the record under the transport's tiny mutex,
    // the thread that owns the simulator takes it and applies it outside the
    // lock.  The setting in force above is written by that thread alone.
    struct rocketnet_simulator_config_t pending;
    int config_pending;
    // The lock-free view of the setting, so an inactive simulator costs a send
    // no lock: bit 0 says a handed-over setting is waiting, bit 1 says the
    // setting in force changes the packets.  It is written under the mutex by
    // the hand-over and the take (and once, at configure, before any other
    // thread sees the simulator), and read without it before every send.
    volatile uint64_t setting_word;
    // The packets held in the slots, counted by the thread that holds the
    // simulator's lock and read without it, so a drain of an empty simulator
    // takes no lock.
    volatile uint64_t held_count;
    // Whether any config call has set a nonzero setting.  It latches on the
    // first active config and is never cleared, not even by a later all-zero
    // config.  Production configures once, at create, so there it records the
    // create-time setting; a live change goes through the hand-over
    // (rocketnet_network_simulator_reconfigure and the take), which never
    // touches it.  The transports use it to pick the path a connection runs
    // on, and a connection's path never moves.
    int created_active;
    uint64_t rng_state;
    int audit;
    int drop_every_nth;
    int reverse_window;
    uint64_t send_count;
    int window_count;
    struct rocketnet_simulator_window_entry_t * window;

    // parallel fixed arrays, allocated once at create
    int * used;
    double * delivery_time;
    struct rocketnet_address_t * from;
    struct rocketnet_address_t * to;
    int * packet_bytes;
    uint8_t * packet_data;                  // capacity * ROCKETNET_RECEIVE_CEILING

    // The index over occupied slots. Every slot generation changes on reuse;
    // index removal validates it before touching membership. A fixed 64-way
    // shard table keeps bitmap memory linear in capacity instead of making a
    // capacity-by-capacity bit matrix. Each shard row is still an exact flat
    // physical-slot bitmap, summarized one level above its occupancy words.
    uint64_t * generation;                  // per-slot reuse tag, zero before first use
    int * slot_index_shard;                 // membership owner, or NO_SLOT
    int shard_count;                        // ROCKETNET_SIMULATOR_SHARDS, power of two
    int shard_mask;
    int occupancy_words;                    // ceil(capacity / 64)
    int summary_words;                      // ceil(occupancy_words / 64)
    uint64_t * shard_occupancy;             // shard_count * occupancy_words
    uint64_t * shard_summary;               // shard_count * summary_words
    double * shard_min_delivery_time;       // lower bound on the shard's earliest delivery
    int free_hint;                          // every slot below this one is occupied
    struct rocketnet_network_simulator_index_stats_t index_stats;
};

static uint64_t rocketnet_simulator_random( struct rocketnet_network_simulator_t * simulator )
{
    // xorshift64: deterministic, seedable, and nowhere near the crypto -- this
    // randomness decides packet fates, never key material

    uint64_t x = simulator->rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;

    simulator->rng_state = x;

    return x;
}

static double rocketnet_simulator_random_float( struct rocketnet_network_simulator_t * simulator )
{
    return (double) ( rocketnet_simulator_random( simulator ) >> 11 ) / (double) ( 1ULL << 53 );
}

static uint32_t rocketnet_simulator_address_hash( const struct rocketnet_address_t * address )
{
    // the shard hash folds EXACTLY the fields rocketnet_address_equal compares,
    // and nothing else. That equivalence is the index's load-bearing property:
    // two addresses that compare equal must land in the same shard, or a packet
    // becomes unreachable. Note the degenerate types -- equal() never considers
    // two addresses of a type that is neither IPv4 nor IPv6 equal, so folding
    // only their type is merely conservative: equal still implies same shard.

    uint32_t hash = 2166136261u;

#define ROCKETNET_SIMULATOR_FOLD( byte )                    \
        do {                                                    \
            hash ^= (uint32_t) (uint8_t) ( byte );              \
            hash *= 16777619u;                                  \
        } while ( 0 )

    ROCKETNET_SIMULATOR_FOLD( address->type );

    if ( address->type == ROCKETNET_ADDRESS_IPV4 || address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        ROCKETNET_SIMULATOR_FOLD( address->port );
        ROCKETNET_SIMULATOR_FOLD( address->port >> 8 );
        const uint8_t * bytes = (const uint8_t *) &address->data;
        const int count = ( address->type == ROCKETNET_ADDRESS_IPV4 ) ? 4 : 16;
        for ( int i = 0; i < count; i++ )
            ROCKETNET_SIMULATOR_FOLD( bytes[i] );
    }

#undef ROCKETNET_SIMULATOR_FOLD

    hash ^= hash >> 15;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    return hash;
}

static int rocketnet_simulator_shard( const struct rocketnet_network_simulator_t * simulator,
                                      const struct rocketnet_address_t * address )
{
    return (int) ( rocketnet_simulator_address_hash( address ) & (uint32_t) simulator->shard_mask );
}

static void rocketnet_simulator_audit_failed( const char * detail )
{
    // the audit's voice. Not rocketnet_error and not rocketnet_assert: the soak
    // runs at log level NONE under NDEBUG, and both of those go silent exactly
    // where this has to be heard. It prints to both streams and aborts, because
    // an instrument that has caught itself lying must not go on producing
    // numbers.

    printf( "\nNETWORK SIMULATOR AUDIT FAILED: %s\n", detail );
    fflush( stdout );
    fprintf( stderr, "NETWORK SIMULATOR AUDIT FAILED: %s\n", detail );
    fflush( stderr );
    abort();
}

struct rocketnet_network_simulator_t * rocketnet_network_simulator_create( void * context,
                                                                           int capacity,
                                                                           uint64_t seed )
{
    rocketnet_assert( capacity > 0 );

    if ( capacity <= 0 )
        return NULL;

    // keep the historical overflow refusal. The bitmap geometry below is
    // computed in 64 bits, then narrowed only after this bound.

    if ( capacity > ( 1 << 30 ) )
        return NULL;

    const int shard_count = ROCKETNET_SIMULATOR_SHARDS;
    const int occupancy_words = (int) ( ( (uint64_t) capacity + 63 ) / 64 );
    const int summary_words = ( occupancy_words + 63 ) / 64;

    struct rocketnet_network_simulator_t * simulator = (struct rocketnet_network_simulator_t *) rocketnet_malloc( context, sizeof( struct rocketnet_network_simulator_t ) );
    if ( !simulator )
        return NULL;

    memset( simulator, 0, sizeof( *simulator ) );

    simulator->context = context;
    simulator->capacity = capacity;
    simulator->rng_state = seed ? seed : 1;
    simulator->free_hint = 0;
    simulator->shard_count = shard_count;
    simulator->shard_mask = shard_count - 1;
    simulator->occupancy_words = occupancy_words;
    simulator->summary_words = summary_words;
    simulator->config_pending = 0;
    simulator->created_active = 0;

    simulator->used = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    simulator->delivery_time = (double *) rocketnet_malloc( context, sizeof( double ) * (size_t) capacity );
    simulator->from = (struct rocketnet_address_t *) rocketnet_malloc( context, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    simulator->to = (struct rocketnet_address_t *) rocketnet_malloc( context, sizeof( struct rocketnet_address_t ) * (size_t) capacity );
    simulator->packet_bytes = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    simulator->packet_data = (uint8_t *) rocketnet_malloc( context, (size_t) ROCKETNET_RECEIVE_CEILING * (size_t) capacity );
    simulator->generation = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) capacity );
    simulator->slot_index_shard = (int *) rocketnet_malloc( context, sizeof( int ) * (size_t) capacity );
    simulator->shard_occupancy = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) shard_count * (size_t) occupancy_words );
    simulator->shard_summary = (uint64_t *) rocketnet_malloc( context, sizeof( uint64_t ) * (size_t) shard_count * (size_t) summary_words );
    simulator->shard_min_delivery_time = (double *) rocketnet_malloc( context, sizeof( double ) * (size_t) shard_count );

    if ( !simulator->used || !simulator->delivery_time || !simulator->from || !simulator->to ||
         !simulator->packet_bytes || !simulator->packet_data || !simulator->generation ||
         !simulator->slot_index_shard || !simulator->shard_occupancy ||
         !simulator->shard_summary || !simulator->shard_min_delivery_time )
    {
        rocketnet_network_simulator_destroy( simulator );
        return NULL;
    }

    memset( simulator->used, 0, sizeof( int ) * (size_t) capacity );
    memset( simulator->generation, 0, sizeof( uint64_t ) * (size_t) capacity );
    memset( simulator->shard_occupancy, 0, sizeof( uint64_t ) * (size_t) shard_count * (size_t) occupancy_words );
    memset( simulator->shard_summary, 0, sizeof( uint64_t ) * (size_t) shard_count * (size_t) summary_words );

    for ( int i = 0; i < capacity; i++ )
        simulator->slot_index_shard[i] = ROCKETNET_SIMULATOR_NO_SLOT;

    for ( int i = 0; i < shard_count; i++ )
        simulator->shard_min_delivery_time[i] = DBL_MAX;

    return simulator;
}

void rocketnet_network_simulator_destroy( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );

    rocketnet_free( simulator->context, simulator->used );
    rocketnet_free( simulator->context, simulator->delivery_time );
    rocketnet_free( simulator->context, simulator->from );
    rocketnet_free( simulator->context, simulator->to );
    rocketnet_free( simulator->context, simulator->packet_bytes );
    rocketnet_free( simulator->context, simulator->packet_data );
    rocketnet_free( simulator->context, simulator->generation );
    rocketnet_free( simulator->context, simulator->slot_index_shard );
    rocketnet_free( simulator->context, simulator->shard_occupancy );
    rocketnet_free( simulator->context, simulator->shard_summary );
    rocketnet_free( simulator->context, simulator->shard_min_delivery_time );
    rocketnet_free( simulator->context, simulator->window );
    rocketnet_free( simulator->context, simulator );
}

void rocketnet_network_simulator_config( struct rocketnet_network_simulator_t * simulator,
                                         double latency,
                                         double jitter,
                                         float packet_loss_percent,
                                         float duplicate_percent )
{
    rocketnet_assert( simulator );

    simulator->latency = latency;
    simulator->jitter = jitter;
    simulator->packet_loss_percent = packet_loss_percent;
    simulator->duplicate_percent = duplicate_percent;

    // the setting the simulator was configured with decides the path a
    // connection runs on (the transports read it through
    // rocketnet_network_simulator_created_active).  The flag latches on the
    // first active config and is never cleared here.  Production calls config
    // only at create; a live change goes through the hand-over, which never
    // moves the flag, so a connection's path never moves.

    const bool active = latency != 0.0 || jitter != 0.0 || packet_loss_percent != 0.0f || duplicate_percent != 0.0f;
    if ( !simulator->created_active && active )
        simulator->created_active = 1;

    const uint64_t pending = rocketnet_platform_atomic_load_u64( &simulator->setting_word ) & ROCKETNET_SIMULATOR_SETTING_PENDING;
    rocketnet_platform_atomic_store_u64( &simulator->setting_word, pending | ( active ? ROCKETNET_SIMULATOR_SETTING_ACTIVE : 0 ) );
}

uint64_t rocketnet_network_simulator_memory_bytes( int capacity, int reverse_window )
{
    // the bytes a simulator of this capacity allocates, so a transport's memory
    // plan can cover it: the slot arrays, the index over them and the packet
    // data.  The window of the reverse pattern is counted when the pattern holds
    // one.

    if ( capacity <= 0 )
        return 0;
    if ( capacity > ( 1 << 30 ) )
        return 0;

    const uint64_t shard_count = ROCKETNET_SIMULATOR_SHARDS;
    const uint64_t slots = (uint64_t) capacity;
    const uint64_t occupancy_words = ( slots + 63 ) / 64;
    const uint64_t summary_words = ( occupancy_words + 63 ) / 64;
    uint64_t bytes = rocketnet_memory_allocations( {
        sizeof( struct rocketnet_network_simulator_t ),
        slots * sizeof( int ),
        slots * sizeof( double ),
        slots * sizeof( struct rocketnet_address_t ),
        slots * sizeof( struct rocketnet_address_t ),
        slots * sizeof( int ),
        slots * (uint64_t) ROCKETNET_RECEIVE_CEILING,
        slots * sizeof( uint64_t ),
        slots * sizeof( int ),
        shard_count * occupancy_words * sizeof( uint64_t ),
        shard_count * summary_words * sizeof( uint64_t ),
        shard_count * sizeof( double ),
    } );

    if ( reverse_window >= 2 )
    {
        const uint64_t window = rocketnet_memory_allocations( { (uint64_t) reverse_window * sizeof( struct rocketnet_simulator_window_entry_t ) } );
        if ( !window || !rocketnet_memory_add( bytes, 1, window ) )
            return 0;
    }

    return bytes;
}

void rocketnet_network_simulator_reconfigure( struct rocketnet_network_simulator_t * simulator,
                                              struct rocketnet_platform_mutex_t * mutex,
                                              double latency,
                                              double jitter,
                                              float packet_loss_percent,
                                              float duplicate_percent )
{
    // the live setting.  The caller hands a new setting over as a config record
    // under the simulator's own tiny mutex and returns at once; the thread that
    // owns the simulator takes it before its next send.  No simulator work
    // happens under the lock: the lock covers the record and the flag, and the
    // send, the loss roll and the delivery stay outside it.
    //
    // tla/MCLinkReconfigure.tla models the change: the setting in force at the
    // send holds the packet (MCSend), and the change itself removes nothing that
    // was already in flight (MCReconfigure, LiveSettingHolds).

    rocketnet_assert( simulator );
    rocketnet_assert( mutex );

    rocketnet_platform_mutex_acquire( mutex );
    simulator->pending.latency = latency;
    simulator->pending.jitter = jitter;
    simulator->pending.packet_loss_percent = packet_loss_percent;
    simulator->pending.duplicate_percent = duplicate_percent;
    simulator->config_pending = 1;

    rocketnet_platform_atomic_store_u64( &simulator->setting_word,
                                         rocketnet_platform_atomic_load_u64( &simulator->setting_word ) | ROCKETNET_SIMULATOR_SETTING_PENDING );

    rocketnet_platform_mutex_release( mutex );
}

bool rocketnet_network_simulator_take_setting( struct rocketnet_network_simulator_t * simulator,
                                               struct rocketnet_platform_mutex_t * mutex )
{
    // the owner takes the setting that was handed over and applies it, and reads
    // whether the setting in force changes the packets.  A zero setting is a
    // pass-through: it adds no latency and copies nothing.  With nothing handed
    // over the answer is read from the setting word and no lock is taken, so an
    // inactive simulator puts no mutex on the send path.

    rocketnet_assert( simulator );
    rocketnet_assert( mutex );

    const uint64_t word = rocketnet_platform_atomic_load_u64( &simulator->setting_word );
    if ( !( word & ROCKETNET_SIMULATOR_SETTING_PENDING ) )
        return ( word & ROCKETNET_SIMULATOR_SETTING_ACTIVE ) != 0;

    rocketnet_platform_mutex_acquire( mutex );

    if ( simulator->config_pending )
    {
        simulator->latency = simulator->pending.latency;
        simulator->jitter = simulator->pending.jitter;
        simulator->packet_loss_percent = simulator->pending.packet_loss_percent;
        simulator->duplicate_percent = simulator->pending.duplicate_percent;
        simulator->config_pending = 0;
    }

    const bool active = simulator->latency != 0.0 || simulator->jitter != 0.0 ||
                        simulator->packet_loss_percent != 0.0f || simulator->duplicate_percent != 0.0f;
    rocketnet_platform_atomic_store_u64( &simulator->setting_word, active ? ROCKETNET_SIMULATOR_SETTING_ACTIVE : 0 );

    rocketnet_platform_mutex_release( mutex );

    return active;
}

bool rocketnet_network_simulator_created_active( const struct rocketnet_network_simulator_t * simulator )
{
    // whether the simulator was configured with a setting that changes the
    // packets, patterns included.  The configured part latches on the first
    // active config and is never cleared; production configures once, at create,
    // and a live change through the hand-over never moves it, so a connection's
    // path is fixed when it is created.

    if ( !simulator )
        return false;

    return simulator->created_active || rocketnet_network_simulator_patterns_active( simulator );
}

bool rocketnet_network_simulator_patterns( struct rocketnet_network_simulator_t * simulator,
                                           int drop_every_nth,
                                           int reverse_window )
{
    rocketnet_assert( simulator );
    rocketnet_assert( drop_every_nth == 0 || ( drop_every_nth >= 2 && drop_every_nth <= 1000000 ) );
    rocketnet_assert( reverse_window == 0 || ( reverse_window >= 2 && reverse_window <= ROCKETNET_SIMULATOR_MAX_REVERSE_WINDOW ) );

    rocketnet_free( simulator->context, simulator->window );
    simulator->window = NULL;
    simulator->window_count = 0;
    simulator->send_count = 0;
    simulator->drop_every_nth = drop_every_nth;
    simulator->reverse_window = reverse_window;

    if ( reverse_window >= 2 )
    {
        simulator->window = (struct rocketnet_simulator_window_entry_t *) rocketnet_malloc( simulator->context, sizeof( struct rocketnet_simulator_window_entry_t ) * (size_t) reverse_window );
        if ( !simulator->window )
        {
            simulator->reverse_window = 0;
            return false;
        }
    }

    return true;
}

bool rocketnet_network_simulator_holds_any( struct rocketnet_network_simulator_t * simulator )
{
    // whether any packet is held in the slots, read without the lock: a drain
    // that sees none has nothing to take, and a packet held a moment later leaves
    // with the next drain.

    rocketnet_assert( simulator );
    return rocketnet_platform_atomic_load_u64( &simulator->held_count ) != 0;
}

bool rocketnet_network_simulator_patterns_active( const struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );
    return simulator->drop_every_nth >= 2 || simulator->reverse_window >= 2;
}

static int rocketnet_simulator_ctz64( uint64_t value )
{
    rocketnet_assert( value != 0 );

#if defined( _MSC_VER )
    unsigned long index;
    _BitScanForward64( &index, value );
    return (int) index;
#else
    return __builtin_ctzll( value );
#endif // #if defined( _MSC_VER )
}

static uint64_t * rocketnet_simulator_occupancy_word( struct rocketnet_network_simulator_t * simulator,
                                                      int shard,
                                                      int word )
{
    return &simulator->shard_occupancy[(size_t) shard * (size_t) simulator->occupancy_words + (size_t) word];
}

static const uint64_t * rocketnet_simulator_occupancy_word_const( const struct rocketnet_network_simulator_t * simulator,
                                                                  int shard,
                                                                  int word )
{
    return &simulator->shard_occupancy[(size_t) shard * (size_t) simulator->occupancy_words + (size_t) word];
}

static uint64_t * rocketnet_simulator_summary_word( struct rocketnet_network_simulator_t * simulator,
                                                    int shard,
                                                    int word )
{
    return &simulator->shard_summary[(size_t) shard * (size_t) simulator->summary_words + (size_t) word];
}

static const uint64_t * rocketnet_simulator_summary_word_const( const struct rocketnet_network_simulator_t * simulator,
                                                                int shard,
                                                                int word )
{
    return &simulator->shard_summary[(size_t) shard * (size_t) simulator->summary_words + (size_t) word];
}

static void rocketnet_simulator_record_membership_updates( struct rocketnet_network_simulator_t * simulator,
                                                           uint64_t updates )
{
    simulator->index_stats.membership_word_updates += updates;
    if ( updates > simulator->index_stats.max_membership_word_updates )
        simulator->index_stats.max_membership_word_updates = updates;
}

static void rocketnet_simulator_index_add( struct rocketnet_network_simulator_t * simulator, int slot )
{
    // membership is one physical-slot bit and, only when its occupancy word
    // crosses empty/nonempty, one summary bit. Thus insert and remove touch at
    // most two bitmap words regardless of capacity or shard population.

    const int shard = rocketnet_simulator_shard( simulator, &simulator->to[slot] );
    const int word = slot >> 6;
    const uint64_t bit = UINT64_C( 1 ) << ( slot & 63 );
    uint64_t * occupancy = rocketnet_simulator_occupancy_word( simulator, shard, word );
    rocketnet_assert( ( *occupancy & bit ) == 0 );
    rocketnet_assert( simulator->slot_index_shard[slot] == ROCKETNET_SIMULATOR_NO_SLOT );

    const bool word_was_empty = *occupancy == 0;
    *occupancy |= bit;

    uint64_t updates = 1;
    if ( word_was_empty )
    {
        uint64_t * summary = rocketnet_simulator_summary_word( simulator, shard, word >> 6 );
        *summary |= UINT64_C( 1 ) << ( word & 63 );
        updates++;
    }

    simulator->slot_index_shard[slot] = shard;
    rocketnet_simulator_record_membership_updates( simulator, updates );

    if ( simulator->delivery_time[slot] < simulator->shard_min_delivery_time[shard] )
        simulator->shard_min_delivery_time[shard] = simulator->delivery_time[slot];
}

static bool rocketnet_simulator_index_remove( struct rocketnet_network_simulator_t * simulator,
                                              const struct rocketnet_network_simulator_slot_handle_t * handle )
{
    if ( handle == NULL || handle->slot < 0 || handle->slot >= simulator->capacity ||
         !simulator->used[handle->slot] || simulator->generation[handle->slot] != handle->generation )
    {
        simulator->index_stats.stale_removals++;
        return false;
    }

    const int slot = handle->slot;
    const int shard = rocketnet_simulator_shard( simulator, &simulator->to[slot] );
    const int word = slot >> 6;
    const uint64_t bit = UINT64_C( 1 ) << ( slot & 63 );
    uint64_t * occupancy = rocketnet_simulator_occupancy_word( simulator, shard, word );
    if ( simulator->slot_index_shard[slot] != shard || ( *occupancy & bit ) == 0 )
    {
        simulator->index_stats.stale_removals++;
        return false;
    }

    *occupancy &= ~bit;

    uint64_t updates = 1;
    if ( *occupancy == 0 )
    {
        uint64_t * summary = rocketnet_simulator_summary_word( simulator, shard, word >> 6 );
        *summary &= ~( UINT64_C( 1 ) << ( word & 63 ) );
        updates++;
    }

    simulator->slot_index_shard[slot] = ROCKETNET_SIMULATOR_NO_SLOT;
    rocketnet_simulator_record_membership_updates( simulator, updates );

    // shard_min_delivery_time is a lower bound. Removing can only raise the
    // true minimum, so leaving it stale-low is safe; a failed selection scan
    // refreshes it exactly.

    return true;
}

static bool rocketnet_simulator_retire( struct rocketnet_network_simulator_t * simulator,
                                        const struct rocketnet_network_simulator_slot_handle_t * handle )
{
    if ( !rocketnet_simulator_index_remove( simulator, handle ) )
        return false;

    simulator->used[handle->slot] = 0;
    rocketnet_platform_atomic_add_u64( &simulator->held_count, UINT64_MAX );
    if ( handle->slot < simulator->free_hint )
        simulator->free_hint = handle->slot;

    return true;
}

int rocketnet_network_simulator_receive_any( struct rocketnet_network_simulator_t * simulator,
                                             double current_time,
                                             struct rocketnet_address_t * out_from,
                                             struct rocketnet_address_t * out_to,
                                             uint8_t * out_packet )
{
    rocketnet_assert( simulator );
    rocketnet_assert( out_from );
    rocketnet_assert( out_to );
    rocketnet_assert( out_packet );

    int slot = -1;
    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( simulator->used[i] && simulator->delivery_time[i] <= current_time )
        {
            slot = i;
            break;
        }
    }

    if ( slot < 0 )
        return 0;

    const int bytes = simulator->packet_bytes[slot];
    *out_from = simulator->from[slot];
    *out_to = simulator->to[slot];
    memcpy( out_packet, simulator->packet_data + (size_t) slot * ROCKETNET_RECEIVE_CEILING, (size_t) bytes );

    const struct rocketnet_network_simulator_slot_handle_t handle = { slot, simulator->generation[slot] };
    const bool retired = rocketnet_simulator_retire( simulator, &handle );
    rocketnet_assert( retired );
    (void) retired;

    return bytes;
}

void rocketnet_network_simulator_audit( struct rocketnet_network_simulator_t * simulator, int enabled )
{
    rocketnet_assert( simulator );
    simulator->audit = enabled ? 1 : 0;
}

// --------------------------------------------------------------------------
// selection: two implementations of one rule, so one can check the other

static int rocketnet_simulator_select_reference( const struct rocketnet_network_simulator_t * simulator,
                                                 const struct rocketnet_address_t * to,
                                                 double current_time )
{
    // the reference: the original full-array scan, unchanged, pure. It mutates
    // nothing, so the audit can run it beside the fast path on the same state.

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( !simulator->used[i] )
            continue;
        if ( simulator->delivery_time[i] > current_time )
            continue;
        if ( !rocketnet_address_equal( &simulator->to[i], to ) )
            continue;
        return i;
    }

    return ROCKETNET_SIMULATOR_NO_SLOT;
}

static int rocketnet_simulator_select_fast( struct rocketnet_network_simulator_t * simulator,
                                            const struct rocketnet_address_t * to,
                                            double current_time )
{
    // the fast path: the same rule over the shard's bitmap. Summary words,
    // occupancy words and set bits are all enumerated low-to-high, so this is
    // exactly ascending physical-slot order without a sortable/linkable object.

    const int shard = rocketnet_simulator_shard( simulator, to );

    if ( simulator->shard_min_delivery_time[shard] > current_time )
        return ROCKETNET_SIMULATOR_NO_SLOT;

    double minimum = DBL_MAX;
    uint64_t summary_words = 0;
    uint64_t occupancy_words = 0;
    uint64_t slots = 0;

    for ( int summary_index = 0; summary_index < simulator->summary_words; summary_index++ )
    {
        uint64_t summary = *rocketnet_simulator_summary_word( simulator, shard, summary_index );
        summary_words++;
        while ( summary != 0 )
        {
            const int occupancy_index = summary_index * 64 + rocketnet_simulator_ctz64( summary );
            if ( occupancy_index >= simulator->occupancy_words )
                break;

            uint64_t occupancy = *rocketnet_simulator_occupancy_word( simulator, shard, occupancy_index );
            occupancy_words++;
            while ( occupancy != 0 )
            {
                const int slot = occupancy_index * 64 + rocketnet_simulator_ctz64( occupancy );
                slots++;

                rocketnet_assert( slot >= 0 && slot < simulator->capacity );
                rocketnet_assert( simulator->used[slot] );
                rocketnet_assert( simulator->slot_index_shard[slot] == shard );

                if ( simulator->delivery_time[slot] <= current_time && rocketnet_address_equal( &simulator->to[slot], to ) )
                {
                    simulator->index_stats.selection_summary_words += summary_words;
                    simulator->index_stats.selection_occupancy_words += occupancy_words;
                    simulator->index_stats.selection_slots += slots;

                    if ( summary_words > simulator->index_stats.max_selection_summary_words )
                        simulator->index_stats.max_selection_summary_words = summary_words;
                    if ( occupancy_words > simulator->index_stats.max_selection_occupancy_words )
                        simulator->index_stats.max_selection_occupancy_words = occupancy_words;
                    if ( slots > simulator->index_stats.max_selection_slots )
                        simulator->index_stats.max_selection_slots = slots;

                    return slot;
                }

                if ( simulator->delivery_time[slot] < minimum )
                    minimum = simulator->delivery_time[slot];

                occupancy &= occupancy - 1;
            }

            summary &= summary - 1;
        }
    }

    simulator->index_stats.selection_summary_words += summary_words;
    simulator->index_stats.selection_occupancy_words += occupancy_words;
    simulator->index_stats.selection_slots += slots;

    if ( summary_words > simulator->index_stats.max_selection_summary_words )
        simulator->index_stats.max_selection_summary_words = summary_words;
    if ( occupancy_words > simulator->index_stats.max_selection_occupancy_words )
        simulator->index_stats.max_selection_occupancy_words = occupancy_words;
    if ( slots > simulator->index_stats.max_selection_slots )
        simulator->index_stats.max_selection_slots = slots;

    // nothing was due for this destination, and getting there required
    // looking at every entry in the shard -- so the exact minimum is in hand
    // and the bound is refreshed for free. Until the next insert lowers it,
    // further empty receives on this shard cost nothing at all.

    simulator->shard_min_delivery_time[shard] = minimum;

    return ROCKETNET_SIMULATOR_NO_SLOT;
}

// --------------------------------------------------------------------------
// the audit

static void rocketnet_simulator_audit_index( struct rocketnet_network_simulator_t * simulator )
{
    // walked per slot, O(capacity) like the reference scan beside it. Every slot
    // has one explicit shard-owner tag, which lets the audit prove a free slot has
    // no membership and an occupied slot has exactly the expected bitmap bit
    // without scanning every shard row for every slot.

    char detail[256];

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( !simulator->used[i] )
        {
            if ( simulator->slot_index_shard[i] != ROCKETNET_SIMULATOR_NO_SLOT )
            {
                snprintf( detail, sizeof( detail ), "slot %d is free but still belongs to index shard %d", i, simulator->slot_index_shard[i] );
                rocketnet_simulator_audit_failed( detail );
            }

            if ( i < simulator->free_hint )
            {
                snprintf( detail, sizeof( detail ), "slot %d is free but below the free hint %d -- send would skip it and take a higher slot", i, simulator->free_hint );
                rocketnet_simulator_audit_failed( detail );
            }

            continue;
        }

        const int shard = rocketnet_simulator_shard( simulator, &simulator->to[i] );
        const int word = i >> 6;
        const uint64_t bit = UINT64_C( 1 ) << ( i & 63 );
        const uint64_t occupancy = *rocketnet_simulator_occupancy_word_const( simulator, shard, word );
        const uint64_t summary = *rocketnet_simulator_summary_word_const( simulator, shard, word >> 6 );
        if ( simulator->slot_index_shard[i] != shard || ( occupancy & bit ) == 0 )
        {
            snprintf( detail, sizeof( detail ), "slot %d holds a packet but its shard bitmap membership is missing", i );
            rocketnet_simulator_audit_failed( detail );
        }

        if ( ( summary & ( UINT64_C( 1 ) << ( word & 63 ) ) ) == 0 )
        {
            snprintf( detail, sizeof( detail ), "slot %d is occupied but occupancy word %d is absent from shard %d's summary", i, word, shard );
            rocketnet_simulator_audit_failed( detail );
        }

        if ( simulator->delivery_time[i] < simulator->shard_min_delivery_time[shard] )
        {
            snprintf( detail, sizeof( detail ), "shard %d claims a minimum delivery time later than slot %d actually holds -- due packets can be skipped", shard, i );
            rocketnet_simulator_audit_failed( detail );
        }
    }
}

void rocketnet_network_simulator_inject_index_fault( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );

    // the audit's negative control: remove one occupied slot from the index
    // and leave it occupied. Nothing crashes, nothing logs, the packet simply
    // stops being deliverable -- which is precisely the class of silent
    // mis-measurement an index introduces and the audit exists to catch. If
    // the audit does not go red on this, the audit is not an instrument.

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( simulator->used[i] )
        {
            const struct rocketnet_network_simulator_slot_handle_t handle = { i, simulator->generation[i] };
            const bool removed = rocketnet_simulator_index_remove( simulator, &handle );

            rocketnet_assert( removed );
            (void) removed;

            return;
        }
    }
}

// --------------------------------------------------------------------------

static int rocketnet_simulator_insert( struct rocketnet_network_simulator_t * simulator,
                                       const struct rocketnet_address_t * from,
                                       const struct rocketnet_address_t * to,
                                       const uint8_t * packet_data,
                                       int packet_bytes,
                                       double delivery_time )
{
    // the lowest free slot. every slot below free_hint is occupied, which is
    // the invariant the audit checks, so starting there finds the same slot
    // the scan from zero found.

    for ( int i = simulator->free_hint; i < simulator->capacity; i++ )
    {
        if ( simulator->used[i] )
            continue;

        if ( simulator->generation[i] == UINT64_MAX )
        {
            rocketnet_spam( "network simulator slot generation exhausted. packet dropped" );
            return 0;
        }

        simulator->generation[i]++;
        simulator->used[i] = 1;
        simulator->delivery_time[i] = delivery_time;
        simulator->from[i] = *from;
        simulator->to[i] = *to;
        simulator->packet_bytes[i] = packet_bytes;
        memcpy( simulator->packet_data + (size_t) i * ROCKETNET_RECEIVE_CEILING, packet_data, (size_t) packet_bytes );

        rocketnet_simulator_index_add( simulator, i );
        simulator->free_hint = i + 1;
        rocketnet_platform_atomic_add_u64( &simulator->held_count, 1 );

        return 1;
    }

    simulator->free_hint = simulator->capacity;
    rocketnet_spam( "network simulator queue full. packet dropped" );
    return 0;
}

static void rocketnet_simulator_emit( struct rocketnet_network_simulator_t * simulator,
                                      const struct rocketnet_address_t * from,
                                      const struct rocketnet_address_t * to,
                                      const uint8_t * packet_data,
                                      int packet_bytes,
                                      double delivery_time )
{
    if ( simulator->reverse_window < 2 )
    {
        rocketnet_simulator_insert( simulator, from, to, packet_data, packet_bytes, delivery_time );
        return;
    }

    const int idx = simulator->window_count;
    rocketnet_assert( idx >= 0 && idx < simulator->reverse_window && idx < ROCKETNET_SIMULATOR_MAX_REVERSE_WINDOW );

    simulator->window[idx].from = *from;
    simulator->window[idx].to = *to;
    simulator->window[idx].delivery_time = delivery_time;
    simulator->window[idx].packet_bytes = packet_bytes;
    memcpy( simulator->window[idx].packet_data, packet_data, (size_t) packet_bytes );
    simulator->window_count = idx + 1;

    if ( simulator->window_count >= simulator->reverse_window )
    {
        rocketnet_network_simulator_flush_window( simulator );
    }
}

void rocketnet_network_simulator_flush_window( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );

    if ( simulator->window_count <= 0 )
        return;

    double max_delivery_time = simulator->window[0].delivery_time;
    for ( int i = 1; i < simulator->window_count; i++ )
    {
        if ( simulator->window[i].delivery_time > max_delivery_time )
            max_delivery_time = simulator->window[i].delivery_time;
    }

    for ( int i = simulator->window_count - 1; i >= 0; i-- )
    {
        rocketnet_simulator_insert( simulator,
                                    &simulator->window[i].from,
                                    &simulator->window[i].to,
                                    simulator->window[i].packet_data,
                                    simulator->window[i].packet_bytes,
                                    max_delivery_time );
    }

    simulator->window_count = 0;
}

void rocketnet_network_simulator_send( struct rocketnet_network_simulator_t * simulator,
                                       const struct rocketnet_address_t * from,
                                       const struct rocketnet_address_t * to,
                                       const uint8_t * packet_data,
                                       int packet_bytes,
                                       double current_time )
{
    rocketnet_assert( simulator );
    rocketnet_assert( from );
    rocketnet_assert( to );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    simulator->send_count++;
    if ( simulator->drop_every_nth >= 2 && ( simulator->send_count % (uint64_t) simulator->drop_every_nth ) == 0 )
        return;

    if ( rocketnet_simulator_random_float( simulator ) * 100.0 < (double) simulator->packet_loss_percent )
        return;

    double delivery_time = current_time + simulator->latency + rocketnet_simulator_random_float( simulator ) * simulator->jitter;
    rocketnet_simulator_emit( simulator, from, to, packet_data, packet_bytes, delivery_time );

    if ( rocketnet_simulator_random_float( simulator ) * 100.0 < (double) simulator->duplicate_percent )
    {
        double duplicate_time = current_time + simulator->latency + rocketnet_simulator_random_float( simulator ) * simulator->jitter;
        rocketnet_simulator_emit( simulator, from, to, packet_data, packet_bytes, duplicate_time );
    }
}

int rocketnet_network_simulator_deliver_packets( struct rocketnet_network_simulator_t * simulator,
                                                 const struct rocketnet_address_t * from,
                                                 const struct rocketnet_address_t * to,
                                                 const uint8_t * const * packet_data,
                                                 const int * packet_bytes,
                                                 int packet_count,
                                                 double delivery_time )
{
    rocketnet_assert( simulator );
    rocketnet_assert( from );
    rocketnet_assert( to );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( packet_count >= 0 );

    int accepted = 0;
    for ( int i = 0; i < packet_count; i++ )
    {
        if ( packet_data[i] == NULL || packet_bytes[i] <= 0 || packet_bytes[i] > ROCKETNET_RECEIVE_CEILING )
            continue;
        if ( rocketnet_simulator_insert( simulator, from, to, packet_data[i], packet_bytes[i], delivery_time ) )
            accepted++;
    }
    return accepted;
}

int rocketnet_network_simulator_receive( struct rocketnet_network_simulator_t * simulator,
                                         const struct rocketnet_address_t * to,
                                         double current_time,
                                         struct rocketnet_address_t * out_from,
                                         uint8_t * out_packet )
{
    rocketnet_assert( simulator );
    rocketnet_assert( to );
    rocketnet_assert( out_from );
    rocketnet_assert( out_packet );

    const int slot = rocketnet_simulator_select_fast( simulator, to, current_time );

    if ( simulator->audit )
    {
        const int expected = rocketnet_simulator_select_reference( simulator, to, current_time );
        if ( slot != expected )
        {
            char detail[256];
            snprintf( detail, sizeof( detail ),
                      "the index chose slot %d where the reference scan chose slot %d at t=%.6f -- the wire is delivering a different packet sequence",
                      slot, expected, current_time );
            rocketnet_simulator_audit_failed( detail );
        }

        rocketnet_simulator_audit_index( simulator );
    }

    if ( slot == ROCKETNET_SIMULATOR_NO_SLOT )
        return 0;

    const int packet_bytes = simulator->packet_bytes[slot];
    *out_from = simulator->from[slot];
    memcpy( out_packet, simulator->packet_data + (size_t) slot * ROCKETNET_RECEIVE_CEILING, (size_t) packet_bytes );

    const struct rocketnet_network_simulator_slot_handle_t handle = { slot, simulator->generation[slot] };
    const bool retired = rocketnet_simulator_retire( simulator, &handle );
    rocketnet_assert( retired );
    (void) retired;

    return packet_bytes;
}

bool rocketnet_network_simulator_test_handle_for_slot( const struct rocketnet_network_simulator_t * simulator,
                                                       int slot,
                                                       struct rocketnet_network_simulator_slot_handle_t * handle )
{
    if ( !simulator || !handle || slot < 0 || slot >= simulator->capacity || !simulator->used[slot] )
        return false;

    handle->slot = slot;
    handle->generation = simulator->generation[slot];

    return true;
}

bool rocketnet_network_simulator_test_remove_handle( struct rocketnet_network_simulator_t * simulator,
                                                     const struct rocketnet_network_simulator_slot_handle_t * handle )
{
    if ( !simulator )
        return false;
    return rocketnet_simulator_retire( simulator, handle );
}

void rocketnet_network_simulator_test_index_stats( const struct rocketnet_network_simulator_t * simulator,
                                                   struct rocketnet_network_simulator_index_stats_t * stats )
{
    rocketnet_assert( simulator );
    rocketnet_assert( stats );
    *stats = simulator->index_stats;
}

void rocketnet_network_simulator_test_reset_index_stats( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );
    memset( &simulator->index_stats, 0, sizeof( simulator->index_stats ) );
}

#endif // #if ROCKETNET_DEV
