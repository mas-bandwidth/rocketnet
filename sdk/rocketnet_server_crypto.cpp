/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_crypto.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <new>
#include <string.h>

// Bank states. The mutex covers these words and filling / running / next_ord /
// next_take. Job bytes move only while the bank is FILL (the game) or RUN
// (the worker), and the game reads them only while it holds a taken DONE bank.

enum
{
    ROCKETNET_CRYPTO_FREE = 0,
    ROCKETNET_CRYPTO_FILL = 1,
    ROCKETNET_CRYPTO_READY = 2,
    ROCKETNET_CRYPTO_RUN = 3,
    ROCKETNET_CRYPTO_DONE = 4
};

struct rocketnet_server_crypto_bank_t
{
    int state;
    uint64_t ordinal;
    int count;
    struct rocketnet_server_crypto_job_t jobs[ROCKETNET_SERVER_CRYPTO_JOBS];
};

struct rocketnet_server_crypto_t
{
    struct rocketnet_server_crypto_bank_t banks[ROCKETNET_SERVER_CRYPTO_BANKS];
    int filling;
    int running;
    int taken;          // bank the game holds, or -1
    uint64_t next_ord;
    uint64_t next_take;
    struct rocketnet_platform_mutex_t mutex;
    bool mutex_ok;
    struct rocketnet_platform_wake_t * wake;
    struct rocketnet_platform_thread_t * thread;
    volatile int quit;
    volatile int exited;
    int manual;
    uint64_t busy_skips;
};

static void rocketnet_server_crypto_clear_job( struct rocketnet_server_crypto_job_t * job )
{
    job->kind = 0;
    job->result = 0;
    job->packet_type = 0;
    job->sequence = 0;
    job->protocol_id = 0;
    job->has_ephemeral = 0;
    job->has_session_nonce = 0;
    job->ack_bytes = 0;
    job->packet_bytes = 0;
    job->plain_bytes = 0;
    job->inplace = NULL;
    job->tag = 0;
}

static void rocketnet_server_crypto_run_job( struct rocketnet_server_crypto_job_t * job )
{
    const uint8_t * ephemeral = job->has_ephemeral ? job->ephemeral : NULL;
    const uint8_t * session_nonce = job->has_session_nonce ? job->session_nonce : NULL;

    if ( job->kind == ROCKETNET_SERVER_CRYPTO_SEAL )
    {
        job->result = rocketnet_write_sealed_packet( job->packet, job->packet_type, job->filter,
                                                     job->sequence, session_nonce, job->plain, job->plain_bytes,
                                                     job->protocol_id, job->salt, ephemeral, job->key );
        return;
    }

    if ( job->kind == ROCKETNET_SERVER_CRYPTO_OPEN )
    {
        uint8_t * out_nonce = job->has_session_nonce ? job->session_nonce : NULL;
        job->result = rocketnet_open_sealed_packet( job->plain, out_nonce, ROCKETNET_RECEIVE_CEILING,
                                                    job->packet, job->packet_bytes, job->packet_type,
                                                    job->sequence, job->protocol_id, job->salt,
                                                    ephemeral, job->key );
        return;
    }

    if ( job->kind == ROCKETNET_SERVER_CRYPTO_SEAL_IN_PLACE )
    {
        job->result = rocketnet_write_sealed_payload_in_place( job->inplace, job->filter, job->sequence,
                                                               job->ack, job->ack_bytes, job->plain_bytes,
                                                               job->protocol_id, job->salt, job->key );
        return;
    }

    job->result = 0;
}

static void rocketnet_server_crypto_thread( void * argument )
{
    struct rocketnet_server_crypto_t * crypto = (struct rocketnet_server_crypto_t *) argument;
    while ( !crypto->quit )
    {
        if ( rocketnet_server_crypto_step( crypto ) )
            continue;
        if ( crypto->quit )
            break;
        if ( crypto->wake )
            rocketnet_platform_wake_wait( crypto->wake, false, 0.1 );
    }
    crypto->exited = 1;
}

