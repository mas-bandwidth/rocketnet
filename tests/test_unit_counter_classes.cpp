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

static const counters_row_t counters_rows[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_COUNTER_TEST_ROW( name, klass, family ) { #name, ROCKETNET_COUNTER_CLASS_##klass, ROCKETNET_COUNTER_##family },
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_TEST_ROW )
#undef ROCKETNET_COUNTER_TEST_ROW
};

// The counter list at BASE, in index order. Before the 1.0 release a retired
// counter is removed and the rest renumbered; from the 1.0 release on the
// indices never move. A counter appended since BASE is listed after, in the
// order it was appended.

#if ROCKETNET_DEV
static const char * const counters_base_names[] = {
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
    "CONNECTION_RESPONSE_ALREADY_CONNECTED",
    "CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED",
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

static const char * const counters_appended_names[] = {
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
    "CONNECTION_RESPONSE_PADDING_NOT_ZERO",
    "CONNECTION_REQUEST_CLAIM_TABLE_FULL",
    "PAYLOAD_BATCH_FULL",
    "XDP_RECEIVE_FRAME_DROPPED",
};
#endif // #if ROCKETNET_DEV

#define COUNTERS_NUM_BASE       ( (int) ( sizeof(counters_base_names) / sizeof(counters_base_names[0]) ) )
#define COUNTERS_NUM_APPENDED   ( (int) ( sizeof(counters_appended_names) / sizeof(counters_appended_names[0]) ) )

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

#if ROCKETNET_DEV
static void test_counters_every_counter_has_a_class()
{
    check( COUNTERS_NUM_BASE == 213 );
    check( ROCKETNET_NUM_COUNTERS == COUNTERS_NUM_BASE + COUNTERS_NUM_APPENDED );
    check( rocketnet_num_counters() == ROCKETNET_NUM_COUNTERS );

    int per_class[ROCKETNET_NUM_COUNTER_CLASSES];
    memset( per_class, 0, sizeof( per_class ) );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const char * expected = i < COUNTERS_NUM_BASE ? counters_base_names[i] : counters_appended_names[i - COUNTERS_NUM_BASE];
        const char * name = rocketnet_counter_name( i );
        check( name != NULL );
        if ( strcmp( name, expected ) != 0 )
        {
            printf( "counter %d is %s, expected %s\n", i, name, expected );
        }
        check( strcmp( name, expected ) == 0 );

        const int counter_class = rocketnet_counter_class( i );
        if ( counter_class < 0 || counter_class >= ROCKETNET_NUM_COUNTER_CLASSES )
        {
            printf( "counter %s has no class\n", name );
        }
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
            {
                printf( "counter %s sums into %s, which is not an arrival counter\n", name, counters_rows[family].name );
            }
            check( counters_rows[family].counter_class == ROCKETNET_COUNTER_CLASS_ARRIVAL );
        }

        // every arrival counter heads a family, but the packet entry's
        // PACKETS_RECEIVED, which counts before any type dispatches

        if ( counters_rows[i].counter_class == ROCKETNET_COUNTER_CLASS_ARRIVAL && i != ROCKETNET_COUNTER_PACKETS_RECEIVED )
        {
            if ( !counters_is_received( i ) )
            {
                printf( "arrival counter %s has no outcomes\n", name );
            }
            check( counters_is_received( i ) );
        }
    }

    // exactly four classes, each used, and each named as the API names it

    check( ROCKETNET_NUM_COUNTER_CLASSES == 4 );
    for ( int c = 0; c < ROCKETNET_NUM_COUNTER_CLASSES; c++ )
    {
        check( per_class[c] > 0 );
    }
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
    check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_REQUEST_CLAIM_TABLE_FULL ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_PAYLOAD_BATCH_FULL ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_RECEIVE_FRAME_DROPPED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
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
#endif // #if ROCKETNET_DEV

// ---------------------------------------------------------------------------
// 3: conservation reports the first unbalanced family

// a block in which one packet of the family's type arrived and took the
// family's first outcome, with every ancestor family balanced the same way

