/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the XDP worker's receive pass and the ownership of its
// frames (proofs/tla/RNXdpReceiveRing.tla, proofs/tla/TraceRNXdpReceiveRing.tla,
// README-TraceCheck.md). A seeded walk drives the worker's own receive
// code (sdk/rocketnet_xdp_receive_pass.h and rocketnet_xdp_receive_ring_finish)
// over an RX ring and a fill ring it lays out itself: libxdp's own
// struct xsk_ring_cons and struct xsk_ring_prod over memory the walk owns,
// read and written through libxdp's inline ring functions, exactly as over a
// socket's mapped rings. The walk is the kernel too: it posts each packet
// into the next frame of its fill batch, as a step of its own.
//
// The UMEM is six frames: frames 0 and 1 are the send region, frames 2 to 5
// the receive region, named by their numbers 2 to 5 in the records; a
// descriptor outside the receive region is 0, the model's SendFrame. Each packet the
// kernel posts is an IPv4 UDP frame the codec takes, so the code stages one
// packet for every receive frame it reads, as the model does.
//
// The operations and the records they write:
//   kernel_receive     the kernel posts a packet in its batch's next frame   KernelReceive
//   kernel_refresh     the kernel publishes its consumer and peeks again      KernelRefresh
//   kernel_misdeliver  a defect posts a descriptor outside the receive region  KernelMisdeliver
//   peek               rocketnet_xdp_receive_peek                            Peek
//   take               rocketnet_xdp_receive_take                            Take
//   finish             rocketnet_xdp_receive_end, then rocketnet_xdp_fill_flush  Finish
//   flush              rocketnet_xdp_fill_flush at the top of a pass         Refill
//   drain              the application takes the staged packets              Drain
// A peek that returns 0 and a flush that reserves nothing take no model step
// and write no record (the model: "a peek of zero is no step", "a
// reservation of zero is this step not yet taken").
//
// The plant (ROCKETNET_XDP_RECEIVE_RING_PLANT in
// sdk/rocketnet_xdp_receive_pass.h, reached through the socket source the
// plant build includes) is the
// reversed witness MCRNXdpReceiveRingBrokenPeekPastStaging: the peek asks
// for the pending return's room alone. Its effect is a peek that returns
// more descriptors than the staging buffer has room for; the walk reports it
// at that record. The takes after it overrun the staging room, which the
// take's rocketnet_assert traps in a build with asserts: the plant runs in a
// Release build, as CI's trace check does. No socket, no kernel object, no
// clock.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_XDP_RECEIVE_RING_PLANT 1
#include "../sdk/rocketnet_server_socket_linux.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "tla_trace_record.h"

#if defined( __linux__ ) && ROCKETNET_XDP

#include "../sdk/rocketnet_pending_frames.h"
#include "../sdk/rocketnet_xdp_receive_pass.h"

namespace
{
    const uint32_t RING_FRAMES = 4;             // the receive frames: Frames
    const uint32_t RING_SEND_FRAMES = 2;        // the send region below them
    const uint32_t RING_RX_SIZE = 4;            // the RX ring's entries: RingSize
    const uint32_t RING_STAGE_CAPACITY = 4;     // the staging buffer's packets: StageCap
    const uint32_t RING_FILL_SIZE = 8;          // the fill ring's most entries: twice the frames, as the server sizes it
    const uint32_t RING_STAGE_SLOTS = 8;        // the staging arrays: room for the planted peek's overrun
    const int RING_PACKET_BYTES = 64;           // each packet's payload

    const uint64_t RING_REGION_BEGIN = (uint64_t) RING_SEND_FRAMES * ROCKETNET_XDP_FRAME_SIZE;
    const uint64_t RING_REGION_END = (uint64_t) ( RING_SEND_FRAMES + RING_FRAMES ) * ROCKETNET_XDP_FRAME_SIZE;

    // the staging buffer: the worker's rocketnet_server_socket_receive_buffer_t, small
    struct ring_staging_t
    {
        uint32_t num_packets;
        uint8_t eth[RING_STAGE_SLOTS][ETH_ALEN];
        rocketnet_address_t from[RING_STAGE_SLOTS];
        size_t packet_bytes[RING_STAGE_SLOTS];
        uint8_t packet_data[RING_STAGE_SLOTS][ROCKETNET_XDP_FRAME_SIZE];
    };

