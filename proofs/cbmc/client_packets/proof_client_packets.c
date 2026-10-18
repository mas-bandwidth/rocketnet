/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the client's packet readers, for packets from the server:
    the dispatcher rocketnet_client_protocol_process_packet, the per-type
    readers behind it (cookie, challenge, denied, the shared session open,
    keep-alive, payload, ack, fragment, disconnect) and
    rocketnet_client_protocol_process_record.

    The readers are sdk/rocketnet_client_protocol.cpp, included below with
    the sdk code they call: rocketnet_protocol.cpp (prefilter, sealed open,
    sequence reconstruction), rocketnet_keys.cpp (key derivation) and
    rocketnet_replay.cpp (replay check). Each harness checks no out-of-bounds read or
    write, no integer overflow and no division by zero on any packet of
    0..ROCKETNET_MAX_PACKET_BYTES bytes, over a buffer object of exactly
    packet_bytes symbolic bytes, and that every malformed packet is refused.

    Models, not proved here (each checks its own buffer arguments):
    - libsodium: every call is a model that checks __CPROVER_r_ok on what it
      reads and __CPROVER_w_ok on what it writes at the lengths libsodium
      documents, writes nondet bytes, and returns nondet success or failure.
    - rocketnet_replay_advance: its precondition (a nonzero sequence not
      already received) is asserted, then the window is havocked and the
      high-water mark moves to the larger of the two. The real advance runs
      a clear loop of up to 4095 steps (proved separately for deltas 1..3:
      replay-delta-1-3).
    - the four rocketnet_ack.cpp calls the readers make. rocketnet_ack.cpp
      is C++ (a template and std::initializer_list through
      rocketnet_memory.h), so a C proof cannot include it.
      rocketnet_ack_block_read asserts it may read data for data_bytes and
      returns 0 or a block of 7 + 4 * num_ranges bytes inside data_bytes,
      with the validity rules the code documents (its own card proves the
      reader). rocketnet_ack_endpoint_process_block asserts the block it is
      given is one the read accepted and that the acked id batch is
      writable. rocketnet_ack_endpoint_record_received and
      rocketnet_ack_endpoint_reset assert their endpoint is writable.
    - rocketnet_reassembly_process_fragment: asserts its type data is
      readable for type_data_bytes, at least the header and one byte; the
      reassembly itself is proved by its own card.
    - the logging calls do nothing.

    rocketnet_assert is live (ROCKETNET_ASSERTS 1): every assert the code
    makes on these paths is a property.

    The client struct comes from rocketnet_client_protocol_init, then every
    field a reader branches on is made symbolic: the state over all twelve
    client states, the replay windows and high-water marks, the send
    sequences, the integrity failure count, the cookie flag, the rtt and
    ack-debt fields, and the I/O verdict of the record being consumed.
    Sequences stay below 2^63 (an attempt starts its sequences at one).

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 64. The longest loop on these paths is the 32-byte key
    write in the associated data writer.
    Solver: --arrays-uf-always. Without it the payload, record and
    dispatcher harnesses exhaust 30 GB of memory on the symbolic-length
    copies out of the 1472-byte opened buffer.

    Reversed witnesses: built with -DPROOF_WITNESS, every harness must fail
    exactly its witness assertions (proof.mk checks it), so each accepting
    path is reachable and no harness passes vacuously.
*/

#define thread_local
#define ROCKETNET_ASSERTS 1

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "rocketnet_replay.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"

#include "rocketnet_protocol.cpp"
#include "rocketnet_keys.cpp"

#define rocketnet_replay_advance rocketnet_replay_advance_not_in_this_proof
#include "rocketnet_replay.cpp"
#undef rocketnet_replay_advance

