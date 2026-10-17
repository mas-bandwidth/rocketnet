#include <rocketnet.hpp>

// The C++ transport interfaces are header-only and construct nothing that
// needs a socket: including the installed header here proves it compiles
// standalone under the package's C++ toolchain.
static_assert( sizeof( rocketnet::ClientTransport ) > 0, "ClientTransport must exist" );
static_assert( sizeof( rocketnet::ServerTransport ) > 0, "ServerTransport must exist" );

#include "main.c"
