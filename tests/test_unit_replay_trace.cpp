/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A differential check of the code's replay window against a hand-written
// C++ reference of the ReplayWindow model's state (proofs/tla/ReplayWindow.tla,
// proofs/tla/MCReplayWindow.tla, proofs/tla/README-ReplayWindow.md). It is not
// a TLC trace: no TLC behaviour is read or replayed here. The reference -- the
// set of received sequences and the window floor -- is stepped in lockstep
// with the code's rocketnet_replay_already_received / rocketnet_replay_advance
// over every advance sequence of length up to five on a small sequence
// alphabet, comparing after each step which sequences each side accepts as
// new. The enumeration runs twice: on an alphabet near 1, and on the same
// alphabet moved up so its largest sequence is UINT64_MAX, so the window's
// arithmetic is compared at the top of the sequence space as well.
//
// The reference carries the model's ClearedWords constant (the CONSTANTS of
// proofs/tla/ReplayWindow.tla) at the rule value the code implements,
// ClearedWords = Words (a long skip clears the whole ring). The model's
// other rule, GateBeforeAdvance = TRUE (the server checks the slot before it
// advances the window), is not part of the raw advance compared here; the
// two named witness tests below assert both rules against the code itself.

#include "test_unit_registry.h"

#include "rocketnet_replay.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <set>

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// ---------------------------------------------------------------------------
// The pure C++ reference of the model's state. W is the code's real width
// (ROCKETNET_REPLAY_WINDOW_BITS); the model's small instance (W = 12,
// Words = 6, WordBits = 2, proofs/tla/MCReplayWindow.cfg) is the same ring at that
// width, and the small-alphabet sequences below are scaled to the real width
// exactly as README-ReplayWindow.md scales the witness.
// ---------------------------------------------------------------------------

static const int k_model_words = ROCKETNET_REPLAY_WINDOW_BITS / 64;

struct replay_window_ref_t
{
    // model constant (the CONSTANTS of proofs/tla/ReplayWindow.tla)
    int cleared_words;          // ClearedWords: words the long-skip branch clears

    // model state (the VARIABLES of proofs/tla/ReplayWindow.tla)
    uint64_t highest;                       // highest_opened
    uint64_t window[k_model_words];         // the ring, the code's exact bit layout
    std::set<uint64_t> received;            // ghost: in-range sequences that opened

    void reset( int cleared_words_ )
    {
        cleared_words = cleared_words_;
        highest = 0;
        memset( window, 0, sizeof( window ) );
        received.clear();
    }

