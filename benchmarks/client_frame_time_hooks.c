/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The meter's view inside the library, without changing it
// (benchmarks/client_frame_time.md, "What is counted and how").
//
// POSIX: this executable defines pthread_mutex_trylock and pthread_mutex_lock
// (and the wake's own call: write on Linux, kevent on macOS). The static
// library's references bind to these definitions at link time; each counts
// when the calling thread is inside a call window and forwards to the C
// library's function, found with dlsym( RTLD_NEXT ).
//
// Windows: the executable's import address table entries for the same
// operations are pointed at counting functions at startup.
//
// Only the game thread ever sets a call window, so the counts are plain
// integers written by that thread alone.

#if defined( __linux__ )
#define _GNU_SOURCE
#endif // #if defined( __linux__ )

#include "client_frame_time.h"

#include <string.h>

#if defined( _WIN32 )
#include <winsock2.h>
#include <windows.h>
#define CFT_THREAD_LOCAL __declspec( thread )
#else // #if defined( _WIN32 )
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define CFT_THREAD_LOCAL _Thread_local
#endif // #if defined( _WIN32 )

#if defined( __APPLE__ )
#include <mach/mach_time.h>
#include <sys/event.h>
#endif // #if defined( __APPLE__ )

struct cft_counts_t cft_counts;

static CFT_THREAD_LOCAL int cft_call;
static CFT_THREAD_LOCAL int cft_planted;

static int cft_plant_call;
static int cft_plant_kind;
static uint64_t cft_plant_ns;
static void * ( *cft_plant_malloc )( void * context, size_t bytes );
static void ( *cft_plant_free )( void * context, void * p );

