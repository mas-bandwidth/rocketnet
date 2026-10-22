/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_transport_appendices.cpp -- the vector twins of RN-1 tb
// and tc (standard/TRANSPORT.md, Appendix B and Appendix C). Both appendices
// are prose: tb lists differences from netcode 1.02 and tc holds informative
// implementation notes, so neither defines a known-answer byte string. The
// table is empty and the case asserts that the walk accepts an empty table and
// that its generator refuses every id. A later vector in either appendix gets
// its RN_STANDARD_VECTOR row here and its generator arm below.

#include "test_unit_standard.h"

static int standard_transport_appendices_generate( const char * id, uint8_t * out, int capacity )
{
    (void) id;
    (void) out;
    (void) capacity;
    return -1;
}

static void test_standard_transport_appendices_vectors()
{
    const int count = rn_standard_vectors_walk( NULL, 0, standard_transport_appendices_generate );
    rocketnet_unit_check( count == 0 );

    uint8_t scratch[4];
    rocketnet_unit_check( standard_transport_appendices_generate( "tb.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
    rocketnet_unit_check( standard_transport_appendices_generate( "tc.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_transport_appendices_vectors );
