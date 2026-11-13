/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The Linux server transports' shared create steps
// (sdk/rocketnet_server_udp_linux.h), the gso and io_uring transports' open
// and offload probes. The probes: each all or nothing over the group, a
// partial UDP_GRO enable rolled back on every socket, a probe not asked for
// not made. The open: every option at its level and value in order, every
// refusal closing the socket and naming its reason, the grant report at its
// edges. Fake system calls answer each call: no socket, no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "../sdk/rocketnet_server_udp_linux.h"
#include "../sdk/rocketnet_constants.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

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

// a group of one socket (a server with one queue) is probed like any other:
// its one answer is the group's

static void test_server_udp_probe_offloads_group_of_one()
{
    memset( &fake_udp, 0, sizeof( fake_udp ) );
    bool segment = false;
    bool gro = false;
    rocketnet_server_udp_probe_offloads( fake_udp_fds, 1, true, true, fake_udp_setsockopt, &segment, &gro );
    rocketnet_unit_check( segment );
    rocketnet_unit_check( gro );
    rocketnet_unit_check( fake_udp.num_calls == 2 );
    rocketnet_unit_check( fake_udp.gro_value[0] == 1 );

    memset( &fake_udp, 0, sizeof( fake_udp ) );
    fake_udp.refuse_segment[0] = true;
    fake_udp.refuse_gro[0] = true;
    rocketnet_server_udp_probe_offloads( fake_udp_fds, 1, true, true, fake_udp_setsockopt, &segment, &gro );
    rocketnet_unit_check( !segment );
    rocketnet_unit_check( !gro );
}

REGISTER_UNIT_TEST( test_server_udp_probe_offloads_group_of_one );

// ----------------------------------------------------------------
// the shared open (rocketnet_server_udp_open_with): fake system calls record
// every call in order and answer it, one of them refusing when a case says
// so; a log hook keeps every line. No socket, no clock.

enum fake_open_kind_t
{
    FAKE_OPEN_SOCKET = 1,
    FAKE_OPEN_SET,
    FAKE_OPEN_GET,
    FAKE_OPEN_BIND,
    FAKE_OPEN_NAME,
    FAKE_OPEN_GET_FLAGS,
    FAKE_OPEN_SET_FLAGS,
    FAKE_OPEN_CLOSE,
};

#define FAKE_OPEN_MAX_CALLS 32
#define FAKE_OPEN_MAX_LINES 16
#define FAKE_OPEN_FD 21
#define FAKE_OPEN_FLAGS_BEFORE 02
#define FAKE_OPEN_REFUSAL ENOBUFS

struct fake_open_call_t
{
    int kind;
    int fd;
    int a; // socket: domain; set, get: level; bind: family; set_flags: flags
    int b; // socket: type; set, get: option name; bind: port
    int c; // socket: protocol; set: value; bind: length
};

struct fake_open_line_t
{
    int level;
    char text[512];
};

struct fake_open_world_t
{
    // the answers
    int socket_fd;
    int refuse_kind;          // the call that refuses with FAKE_OPEN_REFUSAL (0: none)
    int refuse_option;        // for FAKE_OPEN_SET, the option that refuses
    int granted_send_raw;     // what getsockopt SO_SNDBUF writes (the doubled value)
    int granted_receive_raw;  // what getsockopt SO_RCVBUF writes
    int name_family;          // the family getsockname reports
    uint16_t name_port;       // the port getsockname reports

    // the record
    int num_calls;
    fake_open_call_t calls[FAKE_OPEN_MAX_CALLS];
    int num_lines;
    fake_open_line_t lines[FAKE_OPEN_MAX_LINES];
};

static fake_open_world_t fake_open;

static fake_open_call_t * fake_open_record( int kind, int fd, int a, int b, int c )
{
    rocketnet_unit_check( fake_open.num_calls < FAKE_OPEN_MAX_CALLS );
    fake_open_call_t * call = &fake_open.calls[fake_open.num_calls++];
    call->kind = kind;
    call->fd = fd;
    call->a = a;
    call->b = b;
    call->c = c;
    return call;
}

static bool fake_open_refuses( int kind, int option_name )
{
    if ( fake_open.refuse_kind != kind || ( kind == FAKE_OPEN_SET && fake_open.refuse_option != option_name ) )
    {
        return false;
    }
    errno = FAKE_OPEN_REFUSAL;
    return true;
}

