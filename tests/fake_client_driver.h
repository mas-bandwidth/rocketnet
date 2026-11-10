/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An in-memory fake client transport for the I/O step's unit tests (docs/
// CLIENT-IO.md "The I/O step", proofs/tla/RNClientIo.tla): no socket, no thread, no
// time; wait never blocks. Unit tests drive the step with this transport on the
// test's thread. Four callbacks are required (create/destroy/send/receive);
// wait, wake, send_batch and receive_batch are optional, and the three
// static const rocketnet_client_transport_io_operations_t tables expose the three
// subsets in the transport struct's member order.

#pragma once

#ifndef ROCKETNET_TESTS_FAKE_CLIENT_DRIVER_H
#define ROCKETNET_TESTS_FAKE_CLIENT_DRIVER_H

#include "rocketnet_internal.h"
#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS 256

// a full queue fails the test under every build, NDEBUG included

static inline void rocketnet_fake_client_check_room( int count, const char * queue )
{
    if ( count < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS )
        return;
    printf( "fake client driver: %s queue full at %d datagrams (ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS)\n", queue, ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    exit( 1 );
}

struct rocketnet_fake_client_datagram_t
{
    rocketnet_address_t address;
    int bytes;
    uint8_t data[ROCKETNET_RECEIVE_PACKET_BYTES];
};

struct rocketnet_fake_client_transport_io_t
{
    rocketnet_fake_client_datagram_t inbound[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    int inbound_head;
    int inbound_count;
    rocketnet_fake_client_datagram_t sent[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    int num_sent;
    int send_calls, receive_calls, send_batch_calls, receive_batch_calls, wait_calls, wake_calls, destroy_calls;
    bool wake;
    double last_wait_timeout;
    bool last_wait_readable;
    // create by family: the calls, the family of the last bind, and which
    // bind families it refuses; a refusal reports OUT_OF_MEMORY when
    // open_out_of_memory is set
    int create_calls;
    int last_bind_family;
    bool refuse_open_ipv4, refuse_open_ipv6;
    bool open_out_of_memory;
    // connect: the calls, the address of the last one, the sends and
    // receives made before it, and whether it refuses every server or the
    // servers of one family
    int connect_calls;
    rocketnet_address_t connected_to;
    int calls_before_connect;
    bool refuse_connect;
    bool refuse_connect_ipv4, refuse_connect_ipv6;
    // close: the calls (the family race's losing family, closed by the
    // reading thread; its memory goes at destroy)
    int close_calls;
};

// create returns its context argument (the test passes the fake as the
// context) and leaves bind unchanged.

static void * rocketnet_fake_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;
}

// create by family: counts, records the bind family, and refuses a bind
// family the fake refuses, with OUT_OF_MEMORY in the config's status when
// open_out_of_memory is set; otherwise as create. No table below has it: a
// case that wants it sets operations.create on its own copy.

static inline void * rocketnet_fake_client_create_by_family( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) context;
    fake->create_calls++;
    fake->last_bind_family = bind->type;

    const bool refused = ( bind->type == ROCKETNET_ADDRESS_IPV4 && fake->refuse_open_ipv4 ) ||
                         ( bind->type == ROCKETNET_ADDRESS_IPV6 && fake->refuse_open_ipv6 );
    if ( refused )
    {
        if ( fake->open_out_of_memory && config->status )
            *config->status = ROCKETNET_STATUS_OUT_OF_MEMORY;
        return NULL;
    }

    return rocketnet_fake_client_create( context, config, bind );
}

// destroy counts.

static void rocketnet_fake_client_destroy( void * transport )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->destroy_calls++;
}

// send appends to sent.

static void rocketnet_fake_client_send( void * transport, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->send_calls++;
    rocketnet_fake_client_check_room( fake->num_sent, "sent" );

    rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
    dg->address = *to;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
}

// the bytes a receive copies: the stored prefix of a datagram, at most the
// caller's room. The wire size, which may exceed both, is what is reported
// (sdk/rocketnet_driver.h).

static inline int rocketnet_fake_client_copy_bytes( const rocketnet_fake_client_datagram_t * dg, int room )
{
    int n = dg->bytes < room ? dg->bytes : room;
    return n < (int) sizeof( dg->data ) ? n : (int) sizeof( dg->data );
}

// receive pops one inbound datagram into the caller's buffer, copying at most
// the capacity, and returns its wire size, or 0 when drained.

