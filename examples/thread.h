/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_EXAMPLES_THREAD_H
#define ROCKETNET_EXAMPLES_THREAD_H

// The platform's own threads, as small as the server examples need: start
// one function on a new thread and join it, and run one function over the
// ranges of a count on some threads (a stand-in for a game's job system).
// A game hands this work to its own job system on its game threads
// instead; the examples use plain platform threads so they depend on
// nothing else. rocketnet owns no threads or job system for this.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // #ifndef WIN32_LEAN_AND_MEAN
#include <windows.h>
#else // #ifdef _WIN32
#include <pthread.h>
#endif // #ifdef _WIN32

typedef void ( *rn_cli_thread_fn )( void * data );

struct rn_cli_thread_t
{
    rn_cli_thread_fn function;
    void * data;
    int running;
#ifdef _WIN32
    HANDLE handle;
#else  // #ifdef _WIN32
    pthread_t handle;
#endif // #ifdef _WIN32
};

#ifdef _WIN32
static DWORD WINAPI rn_cli_thread_entry( LPVOID object )
{
    struct rn_cli_thread_t * thread = (struct rn_cli_thread_t *) object;
    thread->function( thread->data );
    return 0;
}
#else  // #ifdef _WIN32
static void * rn_cli_thread_entry( void * object )
{
    struct rn_cli_thread_t * thread = (struct rn_cli_thread_t *) object;
    thread->function( thread->data );
    return NULL;
}
#endif // #ifdef _WIN32

// returns 1 when the thread started; on 0 nothing runs and there is nothing to join
static inline int rn_cli_thread_start( struct rn_cli_thread_t * thread, rn_cli_thread_fn function, void * data )
{
    thread->function = function;
    thread->data = data;
#ifdef _WIN32
    thread->handle = CreateThread( NULL, 0, rn_cli_thread_entry, thread, 0, NULL );
    thread->running = thread->handle != NULL;
#else  // #ifdef _WIN32
    thread->running = pthread_create( &thread->handle, NULL, rn_cli_thread_entry, thread ) == 0;
#endif // #ifdef _WIN32
    return thread->running;
}

// waits for the thread to finish; joining a thread that is not running does nothing
static inline void rn_cli_thread_join( struct rn_cli_thread_t * thread )
{
    if ( !thread->running )
    {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject( thread->handle, INFINITE );
    CloseHandle( thread->handle );
#else  // #ifdef _WIN32
    pthread_join( thread->handle, NULL );
#endif // #ifdef _WIN32
    thread->running = 0;
}

#define RN_CLI_MAX_GAME_THREADS 64

typedef void ( *rn_cli_range_fn )( void * data, int begin, int end );

struct rn_cli_range_t
{
    rn_cli_range_fn function;
    void * data;
    int begin, end;
};

static void rn_cli_range_entry( void * object )
{
    struct rn_cli_range_t * range = (struct rn_cli_range_t *) object;
    range->function( range->data, range->begin, range->end );
}

// The stand-in job system: runs function over [0, count) in at most
// game_threads contiguous ranges, one range per thread, the calling thread
// taking the first, and returns when every range is done. The join is the
// barrier the server's threading rules ask for between the game threads'
// calls and the next call the main thread makes. A range whose thread
// could not start runs on the calling thread, so no range is skipped.
static inline void rn_cli_run_ranges( int game_threads, int count, rn_cli_range_fn function, void * data )
{
    struct rn_cli_range_t ranges[RN_CLI_MAX_GAME_THREADS];
    struct rn_cli_thread_t threads[RN_CLI_MAX_GAME_THREADS];

    if ( count < 1 )
    {
        return;
    }

    if ( game_threads > RN_CLI_MAX_GAME_THREADS )
    {
        game_threads = RN_CLI_MAX_GAME_THREADS;
    }

    if ( game_threads > count )
    {
        game_threads = count;
    }

    if ( game_threads < 1 )
    {
        game_threads = 1;
    }

    for ( int i = 0; i < game_threads; i++ )
    {
        ranges[i].function = function;
        ranges[i].data = data;
        ranges[i].begin = count * i / game_threads;
        ranges[i].end = count * ( i + 1 ) / game_threads;
        threads[i].running = 0;
    }

    for ( int i = 1; i < game_threads; i++ )
    {
        if ( !rn_cli_thread_start( &threads[i], rn_cli_range_entry, &ranges[i] ) )
        {
            rn_cli_range_entry( &ranges[i] );
        }
    }

    rn_cli_range_entry( &ranges[0] );

    for ( int i = 1; i < game_threads; i++ )
    {
        rn_cli_thread_join( &threads[i] );
    }
}

#endif // ROCKETNET_EXAMPLES_THREAD_H
