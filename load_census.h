/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

/*
    The resource census of the load harnesses: which threads and handles (fds)
    the process holds at a lifecycle phase, by identity, not by count.

    A count comparison cannot tell a leak from the operating system's own
    churn. On Windows the first rocketnet_init, socket and adapter query
    leave about 34 handles the OS keeps for the life of the process (ETW
    registrations, registry keys, the thread pool's worker factory and its
    completion port, an ALPC port), the loader's three startup worker threads
    exit about half a minute after start, and the process thread pool starts
    and retires its own workers (start routine in ntdll) under load. None of
    that is RocketNet's, and a count moves with all of it (captain,
    2026-10-22: 63 -> 97 handles and 4 -> 3 threads on every transport).

    So the harness takes a census at each phase and compares identities: a
    survivor is a thread or handle present at the later phase and absent at
    the earlier one. The harness first pays one warm cycle (init, a server,
    term) before its baseline, so first-use OS retention is outside the
    measured cycle. On Windows the process thread pool's workers, and the one
    Thread and one Event handle each worker brings, are the operating
    system's and are reported, never counted as survivors. The random
    device the SDK's bundled sodium reopens on the first random draw after a
    re-init (and closes at term) is init-scope state: the pair that ends
    before term reports it, the pair through term counts it. Every other
    survivor is a leak.
*/

#pragma once

#ifndef ROCKETNET_LOAD_CENSUS_H
#define ROCKETNET_LOAD_CENSUS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined( __APPLE__ )
#include <fcntl.h>
#include <mach/mach.h>
#include <libproc.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define LOAD_CENSUS_MAX 16384

/* One thread or handle. id is the thread id, or the handle value (fd
   number); kind is the object type index on Windows, the file identity
   elsewhere, so a closed and reused fd number is a different object. */
struct load_census_entry_t
{
    uint64_t id;
    uint64_t kind;
    int os_pool; /* windows: a process thread pool worker, the OS's */
    int rng;     /* posix: the system random device, which the SDK's bundled
                    sodium opens on the first random draw after an init and
                    closes at term (rocketnet_term): init-scope state */
};

struct load_census_t
{
    int valid;
    int threads;
    int handles;
    struct load_census_entry_t thread[LOAD_CENSUS_MAX];
    struct load_census_entry_t handle[LOAD_CENSUS_MAX];
};

/* The survivors between two censuses: what the later one holds that the
   earlier one did not, less what the operating system owns. */
struct load_census_survivors_t
{
    long threads;
    long handles;
    long os_pool_threads;  /* reported, not counted */
    long os_pool_handles;  /* reported, not counted */
    long init_scope_handles; /* reported, not counted, when init_scope allows */
};

#if defined( _WIN32 )

typedef struct
{
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} load_census_handle_entry_t;

typedef struct
{
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    load_census_handle_entry_t Handles[1];
} load_census_handle_info_t;

typedef LONG( WINAPI * load_census_query_system_t )( ULONG, PVOID, ULONG, PULONG );
typedef LONG( WINAPI * load_census_query_thread_t )( HANDLE, ULONG, PVOID, ULONG, PULONG );
typedef LONG( WINAPI * load_census_query_object_t )( HANDLE, ULONG, PVOID, ULONG, PULONG );

#define LOAD_CENSUS_SYSTEM_EXTENDED_HANDLE_INFORMATION 64
#define LOAD_CENSUS_THREAD_QUERY_SET_WIN32_START_ADDRESS 9
#define LOAD_CENSUS_OBJECT_TYPE_INFORMATION 2

static FARPROC load_census_ntdll( const char * name )
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    return ntdll ? GetProcAddress( ntdll, name ) : NULL;
}

/* A thread whose start routine lies in ntdll is a worker the OS started for
   the process thread pool; every thread the process creates itself starts
   in its own code or the C runtime. */