    // Floor (proofs/tla/ReplayWindow.tla)
    uint64_t floor() const
    {
        return highest >= (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS ? highest - ROCKETNET_REPLAY_WINDOW_BITS + 1 : 1;
    }

    static int bit_of( uint64_t s )
    {
        return (int) ( s % ROCKETNET_REPLAY_WINDOW_BITS );
    }

    // At or below highest - W, the window's floor less one: true when s is too
    // old. standard/TRANSPORT.md t14 states it as s + W <= highest; written
    // here without the addition, which wraps for s within W of UINT64_MAX.
    bool too_old( uint64_t s ) const
    {
        return s <= highest && highest - s >= (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS;
    }

    // Refused below the window floor or already recorded. standard/TRANSPORT.md
    // t14, on the received set: too old, or s is in received.
    // Not the ring's three bit-window tests.
    int already_received( uint64_t s ) const
    {
        if ( too_old( s ) )
        {
            return 1;
        }
        return received.count( s ) != 0;
    }

    void clear_bit( uint64_t s )
    {
        int b = bit_of( s );
        window[b / 64] &= ~( 1ULL << ( b % 64 ) );
    }

    void set_bit( uint64_t s )
    {
        int b = bit_of( s );
        window[b / 64] |= ( 1ULL << ( b % 64 ) );
    }

    void prune( std::set<uint64_t> & set_ )
    {
        // received' = { t in received : t + W > highest } (proofs/tla/ReplayWindow.tla,
        // Advance), that is every t not too old; ascending, so the dropped
        // entries are the smallest.
        std::set<uint64_t>::iterator it = set_.begin();
        while ( it != set_.end() && too_old( *it ) )
        {
            it = set_.erase( it );
        }
    }

    // Advance (proofs/tla/ReplayWindow.tla); the code's rocketnet_replay_advance.
    // TRANSPORT.md t14 Replay protection.
    void advance( uint64_t s )
    {
        if ( s > highest )
        {
            uint64_t skipped = s - highest;
            if ( skipped >= (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS )
            {
                // Cleared (proofs/tla/ReplayWindow.tla), the long-skip branch: clear every
                // word, ClearedWords = Words.
                for ( int w = 0; w < cleared_words; w++ )
                {
                    window[w] = 0;
                }
            }
            else
            {
                for ( uint64_t t = highest + 1; t < s; t++ )
                {
                    clear_bit( t );
                }
            }
            highest = s;
        }
        set_bit( s );
        received.insert( s );
        prune( received );
    }
};

// ---------------------------------------------------------------------------
// The trace: every action sequence of length up to five over the small
// alphabet, driving the reference and the real replay struct in lockstep.
// ---------------------------------------------------------------------------

static int g_replay_trace_steps = 0;

// added to every alphabet sequence and fixed probe: 0 for the run near 1,
// UINT64_MAX - 4397 for the run whose largest sequence is UINT64_MAX
static uint64_t g_replay_trace_base = 0;

static const uint64_t k_trace_alphabet[] = { 1, 2, 3, 300, 4096, 4097, 4397 };
static const int k_trace_alphabet_count = (int) ( sizeof( k_trace_alphabet ) / sizeof( k_trace_alphabet[0] ) );

// sequences that probe every branch, plus the dynamic floor/high-water
// neighbours, compared against the code after each step.
static const uint64_t k_trace_probes[] = {
    1,
    2,
    3,
    4,
    5,
    6,
    299,
    300,
    301,
    4095,
    4096,
    4097,
    4098,
    4395,
    4396,
    4397,
    4398,
    8191,
    8192,
    8193,
};
static const int k_trace_probe_count = (int) ( sizeof( k_trace_probes ) / sizeof( k_trace_probes[0] ) );

static void replay_trace_compare( const replay_window_ref_t & model, const struct rocketnet_replay_protection_t & real )
{
    for ( int i = 0; i < k_trace_probe_count; i++ )
    {
        uint64_t s = g_replay_trace_base + k_trace_probes[i];

        // the fixed probes past 4397 (8191 .. 8193) would wrap past
        // UINT64_MAX in the run at the top; they exist only for the run near 1

        if ( s < g_replay_trace_base )
        {
            continue;
        }
        rocketnet_unit_check( model.already_received( s ) == ( rocketnet_replay_already_received( &real, s ) != 0 ) );
    }

    uint64_t f = model.floor();
    uint64_t h = model.highest;
    uint64_t dynamic[6];
    int dn = 0;
    dynamic[dn++] = f;
    if ( f != UINT64_MAX )
    {
        dynamic[dn++] = f + 1;
    }

    // sequence 0 is out of the ring and the real code asserts it away
    // (rocketnet_replay_already_received, sdk/rocketnet_replay.cpp);
    // the model's already_received is likewise undefined at 0, so a probe
    // set built from highest == 0, or from highest + 1 at UINT64_MAX, must
    // never emit it.

    if ( h != 0 )
    {
        dynamic[dn++] = h;
    }

    if ( h != UINT64_MAX )
    {
        dynamic[dn++] = h + 1;
    }
    if ( h > 1 )
    {
        dynamic[dn++] = h - 1;
    }

    if ( f > 1 )
    {
        dynamic[dn++] = f - 1;
    }

    for ( int i = 0; i < dn; i++ )
    {
        rocketnet_unit_check( model.already_received( dynamic[i] ) == ( rocketnet_replay_already_received( &real, dynamic[i] ) != 0 ) );
    }
}

static void replay_trace_dfs( replay_window_ref_t & model, struct rocketnet_replay_protection_t & real, int depth )
{
    replay_trace_compare( model, real );
    g_replay_trace_steps++;

    if ( depth >= 5 )
    {
        return;
    }

    for ( int i = 0; i < k_trace_alphabet_count; i++ )
    {
        uint64_t s = g_replay_trace_base + k_trace_alphabet[i];
        if ( model.already_received( s ) )
        {
            continue;
        }

        replay_window_ref_t saved_model = model;
        struct rocketnet_replay_protection_t saved_real = real;

        // The trace drives the raw advance on both sides. The server's gate
        // before the advance (the rule GateBeforeAdvance = TRUE) is asserted
        // by the witness below.

        model.advance( s );
        rocketnet_replay_advance( &real, s );

        replay_trace_dfs( model, real, depth + 1 );

        model = saved_model;
        real = saved_real;
    }
}

// The reference-to-C++ differential: the code and the reference agree on the
// accepted projection after every advance sequence of length up to five, near
// sequence 1 and again with the largest alphabet sequence at UINT64_MAX.
static void test_replay_window_model_trace()
{
    const uint64_t bases[2] = { 0, UINT64_MAX - 4397 };
    for ( int b = 0; b < 2; b++ )
    {
        replay_window_ref_t model;
        model.reset( /* ClearedWords */ k_model_words );

        struct rocketnet_replay_protection_t real;
        memset( &real, 0, sizeof( real ) );

        g_replay_trace_base = bases[b];
        g_replay_trace_steps = 0;
        replay_trace_dfs( model, real, 0 );

        // the enumeration actually drove the ring past its floor and the
        // long-skip branch, not a degenerate prefix

        rocketnet_unit_check( g_replay_trace_steps >= 1000 );
    }

    g_replay_trace_base = 0;
}

// ---------------------------------------------------------------------------
// The two witnesses, by name. Each asserts a rule the code now implements:
// ClearedWords = Words for the long skip and GateBeforeAdvance = TRUE for the
// slot gate. Both are registered as ordinary tests and pass.
// ---------------------------------------------------------------------------

// F1 (rnfix-replay-long-skip-clears-window-r2): the
// ClearedWords = Words rule (the CONSTANTS of proofs/tla/ReplayWindow.tla) -- an
// advance of at
// least the window width forgets everything, so a genuine in-range packet is
// never refused as already received. TRANSPORT.md t14 Replay protection. The
// long skip now clears the whole ring, ClearedWords = Words
// (README-ReplayWindow.md "What TLC found" 1).
static void test_replay_long_skip_clears_the_whole_window()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    // the reversed witness, scaled to the real width: advance(2), advance(300),
    // advance(4397) -- the skip 300 -> 4397 is 4097, at least one window.

    rocketnet_replay_advance( &replay, 2 );
    rocketnet_replay_advance( &replay, 300 );
    rocketnet_replay_advance( &replay, 4397 );

    // a genuine packet 4396 is inside the retained range and never opened; the
    // rule says it must be accepted, not refused as already received.

    rocketnet_unit_check( !rocketnet_replay_already_received( &replay, 4396 ) );

    // the card's own shape at full width: fill 1..W, jump to 3W. every
    // sequence 2W+257 .. 3W-1 is new and must not read as received.

    struct rocketnet_replay_protection_t full;
    memset( &full, 0, sizeof( full ) );

    for ( uint64_t s = 1; s <= (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS; s++ )
    {
        rocketnet_replay_advance( &full, s );
    }

    rocketnet_replay_advance( &full, 3ULL * ROCKETNET_REPLAY_WINDOW_BITS );
    rocketnet_unit_check( !rocketnet_replay_already_received( &full, 2ULL * ROCKETNET_REPLAY_WINDOW_BITS + 257 ) );
    rocketnet_unit_check( !rocketnet_replay_already_received( &full, 3ULL * ROCKETNET_REPLAY_WINDOW_BITS - 1 ) );
}

// OV-34 (rnfix-replay-advanced-after-connect-check-r2): the GateBeforeAdvance
// rule (the CONSTANTS and Receive of proofs/tla/ReplayWindow.tla) -- a mapping without a slot never
// moves its session window, so the server checks the slot before the mark.
// TRANSPORT.md t10 step 12. The server now gates before the advance,
// GateBeforeAdvance = TRUE.
static void test_slotless_packet_never_moves_the_session_window()
{
    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.counters = &counters;
    rocketnet_unit_check( server.encryption_map != NULL );

    struct rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "198.51.100.7:56789" ) );

    int mapping_index = rocketnet_encryption_map_add( server.encryption_map, &client_address, 1000.0 );
    rocketnet_unit_check( mapping_index != -1 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server.encryption_map, mapping_index );
    rocketnet_unit_check( mapping->control->client_index == -1 );

    // the lost-response race the standard names: a mapping with session keys
    // but no slot yet. set the keys directly, as the challenge step would.

    memcpy( mapping->keys.session_client_to_server, rocketnet_test_session_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( mapping->connection_salt, rocketnet_test_salt, ROCKETNET_CONNECTION_SALT_BYTES );

    // a session payload sealed under the session key, at sequence 1: the fresh
    // 7-byte ack block then four payload bytes.

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + 4];
    memset( plaintext, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );

    const uint8_t type_data[4] = { 1, 2, 3, 4 };
    memcpy( plaintext + ROCKETNET_ACK_BLOCK_MIN_BYTES, type_data, 4 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, rocketnet_test_filter_value,
                                                      1, NULL, plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + 4,
                                                      ROCKETNET_TEST_PROTOCOL_ID, rocketnet_test_salt, NULL,
                                                      rocketnet_test_session_c2s_key );
    rocketnet_unit_check( packet_bytes != 0 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload_out[ROCKETNET_RECEIVE_CEILING];
    int payload_out_bytes = 0;
    int payload_client_index = -1;

    int result = rocketnet_server_protocol_process_packet( &server, &client_address, packet, packet_bytes,
                                                           ROCKETNET_TEST_CREATE_TS, 100.0,
                                                           response, &response_bytes,
                                                           payload_out, &payload_out_bytes, &payload_client_index );

    // the packet is refused as not connected ...

    rocketnet_unit_check( result == ROCKETNET_SERVER_PROCESS_IGNORED );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED] == 1 );

    // ... and the rule: the window of a mapping without a slot never moves

    rocketnet_unit_check( mapping->replay_session.highest_opened == 0 );

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_counters_assert_conserved( &counters );
}

REGISTER_UNIT_TEST( test_replay_window_model_trace );
REGISTER_UNIT_TEST( test_replay_long_skip_clears_the_whole_window );
REGISTER_UNIT_TEST( test_slotless_packet_never_moves_the_session_window );