struct rocketnet_server_crypto_t * rocketnet_server_crypto_create( void * context, int manual )
{
    struct rocketnet_server_crypto_t * crypto = new (std::nothrow) rocketnet_server_crypto_t;
    if ( !crypto )
        return NULL;
    memset( crypto, 0, sizeof( *crypto ) );
    crypto->filling = -1;
    crypto->running = -1;
    crypto->taken = -1;
    crypto->next_ord = 1;
    crypto->next_take = 1;
    crypto->manual = manual ? 1 : 0;
    for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
        crypto->banks[b].state = ROCKETNET_CRYPTO_FREE;

    if ( !rocketnet_platform_mutex_create( &crypto->mutex ) )
    {
        delete crypto;
        return NULL;
    }
    crypto->mutex_ok = true;

    if ( !crypto->manual )
    {
        crypto->wake = rocketnet_platform_wake_create( context, NULL );
        if ( !crypto->wake )
        {
            rocketnet_server_crypto_destroy( crypto );
            return NULL;
        }
        crypto->thread = rocketnet_platform_thread_create( context, rocketnet_server_crypto_thread, crypto );
        if ( !crypto->thread )
        {
            rocketnet_server_crypto_destroy( crypto );
            return NULL;
        }
    }
    return crypto;
}

void rocketnet_server_crypto_destroy( struct rocketnet_server_crypto_t * crypto )
{
    if ( !crypto )
        return;
    crypto->quit = 1;
    if ( crypto->wake )
        rocketnet_platform_wake_set( crypto->wake );
    if ( crypto->thread )
    {
        rocketnet_platform_thread_join( crypto->thread );
        rocketnet_platform_thread_destroy( crypto->thread );
        crypto->thread = NULL;
    }
    if ( crypto->wake )
    {
        rocketnet_platform_wake_destroy( crypto->wake );
        crypto->wake = NULL;
    }
    if ( crypto->mutex_ok )
    {
        rocketnet_platform_mutex_destroy( &crypto->mutex );
        crypto->mutex_ok = false;
    }
    delete crypto;
}

struct rocketnet_server_crypto_job_t * rocketnet_server_crypto_claim( struct rocketnet_server_crypto_t * crypto )
{
    rocketnet_assert( crypto );
    if ( !rocketnet_platform_mutex_try_acquire( &crypto->mutex ) )
    {
        crypto->busy_skips++;
        return NULL;
    }

    if ( crypto->filling >= 0 && crypto->banks[crypto->filling].count >= ROCKETNET_SERVER_CRYPTO_JOBS )
    {
        crypto->busy_skips++;
        rocketnet_platform_mutex_release( &crypto->mutex );
        return NULL;
    }

    if ( crypto->filling < 0 )
    {
        int bank = -1;
        for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
        {
            if ( crypto->banks[b].state == ROCKETNET_CRYPTO_FREE )
            {
                bank = b;
                break;
            }
        }
        if ( bank < 0 )
        {
            crypto->busy_skips++;
            rocketnet_platform_mutex_release( &crypto->mutex );
            return NULL;
        }
        crypto->banks[bank].state = ROCKETNET_CRYPTO_FILL;
        crypto->banks[bank].count = 0;
        crypto->banks[bank].ordinal = 0;
        crypto->filling = bank;
    }

    struct rocketnet_server_crypto_bank_t * fill = &crypto->banks[crypto->filling];
    struct rocketnet_server_crypto_job_t * job = &fill->jobs[fill->count];
    fill->count++;
    rocketnet_platform_mutex_release( &crypto->mutex );
    rocketnet_server_crypto_clear_job( job );
    return job;
}

bool rocketnet_server_crypto_publish( struct rocketnet_server_crypto_t * crypto )
{
    rocketnet_assert( crypto );
    if ( !rocketnet_platform_mutex_try_acquire( &crypto->mutex ) )
    {
        crypto->busy_skips++;
        return false;
    }
    if ( crypto->filling < 0 || crypto->banks[crypto->filling].count <= 0 )
    {
        rocketnet_platform_mutex_release( &crypto->mutex );
        return false;
    }
    struct rocketnet_server_crypto_bank_t * fill = &crypto->banks[crypto->filling];
    fill->ordinal = crypto->next_ord;
    crypto->next_ord++;
    fill->state = ROCKETNET_CRYPTO_READY;
    crypto->filling = -1;
    rocketnet_platform_mutex_release( &crypto->mutex );
    if ( crypto->wake )
        rocketnet_platform_wake_set( crypto->wake );
    return true;
}

