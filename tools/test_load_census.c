/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

/* The load harness resource census (load_census.h) finds the thread and the
   handle a cycle leaves behind, and nothing when it leaves nothing. Build and
   run beside the harness, from the repository root:

       cc -std=c11 -I. tools/test_load_census.c -o test_load_census -lpthread && ./test_load_census
       cl /W4 /WX /I. tools\test_load_census.c && test_load_census.exe
*/

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#endif

#include "load_census.h"
#include "test/portable_crt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
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
#endif

static struct load_census_t test_before, test_after;

static struct load_census_survivors_t test_survivors( void )
{
    struct load_census_survivors_t survivors;
    assert( load_census_take( &test_after ) );
    assert( load_census_survivors( NULL, NULL, 0, &test_before, &test_after, &survivors ) );
    return survivors;
}

int main( void )
{
#if defined(_WIN32)
    test_release = CreateEventA( NULL, TRUE, FALSE, NULL );
    assert( test_release );
#endif

    /* nothing taken: nothing survives */
    assert( load_census_take( &test_before ) );
    struct load_census_survivors_t survivors = test_survivors();
    assert( survivors.threads == 0 && survivors.handles == 0 );

    /* a file left open survives; closed, it does not */
    FILE * file = rocketnet_test_fopen( "test_load_census.tmp", "w" );
    assert( file );
    survivors = test_survivors();
    assert( survivors.handles == 1 && survivors.threads == 0 );
    fclose( file );
    survivors = test_survivors();
    assert( survivors.handles == 0 );

#if !defined(_WIN32)
    /* a closed descriptor's number reused by another file is a different
       object: identity, not number, decides (posix census only) */
    FILE * first = rocketnet_test_fopen( "test_load_census.tmp", "w" );
    assert( first );
    assert( load_census_take( &test_before ) );
    const int number = fileno( first );
    fclose( first );
    FILE * second = rocketnet_test_fopen( "test_load_census_other.tmp", "w" );
    assert( second && fileno( second ) == number );
    survivors = test_survivors();
    assert( survivors.handles == 1 );
    fclose( second );
    remove( "test_load_census_other.tmp" );
    assert( load_census_take( &test_before ) );
#endif

#if !defined(_WIN32)
    /* the random device is init-scope state: a pair that ends before term
       reports it without counting it; the pair through term counts it */
    FILE * device = rocketnet_test_fopen( "/dev/urandom", "r" );
    assert( device );
    assert( load_census_take( &test_after ) );
    assert( load_census_survivors( NULL, NULL, 1, &test_before, &test_after, &survivors ) );
    assert( survivors.handles == 0 && survivors.init_scope_handles == 1 );
    assert( load_census_survivors( NULL, NULL, 0, &test_before, &test_after, &survivors ) );
    assert( survivors.handles == 1 );
    fclose( device );
    assert( load_census_take( &test_before ) );
#endif

    /* a thread left running survives, a joined one does not: this process
       created it, so it is never an operating-system pool worker */
#if defined(_WIN32)
    HANDLE thread = CreateThread( NULL, 0, test_thread_main, NULL, 0, NULL );
    assert( thread );
    survivors = test_survivors();
    assert( survivors.threads == 1 && survivors.os_pool_threads == 0 );
    assert( survivors.handles >= 1 );
    SetEvent( test_release );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
#else
    pthread_mutex_lock( &test_mutex );
    pthread_t thread;
    assert( pthread_create( &thread, NULL, test_thread_main, NULL ) == 0 );
    survivors = test_survivors();
    assert( survivors.threads == 1 && survivors.os_pool_threads == 0 );
    pthread_mutex_unlock( &test_mutex );
    assert( pthread_join( thread, NULL ) == 0 );
#endif
    survivors = test_survivors();
    assert( survivors.threads == 0 && survivors.handles == 0 );

    remove( "test_load_census.tmp" );
    printf( "test_load_census\n" );
    return 0;
}
