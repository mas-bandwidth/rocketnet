/*
    rocketnet transport bench: Network.framework (#100)

    Darwin candidate 3, Apple's sanctioned user-space stack (Skywalk).
    The architecture is callback-driven -- one datagram per receive
    completion on a dispatch queue, sends enqueued with completions --
    which is the opposite shape from the pump seam's pull-a-batch model.
    This harness measures that architecture as itself rather than forcing
    it through a batch loop it does not have:

    - recv: nw_listener + nw_connection_receive_message re-armed per
      datagram; the window accounting runs inside the completion, batch
      size 1 by construction. "syscalls" in the WINDOW line counts
      receive completions -- the framework owns the real syscalls, so
      per-packet kernel cost attribution belongs to the profiler, not
      the harness.
    - send: nw_connection_send in batches of 32 with a semaphore waiting
      for the batch's completions -- the pump's fill-then-flush shape,
      as close as the API allows.

    bench_nw recv <port> <warmup> <window> <windows>
    bench_nw send <ip> <port> <seconds> [drop-half] [pace=<pps>]

    Darwin only. Wire-compatible with the portable harness on both sides.
*/

#include "bench_common.h"

#include <Network/Network.h>
#include <dispatch/dispatch.h>

static struct bench_windows_t bench_windows;
static struct bench_send_health_t nw_send_health;

/*
    Network.framework reports failures as an nw_error_t rather than an errno,
    so the shared rule is reached through a translation. Everything that is
    not the ordinary teardown of a finished connection is a receiver that has
    stopped receiving, and a receiver that has stopped receiving reports a
    rate of zero, which reads exactly like a slow transport (#98 item 5).

    Before this, the completion handler returned on ANY error and re-armed
    nothing, so a broken listener produced a clean-looking window of no
    packets. That is the defect #98 filed, in the one harness whose callback
    shape hid it.
*/

static const int nw_teardown_errors[] = { ECANCELED, ENOTCONN, ECONNRESET, ECONNABORTED };

static void nw_check_error( const char * call, nw_error_t error )
{
    int code = EAGAIN;                          /* no error: the ordinary case */

    if ( error != NULL )
    {
        code = nw_error_get_error_code( error );
        if ( nw_error_get_error_domain( error ) != nw_error_domain_posix )
            code = EIO;                         /* not an errno; refuse under a code never tolerated */
    }

    bench_receive_refused_or_empty_except( call, code, nw_teardown_errors,
                                           (int) ( sizeof( nw_teardown_errors ) / sizeof( nw_teardown_errors[0] ) ) );
}

static void nw_receive_loop( nw_connection_t connection )
{
    nw_connection_receive_message( connection, ^( dispatch_data_t content, nw_content_context_t context, bool is_complete, nw_error_t error ) {
      (void) context;
      (void) is_complete;

        /* checked on EVERY completion, success included, so the fault switch
           can turn a healthy run into the failure a broken listener produces */
      nw_check_error( "nw_connection_receive_message", error );

      if ( error != NULL )
      {
            /* A tolerated teardown: this connection stops re-arming and the
               listener keeps running. If it was the ONLY connection, the run
               now produces no further windows and simply hangs.

               That hang is loud in the one way that counts -- it yields no
               number at all, and the doctrine here is aimed at numbers that
               are not measurements, which a run that never finishes cannot
               produce. But it is loud by accident rather than by design, so
               it at least says why it went quiet. No new state: this is the
               one place the loop stops. */

          printf( "nw receive loop stopped re-arming: %s (errno %d). If no further WINDOW lines follow, this connection was the only one and the run has nothing left to measure.\n",
                  nw_error_get_error_domain( error ) == nw_error_domain_posix ? "posix" : "non-posix",
                  nw_error_get_error_code( error ) );
          fflush( stdout );

          return;
      }

      uint64_t t0 = bench_time_ns();
      __block uint64_t batch_bytes = 0;
      __block uint64_t batch_fold = 0;
      int batch_packets = 0;

      if ( content != NULL )
      {
          dispatch_data_apply( content, ^bool( dispatch_data_t region, size_t offset, const void * buffer, size_t size ) {
            (void) region;
            (void) offset;
            batch_fold += bench_fold_packet( (const uint8_t *) buffer, (int) size );
            batch_bytes += (uint64_t) size;
            return true;
          } );
          batch_packets = 1;
      }

      uint64_t batch_ns = bench_time_ns() - t0;

      if ( bench_window_tick( &bench_windows, batch_ns, batch_packets, batch_bytes, 1, content == NULL ? 1 : 0, batch_fold, 1 ) )
          exit( 0 );

      nw_receive_loop( connection );
    } );
}

