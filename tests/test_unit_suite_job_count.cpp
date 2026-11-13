/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The suite runner's job count against its Windows wait set. The Windows
// runner waits on every running case from a fixed array in one
// WaitForMultipleObjects call, so the job count must never exceed that array.
// It once held 8 handles while the count followed the cores and the memory
// (about 30 on captain: 64 cores, 48 GiB free), and the ninth running case
// wrote past it: the parent ./bin/test died with 0xC0000409 partway through
// the suite. The count is a function of its inputs here; no time, sockets or
// processes.

#include "test_unit_registry.h"
#include "suite_runner.h"

#define check rocketnet_unit_check

static void test_suite_job_count_fits_the_windows_wait_set()
{
    const uint64_t gib = 1024ull * 1024 * 1024;

    // one job per core, at least one, as many as the memory holds

    check( suite_job_count_for( 4, 0, 0, SUITE_WAIT_HANDLES ) == 4 );
    check( suite_job_count_for( 0, 0, 0, SUITE_WAIT_HANDLES ) == 1 );
    check( suite_job_count_for( 64, 1, 0, SUITE_WAIT_HANDLES ) == 1 );
    check( suite_job_count_for( 64, 10 * gib, 0, SUITE_WAIT_HANDLES ) == 6 );

    // 64 cores and 50 GiB plan 31 jobs, far past the old 8

    check( suite_job_count_for( 64, 50 * gib, 0, SUITE_WAIT_HANDLES ) == 31 );

    // the ceiling holds against the cores and against a requested count

    check( suite_job_count_for( 128, 0, 0, SUITE_WAIT_HANDLES ) == SUITE_WAIT_HANDLES );
    check( suite_job_count_for( 64, 50 * gib, 1000, SUITE_WAIT_HANDLES ) == SUITE_WAIT_HANDLES );
    check( suite_job_count_for( 4, 0, 9, SUITE_WAIT_HANDLES ) == 9 );
    check( suite_job_count_for( 4, 0, -3, SUITE_WAIT_HANDLES ) == 4 );

    // without a ceiling (POSIX) the count is not capped

    check( suite_job_count_for( 128, 0, 0, 0 ) == 128 );
    check( suite_job_count_for( 4, 0, 1000, 0 ) == 1000 );

    // the live count on this host, whatever its cores, memory and
    // ROCKETNET_TEST_JOBS, fits the Windows wait set

    check( suite_job_count() >= 1 );

    // and the live count is wired to this platform's ceiling: a requested
    // count far past it reads exactly the ceiling on Windows, on any host, and
    // the request itself elsewhere (VERIFY-4 W10: no CI host asks for more
    // than 64 jobs, so a bound on suite_job_count() alone could not go red).
    // The request goes in as an argument; a unit case never writes the
    // environment (tools/check_unit_purity.py).

    const int asked_past = suite_job_count_requested( 1000 );
#if defined( _WIN32 )
    check( suite_job_count() <= SUITE_WAIT_HANDLES );
    check( asked_past == SUITE_WAIT_HANDLES );
#else
    check( asked_past == 1000 );
#endif // #if defined( _WIN32 )
}

REGISTER_UNIT_TEST( test_suite_job_count_fits_the_windows_wait_set );

// The default job count follows the cpus the process may run on, not the
// host's: a 48-cpu bench whose run is held to 5 cpus (a nova-reserve slot)
// ran 48 jobs on 5 cpus before (std::thread::hardware_concurrency()). It plans
// SUITE_JOBS_PER_USABLE_CPU (3) jobs a usable cpu, never more than the host's
// count. The mask goes in as an argument; the live count is checked against
// the live mask.
static void test_suite_job_count_follows_the_cpu_mask()
{
    const uint64_t gib = 1024ull * 1024 * 1024;

    // cpus 4-8 of a 48-cpu host, as a nova-reserve slot holds them: 15 jobs

    const uint64_t slot[1] = { 0x1F0ull };
    check( suite_cpus_in_mask( slot, 1 ) == 5 );
    check( suite_planned_cpus( suite_cpus_in_mask( slot, 1 ), 48 ) == 15 );
    check( suite_job_count_for( suite_planned_cpus( suite_cpus_in_mask( slot, 1 ), 48 ), 300 * gib, 0, 0 ) == 15 );

    // the whole host plans the host's count, as before; a wide slot stops at
    // the host's count; an unknown host count plans the usable cpus

    const uint64_t host[1] = { ( 1ull << 48 ) - 1 };
    check( suite_cpus_in_mask( host, 1 ) == 48 );
    check( suite_planned_cpus( suite_cpus_in_mask( host, 1 ), 48 ) == 48 );
    check( suite_planned_cpus( 20, 48 ) == 48 );
    check( suite_planned_cpus( 4, 0 ) == 4 );

    // a mask over two words, and an empty mask

    const uint64_t split[2] = { 1ull << 3, ( 1ull << 0 ) | ( 1ull << 63 ) };
    check( suite_cpus_in_mask( split, 2 ) == 3 );
    check( suite_planned_cpus( suite_cpus_in_mask( split, 2 ), 64 ) == 9 );
    const uint64_t none[2] = { 0, 0 };
    check( suite_cpus_in_mask( none, 2 ) == 0 );
    check( suite_job_count_for( suite_planned_cpus( suite_cpus_in_mask( none, 2 ), 48 ), 300 * gib, 0, 0 ) == 1 );

    // the memory budget still caps the planned count, and a request still wins

    check( suite_job_count_for( suite_planned_cpus( suite_cpus_in_mask( slot, 1 ), 48 ), 10 * gib, 0, 0 ) == 6 );
    check( suite_job_count_for( suite_planned_cpus( suite_cpus_in_mask( slot, 1 ), 48 ), 300 * gib, 4, 0 ) == 4 );

    // live: with no request, this process never plans more than
    // SUITE_JOBS_PER_USABLE_CPU jobs a cpu it may run on

    const unsigned usable = suite_usable_cpus();
    check( usable >= 1 );
    check( suite_job_count_requested( 0 ) <= (int) ( usable * SUITE_JOBS_PER_USABLE_CPU ) );
}

