/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Internal platform layer, moved from flow's flow_platform.cpp.

    Stage 1 moves only what rocketnet_time and the address code need: the
    monotonic time base per platform, sleep, getenv, byte-order helpers and
    inet_pton/ntop wrappers. flow's connection-type detection, sockets,
    threads and mutexes are NOT moved yet; they arrive with the IO backends.

    Per-platform sections mirror the reference file's structure so future
    moves stay mechanical.
*/

#include "rocketnet_platform.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// linux

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static double time_start;

bool rocketnet_platform_init()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return true;
}

void rocketnet_platform_term()
{
    // ...
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    // flow's linux version returned == NULL here, inverting the result on this
    // one platform. mac and windows both had != NULL. Fixed on arrival.
    return inet_ntop( AF_INET6, (void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

// ---------------------------------------------------

double rocketnet_platform_time()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
}

void rocketnet_platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// macos and ios

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach_time.h>

static mach_timebase_info_data_t timebase_info;

static uint64_t time_start;

bool rocketnet_platform_init()
{
    mach_timebase_info( &timebase_info );

    time_start = mach_absolute_time();

    return true;
}

void rocketnet_platform_term()
{
    // ...
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

// ---------------------------------------------------

double rocketnet_platform_time()
{
    uint64_t current = mach_absolute_time();
    return ( (double) ( current - time_start ) ) * ( (double) timebase_info.numer ) / ( (double) timebase_info.denom ) / 1000000000.0;
}

void rocketnet_platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// windows
//
// NOTE: moved from flow's windows section but NOT yet exercised by CI, which
// covers ubuntu and macos in stage 1. Treat as unverified until the windows
// leg exists.

#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment( lib, "WS2_32.lib" )

static LARGE_INTEGER timer_frequency;
static LARGE_INTEGER timer_start;

bool rocketnet_platform_init()
{
    QueryPerformanceFrequency( &timer_frequency );
    QueryPerformanceCounter( &timer_start );

    WSADATA WsaData;
    if ( WSAStartup( MAKEWORD(2,2), &WsaData ) != NO_ERROR )
    {
        return false;
    }

    return true;
}

void rocketnet_platform_term()
{
    WSACleanup();
}

const char * rocketnet_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

uint16_t rocketnet_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t rocketnet_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool rocketnet_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool rocketnet_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool rocketnet_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, address_string_size ) != NULL;
}

// ---------------------------------------------------

double rocketnet_platform_time()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter( &now );
    return ( (double) ( now.QuadPart - timer_start.QuadPart ) ) / ( (double) ( timer_frequency.QuadPart ) );
}

void rocketnet_platform_sleep( double time )
{
    const int milliseconds = (int) ( time * 1000 );
    Sleep( milliseconds );
}

#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#error unsupported platform

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
