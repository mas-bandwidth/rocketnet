/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_network_simulator.h"

#if ROCKETNET_TEST_SEAMS
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

    The index: every occupied slot is linked into a per-destination-shard
    doubly linked list, and each list is kept in ASCENDING SLOT INDEX ORDER.
    The shard is a hash of the destination address, so all slots addressed to
    a given destination live in exactly one shard; walking that shard's list
    therefore visits every candidate slot in the same ascending index order
    the full array walk did, and the address predicate still runs (a shard may
    hold more than one destination). Same visits, same order, same winner --
    fewer slots touched to get there.

    Two further aids, both exact, and neither one constant-cost -- free_hint's
    scan is O(the gap it skips) and the bound's refresh is O(the list it
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
    // Whether the setting the simulator was configured with was nonzero.  It
    // is decided once, at create, and never changes: the transports use it to
    // pick the path a connection runs on, and a connection's path never moves.
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

    // the index over the occupied slots
    int shard_count;                        // power of two
    int shard_mask;
    int * shard_head;                       // shard -> lowest occupied slot in it, or NO_SLOT
    int * shard_tail;
    double * shard_min_delivery_time;       // lower bound on the shard's earliest delivery
    int * next_in_shard;                    // per slot
    int * prev_in_shard;                    // per slot
    int free_hint;                          // every slot below this one is occupied
};

// xorshift64: deterministic, seedable, and nowhere near the crypto -- this
// randomness decides packet fates, never key material

