/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Shared by native transport tests and the socket-free s390x executable.

#pragma once

#ifndef ROCKETNET_TESTS_ROUTE_CHECKS_H
#define ROCKETNET_TESTS_ROUTE_CHECKS_H

#include "rocketnet_route.h"

static void test_route_prefixes()
{
    const uint8_t bytes[4] = { 10, 160, 3, 9 };
    const uint32_t ip = rocketnet_route_ip( bytes );
    rocketnet_ifforward4_t fallback = { 1, 0, 0, 100 };
    CHECK( better_match( &fallback, NULL, ip ) );
    CHECK( ip == 0x0aa00309u );
    rocketnet_ifforward4_t net = { 2, 0x0aa00000u, 0xffe00000u, 50 }; // /11
    CHECK( better_match( &net, &fallback, ip ) );
    CHECK( !better_match( &fallback, &net, ip ) );
    rocketnet_ifforward4_t wrong = { 3, 0x0aa10309u, 0xffffffffu, 1 };
    CHECK( !better_match( &wrong, &net, ip ) ); // 31 matching bits is still no route
    rocketnet_ifforward4_t host = { 4, ip, 0xffffffffu, 90 };
    CHECK( better_match( &host, &net, ip ) );
    rocketnet_ifforward4_t cheap = host;
    cheap.forward_metric = 10;
    CHECK( better_match( &cheap, &host, ip ) );
    CHECK( !better_match( &host, &cheap, ip ) );
    CHECK( !better_match( &cheap, &cheap, ip ) );
    const uint8_t edge[4] = { 10, 191, 255, 255 };
    const uint8_t outside[4] = { 10, 192, 0, 0 };
    CHECK( better_match( &net, NULL, rocketnet_route_ip( edge ) ) );
    CHECK( !better_match( &net, NULL, rocketnet_route_ip( outside ) ) );
    printf( "test_route_prefixes\n" );
}

#if defined( __linux__ )
// Netlink headers/integers use native byte order; destination bytes use
// network order. The same fixture executes on native Linux and s390x.
struct route_fixture_t
{
    nlmsghdr header;
    rtmsg route;
    uint8_t attributes[128];
    route_fixture_t( uint8_t prefix = 0 )
    {
        memset( this, 0, sizeof( *this ) );
        header.nlmsg_len = NLMSG_LENGTH( sizeof( rtmsg ) );
        header.nlmsg_type = RTM_NEWROUTE;
        route.rtm_family = AF_INET;
        route.rtm_table = RT_TABLE_MAIN;
        route.rtm_type = RTN_UNICAST;
        route.rtm_dst_len = prefix;
    }
    void add( unsigned short type, const void * value, unsigned bytes )
    {
        CHECK( bytes <= sizeof( attributes ) );
        rtattr * attr = (rtattr *) ( (uint8_t *) this + NLMSG_ALIGN( header.nlmsg_len ) );
        attr->rta_type = type;
        attr->rta_len = (unsigned short) RTA_LENGTH( bytes );
        memcpy( RTA_DATA( attr ), value, bytes );
        header.nlmsg_len = NLMSG_ALIGN( header.nlmsg_len ) + RTA_ALIGN( attr->rta_len );
    }
};

static void test_route_netlink()
{
    rocketnet_ifforward4_t parsed;
    memset( &parsed, 0xa5, sizeof( parsed ) );
    uint32_t oif = 7, priority = 123;
    route_fixture_t fallback;
    fallback.add( RTA_OIF, &oif, sizeof( oif ) );
    CHECK( parse_kernel_route( &fallback.header, &parsed ) );
    CHECK( parsed.if_index == 7 && parsed.forward_dest == 0 && parsed.forward_mask == 0 && parsed.forward_metric == 0 );
    const uint8_t destination[4] = { 10, 160, 0, 0 };
    route_fixture_t subnet( 11 );
    subnet.add( RTA_DST, destination, 4 );
    subnet.add( RTA_OIF, &oif, 4 );
    subnet.add( RTA_PRIORITY, &priority, 4 );
    uint32_t nested_metrics[2] = { 0xdeadbeef, 0x12345678 };
    subnet.add( RTA_METRICS, nested_metrics, sizeof( nested_metrics ) );
    CHECK( parse_kernel_route( &subnet.header, &parsed ) );
    CHECK( parsed.forward_dest == 0x0aa00000u && parsed.forward_mask == 0xffe00000u );
    CHECK( parsed.if_index == oif && parsed.forward_metric == priority );
    subnet.route.rtm_dst_len = 32;
    CHECK( parse_kernel_route( &subnet.header, &parsed ) && parsed.forward_mask == 0xffffffffu );
    subnet.route.rtm_dst_len = 33;
    CHECK( !parse_kernel_route( &subnet.header, &parsed ) );
    subnet.route.rtm_dst_len = 11;
    subnet.route.rtm_family = AF_INET6;
    CHECK( !parse_kernel_route( &subnet.header, &parsed ) );
    subnet.route.rtm_family = AF_INET;
    subnet.route.rtm_table = RT_TABLE_LOCAL;
    CHECK( !parse_kernel_route( &subnet.header, &parsed ) );
    subnet.route.rtm_table = RT_TABLE_MAIN;
    subnet.route.rtm_type = RTN_BLACKHOLE;
    CHECK( !parse_kernel_route( &subnet.header, &parsed ) );
    route_fixture_t missing;
    CHECK( !parse_kernel_route( &missing.header, &parsed ) );
    route_fixture_t short_attr;
    short_attr.add( RTA_OIF, &oif, 1 );
    CHECK( !parse_kernel_route( &short_attr.header, &parsed ) );
    route_fixture_t short_header;
    short_header.header.nlmsg_len = NLMSG_LENGTH( sizeof( rtmsg ) ) - 1;
    CHECK( !parse_kernel_route( &short_header.header, &parsed ) );
    printf( "test_route_netlink\n" );
}
#endif

#endif // ROCKETNET_TESTS_ROUTE_CHECKS_H
