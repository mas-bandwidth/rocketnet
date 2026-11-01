/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The reply MAC table (sdk/rocketnet_xdp_frame.h) under a delayed send. With
// no gateway, a handshake reply is addressed by the source MAC of the frame
// it answers, found by its destination pointing into the current receive
// view. The development link simulator holds that reply for its latency and
// sends it, sometimes twice, from a copied address once the view is gone.
// The fakes here are a receive view, the start rule the xdp transport applies
// to a reply (answered MAC only for an address inside the view) and a link
// that holds packets until they are due. No sockets, no threads, no real
// time: the clock is an argument and starts at 1000.0.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"

#define FAKE_VIEW_PACKETS       4
#define FAKE_LINK_PACKETS       8

struct fake_receive_view_t
{
    int num_packets;
    rocketnet_address_t from[FAKE_VIEW_PACKETS];
    uint8_t eth[FAKE_VIEW_PACKETS][6];
};

struct fake_held_packet_t
{
    double due_time;
    rocketnet_address_t to;
};

struct fake_link_t
{
    int num_packets;
    struct fake_held_packet_t packets[FAKE_LINK_PACKETS];
};

// the xdp start rule: a destination inside the view answers that view
// packet, and its source MAC is offered; any other destination offers none
static const uint8_t * fake_start_packet( struct rocketnet_xdp_reply_macs_t * table, const struct fake_receive_view_t * view,
                                          const rocketnet_address_t * to, uint64_t * refused )
{
    const uint8_t * answered_mac = NULL;
    const uintptr_t view_begin = (uintptr_t) &view->from[0];
    const uintptr_t view_end = (uintptr_t) &view->from[view->num_packets];
    const uintptr_t to_pointer = (uintptr_t) to;
    if ( to_pointer >= view_begin && to_pointer < view_end )
    {
        answered_mac = view->eth[( to_pointer - view_begin ) / sizeof( rocketnet_address_t )];
    }

    return rocketnet_xdp_reply_mac( table, to, answered_mac, refused );
}

static void fake_receive( struct fake_receive_view_t * view, const rocketnet_address_t * from, const uint8_t * mac )
{
    view->num_packets = 1;
    view->from[0] = *from;
    memcpy( view->eth[0], mac, 6 );
}

// the simulator copies the destination and holds the packet; a duplicate is a second copy
static void fake_link_hold( struct fake_link_t * link, const rocketnet_address_t * to, double due_time )
{
    rocketnet_unit_check( link->num_packets < FAKE_LINK_PACKETS );

    link->packets[link->num_packets].due_time = due_time;
    link->packets[link->num_packets].to = *to;
    link->num_packets++;
}

// sends every packet due at time; returns how many found a MAC, and checks each one is mac
static int fake_link_drain( struct fake_link_t * link, struct rocketnet_xdp_reply_macs_t * table,
                            const struct fake_receive_view_t * view, double time, const uint8_t * mac, uint64_t * refused )
{
    int sent = 0;
    int kept = 0;
    for ( int i = 0; i < link->num_packets; i++ )
    {
        if ( link->packets[i].due_time > time )
        {
            link->packets[kept++] = link->packets[i];
            continue;
        }

        const uint8_t * destination_mac = fake_start_packet( table, view, &link->packets[i].to, refused );
        if ( destination_mac )
        {
            rocketnet_unit_check( mac && memcmp( destination_mac, mac, 6 ) == 0 );

            sent++;
        }
    }

    link->num_packets = kept;

    return sent;
}

static struct rocketnet_xdp_reply_macs_t simulated_reply_macs;
static struct rocketnet_xdp_reply_mac_entry_t simulated_reply_macs_entries[ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES];

