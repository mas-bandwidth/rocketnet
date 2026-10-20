/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// An in-memory fake client driver for the I/O step's unit tests (docs/
// CLIENT-IO.md "The I/O step", tla/RNClientIo.tla): no socket, no thread, no
// time; wait never blocks. Unit tests drive the step with this driver on the
// test's thread. Four callbacks are required (create/destroy/send/receive);
// wait, wake, send_batch, receive_batch, post_send and reap_sends are
// optional, and the four static const rocketnet_client_driver_operations_t
// tables expose four subsets in the driver struct's member order. The
// overlapped table's sends complete only when the test completes them
// (rocketnet_fake_client_complete), or after auto_complete_reaps reaps
// (tla/RNClientIo.tla IoSend, Complete).

#pragma once

#ifndef ROCKETNET_FAKE_CLIENT_DRIVER_H
#define ROCKETNET_FAKE_CLIENT_DRIVER_H

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
    uint8_t data[ROCKETNET_MAX_PACKET_BYTES];
};

struct rocketnet_fake_client_driver_t
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
    // the overlapped send: posted (pending) and completed but not reaped (done)
    int post_calls, reap_calls;
    int pending_tags[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    int num_pending;
    int done_tags[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    bool done_ok[ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    int num_done;
    bool refuse_posts;          // post_send returns false
    int auto_complete_reaps;    // > 0: that many reaps later every pending send completes ok
    int outstanding_at_destroy; // pending plus done when destroy ran
};

// create returns its context argument (the test passes the fake as the
// context) and leaves bind unchanged.
static void * rocketnet_fake_client_create( void * context, const rocketnet_client_driver_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    return context;
}

// destroy counts.
static void rocketnet_fake_client_destroy( void * driver )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->destroy_calls++;
    fake->outstanding_at_destroy = fake->num_pending + fake->num_done;
}

// send appends to sent.
static void rocketnet_fake_client_send( void * driver, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->send_calls++;
    assert( fake->num_sent < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
    dg->address = *to;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
}

// receive pops one inbound datagram into the caller's buffer, copying at most
// the capacity, and returns its bytes, or 0 when drained.
static int rocketnet_fake_client_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
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
static void rocketnet_fake_client_send_batch( void * driver, const rocketnet_platform_send_t * sends, int count )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
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
static int rocketnet_fake_client_receive_batch( void * driver, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
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
static bool rocketnet_fake_client_wait( void * driver, bool readable, double timeout_seconds )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->wait_calls++;
    fake->last_wait_timeout = timeout_seconds;
    fake->last_wait_readable = readable;
    bool ready = fake->wake || ( readable && fake->inbound_count > 0 ) || fake->num_done > 0;
    fake->wake = false;
    return ready;
}

// wake sets wake and counts.
static void rocketnet_fake_client_wake( void * driver )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->wake_calls++;
    fake->wake = true;
}

// post_send records the datagram in sent (as send does) and its tag as
// pending; it returns false, recording nothing, while refuse_posts is set.
static bool rocketnet_fake_client_post_send( void * driver, int tag, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->post_calls++;
    if ( fake->refuse_posts )
        return false;
    assert( fake->num_sent < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    rocketnet_fake_client_datagram_t * dg = &fake->sent[fake->num_sent++];
    dg->address = *to;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
    assert( fake->num_pending < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    fake->pending_tags[fake->num_pending++] = tag;
    return true;
}

// Complete the first count pending sends, oldest first, ok or with an error
// (tla/RNClientIo.tla Complete); they wait in done until a reap.
inline void rocketnet_fake_client_complete( rocketnet_fake_client_driver_t * fake, int count, bool ok )
{
    assert( count <= fake->num_pending );
    for ( int i = 0; i < count; i++ )
    {
        fake->done_tags[fake->num_done] = fake->pending_tags[i];
        fake->done_ok[fake->num_done] = ok;
        fake->num_done++;
    }
    for ( int i = count; i < fake->num_pending; i++ )
        fake->pending_tags[i - count] = fake->pending_tags[i];
    fake->num_pending -= count;
}

// reap_sends pops up to max completed sends, oldest first; with
// auto_complete_reaps set it counts down and completes every pending send ok
// when it reaches zero, before the pop.
static int rocketnet_fake_client_reap_sends( void * driver, int * tags, bool * ok, int max )
{
    rocketnet_fake_client_driver_t * fake = (rocketnet_fake_client_driver_t *) driver;
    fake->reap_calls++;
    if ( fake->auto_complete_reaps > 0 && --fake->auto_complete_reaps == 0 )
        rocketnet_fake_client_complete( fake, fake->num_pending, true );
    int got = fake->num_done < max ? fake->num_done : max;
    for ( int i = 0; i < got; i++ )
    {
        tags[i] = fake->done_tags[i];
        ok[i] = fake->done_ok[i];
    }
    for ( int i = got; i < fake->num_done; i++ )
    {
        fake->done_tags[i - got] = fake->done_tags[i];
        fake->done_ok[i - got] = fake->done_ok[i];
    }
    fake->num_done -= got;
    return got;
}

// The four tables: base (four required), io (four plus wait and wake), batch
// (the first eight), overlapped (io plus post_send and reap_sends; send_failures
// NULL), each in
// rocketnet_client_driver_operations_t member order.

static const rocketnet_client_driver_operations_t rocketnet_fake_client_operations_base =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static const rocketnet_client_driver_operations_t rocketnet_fake_client_operations_io =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, rocketnet_fake_client_wait, rocketnet_fake_client_wake, NULL, NULL, NULL
};

static const rocketnet_client_driver_operations_t rocketnet_fake_client_operations_batch =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    rocketnet_fake_client_send_batch, rocketnet_fake_client_receive_batch,
    rocketnet_fake_client_wait, rocketnet_fake_client_wake, NULL, NULL, NULL
};

static const rocketnet_client_driver_operations_t rocketnet_fake_client_operations_overlapped =
{
    rocketnet_fake_client_create, rocketnet_fake_client_destroy,
    rocketnet_fake_client_send, rocketnet_fake_client_receive,
    NULL, NULL, rocketnet_fake_client_wait, rocketnet_fake_client_wake,
    NULL, rocketnet_fake_client_post_send, rocketnet_fake_client_reap_sends
};

// Push an inbound datagram (docs/CLIENT-IO.md "The I/O step"). Appends past
// ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS assert.

inline void rocketnet_fake_client_push( rocketnet_fake_client_driver_t * fake, const rocketnet_address_t * from, const uint8_t * data, int bytes )
{
    assert( fake->inbound_count < ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS );
    rocketnet_fake_client_datagram_t * dg = &fake->inbound[( fake->inbound_head + fake->inbound_count ) % ROCKETNET_FAKE_CLIENT_MAX_DATAGRAMS];
    dg->address = *from;
    dg->bytes = bytes;
    memcpy( dg->data, data, (size_t) bytes );
    fake->inbound_count++;
}

#endif // #ifndef ROCKETNET_FAKE_CLIENT_DRIVER_H