static uint64_t rocketnet_simulator_random( struct rocketnet_network_simulator_t * simulator )
{
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

// The shard hash folds EXACTLY the fields rocketnet_address_equal compares,
// and nothing else. That equivalence is the index's load-bearing property:
// two addresses that compare equal must land in the same shard, or a packet
// becomes unreachable. Note the degenerate types -- equal() considers two
// addresses of a type that is neither IPv4 nor IPv6 equal on the type alone,
// so the hash must ignore their payload bytes for the same reason.

static uint32_t rocketnet_simulator_address_hash( const struct rocketnet_address_t * address )
{
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
        const uint8_t * bytes = (const uint8_t*) &address->data;
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

// The audit's voice. Not rocketnet_error and not rocketnet_assert: the soak
// runs at log level NONE under NDEBUG, and both of those go silent exactly
// where this has to be heard. It prints to both streams and aborts, because
// an instrument that has caught itself lying must not go on producing
// numbers.

static void rocketnet_simulator_audit_failed( const char * detail )
{
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

    // one shard per slot, rounded up to a power of two and floored at 64:
    // occupancy can never exceed capacity, so the lists stay short, and the
    // whole shard table is a few tens of kilobytes at soak_sim's 4096.
    //
    // The rounding runs in 64 bits and the result must fit the int the fields
    // hold: a capacity above 2^30 rounds to a shard count past INT_MAX and is
    // refused here before any allocation. Rounding in an int would overflow
    // the doubling into a corrupt mask or an unbounded loop.

    uint64_t shard_count_rounded = 64;
    while ( shard_count_rounded < (uint64_t) capacity )
        shard_count_rounded *= 2;

    if ( shard_count_rounded > (uint64_t) INT_MAX )
        return NULL;

    const int shard_count = (int) shard_count_rounded;

    struct rocketnet_network_simulator_t * simulator = (struct rocketnet_network_simulator_t*) rocketnet_malloc( context, sizeof(struct rocketnet_network_simulator_t) );
    if ( !simulator )
        return NULL;
    memset( simulator, 0, sizeof(*simulator) );

    simulator->context = context;
    simulator->capacity = capacity;
    simulator->rng_state = seed ? seed : 1;
    simulator->free_hint = 0;
    simulator->shard_count = shard_count;
    simulator->shard_mask = shard_count - 1;
    simulator->config_pending = 0;
    simulator->created_active = 0;

    simulator->used = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    simulator->delivery_time = (double*) rocketnet_malloc( context, sizeof(double) * (size_t) capacity );
    simulator->from = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    simulator->to = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    simulator->packet_bytes = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    simulator->packet_data = (uint8_t*) rocketnet_malloc( context, (size_t) ROCKETNET_RECEIVE_CEILING * (size_t) capacity );
    simulator->shard_head = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) shard_count );
    simulator->shard_tail = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) shard_count );
    simulator->shard_min_delivery_time = (double*) rocketnet_malloc( context, sizeof(double) * (size_t) shard_count );
    simulator->next_in_shard = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    simulator->prev_in_shard = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );

    if ( !simulator->used || !simulator->delivery_time || !simulator->from || !simulator->to ||
         !simulator->packet_bytes || !simulator->packet_data || !simulator->shard_head ||
         !simulator->shard_tail || !simulator->shard_min_delivery_time ||
         !simulator->next_in_shard || !simulator->prev_in_shard )
    {
        rocketnet_network_simulator_destroy( simulator );
        return NULL;
    }

    memset( simulator->used, 0, sizeof(int) * (size_t) capacity );

    for ( int i = 0; i < capacity; i++ )
    {
        simulator->next_in_shard[i] = ROCKETNET_SIMULATOR_NO_SLOT;
        simulator->prev_in_shard[i] = ROCKETNET_SIMULATOR_NO_SLOT;
    }

    for ( int i = 0; i < shard_count; i++ )
    {
        simulator->shard_head[i] = ROCKETNET_SIMULATOR_NO_SLOT;
        simulator->shard_tail[i] = ROCKETNET_SIMULATOR_NO_SLOT;
        simulator->shard_min_delivery_time[i] = DBL_MAX;
    }

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
    rocketnet_free( simulator->context, simulator->shard_head );
    rocketnet_free( simulator->context, simulator->shard_tail );
    rocketnet_free( simulator->context, simulator->shard_min_delivery_time );
    rocketnet_free( simulator->context, simulator->next_in_shard );
    rocketnet_free( simulator->context, simulator->prev_in_shard );
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
    // The setting the simulator was configured with decides the path a
    // connection runs on (the transports read it through
    // rocketnet_network_simulator_created_active).  It is recorded once, at
    // create: a live change never moves a connection's path.
    const bool active = latency != 0.0 || jitter != 0.0 || packet_loss_percent != 0.0f || duplicate_percent != 0.0f;
    if ( !simulator->created_active && active )
        simulator->created_active = 1;
    const uint64_t pending = rocketnet_platform_atomic_load_u64( &simulator->setting_word ) & ROCKETNET_SIMULATOR_SETTING_PENDING;
    rocketnet_platform_atomic_store_u64( &simulator->setting_word, pending | ( active ? ROCKETNET_SIMULATOR_SETTING_ACTIVE : 0 ) );
}

// The bytes a simulator of this capacity allocates, so a transport's memory
// plan can cover it: the slot arrays, the index over them and the packet
// data.  The window of the reverse pattern is counted when the pattern holds
// one.
uint64_t rocketnet_network_simulator_memory_bytes( int capacity, int reverse_window )
{
    if ( capacity <= 0 )
        return 0;
    uint64_t shard_count = 64;
    while ( shard_count < (uint64_t) capacity )
        shard_count *= 2;
    const uint64_t slots = (uint64_t) capacity;
    uint64_t bytes = rocketnet_memory_allocations( {
        sizeof( struct rocketnet_network_simulator_t ),
        slots * sizeof( int ),
        slots * sizeof( double ),
        slots * sizeof( struct rocketnet_address_t ),
        slots * sizeof( struct rocketnet_address_t ),
        slots * sizeof( int ),
        slots * (uint64_t) ROCKETNET_RECEIVE_CEILING,
        shard_count * sizeof( int ),
        shard_count * sizeof( int ),
        shard_count * sizeof( double ),
        slots * sizeof( int ),
        slots * sizeof( int )
    } );
    if ( reverse_window >= 2 )
    {
        const uint64_t window = rocketnet_memory_allocations( { (uint64_t) reverse_window * sizeof( struct rocketnet_simulator_window_entry_t ) } );
        if ( !window || !rocketnet_memory_add( bytes, 1, window ) )
            return 0;
    }
    return bytes;
}