static int fake_open_socket( int domain, int type, int protocol )
{
    fake_open_record( FAKE_OPEN_SOCKET, -1, domain, type, protocol );
    return fake_open_refuses( FAKE_OPEN_SOCKET, 0 ) ? -1 : fake_open.socket_fd;
}

static int fake_open_set_option( int fd, int level, int option_name, const void * value, socklen_t length )
{
    rocketnet_unit_check( length == sizeof( int ) );
    int written = 0;
    memcpy( &written, value, sizeof( written ) );
    fake_open_record( FAKE_OPEN_SET, fd, level, option_name, written );
    return fake_open_refuses( FAKE_OPEN_SET, option_name ) ? -1 : 0;
}

static int fake_open_get_option( int fd, int level, int option_name, void * value, socklen_t * length )
{
    rocketnet_unit_check( length && *length == sizeof( int ) );
    fake_open_record( FAKE_OPEN_GET, fd, level, option_name, 0 );
    const int granted = option_name == SO_SNDBUF ? fake_open.granted_send_raw : fake_open.granted_receive_raw;
    memcpy( value, &granted, sizeof( granted ) );
    *length = sizeof( int );
    return 0;
}

static int fake_open_bind( int fd, const struct sockaddr * address, socklen_t length )
{
    int port = 0;
    if ( address->sa_family == AF_INET )
    {
        port = ntohs( ( (const struct sockaddr_in *) address )->sin_port );
    }
    else if ( address->sa_family == AF_INET6 )
    {
        port = ntohs( ( (const struct sockaddr_in6 *) address )->sin6_port );
    }
    fake_open_record( FAKE_OPEN_BIND, fd, address->sa_family, port, (int) length );
    return fake_open_refuses( FAKE_OPEN_BIND, 0 ) ? -1 : 0;
}

static int fake_open_socket_name( int fd, struct sockaddr * address, socklen_t * length )
{
    fake_open_record( FAKE_OPEN_NAME, fd, 0, 0, 0 );
    if ( fake_open_refuses( FAKE_OPEN_NAME, 0 ) )
    {
        return -1;
    }
    rocketnet_unit_check( *length >= sizeof( struct sockaddr_in6 ) );
    if ( fake_open.name_family == AF_INET6 )
    {
        struct sockaddr_in6 name;
        memset( &name, 0, sizeof( name ) );
        name.sin6_family = AF_INET6;
        name.sin6_port = htons( fake_open.name_port );
        name.sin6_addr = in6addr_any;
        memcpy( address, &name, sizeof( name ) );
        *length = sizeof( name );
    }
    else
    {
        struct sockaddr_in name;
        memset( &name, 0, sizeof( name ) );
        name.sin_family = (sa_family_t) fake_open.name_family;
        name.sin_port = htons( fake_open.name_port );
        name.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        memcpy( address, &name, sizeof( name ) );
        *length = sizeof( name );
    }
    return 0;
}

static int fake_open_get_flags( int fd )
{
    fake_open_record( FAKE_OPEN_GET_FLAGS, fd, 0, 0, 0 );
    return FAKE_OPEN_FLAGS_BEFORE;
}

static int fake_open_set_flags( int fd, int flags )
{
    fake_open_record( FAKE_OPEN_SET_FLAGS, fd, flags, 0, 0 );
    return fake_open_refuses( FAKE_OPEN_SET_FLAGS, 0 ) ? -1 : 0;
}

static int fake_open_close( int fd )
{
    fake_open_record( FAKE_OPEN_CLOSE, fd, 0, 0, 0 );
    return 0;
}

static const rocketnet_server_udp_calls_t fake_open_calls = {
    fake_open_socket,
    fake_open_set_option,
    fake_open_get_option,
    fake_open_bind,
    fake_open_socket_name,
    fake_open_get_flags,
    fake_open_set_flags,
    fake_open_close,
};

static void fake_open_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( fake_open.num_lines >= FAKE_OPEN_MAX_LINES )
    {
        return;
    }
    fake_open_line_t * line = &fake_open.lines[fake_open.num_lines++];
    line->level = (int) level;
    snprintf( line->text, sizeof( line->text ), "%s", message );
}