    struct ring_walk_t
    {
        uint8_t umem[( RING_SEND_FRAMES + RING_FRAMES ) * ROCKETNET_XDP_FRAME_SIZE];

        // the RX ring and the fill ring: the shared producer and consumer
        // words and the descriptors, and libxdp's user-side views of them
        uint32_t rx_producer;
        uint32_t rx_consumer;
        uint32_t rx_flags;
        struct xdp_desc rx_desc[RING_RX_SIZE];
        struct xsk_ring_cons rx;

        uint32_t fill_producer;
        uint32_t fill_consumer;
        uint32_t fill_flags;
        uint64_t fill_addr[RING_FILL_SIZE];
        uint32_t fill_size;
        struct xsk_ring_prod fill;

        // the worker's: its staging buffer, its pending return and the
        // pass's scratch, receive_frames
        ring_staging_t staging;
        rocketnet_pending_frames_t<RING_FRAMES> pending_return;
        uint64_t frame[RING_FRAMES];
        struct rocketnet_xdp_receive_drops_t drops;

        // the pass in progress: the worker's locals
        bool taking;
        uint32_t receive_index;
        uint32_t num_packets;
        uint32_t num_consumed;
        uint32_t num_returned;

        // the kernel's fill batch, and the one defect
        uint32_t kcache;
        uint32_t kused;
        bool defect;

        rocketnet_address_t client;
        rocketnet_address_t server;
    };

    // a frame the worker holds or hands to the fill ring, by its number in
    // the UMEM: only a receive frame's own address is that frame. An address
    // outside the receive region is 0, the model's SendFrame; an address
    // inside a frame (a descriptor's, not masked back to its frame) is a
    // negative number below -2, which no model value matches
    std::string ring_frame_name( uint64_t addr )
    {
        if ( addr < RING_REGION_BEGIN || addr >= RING_REGION_END )
        {
            return tla_int( 0 );
        }
        if ( addr % ROCKETNET_XDP_FRAME_SIZE != 0 )
        {
            return tla_int( -2 - (int64_t) addr );
        }
        return tla_int( (int64_t) ( addr / ROCKETNET_XDP_FRAME_SIZE ) );
    }

    // a descriptor on the RX ring names the frame that holds its address
    std::string ring_descriptor_name( uint64_t addr )
    {
        if ( addr < RING_REGION_BEGIN || addr >= RING_REGION_END )
        {
            return tla_int( 0 );
        }
        return ring_frame_name( addr - addr % ROCKETNET_XDP_FRAME_SIZE );
    }

    // the frames the worker holds, a set in the model: a frame held twice
    // adds -1, a member no model value matches
    void ring_hold( std::vector<std::string> & pending, uint64_t addr )
    {
        const std::string name = ring_frame_name( addr );
        for ( size_t i = 0; i < pending.size(); i++ )
        {
            if ( pending[i] == name )
            {
                pending.push_back( tla_int( -1 ) );
                return;
            }
        }
        pending.push_back( name );
    }

    std::string ring_set( const std::vector<std::string> & members )
    {
        std::string text = "{";
        for ( size_t i = 0; i < members.size(); i++ )
        {
            text += ( i > 0 ? ", " : "" ) + members[i];
        }
        return text + "}";
    }

