/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The hostile soak: client and server protocol objects through the network
    simulator at heavy loss, latency, jitter and duplication, under mocked
    time stepped fast -- hours of simulated network abuse per minute of wall
    clock. Its job is smoking out crashes and state machine hangs: every
    payload is pattern verified -- fragmented payloads included, reassembled
    under the same abuse, where a lost fragment abandons its payload by
    design and any other fragment refusal fails the run -- and a watchdog
    treats a machine that neither progresses nor terminally fails as a
    wedge and fails loudly.

    ./bin/soak_sim            runs forever
    ./bin/soak_sim 200000     bounded iterations

    The wire's seed is random per run by default -- a soak wants a different
    hostile wire every night. ROCKETNET_SOAK_SIM_SEED=<n> pins it, which is
    what makes an A/B of two builds honest: at the same seed every packet
    meets the same fate, the two runs do exactly the same work, and their
    summary lines must match exactly or the change did more than it claimed.

    ROCKETNET_SOAK_SIM_PROFILE=1  reports where the soak's wall clock went:
                                  how much of it the network simulator took
                                  (harness) against how much the protocol
                                  objects took (product). #93 needed a perf
                                  session on a machine most of us cannot get
                                  to in order to learn that 38.6% of the soak
                                  was harness; this makes any machine able to
                                  answer that about itself. The accounting is
                                  self-checking: it refuses the whole run if
                                  the parts exceed the whole, and marks any
                                  share its own timer is too coarse to support.

    ROCKETNET_SOAK_SIM_FAULT_ACCOUNTING=1
                                  the profile's negative control: double-charge
                                  the wire bucket so the accounting exceeds the
                                  wall clock it is dividing up. The profile
                                  MUST refuse the run. scripts/soak-sim-selftest
                                  runs this leg and fails unless it goes red.

    ROCKETNET_SOAK_SIM_AUDIT=1    cross-checks every simulator receive against
                                  the reference full-array scan and aborts
                                  loudly on any divergence. Slow by
                                  construction -- it is the proof that the
                                  indexed wire delivers the same packets in
                                  the same order, not a mode to soak in.

    ROCKETNET_SOAK_SIM_FAULT_INDEX=<n>
                                  the audit's negative control: at iteration n,
                                  corrupt the simulator's index on purpose.
                                  With the audit on the run MUST die red; if it
                                  survives, the audit proves nothing. Without
                                  the audit it does NOT die, which is the whole
                                  reason the audit exists -- the soak simply
                                  loses a packet and reports success.
                                  scripts/soak-sim-selftest runs both.
*/

#include "rocketnet_internal.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_fragment.h"
#include "rocketnet_ack.h"
#include "rocketnet_platform.h"
#include "rocketnet_test_vectors.h"
#include "test/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <cerrno>
#include <sodium.h>

static volatile int quit;

// ---------------------------------------------------------------------------
// where the soak's wall clock goes
//
// Two MEASURED buckets and a remainder, and the difference matters. WIRE and
// PRODUCT are explicit lists of wrapped calls: WIRE is the network simulator,
// harness by definition, since it inflates nothing about the library and
// exists only to abuse it; PRODUCT is the client and server protocol objects
// and everything under them, including the packet crypto, which is the thing
// the soak is for.
//
// The third line is not a bucket. It is total minus the two, so it holds
// whatever was never wrapped, and it is MIXED BY CONSTRUCTION -- calling it
// "soak bookkeeping" implied a purity a remainder can never have, which is
// what an earlier version of this comment and its printed label both did.
// Most of it is genuinely the soak's own work (payload generation and
// verification, connect token field assembly, the watchdog), but it also
// holds library calls that were never worth a timer:
//
//   - randombytes_buf twice per connect attempt, generating the session keys
//     a real backend would generate. That is libsodium, so it is product
//     code, doing a job that is the harness's in this arrangement. It has no
//     honest side and is left here rather than assigned to one.
//   - rocketnet_client_slots_mapping_index and rocketnet_encryption_map_
//     address, two library lookups per keep-alive that is actually due.
//
// Wrapping those would shrink what this line has to confess without removing
// the need to confess it, and would change the number of timed regions, which
// is what an A/B against an earlier build compares. So the contents are named
// instead. The line is 0.476 s and flat across the #99 change (0.477 before,
// 0.476 after), so nothing in the mix moved when the wire did.
//
// The point of measuring it here rather than under a profiler is that a soak
// runs on whatever box it runs on, and "how much of this is instrument" has
// to be answerable there. #93 needed perf on the test box to find that the
// simulator held 38.6% of cycles; this answers the same question anywhere,
// including hosts where a sampling profiler is not available at all.
//
// THE FAILURE STATE. A wall-clock attribution can go wrong in one direction
// that matters: the parts can come to more than the whole, which means the
// buckets are double-charging and every share printed is a fiction. That is
// checked, and it refuses the whole report rather than printing any of it.
// Note what that check can and cannot see -- it compares the SUM against the
// wall clock, so it catches double-charging and a clock that ran backwards,
// and it cannot catch time charged to the wrong bucket, which leaves the sum
// untouched. Only the wrapped call sites below rule that out.
//
// The instrument also knows what its own clock costs, calibrated at startup
// rather than assumed. A bucket whose time is not comfortably larger than the
// timer that collected it gets its figure printed WITH that said, not
// withheld: an earlier version suppressed the number instead, which put a
// refusal exactly where the flattering reading was. What is withheld is the
// confidence, stated beside the figure it qualifies.

