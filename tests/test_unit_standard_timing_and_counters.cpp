/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_timing_and_counters.cpp -- the vector twins of RN-1 t17
// (Timing and defaults) and t18 (Error handling and counters), regenerated
// through the implementation.

#include "test_unit_standard.h"

#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_server.h"
#include "rocketnet_constants.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_config.h"

#define ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
#include "rocketnet_server_transport.cpp"
#undef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

static const char * const counter_names[] = {
#define ROCKETNET_COUNTER_NAME_ROW( name, klass, family ) #name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_NAME_ROW )
#undef ROCKETNET_COUNTER_NAME_ROW
};

static const int16_t counter_families[] = {
#define ROCKETNET_COUNTER_FAMILY_ROW( name, klass, family ) ROCKETNET_COUNTER_##family,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_FAMILY_ROW )
#undef ROCKETNET_COUNTER_FAMILY_ROW
};

static uint32_t standard_fnv1a32( const char * text )
{
    uint32_t hash = 2166136261u;
    for ( const unsigned char * p = (const unsigned char *) text; *p; p++ )
    {
        hash ^= *p;
        hash *= 16777619u;
    }

    return hash;
}

static void standard_put_u16le( uint8_t * out, int & at, uint16_t value )
{
    out[at++] = (uint8_t) ( value & 0xff );
    out[at++] = (uint8_t) ( ( value >> 8 ) & 0xff );
}

static void standard_put_u64le( uint8_t * out, int & at, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
    {
        out[at++] = (uint8_t) ( ( value >> ( 8 * i ) ) & 0xff );
    }
}

static int standard_timing_and_counters_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t17.timing-defaults" ) == 0 )
    {
        // the handshake deadline default is the config table's, the one the create path reads
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config_current( &config );

        const uint64_t defaults[] = {
            (uint64_t) ROCKETNET_DEFAULT_SEND_RATE,
            (uint64_t) config.handshake_deadline_seconds,
            (uint64_t) ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS,
            (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_ACK_DEBT_THRESHOLD,
            (uint64_t) ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE,
            (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET,
            (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS,
        };
        const int count = (int) ( sizeof( defaults ) / sizeof( defaults[0] ) );
        if ( capacity < count * 8 )
            return -1;

        int at = 0;
        for ( int i = 0; i < count; i++ )
            standard_put_u64le( out, at, defaults[i] );
        return at;
    }

    if ( strcmp( id, "t18.counter-registry" ) == 0 )
    {
        const int count = ROCKETNET_NUM_COUNTERS;
        if ( capacity < count * 7 )
            return -1;

        int at = 0;
        for ( int i = 0; i < count; i++ )
        {
            const uint32_t hash = standard_fnv1a32( counter_names[i] );
            out[at++] = (uint8_t) ( hash & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 8 ) & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 16 ) & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 24 ) & 0xff );
            out[at++] = (uint8_t) rocketnet_counter_class( i );
            standard_put_u16le( out, at, (uint16_t) ( counter_families[i] + 1 ) );
        }

        return at;
    }

    return -1;
}