#include "rocketnet_client_protocol.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
static unsigned long long nondet_uint64( void ) { return 0; }
static double nondet_double( void ) { return 0.0; }
#define PROOF_R_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_W_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_HAVOC( p, n ) ( memset( (p), 0, (size_t) (n) ) )
#else
int nondet_int( void );
unsigned char nondet_uchar( void );
unsigned long long nondet_uint64( void );
double nondet_double( void );
#define PROOF_R_OK( p, n ) __CPROVER_r_ok( (p), (size_t) (n) )
#define PROOF_W_OK( p, n ) __CPROVER_w_ok( (p), (size_t) (n) )
#define PROOF_HAVOC( p, n ) __CPROVER_havoc_slice( (p), (size_t) (n) )
#endif

#define PROOF_SEQUENCE_LIMIT ( 1ULL << 63 )

/* reversed witnesses: built with -DPROOF_WITNESS, each harness asserts that
   its accepting path never runs, and CBMC must refute exactly those
   assertions, so no proof above passes because its path is unreachable */

#ifdef PROOF_WITNESS
#define PROOF_REACHED( c, m ) __CPROVER_assert( !( c ), "witness: " m )
#else
#define PROOF_REACHED( c, m ) ( (void) 0 )
#endif

/* rocketnet_assert: a failed assert is a failed property */

static void proof_assert_function( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    __CPROVER_assert( 0, "rocketnet_assert holds" );
    __CPROVER_assume( 0 );
}

void ( * rocketnet_assert_function_pointer )( const char * condition, const char * function, const char * file, int line ) = proof_assert_function;

/* logging: the readers' log calls do nothing */

void rocketnet_info( const char * format, ... ) { (void) format; }
void rocketnet_debug( const char * format, ... ) { (void) format; }
void rocketnet_spam( const char * format, ... ) { (void) format; }
void rocketnet_warn( const char * format, ... ) { (void) format; }
void rocketnet_error( const char * format, ... ) { (void) format; }

/* libsodium models */

int crypto_shorthash_siphash24( unsigned char * out, const unsigned char * in, unsigned long long inlen, const unsigned char * k )
{
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "siphash reads its input" );
    __CPROVER_assert( PROOF_R_OK( k, 16 ), "siphash reads its 16-byte key" );
    __CPROVER_assert( PROOF_W_OK( out, 8 ), "siphash writes 8 bytes" );
    PROOF_HAVOC( out, 8 );
    return 0;
}

void sodium_memzero( void * const pnt, const size_t len )
{
    __CPROVER_assert( PROOF_W_OK( pnt, len ), "memzero writes its buffer" );
    PROOF_HAVOC( pnt, len );
}

int sodium_memcmp( const void * const b1_, const void * const b2_, size_t len )
{
    __CPROVER_assert( PROOF_R_OK( b1_, len ), "memcmp reads its first buffer" );
    __CPROVER_assert( PROOF_R_OK( b2_, len ), "memcmp reads its second buffer" );
    int result = nondet_int();
    __CPROVER_assume( result == 0 || result == -1 );
    return result;
}

int crypto_aead_chacha20poly1305_ietf_encrypt( unsigned char * c, unsigned long long * clen_p,
                                               const unsigned char * m, unsigned long long mlen,
                                               const unsigned char * ad, unsigned long long adlen,
                                               const unsigned char * nsec, const unsigned char * npub,
                                               const unsigned char * k )
{
    (void) nsec;
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "encrypt reads the plaintext" );
    __CPROVER_assert( PROOF_R_OK( ad, adlen ), "encrypt reads the associated data" );
    __CPROVER_assert( PROOF_R_OK( npub, 12 ), "encrypt reads the 12-byte nonce" );
    __CPROVER_assert( PROOF_R_OK( k, 32 ), "encrypt reads the 32-byte key" );
    __CPROVER_assert( PROOF_W_OK( c, mlen + 16 ), "encrypt writes the ciphertext and tag" );
    __CPROVER_assert( PROOF_W_OK( clen_p, 8 ), "encrypt writes the ciphertext length" );
    PROOF_HAVOC( c, mlen + 16 );
    *clen_p = mlen + 16;
    return 0;
}

