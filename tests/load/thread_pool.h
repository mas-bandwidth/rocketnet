/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    thread_pool.h: space game's ThreadPool API over enkiTS, for harden_load's
    space frame shape only (tests only, never linked into the SDK; enkiTS is
    vendored beside it in tests/load/enkiTS). The API is the one space's server
    calls (Apps/Server/Server.cpp, Source/PacketWriter.h at the framework
    import e3c684e):

        ThreadPool pool;
        pool.Initialize( n );                    // n threads in all, the caller is thread 0
        Task * task = pool.AddTask( fn, data, count );
        task->Wait();
        pool.Reset();                            // once a frame, after the join

    A task function is called with a range [start, finish) of the count items,
    never promised one item per call (space's core_tests.cpp,
    test_thread_pool_coarse_range): every function here loops the range.

    Pinning is space's layout: the OS on the first cores, the network threads
    on the next, then the main thread (thread 0) and the game threads in
    order, each at SCHED_FIFO max where the process may. Space pins its game
    threads with a task that sleeps so each thread takes one (Server.cpp
    PinThreadToCPUTask, whose comment names the race); here each enkiTS thread
    pins itself as it starts (the threadStart callback), so every game thread
    pins exactly once.

    Windows has the same layout: SetThreadGroupAffinity puts a thread on the
    cpu'th logical processor counted across the processor groups in order
    (group 0 holds the first 64), and SetThreadPriority raises it to
    THREAD_PRIORITY_TIME_CRITICAL, the highest a process that is not in the
    realtime class gets, the counterpart of SCHED_FIFO max. A setting the
    system refuses is counted (pin_refused, realtime_refused) with the first
    error code, and harden_load prints that in the row.
*/

#ifndef HARDEN_THREAD_POOL_H
#define HARDEN_THREAD_POOL_H

// a system include (CMakeLists.txt): enkiTS is vendored unmodified and is not held to rocketnet's warning floor
#if defined( _WIN32 )
#include <winsock2.h>
#include <windows.h>
#endif // #if defined( _WIN32 )

#include <TaskScheduler.h>

#include <atomic>
#include <cstring>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined( __linux__ )
#include <pthread.h>
#include <sched.h>
#endif // #if defined( __linux__ )

#define HARDEN_MAX_TASKS 64

struct platform_task_args_t
{
    void * data;
    int start;
    int finish;
    int thread_index;
};

typedef void ( *platform_task_function_t )( platform_task_args_t * args );

class ThreadPool;

class Task : public enki::ITaskSet
{
public:
    void ExecuteRange( enki::TaskSetPartition range, uint32_t threadnum ) override
    {
        platform_task_args_t args;
        args.data = data;
        args.start = (int) range.start;
        args.finish = (int) range.end;
        args.thread_index = (int) threadnum;
        function( &args );
    }

    void Wait();

    platform_task_function_t function = nullptr;
    void * data = nullptr;
    ThreadPool * pool = nullptr;
};

// the thread pinning: first_cpu is the main thread's core, game thread i
// (enkiTS thread i, i >= 1) goes on first_cpu + i. -1 pins nothing
struct harden_pin_t
{
    std::atomic<int> first_cpu{ -1 };
    std::atomic<int> pinned{ 0 };   // threads pinned to their core
    std::atomic<int> realtime{ 0 }; // threads the process could raise to SCHED_FIFO (Windows: THREAD_PRIORITY_TIME_CRITICAL)
    std::atomic<int> attempted{ 0 }; // threads that tried to pin, whether or not it was allowed
    std::atomic<int> pin_refused{ 0 };      // pinning the system refused
    std::atomic<int> realtime_refused{ 0 }; // raising the priority the system refused
    std::atomic<int> pin_error{ 0 };        // first refusal's error code (errno, Windows GetLastError), 0 if none
    std::atomic<int> realtime_error{ 0 };
};

static harden_pin_t harden_pin;

// the pool threads' ids (the profile classes them as game threads): each pool thread records its own as it starts
static std::atomic<unsigned long> harden_pool_ids[256];
static std::atomic<int> harden_pool_id_count{ 0 };

static void harden_pool_record_thread()
{
    unsigned long id = 0;
#if defined( _WIN32 )
    id = (unsigned long) GetCurrentThreadId();
#endif // #if defined( _WIN32 )
    const int slot = harden_pool_id_count.fetch_add( 1 );
    if ( slot < 256 )
    {
        harden_pool_ids[slot].store( id );
    }
}

static void harden_pin_this_thread( int cpu )
{
#if defined( __linux__ )
    cpu_set_t set;
    CPU_ZERO( &set );
    const size_t core = (size_t) cpu;
    CPU_SET( core, &set );
    const int pin_result = pthread_setaffinity_np( pthread_self(), sizeof( set ), &set );
    if ( pin_result == 0 )
    {
        harden_pin.pinned.fetch_add( 1 );
    }
    else
    {
        int none = 0;
        harden_pin.pin_error.compare_exchange_strong( none, pin_result );
        harden_pin.pin_refused.fetch_add( 1 );
    }
    struct sched_param param;
    param.sched_priority = sched_get_priority_max( SCHED_FIFO );
    const int realtime_result = pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );
    if ( realtime_result == 0 )
    {
        harden_pin.realtime.fetch_add( 1 );
    }
    else
    {
        int none = 0;
        harden_pin.realtime_error.compare_exchange_strong( none, realtime_result );
        harden_pin.realtime_refused.fetch_add( 1 );
    }
