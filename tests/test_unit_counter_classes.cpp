/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The counter audit. The counter list is the one place a counter's meaning
// lives: its name, its class (what a clean run shows) and its family (the
// RECEIVED counter whose outcomes it sums into) are one row in
// sdk/rocketnet_counters.h, and the tests here keep the rows honest -- every
// index has a class, no outcome sits in two families, conservation reports
// the first family that does not balance, the exact-delta helper catches a
// move nobody named, and every counter is named by a test.
//
// Every case runs in memory. The one file access is the source audit, which
// reads the repository's own test and sdk sources named by the build (no
// directory walk, no network, no clock).

#include "test_unit_registry.h"
#include "resource_file.h"

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the rows, read back out of the counter list

struct counters_row_t
{
    const char * name;
    int counter_class;
    int family;                 // the RECEIVED counter this outcome sums into, or ROCKETNET_COUNTER_NONE
};

static const counters_row_t counters_rows[ROCKETNET_NUM_COUNTERS] =
{
#define ROCKETNET_COUNTER_TEST_ROW( name, klass, family ) { #name, ROCKETNET_COUNTER_CLASS_##klass, ROCKETNET_COUNTER_##family },
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_TEST_ROW )
#undef ROCKETNET_COUNTER_TEST_ROW
};

// The counter list at BASE, in index order: the indices are a contract and
// never move. A counter appended since BASE is listed after, in the order it
// was appended.