int crypto_aead_chacha20poly1305_ietf_decrypt( unsigned char * m, unsigned long long * mlen_p,
                                               unsigned char * nsec,
                                               const unsigned char * c, unsigned long long clen,
                                               const unsigned char * ad, unsigned long long adlen,
                                               const unsigned char * npub, const unsigned char * k )
{
    (void) nsec;
    __CPROVER_assert( clen >= 16, "decrypt input holds the 16-byte tag" );
    __CPROVER_assert( PROOF_R_OK( c, clen ), "decrypt reads the ciphertext and tag" );
    __CPROVER_assert( PROOF_R_OK( ad, adlen ), "decrypt reads the associated data" );
    __CPROVER_assert( PROOF_R_OK( npub, 12 ), "decrypt reads the 12-byte nonce" );
    __CPROVER_assert( PROOF_R_OK( k, 32 ), "decrypt reads the 32-byte key" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    if ( ok != 0 )
        return -1;

    const unsigned long long plaintext = clen - 16;
    __CPROVER_assert( PROOF_W_OK( m, plaintext ), "decrypt writes the plaintext inside the output" );
    __CPROVER_assert( PROOF_W_OK( mlen_p, 8 ), "decrypt writes the plaintext length" );
    PROOF_HAVOC( m, plaintext );
    *mlen_p = plaintext;
    return 0;
}

int crypto_generichash( unsigned char * out, size_t outlen, const unsigned char * in, unsigned long long inlen,
                        const unsigned char * key, size_t keylen )
{
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "generichash reads its input" );
    __CPROVER_assert( PROOF_R_OK( key, keylen ), "generichash reads its key" );
    __CPROVER_assert( PROOF_W_OK( out, outlen ), "generichash writes its output" );
    PROOF_HAVOC( out, outlen );
    return 0;
}

int crypto_scalarmult( unsigned char * q, const unsigned char * n, const unsigned char * p )
{
    __CPROVER_assert( PROOF_R_OK( n, 32 ), "scalarmult reads the 32-byte scalar" );
    __CPROVER_assert( PROOF_R_OK( p, 32 ), "scalarmult reads the 32-byte point" );
    __CPROVER_assert( PROOF_W_OK( q, 32 ), "scalarmult writes 32 bytes" );
    PROOF_HAVOC( q, 32 );
    int result = nondet_int();
    __CPROVER_assume( result == 0 || result == -1 );
    return result;
}

/* models of the sdk code proved elsewhere */

void rocketnet_replay_advance( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    __CPROVER_assert( PROOF_W_OK( replay, sizeof(*replay) ), "advance writes its replay struct" );
    __CPROVER_assert( sequence != 0, "advance is given a nonzero sequence" );
    __CPROVER_assert( !rocketnet_replay_already_received( replay, sequence ), "advance is given a sequence not yet received" );
    PROOF_HAVOC( replay->window, sizeof(replay->window) );
    if ( sequence > replay->highest_opened )
        replay->highest_opened = sequence;
}

void rocketnet_ack_endpoint_reset( struct rocketnet_ack_endpoint_t * endpoint )
{
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof(*endpoint) ), "reset writes its endpoint" );
}

int rocketnet_ack_block_read( struct rocketnet_ack_block_t * block, const uint8_t * data, int data_bytes, uint64_t highest_sent )
{
    __CPROVER_assert( PROOF_W_OK( block, sizeof(*block) ), "block read writes its block" );
    __CPROVER_assert( data_bytes >= 0, "block read is given a length that is not negative" );
    __CPROVER_assert( PROOF_R_OK( data, data_bytes ), "block read may read only the opened plaintext" );

    if ( data_bytes < ROCKETNET_ACK_BLOCK_MIN_BYTES || nondet_int() )
        return 0;

    const int num_ranges = nondet_int();
    __CPROVER_assume( num_ranges >= 0 && num_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES );
    const int block_bytes = ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * num_ranges;
    if ( block_bytes > data_bytes )
        return 0;

    const uint64_t largest = nondet_uint64();
    const uint32_t first_range_length = (uint32_t) nondet_int();
    __CPROVER_assume( first_range_length <= 0xFFFF );
    if ( largest == 0 )
        __CPROVER_assume( first_range_length == 0 && num_ranges == 0 );
    else
        __CPROVER_assume( first_range_length >= 1 && first_range_length <= largest );
    (void) highest_sent;

    block->largest = largest;
    block->first_range_length = first_range_length;
    block->num_ranges = num_ranges;
    return block_bytes;
}

