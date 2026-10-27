/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The differential fuzz target (nightly, Linux leg): mutate the abstract
    admission CASE both deciders see, run rocketnet and netcode 1.02 on that
    one case, and abort when rocketnet is weaker (netcode refuses, rocketnet
    admits) or when rocketnet is stricter with a reason class that is not on
    tools/differential/README.md's permitted-stricter list. The deciders are
    the same code the fixed gate runs (tools/differential/decide.h).

    The input is never packet bytes: the two wire formats differ, so the
    fuzzer mutates the case. The structured case is a fixed 25-byte record:

      [0]      address_mode: 0 advertised IPv4, 1 wrong IPv4, 2 IPv6
               family, 3 wrong port
      [1]      protocol_wrong: 0 the test protocol id, nonzero a wrong id
      [2]      flip_authenticator: 0 no, nonzero flip one signature/MAC byte
      [3]      replay: 0 no, nonzero present the same token from a second
               address
      [4]      occupied_slots: how many client slots are taken before the
               candidate request
      [5..12]  create_offset, int64 little-endian seconds relative to the
               server's now (rocketnet side)
      [13..20] expire_offset, int64 little-endian seconds relative to now
      [21..24] timeout_seconds, int32 little-endian (0 and negative allowed)

    The netcode side reads its own wall clock for expiry, so both offsets are
    relative to the moment netcode reads it: a negative expire offset maps to
    expire_seconds 0 (already expired), any other to the same count of
    seconds (README, "The netcode side, live"). That makes a crash input
    reproduce. Seeds: one per case in cases.tsv, encoded here
    (fuzz/corpus_differential/); the dictionary under fuzz/dict/ lists the
    field tags.

    The verdict is rocketnet-weaker (abort), rocketnet-stricter (abort only
    if the reason is not a permitted stricter class), or agree. A mint
    failure on either side is an input-encoding artifact, not a decision, so
    it is not judged.
*/

#include <stdint.h>
#include <stddef.h>

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

#include "decide.h"

#include <stdlib.h>
#include <string.h>
#include <sodium.h>

static uint64_t decode_u64( const uint8_t * p )
{
    uint64_t v = 0;
    for ( int i = 0; i < 8; i++ )
        v |= (uint64_t) p[i] << ( 8 * i );
    return v;
}

static uint32_t decode_u32( const uint8_t * p )
{
    uint32_t v = 0;
    for ( int i = 0; i < 4; i++ )
        v |= (uint32_t) p[i] << ( 8 * i );
    return v;
}

// Offsets are bounded so now + offset stays a small positive uint64 on both
// sides: a huge negative expire offset would underflow rocketnet's uint64
// expire timestamp into the far future (not "expired"), which netcode cannot
// express at all, so the comparison would be meaningless.
#define DIFFERENTIAL_MAX_OFFSET  100000000

static int64_t s64( int64_t v )
{
    if ( v > DIFFERENTIAL_MAX_OFFSET )
        return DIFFERENTIAL_MAX_OFFSET;
    if ( v < -DIFFERENTIAL_MAX_OFFSET )
        return -DIFFERENTIAL_MAX_OFFSET;
    return v;
}

static void decode_case( const uint8_t * data, size_t size, struct differential_case_t * c )
{
    memset( c, 0, sizeof( *c ) );
    if ( size < 25 )
        return;
    c->address_mode = data[0] % 4;
    c->protocol_wrong = data[1] != 0;
    c->flip_authenticator = data[2] != 0;
    c->replay = data[3] != 0;
    c->occupied_slots = data[4];
    c->create_offset = s64( (int64_t) decode_u64( data + 5 ) );
    c->expire_offset = s64( (int64_t) decode_u64( data + 13 ) );
    c->timeout_seconds = (int32_t) decode_u32( data + 21 );
}

static void report_case( const struct differential_case_t * c, const struct decision_t * rn,
                         const char * nc_reason, const char * v )
{
    fprintf( stderr, "differential finding: verdict %s\n", v );
    fprintf( stderr, "  case: create=%+lld expire=%+lld timeout=%d prot_wrong=%u "
                     "addr=%u flip=%u occ=%u replay=%u\n",
             (long long) c->create_offset, (long long) c->expire_offset,
             c->timeout_seconds, c->protocol_wrong, c->address_mode,
             c->flip_authenticator, c->occupied_slots, c->replay );
    fprintf( stderr, "  rocketnet: %s\n", rn->accept ? "accept" : rn->reason );
    fprintf( stderr, "  netcode: %s\n", nc_reason );
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    if ( sodium_init() < 0 )
        abort();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    netcode_log_level( NETCODE_LOG_LEVEL_DEBUG );
    netcode_set_printf_function( netcode_log_sink );
    if ( netcode_init() != NETCODE_OK )
        abort();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    struct differential_case_t c;
    decode_case( data, size, &c );

    const uint64_t now = ROCKETNET_TEST_CREATE_TS;

    struct decision_t rn = rocketnet_decide_case( &c, now );
    char nc_reason[64];
    (void) netcode_decide_case_live( &c, now, nc_reason, sizeof( nc_reason ) );

    // a mint/capture failure on either side is an encoding artifact, not a
    // decision; skip the verdict
    if ( strcmp( rn.reason, "mint" ) == 0 ||
         strncmp( nc_reason, "refuse:mint", 11 ) == 0 ||
         strcmp( nc_reason, "refuse:unclassified" ) == 0 )
        return 0;

    const char * v = verdict( &rn, nc_reason );
    if ( strcmp( v, "rocketnet-weaker" ) == 0 )
    {
        report_case( &c, &rn, nc_reason, v );
        abort();
    }
    if ( strcmp( v, "rocketnet-stricter" ) == 0 )
    {
        // a permitted stricter class (timeout, future) is documented in
        // README and tolerated; any other stricter class is a finding and
        // is listed with its reason before aborting
        if ( !stricter_class_permitted( rn.reason ) )
        {
            report_case( &c, &rn, nc_reason, v );
            abort();
        }
    }
    return 0;
}

#else

// built without the netcode checkout: nothing to compare
extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    (void) data;
    (void) size;
    return 0;
}

#endif