// binds the table to its storage, sized as a default-config server sizes it,
// and resets it with gateway (NULL: none)
static void simulated_reply_macs_bind( struct rocketnet_xdp_reply_macs_t * table, const uint8_t * gateway )
{
    rocketnet_unit_check( rocketnet_xdp_reply_macs_bind( table, simulated_reply_macs_entries, ROCKETNET_XDP_REPLY_MAC_DEFAULT_ENTRIES ) );
    rocketnet_unit_check( (int) table->mask + 1 == rocketnet_xdp_reply_mac_entries( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    rocketnet_xdp_reply_macs_reset( table, gateway );
}

static void test_xdp_transport_reply_mac_held_reply_finds_the_answered_mac()
{
    static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
    static const uint8_t spoofer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0d };
    static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

    struct rocketnet_xdp_reply_macs_t * table = &simulated_reply_macs;
    simulated_reply_macs_bind( table, NULL );    // no gateway: an on-link segment

    rocketnet_address_t client, stranger;
    rocketnet_unit_check( rocketnet_address_parse( &client, "10.77.4.2:50000" ) );
    rocketnet_unit_check( rocketnet_address_parse( &stranger, "10.77.4.3:50000" ) );

    struct fake_receive_view_t view;
    memset( &view, 0, sizeof( view ) );

    struct fake_link_t link;
    memset( &link, 0, sizeof( link ) );

    uint64_t refused = 0;
    double time = 1000.0;

    // a connection request arrives; its cookie reply is reserved in the view
    // with the request's MAC, then handed to the link, which holds it 30 ms
    // and duplicates it

    fake_receive( &view, &client, client_mac );
    const uint8_t * mac = fake_start_packet( table, &view, &view.from[0], &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    fake_link_hold( &link, &view.from[0], time + 0.030 );
    fake_link_hold( &link, &view.from[0], time + 0.032 );
    view.num_packets = 0;                              // the view is gone

    // nothing is due yet

    rocketnet_unit_check( fake_link_drain( &link, table, &view, time + 0.010, client_mac, &refused ) == 0 );
    rocketnet_unit_check( link.num_packets == 2 );

    // the held reply and its duplicate leave to the MAC the request taught,
    // before anything is learned (the client opens nothing until the cookie
    // reaches it)

    time += 0.040;

    rocketnet_unit_check( rocketnet_xdp_reply_macs_find( table, &client ) == NULL );
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, client_mac, &refused ) == 2 );
    rocketnet_unit_check( refused == 0 );
    rocketnet_unit_check( link.num_packets == 0 );

    // an address no frame ever came from is still refused and counted

    fake_link_hold( &link, &stranger, time );
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, NULL, &refused ) == 0 );
    rocketnet_unit_check( refused == 1 );

    // a frame with a zero source MAC teaches nothing and its reply is refused

    fake_receive( &view, &stranger, zero_mac );
    rocketnet_unit_check( fake_start_packet( table, &view, &view.from[0], &refused ) == NULL );
    rocketnet_unit_check( refused == 2 );

    view.num_packets = 0;
    fake_link_hold( &link, &stranger, time );
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, NULL, &refused ) == 0 );
    rocketnet_unit_check( refused == 3 );

    // the client's response opens under its keys: the learn takes over, and
    // an unauthenticated frame from its address steers no later reply

    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );
    fake_receive( &view, &client, spoofer_mac );
    mac = fake_start_packet( table, &view, &view.from[0], &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );

    fake_link_hold( &link, &view.from[0], time + 0.030 );
    view.num_packets = 0;
    time += 0.040;
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, client_mac, &refused ) == 1 );

    // the client is released: its learned MAC is forgotten, so it is no
    // longer a learned client, and an ack still held for it leaves to the
    // MAC its replies went to, as it would have on the wire

    fake_link_hold( &link, &client, time + 0.030 );
    rocketnet_xdp_reply_macs_forget( table, &client );
    rocketnet_unit_check( rocketnet_xdp_reply_macs_find( table, &client ) == NULL );

    time += 0.040;
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, client_mac, &refused ) == 1 );
    rocketnet_unit_check( refused == 3 );

    // a new client at the released address: the newest answered frame wins

    static const uint8_t next_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0e };
    fake_receive( &view, &client, next_mac );
    mac = fake_start_packet( table, &view, &view.from[0], &refused );
    rocketnet_unit_check( mac && memcmp( mac, next_mac, 6 ) == 0 );

    fake_link_hold( &link, &view.from[0], time + 0.030 );
    view.num_packets = 0;
    time += 0.040;
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, next_mac, &refused ) == 1 );

    // a forget leaves nothing for an address no frame came from

    rocketnet_address_t never;
    rocketnet_unit_check( rocketnet_address_parse( &never, "10.77.4.9:50000" ) );

    rocketnet_xdp_reply_macs_forget( table, &never );
    fake_link_hold( &link, &never, time );
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, NULL, &refused ) == 0 );
    rocketnet_unit_check( refused == 4 );

    // with a gateway, a held reply to an unlearned address goes to the
    // gateway, as a direct one does

    static const uint8_t gateway_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    rocketnet_xdp_reply_macs_reset( table, gateway_mac );
    fake_receive( &view, &client, client_mac );
    mac = fake_start_packet( table, &view, &view.from[0], &refused );
    rocketnet_unit_check( mac && memcmp( mac, gateway_mac, 6 ) == 0 );

    fake_link_hold( &link, &view.from[0], time + 0.030 );
    view.num_packets = 0;
    time += 0.040;
    rocketnet_unit_check( fake_link_drain( &link, table, &view, time, gateway_mac, &refused ) == 1 );
    rocketnet_unit_check( refused == 4 );
}

