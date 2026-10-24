/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Game loop: what server.cpp, client.cpp and launcher.cpp share. Each side
    runs the same 60 Hz frame loop: the network step for what the last frame
    produced runs on its own thread while this frame simulates, and the frame
    joins it before it swaps the two buffers (a double buffer with a join).
    RocketNet is called from one thread at a time, the network step's, and
    runs no thread of its own here: the parallelism is the game's.
*/

#ifndef GAME_LOOP_GAME_H
#define GAME_LOOP_GAME_H

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"

#include <atomic>
#include <thread>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_DT ( 1.0 / 60.0 )
#define MAX_CLIENTS 64
#define MESSAGE_BYTES 12                        // the sender (uint32), the frame (uint32), a value (float)

// one message each way per frame: a client's input, the server's state for that client
struct message_t
{
    uint32_t sender;                            // the client number, 0 .. MAX_CLIENTS - 1
    uint32_t frame;                             // the frame it was queued in: its sequence
    float value;
};

static inline void write_message( uint8_t * p, const message_t & m )
{
    memcpy( p, &m.sender, 4 );
    memcpy( p + 4, &m.frame, 4 );
    memcpy( p + 8, &m.value, 4 );
}

static inline bool read_message( const uint8_t * p, int bytes, message_t & m )
{
    if ( bytes != MESSAGE_BYTES )
        return false;
    memcpy( &m.sender, p, 4 );
    memcpy( &m.frame, p + 4, 4 );
    memcpy( &m.value, p + 8, 4 );
    return m.sender < MAX_CLIENTS;
}

// The wire check of listen mode, where the server and the clients are in one
// process: after its send_packets returns, a side stores the last frame it
// sent; at the start of its update, the other side reads it. Everything sent
// before an update began must be taken by that update, so a message sent
// before the receiver's network step is in that step's receive: no frame is
// lost to the handoff between the two threads. A client that is leaving is
// not checked: its disconnect ends the connection with its last input.
struct wire_t
{
    std::atomic<int64_t> input_sent[MAX_CLIENTS];   // client i: its last input's frame, sent
    std::atomic<int64_t> state_sent[MAX_CLIENTS];   // the server: the last state frame sent to client i
    std::atomic<bool> leaving[MAX_CLIENTS];         // client i is about to disconnect
    std::atomic<int> late;                          // messages an update should have taken and did not

    wire_t()
    {
        for ( int i = 0; i < MAX_CLIENTS; i++ ) { input_sent[i] = -1; state_sent[i] = -1; leaving[i] = false; }
        late = 0;
    }
};

// the network step's cost, in microseconds, over the run
struct cost_t
{
    double sum, max;
    int count;
};

static inline void add_cost( cost_t & cost, double seconds )
{
    const double us = seconds * 1e6;
    cost.sum += us;
    cost.max = us > cost.max ? us : cost.max;
    cost.count++;
}

// fixed 60 Hz frames: sleep to the frame's end, as the game does
static inline void wait_for_frame_end( double & frame_end )
{
    const double now = rocketnet_time();
    if ( now < frame_end )
        rocketnet_sleep( frame_end - now );
    frame_end += FRAME_DT;
    if ( frame_end < rocketnet_time() )
        frame_end = rocketnet_time() + FRAME_DT;    // dropped frames: start again from now
}

struct server_args_t
{
    const char * address;                       // the bind address
    const char * transport;                     // "loopback" in listen mode, NULL (auto) over UDP
    int max_clients;
    wire_t * wire;                              // listen mode only, else NULL
    std::atomic<int> * quit;
    // written by the server once it listens
    std::atomic<int> ready;
    uint16_t port;
    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    // the result
    int inputs;                                 // inputs taken from clients
    int clients_seen;                           // the most clients connected at once
    cost_t cost;
    int result;
};

struct client_args_t
{
    int number;                                 // 0 .. MAX_CLIENTS - 1, the sender field
    const char * server_address;
    const uint8_t * server_public_key;
    const char * transport;
    double seconds;                             // connected time to run for
    wire_t * wire;
    // the result
    int states;                                 // state updates taken from the server
    cost_t cost;
    int result;
};

void server_main( server_args_t * args );
void client_main( client_args_t * args );

#endif // #ifndef GAME_LOOP_GAME_H
