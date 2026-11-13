/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

// The io_uring server transport's worker placement (pure, every platform).
// The placement is the XDP transport's: the server's num_os_cpus (config,
// default 1) are left to the OS, and queue q's worker goes on cpu
// num_os_cpus + q. With the owner's layout (cpu 0 the OS, two queues) the two
// workers sit on cpus 1 and 2, each on a core of its own.

#ifndef ROCKETNET_SDK_ROCKETNET_URING_PIN_H
#define ROCKETNET_SDK_ROCKETNET_URING_PIN_H

#include <stdint.h>

// the pin states a worker reports (rocketnet_server_uring_worker_cpu): a cpu
// number once pinned, or one of these

#define ROCKETNET_URING_PIN_PENDING ( -2 ) // the worker thread has not run its pin yet
#define ROCKETNET_URING_PIN_NONE ( -1 )  // not pinned: refused (counted, logged by name)

// why a pin was refused

#define ROCKETNET_URING_PIN_OK 0
#define ROCKETNET_URING_PIN_OUTSIDE_SET 1 // the cpu is outside the cpu_set_t (0 .. set_size - 1)
#define ROCKETNET_URING_PIN_OFFLINE 2     // the cpu is not online (0 .. online - 1)

// the cpus the OS keeps: the server's num_os_cpus, never fewer than 1 (cpu 0
// is the OS's, whatever the config says)

static inline int rocketnet_uring_pin_os_cpus( int num_os_cpus )
{
    return num_os_cpus < 1 ? 1 : num_os_cpus;
}

// queue q's cpu: num_os_cpus + q, the XDP transport's own rule

static inline int rocketnet_uring_pin_cpu( int queue, int num_os_cpus )
{
    return rocketnet_uring_pin_os_cpus( num_os_cpus ) + queue;
}

// whether the cpu can be pinned to: in the set and online. The refusal is
// returned, never clamped: a worker on a cpu the config did not name is a
// different server

static inline int rocketnet_uring_pin_check( int cpu, int set_size, int online )
{
    if ( cpu < 0 || cpu >= set_size )
    {
        return ROCKETNET_URING_PIN_OUTSIDE_SET;
    }
    if ( cpu >= online )
    {
        return ROCKETNET_URING_PIN_OFFLINE;
    }
    return ROCKETNET_URING_PIN_OK;
}

// where a worker whose pin was refused may run, as a cpu bitmask (bit c of
// word c / 64 is cpu c): taken from the mask the worker has now, which holds
// only cpus the system lets it use (online, inside its cpuset; gaps in the
// numbering are its own), so the confine never asks for a cpu the system
// would refuse. The cpus of that mask from the OS's up (num_os_cpus, never
// below 1); when none of them is, every cpu of it but cpu 0. Never cpu 0, the
// OS's core: the refused worker would otherwise keep the mask it inherited,
// which holds cpu 0. false, and confined all zero, when nothing but cpu 0 is
// left (a mask of cpu 0 alone, or an empty one): the mask stays as it was.

static inline bool rocketnet_uring_pin_confine_words( const uint64_t * current, int words, int num_os_cpus, uint64_t * confined )
{
    const int os_cpus = rocketnet_uring_pin_os_cpus( num_os_cpus );
    bool past_os = false;
    for ( int w = 0; w < words; w++ )
    {
        uint64_t keep = current[w];
        const int first = w * 64;
        if ( os_cpus >= first + 64 )
        {
            keep = 0;
        }
        else if ( os_cpus > first )
        {
            keep &= ~( ( (uint64_t) 1 << ( os_cpus - first ) ) - 1 );
        }
        confined[w] = keep;
        past_os = past_os || keep != 0;
    }
    if ( past_os )
    {
        return true;
    }

    bool any = false;
    for ( int w = 0; w < words; w++ )
    {
        confined[w] = w == 0 ? ( current[w] & ~(uint64_t) 1 ) : current[w];
        any = any || confined[w] != 0;
    }
    return any;
}

#endif // ROCKETNET_SDK_ROCKETNET_URING_PIN_H