// the answered table is bounded like the learned one: far more answered
// addresses than entries never displace a learned client, and the newest
// answered address is held
static void test_xdp_transport_reply_mac_answered_entries_never_displace_learned()
{
    static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };

    struct rocketnet_xdp_reply_macs_t * table = &simulated_reply_macs;
    simulated_reply_macs_bind( table, NULL );

    rocketnet_address_t client;
    rocketnet_unit_check( rocketnet_address_parse( &client, "10.77.4.2:50000" ) );

    rocketnet_xdp_reply_macs_learn( table, &client, client_mac );

    struct fake_receive_view_t view;
    memset( &view, 0, sizeof( view ) );

    uint64_t refused = 0;
    rocketnet_address_t many = client;
    uint8_t many_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };

    // every answered address and MAC is distinct, and none is the client's,
    // at any ROCKETNET_MAX_CLIENTS: the index spreads over three address bytes
    // (10.78.0.0 upward, port 1000) and the low three MAC bytes

    many.port = 1000;

    for ( int i = 0; i < (int) ( table->mask + 1 ) * 2; ++i )
    {
        many.data.ipv4[1] = (uint8_t) ( 78 + ( i >> 16 ) );
        many.data.ipv4[2] = (uint8_t) ( i >> 8 );
        many.data.ipv4[3] = (uint8_t) i;
        many_mac[3] = (uint8_t) ( i >> 16 );
        many_mac[4] = (uint8_t) ( i >> 8 );
        many_mac[5] = (uint8_t) i;
        fake_receive( &view, &many, many_mac );
        rocketnet_unit_check( fake_start_packet( table, &view, &view.from[0], &refused ) != NULL );

        view.num_packets = 0;
        const uint8_t * mac = fake_start_packet( table, &view, &many, &refused );
        rocketnet_unit_check( mac && memcmp( mac, many_mac, 6 ) == 0 );
    }

    rocketnet_unit_check( refused == 0 );

    const uint8_t * mac = fake_start_packet( table, &view, &client, &refused );
    rocketnet_unit_check( mac && memcmp( mac, client_mac, 6 ) == 0 );
}

REGISTER_UNIT_TEST( test_xdp_transport_reply_mac_held_reply_finds_the_answered_mac );
REGISTER_UNIT_TEST( test_xdp_transport_reply_mac_answered_entries_never_displace_learned );
