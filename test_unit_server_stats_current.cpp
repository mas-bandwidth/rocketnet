/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include <string.h>
#include <math.h>

// The server keeps per-slot statistics current as acks land, instead of
// recomputing them from the full histories on a round-robin phase sweep.
// This pins that a slot whose phase is far from the current one shows the new
// rtt, loss and rates immediately after one ack arrives -- the published
// numbers cost nothing per idle slot because they are maintained incrementally
// in O(1) per acked or received packet.

static void test_server_stats_current_after_one_update()
{
    const int num_clients = 1000;
    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, num_clients );
    rocketnet_unit_check( acks );

    // a slot whose modulo phase is far from the first update's phase
    const int slot = 5;
    struct rocketnet_ack_endpoint_t * ep = &acks->endpoints[slot];

    const double t0 = 1000.0;
    uint64_t acked_ids[16];
    int acked_count = 0;
    uint64_t unknown = 0;
    uint64_t batch_full = 0;

    // two packets sent half a second apart, both acked one second after the
    // first was sent: known RTT samples of 1000 ms and 500 ms
    rocketnet_ack_endpoint_record_sent( ep, 1, 100, 7, 1, 0, t0 );
    rocketnet_ack_endpoint_record_sent( ep, 2, 100, 8, 1, 0, t0 + 0.5 );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = 2;
    block.first_range_length = 2;

    int n = rocketnet_ack_endpoint_process_block( ep, &block, t0 + 1.0,
                                                  acked_ids, 16, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( n == 2 );
    rocketnet_unit_check( acked_count == 2 &&
                         ( ( acked_ids[0] == 7 && acked_ids[1] == 8 ) ||
                           ( acked_ids[0] == 8 && acked_ids[1] == 7 ) ) );

    // the published rtt is current immediately, with no phase sweep: the
    // minimum of the two samples is 500 ms
    rocketnet_unit_check( ep->rtt > 450.0f && ep->rtt < 550.0f );
    rocketnet_unit_check( ep->jitter >= 0.0f );
    rocketnet_unit_check( ep->packet_loss == 0.0f );      // every sent packet was acked
    rocketnet_unit_check( ep->bandwidth_sent_kbps > 0.0f );
    rocketnet_unit_check( ep->bandwidth_acked_kbps > 0.0f );

    // the running aggregates match the reference recompute after a longer
    // scripted history spanning the older-half window boundary

    rocketnet_ack_endpoint_reset( ep );

    const int M = 3000;
    for ( int s = 1; s <= M; s++ )
        rocketnet_ack_endpoint_record_sent( ep, (uint64_t) s, 100, (uint64_t) s, 1, 0, t0 + (double) s * 0.001 );

    // ack the first half of the history in one block
    uint64_t acked_ids2[3000];
    acked_count = 0;
    unknown = 0;
    batch_full = 0;
    memset( &block, 0, sizeof(block) );
    block.largest = M / 2;
    block.first_range_length = M / 2;
    n = rocketnet_ack_endpoint_process_block( ep, &block, t0 + (double) M * 0.002,
                                              acked_ids2, 3000, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( n == M / 2 );

    // capture the incrementally-published values, then ask the reference
    // recompute for its answer to the same history
    const float inc_rtt = ep->rtt;
    const float inc_jitter = ep->jitter;
    const float inc_loss = ep->packet_loss;
    const float inc_sent = ep->bandwidth_sent_kbps;
    const float inc_acked = ep->bandwidth_acked_kbps;

    rocketnet_ack_endpoint_update_stats( ep );

    rocketnet_unit_check( fabs( inc_rtt - ep->rtt ) < 0.5f );
    rocketnet_unit_check( fabs( inc_jitter - ep->jitter ) < 0.5f );
    rocketnet_unit_check( fabs( inc_loss - ep->packet_loss ) < 0.5f );
    rocketnet_unit_check( fabs( inc_sent - ep->bandwidth_sent_kbps ) < 1.0f );
    rocketnet_unit_check( fabs( inc_acked - ep->bandwidth_acked_kbps ) < 1.0f );

    rocketnet_server_acks_destroy( acks );
}

REGISTER_UNIT_TEST( test_server_stats_current_after_one_update );