REGISTER_UNIT_TEST( test_suite_job_count_follows_the_cpu_mask );

// A run that collected no case, a suite or a shard with nothing registered
// for it, fails: an empty suite is a lost registration, not a pass. The
// failure line is printed instead of the pass banner.
static void test_suite_finish_fails_when_no_case_ran()
{
    std::vector<suite_result> results;
    check( suite_finish( results, true ) == 1 );
    check( suite_finish( results, false ) == 1 );
}

REGISTER_UNIT_TEST( test_suite_finish_fails_when_no_case_ran );

// A case the runner failed (its memory bound, its time limit) gets the
// runner's reason line even when the case printed a check line while passing,
// as a case that exercises the check path does; a case that failed itself and
// printed its own check line does not get a second one.
static void test_suite_runner_reason_is_printed_over_a_check_line()
{
    suite_result over = suite_result();
    over.name = "test_planted";
    over.failed = false;
    over.peak_rss_mb = 1506;
    over.rss_over_bound = false;
    over.output = "check failed: no case ran\n";
    suite_check_rss( over, 698, true );
    check( over.failed && over.rss_over_bound );
    check( over.detail == "test_planted peak_rss_mb=1506 bound_mb=698" );
    check( suite_runner_failed( over ) );
    check( suite_failure_line_needed( over ) );

    suite_result slow = suite_result();
    slow.name = "test_planted";
    slow.failed = true;
    slow.rss_over_bound = false;
    slow.detail = "test_planted exceeded 120s";
    slow.output = "check failed: no case ran\n";
    check( suite_runner_failed( slow ) );
    check( suite_failure_line_needed( slow ) );

    suite_result own = suite_result();
    own.name = "test_planted";
    own.failed = true;
    own.rss_over_bound = false;
    own.detail = "test_planted exit 1";
    own.output = "check failed: x == 1, function f, file g, line 2\n";
    check( !suite_runner_failed( own ) );
    check( !suite_failure_line_needed( own ) );

    own.output = "no check line\n";
    check( suite_failure_line_needed( own ) );
}

REGISTER_UNIT_TEST( test_suite_runner_reason_is_printed_over_a_check_line );

// A case passes only by returning. While it runs, the exit guard is armed for
// the process that started it, so an exit( 0 ) inside the case fails it; a
// child the case forks (another pid) and the time after the case returned are
// not guarded. The guard's state is read inside and after a case here; the
// exit itself is planted by hand (FIX-VERIFY-D: --run, the suite, shards).

static bool suite_guard_seen_running;
static long suite_guard_seen_pid;

static int suite_guard_probe_worker( const char * name )
{
    const suite_case_guard_state & state = suite_case_guard();
    suite_guard_seen_running = state.running && state.name != NULL && std::strcmp( state.name, name ) == 0;
    suite_guard_seen_pid = state.pid;
    return 7;
}

static void test_suite_case_exit_guard_is_armed_only_while_the_case_runs()
{
    const suite_case_guard_state before = suite_case_guard();
    suite_guard_seen_running = false;
    suite_guard_seen_pid = 0;

    check( suite_run_case( suite_guard_probe_worker, "probe" ) == 7 );
    check( suite_guard_seen_running );
    check( suite_guard_seen_pid == suite_process_id() );
    check( suite_case_guard().registered );
    check( !suite_case_guard().running );

    suite_case_guard_state armed = { "probe", 4242, true, true };
    check( suite_case_exit_fails( armed, 4242 ) );
    check( !suite_case_exit_fails( armed, 4243 ) );
    armed.running = false;
    check( !suite_case_exit_fails( armed, 4242 ) );

    // this case itself runs under the guard: put its state back
    suite_case_guard() = before;
}

REGISTER_UNIT_TEST( test_suite_case_exit_guard_is_armed_only_while_the_case_runs );

// --run accepts every name --list prints: an exec row's name is found among
// the rows, and the collected run is kept to that one row, its result with
// it; a registered case's name or an unknown one is not an exec row
static void test_suite_run_keeps_only_the_named_exec_row()
{
    const suite_exec execs[] = {
        { "test_exec_a", "a", "", false, 0 },
        { "test_exec_b", "b", "--flag", true, 30 },
    };
    check( suite_exec_named( execs, 2, "test_exec_a" ) );
    check( suite_exec_named( execs, 2, "test_exec_b" ) );
    check( !suite_exec_named( execs, 2, "test_registered" ) );
    check( !suite_exec_named( execs, 2, "test_exec" ) );
    check( !suite_exec_named( execs, 0, "test_exec_a" ) );

    std::vector<suite_case> pending( 3 );
    std::vector<suite_result> results( 3 );
    const char * names[] = { "test_registered", "test_exec_a", "test_exec_b" };
    for ( size_t i = 0; i < 3; i++ )
    {
        pending[i].name = names[i];
        results[i].name = names[i];
    }
    pending[2].exec_path = "dir/b";

    suite_keep_named( pending, results, "test_exec_b" );
    check( pending.size() == 1 && results.size() == 1 );
    check( pending[0].name == "test_exec_b" && pending[0].exec_path == "dir/b" );
    check( results[0].name == "test_exec_b" );
}

REGISTER_UNIT_TEST( test_suite_run_keeps_only_the_named_exec_row );