static void counters_balanced_family( struct rocketnet_counters_t * counters, int received, int outcome )
{
    counters->values[outcome]++;
    for ( int c = received; c != ROCKETNET_COUNTER_NONE; c = counters_rows[c].family )
    {
        counters->values[c]++;
    }
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
#endif // #if ROCKETNET_ASSERTS

static void test_counters_conservation_reports_the_first_unbalanced_family()
{
    char report[256];

    // a fresh block is balanced, and says nothing

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    snprintf( report, sizeof( report ), "%s", "not written" );
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 1 );
    check( report[0] == '\0' );

    // one request refused for its size: the arrival and the one outcome

    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED]++;
    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE]++;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 1 );
    check( rocketnet_counters_conserved( &counters, NULL, 0 ) == 1 );

    // the outcome removed: the request family is named, with both sums

    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE]--;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 0 );
    check( strcmp( report, "CONNECTION_REQUEST_RECEIVED received=1 outcomes=0" ) == 0 );
    check( rocketnet_counters_conserved( &counters, NULL, 0 ) == 0 );

    // an extra outcome is as unbalanced as a missing one

    counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] += 2;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 0 );
    check( strcmp( report, "CONNECTION_REQUEST_RECEIVED received=1 outcomes=2" ) == 0 );

    // side counters are outside every family: they move without unbalancing

    memset( &counters, 0, sizeof( counters ) );
    counters.values[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] += 5;
    counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] += 3;
    counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] += 7;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 1 );

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

        memset( &counters, 0, sizeof( counters ) );
        counters_balanced_family( &counters, received, outcome );
        if ( !rocketnet_counters_conserved( &counters, report, sizeof( report ) ) )
        {
            printf( "family %s does not balance with one outcome: %s\n", counters_rows[received].name, report );
        }
        check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 1 );

        counters.values[outcome]--;
        check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 0 );

        char expected[256];
        snprintf( expected, sizeof( expected ), "%s received=1 outcomes=0", counters_rows[received].name );
        if ( strcmp( report, expected ) != 0 )
        {
            printf( "family %s reported as: %s\n", counters_rows[received].name, report );
        }
        check( strcmp( report, expected ) == 0 );
    }

    check( families == 15 );               // seven at the server, eight at the client

    // two families unbalanced: the one whose RECEIVED counter comes first

    memset( &counters, 0, sizeof( counters ) );
    counters.values[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] = 2;
    counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED] = 1;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 0 );
    check( strcmp( report, "CONNECTION_RESPONSE_RECEIVED received=1 outcomes=0" ) == 0 );
    counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING] = 1;
    check( rocketnet_counters_conserved( &counters, report, sizeof( report ) ) == 0 );
    check( strcmp( report, "PAYLOAD_RECEIVED received=2 outcomes=0" ) == 0 );

    // a short report buffer is cut, never overrun, and stays terminated

    char tiny[10];
    memset( tiny, 'x', sizeof( tiny ) );
    check( rocketnet_counters_conserved( &counters, tiny, sizeof( tiny ) ) == 0 );
    check( memchr( tiny, '\0', sizeof( tiny ) ) != NULL );
    check( strncmp( tiny, "PAYLOAD_RE", 9 ) == 0 );

    // a NULL block is not conserved

    const uint64_t misuse_before = rocketnet_api_misuse_count();
#if ROCKETNET_ASSERTS
    void ( *saved_assert )( const char *, const char *, const char *, int ) = rn_assert_function;
    counters_expected_null_asserts = 0;
    rocketnet_assert_function( counters_expected_null_assert );
#endif // #if ROCKETNET_ASSERTS
    check( rocketnet_counters_conserved( NULL, report, sizeof( report ) ) == 0 );
#if ROCKETNET_ASSERTS
    rocketnet_assert_function( saved_assert );
    check( counters_expected_null_asserts == 1 );
#endif // #if ROCKETNET_ASSERTS
    check( rocketnet_api_misuse_count() == misuse_before + 1 );
}