enum { SOAK_PROFILE_WIRE, SOAK_PROFILE_PRODUCT, SOAK_PROFILE_BUCKETS };

static const char * soak_profile_bucket_name[SOAK_PROFILE_BUCKETS] =
{
    "the wire      (harness: network simulator)",
    "the protocol  (product: client + server + crypto)",
};

static int      soak_profile_enabled;
static int      soak_profile_double_charge;             // the negative control
static double   soak_profile_seconds[SOAK_PROFILE_BUCKETS];
static uint64_t soak_profile_regions[SOAK_PROFILE_BUCKETS];
static double   soak_profile_timer_cost;                // seconds per timed region, measured

static inline double soak_profile_now()
{
    return soak_profile_enabled ? rocketnet_platform_time() : 0.0;
}

static inline void soak_profile_charge( int bucket, double start )
{
    if ( !soak_profile_enabled )
        return;
    const double elapsed = rocketnet_platform_time() - start;
    soak_profile_seconds[bucket] += elapsed;
    soak_profile_regions[bucket]++;
    if ( soak_profile_double_charge )
    {
        // the negative control: charge every bucket twice, so the parts add up
        // to more than the wall clock they are dividing. Every bucket and not
        // just one, because the wire is small enough that doubling it alone
        // does not break the sum -- a negative control has to stay able to
        // break the thing it is controlling for.
        soak_profile_seconds[bucket] += elapsed;
    }
}

#define SOAK_PROFILE( bucket, ... )                                             \
    do {                                                                        \
        const double soak_profile_start = soak_profile_now();                   \
        __VA_ARGS__;                                                            \
        soak_profile_charge( bucket, soak_profile_start );                      \
    } while ( 0 )

// what one timed region costs on this machine, so a bucket can be compared
// against its own instrument. Measured, not assumed -- it differs by an order
// of magnitude between a vDSO clock and a trapping one.

static void soak_profile_calibrate()
{
    // five batches, median batch taken. A single batch reads 30-50% apart run
    // to run on a busy machine, and this cost is what decides whether a
    // bucket's figure carries the NOT SUPPORTABLE caveat -- an unstable
    // threshold would make the instrument's own honesty a coin flip. It is
    // also why the NET share moves between runs while the gross does not:
    // net subtracts this, and this is the unstable term.

    const int batches = 5;
    const int samples = 50000;
    double cost[batches];

    for ( int b = 0; b < batches; b++ )
    {
        const double start = rocketnet_platform_time();
        double sink = 0.0;
        for ( int i = 0; i < samples; i++ )
        {
            const double t0 = rocketnet_platform_time();
            sink += rocketnet_platform_time() - t0;
        }
        cost[b] = ( rocketnet_platform_time() - start ) / (double) samples;
        (void) sink;
    }

    for ( int i = 1; i < batches; i++ )
    {
        for ( int j = i; j > 0 && cost[j] < cost[j-1]; j-- )
        {
            const double swap = cost[j]; cost[j] = cost[j-1]; cost[j-1] = swap;
        }
    }

    soak_profile_timer_cost = cost[batches / 2];
}

