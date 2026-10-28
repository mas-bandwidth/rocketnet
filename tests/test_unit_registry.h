/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The unit-file registry: the registrar behind the test_unit_<topic>.cpp
// convention in tests/. A unit test file includes this header
// and calls REGISTER_UNIT_TEST(name) at namespace scope; the registrar's
// constructor inserts the test into a fixed registry before main runs. test.cpp
// enumerates the registry into ./bin/test (the CMake glob for test_unit_*.cpp
// builds every file) and runs a test by name in its worker subprocess, after
// its own sdk and channels tests.
//
// A test that fails prints its name (run_named prints it first) and aborts,
// as the sdk suite's tests do. The one departure from the sdk suite is the
// expected-fail marker: REGISTER_UNIT_TEST_EXPECTED_FAIL(name, card) declares
// that the test asserts the correct behaviour of a still-open defect card,
// so at BASE it must fail. The registry runs it and a failure is the expected
// outcome (the suite stays green); a pass is a failure -- the fix card has
// landed and the marker must be removed.

#pragma once

#ifndef ROCKETNET_TESTS_TEST_UNIT_REGISTRY_H
#define ROCKETNET_TESTS_TEST_UNIT_REGISTRY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A status value no call writes. A test sets a status to it before a call to
// see that the call wrote the status. Every code is zero or negative.
#define ROCKETNET_TEST_STATUS_UNSET ( (enum rocketnet_status_t) 1 )

struct rocketnet_unit_file_test_t
{
    const char * name;
    const char * expected_fail_card;    // NULL: ordinary test; else: the defect card that must still fail today
    void ( *function )();
};

// The tree had 1,029 registrations when this fixed registry replaced the
// pointer chain. 2,048 is the next power of two and leaves the same amount of
// explicit growth again. Registration needs no allocation before main; a tree
// that exceeds the bound aborts with the omitted test's name instead of
// silently dropping it. Entries are inserted at the front to preserve the
// linked registry's established reverse-construction enumeration order.

#define ROCKETNET_UNIT_FILE_TEST_CAPACITY 2048

struct rocketnet_unit_file_registry_t
{
    struct rocketnet_unit_file_test_t tests[ROCKETNET_UNIT_FILE_TEST_CAPACITY];
    int count;
};

inline struct rocketnet_unit_file_registry_t * rocketnet_unit_file_registry()
{
    static struct rocketnet_unit_file_registry_t registry = {};
    return &registry;
}

enum rocketnet_unit_file_register_result_t
{
    ROCKETNET_UNIT_FILE_REGISTERED,
    ROCKETNET_UNIT_FILE_DUPLICATE,
    ROCKETNET_UNIT_FILE_FULL
};

inline enum rocketnet_unit_file_register_result_t rocketnet_unit_file_register(
    struct rocketnet_unit_file_registry_t * registry, int capacity,
    const char * name, void ( *function )(), const char * expected_fail_card )
{
    for ( int i = 0; i < registry->count; i++ )
        if ( strcmp( registry->tests[i].name, name ) == 0 )
            return ROCKETNET_UNIT_FILE_DUPLICATE;
    if ( registry->count == capacity )
        return ROCKETNET_UNIT_FILE_FULL;
    if ( registry->count > 0 )
        memmove( &registry->tests[1], &registry->tests[0],
                 (size_t) registry->count * sizeof( registry->tests[0] ) );
    registry->tests[0].name = name;
    registry->tests[0].function = function;
    registry->tests[0].expected_fail_card = expected_fail_card;
    registry->count++;

    return ROCKETNET_UNIT_FILE_REGISTERED;
}

// set by the expected-fail harness while it runs a test, so a failing
// rocketnet_unit_check inside it throws rocketnet_unit_expected_failure_t
// back to the harness instead of aborting the process. false outside an
// expected-fail test. (An exception, not setjmp/longjmp: longjmp skips the
// destructors of the C++ objects it unwinds past.)

struct rocketnet_unit_expected_failure_t
{
    int line;
};

inline bool * rocketnet_unit_expected_fail_active()
{
    static bool active = false;
    return &active;
}

// A static registrar: a file-scope object whose constructor registers the
// test, so the test runs without any manual enumeration. Names are unique
// across every test_unit_*.cpp: rocketnet_unit_file_find returns the first
// match, so a second test under a name would never run. The constructor
// aborts with the name when it is already registered, before main runs.

