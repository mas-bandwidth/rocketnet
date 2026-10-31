/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The family fallback of the client ladder (proofs/tla/RNClientLadder.tla).
// A token naming 203.0.113.10:40000 and [2001:db8::1]:40000 on a host with
// IPv6 (the unit suite's network lookup answers true) selects the IPv6
// server. When a candidate's IPv6 socket is refused, at open or at connect,
// the same candidate is opened again for the IPv4 server, bound IPv4,
// before the ladder falls to the next candidate; the client then connects
// over IPv4 and races nothing. A refused IPv4 socket with a granted IPv6 one
// races nothing and connects over IPv6. Both refused on every candidate
// exhausts the ladder, and a refusal for want of memory on either family
// ends the create with OUT_OF_MEMORY. The transport runs step-driven on the
// fake client driver with per-family refusal: no socket, no thread, and the
// clock is an argument.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_client_io.h"
#include "rocketnet_driver.h"
#include "rocketnet_constants.h"
#include "rocketnet_test_vectors.h"
#include "fake_client_driver.h"
#include "unit_shutdown.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

static const double family_fallback_start = 1000.0;

static double family_fallback_time( void * context )
{
    return *(double *) context;
}

// a token naming the families in address_flags: 203.0.113.10:40000 and
// [2001:db8::1]:40000

static void family_fallback_mint( uint8_t * token_data, uint8_t address_flags )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 5;
    signed_fields.address_flags = address_flags;

    const uint8_t ipv4_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, ipv4_address, 6 );

    // eight little-endian groups and the port, as the token carries them

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

    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

struct family_fallback_case_t
{
    rocketnet_fake_client_transport_io_t * fake;
    rocketnet_client_transport_t * transport;
    int status;
    double now;
};

// creates a client on the fake with the case's refusals already set on
// fake; transport (pinned) is NULL for the default ladder