// The live setting.  The caller hands a new setting over as a config record
// under the simulator's own tiny mutex and returns at once; the thread that
// owns the simulator takes it before its next send.  No simulator work
// happens under the lock: the lock covers the record and the flag, and the
// send, the loss roll and the delivery stay outside it.
//
// tla/MCLinkReconfigure.tla models the change: the setting in force at the
// send holds the packet (MCSend), and the change itself removes nothing that
// was already in flight (MCReconfigure, LiveSettingHolds).
void rocketnet_network_simulator_reconfigure( struct rocketnet_network_simulator_t * simulator,
                                              struct rocketnet_platform_mutex_t * mutex,
                                              double latency,
                                              double jitter,
                                              float packet_loss_percent,
                                              float duplicate_percent )
{
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

// The owner takes the setting that was handed over and applies it, and reads
// whether the setting in force changes the packets.  A zero setting is a
// pass-through: it adds no latency and copies nothing.  With nothing handed
// over the answer is read from the setting word and no lock is taken, so an
// inactive simulator puts no mutex on the send path.
bool rocketnet_network_simulator_take_setting( struct rocketnet_network_simulator_t * simulator,
                                                struct rocketnet_platform_mutex_t * mutex )
{
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

// Whether the simulator was configured with a setting that changes the
// packets, patterns included.  It is decided at create and never changes: a
// connection's path is fixed when it is created, and a live setting never
// moves it.
bool rocketnet_network_simulator_created_active( const struct rocketnet_network_simulator_t * simulator )
{
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
        simulator->window = (struct rocketnet_simulator_window_entry_t*) rocketnet_malloc( simulator->context, sizeof(struct rocketnet_simulator_window_entry_t) * (size_t) reverse_window );
        if ( !simulator->window )
        {
            simulator->reverse_window = 0;
            return false;
        }
    }
    return true;
}

// Whether any packet is held in the slots, read without the lock: a drain
// that sees none has nothing to take, and a packet held a moment later leaves
// with the next drain.
bool rocketnet_network_simulator_holds_any( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );
    return rocketnet_platform_atomic_load_u64( &simulator->held_count ) != 0;
}

bool rocketnet_network_simulator_patterns_active( const struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );
    return simulator->drop_every_nth >= 2 || simulator->reverse_window >= 2;
}

static void rocketnet_simulator_unlink( struct rocketnet_network_simulator_t * simulator, int slot );

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
    rocketnet_simulator_unlink( simulator, slot );
    simulator->used[slot] = 0;
    rocketnet_platform_atomic_add_u64( &simulator->held_count, UINT64_MAX );
    if ( slot < simulator->free_hint )
        simulator->free_hint = slot;
    return bytes;
}

void rocketnet_network_simulator_audit( struct rocketnet_network_simulator_t * simulator, int enabled )
{
    rocketnet_assert( simulator );
    simulator->audit = enabled ? 1 : 0;
}

// --------------------------------------------------------------------------
// the index