static const char * const counters_base_names[] =
{
    "PACKETS_RECEIVED",
    "PREFILTER_TOO_SHORT",
    "PREFILTER_TOO_LARGE",
    "PREFILTER_WRONG_EPOCH",
    "PREFILTER_WRONG_VALUE",
    "RESERVED_TYPE",
    "WRONG_DIRECTION",
    "CONNECTION_REQUEST_RECEIVED",
    "CONNECTION_REQUEST_WRONG_SIZE",
    "CONNECTION_REQUEST_ALREADY_CONNECTED",
    "CONNECTION_REQUEST_CHALLENGE_RESENT",
    "CONNECTION_REQUEST_WRONG_VERSION",
    "CONNECTION_REQUEST_WRONG_PROTOCOL_ID",
    "CONNECTION_REQUEST_TOKEN_EXPIRED",
    "CONNECTION_REQUEST_TIMEOUT_INVALID",
    "CONNECTION_REQUEST_BAD_ADDRESS_FLAGS",
    "CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED",
    "CONNECTION_REQUEST_WRONG_SERVER_KEY",
    "CONNECTION_REQUEST_NO_COOKIE",
    "CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY",
    "CONNECTION_REQUEST_COOKIE_EXPIRED",
    "CONNECTION_REQUEST_TOKEN_ALREADY_USED",
    "CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY",
    "CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK",
    "CONNECTION_REQUEST_SEALED_MISMATCH",
    "CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED",
    "CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED",
    "CONNECTION_REQUEST_SMALL_ORDER_KEY",
    "CONNECTION_REQUEST_NO_SLOTS",
    "CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING",
    "CONNECTION_REQUEST_SENT_CHALLENGE",
    "CONNECTION_REQUEST_INTERNAL_ERROR",
    "CONNECTION_RESPONSE_RECEIVED",
    "CONNECTION_RESPONSE_WRONG_SIZE",
    "CONNECTION_RESPONSE_NO_MAPPING",
    "CONNECTION_RESPONSE_ZERO_SEQUENCE",
    "CONNECTION_RESPONSE_DID_NOT_OPEN",
    "CONNECTION_RESPONSE_PADDING_NOT_ZERO",
    "CONNECTION_RESPONSE_ALREADY_CONNECTED",
    "CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED",
    "CONNECTION_RESPONSE_ADMITTED_SET_FULL",
    "CONNECTION_RESPONSE_NO_SLOTS",
    "CONNECTION_RESPONSE_CONNECTED",
    "KEEP_ALIVE_RECEIVED",
    "KEEP_ALIVE_NO_MAPPING",
    "KEEP_ALIVE_ZERO_SEQUENCE",
    "KEEP_ALIVE_ALREADY_RECEIVED",
    "KEEP_ALIVE_DID_NOT_OPEN",
    "KEEP_ALIVE_NOT_CONNECTED",
    "KEEP_ALIVE_ACCEPTED",
    "PAYLOAD_RECEIVED",
    "PAYLOAD_NO_MAPPING",
    "PAYLOAD_ZERO_SEQUENCE",
    "PAYLOAD_ALREADY_RECEIVED",
    "PAYLOAD_DID_NOT_OPEN",
    "PAYLOAD_NOT_CONNECTED",
    "PAYLOAD_DELIVERED",
    "DISCONNECT_RECEIVED",
    "DISCONNECT_NO_MAPPING",
    "DISCONNECT_ZERO_SEQUENCE",
    "DISCONNECT_ALREADY_RECEIVED",
    "DISCONNECT_DID_NOT_OPEN",
    "DISCONNECT_NOT_CONNECTED",
    "DISCONNECT_DISCONNECTED",
    "MAPPING_CREATED",
    "MAPPING_IDLE_TIMEOUT",
    "MAPPING_HANDSHAKE_DEADLINE_EXPIRED",
    "MAPPING_TABLE_FULL",
    "CLIENT_CONNECTED",
    "SEND_RING_FULL",
    "SLOT_WITHOUT_MAPPING",
    "CLIENT_DISCONNECTED",
    "SERVER_DISCONNECTED_CLIENT",
    "CLIENT_TIMED_OUT",
    "CLIENT_COOKIE_RECEIVED",
    "CLIENT_COOKIE_WRONG_STATE",
    "CLIENT_COOKIE_SALT_MISMATCH",
    "CLIENT_COOKIE_STORED",
    "CLIENT_CHALLENGE_RECEIVED",
    "CLIENT_CHALLENGE_WRONG_STATE",
    "CLIENT_CHALLENGE_ZERO_SEQUENCE",
    "CLIENT_CHALLENGE_DID_NOT_OPEN",
    "CLIENT_CHALLENGE_SMALL_ORDER_KEY",
    "CLIENT_CHALLENGE_ACCEPTED",
    "CLIENT_DENIED_RECEIVED",
    "CLIENT_DENIED_WRONG_STATE",
    "CLIENT_DENIED_ZERO_SEQUENCE",
    "CLIENT_DENIED_DID_NOT_OPEN",
    "CLIENT_DENIED_ACCEPTED",
    "CLIENT_KEEP_ALIVE_RECEIVED",
    "CLIENT_KEEP_ALIVE_WRONG_STATE",
    "CLIENT_KEEP_ALIVE_ZERO_SEQUENCE",
    "CLIENT_KEEP_ALIVE_ALREADY_RECEIVED",
    "CLIENT_KEEP_ALIVE_DID_NOT_OPEN",
    "CLIENT_KEEP_ALIVE_ACCEPTED",
    "CLIENT_PAYLOAD_RECEIVED",
    "CLIENT_PAYLOAD_WRONG_STATE",
    "CLIENT_PAYLOAD_ZERO_SEQUENCE",
    "CLIENT_PAYLOAD_ALREADY_RECEIVED",
    "CLIENT_PAYLOAD_DID_NOT_OPEN",
    "CLIENT_PAYLOAD_DELIVERED",
    "CLIENT_DISCONNECT_RECEIVED",
    "CLIENT_DISCONNECT_WRONG_STATE",
    "CLIENT_DISCONNECT_ZERO_SEQUENCE",
    "CLIENT_DISCONNECT_ALREADY_RECEIVED",
    "CLIENT_DISCONNECT_DID_NOT_OPEN",
    "CLIENT_DISCONNECT_ACCEPTED",
    "KEEP_ALIVE_WRONG_SIZE",
    "KEEP_ALIVE_BAD_ACK_BLOCK",
    "PAYLOAD_WRONG_SIZE",
    "PAYLOAD_BAD_ACK_BLOCK",
    "DISCONNECT_WRONG_SIZE",
    "DISCONNECT_BAD_ACK_BLOCK",
    "ACK_RECEIVED",
    "ACK_WRONG_SIZE",
    "ACK_NO_MAPPING",
    "ACK_ZERO_SEQUENCE",
    "ACK_ALREADY_RECEIVED",
    "ACK_DID_NOT_OPEN",
    "ACK_NOT_CONNECTED",
    "ACK_BAD_ACK_BLOCK",
    "ACK_ACCEPTED",
    "ACK_UNKNOWN_SEQUENCE",
    "ACKED_BATCH_FULL",
    "CLIENT_KEEP_ALIVE_WRONG_SIZE",
    "CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK",
    "CLIENT_PAYLOAD_WRONG_SIZE",
    "CLIENT_PAYLOAD_BAD_ACK_BLOCK",
    "CLIENT_DISCONNECT_WRONG_SIZE",
    "CLIENT_DISCONNECT_BAD_ACK_BLOCK",
    "CLIENT_ACK_RECEIVED",
    "CLIENT_ACK_WRONG_STATE",
    "CLIENT_ACK_WRONG_SIZE",
    "CLIENT_ACK_ZERO_SEQUENCE",
    "CLIENT_ACK_ALREADY_RECEIVED",
    "CLIENT_ACK_DID_NOT_OPEN",
    "CLIENT_ACK_BAD_ACK_BLOCK",
    "CLIENT_ACK_ACCEPTED",
    "CLIENT_ACK_UNKNOWN_SEQUENCE",
    "CLIENT_ACKED_BATCH_FULL",
    "KEEP_ALIVE_WRONG_OPENED_SIZE",
    "PAYLOAD_WRONG_OPENED_SIZE",
    "DISCONNECT_WRONG_OPENED_SIZE",
    "ACK_WRONG_OPENED_SIZE",
    "CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE",
    "CLIENT_PAYLOAD_WRONG_OPENED_SIZE",
    "CLIENT_DISCONNECT_WRONG_OPENED_SIZE",
    "CLIENT_ACK_WRONG_OPENED_SIZE",
    "FRAGMENT_RECEIVED",
    "FRAGMENT_WRONG_SIZE",
    "FRAGMENT_NO_MAPPING",
    "FRAGMENT_ZERO_SEQUENCE",
    "FRAGMENT_ALREADY_RECEIVED",
    "FRAGMENT_DID_NOT_OPEN",
    "FRAGMENT_NOT_CONNECTED",
    "FRAGMENT_BAD_ACK_BLOCK",
    "FRAGMENT_WRONG_OPENED_SIZE",
    "FRAGMENT_ZERO_PAYLOAD_ID",
    "FRAGMENT_STALE",
    "FRAGMENT_COUNT_INVALID",
    "FRAGMENT_INDEX_OUT_OF_RANGE",
    "FRAGMENT_COUNT_MISMATCH",
    "FRAGMENT_SIZE_MISMATCH",
    "FRAGMENT_PAYLOAD_TOO_LARGE",
    "FRAGMENT_DUPLICATE_INDEX",
    "FRAGMENT_ACCEPTED",
    "FRAGMENT_PAYLOAD_REASSEMBLED",
    "FRAGMENT_PAYLOAD_ABANDONED",
    "FRAGMENT_PAYLOAD_BATCH_FULL",
    "CLIENT_FRAGMENT_RECEIVED",
    "CLIENT_FRAGMENT_WRONG_STATE",
    "CLIENT_FRAGMENT_WRONG_SIZE",
    "CLIENT_FRAGMENT_ZERO_SEQUENCE",
    "CLIENT_FRAGMENT_ALREADY_RECEIVED",
    "CLIENT_FRAGMENT_DID_NOT_OPEN",
    "CLIENT_FRAGMENT_BAD_ACK_BLOCK",
    "CLIENT_FRAGMENT_WRONG_OPENED_SIZE",
    "CLIENT_FRAGMENT_ZERO_PAYLOAD_ID",
    "CLIENT_FRAGMENT_STALE",
    "CLIENT_FRAGMENT_COUNT_INVALID",
    "CLIENT_FRAGMENT_INDEX_OUT_OF_RANGE",
    "CLIENT_FRAGMENT_COUNT_MISMATCH",
    "CLIENT_FRAGMENT_SIZE_MISMATCH",
    "CLIENT_FRAGMENT_PAYLOAD_TOO_LARGE",
    "CLIENT_FRAGMENT_DUPLICATE_INDEX",
    "CLIENT_FRAGMENT_ACCEPTED",
    "CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED",
    "CLIENT_FRAGMENT_PAYLOAD_ABANDONED",
    "CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL",
    "SEND_ARENA_FULL",
    "RECEIVE_CURSOR_TOO_SMALL",
    "SEND_HISTORY_OVERFLOW",
    "SEND_LEASE_FULL",
    "INTEGRITY_LIMIT_REACHED",
    "CLIENT_COOKIE_WRONG_SIZE",
    "MESSAGE_SEND_STORE_FULL",
    "MESSAGE_SEND_QUEUE_FULL",
    "CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED",
    "CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT",
    "MESSAGE_UNRELIABLE_DELIVERY_FULL",
    "MESSAGE_UNRELIABLE_SEND_FAILED",
    "MESSAGE_DUPLICATE",
    "MESSAGE_WINDOW_STALL",
    "MESSAGE_SENT_MAP_EVICTED",
    "MESSAGE_RESENT",
    "MESSAGE_MEMORY_EXHAUSTED",
    "MESSAGE_SEND_TYPE_NOT_ENABLED",
    "MESSAGE_TYPE_NOT_ENABLED",
    "MESSAGE_UNRELIABLE_OVER_BUDGET",
    "MESSAGE_ORDERING_BEHIND",
    "MESSAGE_SEQUENCED_STALE",
    "MESSAGE_SEQUENCED_SUPERSEDED",
    "MESSAGE_SEQUENCED_HOLD_FULL",
    "MESSAGE_RECEIPT_RING_FULL",
    "MESSAGE_RECEIPT_ACKED",
    "MESSAGE_RECEIPT_LOSS",
};