static const int fake_open_max_clients = 64;

static int fake_open_send_request()
{
    return ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
}

static int fake_open_receive_request()
{
    return rocketnet_server_transport_receive_buffer_request( "gso", fake_open_max_clients );
}

// a fresh world: the kernel grants both requests in full (getsockopt reports
// double the grant) and getsockname reports the bound port
static void fake_open_reset()
{
    memset( &fake_open, 0, sizeof( fake_open ) );
    fake_open.socket_fd = FAKE_OPEN_FD;
    fake_open.granted_send_raw = 2 * fake_open_send_request();
    fake_open.granted_receive_raw = 2 * fake_open_receive_request();
    fake_open.name_family = AF_INET;
    fake_open.name_port = 40123;
}

// open over the fake with every line logged and captured
static int fake_open_run( const char * address_string, int queue_index, uint16_t * bound_port )
{
    rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    if ( address_string )
    {
        rocketnet_unit_check( rocketnet_address_parse( &address, address_string ) );
    }

    const enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( fake_open_log, NULL );

    const int fd = rocketnet_server_udp_open_with( &fake_open_calls, "gso", &address, queue_index, fake_open_max_clients,
                                                   bound_port );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
    return fd;
}

static void fake_open_check_call( int index, int kind, int a, int b, int c )
{
    rocketnet_unit_check( index < fake_open.num_calls );
    const fake_open_call_t * call = &fake_open.calls[index];
    rocketnet_unit_check( call->kind == kind );
    rocketnet_unit_check( call->a == a );
    rocketnet_unit_check( call->b == b );
    rocketnet_unit_check( call->c == c );
    if ( kind != FAKE_OPEN_SOCKET )
    {
        rocketnet_unit_check( call->fd == fake_open.socket_fd );
    }
}

static void fake_open_check_set( int index, int level, int option_name, int value )
{
    fake_open_check_call( index, FAKE_OPEN_SET, level, option_name, value );
}

static int fake_open_lines_with( int level, const char * text )
{
    int count = 0;
    for ( int i = 0; i < fake_open.num_lines; i++ )
    {
        if ( fake_open.lines[i].level == level && strstr( fake_open.lines[i].text, text ) != NULL )
        {
            count++;
        }
    }
    return count;
}

static int fake_open_lines_at( int level )
{
    int count = 0;
    for ( int i = 0; i < fake_open.num_lines; i++ )
    {
        count += fake_open.lines[i].level == level ? 1 : 0;
    }
    return count;
}

// the first queue, ipv4, ephemeral port: the socket, the buffer requests and
// their read back, the bind before SO_REUSEPORT, the discovered port, non
// blocking, don't-fragment; every option at its exact level and value, in
// this order, and the grant reported once in full

static void test_server_udp_open_first_queue_ipv4_calls_in_order()
{
    fake_open_reset();
    uint16_t bound_port = 0;
    const int fd = fake_open_run( "127.0.0.1:0", 0, &bound_port );

    rocketnet_unit_check( fd == FAKE_OPEN_FD );
    rocketnet_unit_check( bound_port == 40123 );

    int i = 0;
    fake_open_check_call( i++, FAKE_OPEN_SOCKET, AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP );
    fake_open_check_set( i++, SOL_SOCKET, SO_SNDBUF, fake_open_send_request() );
    fake_open_check_set( i++, SOL_SOCKET, SO_RCVBUF, fake_open_receive_request() );
    fake_open_check_call( i++, FAKE_OPEN_GET, SOL_SOCKET, SO_SNDBUF, 0 );
    fake_open_check_call( i++, FAKE_OPEN_GET, SOL_SOCKET, SO_RCVBUF, 0 );
    fake_open_check_call( i++, FAKE_OPEN_BIND, AF_INET, 0, (int) sizeof( struct sockaddr_in ) );
    fake_open_check_set( i++, SOL_SOCKET, SO_REUSEPORT, 1 );
    fake_open_check_call( i++, FAKE_OPEN_NAME, 0, 0, 0 );
    fake_open_check_call( i++, FAKE_OPEN_GET_FLAGS, 0, 0, 0 );
    fake_open_check_call( i++, FAKE_OPEN_SET_FLAGS, FAKE_OPEN_FLAGS_BEFORE | O_NONBLOCK, 0, 0 );
    fake_open_check_set( i++, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO );
    rocketnet_unit_check( fake_open.num_calls == i );

    char granted[128];
    snprintf( granted, sizeof( granted ), "gso transport: socket buffers granted: send %d receive %d", fake_open_send_request(),
              fake_open_receive_request() );
    rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_INFO, granted ) == 1 );
    rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_WARN ) == 0 );
    rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_ERROR ) == 0 );
}

