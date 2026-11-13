/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The gso client's receive (sdk/rocketnet_client_gso_linux.cpp) on a scripted
// fake socket: each recvmmsg answers the next scripted read (an errno, or one
// message with an optional UDP_GRO cmsg), so what a call delivers and how many
// times it reads are exact. No socket, no thread, no time. Every expected
// value is a literal, never computed by the code under test.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_driver.h"
#include "rocketnet_client_gso.h"
#include "rocketnet_memory.h"
#include "rocketnet_platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef UDP_GRO
#define UDP_GRO 104
#endif // #ifndef UDP_GRO

// a failing check prints the fault it found, then fails as every unit check does

#define delta_gso_check( condition, fault )                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        if ( !( condition ) )                                                                                          \
        {                                                                                                              \
            printf( "mutation gap: %s\n", fault );                                                                     \
        }                                                                                                              \
        rocketnet_unit_check( condition );                                                                             \
    } while ( 0 )

#define DELTA_GSO_SCRIPT 8

// one scripted read: error nonzero fails the call with that errno; otherwise
// one message of bytes bytes, byte i equal to tag + i / stride (tag when stride
// is 0), with a UDP_GRO cmsg holding stride when gro is set
struct delta_gso_read_t
{
    int error;
    int bytes;
    int stride;
    int tag;
    bool gro;
};

struct delta_gso_socket_t
{
    delta_gso_read_t reads[DELTA_GSO_SCRIPT];
    int num_reads;
    int next_read;
    int recvmmsg_calls;
};

static int delta_gso_sendmmsg( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    (void) context;
    (void) socket;
    (void) messages;
    return (int) num_messages;
}

static int delta_gso_segment_probe( void * context, int socket )
{
    (void) context;
    (void) socket;
    return 0;
}

static int delta_gso_gro_enable( void * context, int socket )
{
    (void) context;
    (void) socket;
    return 0;
}

// past the script the socket is empty: EAGAIN

static int delta_gso_recvmmsg( void * context, int socket, struct mmsghdr * messages, unsigned int num_messages )
{
    delta_gso_socket_t * fake = (delta_gso_socket_t *) context;
    rocketnet_unit_check( socket == 7 );
    rocketnet_unit_check( num_messages == 1 );
    fake->recvmmsg_calls++;

    if ( fake->next_read >= fake->num_reads )
    {
        errno = EAGAIN;
        return -1;
    }

    const delta_gso_read_t * read = &fake->reads[fake->next_read++];
    if ( read->error != 0 )
    {
        errno = read->error;
        return -1;
    }

    struct msghdr * header = &messages[0].msg_hdr;
    rocketnet_unit_check( header->msg_iovlen == 1 );
    rocketnet_unit_check( header->msg_iov[0].iov_len >= (size_t) read->bytes );

    uint8_t * data = (uint8_t *) header->msg_iov[0].iov_base;
    for ( int i = 0; i < read->bytes; i++ )
    {
        data[i] = (uint8_t) ( read->tag + ( read->stride > 0 ? i / read->stride : 0 ) );
    }
    messages[0].msg_len = (unsigned int) read->bytes;

    struct sockaddr_in name;
    memset( &name, 0, sizeof( name ) );
    name.sin_family = AF_INET;
    name.sin_port = htons( 40000 );
    name.sin_addr.s_addr = htonl( 0x7F000001 );
    memcpy( header->msg_name, &name, sizeof( name ) );
    header->msg_namelen = sizeof( name );
    header->msg_flags = 0;

    if ( !read->gro )
    {
        header->msg_controllen = 0;
        return 1;
    }

    rocketnet_unit_check( header->msg_controllen >= CMSG_SPACE( sizeof( int ) ) );
    memset( header->msg_control, 0, header->msg_controllen );
    struct cmsghdr * cmsg = CMSG_FIRSTHDR( header );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_GRO;
    cmsg->cmsg_len = CMSG_LEN( sizeof( int ) );
    const int stride = read->stride;
    memcpy( CMSG_DATA( cmsg ), &stride, sizeof( stride ) );
    header->msg_controllen = CMSG_SPACE( sizeof( int ) );
    return 1;
}

static bool delta_gso_read_address( void * context, const struct sockaddr_storage * storage, rocketnet_address_t * from )
{
    (void) context;
    rocketnet_unit_check( storage->ss_family == AF_INET );
    const struct sockaddr_in * name = (const struct sockaddr_in *) storage;
    rocketnet_address_load_ipv4( from, name->sin_addr.s_addr, name->sin_port );
    return true;
}

static const rocketnet_client_gso_syscalls_t delta_gso_syscalls = {
    delta_gso_sendmmsg, delta_gso_segment_probe, delta_gso_recvmmsg, delta_gso_gro_enable, delta_gso_read_address
};