static const char * const counters_appended_names[] =
{
    "CONNECTION_RESPONSE_INTERNAL_ERROR",
    "SEND_HISTORY_LOST",
    "MESSAGE_BAD_CHANNEL",
    "MESSAGE_BAD_BLOCK",
    "MESSAGE_BAD_RING",
    "MESSAGE_ORDERING_AHEAD",
    "MESSAGE_PROTOCOL_VIOLATION",
    "MESSAGE_SEND_REFUSED",
    "MESSAGE_STALE_CONNECTION_ENTRY",
    "MESSAGE_NOT_RELEASED",
    "XDP_KERNEL_REDIRECTED",
    "XDP_KERNEL_DROPPED_TOO_SMALL",
    "XDP_KERNEL_DROPPED_TOO_LARGE",
    "XDP_KERNEL_DROPPED_WRONG_FILTER",
    "XDP_KERNEL_DROPPED_INVALID_TYPE",
    "XDP_KERNEL_DROPPED_WRONG_EPOCH",
    "XDP_KERNEL_PASSED_IPV6_EXTENSION",
    "CLIENT_SEND_DEFERRED",
    "XDP_KERNEL_DROPPED_REPLAYED",
    "CLIENT_CHALLENGE_WRONG_SIZE",
    "CLIENT_DENIED_WRONG_SIZE",
    "CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE",
    "CONNECTION_REQUEST_TOKEN_LIFETIME_TOO_LONG",
    "SEND_NO_LINK_ADDRESS",
    "MESSAGE_PROMOTED_UNRELIABLE",
    "MESSAGE_PROMOTED_UNRELIABLE_SEQUENCED",
    "MESSAGE_PROMOTED_UNRELIABLE_WITH_ACK_RECEIPT",
    "MESSAGE_UNRELIABLE_IN_PLACE_FALLBACK",
    "SEND_FAILED",
    "XDP_KERNEL_PASSED_FRAGMENT",
    "XDP_KERNEL_DROPPED_FIRST_FRAGMENT",
    "MESSAGE_SEND_NOT_CONNECTED",
    "MESSAGE_BAD_FORM",
    "MESSAGE_TRUNCATED",
    "MESSAGE_BAD_FLAGS",
    "MESSAGE_BAD_LENGTH",
    "MESSAGE_BAD_SIZE",
    "MESSAGE_BAD_CONTROL",
    "MESSAGE_NUMBER_OUTSIDE_WINDOW",
    "MESSAGE_SLOT_COLLISION",
    "MESSAGE_DUPLICATE_ORDERING",
    "XDP_KERNEL_DROPPED_NO_SOCKET",
    "CLIENT_RECEIVE_FOREIGN_SOURCE",
    "XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION",
    "MESSAGE_NOT_RECEIVED",
};