REGISTER_UNIT_TEST( test_server_udp_open_first_queue_ipv4_calls_in_order );

// the first queue with an explicit port binds that port and still reads the
// bound port back; a port handed in through bound_port wins over the address

static void test_server_udp_open_binds_the_port_asked_for()
{
    fake_open_reset();
    fake_open.name_port = 40000;
    uint16_t bound_port = 0;
    rocketnet_unit_check( fake_open_run( "127.0.0.1:40000", 0, &bound_port ) == FAKE_OPEN_FD );
    rocketnet_unit_check( fake_open.calls[5].kind == FAKE_OPEN_BIND && fake_open.calls[5].b == 40000 );
    rocketnet_unit_check( bound_port == 40000 );

    fake_open_reset();
    bound_port = 40001;
    rocketnet_unit_check( fake_open_run( "127.0.0.1:0", 0, &bound_port ) == FAKE_OPEN_FD );
    rocketnet_unit_check( fake_open.calls[5].kind == FAKE_OPEN_BIND && fake_open.calls[5].b == 40001 );
    rocketnet_unit_check( bound_port == 40001 );
    for ( int i = 0; i < fake_open.num_calls; i++ )
    {
        rocketnet_unit_check( fake_open.calls[i].kind != FAKE_OPEN_NAME );
    }
}

REGISTER_UNIT_TEST( test_server_udp_open_binds_the_port_asked_for );

// a later queue, ipv6, joining the first queue's port: SO_REUSEPORT before
// the bind (and not after), dual stack, the handed-in port bound and not read
// back, both don't-fragment options; a later queue reports no grant, even a
// short one

static void test_server_udp_open_later_queue_ipv6_calls_in_order()
{
    fake_open_reset();
    fake_open.granted_send_raw = 2;
    fake_open.granted_receive_raw = 2;
    uint16_t bound_port = 40123;
    const int fd = fake_open_run( "[::]:0", 1, &bound_port );

    rocketnet_unit_check( fd == FAKE_OPEN_FD );
    rocketnet_unit_check( bound_port == 40123 );

    int i = 0;
    fake_open_check_call( i++, FAKE_OPEN_SOCKET, AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP );
    fake_open_check_set( i++, SOL_SOCKET, SO_REUSEPORT, 1 );
    fake_open_check_set( i++, IPPROTO_IPV6, IPV6_V6ONLY, 0 );
    fake_open_check_set( i++, SOL_SOCKET, SO_SNDBUF, fake_open_send_request() );
    fake_open_check_set( i++, SOL_SOCKET, SO_RCVBUF, fake_open_receive_request() );
    fake_open_check_call( i++, FAKE_OPEN_GET, SOL_SOCKET, SO_SNDBUF, 0 );
    fake_open_check_call( i++, FAKE_OPEN_GET, SOL_SOCKET, SO_RCVBUF, 0 );
    fake_open_check_call( i++, FAKE_OPEN_BIND, AF_INET6, 40123, (int) sizeof( struct sockaddr_in6 ) );
    fake_open_check_call( i++, FAKE_OPEN_GET_FLAGS, 0, 0, 0 );
    fake_open_check_call( i++, FAKE_OPEN_SET_FLAGS, FAKE_OPEN_FLAGS_BEFORE | O_NONBLOCK, 0, 0 );
    fake_open_check_set( i++, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IPV6_PMTUDISC_DO );
    fake_open_check_set( i++, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO );
    rocketnet_unit_check( fake_open.num_calls == i );

    rocketnet_unit_check( fake_open.num_lines == 0 );
}

REGISTER_UNIT_TEST( test_server_udp_open_later_queue_ipv6_calls_in_order );

// descriptor 0 is a socket like any other