/*
    Linking keeps each shard in ascending slot order, and finding the position
    is the one operation here that is NOT constant time.

    An earlier version of this function walked back from the tail and claimed
    both common shapes were short. Half of that was wrong, and wrong in the
    expensive direction: send() takes the LOWEST free slot, so the shape where
    a low slot is vacated and immediately refilled puts the new entry at the
    FRONT of the list, which is the farthest point from the tail.

    So the walk picks its end. A slot below the head is a prepend and a slot
    above the tail is an append, both constant time, and those are the two
    shapes that actually occur; anything landing in the interior walks from
    whichever end is nearer IN INDEX SPACE.

    Index distance is not entry count, so that heuristic can pick the longer
    end and the worst case is still the whole list: a shard holding slots
    0..1199 plus one at 4000, inserted at 1200, walks 1199 entries when the
    other end was one step away. Measured over the soak it chooses wrong 2.1%
    of the time for 1.004x the optimal total steps, worst single excess 31
    entries, which is why this is a heuristic and not a fix.

    Measured, Apple silicon, one destination, the lowest slot
    vacated and refilled 20,000 times, nanoseconds per receive+send pair:

        occupancy                    500      1000      4000
        the array scan this replaced  17         8         8
        index, walking from the tail 943      1029      3757
        index, walk picks its end     19        18        18

    At 4000 that is 470x the array scan before and 2.25x after.

    The occupancy-500 column is a cold first iteration, not a property of the
    structure: a reader re-running it warmed read 8.9 and 544 where this run
    read 17 and 943. The 4000 column, which is the one the argument rests on,
    reproduced (3984 against 3757). Flagged rather than re-run, so that a
    reader who notices 500 disagreeing with 1000 distrusts the cell instead of
    the mechanism.

    WHERE THIS INDEX IS STILL SLOWER THAN THE SCAN IT REPLACED, which is a
    real property and not a rounding error. Finding a due packet costs one
    list step per entry the walk passes, and a list step chases a pointer and
    then reads delivery_time at a random index; the array scan read a
    contiguous used[] array and was much cheaper PER ENTRY. The index wins
    whenever it visits fewer entries, which is every shape the soak produces.
    It loses when it must visit all of them anyway -- a shard whose head is a
    long run of not-yet-due entries with the due one at the far end:

        occupancy                    1000      4000
        the array scan this replaced  664      2529
        this index                   1635      5955

    2.35x slower at 4000. soak_sim runs about 400 packets in flight over nine
    destinations, so its lists are tens of entries and it is nowhere near
    either of these shapes -- its measured gain is 1.34x overall. The numbers
    are here because this file ships inside librocketnet and the functional
    tests use it, so a caller with a different shape should meet them rather
    than discover them.
*/

static void rocketnet_simulator_link( struct rocketnet_network_simulator_t * simulator, int slot )
{
    const int shard = rocketnet_simulator_shard( simulator, &simulator->to[slot] );
    const int head = simulator->shard_head[shard];
    const int tail = simulator->shard_tail[shard];

    int after;                                  // the last entry below slot, or NO_SLOT to prepend

    if ( head == ROCKETNET_SIMULATOR_NO_SLOT || slot < head )
    {
        after = ROCKETNET_SIMULATOR_NO_SLOT;    // empty list, or a new lowest entry
    }
    else if ( slot > tail )
    {
        after = tail;                           // a new highest entry
    }
    else if ( slot - head <= tail - slot )
    {
        after = head;                           // nearer the head: walk forward
        while ( simulator->next_in_shard[after] != ROCKETNET_SIMULATOR_NO_SLOT &&
                simulator->next_in_shard[after] < slot )
            after = simulator->next_in_shard[after];
    }
    else
    {
        after = tail;                           // nearer the tail: walk back
        while ( after != ROCKETNET_SIMULATOR_NO_SLOT && after > slot )
            after = simulator->prev_in_shard[after];
    }

    if ( after == ROCKETNET_SIMULATOR_NO_SLOT )
    {
        simulator->prev_in_shard[slot] = ROCKETNET_SIMULATOR_NO_SLOT;
        simulator->next_in_shard[slot] = head;
        if ( head != ROCKETNET_SIMULATOR_NO_SLOT )
            simulator->prev_in_shard[head] = slot;
        else
            simulator->shard_tail[shard] = slot;
        simulator->shard_head[shard] = slot;
    }
    else
    {
        const int next = simulator->next_in_shard[after];
        simulator->prev_in_shard[slot] = after;
        simulator->next_in_shard[slot] = next;
        if ( next != ROCKETNET_SIMULATOR_NO_SLOT )
            simulator->prev_in_shard[next] = slot;
        else
            simulator->shard_tail[shard] = slot;
        simulator->next_in_shard[after] = slot;
    }

    if ( simulator->delivery_time[slot] < simulator->shard_min_delivery_time[shard] )
        simulator->shard_min_delivery_time[shard] = simulator->delivery_time[slot];
}

