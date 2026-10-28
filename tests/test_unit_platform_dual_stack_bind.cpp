/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The dual-stack port pick of the Darwin IPv6 wildcard bind
// (rocketnet_platform_dual_stack_bind in sdk/rocketnet_platform.cpp), driven
// with a fake kernel: no socket, no thread and no clock. The fake hands out
// IPv4 reservations from a small port range, never a port a live reservation
// holds, and refuses an IPv6 bind to a port it marks as held in IPv6 alone,
// as Darwin does when another IPv6 socket owns it.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <string.h>

#define FAKE_FIRST_PORT     40000
#define FAKE_NUM_PORTS      64

struct fake_dual_stack_kernel_t
{
    bool reserved[FAKE_NUM_PORTS];          // held by a live IPv4 reservation
    bool ipv6_held[FAKE_NUM_PORTS];         // held in IPv6 alone by another socket
    bool refuse_reservations;
    bool bind_fails_other;
    int num_reserves;
    int num_binds;
    int num_releases;
    int live_reservations;
    uint16_t bound_port;
};

static int fake_reserve_ipv4( void * context, uint16_t port, uint16_t * reserved_port )
{
    fake_dual_stack_kernel_t * kernel = (fake_dual_stack_kernel_t *) context;
    kernel->num_reserves++;
    if ( kernel->refuse_reservations )
        return -1;
    for ( int i = 0; i < FAKE_NUM_PORTS; i++ )
    {
        const uint16_t candidate = (uint16_t) ( FAKE_FIRST_PORT + i );
        if ( kernel->reserved[i] || ( port != 0 && candidate != port ) )
            continue;
        kernel->reserved[i] = true;
        kernel->live_reservations++;
        *reserved_port = candidate;
        return i;
    }
    return -1;
}

static int fake_bind_ipv6( void * context, uint16_t port )
{
    fake_dual_stack_kernel_t * kernel = (fake_dual_stack_kernel_t *) context;
    kernel->num_binds++;
    rocketnet_unit_check( port >= FAKE_FIRST_PORT && port < FAKE_FIRST_PORT + FAKE_NUM_PORTS );
    rocketnet_unit_check( kernel->reserved[port - FAKE_FIRST_PORT] );     // the reservation is held across the bind
    if ( kernel->bind_fails_other )
        return ROCKETNET_PLATFORM_DUAL_STACK_BIND_FAILED;
    if ( kernel->ipv6_held[port - FAKE_FIRST_PORT] )
        return ROCKETNET_PLATFORM_DUAL_STACK_BIND_IN_USE;
    kernel->bound_port = port;
    return ROCKETNET_PLATFORM_DUAL_STACK_BIND_OK;
}

static void fake_release_ipv4( void * context, int reservation )
{
    fake_dual_stack_kernel_t * kernel = (fake_dual_stack_kernel_t *) context;
    rocketnet_unit_check( reservation >= 0 && reservation < FAKE_NUM_PORTS );
    rocketnet_unit_check( kernel->reserved[reservation] );
    kernel->reserved[reservation] = false;
    kernel->live_reservations--;
    kernel->num_releases++;
}

static void fake_ops( fake_dual_stack_kernel_t * kernel, rocketnet_platform_dual_stack_bind_ops_t * ops )
{
    memset( kernel, 0, sizeof( *kernel ) );
    ops->context = kernel;
    ops->reserve_ipv4 = fake_reserve_ipv4;
    ops->bind_ipv6 = fake_bind_ipv6;
    ops->release_ipv4 = fake_release_ipv4;
}

// the first free port binds at once, and its reservation is released
static void test_platform_dual_stack_bind_free_port_binds_first_time()
{
    fake_dual_stack_kernel_t kernel;
    rocketnet_platform_dual_stack_bind_ops_t ops;
    fake_ops( &kernel, &ops );

    uint16_t bound = 0;
    int attempts = 0;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, 0, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_OK );
    rocketnet_unit_check( bound == FAKE_FIRST_PORT );
    rocketnet_unit_check( attempts == 1 );
    rocketnet_unit_check( kernel.live_reservations == 0 );
}