static void test_server_udp_open_descriptor_zero_is_a_socket()
{
    fake_open_reset();
    fake_open.socket_fd = 0;
    uint16_t bound_port = 0;
    rocketnet_unit_check( fake_open_run( "127.0.0.1:0", 0, &bound_port ) == 0 );
    rocketnet_unit_check( bound_port == 40123 );
    for ( int i = 0; i < fake_open.num_calls; i++ )
    {
        rocketnet_unit_check( fake_open.calls[i].kind != FAKE_OPEN_CLOSE );
    }
}

REGISTER_UNIT_TEST( test_server_udp_open_descriptor_zero_is_a_socket );

// every step that can fail: the open returns -1, makes no call after the
// refused one but the close of the socket (none when the socket itself was
// refused), logs one error naming the step and the system's reason, and leaves a
// handed-in port as it was (a later queue makes the non-blocking refusal, past
// the point where the first queue writes the port it discovered)

struct fake_open_refusal_t
{
    const char * address;
    int queue_index;
    uint16_t bound_port;
    int kind;
    int option_name;
    const char * reason;
};

static const fake_open_refusal_t fake_open_refusals[] = {
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_SOCKET, 0, "gso transport: failed to create socket: " },
    { "127.0.0.1:0", 1, 40123, FAKE_OPEN_SET, SO_REUSEPORT, "gso transport: failed to set SO_REUSEPORT: " },
    { "[::]:0", 0, 0, FAKE_OPEN_SET, IPV6_V6ONLY, "gso transport: failed to clear socket ipv6 only: " },
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_SET, SO_SNDBUF, "gso transport: failed to set socket send buffer size: " },
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_SET, SO_RCVBUF, "gso transport: failed to set socket receive buffer size: " },
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_BIND, 0, "gso transport: failed to bind socket (ipv4): " },
    { "[::]:0", 0, 0, FAKE_OPEN_BIND, 0, "gso transport: failed to bind socket (ipv6): " },
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_SET, SO_REUSEPORT, "gso transport: failed to set SO_REUSEPORT: " },
    { "127.0.0.1:0", 0, 0, FAKE_OPEN_NAME, 0, "gso transport: failed to get bound socket port: " },
    { "127.0.0.1:0", 1, 40123, FAKE_OPEN_SET_FLAGS, 0, "gso transport: failed to set socket non-blocking: " },
};

static void test_server_udp_open_every_refusal_closes_and_names_its_reason()
{
    const int count = (int) ( sizeof( fake_open_refusals ) / sizeof( fake_open_refusals[0] ) );
    for ( int r = 0; r < count; r++ )
    {
        const fake_open_refusal_t * refusal = &fake_open_refusals[r];
        fake_open_reset();
        fake_open.refuse_kind = refusal->kind;
        fake_open.refuse_option = refusal->option_name;
        uint16_t bound_port = refusal->bound_port;
        rocketnet_unit_check( fake_open_run( refusal->address, refusal->queue_index, &bound_port ) == -1 );
        rocketnet_unit_check( bound_port == refusal->bound_port );

        // the refused call is the last one made, but for the close
        const bool closes = refusal->kind != FAKE_OPEN_SOCKET;
        const int refused_index = fake_open.num_calls - ( closes ? 2 : 1 );
        rocketnet_unit_check( refused_index >= 0 );
        rocketnet_unit_check( fake_open.calls[refused_index].kind == refusal->kind );
        if ( refusal->kind == FAKE_OPEN_SET )
        {
            rocketnet_unit_check( fake_open.calls[refused_index].b == refusal->option_name );
        }
        int closes_made = 0;
        for ( int i = 0; i < fake_open.num_calls; i++ )
        {
            closes_made += fake_open.calls[i].kind == FAKE_OPEN_CLOSE ? 1 : 0;
        }
        rocketnet_unit_check( closes_made == ( closes ? 1 : 0 ) );
        if ( closes )
        {
            fake_open_check_call( fake_open.num_calls - 1, FAKE_OPEN_CLOSE, 0, 0, 0 );
        }

        char line[256];
        snprintf( line, sizeof( line ), "%s%s", refusal->reason, strerror( FAKE_OPEN_REFUSAL ) );
        rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_ERROR ) == 1 );
        rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_ERROR, line ) == 1 );
    }
}

REGISTER_UNIT_TEST( test_server_udp_open_every_refusal_closes_and_names_its_reason );

