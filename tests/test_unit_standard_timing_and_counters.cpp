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

#define ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
#include "rocketnet_server_transport.cpp"
#undef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

static const char * const counter_names[] =
{
#define ROCKETNET_COUNTER_NAME_ROW( name, klass, family ) #name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_NAME_ROW )
#undef ROCKETNET_COUNTER_NAME_ROW
};

static const int16_t counter_families[] =
{
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
        out[at++] = (uint8_t) ( ( value >> ( 8 * i ) ) & 0xff );
}

static int standard_timing_and_counters_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t17.timing-defaults" ) == 0 )
    {
        static const uint64_t defaults[] =
        {
            (uint64_t) ROCKETNET_DEFAULT_SEND_RATE,
            (uint64_t) ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS,
            (uint64_t) ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS,
            (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_ACK_DEBT_THRESHOLD,
            (uint64_t) ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE,
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

// Re-pin t17 because the ACK-debt threshold now records one eliciting packet.
static const rn_standard_vector_t standard_timing_and_counters_table[] =
{
    RN_STANDARD_VECTOR( "t17.timing-defaults", "0a0000000000000005000000000000000a000000000000000300000000000000"
    "3c000000000000000a000000000000000a000000000000000100000000000000"
    "00040000000000005a000000000000000a00000000000000" ),
    // Vector reflects the nine message-violation counters plus XDP_KERNEL_DROPPED_NO_SOCKET, CLIENT_RECEIVE_FOREIGN_SOURCE, XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION and MESSAGE_NOT_RECEIVED appended in sdk/rocketnet_counters.h.
    RN_STANDARD_VECTOR( "t18.counter-registry",
    "8472d40203000064f915d0000000ed4bcc7f000000d0a5f5ee000000a0f3313d000000b6f1b51a00"
    "0000c4cd1cf70000007b0a16790300009f6c53a8000800f4318ea9010800e9bb27a7010800e2b441"
    "47000800a6f4da6d0008008fbe1858000800078a62f3000800381e6d9d00080074ad3d54000800d9"
    "133a06000800867d97ae010800f2c0a831000800b054ce7a000800765c65f4000800c63a5e6b0008"
    "0083ac9718000800392db38a000800cab3522700080022c304a2000800d44833550008002d7185d4"
    "0008009e6881e9000800bc8fa1e80108003c90b3080008008d404106030000dda2ee3d002100785d"
    "386600210044c9d6310021000e509b5200210012666f95012100104e2af9002100e9b64344002100"
    "c38d493f0021002dcb1de3012100142d6274030000dd093874012b00a718c361002b007d7e147800"
    "2b00239fe39f002b00e4d57b22002b000ead34f4012b00edc0b2220300005882f200013200e45a4f"
    "d7003200a0c9d1a9003200eed15a44003200036293d90032003236a2080132000f0d2e81030000ea"
    "c25dfe013900a2a8d3d10039003233cd4e003900ac2292170039004568ca46003900b7e22fb20139"
    "00aa41977f010000e816ddd1010000ce68d7b201000082b49c08000000462ea274010000aa0cf9d4"
    "00000083bb10c200000090a272920100004a5a6ef50100004114e7e901000005ef7afd03000041c7"
    "d879014a0093c7db0b004a00390da171014a004e9794f8030000c8f0edd4014e00d5c9a8e0004e00"
    "d1506eb9004e00ef2550d4004e00e4bc5985014e004a2735c20300001427179100540081ecdf6c00"
    "540085605f2c00540028d57e9100540002c694a60300009cf448b7015900590f01f8005900f36f06"
    "860059003d948096005900900e20d4015900579ee834030000af3ffc5c015f000af39798005f002a"
    "aa4800005f00d4c63f68005f00ac22c8ca015f0039ab6d3b030000e56b749901650080f250980065"
    "00bcfdcd35006500e2f890b500650093ae2e5f016500704aeeba002b00ae736c4b002b00bdbdd030"
    "003200298bcc0a003200bbaf99b00039007f4a291100390048125558030000ec98a5af007100e982"
    "820e0171004bddb83500710011fb6fa7007100afcad64a007100c0d9e3570071001a2f7e36007100"
    "ca9866b0017100edf37c75010000500c6ade000000fa02eca4005900c84be01e005900836c601200"
    "5f000732aa00005f00812496670065004d51b5f9006500be45d6100300005852df2f0182000edc53"
    "ed008200a51ca687008200ef8c72ea008200018c3c770082003c00083b008200549bf77f0182009f"
    "c827da01000032bd5934000000208d29eb002b001f85bd3d003200952e8fa2003900146d25e00071"
    "00da2fc91f005900fd2fafea005f009bd7db8e006500364c15f10082001585fa9b03000045d5c2e0"
    "00940010e1ec540194006c08416f00940078e8d17f009400f68925b4009400cb1b1e15009400d130"
    "e1f6009400e7940e7e009400275197bb009400534a56eb0294004d5372c5009400213d3af3009400"
    "524fed8a009400144bf7f8009400496d8a2f00940084d7ac2f009400d70f304a01940048b0cf5401"
    "0000334b1d7f020000c51b4a3b000000877fa435030000ffb47e1001a900133b8fb100a900dad1d8"
    "0c00a9003a47736e00a90004ce96ae00a9009778a8bd00a900ed64901300a900b125a11200a90051"
    "c3b44902a9001f07086700a900bff6bf6e00a900804bf16e00a9001a79664500a900c781a5eb00a9"
    "007261031900a900ed197bce01a9005af5d9f501000015466d2c020000db577009000000352cd668"
    "000000b81100f30000003bc4a0c50000007ed194cb000000c935a956000000958442e9004a0087ed"
    "875f0000006130e850000000c9577e8600080072480e32000800e3c797df00000041af4dad000000"
    "1205760d0200005c34ae7c000000dd1ac85c000000ea22624a0200006e2d52bb0000006859e82800"
    "0000a1b5721800000055a4ca83000000d28aa748000000fea38fb7020000fd92bd5f020000bc1109"
    "050000002a62770c000000286577120100009fad6596020000c22fdb2c0021006bc6a44102000088"
    "edb156000000d692db95000000d76a4df9000000eb037f49000000111a37e10000009c475ee70000"
    "008c5c0526010000445bfe0f000000ff64f7fa010000754f7d3a000000b1eb078c000000e9539e77"
    "000000b9172c0700000034b485d8000000b0cb42af010000bdb0385c0100009f8d1dfd000000bea9"
    "a8b8004e0022203fb10054006ac070be000800176f7f780008008b6d57dc000000bf7a1aaf010000"
    "9b861b950100007987fe4c010000f984f06e010000251195dc000000dfffa1970100007e014fa100"
    "00001d39f161010000ab9fe25c0000007d984f4e00000058b60b73000000f925ca24000000509a50"
    "f50000000a0426b7000000dd1a0477000000ee4e501b00000037af52db0000007a31932200"
    "000079b815e7000000"
    "395ff812000000a458a7b3000000" ), // regenerated twin updated to include CONNECTION_RESPONSE_PADDING_NOT_ZERO counter
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
            rocketnet_unit_check( strcmp( counter_names[i], counter_names[j] ) != 0 );
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
