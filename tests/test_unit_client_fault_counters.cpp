/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Client I/O transport faults and the public counters (sdk/rocketnet_client_io.h).
// A send driver that returns a consumed count out of range is a transport
// fault, counted on the I/O state; no public counter names it, so the fold at
// update leaves PREFILTER_TOO_LARGE alone. Single-threaded: the fake client
// transport (tests/fake_client_driver.h), no sockets, no real time; the clock
// argument is 1000.0.

#include "test_unit_registry.h"
#include "fake_client_driver.h"
#include "client_io_rig.h"

#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_counters.h"

#include <string.h>

static double client_fault_counters_time( void * context )
{
    return *(const double *) context;
}

static int client_fault_counters_send_over( void *, const rocketnet_platform_send_t *, int count )
{
    return count + 1;
}

static void test_client_send_fault_is_not_counted_too_large()
{
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.send_batch = rocketnet_fake_client_send_batch;
    operations.send_batch_progress = client_fault_counters_send_over;

    rocketnet_client_transport_io_config_t config = { "fake", NULL, NULL };
    rocketnet_address_t bind = {};
    rocketnet_client_transport_io_t transport = {};
    rocketnet_unit_check( rocketnet_client_transport_io_open( &operations, &fake, &config, &bind, &transport ) );

    rocketnet_client_io_rig_t * rig = rocketnet_client_io_rig_create();
    rig->counts[0][0] = 1;
    rocketnet_client_io_rig_slot( rig, 0, 0 )[0] = 0x30;

    double now = 1000.0;
    rocketnet_client_io_t io = {};
    rocketnet_client_io_rig_init( rig, &io, &transport, client_fault_counters_time, &now );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    io.prefilter_too_large = &counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE];

    rocketnet_address_t to = {};
    rocketnet_address_load_ipv4( &to, 0x7f000001, 9600 );
    rocketnet_unit_check( rocketnet_client_io_hand_off( &io, 1, &to ) == ROCKETNET_CLIENT_RINGS_HANDED_OFF );

    rocketnet_client_io_step( &io, false );
    rocketnet_unit_check( io.transport_faults == 1 );

    rocketnet_client_io_fold_counters( &io );
    rocketnet_unit_check( counters.values[ROCKETNET_COUNTER_PREFILTER_TOO_LARGE] == 0 );

    rocketnet_client_transport_io_close( &transport );
    rocketnet_client_io_rig_destroy( rig );
}
REGISTER_UNIT_TEST( test_client_send_fault_is_not_counted_too_large );
