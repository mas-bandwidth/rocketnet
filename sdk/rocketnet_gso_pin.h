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
#define ROCKETNET_GSO_PIN_NONE ( -1 )   // not pinned: refused (counted, logged by name)

// why a pin was refused

#define ROCKETNET_GSO_PIN_OK 0
#define ROCKETNET_GSO_PIN_OUTSIDE_SET 1 // the cpu is outside the cpu_set_t (0 .. set_size - 1)
#define ROCKETNET_GSO_PIN_OFFLINE 2     // the cpu is not online (0 .. online - 1)

// the cpus the OS keeps: the server's num_os_cpus, never fewer than 1 (cpu 0
// is the OS's on Linux, whatever the config says)

static inline int rocketnet_gso_pin_os_cpus( int num_os_cpus )
{
    return num_os_cpus < 1 ? 1 : num_os_cpus;
}

// queue q's cpu: num_os_cpus + q, the XDP transport's own rule

static inline int rocketnet_gso_pin_cpu( int queue, int num_os_cpus )
{
    return rocketnet_gso_pin_os_cpus( num_os_cpus ) + queue;
}

// The cpu a Linux transport's network thread number worker takes (xdp, gso,
// io_uring and portable): with cpu 0 reserved for the OS (the default, the
// transport's *_reserve_cpu_0 field 0 or ROCKETNET_CPU_0_RESERVED) it is
// num_os_cpus + worker, a num_os_cpus below 1 read as 1; not reserved
// (ROCKETNET_CPU_0_NOT_RESERVED) it is worker itself, from cpu 0, and
// num_os_cpus is not applied

static inline int rocketnet_network_thread_cpu( int worker, int num_os_cpus, bool reserve_cpu_0 )
{
    if ( !reserve_cpu_0 )
    {
        return worker;
    }
    return ( num_os_cpus < 1 ? 1 : num_os_cpus ) + worker;
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
// leaves the worker on a mask it inherited that includes the OS's cpus. When
// num_os_cpus leaves none (it is at or past the online count) every online cpu
// but cpu 0 is allowed. allowed[0 .. set_size - 1] is written 1 or 0; returns
// how many are 1. Zero only on a one cpu machine, where the caller changes
// nothing: there is no cpu but 0.

static inline int rocketnet_gso_pin_fallback( unsigned char * allowed, int set_size, int online, int num_os_cpus )
{
    int first = num_os_cpus > 1 ? num_os_cpus : 1;
    if ( first >= online || first >= set_size )
    {
        first = 1;
    }
    int count = 0;
    for ( int cpu = 0; cpu < set_size; cpu++ )
    {
        const bool ok = cpu >= first && cpu < online;
        allowed[cpu] = ok ? 1 : 0;
        count += ok ? 1 : 0;
    }
    return count;
}

#endif // ROCKETNET_SDK_ROCKETNET_GSO_PIN_H