// the wire's two calls, wrapped so every one of them is charged. receive()
// lives inside a while condition, which is why these are functions and the
// protocol calls are a macro.

static void wire_send( struct rocketnet_network_simulator_t * simulator,
                       const struct rocketnet_address_t * from,
                       const struct rocketnet_address_t * to,
                       const uint8_t * packet_data,
                       int packet_bytes,
                       double current_time )
{
    const double start = soak_profile_now();
    rocketnet_network_simulator_send( simulator, from, to, packet_data, packet_bytes, current_time );
    soak_profile_charge( SOAK_PROFILE_WIRE, start );
}

static int wire_receive( struct rocketnet_network_simulator_t * simulator,
                         const struct rocketnet_address_t * to,
                         double current_time,
                         struct rocketnet_address_t * out_from,
                         uint8_t * out_packet )
{
    const double start = soak_profile_now();
    const int bytes = rocketnet_network_simulator_receive( simulator, to, current_time, out_from, out_packet );
    soak_profile_charge( SOAK_PROFILE_WIRE, start );
    return bytes;
}

// returns nonzero if the attribution refused itself

static int soak_profile_report( double total_seconds, uint64_t iterations )
{
    if ( !soak_profile_enabled )
        return 0;

    double accounted = 0.0;
    uint64_t regions = 0;
    for ( int i = 0; i < SOAK_PROFILE_BUCKETS; i++ )
    {
        accounted += soak_profile_seconds[i];
        regions += soak_profile_regions[i];
    }

    printf( "\nsoak_sim profile: %llu iterations in %.3f s (%.0f iterations/sec)\n",
            (unsigned long long) iterations, total_seconds,
            total_seconds > 0.0 ? (double) iterations / total_seconds : 0.0 );
    printf( "  timer cost measured at %.1f ns per timed region, %llu regions charged (%.3f s of the total is this instrument)\n",
            soak_profile_timer_cost * 1e9, (unsigned long long) regions, soak_profile_timer_cost * (double) regions );

    if ( accounted > total_seconds )
    {
        printf( "\nPROFILE REFUSED: the buckets account for %.3f s of a run that lasted %.3f s.\n"
                "The parts exceed the whole, so every share below would be a fiction. No shares printed.\n",
                accounted, total_seconds );
        fflush( stdout );
        fprintf( stderr, "PROFILE REFUSED: accounted %.3f s exceeds total %.3f s\n", accounted, total_seconds );
        return 1;
    }

    int unsupportable = 0;
    for ( int i = 0; i < SOAK_PROFILE_BUCKETS; i++ )
    {
        const double instrument = soak_profile_timer_cost * (double) soak_profile_regions[i];
        const double gross = 100.0 * soak_profile_seconds[i] / total_seconds;
        const double net = 100.0 * ( soak_profile_seconds[i] - instrument ) / total_seconds;

        printf( "  %-50s %8.3f s  %5.1f%% gross  %5.1f%% net of its own %.3f s of timer\n",
                soak_profile_bucket_name[i], soak_profile_seconds[i], gross, net, instrument );

        // The number is always printed. Withholding it would be its own kind
        // of dishonesty -- a refusal that happens to sit where the flattering
        // reading is -- so what gets withheld is the CONFIDENCE, stated in the
        // same breath as the figure it qualifies.

        if ( soak_profile_seconds[i] < instrument * 4.0 )
        {
            printf( "  %-50s   ^ NOT SUPPORTABLE: the net figure is a difference of two numbers within 4x\n"
                    "  %-50s     of each other, so its error bars are the same size as itself. Read the\n"
                    "  %-50s     gross %.1f%% as an upper bound and the net %.1f%% as an estimate, not a\n"
                    "  %-50s     measurement. The bucket has become too cheap for this timer to divide.\n",
                    "", "", "", gross, net, "" );
            unsupportable = 1;
        }
    }

    printf( "  %-50s %8.3f s  %5.1f%%\n",
            "the remainder (total minus the two above)",
            total_seconds - accounted,
            100.0 * ( total_seconds - accounted ) / total_seconds );
    printf( "  %-50s   ^ NOT a bucket and NOT harness-only: it is whatever was never wrapped.\n"
            "  %-50s     Mostly the soak's own work -- payload generate and verify, connect token\n"
            "  %-50s     assembly, the watchdog -- but it also holds randombytes_buf twice per\n"
            "  %-50s     connect attempt (libsodium, doing the backend's job here) and two library\n"
            "  %-50s     lookups per due keep-alive. Named because a remainder cannot be pure.\n",
            "", "", "", "", "" );

    // What the accounting check above can and cannot see, said here rather
    // than left for a reader to assume. It compares the SUM against the wall
    // clock, so it catches double-charging and a clock that ran backwards. It
    // cannot see a single bucket charged with work that belongs to another
    // one: that moves time between buckets and leaves the sum untouched. The
    // guard against THAT is not arithmetic, it is that each bucket's contents
    // are a short explicit list of calls in this file.

    printf( "\n  The parts-within-the-whole check passed. It compares the sum against the wall clock,\n"
            "  so it sees double-charging and a clock that went backwards. It CANNOT see time charged\n"
            "  to the wrong bucket, which leaves the sum unchanged -- only reading the wrapped call\n"
            "  sites in soak_sim.cpp can rule that out.\n" );

    if ( unsupportable )
        printf( "\n  One or more buckets is at the level of the instrument measuring it. The figures are\n"
                "  printed with that said, rather than withheld.\n" );

    fflush( stdout );
    return 0;
}

