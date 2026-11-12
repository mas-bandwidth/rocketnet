// diagnostic, not a proof (INVENTORY.md, "ACK run measurements"): isolate what makes ESBMC's symex of the ACK harness slow
#include <stdint.h>
#include "rocketnet_ack.cpp"

int harness_micro( void )
{
#if MICRO_LOCAL
    struct rocketnet_ack_endpoint_t endpoint;
#else
    static struct rocketnet_ack_endpoint_t endpoint;
#endif
    uint64_t s = nondet_uint64();
    const uint64_t slot = s % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
#if MICRO == 1
    endpoint.sent[slot].sequence = s;
    __CPROVER_assert( endpoint.sent[slot].sequence == s, "direct" );
#elif MICRO == 2
    struct rocketnet_sent_packet_record_t * r = &endpoint.sent[slot];
    r->sequence = s;
    __CPROVER_assert( r->sequence == s, "pointer" );
#elif MICRO == 3
    struct rocketnet_ack_endpoint_t * e = &endpoint;
    e->sent[slot].sequence = s;
    __CPROVER_assert( e->sent[slot].sequence == s, "endpoint pointer" );
#elif MICRO == 4
    for ( int i = 0; i < MICRO_N; ++i )
    {
        endpoint.sent_unacked[( slot + i ) >> 6 & 63] |= 1ULL << ( ( slot + i ) & 63 );
    }
    __CPROVER_assert( endpoint.sent_unacked[slot >> 6] != 0, "bitmap" );
#elif MICRO == 5
    for ( int i = 0; i < MICRO_N; ++i )
    {
        struct rocketnet_sent_packet_record_t * r = &endpoint.sent[( slot + i ) % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        r->sequence = s + i;
        r->acked = 0;
    }
    __CPROVER_assert( endpoint.sent[slot].sequence == s, "pointer loop" );
#elif MICRO == 6
    // direct write, pointer read
    endpoint.sent[slot].sequence = s;
    struct rocketnet_sent_packet_record_t * r = &endpoint.sent[slot];
    __CPROVER_assert( r->sequence == s, "direct write, pointer read" );
#elif MICRO == 7
    // pointer write, direct read
    struct rocketnet_sent_packet_record_t * r = &endpoint.sent[slot];
    r->sequence = s;
    __CPROVER_assert( endpoint.sent[slot].sequence == s, "pointer write, direct read" );
#elif MICRO == 8
    // pointer writes in a loop, pointer reads only (through an endpoint pointer, as production)
    struct rocketnet_ack_endpoint_t * e = &endpoint;
    for ( int i = 0; i < MICRO_N; ++i )
    {
        struct rocketnet_sent_packet_record_t * r = &e->sent[( slot + i ) % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        r->sequence = s + i;
        r->acked = 0;
    }
    struct rocketnet_sent_packet_record_t * q = &e->sent[slot];
    __CPROVER_assert( q->sequence == s, "pointer loop, pointer read" );
#endif
    return 0;
}