static void run_receiver( int port, double warmup, double window, int windows )
{
    bench_windows_init( &bench_windows, warmup, window, windows );

    nw_parameters_t parameters = nw_parameters_create_secure_udp( NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION );
    nw_parameters_set_local_only( parameters, false );
    nw_parameters_set_reuse_local_address( parameters, true );

    char port_string[16];
    snprintf( port_string, sizeof( port_string ), "%d", port );

    nw_endpoint_t local = nw_endpoint_create_host( "0.0.0.0", port_string );
    nw_parameters_set_local_endpoint( parameters, local );

    nw_listener_t listener = nw_listener_create( parameters );
    if ( listener == NULL )
    {
        fprintf( stderr, "nw_listener_create failed\n" );
        exit( 1 );
    }

    dispatch_queue_t queue = dispatch_queue_create( "bench_nw.recv", DISPATCH_QUEUE_SERIAL );
    nw_listener_set_queue( listener, queue );

    nw_listener_set_new_connection_handler( listener, ^( nw_connection_t connection ) {
      nw_retain( connection );
      nw_connection_set_queue( connection, queue );
      nw_connection_start( connection );
      nw_receive_loop( connection );
    } );

    nw_listener_set_state_changed_handler( listener, ^( nw_listener_state_t state, nw_error_t error ) {
      if ( state == nw_listener_state_failed )
      {
          fprintf( stderr, "nw listener failed: %d\n", error ? nw_error_get_error_code( error ) : 0 );
          exit( 1 );
      }
      if ( state == nw_listener_state_ready )
          printf( "nw listener ready on port %d\n", port );
    } );

    nw_listener_start( listener );
    dispatch_main();
}

static void run_sender( const char * ip, int port, double seconds, struct bench_sender_options_t options )
{
    nw_parameters_t parameters = nw_parameters_create_secure_udp( NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION );

    char port_string[16];
    snprintf( port_string, sizeof( port_string ), "%d", port );

    nw_endpoint_t endpoint = nw_endpoint_create_host( ip, port_string );
    nw_connection_t connection = nw_connection_create( endpoint, parameters );

    dispatch_queue_t queue = dispatch_queue_create( "bench_nw.send", DISPATCH_QUEUE_SERIAL );
    nw_connection_set_queue( connection, queue );

    dispatch_semaphore_t ready = dispatch_semaphore_create( 0 );

    nw_connection_set_state_changed_handler( connection, ^( nw_connection_state_t state, nw_error_t error ) {
      (void) error;
      if ( state == nw_connection_state_ready )
          dispatch_semaphore_signal( ready );
      if ( state == nw_connection_state_failed )
      {
          fprintf( stderr, "nw connection failed\n" );
          exit( 1 );
      }
    } );
    nw_connection_start( connection );
    dispatch_semaphore_wait( ready, DISPATCH_TIME_FOREVER );

    uint8_t packet[BENCH_MAX_PACKET];

    __block uint64_t sent = 0;

    uint64_t completions = 0;
    uint64_t batch_index = 0;
    uint64_t sequence = 0;
    uint32_t flow = 0;
    (void) completions;

    uint64_t start = bench_time_ns();
    uint64_t deadline = start + (uint64_t) ( seconds * 1e9 );

    dispatch_semaphore_t batch_done = dispatch_semaphore_create( 0 );

    for ( ;; )
    {
        int size = bench_sizes[batch_index % BENCH_NUM_SIZES];
        int skip = options.drop_half && ( batch_index & 1 );

        batch_index++;

        if ( bench_pace_wait( start, sent, options.pace_pps ) >= deadline )
            break;

        if ( !skip )
        {
            /* fill-then-flush: enqueue the batch, wait for its completions
               so the queue depth stays bounded at one seam batch */

            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                bench_fill_packet( packet, size, flow, sequence++ );
                flow = ( flow + 1 ) % BENCH_NUM_FLOWS;

                dispatch_data_t data = dispatch_data_create( packet, (size_t) size, queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT );
                nw_connection_send( connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^( nw_error_t error ) {
                    /* a send completion that failed is not a sent packet, and
                       a long run of them is a host that cannot do this */
                  if ( error == NULL )
                      sent += bench_send_ok( &nw_send_health, "nw_connection_send" );
                  else
                      sent += bench_send_refused( &nw_send_health, "nw_connection_send",
                                                  nw_error_get_error_domain( error ) == nw_error_domain_posix
                                                      ? nw_error_get_error_code( error )
                                                      : EIO );
                  dispatch_semaphore_signal( batch_done );
                } );
                dispatch_release( data );
            }

            for ( int i = 0; i < BENCH_BATCH_SIZE; i++ )
            {
                dispatch_semaphore_wait( batch_done, DISPATCH_TIME_FOREVER );
            }
        }
    }

    double elapsed = (double) ( bench_time_ns() - start ) / 1e9;

    bench_send_validate( &nw_send_health );
    printf( "SENDER pkts=%llu secs=%.3f pps=%.0f syscalls=0 back_pressure=%llu refused=%llu\n",
            (unsigned long long) sent, elapsed, (double) sent / elapsed,
            (unsigned long long) nw_send_health.back_pressure, (unsigned long long) nw_send_health.refused );
    exit( 0 );
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    bench_fault_init();

    if ( argc >= 6 && strcmp( argv[1], "recv" ) == 0 )
    {
        run_receiver( atoi( argv[2] ), atof( argv[3] ), atof( argv[4] ), atoi( argv[5] ) );
    }
    else if ( argc >= 5 && strcmp( argv[1], "send" ) == 0 )
    {
        run_sender( argv[2], atoi( argv[3] ), atof( argv[4] ), bench_parse_sender_options( argc, argv, 5 ) );
    }
    else
    {
        fprintf( stderr, "usage: bench_nw recv <port> <warmup> <window> <windows>\n"
                         "       bench_nw send <ip> <port> <seconds> [drop-half] [pace=<pps>]\n" );
        return 1;
    }

    return 0;
}