    tla_record_t ring_state( const ring_walk_t & w )
    {
        std::vector<std::string> fq;
        for ( uint32_t i = w.fill_consumer; i != w.fill_producer; i++ )
        {
            fq.push_back( ring_frame_name( w.fill_addr[i & ( w.fill_size - 1 )] ) );
        }

        std::vector<std::string> ring;
        for ( uint32_t i = w.rx_consumer; i != w.rx_producer; i++ )
        {
            ring.push_back( ring_descriptor_name( w.rx_desc[i & ( RING_RX_SIZE - 1 )].addr ) );
        }

        // the frames the worker holds: its pending return, and this pass's
        // returned frames not yet appended to it
        std::vector<std::string> pending;
        for ( size_t i = 0; i < w.pending_return.count; i++ )
        {
            ring_hold( pending, w.pending_return.frames[i] );
        }
        if ( w.taking )
        {
            for ( uint32_t i = 0; i < w.num_returned; i++ )
            {
                ring_hold( pending, w.frame[i] );
            }
        }

        tla_record_t state;
        state.add( "fq", tla_sequence( fq ) );
        state.add( "kcache", tla_int( w.kcache ) );
        state.add( "kused", tla_int( w.kused ) );
        state.add( "ring", tla_sequence( ring ) );
        state.add( "ahead", tla_int( (int64_t) (uint32_t) ( w.rx.cached_cons - w.rx_consumer ) ) );
        state.add( "cprod", tla_int( (int64_t) (uint32_t) ( w.rx.cached_prod - w.rx_consumer ) ) );
        state.add( "phase", tla_string( w.taking ? "taking" : "idle" ) );
        state.add( "start", tla_int( w.taking ? (int64_t) (uint32_t) ( w.receive_index - w.rx_consumer ) : 0 ) );
        state.add( "peeked", tla_int( w.taking ? w.num_packets : 0 ) );
        state.add( "taken", tla_int( w.taking ? w.num_consumed : 0 ) );
        state.add( "pending", ring_set( pending ) );
        state.add( "staged", tla_int( w.staging.num_packets ) );
        state.add( "took", tla_bool( w.taking && w.num_consumed > 0 ) );
        state.add( "defect", tla_bool( w.defect ) );
        return state;
    }

    // the kernel: a packet from the client lands in the frame at addr
    void ring_post( ring_walk_t & w, uint64_t addr, uint32_t len )
    {
        struct xdp_desc * desc = &w.rx_desc[w.rx_producer & ( RING_RX_SIZE - 1 )];
        desc->addr = addr;
        desc->len = len;
        desc->options = 0;
        w.rx_producer++;
    }

    uint32_t ring_build_packet( ring_walk_t & w, uint64_t addr, int sequence )
    {
        static const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
        uint8_t * frame = w.umem + addr;
        const int header_bytes = rocketnet_xdp_frame_header_bytes( ROCKETNET_ADDRESS_IPV4 );
        memset( frame + header_bytes, sequence & 0xFF, RING_PACKET_BYTES );
        const size_t room = (size_t) ( ROCKETNET_XDP_FRAME_SIZE - addr % ROCKETNET_XDP_FRAME_SIZE );
        const int bytes = rocketnet_xdp_frame_build( frame, room, client_mac, server_mac, &w.client, &w.server, RING_PACKET_BYTES );
        return (uint32_t) bytes;
    }

    void ring_init( ring_walk_t & w, uint32_t fill_size )
    {
        w = ring_walk_t();
        w.fill_size = fill_size;

        w.rx.cached_prod = 0;
        w.rx.cached_cons = 0;
        w.rx.mask = RING_RX_SIZE - 1;
        w.rx.size = RING_RX_SIZE;
        w.rx.producer = &w.rx_producer;
        w.rx.consumer = &w.rx_consumer;
        w.rx.ring = w.rx_desc;
        w.rx.flags = &w.rx_flags;

        // libxdp's producer view starts with the whole ring free (xsk_umem__create)
        w.fill.cached_prod = 0;
        w.fill.cached_cons = w.fill_size;
        w.fill.mask = w.fill_size - 1;
        w.fill.size = w.fill_size;
        w.fill.producer = &w.fill_producer;
        w.fill.consumer = &w.fill_consumer;
        w.fill.ring = w.fill_addr;
        w.fill.flags = &w.fill_flags;

        w.client.type = ROCKETNET_ADDRESS_IPV4;
        w.client.data.ipv4[0] = 10;
        w.client.data.ipv4[3] = 2;
        w.client.port = 50000;
        w.server.type = ROCKETNET_ADDRESS_IPV4;
        w.server.data.ipv4[0] = 10;
        w.server.data.ipv4[3] = 1;
        w.server.port = 40000;
    }
}

