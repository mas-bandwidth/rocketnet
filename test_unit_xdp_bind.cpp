/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP bind mode, socket and umem configs and the initial fill
// (sdk/rocketnet_xdp_bind.h): copy by default, zero-copy only for
// ROCKETNET_XDP_ZEROCOPY=1, every umem size explicit, the fill ring populated
// before bind. Pure logic over fake structs and fake ring callbacks: no socket,
// no environment read (the value is an argument), no clock.
//
// The helpers alone would let a call site drift back (assign the old flags by
// hand, or move the fill after the bind) with every test green, so the last
// test reads sdk/rocketnet_server_socket_linux.cpp, as the counter audit
// reads its sources (ROCKETNET_SOURCE_DIR from CMakeLists.txt), and holds the
// create path to the helpers and to their order.

#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "test_unit_registry.h"

#include "sdk/rocketnet_constants.h"
#include "sdk/rocketnet_xdp_bind.h"

#ifndef ROCKETNET_SOURCE_DIR
#error "the build passes ROCKETNET_SOURCE_DIR to this file"
#endif

// the fields of libxdp's xsk_socket_config and xsk_umem_config the helpers set
struct fake_xsk_socket_config_t { uint32_t rx_size; uint32_t tx_size; uint32_t libbpf_flags; uint32_t xdp_flags; uint16_t bind_flags; };
struct fake_xsk_umem_config_t { uint32_t fill_size; uint32_t comp_size; uint32_t frame_size; uint32_t frame_headroom; uint32_t flags; };

static const uint32_t fake_inhibit_prog_load = 1;

static void xdp_bind_defaults_to_copy_mode()
{
    const uint16_t flags = rocketnet_xdp_bind_flags( NULL );
    rocketnet_unit_check( flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_COPY ) );
    rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_ZEROCOPY ) == 0 );
    rocketnet_unit_check( !rocketnet_xdp_want_zerocopy( NULL ) );
}

static void xdp_bind_zerocopy_only_for_exactly_one()
{
    const uint16_t flags = rocketnet_xdp_bind_flags( "1" );
    rocketnet_unit_check( flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_ZEROCOPY ) );
    rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_COPY ) == 0 );

    const char * not_one[] = { "", "0", "2", "01", "1 ", " 1", "yes", "true", "on", "zerocopy" };
    for ( size_t i = 0; i < sizeof( not_one ) / sizeof( not_one[0] ); i++ )
    {
        rocketnet_unit_check( !rocketnet_xdp_want_zerocopy( not_one[i] ) );
        rocketnet_unit_check( rocketnet_xdp_bind_flags( not_one[i] ) == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_COPY ) );
    }
}

static void xdp_bind_always_names_exactly_one_mode()
{
    const char * values[] = { NULL, "", "0", "1", "yes" };
    for ( size_t i = 0; i < sizeof( values ) / sizeof( values[0] ); i++ )
    {
        const uint16_t flags = rocketnet_xdp_bind_flags( values[i] );
        const bool copy = ( flags & ROCKETNET_XDP_BIND_COPY ) != 0;
        const bool zerocopy = ( flags & ROCKETNET_XDP_BIND_ZEROCOPY ) != 0;
        rocketnet_unit_check( copy != zerocopy );
        rocketnet_unit_check( ( flags & ROCKETNET_XDP_BIND_USE_NEED_WAKEUP ) != 0 );
    }
}

static void xdp_socket_config_puts_the_mode_in_bind_flags()
{
    fake_xsk_socket_config_t config;
    memset( &config, 0xAB, sizeof(config) );
    rocketnet_xdp_socket_config_init( &config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, fake_inhibit_prog_load, NULL );
    rocketnet_unit_check( config.rx_size == ROCKETNET_XDP_RECV_QUEUE_SIZE );
    rocketnet_unit_check( config.tx_size == ROCKETNET_XDP_SEND_QUEUE_SIZE );
    rocketnet_unit_check( config.xdp_flags == 0 );   // the old code put XDP_ZEROCOPY here, where libxdp never reads it
    rocketnet_unit_check( config.bind_flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_COPY ) );
    rocketnet_unit_check( config.libbpf_flags == fake_inhibit_prog_load );

    rocketnet_xdp_socket_config_init( &config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, fake_inhibit_prog_load, "1" );
    rocketnet_unit_check( config.xdp_flags == 0 );
    rocketnet_unit_check( config.bind_flags == ( ROCKETNET_XDP_BIND_USE_NEED_WAKEUP | ROCKETNET_XDP_BIND_ZEROCOPY ) );
}