#define COUNTERS_NUM_BASE       ( (int) ( sizeof(counters_base_names) / sizeof(counters_base_names[0]) ) )
#define COUNTERS_NUM_APPENDED   ( (int) ( sizeof(counters_appended_names) / sizeof(counters_appended_names[0]) ) )

static int counters_find( const char * name )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        if ( strcmp( counters_rows[i].name, name ) == 0 )
            return i;
    return -1;
}

// is some counter's family this one: a RECEIVED counter

static bool counters_is_received( int counter )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        if ( counters_rows[i].family == counter )
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// 1: every counter has a class and a name, and the names are BASE's, in order

static void test_counters_every_counter_has_a_class()
{
    check( COUNTERS_NUM_BASE == 215 );
    check( ROCKETNET_NUM_COUNTERS == COUNTERS_NUM_BASE + COUNTERS_NUM_APPENDED );
    check( rocketnet_num_counters() == ROCKETNET_NUM_COUNTERS );

    int per_class[ROCKETNET_NUM_COUNTER_CLASSES];
    memset( per_class, 0, sizeof(per_class) );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const char * expected = i < COUNTERS_NUM_BASE ? counters_base_names[i] : counters_appended_names[i - COUNTERS_NUM_BASE];
        const char * name = rocketnet_counter_name( i );
        check( name != NULL );
        if ( strcmp( name, expected ) != 0 )
            printf( "counter %d is %s, expected %s: the indices never move\n", i, name, expected );
        check( strcmp( name, expected ) == 0 );

        const int counter_class = rocketnet_counter_class( i );
        if ( counter_class < 0 || counter_class >= ROCKETNET_NUM_COUNTER_CLASSES )
            printf( "counter %s has no class\n", name );
        check( counter_class >= 0 && counter_class < ROCKETNET_NUM_COUNTER_CLASSES );
        check( counter_class == counters_rows[i].counter_class );
        check( rocketnet_counter_class_name( counter_class ) != NULL );
        per_class[counter_class]++;

        // the family column names a RECEIVED counter, and a RECEIVED counter
        // is an arrival
        const int family = counters_rows[i].family;
        check( family == ROCKETNET_COUNTER_NONE || ( family >= 0 && family < ROCKETNET_NUM_COUNTERS ) );
        check( family != i );
        if ( family != ROCKETNET_COUNTER_NONE )
        {
            if ( counters_rows[family].counter_class != ROCKETNET_COUNTER_CLASS_ARRIVAL )
                printf( "counter %s sums into %s, which is not an arrival counter\n", name, counters_rows[family].name );
            check( counters_rows[family].counter_class == ROCKETNET_COUNTER_CLASS_ARRIVAL );
        }
        // every arrival counter heads a family, but the packet entry's
        // PACKETS_RECEIVED, which counts before any type dispatches
        if ( counters_rows[i].counter_class == ROCKETNET_COUNTER_CLASS_ARRIVAL && i != ROCKETNET_COUNTER_PACKETS_RECEIVED )
        {
            if ( !counters_is_received( i ) )
                printf( "arrival counter %s has no outcomes\n", name );
            check( counters_is_received( i ) );
        }
    }

    // exactly four classes, each used, and each named as the API names it
    check( ROCKETNET_NUM_COUNTER_CLASSES == 4 );
    for ( int c = 0; c < ROCKETNET_NUM_COUNTER_CLASSES; c++ )
        check( per_class[c] > 0 );
    check( strcmp( rocketnet_counter_class_name( ROCKETNET_COUNTER_CLASS_REFUSAL ), "refusal" ) == 0 );
    check( strcmp( rocketnet_counter_class_name( ROCKETNET_COUNTER_CLASS_EXPECTED ), "expected" ) == 0 );
    check( strcmp( rocketnet_counter_class_name( ROCKETNET_COUNTER_CLASS_PATH_LOSS ), "path_loss" ) == 0 );
    check( strcmp( rocketnet_counter_class_name( ROCKETNET_COUNTER_CLASS_ARRIVAL ), "arrival" ) == 0 );
    check( rocketnet_counter_class_name( -1 ) == NULL );
    check( rocketnet_counter_class_name( ROCKETNET_NUM_COUNTER_CLASSES ) == NULL );

    // an index outside the list has no class
    check( rocketnet_counter_class( -1 ) == -1 );
    check( rocketnet_counter_class( ROCKETNET_NUM_COUNTERS ) == -1 );

    // the classes the counter list's rules name, by counter
    check( rocketnet_counter_class( ROCKETNET_COUNTER_SEND_RING_FULL ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_RESPONSE_PADDING_NOT_ZERO ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_RESENT ) == ROCKETNET_COUNTER_CLASS_PATH_LOSS );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_NOT_RECEIVED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED ) == ROCKETNET_COUNTER_CLASS_PATH_LOSS );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED ) == ROCKETNET_COUNTER_CLASS_ARRIVAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_SEND_NO_LINK_ADDRESS ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_SEND_FAILED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
}