static void family_fallback_create( family_fallback_case_t * c, uint8_t address_flags, const char * transport )
{
    static rocketnet_client_transport_io_operations_t operations;
    operations = rocketnet_fake_client_operations_io;
    operations.create = rocketnet_fake_client_create_by_family;
    operations.connect = rocketnet_fake_client_connect;
    operations.close = rocketnet_fake_client_close;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    family_fallback_mint( token, address_flags );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    config.time_callback = family_fallback_time;
    memcpy( config.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    if ( transport )
        snprintf( config.transport, sizeof( config.transport ), "%s", transport );

    c->now = family_fallback_start;
    c->status = ROCKETNET_STATUS_OK;
    c->transport = rocketnet_client_transport_create_step_driven_at_timestamp_ex(
        &c->now, token, &config, c->now, ROCKETNET_TEST_CREATE_TS, &operations, c->fake, &c->status );
}

// runs the connection for a few updates and checks every datagram the
// client sent went to server

static void family_fallback_check_sends_only_to( family_fallback_case_t * c, const rocketnet_address_t * server )
{
    rocketnet_client_io_t * io = rocketnet_client_transport_client_io( c->transport );
    rocketnet_unit_check( io != NULL );

    for ( int i = 0; i < 10; i++, c->now += 0.1 )
    {
        rocketnet_client_io_step( io, false );
        rocketnet_client_transport_update( c->transport, c->now );
        rocketnet_client_io_step( io, false );
    }

    rocketnet_unit_check( c->fake->num_sent > 0 );
    for ( int i = 0; i < c->fake->num_sent; i++ )
        rocketnet_unit_check( rocketnet_address_equal( &c->fake->sent[i].address, server ) );
}

static void family_fallback_servers( rocketnet_address_t * ipv4_server, rocketnet_address_t * ipv6_server )
{
    rocketnet_unit_check( rocketnet_address_parse( ipv4_server, "203.0.113.10:40000" ) );
    rocketnet_unit_check( rocketnet_address_parse( ipv6_server, "[2001:db8::1]:40000" ) );
}

static const uint8_t family_fallback_both = ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6;

// IPv6 connect refused, IPv4 granted, on one candidate: the client connects
// over IPv4 on that candidate, its IPv4 socket bound IPv4, and races nothing

static void family_fallback_expect_ipv4_after_ipv6_connect_refused( const char * transport )
{
    rocketnet_address_t ipv4_server, ipv6_server;
    family_fallback_servers( &ipv4_server, &ipv6_server );

    family_fallback_case_t c = {};
    c.fake = new rocketnet_fake_client_transport_io_t();
    c.fake->refuse_connect_ipv6 = true;

    family_fallback_create( &c, family_fallback_both, transport );
    rocketnet_unit_check( c.transport != NULL );
    rocketnet_unit_check( c.status == ROCKETNET_STATUS_OK );

    // the IPv6 socket was opened, refused at connect and closed; the IPv4
    // socket was opened bound IPv4 and connected to the IPv4 server; no
    // second socket was opened for a race

    rocketnet_unit_check( c.fake->create_calls == 2 );
    rocketnet_unit_check( c.fake->connect_calls == 2 );
    rocketnet_unit_check( c.fake->last_bind_family == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( rocketnet_address_equal( &c.fake->connected_to, &ipv4_server ) );
    rocketnet_unit_check( c.fake->destroy_calls == 1 );
    if ( transport )
        rocketnet_unit_check( strcmp( rocketnet_client_transport_io_name( c.transport ), transport ) == 0 );

    family_fallback_check_sends_only_to( &c, &ipv4_server );

    rn_test_client_transport_stop_and_destroy( c.transport );
    rocketnet_unit_check( c.fake->close_calls == 0 );
    rocketnet_unit_check( c.fake->destroy_calls == 2 );
    delete c.fake;
}

static void test_client_family_fallback_ipv6_connect_refused_auto()
{
    family_fallback_expect_ipv4_after_ipv6_connect_refused( NULL );
}

// every transport this platform offers the client, pinned alone: the pin
// keeps its one candidate and falls to the IPv4 server on it

static void test_client_family_fallback_ipv6_connect_refused_each_pin()
{
    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    rocketnet_unit_check( num_available > 0 );

    for ( int i = 0; i < num_available; i++ )
        family_fallback_expect_ipv4_after_ipv6_connect_refused( rocketnet_transport_name( available[i] ) );
}

// the IPv6 socket refused at open (a host that makes IPv6 sockets and
// refuses the bind): the IPv4 server on the same candidate

static void test_client_family_fallback_ipv6_open_refused()
{
    rocketnet_address_t ipv4_server, ipv6_server;
    family_fallback_servers( &ipv4_server, &ipv6_server );

    family_fallback_case_t c = {};
    c.fake = new rocketnet_fake_client_transport_io_t();
    c.fake->refuse_open_ipv6 = true;

    family_fallback_create( &c, family_fallback_both, NULL );
    rocketnet_unit_check( c.transport != NULL );
    rocketnet_unit_check( c.status == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( c.fake->create_calls == 2 );
    rocketnet_unit_check( c.fake->connect_calls == 1 );
    rocketnet_unit_check( c.fake->last_bind_family == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( rocketnet_address_equal( &c.fake->connected_to, &ipv4_server ) );

    family_fallback_check_sends_only_to( &c, &ipv4_server );

    rn_test_client_transport_stop_and_destroy( c.transport );
    rocketnet_unit_check( c.fake->destroy_calls == 1 );
    delete c.fake;
}

// the mirror: IPv6 granted, the race's IPv4 socket refused at connect. The
// client races nothing and connects over IPv6

static void test_client_family_fallback_ipv4_refused_keeps_ipv6()
{
    rocketnet_address_t ipv4_server, ipv6_server;
    family_fallback_servers( &ipv4_server, &ipv6_server );

    family_fallback_case_t c = {};
    c.fake = new rocketnet_fake_client_transport_io_t();
    c.fake->refuse_connect_ipv4 = true;

    family_fallback_create( &c, family_fallback_both, NULL );
    rocketnet_unit_check( c.transport != NULL );
    rocketnet_unit_check( c.status == ROCKETNET_STATUS_OK );

    // the IPv6 socket granted first; the race's IPv4 socket opened, refused
    // at connect and closed

    rocketnet_unit_check( c.fake->create_calls == 2 );
    rocketnet_unit_check( c.fake->connect_calls == 2 );
    rocketnet_unit_check( rocketnet_address_equal( &c.fake->connected_to, &ipv4_server ) );
    rocketnet_unit_check( c.fake->destroy_calls == 1 );

    family_fallback_check_sends_only_to( &c, &ipv6_server );

    rn_test_client_transport_stop_and_destroy( c.transport );
    rocketnet_unit_check( c.fake->destroy_calls == 2 );
    delete c.fake;
}

// both families refused on every candidate: the ladder exhausts, every
// opened socket is closed and nothing is sent. A token naming IPv6 alone
// never dials an IPv4 server.

static void test_client_family_fallback_both_refused_exhausts()
{
    {
        family_fallback_case_t c = {};
        c.fake = new rocketnet_fake_client_transport_io_t();
        c.fake->refuse_connect_ipv6 = true;
        c.fake->refuse_connect_ipv4 = true;

        family_fallback_create( &c, family_fallback_both, NULL );
        rocketnet_unit_check( c.transport == NULL );
        rocketnet_unit_check( c.status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        rocketnet_unit_check( c.fake->create_calls >= 2 && c.fake->create_calls % 2 == 0 );
        rocketnet_unit_check( c.fake->connect_calls == c.fake->create_calls );
        rocketnet_unit_check( c.fake->destroy_calls == c.fake->create_calls );
        rocketnet_unit_check( c.fake->num_sent == 0 );
        delete c.fake;
    }

    {
        family_fallback_case_t c = {};
        c.fake = new rocketnet_fake_client_transport_io_t();
        c.fake->refuse_open_ipv6 = true;
        c.fake->refuse_open_ipv4 = true;

        family_fallback_create( &c, family_fallback_both, NULL );
        rocketnet_unit_check( c.transport == NULL );
        rocketnet_unit_check( c.status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        rocketnet_unit_check( c.fake->connect_calls == 0 );
        rocketnet_unit_check( c.fake->destroy_calls == 0 );
        delete c.fake;
    }

    {
        rocketnet_address_t ipv4_server, ipv6_server;
        family_fallback_servers( &ipv4_server, &ipv6_server );

        family_fallback_case_t c = {};
        c.fake = new rocketnet_fake_client_transport_io_t();
        c.fake->refuse_connect_ipv6 = true;

        family_fallback_create( &c, ROCKETNET_ADDRESS_FLAG_IPV6, NULL );
        rocketnet_unit_check( c.transport == NULL );
        rocketnet_unit_check( c.status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        rocketnet_unit_check( c.fake->last_bind_family == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_unit_check( rocketnet_address_equal( &c.fake->connected_to, &ipv6_server ) );
        rocketnet_unit_check( c.fake->connect_calls == c.fake->create_calls );
        delete c.fake;
    }
}

// a refusal for want of memory is fatal on either family: the IPv6 open
// failing for memory never dials IPv4, and the IPv4 fallback failing for
// memory never falls to the next candidate

static void test_client_family_fallback_out_of_memory_is_fatal()
{
    {
        family_fallback_case_t c = {};
        c.fake = new rocketnet_fake_client_transport_io_t();
        c.fake->refuse_open_ipv6 = true;
        c.fake->open_out_of_memory = true;

        family_fallback_create( &c, family_fallback_both, NULL );
        rocketnet_unit_check( c.transport == NULL );
        rocketnet_unit_check( c.status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_unit_check( c.fake->create_calls == 1 );
        rocketnet_unit_check( c.fake->last_bind_family == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_unit_check( c.fake->connect_calls == 0 );
        delete c.fake;
    }

    {
        family_fallback_case_t c = {};
        c.fake = new rocketnet_fake_client_transport_io_t();
        c.fake->refuse_connect_ipv6 = true;
        c.fake->refuse_open_ipv4 = true;
        c.fake->open_out_of_memory = true;

        family_fallback_create( &c, family_fallback_both, NULL );
        rocketnet_unit_check( c.transport == NULL );
        rocketnet_unit_check( c.status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_unit_check( c.fake->create_calls == 2 );
        rocketnet_unit_check( c.fake->last_bind_family == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_unit_check( c.fake->connect_calls == 1 );
        rocketnet_unit_check( c.fake->destroy_calls == 1 );
        delete c.fake;
    }
}

REGISTER_UNIT_TEST( test_client_family_fallback_ipv6_connect_refused_auto );
REGISTER_UNIT_TEST( test_client_family_fallback_ipv6_connect_refused_each_pin );
REGISTER_UNIT_TEST( test_client_family_fallback_ipv6_open_refused );
REGISTER_UNIT_TEST( test_client_family_fallback_ipv4_refused_keeps_ipv6 );
REGISTER_UNIT_TEST( test_client_family_fallback_both_refused_exhausts );
REGISTER_UNIT_TEST( test_client_family_fallback_out_of_memory_is_fatal );
#endif // #if ROCKETNET_DEV
