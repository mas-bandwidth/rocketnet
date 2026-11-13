/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

// The gso server transport's worker placement (pure, every platform).
// The placement is the XDP transport's: the server's num_os_cpus (config,
// default 1) are left to the OS, and queue q's worker goes on cpu
// num_os_cpus + q. With the owner's layout (cpu 0 the OS, two queues) the two
// workers sit on cpus 1 and 2, each on a core of its own.

#ifndef ROCKETNET_SDK_ROCKETNET_GSO_PIN_H
#define ROCKETNET_SDK_ROCKETNET_GSO_PIN_H

// the pin states a worker reports (rocketnet_server_gso_worker_cpu): a cpu
// number once pinned, or one of these

#define ROCKETNET_GSO_PIN_PENDING ( -2 ) // the worker thread has not run its pin yet
#define ROCKETNET_GSO_PIN_NONE ( -1 )   // not pinned: not asked for (num_os_cpus 0) or refused (counted, logged by name)

// why a pin was refused

#define ROCKETNET_GSO_PIN_OK 0
#define ROCKETNET_GSO_PIN_OUTSIDE_SET 1 // the cpu is outside the cpu_set_t (0 .. set_size - 1)
#define ROCKETNET_GSO_PIN_OFFLINE 2     // the cpu is not online (0 .. online - 1)

// queue q's cpu: num_os_cpus + q, the XDP transport's own rule

static inline int rocketnet_gso_pin_cpu( int queue, int num_os_cpus )
{
    return num_os_cpus + queue;
}

// whether the cpu can be pinned to: in the set and online. The refusal is
// returned, never clamped: a worker on a cpu the config did not name is a
// different server

static inline int rocketnet_gso_pin_check( int cpu, int set_size, int online )
{
    if ( cpu < 0 || cpu >= set_size )
    {
        return ROCKETNET_GSO_PIN_OUTSIDE_SET;
    }
    if ( cpu >= online )
    {
        return ROCKETNET_GSO_PIN_OFFLINE;
    }
    return ROCKETNET_GSO_PIN_OK;
}

// the cpus a worker whose pin was refused is confined to: every online cpu
// from num_os_cpus up (cpu 0 is the OS's on Linux), so a refused pin never
// leaves the worker on a mask it inherited that includes the OS's cpus.
// allowed[0 .. set_size - 1] is written 1 or 0; returns how many are 1. Zero
// means no cpu is left (a one cpu machine): the caller changes nothing.

static inline int rocketnet_gso_pin_fallback( unsigned char * allowed, int set_size, int online, int num_os_cpus )
{
    int count = 0;
    for ( int cpu = 0; cpu < set_size; cpu++ )
    {
        const bool ok = cpu >= ( num_os_cpus > 1 ? num_os_cpus : 1 ) && cpu < online;
        allowed[cpu] = ok ? 1 : 0;
        count += ok ? 1 : 0;
    }
    return count;
}

#endif // ROCKETNET_SDK_ROCKETNET_GSO_PIN_H