REGISTER_UNIT_TEST( test_counters_every_counter_has_a_class );

// ---------------------------------------------------------------------------
// the source audit: which counters do the tests name, which have a code path

#ifndef ROCKETNET_SOURCE_DIR
#error "the build passes ROCKETNET_SOURCE_DIR to this file"
#endif
#ifndef ROCKETNET_COUNTER_TEST_SOURCES
#error "the build passes ROCKETNET_COUNTER_TEST_SOURCES to this file"
#endif
#ifndef ROCKETNET_COUNTER_SDK_SOURCES
#error "the build passes ROCKETNET_COUNTER_SDK_SOURCES to this file"
#endif

static std::vector<std::string> counters_split_list( const char * list )
{
    std::vector<std::string> out;
    std::string item;
    for ( const char * c = list; ; c++ )
    {
        if ( *c == '|' || *c == '\0' )
        {
            if ( !item.empty() )
                out.push_back( item );
            item.clear();
            if ( *c == '\0' )
                break;
        }
        else
            item += *c;
    }
    return out;
}

static bool counters_read_file( const std::string & relative_path, std::string & contents )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
        return false;
    char buffer[65536];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof(buffer), file ) ) > 0 )
        contents.append( buffer, n );
    fclose( file );
    return true;
}

static bool counters_is_identifier_char( char c )
{
    return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

// every whole-word ROCKETNET_COUNTER_<NAME> in a text, as the bare <NAME>

static void counters_collect_names( const std::string & text, std::set<std::string> & names )
{
    static const char prefix[] = "ROCKETNET_COUNTER_";
    const size_t prefix_bytes = sizeof(prefix) - 1;
    size_t at = 0;
    while ( ( at = text.find( prefix, at ) ) != std::string::npos )
    {
        const bool starts_word = at == 0 || !counters_is_identifier_char( text[at - 1] );
        size_t end = at + prefix_bytes;
        while ( end < text.size() && counters_is_identifier_char( text[end] ) )
            end++;
        if ( starts_word && end > at + prefix_bytes )
            names.insert( text.substr( at + prefix_bytes, end - at - prefix_bytes ) );
        at = end;
    }
}

// the counters that no test source names. this file is the audit and is not
// a witness: a name that appears only here is not tested.

static std::set<std::string> counters_named_by_tests()
{
    std::set<std::string> names;
    const std::vector<std::string> files = counters_split_list( ROCKETNET_COUNTER_TEST_SOURCES );
    check( !files.empty() );
    for ( size_t i = 0; i < files.size(); i++ )
    {
        if ( files[i] == "tests/test_unit_counter_classes.cpp" )
            continue;
        std::string text;
        check( counters_read_file( files[i], text ) );
        counters_collect_names( text, names );
    }
    return names;
}

// the counters the sdk counts somewhere other than the counter list itself

static std::set<std::string> counters_named_by_sdk()
{
    std::set<std::string> names;
    const std::vector<std::string> files = counters_split_list( ROCKETNET_COUNTER_SDK_SOURCES );
    check( !files.empty() );
    for ( size_t i = 0; i < files.size(); i++ )
    {
        std::string text;
        check( counters_read_file( files[i], text ) );
        counters_collect_names( text, names );
    }
    return names;
}

// Counters with no code path in sdk/ yet. A counter whose first user has not
// landed cannot be tested by a packet, so it is listed here with the reason
// it has no path; the audit fails the moment a listed counter gains a path,
// so this list only shrinks.

struct counters_exception_t
{
    const char * name;
    const char * why;
};

static const counters_exception_t counters_no_code_path[] =
{
    { NULL, NULL }
};

// Keep this list empty while every reachable counter has a test witness.
// A future unreachable-by-test counter can only enter with its code-path
// reason; the audit rejects an exception once a test names that counter.

static const counters_exception_t counters_reachable_not_done[] = { { NULL, NULL } };

static bool counters_in_list( const counters_exception_t * list, const std::string & name )
{
    for ( int i = 0; list[i].name; i++ )
        if ( name == list[i].name )
            return true;
    return false;
}

static void test_counters_every_counter_is_named_by_a_test()
{
    const std::set<std::string> named = counters_named_by_tests();
    const std::set<std::string> counted = counters_named_by_sdk();

    int missed = 0;
    int not_done = 0;
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const std::string name = rocketnet_counter_name( i );
        if ( named.count( name ) || counters_in_list( counters_no_code_path, name ) )
            continue;
        if ( counters_in_list( counters_reachable_not_done, name ) )
        {
            printf( "NOT-DONE: reachable counter named by no test: ROCKETNET_COUNTER_%s\n", name.c_str() );
            not_done++;
            continue;
        }
        printf( "counter named by no test: ROCKETNET_COUNTER_%s\n", name.c_str() );
        missed++;
    }
    if ( missed )
        printf( "%d of %d counters are named by no test\n", missed, ROCKETNET_NUM_COUNTERS );
    if ( not_done )
        printf( "NOT-DONE: %d reachable counters are named by no test and are reported, not exempted\n", not_done );
    check( missed == 0 );

    // a listed counter that gained a code path is a counter a test can reach
    for ( int i = 0; counters_no_code_path[i].name; i++ )
    {
        if ( counted.count( counters_no_code_path[i].name ) )
            printf( "counter listed as having no code path now has one: ROCKETNET_COUNTER_%s\n", counters_no_code_path[i].name );
        check( !counted.count( counters_no_code_path[i].name ) );
        check( rocketnet_counter_name( counters_find( counters_no_code_path[i].name ) ) != NULL );
    }

    // a not-done counter has its code path, and no test names it yet; the
    // reason is printed beside the NOT-DONE line
    for ( int i = 0; counters_reachable_not_done[i].name; i++ )
    {
        const char * name = counters_reachable_not_done[i].name;
        if ( !counted.count( name ) )
            printf( "counter listed as not done has no code path: ROCKETNET_COUNTER_%s\n", name );
        check( counted.count( name ) );
        if ( named.count( name ) )
            printf( "counter listed as not done is named by a test now: ROCKETNET_COUNTER_%s\n", name );
        check( !named.count( name ) );
        printf( "NOT-DONE: ROCKETNET_COUNTER_%s: %s\n", name, counters_reachable_not_done[i].why );
    }
}