static int rocketnet_fake_client_receive( void * transport, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->receive_calls++;
    if ( fake->inbound_count == 0 )
        return 0;

    rocketnet_fake_client_datagram_t * dg = &fake->inbound[fake->inbound_head];
    fake->inbound_head = ( fake->inbound_head + 1 ) % ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS;
    fake->inbound_count--;
    *from = dg->address;

    memcpy( data, dg->data, (size_t) rocketnet_fake_client_copy_bytes( dg, capacity ) );

    return dg->bytes;
}

// send_batch appends each send.

static void rocketnet_fake_client_send_batch( void * transport, const rocketnet_platform_send_t * sends, int count )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->send_batch_calls++;

    for ( int i = 0; i < count; i++ )
    {
        rocketnet_fake_client_check_room( fake->num_sent, "sent" );

        rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
        dg->address = *sends[i].to;
        dg->bytes = sends[i].packet_bytes;
        memcpy( dg->data, sends[i].packet_data, (size_t) sends[i].packet_bytes );
    }
}

// receive_batch pops up to max_packets into packet_data + i * packet_stride,
// copying at most the stride, writing from[i] and each wire size to
// packet_bytes[i], and returns the count.

static int rocketnet_fake_client_receive_batch( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->receive_batch_calls++;

    int got = 0;
    while ( got < max_packets && fake->inbound_count > 0 )
    {
        rocketnet_fake_client_datagram_t * dg = &fake->inbound[fake->inbound_head];
        fake->inbound_head = ( fake->inbound_head + 1 ) % ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS;
        fake->inbound_count--;
        from[got] = dg->address;

        memcpy( packet_data + got * packet_stride, dg->data, (size_t) rocketnet_fake_client_copy_bytes( dg, packet_stride ) );
        packet_bytes[got] = dg->bytes;
        got++;
    }

    return got;
}

// wait counts, records the timeout and readable, returns
// wake || ( readable && inbound_count > 0 ) (a datagram counts only when
// readable is true), and clears wake; it never blocks.

static bool rocketnet_fake_client_wait( void * transport, bool readable, double timeout_seconds )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->wait_calls++;
    fake->last_wait_timeout = timeout_seconds;
    fake->last_wait_readable = readable;

    bool ready = fake->wake || ( readable && fake->inbound_count > 0 );
    fake->wake = false;

    return ready;
}

// wake sets wake and counts.

static void rocketnet_fake_client_wake( void * transport )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->wake_calls++;
    fake->wake = true;
}

// connect counts, records the server address and how many sends and
// receives came before it, and refuses when refuse_connect is set or the
// server's family is refused. No table below has it: a case that wants it
// sets operations.connect on its own copy.

static inline bool rocketnet_fake_client_connect( void * transport, const rocketnet_address_t * server_address )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->connect_calls++;
    fake->connected_to = *server_address;
    fake->calls_before_connect = fake->send_calls + fake->send_batch_calls + fake->receive_calls + fake->receive_batch_calls;
    if ( fake->refuse_connect )
        return false;
    if ( server_address->type == ROCKETNET_ADDRESS_IPV4 && fake->refuse_connect_ipv4 )
        return false;
    if ( server_address->type == ROCKETNET_ADDRESS_IPV6 && fake->refuse_connect_ipv6 )
        return false;
    return true;
}

// close counts. No table below has it: a case that wants it sets
// operations.close on its own copy.
static inline void rocketnet_fake_client_close( void * transport )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->close_calls++;
}

// The three tables: base (four required), io (four plus wait and wake), batch
// (all eight), each in rocketnet_client_transport_io_operations_t member order;
// none has create by family, send_failures, connect or close.

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_base = {
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL
};

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_io = {
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, rocketnet_fake_client_wait, rocketnet_fake_client_wake,
    NULL, NULL, NULL, NULL
};

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_batch = {
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    rocketnet_fake_client_send_batch, rocketnet_fake_client_receive_batch,
    rocketnet_fake_client_wait, rocketnet_fake_client_wake,
    NULL, NULL, NULL, NULL
};

// Push an inbound datagram (docs/CLIENT-IO.md "The I/O step"). An append past
// ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS fails the test.

inline void rocketnet_fake_client_push( rocketnet_fake_client_transport_io_t * fake, const rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    rocketnet_fake_client_check_room( fake->inbound_count, "inbound" );

    rocketnet_fake_client_datagram_t * dg = &fake->inbound[( fake->inbound_head + fake->inbound_count ) % ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    dg->address = *from;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
    fake->inbound_count++;
}

#endif // ROCKETNET_TESTS_FAKE_CLIENT_DRIVER_H