static void xdp_umem_config_sets_every_ring_size()
{
    fake_xsk_umem_config_t config;
    memset( &config, 0xAB, sizeof(config) );
    rocketnet_xdp_umem_config_init( &config, ROCKETNET_XDP_FILL_QUEUE_SIZE, 2048, ROCKETNET_XDP_FRAME_SIZE );
    // libxdp < 1.5 hands these to the kernel as given; 0 is EINVAL
    rocketnet_unit_check( config.fill_size == 2048 );
    rocketnet_unit_check( config.comp_size == 2048 );
    rocketnet_unit_check( config.frame_size == 4096 );
    rocketnet_unit_check( config.frame_headroom == 0 );
    rocketnet_unit_check( config.flags == 0 );
}

// A fake fill ring and a fake bind: the bind records how many frames the
// fill ring held when it ran, which is what a zero-copy driver sees.
struct fake_fill_ring_t
{
    uint64_t slots[ROCKETNET_XDP_FILL_QUEUE_SIZE];
    uint32_t reserved = 0;
    uint32_t submitted = 0;
    int submits = 0;
};

static void xdp_prefill_puts_2048_frames_in_fill_before_bind()
{
    static fake_fill_ring_t ring;
    ring = fake_fill_ring_t();
    uint64_t next_frame = (uint64_t) ROCKETNET_XDP_NUM_FRAMES / 2 * ROCKETNET_XDP_FRAME_SIZE;
    const bool ok = rocketnet_xdp_prefill( ROCKETNET_XDP_FILL_QUEUE_SIZE, UINT64_MAX,
        [&]( uint32_t count, uint32_t * index ) { if ( count > ROCKETNET_XDP_FILL_QUEUE_SIZE ) return 0u; ring.reserved = count; *index = 0; return count; },
        [&]() { const uint64_t frame = next_frame; next_frame += ROCKETNET_XDP_FRAME_SIZE; return frame; },
        [&]( uint32_t index, uint64_t frame ) { rocketnet_unit_check( index < ring.reserved ); ring.slots[index] = frame; },
        [&]( uint32_t count ) { ring.submitted += count; ring.submits++; } );
    rocketnet_unit_check( ok );

    // the bind: what the driver finds on the fill ring
    const uint32_t at_bind = ring.submitted;
    rocketnet_unit_check( at_bind == 2048 );
    rocketnet_unit_check( ring.submits == 1 );
    for ( uint32_t i = 0; i < at_bind; i++ )
    {
        rocketnet_unit_check( ring.slots[i] == (uint64_t) ( ROCKETNET_XDP_NUM_FRAMES / 2 + i ) * ROCKETNET_XDP_FRAME_SIZE );
        rocketnet_unit_check( i == 0 || ring.slots[i] != ring.slots[i - 1] );
    }
}

static void xdp_prefill_submits_nothing_when_it_cannot_fill()
{
    int submits = 0;
    // the reservation is refused
    rocketnet_unit_check( !rocketnet_xdp_prefill( 2048, UINT64_MAX,
        []( uint32_t, uint32_t * ) { return 0u; },
        []() { return (uint64_t) 0; },
        []( uint32_t, uint64_t ) { rocketnet_unit_check( false ); },
        [&]( uint32_t ) { submits++; } ) );
    // the allocator runs dry half way
    int allocated = 0;
    rocketnet_unit_check( !rocketnet_xdp_prefill( 2048, UINT64_MAX,
        []( uint32_t count, uint32_t * index ) { *index = 0; return count; },
        [&]() { return allocated++ < 1024 ? (uint64_t) allocated * 4096 : UINT64_MAX; },
        []( uint32_t, uint64_t ) {},
        [&]( uint32_t ) { submits++; } ) );
    rocketnet_unit_check( submits == 0 );
}