struct rocketnet_unit_file_registrar_t
{
    rocketnet_unit_file_registrar_t( const char * name, void ( *function )(), const char * expected_fail_card )
    {
        struct rocketnet_unit_file_registry_t * registry = rocketnet_unit_file_registry();
        const enum rocketnet_unit_file_register_result_t result = rocketnet_unit_file_register(
            registry, ROCKETNET_UNIT_FILE_TEST_CAPACITY, name, function, expected_fail_card );
        if ( result == ROCKETNET_UNIT_FILE_DUPLICATE )
        {
            fprintf( stderr, "error: unit test %s is registered twice; unit test names must be unique\n", name );
            fflush( stderr );
            abort();
        }
        if ( result == ROCKETNET_UNIT_FILE_FULL )
        {
            fprintf( stderr, "error: unit test registry capacity %d exceeded while registering %s\n",
                     ROCKETNET_UNIT_FILE_TEST_CAPACITY, name );
            fflush( stderr );
            abort();
        }
    }
};

// the check a unit-file test uses. inside an expected-fail test a failure
// returns to the harness; otherwise it prints and aborts like the sdk suite.

#define rocketnet_unit_check( condition )                                                                         \
do                                                                                                                \
{                                                                                                                 \
    if ( !(condition) )                                                                                           \
    {                                                                                                             \
        if ( *rocketnet_unit_expected_fail_active() )                                                             \
            throw rocketnet_unit_expected_failure_t{ __LINE__ };                                                  \
        printf( "check failed: ( %s ), function %s, file %s, line %d\n", #condition,                             \
                (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );                                   \
        fflush( stdout );                                                                                         \
        exit( 1 );                                                                                                \
    }                                                                                                             \
} while(0)

#define ROCKETNET_UNIT_TEST_JOIN_( a, b ) a##b
#define ROCKETNET_UNIT_TEST_JOIN( a, b ) ROCKETNET_UNIT_TEST_JOIN_( a, b )
#define ROCKETNET_UNIT_TEST_LINE_( l ) rocketnet_unit_file_registrar_##l
#define ROCKETNET_UNIT_TEST_LINE( l ) ROCKETNET_UNIT_TEST_LINE_( l )

#define REGISTER_UNIT_TEST( name )                                                                                \
    static rocketnet_unit_file_registrar_t ROCKETNET_UNIT_TEST_LINE( __LINE__ )( #name, &( name ), NULL )

#define REGISTER_UNIT_TEST_EXPECTED_FAIL( name, card )                                                            \
    static rocketnet_unit_file_registrar_t ROCKETNET_UNIT_TEST_LINE( __LINE__ )( #name, &( name ), card )

inline int rocketnet_unit_tests_count()
{
    return rocketnet_unit_file_registry()->count;
}

inline const char * rocketnet_unit_tests_name( int index )
{
    const struct rocketnet_unit_file_registry_t * registry = rocketnet_unit_file_registry();
    return index >= 0 && index < registry->count ? registry->tests[index].name : NULL;
}

inline struct rocketnet_unit_file_test_t * rocketnet_unit_file_find( const char * name )
{
    struct rocketnet_unit_file_registry_t * registry = rocketnet_unit_file_registry();
    for ( int i = 0; i < registry->count; i++ )
        if ( strcmp( registry->tests[i].name, name ) == 0 )
            return &registry->tests[i];
    return NULL;
}

// runs one registered test by name; returns 2 when it is not registered, 0 on
// success, 1 on an unexpected pass of an expected-fail test. a failing
// ordinary test aborts the process before returning, as the sdk suite does.

inline int rocketnet_unit_tests_run_named( const char * name )
{
    struct rocketnet_unit_file_test_t * test = rocketnet_unit_file_find( name );
    if ( test == NULL )
        return 2;

    printf( "%s\n", test->name );
    fflush( stdout );

    if ( test->expected_fail_card == NULL )
    {
        test->function();
        return 0;
    }

    *rocketnet_unit_expected_fail_active() = true;
    try
    {
        test->function();
    }
    catch ( const rocketnet_unit_expected_failure_t & )
    {
        *rocketnet_unit_expected_fail_active() = false;
        return 0;
    }
    *rocketnet_unit_expected_fail_active() = false;
    printf( "check failed: ( %s ), expected to fail for card %s but passed; remove the marker\n",
            test->name, test->expected_fail_card );
    fflush( stdout );

    return 1;
}

// Key and seed sizes the SDK's internal key makers (sdk/rocketnet_connect_token.h)
// write: an Ed25519 backend secret is 64 bytes and its seed 32; an X25519
// server secret is 32. The public header carries only the public-key sizes.
#define RN_TEST_BACKEND_SECRET_KEY_BYTES 64
#define RN_TEST_BACKEND_SEED_BYTES 32
#define RN_TEST_SERVER_SECRET_KEY_BYTES 32

#endif // #ifndef ROCKETNET_TESTS_TEST_UNIT_REGISTRY_H