static int load_census_thread_is_os_pool( DWORD tid )
{
    load_census_query_thread_t query = (load_census_query_thread_t) (void ( * )( void )) load_census_ntdll( "NtQueryInformationThread" );
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    if ( !query || !ntdll )
        return 0;
    HANDLE thread = OpenThread( THREAD_QUERY_INFORMATION, FALSE, tid );
    if ( !thread )
        return 0;
    PVOID start = NULL;
    const LONG status = query( thread, LOAD_CENSUS_THREAD_QUERY_SET_WIN32_START_ADDRESS, &start, sizeof( start ), NULL );
    CloseHandle( thread );
    HMODULE owner = NULL;
    if ( status < 0 || !start ||
         !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) start, &owner ) )
        return 0;
    return owner == ntdll;
}

static void load_census_type_name( uint64_t handle, char * name, size_t bytes )
{
    snprintf( name, bytes, "?" );
    load_census_query_object_t query = (load_census_query_object_t) (void ( * )( void )) load_census_ntdll( "NtQueryObject" );
    if ( !query )
        return;
    union
    {
        char raw[1024];
        struct
        {
            USHORT length, maximum;
            PWSTR buffer;
        } type_name;
    } info;
    if ( query( (HANDLE) (uintptr_t) handle, LOAD_CENSUS_OBJECT_TYPE_INFORMATION, &info, sizeof( info ), NULL ) < 0 || !info.type_name.buffer )
        return;
    const int written = WideCharToMultiByte( CP_UTF8, 0, info.type_name.buffer, info.type_name.length / 2, name, (int) bytes - 1, NULL, NULL );
    name[written > 0 ? written : 0] = '\0';
}

static int load_census_take( struct load_census_t * census )
{
    census->valid = 0;
    census->threads = 0;
    census->handles = 0;
    HANDLE thread_list = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( thread_list == INVALID_HANDLE_VALUE )
        return 0;
    THREADENTRY32 entry;
    entry.dwSize = sizeof( entry );
    const DWORD self = GetCurrentProcessId();
    if ( Thread32First( thread_list, &entry ) )
    {
        do
        {
            if ( entry.th32OwnerProcessID != self )
                continue;
            if ( census->threads == LOAD_CENSUS_MAX )
            {
                CloseHandle( thread_list );
                return 0;
            }
            struct load_census_entry_t * thread = &census->thread[census->threads++];
            thread->id = entry.th32ThreadID;
            thread->kind = 0;
            thread->os_pool = load_census_thread_is_os_pool( entry.th32ThreadID );
            thread->rng = 0;
        } while ( Thread32Next( thread_list, &entry ) );
    }
    CloseHandle( thread_list );

    load_census_query_system_t query = (load_census_query_system_t) (void ( * )( void )) load_census_ntdll( "NtQuerySystemInformation" );
    if ( !query )
        return 0;
    ULONG bytes = 1u << 22;
    void * buffer = NULL;
    LONG status = 0;
    for ( int attempt = 0; attempt < 6; ++attempt )
    {
        buffer = malloc( bytes );
        if ( !buffer )
            return 0;
        ULONG needed = 0;
        status = query( LOAD_CENSUS_SYSTEM_EXTENDED_HANDLE_INFORMATION, buffer, bytes, &needed );
        if ( status >= 0 )
            break;
        free( buffer );
        buffer = NULL;
        bytes *= 2;
    }
    if ( !buffer )
        return 0;
    const load_census_handle_info_t * info = (const load_census_handle_info_t *) buffer;
    const ULONG_PTR pid = (ULONG_PTR) self;
    for ( ULONG_PTR i = 0; i < info->NumberOfHandles; ++i )
    {
        if ( info->Handles[i].UniqueProcessId != pid )
            continue;
        if ( census->handles == LOAD_CENSUS_MAX )
        {
            free( buffer );
            return 0;
        }
        struct load_census_entry_t * handle = &census->handle[census->handles++];
        handle->id = (uint64_t) info->Handles[i].HandleValue;
        handle->kind = info->Handles[i].ObjectTypeIndex;
        handle->os_pool = 0;
        handle->rng = 0;
    }
    free( buffer );
    census->valid = 1;
    return 1;
}

#else // #if defined(_WIN32)

