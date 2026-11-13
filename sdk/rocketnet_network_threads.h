/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_NETWORK_THREADS_H
#define ROCKETNET_SDK_ROCKETNET_NETWORK_THREADS_H

// Each server transport's network threads: the one place their defaults and
// limits are written (pure macros, every platform; the server config table in
// sdk/rocketnet_internal.h and each transport read them). A transport's count
// is its own config field and its own environment variable, never a shared
// one: num_xdp_threads (ROCKETNET_SERVER_NUM_XDP_THREADS), num_gso_workers,
// num_io_uring_workers, num_rio_lanes, num_iocp_completion_workers and
// num_iocp_send_threads, num_msgx_transmit_workers. Zero or unset is the
// default below; the environment wins over the config; a value past the
// limit is refused at create, never clamped.
//
// A default is the count at which that transport passed the load test on a
// good machine (1000 players at 60 Hz, 10 Gb/s out, every packet sent, 60
// frames a second, every player whole), so a server with nothing configured
// reaches that bar there. Where two machines gave different answers the
// default is the count that passed on both. Each line names its rows (the
// records are final-fable's REMEASURE-MAIN.md and GOAL-*.md, 2026-11-13).

// XDP threads, one per NIC queue. 1: rows R5-R8, X710 pair (bench-xdp-8),
// main 59890655d, peak and average, with and without a game load (2 also
// passed, R1-R4). The NIC must be provisioned to the same queue count
// (scripts/rocketnet-provision): XDP refuses a NIC with another count.
#define ROCKETNET_XDP_DEFAULT_THREADS                                    1
#define ROCKETNET_XDP_MAX_THREADS                                     1024

// gso queue workers, one reuseport socket each. 1: rows G1-G4, X710 pair,
// main 3218cf5f6; and AWS c8gn.8xlarge (GOAL-gso) at 1.
#define ROCKETNET_GSO_DEFAULT_WORKERS                                    1
#define ROCKETNET_GSO_MAX_WORKERS                                       32

// io_uring workers, one ring and socket each. 2: AWS c6in.8xlarge rows u03,
// u05, u08, u09 (goal-uring c154d7971), where 1 missed the frame with a game
// load (u10, 51 fps); 2 also passed on the X710 pair (U1-U4, main 3afea2aed),
// where 1 was enough. 2 is the count that passed on both.
#define ROCKETNET_IO_URING_DEFAULT_WORKERS                               2
#define ROCKETNET_IO_URING_MAX_WORKERS                                  32

// RIO lanes, a socket, a registered region and an I/O worker each. 5:
// measured from main 3218cf5f6 on 2026-11-13, AWS m7i.16xlarge server and
// c8gn.8xlarge players in one zone and subnet: all four rows (peak and
// average, each with and without a game load) pass at 5 lanes and at 6; 4
// lanes fails the peak row on rate (545k of 900k packets/s). Rows R1-R9 under
// "# RIO" in the 2026-11-13 re-measure of main. More than one lane needs a
// specific bind address: with the wildcard and nothing asked, RIO runs one
// lane and says so.
#define ROCKETNET_RIO_DEFAULT_LANES                                      5
#define ROCKETNET_RIO_MAX_LANES                                         32

// IOCP completion workers (a socket and completion port each) and send
// threads (lane l to thread l % threads): two real choices, each measured.
// 2 and 2: AWS m7i.16xlarge rows f01-f04 (goal-iocp-send 1afaabe69); 2
// workers with 1 send thread (696.7k at 47 fps), 1 worker with 2 (855.9k at
// 57.6 fps) and 1 with 1 all failed. More than one completion worker needs a
// specific bind address: with the wildcard and nothing asked, IOCP runs one
// worker and says so. The send threads are at most the send lanes, which
// max_clients sets (8 at 1000 clients).
#define ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS                        2
#define ROCKETNET_IOCP_MAX_COMPLETION_WORKERS                           32
#define ROCKETNET_IOCP_DEFAULT_SEND_THREADS                              2
#define ROCKETNET_IOCP_MAX_SEND_THREADS                                 16

// macOS msgx transmit workers (sendmsg_x); one receive worker beside them is
// fixed. 3: measured from main 5366e4f13 on 2026-11-13, players on a Linux
// machine over 10 GbE: with mini-m5 (M5 Pro) as the server, 3 transmit
// workers send all 900k packets/s at peak with 1000 players whole; with the
// studio (M3 Ultra) as the server the rate tops out at 694k at 2 transmit
// workers and falls as workers are added (678k at 1, 660k at 3, 492k at 6),
// each worker near 100% in the kernel's send path. Rows under "# macOS" in
// the 2026-11-13 re-measure of main.
#define ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS                          3
#define ROCKETNET_MSGX_MAX_TRANSMIT_WORKERS                             16

// The portable transport has no count to choose: one send and one receive
// thread, always.

// The count a transport with a count of its own runs: asked when nonzero
// (the config field, or its environment variable, which wins), else the
// default. A negative count is refused: 0 back, which no transport runs (the
// config table refuses one at create; this is the answer for a layer caller
// that sizes a config the table never saw). Any thread.
static inline int rocketnet_network_threads_count( int asked, int default_count )
{
    if ( asked < 0 )
    {
        return 0;
    }
    return asked > 0 ? asked : default_count;
}

// The count a Windows transport of a socket per thread runs (rio lanes, iocp
// completion workers). Asked when nonzero: more than one on a wildcard bind
// is that transport's refusal at start, never changed here. Zero is the
// default on a specific bind address, and one on the wildcard (0.0.0.0 or
// ::), where sockets cannot share the port; the transport then warns once,
// naming the default and how to reach it. A negative count is refused: 0
// back, as rocketnet_network_threads_count. Any thread.
static inline int rocketnet_network_threads_per_socket( int asked, int default_count, int wildcard )
{
    if ( asked < 0 )
    {
        return 0;
    }
    if ( asked > 0 )
    {
        return asked;
    }
    return wildcard ? 1 : default_count;
}

// The RIO lanes a server runs, the one answer its memory plan and its create
// both use: num_rio_lanes when asked (0 back, a refusal, below 0 or past
// ROCKETNET_RIO_MAX_LANES); else one on the wildcard (several 0); else, on a
// bind address the lanes can share, test_lanes when it is 2 ..
// ROCKETNET_RIO_MAX_LANES (the development library's ROCKETNET_TEST_RIO_LANES,
// 0 when unset or in a production build), and the default otherwise. Any
// thread.
static inline int rocketnet_rio_lanes_for( int asked, int several, int test_lanes )
{
    if ( asked < 0 || asked > ROCKETNET_RIO_MAX_LANES )
    {
        return 0;
    }
    if ( asked > 0 )
    {
        return asked;
    }
    if ( !several )
    {
        return 1;
    }
    if ( test_lanes >= 2 && test_lanes <= ROCKETNET_RIO_MAX_LANES )
    {
        return test_lanes;
    }
    return ROCKETNET_RIO_DEFAULT_LANES;
}

#endif // ROCKETNET_SDK_ROCKETNET_NETWORK_THREADS_H