REGISTER_UNIT_TEST( test_counters_every_counter_is_named_by_a_test );

// ---------------------------------------------------------------------------
// 2: an outcome sits in at most one family

// An outcome counted from two ladders balances only while it stays zero: the
// request ladder and the response ladder both counted
// CONNECTION_REQUEST_INTERNAL_ERROR, so the two families held only while it
// was 0. The family column gives each counter one family; this test reads the
// ladders' own source and fails when a ladder counts a counter that sums into
// another family. The ladder functions named here are the ones that own a
// RECEIVED counter in their own body; the sealed types share one table-driven
// ladder in each protocol file, and a counter in a table row has one place by
// construction.

struct counters_ladder_t
{
    const char * file;
    const char * function;
    const char * received;      // the RECEIVED counter whose outcomes this function counts
};

static const counters_ladder_t counters_ladders[] =
{
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_protocol_process_connection_request", "CONNECTION_REQUEST_RECEIVED" },
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_process_opened_request",              "CONNECTION_REQUEST_RECEIVED" },
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_process_connection_response",         "CONNECTION_RESPONSE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_cookie",                      "CLIENT_COOKIE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_challenge",                   "CLIENT_CHALLENGE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_denied",                      "CLIENT_DENIED_RECEIVED" },
};

// the text of a function defined at the start of a line: from its name to the
// closing brace at the start of a line

static bool counters_function_body( const std::string & text, const char * function, std::string & body )
{
    const std::string needle = std::string( " " ) + function + "(";
    size_t at = 0;
    while ( ( at = text.find( needle, at ) ) != std::string::npos )
    {
        const size_t line_start = text.rfind( '\n', at ) + 1;
        const char first = text[line_start];
        // a definition starts a line with its return type; a call or a prototype does not
        if ( first != ' ' && first != '\t' && first != '/' && first != '#' && first != '\n' )
        {
            const size_t open = text.find( "\n{\n", at );
            const size_t semicolon = text.find( ';', at );
            if ( open != std::string::npos && ( semicolon == std::string::npos || open < semicolon ) )
            {
                const size_t close = text.find( "\n}\n", open );
                if ( close == std::string::npos )
                    return false;
                body = text.substr( open, close - open );
                return true;
            }
        }
        at += needle.size();
    }
    return false;
}

// every counter a text increments: ROCKETNET_COUNTER_<NAME> ] ++