bool rocketnet_server_crypto_step( struct rocketnet_server_crypto_t * crypto )
{
    rocketnet_assert( crypto );
    rocketnet_platform_mutex_acquire( &crypto->mutex );
    int bank = -1;
    for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
    {
        if ( crypto->banks[b].state != ROCKETNET_CRYPTO_READY )
            continue;
        if ( bank < 0 || crypto->banks[b].ordinal < crypto->banks[bank].ordinal )
            bank = b;
    }
    if ( bank < 0 || crypto->quit )
    {
        rocketnet_platform_mutex_release( &crypto->mutex );
        return false;
    }
    crypto->banks[bank].state = ROCKETNET_CRYPTO_RUN;
    crypto->running = bank;
    int count = crypto->banks[bank].count;
    rocketnet_platform_mutex_release( &crypto->mutex );

    for ( int i = 0; i < count; i++ )
        rocketnet_server_crypto_run_job( &crypto->banks[bank].jobs[i] );

    rocketnet_platform_mutex_acquire( &crypto->mutex );
    crypto->banks[bank].state = ROCKETNET_CRYPTO_DONE;
    crypto->running = -1;
    rocketnet_platform_mutex_release( &crypto->mutex );
    return true;
}

struct rocketnet_server_crypto_job_t * rocketnet_server_crypto_take( struct rocketnet_server_crypto_t * crypto, int * count, int * bank_out )
{
    rocketnet_assert( crypto );
    rocketnet_assert( count );
    rocketnet_assert( bank_out );
    if ( !rocketnet_platform_mutex_try_acquire( &crypto->mutex ) )
    {
        crypto->busy_skips++;
        return NULL;
    }
    if ( crypto->taken >= 0 )
    {
        rocketnet_platform_mutex_release( &crypto->mutex );
        return NULL;
    }
    int bank = -1;
    for ( int b = 0; b < ROCKETNET_SERVER_CRYPTO_BANKS; b++ )
    {
        if ( crypto->banks[b].state == ROCKETNET_CRYPTO_DONE && crypto->banks[b].ordinal == crypto->next_take )
        {
            bank = b;
            break;
        }
    }
    if ( bank < 0 )
    {
        rocketnet_platform_mutex_release( &crypto->mutex );
        return NULL;
    }
    crypto->taken = bank;
    *count = crypto->banks[bank].count;
    *bank_out = bank;
    rocketnet_platform_mutex_release( &crypto->mutex );
    return crypto->banks[bank].jobs;
}

bool rocketnet_server_crypto_release( struct rocketnet_server_crypto_t * crypto, int bank )
{
    rocketnet_assert( crypto );
    rocketnet_assert( bank >= 0 && bank < ROCKETNET_SERVER_CRYPTO_BANKS );
    if ( !rocketnet_platform_mutex_try_acquire( &crypto->mutex ) )
    {
        crypto->busy_skips++;
        return false;
    }
    if ( crypto->taken != bank || crypto->banks[bank].state != ROCKETNET_CRYPTO_DONE || crypto->banks[bank].ordinal != crypto->next_take )
    {
        rocketnet_platform_mutex_release( &crypto->mutex );
        return false;
    }
    crypto->banks[bank].state = ROCKETNET_CRYPTO_FREE;
    crypto->banks[bank].count = 0;
    crypto->banks[bank].ordinal = 0;
    crypto->taken = -1;
    crypto->next_take++;
    rocketnet_platform_mutex_release( &crypto->mutex );
    return true;
}

uint64_t rocketnet_server_crypto_busy_skips( const struct rocketnet_server_crypto_t * crypto )
{
    rocketnet_assert( crypto );
    return crypto->busy_skips;
}
