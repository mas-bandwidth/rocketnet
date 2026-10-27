/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An in-memory fake client transport for the I/O step's unit tests (docs/
// CLIENT-IO.md "The I/O step", tla/RNClientIo.tla): no socket, no thread, no
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

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS 256

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
    // connect: the calls, the address of the last one, the sends and
    // receives made before it, and whether it refuses
    int connect_calls;
    rocketnet_address_t connected_to;
    int calls_before_connect;
    bool refuse_connect;
};

// create returns its context argument (the test passes the fake as the
// context) and leaves bind unchanged.
static void * rocketnet_fake_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;
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
    assert( fake->num_sent < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
    dg->address = *to;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
}

// receive pops one inbound datagram into the caller's buffer, copying at most
// the capacity, and returns its bytes, or 0 when drained.
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
    int n = dg->bytes < capacity ? dg->bytes : capacity;
    memcpy( data, dg->data, (size_t) n );
    return n;
}

// send_batch appends each send.
static void rocketnet_fake_client_send_batch( void * transport, const rocketnet_platform_send_t * sends, int count )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->send_batch_calls++;
    for ( int i = 0; i < count; i++ )
    {
        assert( fake->num_sent < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
        rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
        dg->address = *sends[i].to;
        dg->bytes = sends[i].packet_bytes;
        memcpy( dg->data, sends[i].packet_data, (size_t) sends[i].packet_bytes );
    }
}

// receive_batch pops up to max_packets into packet_data + i * packet_stride,
// writing from[i] and packet_bytes[i], and returns the count.
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
        int n = dg->bytes < packet_stride ? dg->bytes : packet_stride;
        memcpy( packet_data + got * packet_stride, dg->data, (size_t) n );
        packet_bytes[got] = n;
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
// receives came before it, and answers !refuse_connect. No table below has
// it: a case that wants it sets operations.connect on its own copy.
static inline bool rocketnet_fake_client_connect( void * transport, const rocketnet_address_t * server_address )
{
    rocketnet_fake_client_transport_io_t * fake = (rocketnet_fake_client_transport_io_t *) transport;
    fake->connect_calls++;
    fake->connected_to = *server_address;
    fake->calls_before_connect = fake->send_calls + fake->send_batch_calls + fake->receive_calls + fake->receive_batch_calls;
    return !fake->refuse_connect;
}

// The three tables: base (four required), io (four plus wait and wake), batch
// (all eight), each in rocketnet_client_transport_io_operations_t member order;
// none has send_failures or connect.

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_base =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, NULL, NULL,
    NULL, NULL
};

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_io =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, rocketnet_fake_client_wait, rocketnet_fake_client_wake,
    NULL, NULL
};

static const rocketnet_client_transport_io_operations_t rocketnet_fake_client_operations_batch =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    rocketnet_fake_client_send_batch, rocketnet_fake_client_receive_batch,
    rocketnet_fake_client_wait, rocketnet_fake_client_wake,
    NULL, NULL
};

// Push an inbound datagram (docs/CLIENT-IO.md "The I/O step"). Appends past
// ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS assert.

inline void rocketnet_fake_client_push( rocketnet_fake_client_transport_io_t * fake, const rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    assert( fake->inbound_count < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    rocketnet_fake_client_datagram_t * dg = &fake->inbound[( fake->inbound_head + fake->inbound_count ) % ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    dg->address = *from;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
    fake->inbound_count++;
}

#endif // ROCKETNET_TESTS_FAKE_CLIENT_DRIVER_H