static std::string read_source( const char * relative_path )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = fopen( path.c_str(), "rb" );
    rocketnet_unit_check( file != NULL );
    std::string text;
    char buffer[4096];
    size_t n;
    while ( ( n = fread( buffer, 1, sizeof(buffer), file ) ) > 0 )
        text.append( buffer, n );
    fclose( file );
    rocketnet_unit_check( !text.empty() );
    return text;
}

static size_t count_of( const std::string & text, const char * needle )
{
    size_t count = 0;
    for ( size_t at = text.find( needle ); at != std::string::npos; at = text.find( needle, at + 1 ) )
        count++;
    return count;
}

static void xdp_create_path_uses_the_helpers_in_order()
{
    const std::string source = read_source( "sdk/rocketnet_server_socket_linux.cpp" );

    // one umem create, one prefill, one socket create, each through its helper
    rocketnet_unit_check( count_of( source, "xsk_umem__create(" ) == 1 );
    rocketnet_unit_check( count_of( source, "rocketnet_xdp_umem_config_init( &umem_config," ) == 1 );
    rocketnet_unit_check( count_of( source, "rocketnet_xdp_prefill(" ) == 1 );
    rocketnet_unit_check( count_of( source, "rocketnet_xdp_socket_config_init( &xsk_config," ) == 1 );
    rocketnet_unit_check( count_of( source, "xsk_socket__create(" ) == 1 );

    // nothing assigns the configs by hand around the helpers
    rocketnet_unit_check( count_of( source, "xsk_config." ) == 0 );
    rocketnet_unit_check( count_of( source, "umem_config." ) == 0 );
    rocketnet_unit_check( count_of( source, "XDP_ZEROCOPY;" ) == 0 );

    // the order: umem config, umem create, prefill (fill ring populated),
    // socket config, socket create (the bind)
    const size_t umem_config = source.find( "rocketnet_xdp_umem_config_init( &umem_config," );
    const size_t umem_create = source.find( "xsk_umem__create(" );
    const size_t prefill = source.find( "rocketnet_xdp_prefill(" );
    const size_t socket_config = source.find( "rocketnet_xdp_socket_config_init( &xsk_config," );
    const size_t bind = source.find( "xsk_socket__create(" );
    rocketnet_unit_check( umem_config < umem_create );
    rocketnet_unit_check( umem_create < prefill );
    rocketnet_unit_check( prefill < socket_config );
    rocketnet_unit_check( socket_config < bind );

    // the create path submits to the fill ring once, inside the prefill and
    // before the socket config; the only other producer is the worker's
    // pending return, which runs after the bind by design
    const char * create_submit = "xsk_ring_prod__submit( &socket->fill_queue";
    const char * worker_submit = "xsk_ring_prod__submit(&socket->fill_queue";
    rocketnet_unit_check( count_of( source, create_submit ) == 1 );
    rocketnet_unit_check( source.find( create_submit ) > prefill && source.find( create_submit ) < socket_config );
    rocketnet_unit_check( count_of( source, worker_submit ) == 1 );
    rocketnet_unit_check( source.find( worker_submit ) > bind );
}

REGISTER_UNIT_TEST( xdp_bind_defaults_to_copy_mode );
REGISTER_UNIT_TEST( xdp_bind_zerocopy_only_for_exactly_one );
REGISTER_UNIT_TEST( xdp_bind_always_names_exactly_one_mode );
REGISTER_UNIT_TEST( xdp_socket_config_puts_the_mode_in_bind_flags );
REGISTER_UNIT_TEST( xdp_umem_config_sets_every_ring_size );
REGISTER_UNIT_TEST( xdp_prefill_puts_2048_frames_in_fill_before_bind );
REGISTER_UNIT_TEST( xdp_prefill_submits_nothing_when_it_cannot_fill );
REGISTER_UNIT_TEST( xdp_create_path_uses_the_helpers_in_order );
