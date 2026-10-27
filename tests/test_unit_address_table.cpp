/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The address acceptance and equality table: one row per (source form, socket
// mode), checked against the receive edge's one rule for an ipv4-mapped
// source, rocketnet_address_accept_received_source (sdk/rocketnet_address.h:
// 49-71), and against rocketnet_address_equal (sdk/rocketnet_address.cpp:
// 202-237). Each row carries the accept decision, the canonical form the
// address takes after acceptance, and which of the other table forms it is
// equal to, so a mapped-versus-native mismatch or a wrong drop is a named row
// rather than a hidden one.
//
// The standard's rules, cited per row below:
//   STANDARD.md:334-340 -- "Address normalization is a law of this protocol
//     ... an IPv4-mapped IPv6 address (::ffff:a.b.c.d) must never exist. A
//     receiver converts a mapped source to native IPv4 exactly once, at the
//     socket boundary, before any address is used for a session table or an
//     encryption mapping."
//   STANDARD.md:330-333 -- "A server MAY advertise both an IPv4 and an IPv6
//     address, and it accepts a connection request arriving at either."
//   sdk/rocketnet_address.h:56-58 / RFC 4291 2.5.5.2 -- "anywhere the real
//     ipv6 header is seen -- a raw or xdp ipv6 path, or an ipv6-only socket --
//     a mapped source never occurs legitimately: it is spoofed, and dropped."
//
// The equality property is the seeded one below: equality is reflexive except
// for NONE, symmetric and transitive over 10,000 random address triples, on a
// fixed seed so a failure reproduces.

#include "test_unit_registry.h"

#include "rocketnet_address.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// the socket modes a source can arrive through. Only a dual stack socket
// (IPV6_V6ONLY off) delivers an ipv4 client as a mapped address, and it is the
// one mode that accepts and canonicalises it (sdk/rocketnet_address.h:51-58).
// ---------------------------------------------------------------------------

enum socket_mode_t
{
    SOCKET_V4_ONLY = 0,     // an ipv4 socket: native ipv4 sources
    SOCKET_DUAL_STACK = 1,  // IPV6_V6ONLY off: ipv4 clients arrive mapped
    SOCKET_V6_ONLY = 2,     // IPV6_V6ONLY on: the real ipv6 header is seen
    SOCKET_RAW = 3          // a raw or xdp ipv6 path: the real header is seen
};

static bool socket_mode_is_dual_stack( socket_mode_t mode )
{
    return mode == SOCKET_DUAL_STACK;
}

// the native forms the table compares against. Index 0 is the native ipv4 an
// accepted mapped source canonicalises to (STANDARD.md:336-338).

static const char * k_native_forms[] = {
    "1.2.3.4:40000",        // 0: native ipv4, non-zero address and port
    "[2001:db8::1]:40000",  // 1: native ipv6
    "0.0.0.0:0",            // 2: the zero address, port 0
    "[::]:0",               // 3: the zero ipv6 address, port 0
    "1.2.3.4:0"             // 4: same ipv4 as form 0, port 0 in the key
};

static const int k_native_form_count = (int) ( sizeof( k_native_forms ) / sizeof( k_native_forms[0] ) );

struct address_case_t
{
    const char * source;            // the source form; NULL means the all-zero NONE address
    socket_mode_t mode;             // the socket mode it arrives through
    bool expected_accept;           // expected accept decision of the receive edge
    const char * expected_canonical; // the string form after acceptance; NULL when dropped
    int expected_equal_form;        // the one native form it is equal to, or -1 for none
    const char * cite;              // the rule this row pins, in one line
};

static const struct address_case_t k_cases[] = {
    // native ipv4 passes through untouched on every mode (sdk/rocketnet_address.h:60-61).
    { "1.2.3.4:40000", SOCKET_V4_ONLY, true, "1.2.3.4:40000", 0, "STANDARD.md:330-333; native ipv4 accepted" },
    { "1.2.3.4:40000", SOCKET_DUAL_STACK, true, "1.2.3.4:40000", 0, "STANDARD.md:330-333; native ipv4 accepted" },
    { "1.2.3.4:40000", SOCKET_V6_ONLY, true, "1.2.3.4:40000", 0, "rocketnet_address.h:60-61; not mapped, unchanged" },
    { "1.2.3.4:40000", SOCKET_RAW, true, "1.2.3.4:40000", 0, "rocketnet_address.h:60-61; not mapped, unchanged" },

    // native ipv6 passes through untouched on every mode (sdk/rocketnet_address.h:60-61).
    { "[2001:db8::1]:40000", SOCKET_DUAL_STACK, true, "[2001:db8::1]:40000", 1, "rocketnet_address.h:60-61; not mapped, unchanged" },
    { "[2001:db8::1]:40000", SOCKET_V6_ONLY, true, "[2001:db8::1]:40000", 1, "rocketnet_address.h:60-61; not mapped, unchanged" },
    { "[2001:db8::1]:40000", SOCKET_RAW, true, "[2001:db8::1]:40000", 1, "rocketnet_address.h:60-61; not mapped, unchanged" },

    // the mapped source on a dual stack socket is canonicalised to native ipv4,
    // the same address an ipv4 socket sees (STANDARD.md:334-338).
    { "[::ffff:1.2.3.4]:40000", SOCKET_DUAL_STACK, true, "1.2.3.4:40000", 0, "STANDARD.md:334-338; mapped accepted and canonicalised" },

    // the same mapped source where the real ipv6 header is seen is spoofed and
    // dropped (sdk/rocketnet_address.h:56-58; RFC 4291 2.5.5.2).
    { "[::ffff:1.2.3.4]:40000", SOCKET_V6_ONLY, false, NULL, -1, "rocketnet_address.h:56-58; mapped dropped on v6-only" },
    { "[::ffff:1.2.3.4]:40000", SOCKET_RAW, false, NULL, -1, "rocketnet_address.h:56-58; mapped dropped on raw" },

    // the zero address, port 0: a real ipv4/ipv6 address, not NONE, and equal
    // only to itself (sdk/rocketnet_address.cpp:202-237 compares type, port and bytes).
    { "0.0.0.0:0", SOCKET_V4_ONLY, true, "0.0.0.0", 2, "STANDARD.md:386-392; zero ipv4 is an ipv4 endpoint" },
    { "[::]:0", SOCKET_V6_ONLY, true, "::", 3, "STANDARD.md:386-392; zero ipv6 is an ipv6 endpoint" },

    // the port is part of the identity (STANDARD.md:342-351): same address,
    // port 0, is not the non-zero-port form. to_string omits a zero port.
    { "1.2.3.4:0", SOCKET_DUAL_STACK, true, "1.2.3.4", 4, "STANDARD.md:342-351; port is part of the key" },

    // NONE is not a source and is not equal to itself or any other address
    // (sdk/rocketnet_address.cpp:207-211 returns false for a non-ipv4/ipv6 type).
    { NULL, SOCKET_DUAL_STACK, true, "NONE", -1, "rocketnet_address.cpp:207-211; NONE unequal to itself and all" }
};

static const int k_case_count = (int) ( sizeof( k_cases ) / sizeof( k_cases[0] ) );

// a failing row prints its index and the rule it pins, then fails through the
// registry check so the suite's ordinary failure path is the one that fires.

static void check_row( bool ok, int row, const char * cite )
{
    if ( !ok )
    {
        printf( "address table row %d failed: %s\n", row, cite );
        fflush( stdout );
    }
    rocketnet_unit_check( ok );
}

static void address_acceptance_table()
{
    struct rocketnet_address_t forms[k_native_form_count];
    for ( int f = 0; f < k_native_form_count; ++f )
        rocketnet_unit_check( rocketnet_address_parse( &forms[f], k_native_forms[f] ) );

    for ( int i = 0; i < k_case_count; ++i )
    {
        const struct address_case_t * row = &k_cases[i];

        struct rocketnet_address_t from;
        memset( &from, 0, sizeof( from ) );
        if ( row->source != NULL )
            check_row( rocketnet_address_parse( &from, row->source ), i, row->cite );

        const struct rocketnet_address_t before = from;

        const bool accepted = rocketnet_address_accept_received_source( &from, socket_mode_is_dual_stack( row->mode ) );

        check_row( accepted == row->expected_accept, i, row->cite );

        if ( row->expected_accept )
        {
            char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
            const char * canonical = rocketnet_address_to_string( &from, buffer );
            check_row( strcmp( canonical, row->expected_canonical ) == 0, i, row->cite );

            for ( int f = 0; f < k_native_form_count; ++f )
            {
                const bool want = ( f == row->expected_equal_form );
                const bool got = rocketnet_address_equal( &from, &forms[f] );
                check_row( got == want, i, row->cite );
            }

            if ( row->source == NULL )
            {
                // NONE equals nothing, not even itself.
                check_row( !rocketnet_address_equal( &from, &from ), i, row->cite );
                struct rocketnet_address_t other;
                memset( &other, 0, sizeof( other ) );
                check_row( !rocketnet_address_equal( &from, &other ), i, row->cite );
            }
        }
        else
        {
            // dropped, not canonicalised: the mapped ipv6 form is unchanged.
            check_row( from.type == ROCKETNET_ADDRESS_IPV6, i, row->cite );
            check_row( rocketnet_address_equal( &from, &before ), i, row->cite );
        }
    }
}

// ---------------------------------------------------------------------------
// the seeded equality property. A fixed-seed xorshift keeps the run
// deterministic and independent of the clock; only NONE, IPV4 and IPV6 are
// drawn, because reflexivity is stated "except for NONE".
// ---------------------------------------------------------------------------

static uint64_t property_rng_state;

static uint32_t property_next_u32()
{
    property_rng_state ^= property_rng_state << 13;
    property_rng_state ^= property_rng_state >> 7;
    property_rng_state ^= property_rng_state << 17;
    return (uint32_t) ( property_rng_state >> 32 );
}

static void property_random_address( struct rocketnet_address_t * address )
{
    memset( address, 0, sizeof( *address ) );

    const uint32_t choice = property_next_u32() % 8u;

    if ( choice == 0u )
    {
        address->type = ROCKETNET_ADDRESS_NONE;
        return;
    }

    const uint32_t port_roll = property_next_u32();
    address->port = (uint16_t) ( ( port_roll % 5u == 0u ) ? 0u : ( port_roll & 0xFFFFu ) );

    if ( choice <= 4u )
    {
        address->type = ROCKETNET_ADDRESS_IPV4;
        const uint32_t ip = ( choice == 1u ) ? 0u : property_next_u32();
        memcpy( address->data.ipv4, &ip, 4 );
    }
    else
    {
        address->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
            address->data.ipv6[i] = (uint16_t) property_next_u32();
    }
}

static void address_equality_property()
{
    property_rng_state = UINT64_C( 0x9E3779B97F4A7C15 );

    const int iterations = 10000;
    for ( int i = 0; i < iterations; ++i )
    {
        struct rocketnet_address_t a;
        struct rocketnet_address_t b;
        struct rocketnet_address_t c;
        property_random_address( &a );
        property_random_address( &b );
        property_random_address( &c );

        // reflexive except for NONE.
        const bool want_self = ( a.type != ROCKETNET_ADDRESS_NONE );
        rocketnet_unit_check( rocketnet_address_equal( &a, &a ) == want_self );

        // symmetric.
        rocketnet_unit_check( rocketnet_address_equal( &a, &b ) == rocketnet_address_equal( &b, &a ) );

        // transitive.
        if ( rocketnet_address_equal( &a, &b ) && rocketnet_address_equal( &b, &c ) )
            rocketnet_unit_check( rocketnet_address_equal( &a, &c ) );
    }
}

void test_rocketnet_test_address_acceptance_table()
{
    address_acceptance_table();
}

void test_rocketnet_test_address_equality_property()
{
    address_equality_property();
}

REGISTER_UNIT_TEST( test_rocketnet_test_address_acceptance_table );
REGISTER_UNIT_TEST( test_rocketnet_test_address_equality_property );
