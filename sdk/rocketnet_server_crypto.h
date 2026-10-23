/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_CRYPTO_H
#define ROCKETNET_SERVER_CRYPTO_H

#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"

// Server packet AEAD off the game thread. Three banks: the game fills one,
// the worker may hold one READY and one RUN, and a DONE bank waits for the
// game. The mutex covers only the bank state and the fill index. AEAD runs
// in rocketnet_server_crypto_step, outside the mutex. The game thread never
// waits: a failed try-lock or a bank that is not the next DONE bank is a
// skip, and the packet stays with the caller. A full set of banks refuses
// another claim. Nothing rewrites a READY, RUN or DONE bank.
//
// Publish runs only after every claim on the fill bank has returned. The
// server update already joins fill writers before it flushes; this handoff
// uses that same rule.
//
// Model: tla/RNServerCrypto.tla. Claim, Fill, Publish, WorkerTake,
// WorkerSeal, WorkerDone, GameTake.

#define ROCKETNET_SERVER_CRYPTO_BANKS 3
#define ROCKETNET_SERVER_CRYPTO_JOBS  32

#define ROCKETNET_SERVER_CRYPTO_SEAL          1
#define ROCKETNET_SERVER_CRYPTO_OPEN          2
#define ROCKETNET_SERVER_CRYPTO_SEAL_IN_PLACE 3

struct rocketnet_server_crypto_job_t
{
    int kind;
    int result;
    uint8_t packet_type;
    uint64_t sequence;
    uint64_t protocol_id;
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES];
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    int has_ephemeral;
    int has_session_nonce;
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    uint8_t ack[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int ack_bytes;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes;
    uint8_t plain[ROCKETNET_RECEIVE_CEILING];
    int plain_bytes;
    uint8_t * inplace;
    uint64_t tag;
};

struct rocketnet_server_crypto_t;

// manual non-zero: no thread. The test calls step. Production passes 0 and
// the worker thread runs step. Destroy joins that thread before freeing a bank.
struct rocketnet_server_crypto_t * rocketnet_server_crypto_create( void * context, int manual );

void rocketnet_server_crypto_destroy( struct rocketnet_server_crypto_t * crypto );

// A free slot in the fill bank, or NULL when every bank is busy or the fill
// bank is full or the try-lock failed. NULL means keep the packet.
struct rocketnet_server_crypto_job_t * rocketnet_server_crypto_claim( struct rocketnet_server_crypto_t * crypto );

// FILL -> READY. False when there is no fill bank, it is empty, or the
// try-lock failed. On false the fill bank stays with the caller.
bool rocketnet_server_crypto_publish( struct rocketnet_server_crypto_t * crypto );

// One worker pass. True when a bank was sealed. AEAD runs outside the mutex.
bool rocketnet_server_crypto_step( struct rocketnet_server_crypto_t * crypto );

// The next DONE bank, exclusively the caller's until release. NULL when it
// is not DONE yet or the try-lock failed. Never waits. *count is the jobs.
struct rocketnet_server_crypto_job_t * rocketnet_server_crypto_take( struct rocketnet_server_crypto_t * crypto, int * count, int * bank );

// DONE -> FREE. False when the try-lock failed; the bank stays taken.
bool rocketnet_server_crypto_release( struct rocketnet_server_crypto_t * crypto, int bank );

uint64_t rocketnet_server_crypto_busy_skips( const struct rocketnet_server_crypto_t * crypto );

#endif // #ifndef ROCKETNET_SERVER_CRYPTO_H
