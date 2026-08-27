/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_network_simulator.h"
#include "rocketnet_protocol.h"
#include "rocketnet.h"

#include <string.h>

struct rocketnet_network_simulator_t
{
    void * context;
    int capacity;
    double latency;
    double jitter;
    float packet_loss_percent;
    float duplicate_percent;
    uint64_t rng_state;

    // parallel fixed arrays, allocated once at create
    int * used;
    double * delivery_time;
    struct rocketnet_address_t * from;
    struct rocketnet_address_t * to;
    int * packet_bytes;
    uint8_t * packet_data;                  // capacity * ROCKETNET_RECEIVE_CEILING
};

// xorshift64: deterministic, seedable, and nowhere near the crypto -- this
// randomness decides packet fates, never key material

static uint64_t rocketnet_simulator_random( struct rocketnet_network_simulator_t * simulator )
{
    uint64_t x = simulator->rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    simulator->rng_state = x;
    return x;
}

static double rocketnet_simulator_random_float( struct rocketnet_network_simulator_t * simulator )
{
    return (double) ( rocketnet_simulator_random( simulator ) >> 11 ) / (double) ( 1ULL << 53 );
}

struct rocketnet_network_simulator_t * rocketnet_network_simulator_create( void * context,
                                                                           int capacity,
                                                                           uint64_t seed )
{
    rocketnet_assert( capacity > 0 );

    struct rocketnet_network_simulator_t * simulator = (struct rocketnet_network_simulator_t*) rocketnet_malloc( context, sizeof(struct rocketnet_network_simulator_t) );
    if ( !simulator )
        return NULL;
    memset( simulator, 0, sizeof(*simulator) );

    simulator->context = context;
    simulator->capacity = capacity;
    simulator->rng_state = seed ? seed : 1;

    simulator->used = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    simulator->delivery_time = (double*) rocketnet_malloc( context, sizeof(double) * (size_t) capacity );
    simulator->from = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    simulator->to = (struct rocketnet_address_t*) rocketnet_malloc( context, sizeof(struct rocketnet_address_t) * (size_t) capacity );
    simulator->packet_bytes = (int*) rocketnet_malloc( context, sizeof(int) * (size_t) capacity );
    simulator->packet_data = (uint8_t*) rocketnet_malloc( context, (size_t) ROCKETNET_RECEIVE_CEILING * (size_t) capacity );

    if ( !simulator->used || !simulator->delivery_time || !simulator->from || !simulator->to || !simulator->packet_bytes || !simulator->packet_data )
    {
        rocketnet_network_simulator_destroy( simulator );
        return NULL;
    }

    memset( simulator->used, 0, sizeof(int) * (size_t) capacity );

    return simulator;
}

void rocketnet_network_simulator_destroy( struct rocketnet_network_simulator_t * simulator )
{
    rocketnet_assert( simulator );
    rocketnet_free( simulator->context, simulator->used );
    rocketnet_free( simulator->context, simulator->delivery_time );
    rocketnet_free( simulator->context, simulator->from );
    rocketnet_free( simulator->context, simulator->to );
    rocketnet_free( simulator->context, simulator->packet_bytes );
    rocketnet_free( simulator->context, simulator->packet_data );
    rocketnet_free( simulator->context, simulator );
}

void rocketnet_network_simulator_config( struct rocketnet_network_simulator_t * simulator,
                                         double latency,
                                         double jitter,
                                         float packet_loss_percent,
                                         float duplicate_percent )
{
    rocketnet_assert( simulator );
    simulator->latency = latency;
    simulator->jitter = jitter;
    simulator->packet_loss_percent = packet_loss_percent;
    simulator->duplicate_percent = duplicate_percent;
}

static void rocketnet_simulator_insert( struct rocketnet_network_simulator_t * simulator,
                                        const struct rocketnet_address_t * from,
                                        const struct rocketnet_address_t * to,
                                        const uint8_t * packet_data,
                                        int packet_bytes,
                                        double delivery_time )
{
    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( simulator->used[i] )
            continue;
        simulator->used[i] = 1;
        simulator->delivery_time[i] = delivery_time;
        simulator->from[i] = *from;
        simulator->to[i] = *to;
        simulator->packet_bytes[i] = packet_bytes;
        memcpy( simulator->packet_data + (size_t) i * ROCKETNET_RECEIVE_CEILING, packet_data, (size_t) packet_bytes );
        return;
    }
    rocketnet_spam( "network simulator queue full. packet dropped" );
}

void rocketnet_network_simulator_send( struct rocketnet_network_simulator_t * simulator,
                                       const struct rocketnet_address_t * from,
                                       const struct rocketnet_address_t * to,
                                       const uint8_t * packet_data,
                                       int packet_bytes,
                                       double current_time )
{
    rocketnet_assert( simulator );
    rocketnet_assert( from );
    rocketnet_assert( to );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    if ( rocketnet_simulator_random_float( simulator ) * 100.0 < (double) simulator->packet_loss_percent )
        return;

    double delivery_time = current_time + simulator->latency + rocketnet_simulator_random_float( simulator ) * simulator->jitter;
    rocketnet_simulator_insert( simulator, from, to, packet_data, packet_bytes, delivery_time );

    if ( rocketnet_simulator_random_float( simulator ) * 100.0 < (double) simulator->duplicate_percent )
    {
        double duplicate_time = current_time + simulator->latency + rocketnet_simulator_random_float( simulator ) * simulator->jitter;
        rocketnet_simulator_insert( simulator, from, to, packet_data, packet_bytes, duplicate_time );
    }
}

int rocketnet_network_simulator_receive( struct rocketnet_network_simulator_t * simulator,
                                         const struct rocketnet_address_t * to,
                                         double current_time,
                                         struct rocketnet_address_t * out_from,
                                         uint8_t * out_packet )
{
    rocketnet_assert( simulator );
    rocketnet_assert( to );
    rocketnet_assert( out_from );
    rocketnet_assert( out_packet );

    for ( int i = 0; i < simulator->capacity; i++ )
    {
        if ( !simulator->used[i] )
            continue;
        if ( simulator->delivery_time[i] > current_time )
            continue;
        if ( !rocketnet_address_equal( &simulator->to[i], to ) )
            continue;

        int packet_bytes = simulator->packet_bytes[i];
        *out_from = simulator->from[i];
        memcpy( out_packet, simulator->packet_data + (size_t) i * ROCKETNET_RECEIVE_CEILING, (size_t) packet_bytes );
        simulator->used[i] = 0;
        return packet_bytes;
    }

    return 0;
}