static void rocketnet_simulator_unlink( struct rocketnet_network_simulator_t * simulator, int slot )
{
    const int shard = rocketnet_simulator_shard( simulator, &simulator->to[slot] );
    const int previous = simulator->prev_in_shard[slot];
    const int next = simulator->next_in_shard[slot];

    if ( previous != ROCKETNET_SIMULATOR_NO_SLOT )
        simulator->next_in_shard[previous] = next;
    else
        simulator->shard_head[shard] = next;

    if ( next != ROCKETNET_SIMULATOR_NO_SLOT )
        simulator->prev_in_shard[next] = previous;
    else
        simulator->shard_tail[shard] = previous;

    simulator->next_in_shard[slot] = ROCKETNET_SIMULATOR_NO_SLOT;
    simulator->prev_in_shard[slot] = ROCKETNET_SIMULATOR_NO_SLOT;

    // shard_min_delivery_time is only ever a lower bound. Removing an entry
    // can only raise the true minimum, so leaving the bound where it is stays
    // valid -- it costs a walk, and can never hide a due packet.
}

// --------------------------------------------------------------------------
// selection: two implementations of one rule, so one can check the other

// the reference: the original full-array scan, unchanged, pure. It mutates
// nothing, so the audit can run it beside the fast path on the same state.