void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time )
{
    (void) time;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof(*endpoint) ), "record received writes its endpoint" );
    __CPROVER_assert( sequence != 0, "record received is given a nonzero sequence" );
    __CPROVER_assert( wire_bytes >= 0, "record received is given a wire size that is not negative" );
}

int rocketnet_ack_endpoint_process_block( struct rocketnet_ack_endpoint_t * endpoint,
                                          const struct rocketnet_ack_block_t * block,
                                          double time,
                                          uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                          uint64_t * unknown_sequence_counter,
                                          uint64_t * batch_full_counter )
{
    (void) time;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof(*endpoint) ), "process block writes its endpoint" );
    __CPROVER_assert( PROOF_R_OK( block, sizeof(*block) ), "process block reads its block" );
    __CPROVER_assert( block->num_ranges >= 0 && block->num_ranges <= ROCKETNET_ACK_BLOCK_MAX_RANGES,
        "process block is given a block whose range count the read bounded" );
    __CPROVER_assert( block->largest == 0 || ( block->first_range_length >= 1 && block->first_range_length <= block->largest ),
        "process block is given a first range the read validated" );
    __CPROVER_assert( acked_capacity >= 0, "process block capacity is not negative" );
    __CPROVER_assert( PROOF_W_OK( acked_packet_ids, (size_t) acked_capacity * sizeof(uint64_t) ), "process block can write its acked batch" );
    __CPROVER_assert( PROOF_W_OK( acked_count, sizeof(int) ), "process block writes its acked count" );
    __CPROVER_assert( PROOF_W_OK( unknown_sequence_counter, sizeof(uint64_t) ), "process block writes its unknown counter" );
    __CPROVER_assert( PROOF_W_OK( batch_full_counter, sizeof(uint64_t) ), "process block writes its batch full counter" );
    const int count = nondet_int();
    __CPROVER_assume( count >= *acked_count && count <= acked_capacity );
    const int newly = count - *acked_count;
    *acked_count = count;
    return newly;
}

int rocketnet_reassembly_process_fragment( struct rocketnet_reassembly_set_t * set,
                                           int connection_index,
                                           const uint8_t * type_data,
                                           int type_data_bytes,
                                           uint64_t * counters,
                                           const struct rocketnet_reassembly_counters_t * table )
{
    __CPROVER_assert( set != NULL, "reassembly is given its set" );
    __CPROVER_assert( connection_index == 0, "the client reassembles on connection 0" );
    __CPROVER_assert( type_data_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1, "reassembly is given the header and a byte" );
    __CPROVER_assert( PROOF_R_OK( type_data, type_data_bytes ), "reassembly reads only the fragment's type data" );
    __CPROVER_assert( PROOF_R_OK( table, sizeof(*table) ), "reassembly reads its counter table" );
    __CPROVER_assert( counters != NULL, "reassembly is given its counters" );
    int result = nondet_int();
    __CPROVER_assume( result == 0 || result == 1 );
    return result;
}

/* the client, from its own create path, then every field a reader branches
   on made symbolic */

static struct rocketnet_counters_t proof_counters;
static struct rocketnet_client_protocol_t proof_client;
static struct rocketnet_reassembly_set_t proof_reassembly;

static uint64_t proof_sequence( uint64_t low )
{
    uint64_t value = nondet_uint64();
    __CPROVER_assume( value >= low && value < PROOF_SEQUENCE_LIMIT );
    return value;
}