uint64_t cft_now_ns( void )
{
#if defined( _WIN32 )
    static LARGE_INTEGER frequency;
    if ( frequency.QuadPart == 0 )
    {
        QueryPerformanceFrequency( &frequency );
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter( &counter );
    const uint64_t ticks = (uint64_t) counter.QuadPart;
    const uint64_t hz = (uint64_t) frequency.QuadPart;
    return ticks / hz * 1000000000ull + ticks % hz * 1000000000ull / hz;
#elif defined( __APPLE__ ) // #if defined( _WIN32 )
    static mach_timebase_info_data_t timebase;
    if ( timebase.denom == 0 )
    {
        mach_timebase_info( &timebase );
    }
    return mach_absolute_time() * timebase.numer / timebase.denom;
#else // #if defined( _WIN32 )
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (uint64_t) now.tv_sec * 1000000000ull + (uint64_t) now.tv_nsec;
#endif // #if defined( _WIN32 )
}

void cft_set_call( int call )
{
    cft_call = call;
    cft_planted = 0;
}

int cft_get_call( void )
{
    return cft_call;
}

void cft_set_plant( int call, int kind, double microseconds, void * ( *malloc_function )( void * context, size_t bytes ), void ( *free_function )( void * context, void * p ) )
{
    cft_plant_call = call;
    cft_plant_kind = kind;
    cft_plant_ns = (uint64_t) ( microseconds * 1000.0 );
    cft_plant_malloc = malloc_function;
    cft_plant_free = free_function;
}

// the plant runs inside the library: from the first try-acquire of the call it targets

static void cft_plant( void )
{
    if ( cft_plant_kind == CFT_PLANT_NONE || cft_call != cft_plant_call || cft_planted )
    {
        return;
    }

    cft_planted = 1;

    if ( cft_plant_kind == CFT_PLANT_BUSY )
    {
        const uint64_t end = cft_now_ns() + cft_plant_ns;
        while ( cft_now_ns() < end )
        {
        }
    }
    else if ( cft_plant_kind == CFT_PLANT_ALLOC && cft_plant_malloc && cft_plant_free )
    {
        void * p = cft_plant_malloc( NULL, 64 );
        cft_plant_free( NULL, p );
    }
    else if ( cft_plant_kind == CFT_PLANT_SYSCALL )
    {
#if !defined( _WIN32 )
        (void) getppid();
#endif // #if !defined( _WIN32 )
    }
}

#if defined( _WIN32 )

typedef BOOLEAN( WINAPI * cft_try_srw_fn )( PSRWLOCK lock );
typedef void( WINAPI * cft_acquire_srw_fn )( PSRWLOCK lock );
typedef BOOL( WINAPI * cft_post_fn )( HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped );
typedef int( WSAAPI * cft_wsasendto_fn )( SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const struct sockaddr * to, int to_bytes,
                                          LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine );

static cft_try_srw_fn cft_real_try_srw;
static cft_acquire_srw_fn cft_real_acquire_srw;
static cft_post_fn cft_real_post;
static cft_wsasendto_fn cft_real_wsasendto;
static int cft_patched;

static BOOLEAN WINAPI cft_try_srw( PSRWLOCK lock )
{
    if ( cft_call == CFT_CALL_NONE )
    {
        return cft_real_try_srw( lock );
    }

    cft_plant();
    const BOOLEAN taken = cft_real_try_srw( lock );
    cft_counts.trylock++;
    cft_counts.trylock_busy += taken ? 0u : 1u;
    return taken;
}

static void WINAPI cft_acquire_srw( PSRWLOCK lock )
{
    if ( cft_call == CFT_CALL_NONE )
    {
        cft_real_acquire_srw( lock );
        return;
    }

    const uint64_t start = cft_now_ns();
    cft_real_acquire_srw( lock );
    cft_counts.lock++;
    cft_counts.lock_ns += cft_now_ns() - start;
}

static BOOL WINAPI cft_post( HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped )
{
    if ( cft_call != CFT_CALL_NONE )
    {
        cft_counts.wake++;
    }
    return cft_real_post( port, bytes, key, overlapped );
}

static int WSAAPI cft_wsasendto( SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const struct sockaddr * to, int to_bytes,
                                 LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine )
{
    if ( cft_call != CFT_CALL_NONE )
    {
        cft_counts.os_other++;
    }
    return cft_real_wsasendto( s, buffers, count, sent, flags, to, to_bytes, overlapped, routine );
}

// points every import address table entry of this executable named name at
// hook; the entry's old value goes to *real (pointers move by memcpy, so no
// function and data pointer cast is written)

void cft_patch( const char * name, const void * hook, void * real )
{
    BYTE * base = (BYTE *) GetModuleHandleW( NULL );
    const IMAGE_DOS_HEADER * dos = (const IMAGE_DOS_HEADER *) base;
    const IMAGE_NT_HEADERS * nt = (const IMAGE_NT_HEADERS *) ( base + dos->e_lfanew );
    const IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if ( directory.VirtualAddress == 0 )
    {
        return;
    }

    for ( const IMAGE_IMPORT_DESCRIPTOR * import = (const IMAGE_IMPORT_DESCRIPTOR *) ( base + directory.VirtualAddress ); import->Name != 0; import++ )
    {
        if ( import->OriginalFirstThunk == 0 )
        {
            continue;
        }

        const IMAGE_THUNK_DATA * names = (const IMAGE_THUNK_DATA *) ( base + import->OriginalFirstThunk );
        IMAGE_THUNK_DATA * entries = (IMAGE_THUNK_DATA *) ( base + import->FirstThunk );
        for ( ; names->u1.AddressOfData != 0; names++, entries++ )
        {
            if ( IMAGE_SNAP_BY_ORDINAL( names->u1.Ordinal ) )
            {
                continue;
            }

            const IMAGE_IMPORT_BY_NAME * by_name = (const IMAGE_IMPORT_BY_NAME *) ( base + names->u1.AddressOfData );
            if ( strcmp( (const char *) by_name->Name, name ) != 0 )
            {
                continue;
            }

            DWORD protection = 0;
            if ( !VirtualProtect( &entries->u1.Function, sizeof( entries->u1.Function ), PAGE_READWRITE, &protection ) )
            {
                continue;
            }

            memcpy( real, &entries->u1.Function, sizeof( entries->u1.Function ) );
            memcpy( &entries->u1.Function, &hook, sizeof( entries->u1.Function ) );
            VirtualProtect( &entries->u1.Function, sizeof( entries->u1.Function ), protection, &protection );
            cft_patched++;
        }
    }
}

void cft_hooks_init( void )
{
    cft_try_srw_fn try_srw = cft_try_srw;
    cft_acquire_srw_fn acquire_srw = cft_acquire_srw;
    cft_post_fn post = cft_post;
    cft_wsasendto_fn wsasendto = cft_wsasendto;
    const void * hook;

    memcpy( &hook, &try_srw, sizeof( hook ) );
    cft_patch( "TryAcquireSRWLockExclusive", hook, &cft_real_try_srw );
    memcpy( &hook, &acquire_srw, sizeof( hook ) );
    cft_patch( "AcquireSRWLockExclusive", hook, &cft_real_acquire_srw );
    memcpy( &hook, &post, sizeof( hook ) );
    cft_patch( "PostQueuedCompletionStatus", hook, &cft_real_post );
    memcpy( &hook, &wsasendto, sizeof( hook ) );
    cft_patch( "WSASendTo", hook, &cft_real_wsasendto );
}

const char * cft_hooks_describe( void )
{
    return cft_patched >= 3 ? "iat" : "iat-incomplete";
}

#else // #if defined( _WIN32 )

// a function found by dlsym, moved by memcpy (no object to function pointer cast)

static void cft_resolve( const char * name, void * out, size_t bytes )
{
    void * symbol = dlsym( RTLD_NEXT, name );
    memcpy( out, &symbol, bytes );
}

typedef int ( *cft_mutex_fn )( pthread_mutex_t * mutex );

static cft_mutex_fn cft_real_trylock;
static cft_mutex_fn cft_real_lock;

int pthread_mutex_trylock( pthread_mutex_t * mutex )
{
    if ( !cft_real_trylock )
    {
        cft_resolve( "pthread_mutex_trylock", &cft_real_trylock, sizeof( cft_real_trylock ) );
    }
    if ( cft_call == CFT_CALL_NONE )
    {
        return cft_real_trylock( mutex );
    }

    cft_plant();
    const int result = cft_real_trylock( mutex );
    cft_counts.trylock++;
    cft_counts.trylock_busy += result == 0 ? 0u : 1u;
    return result;
}

int pthread_mutex_lock( pthread_mutex_t * mutex )
{
    if ( !cft_real_lock )
    {
        cft_resolve( "pthread_mutex_lock", &cft_real_lock, sizeof( cft_real_lock ) );
    }
    if ( cft_call == CFT_CALL_NONE )
    {
        return cft_real_lock( mutex );
    }

    const uint64_t start = cft_now_ns();
    const int result = cft_real_lock( mutex );
    cft_counts.lock++;
    cft_counts.lock_ns += cft_now_ns() - start;
    return result;
}

#if defined( __APPLE__ )

typedef int ( *cft_kevent_fn )( int kq, const struct kevent * changes, int num_changes, struct kevent * events, int num_events, const struct timespec * timeout );

static cft_kevent_fn cft_real_kevent;

int kevent( int kq, const struct kevent * changes, int num_changes, struct kevent * events, int num_events, const struct timespec * timeout )
{
    if ( !cft_real_kevent )
    {
        cft_resolve( "kevent", &cft_real_kevent, sizeof( cft_real_kevent ) );
    }
    if ( cft_call != CFT_CALL_NONE )
    {
        if ( num_changes == 1 && changes && changes[0].filter == EVFILT_USER && num_events == 0 )
        {
            cft_counts.wake++;
        }
        else
        {
            cft_counts.os_other++;
        }
    }
    return cft_real_kevent( kq, changes, num_changes, events, num_events, timeout );
}

#else // #if defined( __APPLE__ )

typedef ssize_t ( *cft_write_fn )( int fd, const void * data, size_t bytes );

static cft_write_fn cft_real_write;

ssize_t write( int fd, const void * data, size_t bytes )
{
    if ( !cft_real_write )
    {
        cft_resolve( "write", &cft_real_write, sizeof( cft_real_write ) );
    }
    if ( cft_call != CFT_CALL_NONE )
    {
        if ( bytes == 8 )
        {
            cft_counts.wake++;
        }
        else
        {
            cft_counts.os_other++;
        }
    }
    return cft_real_write( fd, data, bytes );
}

#endif // #if defined( __APPLE__ )

void cft_hooks_init( void )
{
    cft_resolve( "pthread_mutex_trylock", &cft_real_trylock, sizeof( cft_real_trylock ) );
    cft_resolve( "pthread_mutex_lock", &cft_real_lock, sizeof( cft_real_lock ) );
}

const char * cft_hooks_describe( void )
{
    return "interpose";
}

#endif // #if defined( _WIN32 )