static int trace_xdp_receive_ring( uint64_t seed, int steps, const char * out )
{
    // an odd seed sizes the fill ring as the server does, twice the frames;
    // an even seed at exactly the frames (MCRNXdpReceiveRingBrokenFillOfTheFrames'
    // size), where a flush can find no room and the pending return keeps its
    // frames for the flush at the top of a later pass (Refill). The code is
    // the same at either size, and so is the model's step relation.
    static ring_walk_t w;
    ring_init( w, seed % 2 == 1 ? RING_FILL_SIZE : RING_FRAMES );

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    // the first fill: every receive frame, in frame order. The server's
    // create reserves and writes them itself; the walk stands in for it with
    // the worker's flush, the same reserve, write and submit, and record 1
    // checks the stand-in's result against the model's Init
    uint64_t first[RING_FRAMES];
    for ( uint32_t i = 0; i < RING_FRAMES; i++ )
    {
        first[i] = RING_REGION_BEGIN + (uint64_t) i * ROCKETNET_XDP_FRAME_SIZE;
    }
    if ( !w.pending_return.append( first, RING_FRAMES ) || !rocketnet_xdp_fill_flush( &w.pending_return, &w.fill ) )
    {
        fprintf( stderr, "tla_trace xdp-receive-ring: the first fill did not fit\n" );
        return 1;
    }

    records.push_back( tla_step( "init", "init", tla_sequence( std::vector<std::string>() ), ring_state( w ) ) );

    const std::string no_args = tla_sequence( std::vector<std::string>() );
    int sequence = 0;
    int idle_tries = 0;

    while ( (int) records.size() < steps + 1 )
    {
        if ( ++idle_tries > 10000 )
        {
            fprintf( stderr, "tla_trace xdp-receive-ring: no step is possible at record %d\n", (int) records.size() );
            return 1;
        }

        const uint32_t fill_entries = w.fill_producer - w.fill_consumer;
        const uint32_t ring_entries = w.rx_producer - w.rx_consumer;

        // the application mostly drains a full staging buffer at once, but
        // not always, so the worker also peeks with no staging room (the
        // code must not peek then); past full, which only the planted peek
        // reaches, it always drains, so the staging arrays never overrun
        int choice = (int) random.below( 9 );
        if ( !w.taking && w.staging.num_packets > RING_STAGE_CAPACITY )
        {
            choice = 8;
        }
        else if ( !w.taking && w.staging.num_packets == RING_STAGE_CAPACITY && random.below( 4 ) != 0 )
        {
            choice = 8;
        }

        if ( choice == 0 || choice == 1 )
        {
            if ( w.kused >= w.kcache || ring_entries >= RING_RX_SIZE )
            {
                continue;
            }
            // the kernel posts the packet at the frame's address or past
            // its headroom (XDP_PACKET_HEADROOM), as a driver does
            const uint64_t addr = w.fill_addr[( w.fill_consumer + w.kused ) & ( w.fill_size - 1 )] + ( random.below( 2 ) == 0 ? 0 : 256 );
            const uint32_t len = ring_build_packet( w, addr, ++sequence );
            w.kused++;
            ring_post( w, addr, len );
            report.act( "KernelReceive" );
            records.push_back( tla_step( "kernel_receive", "KernelReceive", no_args, ring_state( w ) ) );
        }
        else if ( choice == 2 )
        {
            if ( w.kused != w.kcache || !( w.kused > 0 || fill_entries > w.kcache ) )
            {
                continue;
            }
            w.fill_consumer += w.kused;
            w.kcache = fill_entries - w.kused;
            w.kused = 0;
            report.act( "KernelRefresh" );
            records.push_back( tla_step( "kernel_refresh", "KernelRefresh", no_args, ring_state( w ) ) );
        }
        else if ( choice == 3 )
        {
            if ( w.defect || ring_entries >= RING_RX_SIZE || random.below( 8 ) != 0 )
            {
                continue;
            }
            // below the receive region, a send frame past its headroom, or
            // the first address past the region's end
            const uint64_t misdelivered[3] = { 0, ROCKETNET_XDP_FRAME_SIZE + 256, RING_REGION_END };
            ring_post( w, misdelivered[random.below( 3 )], 128 );
            w.defect = true;
            report.act( "KernelMisdeliver" );
            records.push_back( tla_step( "kernel_misdeliver", "KernelMisdeliver", no_args, ring_state( w ) ) );
        }
        else if ( choice == 4 )
        {
            if ( w.taking )
            {
                continue;
            }
            const uint32_t staging_room = RING_STAGE_CAPACITY - w.staging.num_packets;
            const uint32_t peeked = rocketnet_xdp_receive_peek( &w.rx, &w.staging, RING_STAGE_CAPACITY, &w.pending_return, &w.receive_index );
            if ( peeked == 0 )
            {
                continue;
            }
            w.taking = true;
            w.num_packets = peeked;
            w.num_consumed = 0;
            w.num_returned = 0;
            report.act( "Peek" );
            records.push_back( tla_step( "peek", "Peek", no_args, ring_state( w ) ) );
            if ( peeked > staging_room )
            {
                report.planted( (int) records.size() );
            }
        }
        else if ( choice == 5 || choice == 6 )
        {
            if ( !w.taking )
            {
                continue;
            }
            if ( w.num_consumed < w.num_packets )
            {
                rocketnet_xdp_receive_take( &w.rx, w.receive_index, &w.num_consumed, w.umem, RING_REGION_BEGIN, RING_REGION_END,
                                            ROCKETNET_ADDRESS_IPV4, &w.staging, RING_STAGE_CAPACITY, w.frame, &w.num_returned, &w.drops );
                report.act( "Take" );
                records.push_back( tla_step( "take", "Take", no_args, ring_state( w ) ) );
            }
            else
            {
                if ( !rocketnet_xdp_receive_end( &w.rx, w.num_packets, w.num_consumed, w.frame, w.num_returned, &w.pending_return ) )
                {
                    fprintf( stderr, "tla_trace xdp-receive-ring: the pending return refused the pass's frames at record %d\n", (int) records.size() );
                    return 1;
                }
                w.taking = false;
                rocketnet_xdp_fill_flush( &w.pending_return, &w.fill );
                report.act( "Finish" );
                records.push_back( tla_step( "finish", "Finish", no_args, ring_state( w ) ) );
            }
        }
        else if ( choice == 7 )
        {
            if ( w.taking || w.pending_return.count == 0 || !rocketnet_xdp_fill_flush( &w.pending_return, &w.fill ) )
            {
                continue;
            }
            report.act( "Refill" );
            records.push_back( tla_step( "flush", "Refill", no_args, ring_state( w ) ) );
        }
        else
        {
            // the application drains seldom, so the staging buffer fills
            // across passes and the peek is cut to its room
            if ( w.taking || w.staging.num_packets == 0 || ( w.staging.num_packets < RING_STAGE_CAPACITY && random.below( 4 ) != 0 ) )
            {
                continue;
            }
            w.staging.num_packets = 0;
            report.act( "Drain" );
            records.push_back( tla_step( "drain", "Drain", no_args, ring_state( w ) ) );
        }

        idle_tries = 0;
    }

    report.print();

    std::vector<std::string> frames;
    for ( uint32_t i = 0; i < RING_FRAMES; i++ )
    {
        frames.push_back( ring_frame_name( RING_REGION_BEGIN + (uint64_t) i * ROCKETNET_XDP_FRAME_SIZE ) );
    }

    char constants[256];
    snprintf( constants, sizeof( constants ), "    RingSize = %d\n    StageCap = %d\n    PendCap = %d\n    FillSize = %d\n",
              (int) RING_RX_SIZE, (int) RING_STAGE_CAPACITY, (int) RING_FRAMES, (int) w.fill_size );
    const std::string frame_constants = "    Frames = " + ring_set( frames ) + "\n" + constants;
    return tla_write_trace( out, "MCTraceXdpReceiveRing", seed, "TraceRNXdpReceiveRing", "", frame_constants, records ) ? 0 : 1;
}

#else // #if defined( __linux__ ) && ROCKETNET_XDP

// the receive pass is the AF_XDP worker's: a build without XDP has no code to
// walk, and the layer says so rather than pass
static int trace_xdp_receive_ring( uint64_t, int, const char * )
{
    fprintf( stderr, "tla_trace xdp-receive-ring: this build has no XDP transport (ROCKETNET_XDP off): no trace\n" );
    return 1;
}

#endif // #if defined( __linux__ ) && ROCKETNET_XDP

TLA_TRACE_LAYER( "xdp-receive-ring", trace_xdp_receive_ring );