static struct rocketnet_client_protocol_t * proof_make_client( int any_verdict )
{
    struct rocketnet_client_protocol_t * client = &proof_client;

    rocketnet_client_protocol_init( client, &proof_counters );

    int state = nondet_int();
    __CPROVER_assume( state >= ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED &&
                      state <= ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    client->state = state;

    client->replay_handshake.highest_opened = proof_sequence( 0 );
    client->replay_session.highest_opened = proof_sequence( 0 );
    PROOF_HAVOC( client->replay_handshake.window, sizeof(client->replay_handshake.window) );
    PROOF_HAVOC( client->replay_session.window, sizeof(client->replay_session.window) );

    client->send_sequence_handshake = proof_sequence( 1 );
    client->send_sequence_session = proof_sequence( 1 );

    client->integrity_failures = nondet_uint64();
    __CPROVER_assume( client->integrity_failures < ROCKETNET_INTEGRITY_LIMIT );

    client->has_cookie = nondet_int() & 1;
    client->state_first_send_time = nondet_double();
    client->rtt_estimate = nondet_double();
    __CPROVER_assume( client->rtt_estimate >= 0.0 );
    client->rtt_session_sampled = nondet_int() & 1;
    client->last_receive_time = nondet_double();
    client->client_index = nondet_int();
    client->max_clients = nondet_int();

    client->ack.ack_debt = nondet_uint64();
    __CPROVER_assume( client->ack.ack_debt < PROOF_SEQUENCE_LIMIT );
    client->ack.highest_received_sequence = proof_sequence( 0 );
    client->num_acked = nondet_int();
    __CPROVER_assume( client->num_acked >= 0 && client->num_acked <= ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    client->reassembly = &proof_reassembly;

    if ( any_verdict )
    {
        uint8_t verdict = nondet_uchar();
        __CPROVER_assume( verdict <= ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE );
        client->record_verdict = verdict;
        client->record_sequence = nondet_uint64();
        /* the I/O thread's contract (sdk/rocketnet_client_io.h,
           rocketnet_client_io_decrypt_datagram): an opened record carries
           the nonzero sequence it opened at */
        __CPROVER_assume( verdict != ROCKETNET_CLIENT_IO_VERDICT_OPENED || client->record_sequence != 0 );
    }

    return client;
}

static uint8_t * proof_packet( int * out_packet_bytes )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_MAX_PACKET_BYTES );
    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );
    *out_packet_bytes = packet_bytes;
    return packet;
}

static int proof_is_session_state( int state )
{
    return state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE ||
           state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
}

/* cookie: accepted only at exactly 38 bytes in requesting cookie or sending
   connection request; reads only inside the packet */

