/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client's two-family race (sdk/rocketnet_client_transport.cpp): its
// batch send while both families are in flight and after the commit, its
// batched receive on a fault and on a full batch, and its wait when the
// caller is not reading. One fake socket per family (tests/fake_client_driver.h),
// fake[0] the token's IPv6 server (the primary) and fake[1] its IPv4 server,
// as tests/test_unit_gaps_ct_memory_rings.cpp sets them up: no socket, no
// thread; time is an argument starting at 1000.0. Each failing check prints
// the fault first.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "fake_client_driver.h"

#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_address.h"
#include "rocketnet_client_io.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#define check rocketnet_unit_check

extern bool rocketnet_client_transport_commit_race_for_test( rocketnet_client_transport_t *, int );

// ---------------------------------------------------------------------------
// the race world

struct r2_race_t
{
    rocketnet_fake_client_transport_io_t fake[2];
    int creates;
    int receive_batch_fault[2];
    rocketnet_address_t ipv4_server, ipv6_server;
    rocketnet_client_transport_io_operations_t operations;
    double now;
    rocketnet_client_transport_t * transport;
    const rocketnet_client_transport_io_t * io;
};

static r2_race_t * r2_race;

static void * r2_race_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    (void) config;
    (void) bind;
    r2_race_t * r = (r2_race_t *) context;
    check( r->creates < 2 );
    return r->creates < 2 ? &r->fake[r->creates++] : NULL;
}

// a receive_batch that reports a fault for a family whose fault is set
static int r2_race_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    const int family = instance == &r2_race->fake[1] ? 1 : 0;
    if ( r2_race->receive_batch_fault[family] )
    {
        r2_race->fake[family].receive_batch_calls++;
        return -1;
    }
    return rocketnet_fake_client_receive_batch( instance, from, packet_data, packet_stride, packet_bytes, max_packets );
}

static double r2_race_time( void * context )
{
    return ( (r2_race_t *) context )->now;
}