#if defined( __APPLE__ ) || defined( __linux__ )
static uint64_t load_census_fd_kind( int fd )
{
    struct stat info;
    if ( fstat( fd, &info ) != 0 )
        return 0;
    return ( (uint64_t) info.st_dev << 32 ) ^ (uint64_t) info.st_ino;
}

static int load_census_fd_is_rng( int fd )
{
    struct stat info, device;
    if ( fstat( fd, &info ) != 0 || !S_ISCHR( info.st_mode ) )
        return 0;
    return ( stat( "/dev/urandom", &device ) == 0 && device.st_rdev == info.st_rdev ) ||
           ( stat( "/dev/random", &device ) == 0 && device.st_rdev == info.st_rdev );
}
#endif

static int load_census_take( struct load_census_t * census )
{
    census->valid = 0;
    census->threads = 0;
    census->handles = 0;
#if defined( __APPLE__ )
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if ( task_threads( mach_task_self(), &threads, &count ) != KERN_SUCCESS )
        return 0;
    int ok = count <= LOAD_CENSUS_MAX;
    for ( mach_msg_type_number_t i = 0; i < count; i++ )
    {
        /* a thread that is exiting (joined, port not yet reclaimed) answers
           no identity: it is gone, not a survivor */
        struct thread_identifier_info identity;
        mach_msg_type_number_t identity_count = THREAD_IDENTIFIER_INFO_COUNT;
        if ( ok && thread_info( threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t) &identity, &identity_count ) == KERN_SUCCESS )
        {
            struct load_census_entry_t * thread = &census->thread[census->threads++];
            thread->id = identity.thread_id;
            thread->kind = 0;
            thread->os_pool = 0;
            thread->rng = 0;
        }
        (void) mach_port_deallocate( mach_task_self(), threads[i] );
    }
    (void) vm_deallocate( mach_task_self(), (vm_address_t) threads, (vm_size_t) ( count * sizeof( thread_act_t ) ) );
    if ( !ok )
        return 0;
    const int list_bytes = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, NULL, 0 );
    if ( list_bytes <= 0 )
        return 0;
    struct proc_fdinfo * fds = (struct proc_fdinfo *) malloc( (size_t) list_bytes );
    if ( !fds )
        return 0;
    const int used = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, fds, list_bytes );
    const int num_fds = used > 0 ? used / (int) PROC_PIDLISTFD_SIZE : 0;
    for ( int i = 0; i < num_fds && census->handles < LOAD_CENSUS_MAX; ++i )
    {
        struct load_census_entry_t * handle = &census->handle[census->handles++];
        handle->id = (uint64_t) fds[i].proc_fd;
        handle->kind = load_census_fd_kind( fds[i].proc_fd );
        handle->os_pool = 0;
        handle->rng = load_census_fd_is_rng( fds[i].proc_fd );
    }
    free( fds );
    if ( used <= 0 || num_fds >= LOAD_CENSUS_MAX )
        return 0;
#elif defined( __linux__ )
    DIR * tasks = opendir( "/proc/self/task" );
    if ( !tasks )
        return 0;
    struct dirent * entry;
    while ( ( entry = readdir( tasks ) ) != NULL )
    {
        if ( entry->d_name[0] < '0' || entry->d_name[0] > '9' )
            continue;
        if ( census->threads == LOAD_CENSUS_MAX )
        {
            closedir( tasks );
            return 0;
        }
        struct load_census_entry_t * thread = &census->thread[census->threads++];
        thread->id = strtoull( entry->d_name, NULL, 10 );
        thread->kind = 0;
        thread->os_pool = 0;
        thread->rng = 0;
    }
    closedir( tasks );
    DIR * fds = opendir( "/proc/self/fd" );
    if ( !fds )
        return 0;
    const int own = dirfd( fds );
    while ( ( entry = readdir( fds ) ) != NULL )
    {
        if ( entry->d_name[0] < '0' || entry->d_name[0] > '9' )
            continue;
        const int fd = (int) strtol( entry->d_name, NULL, 10 );
        if ( fd == own )
            continue;
        if ( census->handles == LOAD_CENSUS_MAX )
        {
            closedir( fds );
            return 0;
        }
        struct load_census_entry_t * handle = &census->handle[census->handles++];
        handle->id = (uint64_t) fd;
        handle->kind = load_census_fd_kind( fd );
        handle->os_pool = 0;
        handle->rng = load_census_fd_is_rng( fd );
    }
    closedir( fds );
