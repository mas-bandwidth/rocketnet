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
*/

#ifndef HARDEN_THREAD_POOL_H
#define HARDEN_THREAD_POOL_H

#include "tests/load/enkiTS/TaskScheduler.h"

#include <atomic>
#include <cassert>
#include <cstdint>

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
    std::atomic<int> realtime{ 0 }; // threads the process could raise to SCHED_FIFO
};

static harden_pin_t harden_pin;

static void harden_pin_this_thread( int cpu )
{
#if defined( __linux__ )
    cpu_set_t set;
    CPU_ZERO( &set );
    CPU_SET( cpu, &set );
    if ( pthread_setaffinity_np( pthread_self(), sizeof( set ), &set ) == 0 )
    {
        harden_pin.pinned.fetch_add( 1 );
    }
    struct sched_param param;
    param.sched_priority = sched_get_priority_max( SCHED_FIFO );
    if ( pthread_setschedparam( pthread_self(), SCHED_FIFO, &param ) == 0 )
    {
        harden_pin.realtime.fetch_add( 1 );
    }
#else
    (void) cpu;
#endif // #if defined( __linux__ )
}

static void harden_pin_thread_start( uint32_t threadnum )
{
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
        if ( first_cpu >= 0 )
        {
            harden_pin_this_thread( first_cpu );
        }
        enki::TaskSchedulerConfig config;
        config.numTaskThreadsToCreate = (uint32_t) ( n - 1 );
        config.profilerCallbacks.threadStart = harden_pin_thread_start;
        scheduler.Initialize( config );
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