// an IPv6 bind refused with the port in use picks a new port and succeeds:
// the refused port's reservation stays held, so the next is different
static void test_platform_dual_stack_bind_in_use_picks_a_new_port()
{
    fake_dual_stack_kernel_t kernel;
    rocketnet_platform_dual_stack_bind_ops_t ops;
    fake_ops( &kernel, &ops );
    kernel.ipv6_held[0] = true;
    kernel.ipv6_held[1] = true;

    uint16_t bound = 0;
    int attempts = 0;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, 0, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_OK );
    rocketnet_unit_check( bound == FAKE_FIRST_PORT + 2 );
    rocketnet_unit_check( kernel.bound_port == bound );
    rocketnet_unit_check( attempts == 3 );
    rocketnet_unit_check( kernel.num_binds == 3 );
    rocketnet_unit_check( kernel.num_releases == 3 );
    rocketnet_unit_check( kernel.live_reservations == 0 );
}

// every port in use in IPv6: the pick stops at its bound and returns
// BIND_IN_USE, with nothing bound and every reservation released
static void test_platform_dual_stack_bind_in_use_bound_runs_out()
{
    fake_dual_stack_kernel_t kernel;
    rocketnet_platform_dual_stack_bind_ops_t ops;
    fake_ops( &kernel, &ops );
    for ( int i = 0; i < FAKE_NUM_PORTS; i++ )
        kernel.ipv6_held[i] = true;

    uint16_t bound = 0;
    int attempts = 0;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, 0, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_IN_USE );
    rocketnet_unit_check( bound == 0 );
    rocketnet_unit_check( attempts == ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS );
    rocketnet_unit_check( kernel.num_binds == ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS );
    rocketnet_unit_check( kernel.num_releases == ROCKETNET_PLATFORM_DUAL_STACK_BIND_ATTEMPTS );
    rocketnet_unit_check( kernel.live_reservations == 0 );
}

// a requested port is tried once: in use, it returns BIND_IN_USE at once
static void test_platform_dual_stack_bind_requested_port_in_use_tries_once()
{
    fake_dual_stack_kernel_t kernel;
    rocketnet_platform_dual_stack_bind_ops_t ops;
    fake_ops( &kernel, &ops );
    kernel.ipv6_held[5] = true;

    uint16_t bound = 0;
    int attempts = 0;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, FAKE_FIRST_PORT + 5, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_IN_USE );
    rocketnet_unit_check( attempts == 1 );
    rocketnet_unit_check( kernel.live_reservations == 0 );

    kernel.ipv6_held[5] = false;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, FAKE_FIRST_PORT + 5, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_OK );
    rocketnet_unit_check( bound == FAKE_FIRST_PORT + 5 );
}

// a reservation the kernel refuses, or an IPv6 bind refused for any reason
// but the port in use, fails at once with BIND_FAILED and no retry
static void test_platform_dual_stack_bind_other_failures_do_not_retry()
{
    fake_dual_stack_kernel_t kernel;
    rocketnet_platform_dual_stack_bind_ops_t ops;
    uint16_t bound = 0;
    int attempts = -1;

    fake_ops( &kernel, &ops );
    kernel.refuse_reservations = true;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, 0, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_FAILED );
    rocketnet_unit_check( attempts == 0 );
    rocketnet_unit_check( kernel.num_reserves == 1 );

    fake_ops( &kernel, &ops );
    kernel.bind_fails_other = true;
    rocketnet_unit_check( rocketnet_platform_dual_stack_bind( &ops, 0, &bound, &attempts ) == ROCKETNET_PLATFORM_DUAL_STACK_BIND_FAILED );
    rocketnet_unit_check( attempts == 1 );
    rocketnet_unit_check( kernel.num_binds == 1 );
    rocketnet_unit_check( kernel.live_reservations == 0 );
}

REGISTER_UNIT_TEST( test_platform_dual_stack_bind_free_port_binds_first_time );
REGISTER_UNIT_TEST( test_platform_dual_stack_bind_in_use_picks_a_new_port );
REGISTER_UNIT_TEST( test_platform_dual_stack_bind_in_use_bound_runs_out );
REGISTER_UNIT_TEST( test_platform_dual_stack_bind_requested_port_in_use_tries_once );
REGISTER_UNIT_TEST( test_platform_dual_stack_bind_other_failures_do_not_retry );