static void r2_race_mint_both( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 5;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );

    const uint8_t ipv6_address[18] = { 0x01, 0x20, 0xb8, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x40, 0x9c };
    memcpy( signed_fields.ipv6_address, ipv6_address, 18 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// families with send_batch, receive_batch, wait and wake, and no
// send_batch_progress: the race's send_batch is the whole batch path
static r2_race_t * r2_race_setup()
{
    r2_race_t * r = new r2_race_t();
    memset( (void *) r, 0, sizeof( *r ) );
    r2_race = r;

    check( rocketnet_address_parse( &r->ipv4_server, "203.0.113.10:40000" ) );
    check( rocketnet_address_parse( &r->ipv6_server, "[2001:db8::1]:40000" ) );

    r->operations = rocketnet_fake_client_operations_batch;
    r->operations.create = r2_race_create;
    r->operations.connect = rocketnet_fake_client_connect;
    r->operations.close = rocketnet_fake_client_close;
    r->operations.receive_batch = r2_race_receive_batch;
    r->operations.send_batch_progress = NULL;

    r->now = 1000.0;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    r2_race_mint_both( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = r2_race_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );

    r->transport = rocketnet_client_transport_create_step_driven_at_timestamp( r, token, &config, r->now, ROCKETNET_TEST_CREATE_TS, &r->operations, r );
    check( r->transport != NULL );
    check( r->creates == 2 );
    rocketnet_client_io_t * client_io = rocketnet_client_transport_client_io( r->transport );
    check( client_io != NULL );
    r->io = client_io->transport;
    check( r->io->operations->send_batch != NULL && r->io->operations->send_batch_progress == NULL );
    return r;
}

static void r2_race_teardown( r2_race_t * r )
{
    rn_test_client_transport_stop_and_destroy( r->transport );
    r2_race = NULL;
    delete r;
}

static const char * r2_family_name( const rocketnet_address_t * address )
{
    return address->type == ROCKETNET_ADDRESS_IPV6 ? "IPv6" : "IPv4";
}

// the datagram the fake recorded at index: its one data byte and its destination
static bool r2_sent_is( const rocketnet_fake_client_transport_io_t * fake, int index, uint8_t data, const rocketnet_address_t * to )
{
    return index < fake->num_sent && fake->sent[index].bytes == 1 && fake->sent[index].data[0] == data &&
           rocketnet_address_equal( &fake->sent[index].address, to );
}

static void r2_print_sent( const char * id, const char * which, const rocketnet_fake_client_transport_io_t * fake, int from )
{
    for ( int i = from; i < fake->num_sent; i++ )
    {
        printf( "mutation gap %s: %s socket sent byte %d to the %s server\n", id, which, fake->sent[i].data[0], r2_family_name( &fake->sent[i].address ) );
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6607, 2/6608, 2/6609, 2/6610, 2/6611, 2/6612, 2/6616, 2/6618
// (the in-flight batch send: the batch on family 0, a family-1 copy of each packet to the primary)
//
// While both families are in flight a batch goes whole through the primary
// (IPv6) family's send_batch, and every packet to the primary server is
// copied once through the IPv4 family's socket, to the IPv4 server. The
// batch is three packets (bytes 1, 2, 3: to IPv6, to IPv4, to IPv6); a
// fourth entry (byte 4, to IPv6) lies just past it in the same array, so a
// loop that runs one past the batch sends a copy of byte 4.

static void test_gaps_r2_ct_race_in_flight_batch_goes_to_family_0_with_family_1_copies()
{
    r2_race_t * r = r2_race_setup();

    const int sent0 = r->fake[0].num_sent;
    const int sent1 = r->fake[1].num_sent;
    const int batches0 = r->fake[0].send_batch_calls;
    const int batches1 = r->fake[1].send_batch_calls;

    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_platform_send_t sends[4] = {
        { &r->ipv6_server, &data[0], 1 },
        { &r->ipv4_server, &data[1], 1 },
        { &r->ipv6_server, &data[2], 1 },
        { &r->ipv6_server, &data[3], 1 },
    };

    r->io->operations->send_batch( r->io->instance, sends, 3 );

    // family 0: one send_batch call carrying the three packets, as they were addressed

    const bool batch_on_family_0 = r->fake[0].send_batch_calls == batches0 + 1 && r->fake[0].num_sent == sent0 + 3 &&
                                   r2_sent_is( &r->fake[0], sent0 + 0, 1, &r->ipv6_server ) &&
                                   r2_sent_is( &r->fake[0], sent0 + 1, 2, &r->ipv4_server ) &&
                                   r2_sent_is( &r->fake[0], sent0 + 2, 3, &r->ipv6_server );
    if ( !batch_on_family_0 )
    {
        printf( "mutation gap hunt 2/6607,6608: the in-flight batch of 3 was not sent whole through the IPv6 family's send_batch (IPv6 socket: %d batch call(s), %d datagram(s)):\n",
                r->fake[0].send_batch_calls - batches0, r->fake[0].num_sent - sent0 );
        r2_print_sent( "hunt 2/6607,6608", "IPv6", &r->fake[0], sent0 );
    }
    check( batch_on_family_0 );

    // family 1: no batch, and exactly one copy of each packet to the primary (bytes 1 and 3), to the IPv4 server

    const bool copies_on_family_1 = r->fake[1].send_batch_calls == batches1 && r->fake[1].num_sent == sent1 + 2 &&
                                    r2_sent_is( &r->fake[1], sent1 + 0, 1, &r->ipv4_server ) &&
                                    r2_sent_is( &r->fake[1], sent1 + 1, 3, &r->ipv4_server );
    if ( !copies_on_family_1 )
    {
        printf( "mutation gap hunt 2/6607,6609,6610,6611,6612,6616,6618: the IPv4 socket did not send exactly the copies of bytes 1 and 3 to the IPv4 server (%d batch call(s), %d datagram(s)):\n",
                r->fake[1].send_batch_calls - batches1, r->fake[1].num_sent - sent1 );
        r2_print_sent( "hunt 2/6607,6609,6610,6611,6612,6616,6618", "IPv4", &r->fake[1], sent1 );
    }
    check( copies_on_family_1 );

    r2_race_teardown( r );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_race_in_flight_batch_goes_to_family_0_with_family_1_copies );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6601, 2/6602, 2/6603, 2/6604, 2/6605
// (the committed race's mixed batch: only the kept family's packets leave)
//
// After the commit to the IPv6 family a batch handed off before it can still
// hold a packet for the losing IPv4 server. Such a mixed batch sends the
// kept packets one by one through the kept socket and drops the rest. The
// batch is bytes 1 (IPv6), 2 (IPv4), 3 (IPv6), with byte 4 (IPv6) just past
// it in the array.

static void test_gaps_r2_ct_race_committed_mixed_batch_sends_only_the_kept_packets()
{
    r2_race_t * r = r2_race_setup();

    check( rocketnet_client_transport_commit_race_for_test( r->transport, 0 ) );

    const int sent0 = r->fake[0].num_sent;
    const int sent1 = r->fake[1].num_sent;

    const uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_platform_send_t sends[4] = {
        { &r->ipv6_server, &data[0], 1 },
        { &r->ipv4_server, &data[1], 1 },
        { &r->ipv6_server, &data[2], 1 },
        { &r->ipv6_server, &data[3], 1 },
    };

    r->io->operations->send_batch( r->io->instance, sends, 3 );

    const bool kept_sent = r->fake[0].num_sent == sent0 + 2 &&
                           r2_sent_is( &r->fake[0], sent0 + 0, 1, &r->ipv6_server ) &&
                           r2_sent_is( &r->fake[0], sent0 + 1, 3, &r->ipv6_server );
    if ( !kept_sent )
    {
        printf( "mutation gap hunt 2/6601,6602,6603,6604,6605: a mixed batch after the commit to IPv6 sent %d datagram(s) on the kept socket, not bytes 1 and 3 (the kept packets of the batch, and nothing past it):\n",
                r->fake[0].num_sent - sent0 );
        r2_print_sent( "hunt 2/6601,6602,6603,6604,6605", "IPv6", &r->fake[0], sent0 );
    }
    check( kept_sent );

    if ( r->fake[1].num_sent != sent1 )
    {
        printf( "mutation gap hunt 2/6601: the losing IPv4 socket sent %d datagram(s) after the commit\n", r->fake[1].num_sent - sent1 );
    }
    check( r->fake[1].num_sent == sent1 );

    r2_race_teardown( r );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_race_committed_mixed_batch_sends_only_the_kept_packets );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6644 (the in-flight batched receive goes on to family 1 after a fault or a full batch)
//
// While both families are in flight the batched receive reads the primary
// family first, and the second only when the first returned neither a fault
// nor a full batch. A fault from the first is returned as it is (the caller
// counts it); asking the second family to fill from[-1] onwards would write
// before the caller's arrays and turn the fault into "nothing received". A
// full first batch leaves the second family unread (there is no room).

static void test_gaps_r2_ct_race_batched_receive_stops_at_a_fault_or_a_full_batch()
{
    r2_race_t * r = r2_race_setup();

    // the second family holds one datagram throughout

    rocketnet_fake_client_datagram_t * waiting = &r->fake[1].inbound[0];
    waiting->address = r->ipv4_server;
    waiting->bytes = 1;
    waiting->data[0] = 9;
    r->fake[1].inbound_count = 1;

    // a fault from the primary family; the arrays start one entry in, so a write before them stays in the test's memory

    rocketnet_address_t from[4];
    int packet_bytes[4];
    uint8_t packet_data[4 * 8];
    memset( from, 0, sizeof( from ) );
    memset( packet_bytes, 0, sizeof( packet_bytes ) );
    memset( packet_data, 0, sizeof( packet_data ) );

    r->receive_batch_fault[0] = 1;
    const int calls1 = r->fake[1].receive_batch_calls;
    const int faulted = r->io->operations->receive_batch( r->io->instance, &from[1], &packet_data[8], 8, &packet_bytes[1], 2 );
    if ( faulted != -1 || r->fake[1].inbound_count != 1 || r->fake[1].receive_batch_calls != calls1 )
    {
        printf( "mutation gap hunt 2/6644: the primary family's receive fault (-1) came back as %d, and the IPv4 family was read %d time(s) after it (%d datagram(s) taken, written before the caller's arrays)\n",
                faulted, r->fake[1].receive_batch_calls - calls1, 1 - r->fake[1].inbound_count );
    }
    check( faulted == -1 );
    check( r->fake[1].inbound_count == 1 );
    check( r->fake[1].receive_batch_calls == calls1 );
    r->receive_batch_fault[0] = 0;

    // a full batch from the primary family: two datagrams into room for two

    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_fake_client_datagram_t * dg = &r->fake[0].inbound[i];
        dg->address = r->ipv6_server;
        dg->bytes = 1;
        dg->data[0] = (uint8_t) ( 20 + i );
    }
    r->fake[0].inbound_head = 0;
    r->fake[0].inbound_count = 2;

    const int full = r->io->operations->receive_batch( r->io->instance, &from[1], &packet_data[8], 8, &packet_bytes[1], 2 );
    if ( full != 2 || r->fake[1].receive_batch_calls != calls1 )
    {
        printf( "mutation gap hunt 2/6644: a full batch of 2 from the IPv6 family returned %d and the IPv4 family was asked %d time(s) to receive into no room\n",
                full, r->fake[1].receive_batch_calls - calls1 );
    }
    check( full == 2 );
    check( r->fake[1].receive_batch_calls == calls1 );
    check( r->fake[1].inbound_count == 1 );

    r2_race_teardown( r );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_race_batched_receive_stops_at_a_fault_or_a_full_batch );

// ---------------------------------------------------------------------------
// mutation gap: hunt 2/6657, 2/6658 (the in-flight wait when the caller is not reading)
//
// With readable false (the receive buffer is full) the race's wait is the
// primary family's own wait, on its own socket and wake, for the caller's
// timeout, with readable false, and it returns that wait's answer: it does
// not poll for the race's short interval or report readable.

static void test_gaps_r2_ct_race_unreadable_wait_is_the_primary_familys_wait()
{
    r2_race_t * r = r2_race_setup();

    r->fake[0].wake = false;
    r->fake[1].wake = false;
    const int waits0 = r->fake[0].wait_calls;
    const int waits1 = r->fake[1].wait_calls;

    const bool woken = r->io->operations->wait( r->io->instance, false, 0.25 );

    const bool primary_waited = r->fake[0].wait_calls == waits0 + 1 && !r->fake[0].last_wait_readable && r->fake[0].last_wait_timeout == 0.25;
    if ( !primary_waited || r->fake[1].wait_calls != waits1 )
    {
        printf( "mutation gap hunt 2/6657,6658: an unreadable wait in flight waited %d time(s) on the IPv6 socket (readable %d, timeout %g) and %d time(s) on the IPv4 socket, not once on the IPv6 socket, unreadable, for 0.25 s\n",
                r->fake[0].wait_calls - waits0, (int) r->fake[0].last_wait_readable, r->fake[0].last_wait_timeout, r->fake[1].wait_calls - waits1 );
    }
    check( primary_waited );
    check( r->fake[1].wait_calls == waits1 );

    if ( woken )
    {
        printf( "mutation gap hunt 2/6658: an unreadable wait with no wake set reported woken: the I/O thread would read a full buffer\n" );
    }
    check( !woken );

    r2_race_teardown( r );
}

REGISTER_UNIT_TEST( test_gaps_r2_ct_race_unreadable_wait_is_the_primary_familys_wait );

#endif // #if ROCKETNET_DEV