static delta_gso_socket_t delta_socket;

static void delta_gso_script_error( int error )
{
    rocketnet_unit_check( delta_socket.num_reads < DELTA_GSO_SCRIPT );
    delta_gso_read_t * read = &delta_socket.reads[delta_socket.num_reads++];
    memset( read, 0, sizeof( *read ) );
    read->error = error;
}

static void delta_gso_script_message( int bytes, int stride, int tag, bool gro )
{
    rocketnet_unit_check( delta_socket.num_reads < DELTA_GSO_SCRIPT );
    delta_gso_read_t * read = &delta_socket.reads[delta_socket.num_reads++];
    memset( read, 0, sizeof( *read ) );
    read->bytes = bytes;
    read->stride = stride;
    read->tag = tag;
    read->gro = gro;
}

#define DELTA_GSO_SLOTS 8
#define DELTA_GSO_STRIDE 2048

static rocketnet_client_gso_t delta_gso;
static uint8_t delta_data[DELTA_GSO_SLOTS * DELTA_GSO_STRIDE];
static rocketnet_address_t delta_from[DELTA_GSO_SLOTS];
static int delta_bytes[DELTA_GSO_SLOTS];

static void delta_gso_setup()
{
    memset( &delta_socket, 0, sizeof( delta_socket ) );
    memset( delta_data, 0, sizeof( delta_data ) );
    memset( delta_from, 0, sizeof( delta_from ) );
    memset( delta_bytes, 0, sizeof( delta_bytes ) );
    rocketnet_unit_check( rocketnet_client_gso_init( &delta_gso, &delta_gso_syscalls, &delta_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &delta_gso, true ) );
}

// slot i holds bytes bytes, every one equal to value, from 127.0.0.1:40000

static bool delta_gso_slot_is( int i, int bytes, int value )
{
    if ( delta_bytes[i] != bytes || delta_from[i].type != ROCKETNET_ADDRESS_IPV4 || delta_from[i].port != 40000 )
    {
        return false;
    }

    const uint8_t * slot = delta_data + (size_t) i * DELTA_GSO_STRIDE;
    for ( int j = 0; j < bytes; j++ )
    {
        if ( slot[j] != (uint8_t) value )
        {
            return false;
        }
    }
    return true;
}

// A weather errno (here ECONNREFUSED, a refused send's ICMP echo) spends one
// read and the call goes on to the datagram behind it, in the same call; an
// empty socket (EAGAIN) ends the call at its first read.
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive_batch) negate_condition (!rocketnet_client_gso_receive_weather( error ))

static void test_delta_client_gso_receive_reads_past_weather_and_stops_on_empty()
{
    delta_gso_setup();
    delta_gso_script_error( ECONNREFUSED );
    delta_gso_script_message( 100, 0, 0x30, false );

    const int got = rocketnet_client_gso_receive_batch( &delta_gso, delta_from, delta_data, DELTA_GSO_STRIDE, delta_bytes, 4 );
    delta_gso_check( got == 1, "a weather errno ended the gso receive instead of being skipped for the datagram behind it" );
    delta_gso_check( delta_gso_slot_is( 0, 100, 0x30 ), "the datagram behind a weather errno was not delivered whole in slot 0" );
    delta_gso_check( delta_socket.recvmmsg_calls == 2, "the gso receive did not read exactly the weather errno and the datagram" );

    // the socket is empty: one read, nothing delivered

    const int empty = rocketnet_client_gso_receive_batch( &delta_gso, delta_from, delta_data, DELTA_GSO_STRIDE, delta_bytes, 4 );
    delta_gso_check( empty == 0, "an empty socket delivered a datagram" );
    delta_gso_check( delta_socket.recvmmsg_calls == 3, "an empty socket (EAGAIN) was read again instead of ending the call" );
}
REGISTER_UNIT_TEST( test_delta_client_gso_receive_reads_past_weather_and_stops_on_empty );

// A UDP_GRO cmsg holding a stride of 0 names no segments: the message is one
// datagram, delivered whole (it never divides by the stride).
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive_batch) relational < (segment_bytes < 0)
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive_batch) boolean && (segment_bytes <= 0 && segment_bytes > message_bytes)

static void test_delta_client_gso_receive_gro_stride_zero_is_one_datagram()
{
    delta_gso_setup();
    delta_gso_script_message( 300, 0, 0x41, true );

    const int got = rocketnet_client_gso_receive_batch( &delta_gso, delta_from, delta_data, DELTA_GSO_STRIDE, delta_bytes, 4 );
    delta_gso_check( got == 1, "a GRO message with a stride of 0 was not delivered as one datagram" );
    delta_gso_check( delta_gso_slot_is( 0, 300, 0x41 ), "a GRO message with a stride of 0 was not delivered whole" );
}
REGISTER_UNIT_TEST( test_delta_client_gso_receive_gro_stride_zero_is_one_datagram );