#elif defined( _WIN32 )
    // the cpu'th logical processor across the groups, in order
    WORD group = 0;
    KAFFINITY mask = 0;
    {
        const WORD groups = GetActiveProcessorGroupCount();
        int remaining = cpu;
        for ( WORD g = 0; g < groups; g++ )
        {
            const int count = (int) GetActiveProcessorCount( g );
            if ( remaining < count )
            {
                group = g;
                mask = ( (KAFFINITY) 1 ) << remaining;
                break;
            }
            remaining -= count;
        }
    }
    bool pinned = false;
    DWORD pin_error = ERROR_INVALID_PARAMETER; // cpu past the last processor
    if ( mask != 0 )
    {
        GROUP_AFFINITY affinity;
        memset( &affinity, 0, sizeof( affinity ) );
        affinity.Group = group;
        affinity.Mask = mask;
        pinned = SetThreadGroupAffinity( GetCurrentThread(), &affinity, NULL ) != 0;
        pin_error = pinned ? 0 : GetLastError();
    }
    if ( pinned )
    {
        harden_pin.pinned.fetch_add( 1 );
    }
    else
    {
        int none = 0;
        harden_pin.pin_error.compare_exchange_strong( none, (int) pin_error );
        harden_pin.pin_refused.fetch_add( 1 );
    }
    if ( SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL ) != 0 )
    {
        harden_pin.realtime.fetch_add( 1 );
    }
    else
    {
        int none = 0;
        harden_pin.realtime_error.compare_exchange_strong( none, (int) GetLastError() );
        harden_pin.realtime_refused.fetch_add( 1 );
    }
#else
    (void) cpu;
#endif // #if defined( __linux__ )
    harden_pin.attempted.fetch_add( 1 );
}

static void harden_pin_thread_start( uint32_t threadnum )
{
    harden_pool_record_thread();
    const int first = harden_pin.first_cpu.load();
    if ( first >= 0 )
    {
        harden_pin_this_thread( first + (int) threadnum );
    }
}

class ThreadPool
{
public:
    // n threads in all, the caller (the main thread) is thread 0; with
    // first_cpu >= 0 the caller is pinned to first_cpu and game thread i to
    // first_cpu + i
    void Initialize( int n, int first_cpu = -1 )
    {
        assert( n >= 1 );
        harden_pin.first_cpu.store( first_cpu );
        enki::TaskSchedulerConfig config;
        config.numTaskThreadsToCreate = (uint32_t) ( n - 1 );
        config.profilerCallbacks.threadStart = harden_pin_thread_start;
        // the game threads are made before the main thread pins itself: a thread
        // inherits its maker's core and SCHED_FIFO, and a spinning main thread at
        // SCHED_FIFO max on that one core never lets them run to pin themselves
        scheduler.Initialize( config );
        if ( first_cpu >= 0 )
        {
            harden_pin_this_thread( first_cpu );
            // every game thread has tried to pin before the first frame
            for ( int i = 0; i < 2000 && harden_pin.attempted.load() < n; i++ )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }
        num_threads = n;
        for ( int i = 0; i < HARDEN_MAX_TASKS; i++ )
        {
            tasks[i].pool = this;
        }
    }

    void Shutdown()
    {
        scheduler.WaitforAllAndShutdown();
    }

    // callable from the main thread or from inside a task (space's network
    // task adds the player packet jobs); count items in [0, count)
    Task * AddTask( platform_task_function_t function, void * data, int count = 1 )
    {
        const int index = next.fetch_add( 1 );
        assert( index < HARDEN_MAX_TASKS );
        if ( index >= HARDEN_MAX_TASKS )
        {
            return nullptr;
        }
        Task * task = &tasks[index];
        assert( task->GetIsComplete() );
        task->function = function;
        task->data = data;
        task->m_SetSize = (uint32_t) ( count > 0 ? count : 1 );
        scheduler.AddTaskSetToPipe( task );
        return task;
    }

    // once a frame, after the join: every task added this frame is complete
    void Reset()
    {
        const int used = next.load();
        for ( int i = 0; i < used && i < HARDEN_MAX_TASKS; i++ )
        {
            assert( tasks[i].GetIsComplete() );
            scheduler.WaitforTask( &tasks[i] );
        }
        next.store( 0 );
    }

    int NumThreads() const
    {
        return num_threads;
    }

    enki::TaskScheduler scheduler;

private:
    Task tasks[HARDEN_MAX_TASKS];
    std::atomic<int> next{ 0 };
    int num_threads = 0;
};

inline void Task::Wait()
{
    pool->scheduler.WaitforTask( this );
}

#endif // #ifndef HARDEN_THREAD_POOL_H