void interrupt_handler( int signal_value )
{
    (void) signal_value; quit = 1;
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

static int verify_payload( const uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        if ( payload_data[i] != (uint8_t) ( start + i ) )
            return 0;
    }
    return 1;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    uint64_t max_iterations = 0;
    if ( argc > 1 )
    {
        char * end = NULL;
        errno = 0;
        max_iterations = strtoull( argv[1], &end, 10 );
        if ( argv[1][0] < '0' || argv[1][0] > '9' || !end || *end || errno || !max_iterations )
        {
            fprintf( stderr, "soak_sim: iteration count must be a positive integer\n" );
            return 1;
        }
    }

    // the profile, and its negative control

    char profile_string[256];
    if ( rocketnet_test_getenv( "ROCKETNET_SOAK_SIM_PROFILE", profile_string, sizeof( profile_string ) ) && profile_string[0] == '1' )
    {
        soak_profile_enabled = 1;
        soak_profile_calibrate();
        printf( "soak_sim: wall-clock attribution ENABLED\n" );
    }

    char accounting_fault_string[256];
    if ( rocketnet_test_getenv( "ROCKETNET_SOAK_SIM_FAULT_ACCOUNTING", accounting_fault_string, sizeof( accounting_fault_string ) ) && accounting_fault_string[0] == '1' )
    {
        soak_profile_double_charge = 1;
        printf( "soak_sim: accounting fault ARMED -- the profile must refuse itself\n" );
    }

    // the server, on the development keys, at the vector server address

    struct rocketnet_counters_t server_counters;
    memset( &server_counters, 0, sizeof(server_counters) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    server.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    server.send_rate = 10.0;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_dev_server_secret_key, 32 );
    rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" );
    randombytes_buf( server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server.cookie_lifetime_seconds = 3;
    server.cookie_rotation_seconds = 60;
    server.cookie_key_rotation_time = 100.0 + (double) server.cookie_rotation_seconds;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 32 );
    if ( !server.encryption_map )
    {
        printf( "soak_sim: failed to create the encryption map\n" );
        rocketnet_term();
        return 1;
    }
    server.used_token_history = rocketnet_used_token_history_create( NULL, 256 );
    if ( !server.used_token_history )
    {
        printf( "soak_sim: failed to create the used token history\n" );
        rocketnet_term();
        return 1;
    }
    server.client_slots = rocketnet_client_slots_create( NULL, 16 );
    if ( !server.client_slots )
    {
        printf( "soak_sim: failed to create the client slots\n" );
        rocketnet_term();
        return 1;
    }
    server.acks = rocketnet_server_acks_create( NULL, 16 );
    if ( !server.acks )
    {
        printf( "soak_sim: failed to create the server acks\n" );
        rocketnet_term();
        return 1;
    }
    server.reassembly = rocketnet_reassembly_set_create( NULL, 16, 8, 16384, 16 * 8 );
    if ( !server.reassembly )
    {
        printf( "soak_sim: failed to create the reassembly set\n" );
        rocketnet_term();
        return 1;
    }
    server.counters = &server_counters;

    #define SOAK_NUM_CLIENTS 8

    // These addresses are simulated: every one of them is a key into the
    // in-memory network simulator and no socket is ever created for any of
    // them, so the fixed ports here are not the #107 collision hazard and
    // must NOT be converted to port zero -- the documentation ranges are
    // deliberate, and two soak_sim processes on one box share nothing.

    struct rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "203.0.113.10:40000" );

    struct rocketnet_address_t client_address[SOAK_NUM_CLIENTS];
    for ( int i = 0; i < SOAK_NUM_CLIENTS; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "198.51.100.%d:56789", 10 + i );
        rocketnet_address_parse( &client_address[i], address_string );
    }

    // the hostile wire

    // light loss: every attempt SHOULD complete, so a single terminal state
    // during the soak is a finding -- a dropped packet the state machine did
    // not tolerate. the harsh configuration lives in the functional tests,
    // bounded and seeded.

    // the seed is random per run unless pinned: a soak wants a different wire
    // every night, an A/B wants the same one twice. a pinned seed is echoed,
    // so a run's own output says which wire it ran on.

    char seed_string[256];
    const int seed_pinned = rocketnet_test_getenv( "ROCKETNET_SOAK_SIM_SEED", seed_string, sizeof( seed_string ) );
    const uint64_t simulator_seed = seed_pinned ? strtoull( seed_string, NULL, 10 ) : rocketnet_random_uint64();
    if ( seed_pinned )
        printf( "soak_sim: simulator seed pinned to %llu\n", (unsigned long long) simulator_seed );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 4096, simulator_seed );
    rocketnet_network_simulator_config( simulator, 0.05, 0.02, 2.0f, 2.0f );

    // the wire's audit and its negative control. the audit is the wire's
    // failure state: it re-runs the reference scan beside the index on every
    // receive and aborts on divergence. the fault arms a deliberate index
    // corruption so the audit can be WATCHED failing rather than assumed to.

    char audit_string[256];
    if ( rocketnet_test_getenv( "ROCKETNET_SOAK_SIM_AUDIT", audit_string, sizeof( audit_string ) ) && audit_string[0] == '1' )
    {
        rocketnet_network_simulator_audit( simulator, 1 );
        printf( "soak_sim: simulator receive audit ENABLED (slow -- proof mode, not soak mode)\n" );
    }

    char index_fault_string[256];
    const uint64_t index_fault_iteration = rocketnet_test_getenv( "ROCKETNET_SOAK_SIM_FAULT_INDEX", index_fault_string, sizeof( index_fault_string ) ) ? strtoull( index_fault_string, NULL, 10 ) : 0;
    if ( index_fault_iteration )
        printf( "soak_sim: simulator index fault ARMED for iteration %llu -- the audit must go red\n",
                (unsigned long long) index_fault_iteration );

    static struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    static struct rocketnet_client_protocol_t clients[SOAK_NUM_CLIENTS];
    int last_state[SOAK_NUM_CLIENTS];
    double last_state_change_time[SOAK_NUM_CLIENTS];
    double connected_since[SOAK_NUM_CLIENTS];
    for ( int i = 0; i < SOAK_NUM_CLIENTS; i++ )
    {
        rocketnet_client_protocol_init( &clients[i], &client_counters );
        last_state[i] = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
        last_state_change_time[i] = 100.0;
        connected_since[i] = 0.0;
    }

    double time = 100.0;
    uint64_t timestamp = ROCKETNET_TEST_CREATE_TS;
    uint64_t iteration = 0;
    uint64_t attempts = 0;
    uint64_t connects = 0;
    uint64_t terminals = 0;
    uint64_t payloads_verified = 0;
    uint64_t fragmented_sent = 0;
    uint64_t fragmented_verified = 0;
    uint64_t failures = 0;
    uint64_t hangs = 0;
    int payload_size = 1;
    int fragmented_payload_size = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1;
    uint64_t client_id = 1;


    const double soak_started = rocketnet_platform_time();

    while ( !quit )
    {
        iteration++;
        if ( max_iterations && iteration > max_iterations )
            break;

        if ( index_fault_iteration && iteration == index_fault_iteration )
            rocketnet_network_simulator_inject_index_fault( simulator );

        time += 0.005;                              // mocked time, stepped fast
        timestamp = ROCKETNET_TEST_CREATE_TS + (uint64_t) ( time - 100.0 );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes = 0;

        for ( int c = 0; c < SOAK_NUM_CLIENTS; c++ )
        {
        struct rocketnet_client_protocol_t & client = clients[c];

        // a fresh attempt whenever the machine is at rest or terminal --
        // recovery is the ordinary path, run constantly

        if ( client.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        {
            if ( client.state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            {
                // under light loss every attempt must complete: a terminal
                // is a client that failed to connect or got stuck -- found it
                printf( "CLIENT %d FAILED: reached '%s' under light loss -- the state machine did not tolerate a drop\n",
                        c, rocketnet_client_transport_state_name( client.state ) );
                terminals++;
                failures++;
            }

            uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
            struct rocketnet_signed_connect_data_t signed_fields;
            struct rocketnet_connect_token_private_t private_fields;
            memset( &signed_fields, 0, sizeof(signed_fields) );
            memset( &private_fields, 0, sizeof(private_fields) );
            signed_fields.version = ROCKETNET_WIRE_VERSION;
            signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
            signed_fields.create_timestamp = timestamp;
            signed_fields.expire_timestamp = timestamp + 300;
            signed_fields.timeout_seconds = 10;
            signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
            const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
            memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
            memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );
            private_fields.client_id = client_id++;
            randombytes_buf( private_fields.client_to_server_key, 32 );
            randombytes_buf( private_fields.server_to_client_key, 32 );
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_dev_backend_secret_key, token_data ) );

            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                rocketnet_client_protocol_connect( &client, token_data, time, timestamp ) );
            attempts++;
        }

        // the watchdog: a machine that neither progresses nor terminally
        // fails is a wedge. per-state timeouts are 10 seconds, the handshake
        // deadline 5, the total deadline 300 -- sixty seconds of sim time
        // with no transition is a hang, full stop.

        if ( client.state != last_state[c] )
        {
            if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                connects++;
                connected_since[c] = time;
            }
            last_state[c] = client.state;
            last_state_change_time[c] = time;
        }
        else if ( client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && time - last_state_change_time[c] > 60.0 )
        {
            printf( "HANG: client %d wedged in '%s' for 60 sim seconds\n", c, rocketnet_client_transport_state_name( client.state ) );
            hangs++;
            quit = 1;
            break;
        }

        // a connected client cycles off on its own staggered period, through
        // the burst, so disconnect and reconnect stay routine

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && time - connected_since[c] > 20.0 + 3.0 * c )
        {
            uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
            int burst_count = 0;
            int burst_bytes = 0;
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                rocketnet_client_protocol_disconnect( &client, burst, &burst_count, &burst_bytes ) );
            for ( int i = 0; i < burst_count; i++ )
                wire_send( simulator, &client_address[c], &server_address,
                           burst + i * ROCKETNET_SEND_BUDGET,
                           burst_bytes, time );
        }

        // client drives, wire carries, server answers, wire carries back

        SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
            rocketnet_client_protocol_update( &client, time, packet, &packet_bytes ) );
        if ( packet_bytes > 0 )
            wire_send( simulator, &client_address[c], &server_address, packet, packet_bytes, time );

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );
            int bytes = 0;
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                bytes = rocketnet_client_protocol_write_payload( &client, payload, payload_size, time, packet ) );
            if ( bytes > 0 )
                wire_send( simulator, &client_address[c], &server_address, packet, bytes, time );
            payload_size = payload_size < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? payload_size + 1 : 1;

            // an occasional fragmented payload, staggered per client, so
            // reassembly soaks under the same hostile wire: at this loss a
            // multi-fragment payload sometimes dies, which is its specified
            // semantics -- the abandoned counter records it, and nothing
            // else in the fragment family may ever fire

            if ( ( ( iteration + (uint64_t) c * 29 ) % 233 ) == 0 )
            {
                static uint8_t fragmented_payload[16384];
                generate_payload( fragmented_payload, fragmented_payload_size );
                static uint8_t fragment_packets[16 * ROCKETNET_SEND_BUDGET];
                int fragment_packet_bytes[16];
                int fragment_count = 0;
                SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                    fragment_count = rocketnet_client_protocol_write_fragments( &client, fragmented_payload, fragmented_payload_size,
                                                                               time, fragment_packets, fragment_packet_bytes, 16 ) );
                if ( fragment_count > 0 )
                {
                    for ( int i = 0; i < fragment_count; i++ )
                        wire_send( simulator, &client_address[c], &server_address,
                                   fragment_packets + i * ROCKETNET_SEND_BUDGET,
                                   fragment_packet_bytes[i], time );
                    fragmented_sent++;
                }
                fragmented_payload_size = fragmented_payload_size < 16384 - 997 ? fragmented_payload_size + 997 : ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1;
            }
        }

        }   // for each client

        struct rocketnet_address_t from;
        int bytes;
        SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
            rocketnet_reassembly_set_begin_update( server.reassembly ) );
        server.acks->num_acked = 0;
        while ( ( bytes = wire_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
        {
            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;
            uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
            int delivered_bytes = 0;
            int delivered_client_index = -1;
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, timestamp, time,
                                                          response, &response_bytes, delivered, &delivered_bytes, &delivered_client_index ) );
            if ( response_bytes > 0 )
                wire_send( simulator, &server_address, &from, response, response_bytes, time );
            if ( delivered_bytes > 0 )
            {
                if ( verify_payload( delivered, delivered_bytes ) )
                    payloads_verified++;
                else
                    { printf( "PAYLOAD VERIFY FAILED at server (%d bytes)\n", delivered_bytes ); failures++; }
            }
            if ( server.reassembly->completed_bytes > 0 )
            {
                if ( verify_payload( server.reassembly->completed_payload, server.reassembly->completed_bytes ) )
                    fragmented_verified++;
                else
                    { printf( "FRAGMENTED PAYLOAD VERIFY FAILED at server (%d bytes)\n", server.reassembly->completed_bytes ); failures++; }
            }
        }

        SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
            rocketnet_server_protocol_update( &server, time ) );
        for ( int i = 0; i < 16; i++ )
        {
            uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
            int keep_alive_bytes = 0;
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server, i, time, keep_alive ) );
            if ( keep_alive_bytes > 0 )
            {
                int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                wire_send( simulator, &server_address, to, keep_alive, keep_alive_bytes, time );
            }

            uint8_t ack_packet[ROCKETNET_RECEIVE_CEILING];
            int ack_bytes = 0;
            SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                ack_bytes = rocketnet_server_protocol_write_ack_if_due( &server, i, time, ack_packet ) );
            if ( ack_bytes > 0 )
            {
                int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                wire_send( simulator, &server_address, to, ack_packet, ack_bytes, time );
            }
        }

        for ( int c = 0; c < SOAK_NUM_CLIENTS; c++ )
        {
            while ( ( bytes = wire_receive( simulator, &client_address[c], time, &from, packet ) ) > 0 )
            {
                uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
                int delivered_bytes = 0;
                bool processed = false;
                SOAK_PROFILE( SOAK_PROFILE_PRODUCT,
                    processed = rocketnet_client_protocol_process_packet( &clients[c], packet, bytes, time, delivered, sizeof(delivered), &delivered_bytes ) );
                if ( processed && delivered_bytes > 0 )
                {
                    if ( verify_payload( delivered, delivered_bytes ) )
                        payloads_verified++;
                    else
                        { printf( "PAYLOAD VERIFY FAILED at client %d (%d bytes)\n", c, delivered_bytes ); failures++; }
                }
            }
        }

        if ( ( iteration % 100000 ) == 0 )
        {
            printf( "soak_sim: %llu iterations, %.0f sim seconds, %llu attempts, %llu connects, %llu terminals, %llu payloads verified, %llu/%llu fragmented verified (%llu abandoned), %llu failures\n",
                    (unsigned long long) iteration, time - 100.0, (unsigned long long) attempts,
                    (unsigned long long) connects, (unsigned long long) terminals,
                    (unsigned long long) payloads_verified,
                    (unsigned long long) fragmented_verified, (unsigned long long) fragmented_sent,
                    (unsigned long long) server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED],
                    (unsigned long long) failures );
        }
    }

    const double soak_elapsed = rocketnet_platform_time() - soak_started;

    // The counter discipline at exit, computed from the counter list and
    // never kept as a second list here. Two rules, on both ends:
    //
    //   conservation: every family balances, RECEIVED == the sum of its
    //   outcomes, at the server and across the clients' shared block. The
    //   run has ended between updates, so every arrival has taken its outcome.
    //
    //   clean-run: every REFUSAL counter reads 0, except the refusals the
    //   faults this wire injects explain. The wire loses 2% of packets,
    //   duplicates 2%, and jitters delivery by 20 ms around a 50 ms latency,
    //   so the exceptions are the refusals that loss, duplication and
    //   reordering cause by themselves, each row naming the fault. A refusal
    //   not in the table that moved is a defect in the sender or the
    //   receiver, however long the soak ran.

    struct soak_explained_refusal_t
    {
        int counter;
        const char * fault;
    };

    // Found by running the pinned and 24 random seeds with the table empty, and
    // confirmed one fault at a time: loss alone moves only the history
    // overflow, duplication alone moves only the ALREADY_RECEIVED rows, and
    // jitter alone moves nothing.

    static const soak_explained_refusal_t soak_explained_refusals[] =
    {
        { ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW,              "loss: a lost keep-alive takes its acks with it, so sent packets age out of the history unacknowledged" },
        { ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED,        "duplication: the wire delivers a keep-alive twice, the second is a replay" },
        { ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED,           "duplication: the wire delivers a payload twice, the second is a replay" },
        { ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED,          "duplication: the wire delivers a fragment twice, the second is a replay" },
        { ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED, "duplication: the wire delivers a keep-alive twice, the second is a replay" },
        { ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED,               "duplication: the wire delivers a standalone ack twice, the second is a replay" },
        { ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED,        "duplication: the wire delivers a standalone ack twice, the second is a replay" },
        { -1, NULL }
    };

    struct soak_end_t
    {
        const char * name;
        const struct rocketnet_counters_t * counters;
    };

    const soak_end_t soak_ends[2] = { { "server", &server_counters }, { "clients", &client_counters } };

    int conservation_ok = 1;
    for ( int e = 0; e < 2; e++ )
    {
        char report[256];
        if ( !rocketnet_counters_conserved( soak_ends[e].counters, report, sizeof(report) ) )
        {
            printf( "CONSERVATION FAILED at %s: %s\n", soak_ends[e].name, report );
            conservation_ok = 0;
            failures++;
        }
    }
    if ( conservation_ok )
        printf( "soak_sim: conservation ok (server and clients)\n" );

    for ( int e = 0; e < 2; e++ )
    {
        for ( int i = 0; i < rocketnet_num_counters(); i++ )
        {
            const uint64_t value = rocketnet_counter_value( soak_ends[e].counters, i );
            if ( value == 0 || rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL )
                continue;
            const char * fault = NULL;
            for ( int j = 0; soak_explained_refusals[j].fault; j++ )
                if ( soak_explained_refusals[j].counter == i )
                    fault = soak_explained_refusals[j].fault;
            if ( fault )
            {
                printf( "soak_sim: explained refusal at %s: %s = %llu (%s)\n", soak_ends[e].name,
                        rocketnet_counter_name( i ), (unsigned long long) value, fault );
                continue;
            }
            printf( "UNEXPLAINED REFUSAL at %s: %s = %llu\n", soak_ends[e].name,
                    rocketnet_counter_name( i ), (unsigned long long) value );
            failures++;
        }
    }

    printf( "\nsoak_sim finished: %llu iterations, %.0f sim seconds, %llu attempts, %llu connects, %llu terminals, %llu payloads verified, %llu/%llu fragmented verified (%llu abandoned), %llu failures, %llu hangs\n",
            (unsigned long long) iteration, time - 100.0, (unsigned long long) attempts,
            (unsigned long long) connects, (unsigned long long) terminals,
            (unsigned long long) payloads_verified,
            (unsigned long long) fragmented_verified, (unsigned long long) fragmented_sent,
            (unsigned long long) server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED],
            (unsigned long long) failures, (unsigned long long) hangs );

    // a refused attribution is a failed run: the instrument said its own
    // numbers were not trustworthy, and a soak that cannot say where its time
    // went has not measured what it claims to measure.

    const int profile_refused = soak_profile_report( soak_elapsed, iteration );

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );
    rocketnet_network_simulator_destroy( simulator );

    rocketnet_term();

    return ( failures || hangs || terminals || profile_refused ) ? 1 : 0;
}
