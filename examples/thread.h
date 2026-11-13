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
#include <string.h>

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
static inline DWORD WINAPI rn_cli_thread_entry( LPVOID object )
{
    struct rn_cli_thread_t * thread = (struct rn_cli_thread_t *) object;
    thread->function( thread->data );
    return 0;
}
#else  // #ifdef _WIN32
static inline void * rn_cli_thread_entry( void * object )
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

// The stand-in job system: a small pool of persistent game threads, started
// once, that runs a function over [0, count) in contiguous ranges, one range
// per thread, the calling thread taking the first, and returns when every
// range is done. The wait is the barrier the server's threading rules ask
// for between the game threads' calls and the next call the main thread
// (or the thread that took its place) makes. One thread at a time calls
// rn_cli_pool_run.

struct rn_cli_pool_t;

struct rn_cli_worker_t
{
    struct rn_cli_pool_t * pool;
    int index;
    struct rn_cli_thread_t thread;
};

struct rn_cli_pool_t
{
    int workers;        // threads started besides the caller
    int count;
    rn_cli_range_fn function;
    void * data;
    int generation, pending, stop;
    struct rn_cli_worker_t worker[RN_CLI_MAX_GAME_THREADS];
#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE work, done;
#else  // #ifdef _WIN32
    pthread_mutex_t lock;
    pthread_cond_t work, done;
#endif // #ifdef _WIN32
};

static inline void rn_cli_pool_lock( struct rn_cli_pool_t * pool )
{
#ifdef _WIN32
    EnterCriticalSection( &pool->lock );
#else  // #ifdef _WIN32
    pthread_mutex_lock( &pool->lock );
#endif // #ifdef _WIN32
}

static inline void rn_cli_pool_unlock( struct rn_cli_pool_t * pool )
{
#ifdef _WIN32
    LeaveCriticalSection( &pool->lock );
#else  // #ifdef _WIN32
    pthread_mutex_unlock( &pool->lock );
#endif // #ifdef _WIN32
}

static inline void rn_cli_pool_wait( struct rn_cli_pool_t * pool, int for_work )
{
#ifdef _WIN32
    SleepConditionVariableCS( for_work ? &pool->work : &pool->done, &pool->lock, INFINITE );
#else  // #ifdef _WIN32
    pthread_cond_wait( for_work ? &pool->work : &pool->done, &pool->lock );
#endif // #ifdef _WIN32
}

static inline void rn_cli_pool_wake( struct rn_cli_pool_t * pool, int for_work )
{
#ifdef _WIN32
    WakeAllConditionVariable( for_work ? &pool->work : &pool->done );
#else  // #ifdef _WIN32
    pthread_cond_broadcast( for_work ? &pool->work : &pool->done );
#endif // #ifdef _WIN32
}

static inline void rn_cli_pool_worker( void * object )
{
    struct rn_cli_worker_t * worker = (struct rn_cli_worker_t *) object;
    struct rn_cli_pool_t * pool = worker->pool;
    int seen = 0;

    rn_cli_pool_lock( pool );
    for ( ;; )
    {
        while ( pool->generation == seen && !pool->stop )
        {
            rn_cli_pool_wait( pool, 1 );
        }

        if ( pool->stop )
        {
            break;
        }

        seen = pool->generation;
        const int threads = pool->workers + 1;
        const int begin = pool->count * worker->index / threads;
        const int end = pool->count * ( worker->index + 1 ) / threads;
        rn_cli_range_fn function = pool->function;
        void * data = pool->data;
        rn_cli_pool_unlock( pool );

        function( data, begin, end );

        rn_cli_pool_lock( pool );
        if ( --pool->pending == 0 )
        {
            rn_cli_pool_wake( pool, 0 );
        }
    }

    rn_cli_pool_unlock( pool );
}

// Starts game_threads - 1 workers; the caller is the first game thread. A
// worker that cannot start is not counted: its range goes to the others.
static inline void rn_cli_pool_start( struct rn_cli_pool_t * pool, int game_threads )
{
    memset( pool, 0, sizeof( *pool ) );
#ifdef _WIN32
    InitializeCriticalSection( &pool->lock );
    InitializeConditionVariable( &pool->work );
    InitializeConditionVariable( &pool->done );
#else  // #ifdef _WIN32
    pthread_mutex_init( &pool->lock, NULL );
    pthread_cond_init( &pool->work, NULL );
    pthread_cond_init( &pool->done, NULL );
#endif // #ifdef _WIN32

    if ( game_threads > RN_CLI_MAX_GAME_THREADS )
    {
        game_threads = RN_CLI_MAX_GAME_THREADS;
    }

    // the worker count is fixed before any worker runs, so each reads the same range sizes
    int started = 0;
    for ( int i = 1; i < game_threads; i++ )
    {
        pool->worker[started].pool = pool;
        pool->worker[started].index = started + 1;
        if ( !rn_cli_thread_start( &pool->worker[started].thread, rn_cli_pool_worker, &pool->worker[started] ) )
        {
            break;
        }

        started++;
    }

    rn_cli_pool_lock( pool );
    pool->workers = started;
    rn_cli_pool_unlock( pool );
}

// Runs function over [0, count) on the pool's threads and returns when all are done.
static inline void rn_cli_pool_run( struct rn_cli_pool_t * pool, int count, rn_cli_range_fn function, void * data )
{
    if ( !pool || pool->workers == 0 )
    {
        function( data, 0, count );
        return;
    }

    rn_cli_pool_lock( pool );
    pool->count = count;
    pool->function = function;
    pool->data = data;
    pool->pending = pool->workers;
    pool->generation++;
    rn_cli_pool_wake( pool, 1 );
    const int threads = pool->workers + 1;
    rn_cli_pool_unlock( pool );

    function( data, 0, count / threads );

    rn_cli_pool_lock( pool );
    while ( pool->pending > 0 )
    {
        rn_cli_pool_wait( pool, 0 );
    }

    rn_cli_pool_unlock( pool );
}

static inline void rn_cli_pool_stop( struct rn_cli_pool_t * pool )
{
    rn_cli_pool_lock( pool );
    pool->stop = 1;
    rn_cli_pool_wake( pool, 1 );
    rn_cli_pool_unlock( pool );

    for ( int i = 0; i < pool->workers; i++ )
    {
        rn_cli_thread_join( &pool->worker[i].thread );
    }

#ifdef _WIN32
    DeleteCriticalSection( &pool->lock );
#else  // #ifdef _WIN32
    pthread_cond_destroy( &pool->done );
    pthread_cond_destroy( &pool->work );
    pthread_mutex_destroy( &pool->lock );
#endif // #ifdef _WIN32
}

#endif // ROCKETNET_EXAMPLES_THREAD_H
