/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The meter's hooks (benchmarks/client_frame_time_hooks.c), shared with
// benchmarks/client_frame_time.cpp. See benchmarks/client_frame_time.md.

#pragma once

#ifndef ROCKETNET_BENCHMARKS_CLIENT_FRAME_TIME_H
#define ROCKETNET_BENCHMARKS_CLIENT_FRAME_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

// the client call the game thread is inside, or CFT_CALL_NONE

enum
{
    CFT_CALL_NONE = 0,
    CFT_CALL_UPDATE = 1,
    CFT_CALL_RECEIVE = 2,
    CFT_CALL_WRITE = 3,
    CFT_CALL_SEND = 4,
    CFT_NUM_CALLS = 5
};

// the plant: one per call window, inside the library, from the first
// try-acquire of the handoff mutex the call makes

enum
{
    CFT_PLANT_NONE = 0,
    CFT_PLANT_BUSY = 1,       // a busy loop of plant_microseconds
    CFT_PLANT_ALLOC = 2,      // one malloc and free through the library's allocator hooks
    CFT_PLANT_SYSCALL = 3     // one getppid
};

// Counted only on a thread inside a call window (cft_set_call), which is the
// game thread alone, so plain integers.

struct cft_counts_t
{
    uint64_t trylock;           // try-acquires of a mutex
    uint64_t trylock_busy;      // ... that found it held
    uint64_t lock;              // blocking acquires
    uint64_t lock_ns;           // time inside blocking acquires
    uint64_t wake;              // the transport's wake: eventfd write, EVFILT_USER kevent, PostQueuedCompletionStatus
    uint64_t os_other;          // other hooked operating system calls (kevent, WSASendTo, ...)
};

extern struct cft_counts_t cft_counts;

void cft_hooks_init( void );
const char * cft_hooks_describe( void );
void cft_set_call( int call );
int cft_get_call( void );
void cft_set_plant( int call, int kind, double microseconds, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) );
uint64_t cft_now_ns( void );

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // ROCKETNET_BENCHMARKS_CLIENT_FRAME_TIME_H