// a bound address getsockname reports in a family the open cannot read, and a
// bind address of no family, are refused the same way: -1, the socket
// closed, one error naming the step; the address of no family is refused
// before any bind is made

static void test_server_udp_open_refuses_unreadable_addresses()
{
    fake_open_reset();
    fake_open.name_family = AF_UNIX;
    uint16_t bound_port = 0;
    rocketnet_unit_check( fake_open_run( "127.0.0.1:0", 0, &bound_port ) == -1 );
    rocketnet_unit_check( bound_port == 0 );
    rocketnet_unit_check( fake_open.calls[fake_open.num_calls - 2].kind == FAKE_OPEN_NAME );
    fake_open_check_call( fake_open.num_calls - 1, FAKE_OPEN_CLOSE, 0, 0, 0 );
    rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_ERROR ) == 1 );
    rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_ERROR, "gso transport: could not read bound socket address" ) == 1 );

    fake_open_reset();
    bound_port = 0;
    rocketnet_unit_check( fake_open_run( NULL, 0, &bound_port ) == -1 );
    for ( int i = 0; i < fake_open.num_calls; i++ )
    {
        rocketnet_unit_check( fake_open.calls[i].kind != FAKE_OPEN_BIND );
    }
    fake_open_check_call( fake_open.num_calls - 1, FAKE_OPEN_CLOSE, 0, 0, 0 );
    rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_ERROR ) == 1 );
    rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_ERROR, "gso transport: failed to bind socket (ipv4): " ) == 1 );
}

REGISTER_UNIT_TEST( test_server_udp_open_refuses_unreadable_addresses );

// the first queue's grant report at its edges: getsockopt reports double the
// grant, so the halved value is compared with the request. A grant one byte
// short of either request is a warning naming the shortfall, and only a grant
// of both in full is the information line

static void fake_open_check_grant( int send_raw, int receive_raw, int send_granted, int receive_granted )
{
    fake_open_reset();
    fake_open.granted_send_raw = send_raw;
    fake_open.granted_receive_raw = receive_raw;
    uint16_t bound_port = 0;
    rocketnet_unit_check( fake_open_run( "127.0.0.1:0", 0, &bound_port ) == FAKE_OPEN_FD );

    const int send_request = fake_open_send_request();
    const int receive_request = fake_open_receive_request();
    const bool send_full = send_granted >= send_request;
    const bool receive_full = receive_granted >= receive_request;

    char line[256];
    snprintf( line, sizeof( line ), "gso transport: socket send buffer granted %d of %d requested bytes", send_granted, send_request );
    rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_WARN, line ) == ( send_full ? 0 : 1 ) );

    snprintf( line, sizeof( line ), "server gso: socket receive buffer granted %d of %d requested bytes", receive_granted, receive_request );
    rocketnet_unit_check( fake_open_lines_with( receive_full ? ROCKETNET_LOG_LEVEL_INFO : ROCKETNET_LOG_LEVEL_WARN, line ) == 1 );

    snprintf( line, sizeof( line ), "gso transport: socket buffers granted: send %d receive %d", send_granted, receive_granted );
    rocketnet_unit_check( fake_open_lines_with( ROCKETNET_LOG_LEVEL_INFO, line ) == ( send_full && receive_full ? 1 : 0 ) );
    rocketnet_unit_check( fake_open_lines_at( ROCKETNET_LOG_LEVEL_WARN ) == ( send_full ? 0 : 1 ) + ( receive_full ? 0 : 1 ) );
}

static void test_server_udp_open_reports_the_halved_grant()
{
    const int send = fake_open_send_request();
    const int receive = fake_open_receive_request();
    rocketnet_unit_check( send > 2 && receive > 2 );

    fake_open_check_grant( 2 * send, 2 * receive, send, receive );
    fake_open_check_grant( 2 * send - 2, 2 * receive, send - 1, receive );
    fake_open_check_grant( 2 * send, 2 * receive - 2, send, receive - 1 );
    fake_open_check_grant( 2 * send - 2, 2 * receive - 2, send - 1, receive - 1 );

    // a kernel reporting the grant undoubled: half of each request
    fake_open_check_grant( send, receive, send / 2, receive / 2 );
}

REGISTER_UNIT_TEST( test_server_udp_open_reports_the_halved_grant );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
