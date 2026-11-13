/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

/* The load harness resource census (tests/load/load_census.h) finds the thread and the
   handle a cycle leaves behind, and nothing when it leaves nothing. CMake builds it
   as bin/test_load_census beside the harness (ROCKETNET_BUILD_TOOLS); the ci.yml
   functional leg runs it from the repository root on Linux and macOS, and the
   Windows job runs it there too. It opens files and starts a thread, so it is
   a functional test, not a unit test.
*/

#if !defined( _WIN32 )
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#endif // #if !defined( _WIN32 )

#include "tests/load/load_census.h"
#include "tests/portable_crt.h"


#include <stdio.h>
#include <stdlib.h>

// a check that stays in every build (a Release build compiles assert out, and
// several checks below take the census inside the check)
#define CENSUS_CHECK( condition ) \
    do \
    { \
        if ( !( condition ) ) \
        { \
            fprintf( stderr, "test_load_census: check failed: %s (line %d)\n", #condition, __LINE__ ); \
            exit( 1 ); \
        } \
    } while ( 0 )

#if defined( _WIN32 )
static HANDLE test_release;
static DWORD WINAPI test_thread_main( LPVOID arg )
{
    (void) arg;
    WaitForSingleObject( test_release, INFINITE );
    return 0;
}
#else
#include <pthread.h>
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static void * test_thread_main( void * arg )
{
    (void) arg;
    pthread_mutex_lock( &test_mutex );
    pthread_mutex_unlock( &test_mutex );
    return NULL;
}
#endif // #if defined( _WIN32 )

static struct load_census_t test_before, test_after;

// the census after test_before, taken as the harness takes a later phase
// (load_census_take_after, at most `waits` 1 ms waits for joined threads to
// leave on macOS; it returns at once when no new thread is listed); every
// survivor is printed, so a red names it
static struct load_census_survivors_t test_survivors_within( int waits )
{
    struct load_census_survivors_t survivors;

    CENSUS_CHECK( load_census_take_after( &test_before, &test_after, waits ) );
    CENSUS_CHECK( load_census_survivors( "test_load_census", "test", 0, &test_before, &test_after, &survivors ) );
    return survivors;
}

static struct load_census_survivors_t test_survivors( void )
{
    return test_survivors_within( LOAD_CENSUS_SETTLE_WAITS );
}

int main( void )
{
#if defined( _WIN32 )
    test_release = CreateEventA( NULL, TRUE, FALSE, NULL );
    CENSUS_CHECK( test_release );
#endif // #if defined( _WIN32 )

    // nothing taken: nothing survives
    CENSUS_CHECK( load_census_take( &test_before ) );

    struct load_census_survivors_t survivors = test_survivors();
    CENSUS_CHECK( survivors.threads == 0 && survivors.handles == 0 );

    // a file left open survives; closed, it does not
    FILE * file = rocketnet_test_fopen( "test_load_census.tmp", "w" );
    CENSUS_CHECK( file );
    survivors = test_survivors();
    CENSUS_CHECK( survivors.handles == 1 && survivors.threads == 0 );
    fclose( file );
    survivors = test_survivors();
    CENSUS_CHECK( survivors.handles == 0 );

#if !defined( _WIN32 )
    /* a closed descriptor's number reused by another file is a different
       object: identity, not number, decides (posix census only) */
    FILE * first = rocketnet_test_fopen( "test_load_census.tmp", "w" );
    CENSUS_CHECK( first );
    CENSUS_CHECK( load_census_take( &test_before ) );

    const int number = fileno( first );

    fclose( first );

    FILE * second = rocketnet_test_fopen( "test_load_census_other.tmp", "w" );
    CENSUS_CHECK( second && fileno( second ) == number );
    survivors = test_survivors();
    CENSUS_CHECK( survivors.handles == 1 );
    fclose( second );
    remove( "test_load_census_other.tmp" );
    CENSUS_CHECK( load_census_take( &test_before ) );
#endif // #if !defined( _WIN32 )

#if defined( _WIN32 )
    /* a closed handle's number reused by a new object of the same type (an
       SDK handle taking a baseline handle's slot) is a different object:
       the census's held duplicate, not number and type, decides */
    /* the handle table promises no order of reuse, so the pair is retried
       until the second Event takes the first one's number */
    HANDLE second_event = NULL;
    for ( int attempt = 0; attempt < 64 && !second_event; ++attempt )
    {
        HANDLE first_event = CreateEventA( NULL, TRUE, FALSE, NULL );
        CENSUS_CHECK( first_event );
        CENSUS_CHECK( load_census_take( &test_before ) );
        CloseHandle( first_event );

        second_event = CreateEventA( NULL, TRUE, FALSE, NULL );
        CENSUS_CHECK( second_event );
        if ( second_event != first_event )
        {
            CloseHandle( second_event );
            second_event = NULL;
        }
    }
    CENSUS_CHECK( second_event );
    survivors = test_survivors();
    CENSUS_CHECK( survivors.handles == 1 );
    CloseHandle( second_event );
    CENSUS_CHECK( load_census_take( &test_before ) );

    /* and a handle the census took is the same object at the next census:
       nothing survives */
    HANDLE kept_event = CreateEventA( NULL, TRUE, FALSE, NULL );
    CENSUS_CHECK( kept_event );
    CENSUS_CHECK( load_census_take( &test_before ) );
    survivors = test_survivors();
    CENSUS_CHECK( survivors.handles == 0 );
    CloseHandle( kept_event );
    CENSUS_CHECK( load_census_take( &test_before ) );
#endif // #if defined( _WIN32 )

#if !defined( _WIN32 )
    /* the random device is init-scope state: a pair that ends before term
       reports it without counting it; the pair through term counts it */
    FILE * device = rocketnet_test_fopen( "/dev/urandom", "r" );
    CENSUS_CHECK( device );
    CENSUS_CHECK( load_census_take( &test_after ) );
    CENSUS_CHECK( load_census_survivors( NULL, NULL, 1, &test_before, &test_after, &survivors ) );
    CENSUS_CHECK( survivors.handles == 0 && survivors.init_scope_handles == 1 );
    CENSUS_CHECK( load_census_survivors( NULL, NULL, 0, &test_before, &test_after, &survivors ) );
    CENSUS_CHECK( survivors.handles == 1 );
    fclose( device );
    CENSUS_CHECK( load_census_take( &test_before ) );
#endif // #if !defined( _WIN32 )

    /* a thread left running survives, a joined one does not: this process
       created it, so it is never an operating-system pool worker. The running
       one is still counted after a settle (load_census_take_after; 20 waits
       here, not the harness's whole bound, which a live thread would spend in
       full): a leak stays red. The joined one is gone however soon the census
       reads after the join (on macOS the join returns before the kernel
       thread ends) */
#if defined( _WIN32 )
    HANDLE thread = CreateThread( NULL, 0, test_thread_main, NULL, 0, NULL );
    CENSUS_CHECK( thread );
    survivors = test_survivors_within( 20 );
    CENSUS_CHECK( survivors.threads == 1 && survivors.os_pool_threads == 0 );
    CENSUS_CHECK( survivors.handles >= 1 );
    SetEvent( test_release );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
#else
    pthread_mutex_lock( &test_mutex );

    pthread_t thread;
    CENSUS_CHECK( pthread_create( &thread, NULL, test_thread_main, NULL ) == 0 );
    survivors = test_survivors_within( 20 );
    CENSUS_CHECK( survivors.threads == 1 && survivors.os_pool_threads == 0 );
    pthread_mutex_unlock( &test_mutex );
    CENSUS_CHECK( pthread_join( thread, NULL ) == 0 );
#endif // #if defined( _WIN32 )
    survivors = test_survivors();
    CENSUS_CHECK( survivors.threads == 0 );
    CENSUS_CHECK( survivors.handles == 0 );

    remove( "test_load_census.tmp" );
    printf( "test_load_census\n" );

    return 0;
}