static int rocketnet_simulator_select_reference( const struct rocketnet_network_simulator_t * simulator,
                                                 const struct rocketnet_address_t * to,
                                                 double current_time )
{
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

// the fast path: the same rule over the shard's list, which holds exactly the
// candidate slots and holds them in the same ascending index order.

static int rocketnet_simulator_select_fast( struct rocketnet_network_simulator_t * simulator,
                                            const struct rocketnet_address_t * to,
                                            double current_time )
{
    const int shard = rocketnet_simulator_shard( simulator, to );

    if ( simulator->shard_min_delivery_time[shard] > current_time )
        return ROCKETNET_SIMULATOR_NO_SLOT;

    double minimum = DBL_MAX;

    for ( int i = simulator->shard_head[shard]; i != ROCKETNET_SIMULATOR_NO_SLOT; i = simulator->next_in_shard[i] )
    {
        if ( simulator->delivery_time[i] > current_time || !rocketnet_address_equal( &simulator->to[i], to ) )
        {
            if ( simulator->delivery_time[i] < minimum )
                minimum = simulator->delivery_time[i];
            continue;
        }
        return i;
    }

    // nothing was due for this destination, and getting there required
    // looking at every entry in the shard -- so the exact minimum is in hand
    // and the bound is refreshed for free. Until the next insert lowers it,
    // further empty receives on this shard cost nothing at all.

    simulator->shard_min_delivery_time[shard] = minimum;
    return ROCKETNET_SIMULATOR_NO_SLOT;
}

// --------------------------------------------------------------------------
// the audit

// Walked per slot rather than per shard list, which makes it O(capacity) --
// the same order as the reference scan it runs beside -- and checks every
// property the index claims: occupied slots are linked, in ascending order,
// with both links agreeing; free slots are not linked and are not below the
// free hint; and no shard's minimum-delivery bound sits above a delivery time
// it is supposed to bound. A slot dropped out of the index entirely fails the
// head/tail check here, and fails the selection comparison in receive() the
// moment its packet comes due.

static void rocketnet_simulator_audit_index( struct rocketnet_network_simulator_t * simulator )
{
    char detail[256];

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        const int previous = simulator->prev_in_shard[i];
        const int next = simulator->next_in_shard[i];

        if ( !simulator->used[i] )
        {
            if ( previous != ROCKETNET_SIMULATOR_NO_SLOT || next != ROCKETNET_SIMULATOR_NO_SLOT )
            {
                snprintf( detail, sizeof(detail), "slot %d is free but still linked into the index", i );
                rocketnet_simulator_audit_failed( detail );
            }
            if ( i < simulator->free_hint )
            {
                snprintf( detail, sizeof(detail), "slot %d is free but below the free hint %d -- send would skip it and take a higher slot", i, simulator->free_hint );
                rocketnet_simulator_audit_failed( detail );
            }
            continue;
        }

        const int shard = rocketnet_simulator_shard( simulator, &simulator->to[i] );

        if ( previous == ROCKETNET_SIMULATOR_NO_SLOT )
        {
            if ( simulator->shard_head[shard] != i )
            {
                snprintf( detail, sizeof(detail), "slot %d holds a packet but is not in the index -- that packet can never be received", i );
                rocketnet_simulator_audit_failed( detail );
            }
        }
        else if ( previous < 0 || previous >= simulator->capacity || previous >= i ||
                  !simulator->used[previous] || simulator->next_in_shard[previous] != i ||
                  rocketnet_simulator_shard( simulator, &simulator->to[previous] ) != shard )
        {
            snprintf( detail, sizeof(detail), "slot %d has a broken or out-of-order back link to %d -- delivery order has changed", i, previous );
            rocketnet_simulator_audit_failed( detail );
        }

        if ( next == ROCKETNET_SIMULATOR_NO_SLOT )
        {
            if ( simulator->shard_tail[shard] != i )
            {
                snprintf( detail, sizeof(detail), "slot %d ends its shard list but is not the shard tail", i );
                rocketnet_simulator_audit_failed( detail );
            }
        }
        else if ( next < 0 || next >= simulator->capacity || next <= i ||
                  !simulator->used[next] || simulator->prev_in_shard[next] != i ||
                  rocketnet_simulator_shard( simulator, &simulator->to[next] ) != shard )
        {
            snprintf( detail, sizeof(detail), "slot %d has a broken or out-of-order forward link to %d -- delivery order has changed", i, next );
            rocketnet_simulator_audit_failed( detail );
        }

        if ( simulator->delivery_time[i] < simulator->shard_min_delivery_time[shard] )
        {
            snprintf( detail, sizeof(detail), "shard %d claims a minimum delivery time later than slot %d actually holds -- due packets can be skipped", shard, i );
            rocketnet_simulator_audit_failed( detail );
        }
    }
}

void rocketnet_network_simulator_inject_index_fault( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );

    // The audit's negative control: unlink one occupied slot from the index
    // and leave it occupied. Nothing crashes, nothing logs, the packet simply
    // stops being deliverable -- which is precisely the class of silent
    // mis-measurement an index introduces and the audit exists to catch. If
    // the audit does not go red on this, the audit is not an instrument.

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( simulator->used[i] )
        {
            rocketnet_simulator_unlink( simulator, i );
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
        simulator->used[i] = 1;
        simulator->delivery_time[i] = delivery_time;
        simulator->from[i] = *from;
        simulator->to[i] = *to;
        simulator->packet_bytes[i] = packet_bytes;
        memcpy( simulator->packet_data + (size_t) i * ROCKETNET_RECEIVE_CEILING, packet_data, (size_t) packet_bytes );
        rocketnet_simulator_link( simulator, i );
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
            snprintf( detail, sizeof(detail),
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

    rocketnet_simulator_unlink( simulator, slot );
    simulator->used[slot] = 0;
    rocketnet_platform_atomic_add_u64( &simulator->held_count, UINT64_MAX );
    if ( slot < simulator->free_hint )
        simulator->free_hint = slot;

    return packet_bytes;
}

#endif // #if ROCKETNET_TEST_SEAMS