REGISTER_UNIT_TEST( test_counters_conservation_reports_the_first_unbalanced_family );

// ---------------------------------------------------------------------------
// 4: the exact-delta helper fails on a counter nobody named

static void test_counters_expect_fails_on_an_unnamed_move()
{
    struct rocketnet_counters_t before;
    memset( &before, 0, sizeof( before ) );
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
                                           report, sizeof( report ) ) );
    check( report[0] == '\0' );

    // a third counter moved: false, and the report names that counter

    after.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] += 1;
    check( !rocketnet_counters_expect_core( &before, &after,
                                            { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                              { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } },
                                            report, sizeof( report ) ) );
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
                                            report, sizeof( report ) ) );
    check( strstr( report, "wrong delta" ) != NULL );

    // a named counter that did not move at all fails, and a named delta of
    // zero holds a counter still

    check( !rocketnet_counters_expect_core( &before, &after,
                                            { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                              { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 },
                                              { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, 1 } },
                                            report, sizeof( report ) ) );
    check( strstr( report, "KEEP_ALIVE_RECEIVED" ) != NULL );
    check( rocketnet_counters_expect_core( &before, &after,
                                           { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 },
                                             { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 },
                                             { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, 0 } },
                                           report, sizeof( report ) ) );

    // nothing moved and nothing named

    check( rocketnet_counters_expect_core( &before, &before, {}, report, sizeof( report ) ) );
    check( rocketnet_counters_expect_core( &before, &before, {}, NULL, 0 ) );

    // the asserting form passes on the exact move

    rocketnet_counters_expect( &before, &after, { { ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED, 1 }, { ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE, 1 } } );

    struct rocketnet_counters_t balanced;
    memset( &balanced, 0, sizeof( balanced ) );
    rocketnet_counters_assert_conserved( &balanced );
}

REGISTER_UNIT_TEST( test_counters_expect_fails_on_an_unnamed_move );

// ---------------------------------------------------------------------------
// 5: the internal-error coverage entries are executable registered cases

// The source audit's identifier inventory is deliberately lexical: it cannot
// turn a comment, a zero-only check or a counter name in a table into branch
// coverage. These two entries are accounted as executed witnesses instead.
// The full unit driver runs every registry member; the named cases themselves
// drive the real protocol branches and use rocketnet_counters_expect with a
// positive delta. This case makes their registration part of the counter
// suite so neither witness can silently disappear from that full run.

static void test_connection_internal_error_counter_witnesses_are_registered()
{
#if ROCKETNET_DEV
    check( rocketnet_unit_file_find( "test_connection_request_internal_error_counter_executes" ) != NULL );
    check( rocketnet_unit_file_find( "test_connection_response_internal_error_counter_executes" ) != NULL );
#else
    check( rocketnet_unit_file_find( "test_connection_request_internal_error_counter_executes" ) == NULL );
    check( rocketnet_unit_file_find( "test_connection_response_internal_error_counter_executes" ) == NULL );
#endif // #if ROCKETNET_DEV
}

REGISTER_UNIT_TEST( test_connection_internal_error_counter_witnesses_are_registered );

// rocketnet_counter_value checks counters once, through rocketnet_api_null_guard:
// a NULL is a counted misuse that reads 0, and an index outside the table reads 0.

static void counter_value_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

static void test_counter_value_null_and_out_of_range()
{
    rocketnet_assert_function( counter_value_quiet_assert );

    const uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_counter_value( NULL, 0 ) == 0 );
    check( rocketnet_api_misuse_count() == before + 1 );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    counters.values[3] = 42;
    check( rocketnet_counter_value( &counters, -1 ) == 0 );
    check( rocketnet_counter_value( &counters, ROCKETNET_NUM_COUNTERS ) == 0 );
    check( rocketnet_counter_value( &counters, 3 ) == 42 );
    check( rocketnet_api_misuse_count() == before + 1 );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_counter_value_null_and_out_of_range );
