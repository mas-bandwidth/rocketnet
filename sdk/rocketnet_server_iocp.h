/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/
#pragma once

#include "rocketnet_packet_batch.h"
#include "rocketnet_wire.h"

// Windows server driver. The public transport API is unchanged. Fill threads
// acquire and seal directly into driver-owned frames. Flush posts overlapped
// UDP sends; only their completions return those frames to the free pool.
// Receive workers dequeue completions in batches and publish private double
// buffers, following the existing GSO/XDP receive ownership contract.

#define ROCKETNET_IOCP_DEFAULT_WORKERS 4
#define ROCKETNET_IOCP_MAX_WORKERS 32
#define ROCKETNET_IOCP_SEND_QUEUE_SIZE 16384
#define ROCKETNET_IOCP_RECEIVES_PER_WORKER 128
#define ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE 1024

struct rocketnet_server_iocp_t;

// bind_address is updated with the actual port. IPv6 sockets accept both
// families and normalize mapped IPv4 sources. Zero workers selects the default.
// fail_stage is a constructor test control: 1 after socket, 2 after port,
// 3 after first worker, 4 after first posted receive. Production passes zero.
rocketnet_server_iocp_t * rocketnet_server_iocp_create( void * context,
    rocketnet_address_t * bind_address, int num_workers, int fail_stage = 0 );
void rocketnet_server_iocp_destroy( rocketnet_server_iocp_t * iocp );
void rocketnet_server_iocp_stop( rocketnet_server_iocp_t * iocp );
bool rocketnet_server_iocp_stopped( rocketnet_server_iocp_t * iocp );

uint8_t * rocketnet_server_iocp_start_packet( rocketnet_server_iocp_t * iocp,
    const rocketnet_address_t * to, uint64_t * packet_id );
void rocketnet_server_iocp_finish_packet( rocketnet_server_iocp_t * iocp,
    uint64_t packet_id, uint8_t * frame, int packet_bytes );
void rocketnet_server_iocp_abort_packet( rocketnet_server_iocp_t * iocp,
    uint64_t packet_id, uint8_t * frame );
void rocketnet_server_iocp_send_packets( rocketnet_server_iocp_t * iocp );
void rocketnet_server_iocp_receive_packets( rocketnet_server_iocp_t * iocp );
rocketnet_server_socket_process_packets_t * rocketnet_server_iocp_process_packets( rocketnet_server_iocp_t * iocp );

enum rocketnet_server_iocp_counter_t
{
    ROCKETNET_IOCP_SEND_COMPLETED,
    ROCKETNET_IOCP_SEND_FAILED,
    ROCKETNET_IOCP_RECEIVE_COMPLETED,
    ROCKETNET_IOCP_RECEIVE_DROPPED,
    ROCKETNET_IOCP_RECEIVE_REPOST_FAILED,
    ROCKETNET_IOCP_COMPLETION_BATCHES,
    ROCKETNET_IOCP_NUM_COUNTERS
};

void rocketnet_server_iocp_counters( rocketnet_server_iocp_t * iocp, uint64_t * counters );
int rocketnet_server_iocp_pending( rocketnet_server_iocp_t * iocp );

#if defined(ROCKETNET_IOCP_TESTS)
// Hold/release must be paired on the same thread. Do not call stop there while
// holding: Windows CRITICAL_SECTION is recursive. A different thread must stop.
void rocketnet_server_iocp_test_hold_completions( rocketnet_server_iocp_t * iocp, bool hold );
bool rocketnet_server_iocp_test_receive_view_published( rocketnet_server_iocp_t * iocp, const uint8_t * view );
bool rocketnet_server_iocp_test_stopping( rocketnet_server_iocp_t * iocp );
#endif