static std::vector<std::string> counters_incremented( const std::string & text )
{
    std::vector<std::string> names;
    static const char prefix[] = "ROCKETNET_COUNTER_";
    const size_t prefix_bytes = sizeof(prefix) - 1;
    size_t at = 0;
    while ( ( at = text.find( prefix, at ) ) != std::string::npos )
    {
        size_t end = at + prefix_bytes;
        while ( end < text.size() && counters_is_identifier_char( text[end] ) )
            end++;
        size_t after = end;
        while ( after < text.size() && text[after] == ' ' )
            after++;
        if ( after < text.size() && text[after] == ']' )
        {
            after++;
            while ( after < text.size() && text[after] == ' ' )
                after++;
            if ( text.compare( after, 2, "++" ) == 0 )
                names.push_back( text.substr( at + prefix_bytes, end - at - prefix_bytes ) );
        }
        at = end;
    }
    return names;
}

static void test_counters_every_outcome_in_at_most_one_family()
{
    int shared = 0;
    for ( size_t i = 0; i < sizeof(counters_ladders) / sizeof(counters_ladders[0]); i++ )
    {
        const counters_ladder_t & ladder = counters_ladders[i];

        std::string text;
        check( counters_read_file( ladder.file, text ) );
        std::string body;
        if ( !counters_function_body( text, ladder.function, body ) )
            printf( "ladder function not found: %s in %s\n", ladder.function, ladder.file );
        check( counters_function_body( text, ladder.function, body ) );

        const int received = counters_find( ladder.received );
        check( received >= 0 );

        const std::vector<std::string> counted = counters_incremented( body );

        // a function that counts nothing is a function this audit lost
        check( !counted.empty() );

        for ( size_t j = 0; j < counted.size(); j++ )
        {
            const int counter = counters_find( counted[j].c_str() );
            check( counter >= 0 );
            const int family = counters_rows[counter].family;
            if ( counter == received || family == ROCKETNET_COUNTER_NONE || family == received )
                continue;
            printf( "ROCKETNET_COUNTER_%s is counted in %s (the %s ladder) but sums into %s\n",
                    counted[j].c_str(), ladder.function, ladder.received, counters_rows[family].name );
            shared++;
        }
    }
    check( shared == 0 );

    // the two connection ladders each have an internal error of their own
    check( counters_rows[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR].family == ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED );
    check( counters_rows[ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR].family == ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED );
}

REGISTER_UNIT_TEST( test_counters_every_outcome_in_at_most_one_family );

// ---------------------------------------------------------------------------
// 3: conservation reports the first unbalanced family

// a block in which one packet of the family's type arrived and took the
// family's first outcome, with every ancestor family balanced the same way

static void counters_balanced_family( struct rocketnet_counters_t * counters, int received, int outcome )
{
    counters->values[outcome]++;
    for ( int c = received; c != ROCKETNET_COUNTER_NONE; c = counters_rows[c].family )
        counters->values[c]++;
}

static int counters_first_outcome( int received )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        if ( counters_rows[i].family == received )
            return i;
    return -1;
}

#if ROCKETNET_ASSERTS
static int counters_expected_null_asserts = 0;

static void counters_expected_null_assert( const char *, const char *, const char *, int )
{
    counters_expected_null_asserts++;
}
#endif

static void test_counters_conservation_reports_the_first_unbalanced_family()
{
    char report[256];

    // a fresh block is balanced, and says nothing
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    snprintf( report, sizeof( report ), "%s", "not written" );
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 1 );
    check( report[0] == '\0' );

    // one request refused for its size: the arrival and the one outcome
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED]++;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE]++;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 1 );
    check( rocketnet_counters_conserved( &counters, NULL, 0 ) == 1 );

    // the outcome removed: the request family is named, with both sums
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE]--;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 0 );
    check( strcmp( report, "CONNECTION_REQUEST_RECEIVED received=1 outcomes=0" ) == 0 );
    check( rocketnet_counters_conserved( &counters, NULL, 0 ) == 0 );

    // an extra outcome is as unbalanced as a missing one
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] += 2;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 0 );
    check( strcmp( report, "CONNECTION_REQUEST_RECEIVED received=1 outcomes=2" ) == 0 );

    // side counters are outside every family: they move without unbalancing
    memset( &counters, 0, sizeof(counters) );
    counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] += 5;
    counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] += 3;
    counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] += 7;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 1 );

    // every family in the list: balanced with one outcome, and unbalanced,
    // naming exactly that family, when the outcome is taken away
    int families = 0;
    for ( int received = 0; received < ROCKETNET_NUM_COUNTERS; received++ )
    {
        if ( !counters_is_received( received ) )
            continue;
        families++;
        const int outcome = counters_first_outcome( received );
        check( outcome >= 0 );

        memset( &counters, 0, sizeof(counters) );
        counters_balanced_family( &counters, received, outcome );
        if ( !rocketnet_counters_conserved( &counters, report, sizeof(report) ) )
            printf( "family %s does not balance with one outcome: %s\n", counters_rows[received].name, report );
        check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 1 );

        counters.values[outcome]--;
        check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 0 );
        char expected[256];
        snprintf( expected, sizeof(expected), "%s received=1 outcomes=0", counters_rows[received].name );
        if ( strcmp( report, expected ) != 0 )
            printf( "family %s reported as: %s\n", counters_rows[received].name, report );
        check( strcmp( report, expected ) == 0 );
    }
    check( families == 15 );               // seven at the server, eight at the client

    // two families unbalanced: the one whose RECEIVED counter comes first
    memset( &counters, 0, sizeof(counters) );
    counters.values[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] = 2;
    counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED] = 1;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 0 );
    check( strcmp( report, "CONNECTION_RESPONSE_RECEIVED received=1 outcomes=0" ) == 0 );
    counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING] = 1;
    check( rocketnet_counters_conserved( &counters, report, sizeof(report) ) == 0 );
    check( strcmp( report, "PAYLOAD_RECEIVED received=2 outcomes=0" ) == 0 );

    // a short report buffer is cut, never overrun, and stays terminated
    char tiny[10];
    memset( tiny, 'x', sizeof(tiny) );
    check( rocketnet_counters_conserved( &counters, tiny, sizeof(tiny) ) == 0 );
    check( memchr( tiny, '\0', sizeof(tiny) ) != NULL );
    check( strncmp( tiny, "PAYLOAD_RE", 9 ) == 0 );

    // a NULL block is not conserved
    const uint64_t misuse_before = rocketnet_api_misuse_count();
