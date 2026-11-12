/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Linux server transports' shared offload probes
// (sdk/rocketnet_server_udp_linux.h, rocketnet_server_udp_probe_offloads),
// the gso and io_uring transports' create step: each probe all or nothing
// over the group, a partial UDP_GRO enable rolled back on every socket, a
// probe not asked for not made. A fake setsockopt answers each call: no
// socket, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "../sdk/rocketnet_server_udp_linux.h"

#include <netinet/in.h>
#include <netinet/udp.h>
#include <string.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

#define FAKE_UDP_SOCKETS 4
#define FAKE_UDP_MAX_CALLS 64

// the fake group: which descriptors refuse which option, every call in order,
// and the UDP_GRO value each descriptor holds at the end
struct fake_udp_call_t
{
    int fd;
    int level;
    int option_name;
    int value;
};

struct fake_udp_group_t
{
    bool refuse_segment[FAKE_UDP_SOCKETS];
    bool refuse_gro[FAKE_UDP_SOCKETS];
    int gro_value[FAKE_UDP_SOCKETS];
    int num_calls;
    fake_udp_call_t calls[FAKE_UDP_MAX_CALLS];
};

static fake_udp_group_t fake_udp;

// descriptors 10, 11, ... are sockets 0, 1, ...
static int fake_udp_setsockopt( int fd, int level, int option_name, const void * value, socklen_t length )
{
    rocketnet_unit_check( fd >= 10 && fd < 10 + FAKE_UDP_SOCKETS );
    rocketnet_unit_check( level == IPPROTO_UDP );
    rocketnet_unit_check( length == sizeof( int ) );
    rocketnet_unit_check( fake_udp.num_calls < FAKE_UDP_MAX_CALLS );

    const int index = fd - 10;
    int written = 0;
    memcpy( &written, value, sizeof( written ) );

    fake_udp_call_t * call = &fake_udp.calls[fake_udp.num_calls++];
    call->fd = fd;
    call->level = level;
    call->option_name = option_name;
    call->value = written;

    if ( option_name == UDP_SEGMENT )
    {
        // the probe writes segment size zero, the disabled value
        rocketnet_unit_check( written == 0 );
        return fake_udp.refuse_segment[index] ? -1 : 0;
    }

    rocketnet_unit_check( option_name == UDP_GRO );
    if ( fake_udp.refuse_gro[index] )
    {
        return -1;
    }
    fake_udp.gro_value[index] = written;
    return 0;
}

static const int fake_udp_fds[FAKE_UDP_SOCKETS] = { 10, 11, 12, 13 };

static int fake_udp_count( int option_name )
{
    int count = 0;
    for ( int i = 0; i < fake_udp.num_calls; i++ )
    {
        count += fake_udp.calls[i].option_name == option_name ? 1 : 0;
    }
    return count;
}

static void test_server_udp_probe_offloads_every_socket_accepts()
{
    memset( &fake_udp, 0, sizeof( fake_udp ) );
    bool segment = false;
    bool gro = false;
    rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, true, true, fake_udp_setsockopt, &segment, &gro );

    rocketnet_unit_check( segment );
    rocketnet_unit_check( gro );
    rocketnet_unit_check( fake_udp_count( UDP_SEGMENT ) == FAKE_UDP_SOCKETS );
    rocketnet_unit_check( fake_udp_count( UDP_GRO ) == FAKE_UDP_SOCKETS );
    for ( int i = 0; i < FAKE_UDP_SOCKETS; i++ )
    {
        rocketnet_unit_check( fake_udp.gro_value[i] == 1 );
    }
}

REGISTER_UNIT_TEST( test_server_udp_probe_offloads_every_socket_accepts );

// one socket of the group refusing UDP_SEGMENT turns the tier off for the
// group; every socket is still asked, and UDP_GRO is probed as before

static void test_server_udp_probe_offloads_segment_all_or_nothing()
{
    for ( int refused = 0; refused < FAKE_UDP_SOCKETS; refused++ )
    {
        memset( &fake_udp, 0, sizeof( fake_udp ) );
        fake_udp.refuse_segment[refused] = true;
        bool segment = true;
        bool gro = false;
        rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, true, true, fake_udp_setsockopt, &segment, &gro );

        rocketnet_unit_check( !segment );
        rocketnet_unit_check( gro );
        rocketnet_unit_check( fake_udp_count( UDP_SEGMENT ) == FAKE_UDP_SOCKETS );
    }
}

REGISTER_UNIT_TEST( test_server_udp_probe_offloads_segment_all_or_nothing );

// one socket of the group refusing UDP_GRO: the tier is off and every
// socket that accepted is rolled back to zero, so none holds state the
// reported tier denies; UDP_SEGMENT is unaffected

static void test_server_udp_probe_offloads_gro_partial_rolls_back()
{
    for ( int refused = 0; refused < FAKE_UDP_SOCKETS; refused++ )
    {
        memset( &fake_udp, 0, sizeof( fake_udp ) );
        fake_udp.refuse_gro[refused] = true;
        bool segment = false;
        bool gro = true;
        rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, true, true, fake_udp_setsockopt, &segment, &gro );

        rocketnet_unit_check( segment );
        rocketnet_unit_check( !gro );
        for ( int i = 0; i < FAKE_UDP_SOCKETS; i++ )
        {
            rocketnet_unit_check( fake_udp.gro_value[i] == 0 );
        }

        // the enable over every socket, then the roll back over every socket
        rocketnet_unit_check( fake_udp_count( UDP_GRO ) == 2 * FAKE_UDP_SOCKETS );
        const fake_udp_call_t * last = &fake_udp.calls[fake_udp.num_calls - 1];
        rocketnet_unit_check( last->option_name == UDP_GRO && last->value == 0 );
    }
}

REGISTER_UNIT_TEST( test_server_udp_probe_offloads_gro_partial_rolls_back );

// a probe not asked for (the DISABLE_SEGMENT and DISABLE_GRO create flags)
// makes no call and reports false

static void test_server_udp_probe_offloads_disabled_probes_make_no_call()
{
    memset( &fake_udp, 0, sizeof( fake_udp ) );
    bool segment = true;
    bool gro = true;
    rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, false, false, fake_udp_setsockopt, &segment, &gro );
    rocketnet_unit_check( !segment );
    rocketnet_unit_check( !gro );
    rocketnet_unit_check( fake_udp.num_calls == 0 );

    memset( &fake_udp, 0, sizeof( fake_udp ) );
    rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, false, true, fake_udp_setsockopt, &segment, &gro );
    rocketnet_unit_check( !segment );
    rocketnet_unit_check( gro );
    rocketnet_unit_check( fake_udp_count( UDP_SEGMENT ) == 0 );

    memset( &fake_udp, 0, sizeof( fake_udp ) );
    rocketnet_server_udp_probe_offloads( fake_udp_fds, FAKE_UDP_SOCKETS, true, false, fake_udp_setsockopt, &segment, &gro );
    rocketnet_unit_check( segment );
    rocketnet_unit_check( !gro );
    rocketnet_unit_check( fake_udp_count( UDP_GRO ) == 0 );
}

REGISTER_UNIT_TEST( test_server_udp_probe_offloads_disabled_probes_make_no_call );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