void harness_process_cookie( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_cookie( client, packet, packet_bytes, 1000.0 );

    __CPROVER_assert( result == 0 || result == 1, "cookie returns 0 or 1" );
    if ( packet_bytes != ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES )
        __CPROVER_assert( result == 0, "a cookie of the wrong size is refused" );
    if ( state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE &&
         state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        __CPROVER_assert( result == 0 && client->state == state_before, "a cookie in the wrong state is refused and changes nothing" );
    if ( result == 1 )
        __CPROVER_assert( client->has_cookie == 1 &&
                          client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST,
                          "an accepted cookie is stored and the client sends requests" );
    PROOF_REACHED( result == 1, "a cookie is accepted" );
}

/* challenge: accepted only at exactly 66 bytes in sending connection request */

void harness_process_challenge( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_challenge( client, packet, packet_bytes, 1000.0 );

    __CPROVER_assert( result == 0 || result == 1, "challenge returns 0 or 1" );
    if ( packet_bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
        __CPROVER_assert( result == 0, "a challenge of the wrong size is refused" );
    if ( state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST )
        __CPROVER_assert( result == 0 && client->state == state_before, "a challenge in the wrong state is refused and changes nothing" );
    if ( result == 1 )
        __CPROVER_assert( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE,
                          "an accepted challenge moves the client to sending connection response" );
    PROOF_REACHED( result == 1, "a challenge is accepted" );
}

/* denied: accepted only at exactly 34 bytes in sending connection request or
   response */

void harness_process_denied( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_denied( client, packet, packet_bytes, 1000.0 );

    __CPROVER_assert( result == 0 || result == 1, "denied returns 0 or 1" );
    if ( packet_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
        __CPROVER_assert( result == 0, "a denied of the wrong size is refused" );
    if ( state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST &&
         state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
        __CPROVER_assert( result == 0 && client->state == state_before, "a denied in the wrong state is refused and changes nothing" );
    if ( result == 1 )
        __CPROVER_assert( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,
                          "an accepted denied ends the attempt as denied" );
    PROOF_REACHED( result == 1, "a denied is accepted" );
}

/* the shared session open, for every session type's floor and size rule */

void harness_open_session_packet( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 0 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    static const struct rocketnet_client_session_counters_t type_counters = {
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_STATE,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_SIZE,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_ZERO_SEQUENCE, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_DID_NOT_OPEN, ROCKETNET_COUNTER_CLIENT_PAYLOAD_BAD_ACK_BLOCK,
        ROCKETNET_COUNTER_CLIENT_PAYLOAD_WRONG_OPENED_SIZE };

    uint8_t packet_type;
    int min_wire_bytes;
    int expected_type_data_bytes;
    rocketnet_client_type_data_validator_t validator = NULL;

    switch ( nondet_int() )
    {
        case 0:
            packet_type = ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET;
            min_wire_bytes = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
            expected_type_data_bytes = 8;
            validator = rocketnet_client_validate_keep_alive;
            break;
        case 1:
            packet_type = ROCKETNET_CONNECTION_DISCONNECT_PACKET;
            min_wire_bytes = ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
            expected_type_data_bytes = 0;
            break;
        case 2:
            packet_type = ROCKETNET_CONNECTION_ACK_PACKET;
            min_wire_bytes = ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;
            expected_type_data_bytes = 0;
            break;
        case 3:
            packet_type = ROCKETNET_CONNECTION_FRAGMENT_PACKET;
            min_wire_bytes = ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES;
            expected_type_data_bytes = -( ROCKETNET_FRAGMENT_HEADER_BYTES + 1 );
            break;
        default:
            packet_type = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
            min_wire_bytes = ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
            expected_type_data_bytes = -1;
            break;
    }

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int type_data_offset = -1;

    const int type_data_bytes = rocketnet_client_open_session_packet( client, packet_type, &type_counters,
                                                                      min_wire_bytes, expected_type_data_bytes,
                                                                      packet, packet_bytes, 1000.0,
                                                                      opened, &type_data_offset, validator );

    __CPROVER_assert( type_data_bytes >= -1, "the open returns -1 or a length" );
    if ( packet_bytes < min_wire_bytes )
        __CPROVER_assert( type_data_bytes == -1, "a session packet below its floor is refused" );
    if ( !proof_is_session_state( state_before ) )
        __CPROVER_assert( type_data_bytes == -1 && client->state == state_before, "a session packet outside the session states is refused" );
    if ( type_data_bytes >= 0 )
    {
        __CPROVER_assert( type_data_offset >= ROCKETNET_ACK_BLOCK_MIN_BYTES, "the type data follows at least the minimum ack block" );
        __CPROVER_assert( type_data_offset + type_data_bytes == packet_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES,
                          "the block and the type data fill the opened plaintext exactly" );
        if ( expected_type_data_bytes >= 0 )
            __CPROVER_assert( type_data_bytes == expected_type_data_bytes, "a fixed-size type is accepted only at its size" );
        else
            __CPROVER_assert( type_data_bytes >= -expected_type_data_bytes, "a ranged type is accepted only above its floor" );
    }
    PROOF_REACHED( type_data_bytes >= 0, "a session packet opens" );
}

/* the session types, each through its own reader, on every client state and
   every I/O verdict */

static void proof_session_reader_asserts( int result, int packet_bytes, int min_wire_bytes, int state_before,
                                          const struct rocketnet_client_protocol_t * client )
{
    __CPROVER_assert( result == 0 || result == 1, "a session reader returns 0 or 1" );
    if ( packet_bytes < min_wire_bytes )
        __CPROVER_assert( result == 0, "a session packet below its floor is refused" );
    if ( !proof_is_session_state( state_before ) )
        __CPROVER_assert( result == 0 && client->state == state_before, "a session packet outside the session states is refused" );
}

void harness_process_keep_alive( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_keep_alive( client, packet, packet_bytes, 1000.0 );

    proof_session_reader_asserts( result, packet_bytes, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES, state_before, client );
    if ( result == 1 && state_before == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE )
        __CPROVER_assert( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                          client->client_index >= 0 && client->client_index < client->max_clients &&
                          client->max_clients <= ROCKETNET_MAX_CLIENTS,
                          "the first keep-alive connects with a valid slot" );
    PROOF_REACHED( result == 1, "a keep-alive is accepted" );
}

void harness_process_payload( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    uint8_t * out_payload = (uint8_t *) malloc( ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES );
    __CPROVER_assume( out_payload != NULL );
    int out_payload_bytes = 0;

    const int result = rocketnet_client_process_payload( client, packet, packet_bytes, 1000.0, out_payload, &out_payload_bytes );

    proof_session_reader_asserts( result, packet_bytes, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES, state_before, client );
    if ( result == 0 )
        __CPROVER_assert( out_payload_bytes == 0, "a refused payload delivers nothing" );
    else
        __CPROVER_assert( out_payload_bytes >= 1 &&
                          out_payload_bytes <= packet_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES &&
                          out_payload_bytes <= ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES,
                          "a delivered payload is inside the packet and the output" );
    PROOF_REACHED( result == 1 && out_payload_bytes == ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES, "a payload of the largest size is delivered" );
}

void harness_process_ack( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_ack( client, packet, packet_bytes, 1000.0 );

    proof_session_reader_asserts( result, packet_bytes, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES, state_before, client );
    PROOF_REACHED( result == 1, "a standalone ack is accepted" );
}

void harness_process_fragment( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_fragment( client, packet, packet_bytes, 1000.0 );

    proof_session_reader_asserts( result, packet_bytes, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES, state_before, client );
    if ( state_before != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        __CPROVER_assert( result == 0, "a fragment outside connected is refused" );
    PROOF_REACHED( result == 1, "a fragment is accepted" );
}

void harness_process_disconnect( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 1 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    const int result = rocketnet_client_process_disconnect( client, packet, packet_bytes, 1000.0 );

    proof_session_reader_asserts( result, packet_bytes, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES, state_before, client );
    if ( result == 1 )
        __CPROVER_assert( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED, "an accepted disconnect disconnects" );
    PROOF_REACHED( result == 1, "a disconnect is accepted" );
}

/* the dispatcher and the record consumer: any packet, any output capacity */

static void proof_dispatch_asserts( int result, const uint8_t * packet, int packet_bytes,
                                    int out_payload_capacity, int out_payload_bytes,
                                    int state_before, const struct rocketnet_client_protocol_t * client )
{
    __CPROVER_assert( result == 0 || result == 1, "the dispatcher returns 0 or 1" );
    __CPROVER_assert( out_payload_bytes >= 0 && out_payload_bytes <= out_payload_capacity,
                      "the payload length is inside the output capacity" );
    __CPROVER_assert( out_payload_bytes == 0 || result == 1, "only an accepted packet delivers a payload" );

    if ( out_payload_capacity < ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES )
    {
        __CPROVER_assert( result == 0 && out_payload_bytes == 0 && client->state == state_before,
                          "a small output is refused before the packet is read" );
        return;
    }

    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
    {
        __CPROVER_assert( result == 0, "a packet shorter than the prefilter header is refused" );
        return;
    }

    if ( packet[1] != ROCKETNET_FILTER_EPOCH_UNKEYED ||
         packet[2] != client->filter_value[0] || packet[3] != client->filter_value[1] ||
         packet[4] != client->filter_value[2] || packet[5] != client->filter_value[3] )
    {
        __CPROVER_assert( result == 0, "a packet failing the prefilter is refused" );
        return;
    }

    const uint8_t packet_type = packet[0];

    if ( packet_type > ROCKETNET_CONNECTION_FRAGMENT_PACKET ||
         packet_type == ROCKETNET_CONNECTION_REQUEST_PACKET ||
         packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
    {
        __CPROVER_assert( result == 0, "a reserved or client-only type is refused" );
        return;
    }

    if ( out_payload_bytes > 0 )
        __CPROVER_assert( packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET &&
                          out_payload_bytes <= packet_bytes - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES,
                          "only a payload packet delivers, and inside its own bytes" );
}

void harness_process_packet( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 0 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    int out_payload_capacity = nondet_int();
    __CPROVER_assume( out_payload_capacity >= 0 && out_payload_capacity <= ROCKETNET_MAX_PACKET_BYTES );
    uint8_t * out_payload = (uint8_t *) malloc( (size_t) out_payload_capacity );
    __CPROVER_assume( out_payload != NULL );
    int out_payload_bytes = -1;

    const int result = rocketnet_client_protocol_process_packet( client, packet, packet_bytes, 1000.0,
                                                                 out_payload, out_payload_capacity, &out_payload_bytes );

    proof_dispatch_asserts( result, packet, packet_bytes, out_payload_capacity, out_payload_bytes, state_before, client );
    PROOF_REACHED( result == 1 && out_payload_bytes > 0, "the dispatcher delivers a payload" );
}

void harness_process_record( void )
{
    struct rocketnet_client_protocol_t * client = proof_make_client( 0 );
    int packet_bytes;
    uint8_t * packet = proof_packet( &packet_bytes );
    const int state_before = client->state;

    uint8_t verdict = nondet_uchar();
    __CPROVER_assume( verdict <= ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE );
    const uint64_t sequence = nondet_uint64();
    __CPROVER_assume( verdict != ROCKETNET_CLIENT_IO_VERDICT_OPENED || sequence != 0 );

    int out_payload_capacity = nondet_int();
    __CPROVER_assume( out_payload_capacity >= 0 && out_payload_capacity <= ROCKETNET_MAX_PACKET_BYTES );
    uint8_t * out_payload = (uint8_t *) malloc( (size_t) out_payload_capacity );
    __CPROVER_assume( out_payload != NULL );
    int out_payload_bytes = -1;

    const int result = rocketnet_client_protocol_process_record( client, packet, packet_bytes, verdict, sequence, 1000.0,
                                                                 out_payload, out_payload_capacity, &out_payload_bytes );

    proof_dispatch_asserts( result, packet, packet_bytes, out_payload_capacity, out_payload_bytes, state_before, client );
    __CPROVER_assert( client->record_verdict == ROCKETNET_CLIENT_IO_VERDICT_RAW && client->record_sequence == 0,
                      "the record consumer leaves the record raw" );
    PROOF_REACHED( result == 1 && verdict == ROCKETNET_CLIENT_IO_VERDICT_OPENED && out_payload_bytes > 0,
                   "an opened record delivers a payload" );
}

int main( void )
{
    harness_process_packet();
    harness_process_cookie();
    harness_process_challenge();
    harness_process_denied();
    harness_open_session_packet();
    harness_process_keep_alive();
    harness_process_payload();
    harness_process_ack();
    harness_process_fragment();
    harness_process_disconnect();
    harness_process_record();
    return 0;
}
