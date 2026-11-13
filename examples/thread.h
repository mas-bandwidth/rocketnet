/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_EXAMPLES_THREAD_H
#define ROCKETNET_EXAMPLES_THREAD_H

// The platform's own threads, as small as the server examples need: start
// one function on a new thread, and join it. A game hands this work to its
// job system on its game threads instead; the examples use a plain platform
// thread so they depend on nothing else.

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

#endif // ROCKETNET_EXAMPLES_THREAD_H