// Re-pin t17 because the ACK-debt threshold now records one eliciting packet,
// and again for the handshake work budget's default of 16.
static const rn_standard_vector_t standard_timing_and_counters_table[] = {
    RN_STANDARD_VECTOR( "t17.timing-defaults", "0a0000000000000005000000000000000a000000000000000300000000000000"
                                               "3c000000000000000a000000000000000a000000000000000100000000000000"
                                               "10000000000000005a000000000000000a00000000000000" ),
    RN_STANDARD_VECTOR( "t18.counter-registry",
                        "8472d40203000064f915d0000000ed4bcc7f000000d0a5f5ee000000a0f3313d000000b6f1b51a00"
                        "0000c4cd1cf70000007b0a16790300009f6c53a8000800f4318ea9010800e9bb27a7010800e2b441"
                        "47000800a6f4da6d0008008fbe1858000800078a62f3000800381e6d9d00080074ad3d54000800d9"
                        "133a06000800867d97ae010800f2c0a831000800b054ce7a000800765c65f4000800c63a5e6b0008"
                        "0083ac9718000800392db38a000800cab35227000800d44833550008002d7185d40008009e6881e9"
                        "000800bc8fa1e80108003c90b3080008008d404106030000dda2ee3d002000785d386600200044c9"
                        "d6310020000e509b5200200012666f95012000104e2af9002000c38d493f0020002dcb1de3012000"
                        "142d6274030000dd093874012900a718c3610029007d7e1478002900239fe39f002900e4d57b2200"
                        "29000ead34f4012900edc0b2220300005882f200013000e45a4fd7003000a0c9d1a9003000eed15a"
                        "44003000036293d90030003236a2080130000f0d2e81030000eac25dfe013700a2a8d3d100370032"
                        "33cd4e003700ac2292170037004568ca46003700b7e22fb2013700aa41977f010000e816ddd10100"
                        "00ce68d7b201000082b49c08000000462ea274010000aa0cf9d400000083bb10c200000090a27292"
                        "0100004a5a6ef50100004114e7e901000005ef7afd03000041c7d87901480093c7db0b004800390d"
                        "a1710148004e9794f8030000c8f0edd4014c00d5c9a8e0004c00d1506eb9004c00ef2550d4004c00"
                        "e4bc5985014c004a2735c20300001427179100520081ecdf6c00520085605f2c00520028d57e9100"
                        "520002c694a60300009cf448b7015700590f01f8005700f36f06860057003d948096005700900e20"
                        "d4015700579ee834030000af3ffc5c015d000af39798005d002aaa4800005d00d4c63f68005d00ac"
                        "22c8ca015d0039ab6d3b030000e56b749901630080f25098006300bcfdcd35006300e2f890b50063"
                        "0093ae2e5f016300704aeeba002900ae736c4b002900bdbdd030003000298bcc0a003000bbaf99b0"
                        "0037007f4a291100370048125558030000ec98a5af006f00e982820e016f004bddb835006f0011fb"
                        "6fa7006f00afcad64a006f00c0d9e357006f001a2f7e36006f00ca9866b0016f00edf37c75010000"
                        "500c6ade000000fa02eca4005700c84be01e005700836c6012005d000732aa00005d008124966700"
                        "63004d51b5f9006300be45d6100300005852df2f0180000edc53ed008000a51ca687008000ef8c72"
                        "ea008000018c3c770080003c00083b008000549bf77f0180009fc827da01000032bd593400000020"
                        "8d29eb0029001f85bd3d003000952e8fa2003700146d25e0006f00da2fc91f005700fd2fafea005d"
                        "009bd7db8e006300364c15f1008000b81100f30000003bc4a0c50000007ed194cb000000c935a956"
                        "000000958442e900480087ed875f0000006130e850000000c9577e8600080072480e32000800e3c7"
                        "97df00000041af4dad0000001205760d0200005c34ae7c000000dd1ac85c000000ea22624a020000"
                        "6e2d52bb0000006859e828000000a1b5721800000055a4ca83000000d28aa748000000fea38fb702"
                        "0000fd92bd5f020000bc1109050000002a62770c000000286577120100009fad6596020000c22fdb"
                        "2c0020006bc6a44102000088edb156000000d692db95000000d76a4df9000000eb037f4900000011"
                        "1a37e10000009c475ee70000008c5c0526010000445bfe0f000000ff64f7fa010000754f7d3a0000"
                        "00b1eb078c000000e9539e77000000b9172c0700000034b485d8000000b0cb42af010000bdb0385c"
                        "0100009f8d1dfd000000bea9a8b8004c0022203fb10052006ac070be0008008b6d57dc000000bf7a"
                        "1aaf0100009b861b950100007987fe4c010000f984f06e010000251195dc000000dfffa197010000"
                        "7e014fa10000001d39f161010000ab9fe25c0000007d984f4e00000058b60b73000000f925ca2400"
                        "0000509a50f50000000a0426b7000000dd1a0477000000ee4e501b00000037af52db0000007a3193"
                        "2200000079b815e7000000395ff812000000a458a7b300000098214ffa00200049b0ba620008003a"
                        "68d424000000da288aa6000000" ),
};

static void test_standard_timing_and_counters_vectors()
{
    const int count = (int) ( sizeof( standard_timing_and_counters_table ) / sizeof( standard_timing_and_counters_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_timing_and_counters_table, count, standard_timing_and_counters_generate ) == count );

    // the registry is a registry: every name is non-empty and distinct, every
    // class is one of the four, and every family is none or a counter index

    const int counters = ROCKETNET_NUM_COUNTERS;
    rocketnet_unit_check( counters == (int) ( sizeof( counter_names ) / sizeof( counter_names[0] ) ) );
    rocketnet_unit_check( counters == (int) ( sizeof( counter_families ) / sizeof( counter_families[0] ) ) );
    for ( int i = 0; i < counters; i++ )
    {
        rocketnet_unit_check( counter_names[i] != NULL && counter_names[i][0] != 0 );

        const int klass = rocketnet_counter_class( i );
        rocketnet_unit_check( klass >= ROCKETNET_COUNTER_CLASS_REFUSAL && klass < ROCKETNET_NUM_COUNTER_CLASSES );
        rocketnet_unit_check( counter_families[i] == ROCKETNET_COUNTER_NONE ||
                              ( counter_families[i] >= 0 && counter_families[i] < counters ) );
        for ( int j = 0; j < i; j++ )
        {
            rocketnet_unit_check( strcmp( counter_names[i], counter_names[j] ) != 0 );
        }
    }

    // the walk's own refusals, as the skeleton's case checks them

    const uint8_t a[3] = { 1, 2, 3 };
    const uint8_t b[3] = { 1, 9, 3 };
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 3 ) == -1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, b, 3 ) == 1 );

    uint8_t scratch[2];
    rocketnet_unit_check( rn_standard_hex_decode( "0g", scratch, 2 ) == -1 );
    rocketnet_unit_check( standard_timing_and_counters_generate( "t17.no-such-vector", scratch, 2 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_timing_and_counters_vectors );