// A stride of 1 is a real stride: a 3-byte message cut at 1 is three 1-byte
// datagrams, in order.
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive_batch) off_by_one 1 (segment_bytes <= 1)

static void test_delta_client_gso_receive_gro_stride_one_splits_every_byte()
{
    delta_gso_setup();
    delta_gso_script_message( 3, 1, 0x50, true );

    const int got = rocketnet_client_gso_receive_batch( &delta_gso, delta_from, delta_data, DELTA_GSO_STRIDE, delta_bytes, 4 );
    delta_gso_check( got == 3, "a GRO message with a stride of 1 was not cut into one datagram per byte" );
    delta_gso_check( delta_gso_slot_is( 0, 1, 0x50 ) && delta_gso_slot_is( 1, 1, 0x51 ) && delta_gso_slot_is( 2, 1, 0x52 ),
                     "the 1-byte segments of a stride-1 GRO message were not delivered in order" );
}
REGISTER_UNIT_TEST( test_delta_client_gso_receive_gro_stride_one_splits_every_byte );

// The client transport's single receive with UDP_GRO on (rocketnet_client_gso_operations.receive,
// the path a client with no I/O thread reads by): one datagram per call, each
// at its own size, the rest of a coalesced message carried to the next call;
// an empty socket is 0. The record it is handed is the gso client's (one
// allocation: context, socket, wake, then the gso state); this test's mirror
// is checked against the record's byte helper before it is used, and the
// receive path reads only the gso state, so no socket or wake is opened.
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive) off_by_one 0 (receive_batch( ..., 0 ))
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive) off_by_one 2 (receive_batch( ..., 2 ))
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive) relational < (got < 0 ? packet_bytes : 0)
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive) off_by_one 1 (got > 1 ? packet_bytes : 0)
// mutation gap: delta sdk/rocketnet_client_gso_linux.cpp (rocketnet_client_gso_receive) off_by_one 1 (got > 0 ? packet_bytes : 1)

struct delta_gso_io_mirror_t
{
    void * context;
    rocketnet_platform_socket_t * socket;
    rocketnet_platform_wake_t * wake;
    rocketnet_client_gso_t gso;
};

static delta_gso_io_mirror_t delta_io;

static void test_delta_client_gso_single_receive_takes_one_datagram_per_call()
{
    rocketnet_unit_check( rocketnet_memory_allocations( { sizeof( delta_gso_io_mirror_t ) } ) == rocketnet_client_gso_io_allocation_bytes() );

    memset( &delta_socket, 0, sizeof( delta_socket ) );
    delta_io.context = NULL;
    delta_io.socket = NULL;
    delta_io.wake = NULL;
    rocketnet_unit_check( rocketnet_client_gso_init( &delta_io.gso, &delta_gso_syscalls, &delta_socket, 7, true ) );
    rocketnet_unit_check( rocketnet_client_gso_init_receive( &delta_io.gso, true ) );

    // two 100-byte datagrams coalesced, then the socket is empty

    delta_gso_script_message( 200, 100, 0x60, true );

    memset( delta_data, 0, sizeof( delta_data ) );
    memset( delta_from, 0, sizeof( delta_from ) );

    const int first = rocketnet_client_gso_operations.receive( &delta_io, &delta_from[0], delta_data, DELTA_GSO_STRIDE );
    delta_gso_check( first == 100, "the single gso receive did not return the first datagram of a coalesced message at its own size" );
    delta_gso_check( delta_data[0] == 0x60 && delta_data[99] == 0x60 && delta_from[0].port == 40000,
                     "the single gso receive did not deliver the first datagram's bytes and source" );

    const int second = rocketnet_client_gso_operations.receive( &delta_io, &delta_from[0], delta_data, DELTA_GSO_STRIDE );
    delta_gso_check( second == 100, "the single gso receive lost the carried second datagram of a coalesced message" );
    delta_gso_check( delta_data[0] == 0x61 && delta_data[99] == 0x61, "the single gso receive did not deliver the second datagram's bytes" );
    delta_gso_check( delta_socket.recvmmsg_calls == 1, "the single gso receive read the socket while a datagram was carried" );

    const int empty = rocketnet_client_gso_operations.receive( &delta_io, &delta_from[0], delta_data, DELTA_GSO_STRIDE );
    delta_gso_check( empty == 0, "the single gso receive of an empty socket did not return 0" );
    delta_gso_check( delta_io.gso.packets_received == 2, "the single gso receive did not count exactly the two datagrams it delivered" );
}
REGISTER_UNIT_TEST( test_delta_client_gso_single_receive_takes_one_datagram_per_call );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