#if ROCKETNET_ASSERTS
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    counters_expected_null_asserts = 0;
    rocketnet_assert_function( counters_expected_null_assert );
#endif
    check( rocketnet_counters_conserved( NULL, report, sizeof(report) ) == 0 );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );
    check( counters_expected_null_asserts == 1 );
#endif
    check( rocketnet_api_misuse_count() == misuse_before + 1 );
}

REGISTER_UNIT_TEST( test_counters_conservation_reports_the_first_unbalanced_family );

// ---------------------------------------------------------------------------
// 4: the exact-delta helper fails on a counter nobody named

static void test_counters_expect_fails_on_an_unnamed_move()
{
    struct rocketnet_counters_t before;
    memset( &before, 0, sizeof(before) );
    before.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] = 4;

    struct rocketnet_counters_t after = before;
    after.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] += 1;
    after.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] += 1;

    char report[4096];

    // the two named moves, exactly: passes, and says nothing
    snprintf( report, sizeof( report ), "%s", "not written" );
    check( rocketnet_counters_expect_core( &before, &after,
                                           { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                             { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } },
                                           report, sizeof(report) ) );
    check( report[0] == '\0' );

    // a third counter moved: false, and the report names that counter
    after.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] += 1;
    check( !rocketnet_counters_expect_core( &before, &after,
                                            { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                              { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } },
                                            report, sizeof(report) ) );
    check( strstr( report, "CONNECTION_REQUEST_NO_SLOTS" ) != NULL );
    check( strstr( report, "unnamed move" ) != NULL );

    // the report shows every nonzero delta by name, the named ones included
    check( strstr( report, "CONNECTION_REQUEST_RECEIVED" ) != NULL );
    check( strstr( report, "CONNECTION_REQUEST_WRONG_SIZE" ) != NULL );

    // a counter that did not move is not in the report
    check( strstr( report, "KEEP_ALIVE_RECEIVED" ) == NULL );

    // a named counter that moved by a different amount fails
    after.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] -= 1;
    check( !rocketnet_counters_expect_core( &before, &after,
                                            { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 2 },
                                              { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } },
                                            report, sizeof(report) ) );
    check( strstr( report, "wrong delta" ) != NULL );

    // a named counter that did not move at all fails, and a named delta of
    // zero holds a counter still
    check( !rocketnet_counters_expect_core( &before, &after,
                                            { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                              { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 },
                                              { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, 1 } },
                                            report, sizeof(report) ) );
    check( strstr( report, "KEEP_ALIVE_RECEIVED" ) != NULL );
    check( rocketnet_counters_expect_core( &before, &after,
                                           { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                             { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 },
                                             { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, 0 } },
                                           report, sizeof(report) ) );

    // nothing moved and nothing named
    check( rocketnet_counters_expect_core( &before, &before, {}, report, sizeof(report) ) );
    check( rocketnet_counters_expect_core( &before, &before, {}, NULL, 0 ) );

    // the asserting form passes on the exact move
    rocketnet_counters_expect( &before, &after, { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                                  { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } } );

    struct rocketnet_counters_t balanced;
    memset( &balanced, 0, sizeof(balanced) );
    rocketnet_counters_assert_conserved( &balanced );
}

REGISTER_UNIT_TEST( test_counters_expect_fails_on_an_unnamed_move );

// rocketnet_counter_value checks counters once, through rocketnet_api_null_guard:
// a NULL is a counted misuse that reads 0, and an index outside the table reads 0.

static void counter_value_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
}

static void test_counter_value_null_and_out_of_range()
{
    rocketnet_assert_function( counter_value_quiet_assert );

    const uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_counter_value( NULL, 0 ) == 0 );
    check( rocketnet_api_misuse_count() == before + 1 );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    counters.values[3] = 42;
    check( rocketnet_counter_value( &counters, -1 ) == 0 );
    check( rocketnet_counter_value( &counters, ROCKETNET_NUM_COUNTERS ) == 0 );
    check( rocketnet_counter_value( &counters, 3 ) == 42 );
    check( rocketnet_api_misuse_count() == before + 1 );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_counter_value_null_and_out_of_range );