#else
    return 0; /* no census on this platform: the count phase check stands alone */
#endif
    census->valid = 1;
    return 1;
}

#endif // #if defined(_WIN32)

static int load_census_holds( const struct load_census_entry_t * entries, int count, const struct load_census_entry_t * wanted )
{
    for ( int i = 0; i < count; ++i )
    {
        if ( entries[i].id == wanted->id && entries[i].kind == wanted->kind )
            return 1;
    }
    return 0;
}

/* The survivors from before to after. label names the pair in the printed
   survivor lines (prefix "<prefix>: census=<label>"), so a failing receipt
   says exactly which object is left behind; a NULL prefix prints nothing.
   init_scope (for a pair that ends before term) reports, without counting,
   the init-scope state the SDK keeps until term: the random device.
   Returns 0 when either census is invalid. */
static int load_census_survivors( const char * prefix, const char * label, int init_scope,
                                  const struct load_census_t * before, const struct load_census_t * after,
                                  struct load_census_survivors_t * survivors )
{
    memset( survivors, 0, sizeof( *survivors ) );
    if ( !before->valid || !after->valid )
        return 0;
    for ( int i = 0; i < after->threads; ++i )
    {
        const struct load_census_entry_t * thread = &after->thread[i];
        if ( load_census_holds( before->thread, before->threads, thread ) )
            continue;
        if ( thread->os_pool )
            survivors->os_pool_threads++;
        else
        {
            survivors->threads++;
            if ( prefix )
                printf( "%s: census=%s survivor=thread id=%llu\n", prefix, label, (unsigned long long) thread->id );
        }
    }
#if defined( _WIN32 )
    /* each OS pool worker brings one Thread handle (to itself) and one Event */
    long pool_events = 0;
#endif
    for ( int i = 0; i < after->handles; ++i )
    {
        const struct load_census_entry_t * handle = &after->handle[i];
        if ( load_census_holds( before->handle, before->handles, handle ) )
            continue;
        if ( init_scope && handle->rng )
        {
            survivors->init_scope_handles++;
            continue;
        }
#if defined( _WIN32 )
        char type[128];
        load_census_type_name( handle->id, type, sizeof( type ) );
        if ( strcmp( type, "Thread" ) == 0 )
        {
            const DWORD tid = GetThreadId( (HANDLE) (uintptr_t) handle->id );
            int pool = 0;
            for ( int j = 0; j < after->threads && !pool; ++j )
                pool = after->thread[j].id == tid && after->thread[j].os_pool &&
                       !load_census_holds( before->thread, before->threads, &after->thread[j] );
            if ( pool )
            {
                survivors->os_pool_handles++;
                continue;
            }
        }
        else if ( strcmp( type, "Event" ) == 0 && pool_events < survivors->os_pool_threads )
        {
            pool_events++;
            survivors->os_pool_handles++;
            continue;
        }
        survivors->handles++;
        if ( prefix )
            printf( "%s: census=%s survivor=handle value=0x%llx type=%s\n", prefix, label, (unsigned long long) handle->id, type );
#else
        survivors->handles++;
        if ( prefix )
        {
            char path[1024] = "?";
#if defined( __APPLE__ )
            if ( fcntl( (int) handle->id, F_GETPATH, path ) != 0 )
                snprintf( path, sizeof( path ), "?" );
#elif defined( __linux__ )
            char link[64];
            snprintf( link, sizeof( link ), "/proc/self/fd/%llu", (unsigned long long) handle->id );
            const ssize_t used = readlink( link, path, sizeof( path ) - 1 );
            path[used > 0 ? used : 0] = '\0';
#endif
            printf( "%s: census=%s survivor=fd value=%llu path=%s\n", prefix, label, (unsigned long long) handle->id, path );
        }
#endif
    }
    return 1;
}

#endif // ROCKETNET_LOAD_CENSUS_H
