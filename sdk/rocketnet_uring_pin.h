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

// the pin states a worker reports (rocketnet_server_uring_worker_cpu): a cpu
// number once pinned, or one of these

#define ROCKETNET_URING_PIN_PENDING ( -2 ) // the worker thread has not run its pin yet
#define ROCKETNET_URING_PIN_NONE ( -1 )  // not pinned: not asked for (num_os_cpus 0) or refused (counted, logged by name)

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

// where a worker whose pin was refused may run: the first cpu it may use, or
// -1 for none to confine to. The cpus the OS leaves to the server,
// num_os_cpus .. online - 1, and when there is none past the OS's but the
// machine has more than one cpu (online <= num_os_cpus), 1 .. online - 1:
// never cpu 0, the OS's core. The refused worker would otherwise keep the
// mask it inherited, which includes cpu 0. Only a one cpu machine has nothing
// to confine to: the mask stays as it was.

static inline int rocketnet_uring_pin_fallback_first( int num_os_cpus, int online )
{
    const int os_cpus = rocketnet_uring_pin_os_cpus( num_os_cpus );
    if ( online > os_cpus )
    {
        return os_cpus;
    }
    return online > 1 ? 1 : -1;
}

static inline bool rocketnet_uring_pin_fallback_allows( int cpu, int num_os_cpus, int online )
{
    const int first = rocketnet_uring_pin_fallback_first( num_os_cpus, online );
    return first >= 0 && cpu >= first && cpu < online;
}

static inline bool rocketnet_uring_pin_fallback_possible( int num_os_cpus, int online )
{
    return rocketnet_uring_pin_fallback_first( num_os_cpus, online ) >= 0;
}

#endif // ROCKETNET_SDK_ROCKETNET_URING_PIN_H
