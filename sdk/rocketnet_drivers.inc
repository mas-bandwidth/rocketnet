/* Native transport registry: symbol, name, discovers port, operations,
   platforms, server preference, client preference. Lower ranks win; zero
   means unavailable for that endpoint. Keep existing rows in ID order.
   Add a backend by defining its operations and adding one row here. */
// Portable is available in both modes. Production keeps native server
// transports ahead of this fallback; development preserves its existing order.
// The client column ranks the native client first on its OS (gso on Linux,
// msgx on macOS) and portable last everywhere, the fallback when a native
// client's create refuses, and iocp first on Windows.
// tests/test_unit_client_transport_ranking.cpp.
ROCKETNET_TRANSPORT( PORTABLE, "portable", true, rocketnet_portable_transport, ROCKETNET_TRANSPORT_ALL_PLATFORMS, ( ROCKETNET_DEV ? 30 : 50 ), 30 )
ROCKETNET_TRANSPORT( GSO, "gso", true, rocketnet_gso_transport, ROCKETNET_TRANSPORT_LINUX, 20, 10 )
ROCKETNET_TRANSPORT( XDP, "xdp", false, rocketnet_xdp_transport, ROCKETNET_TRANSPORT_LINUX, 10, 0 )
ROCKETNET_TRANSPORT( MSGX, "msgx", true, rocketnet_msgx_transport, ROCKETNET_TRANSPORT_MAC, 10, 10 )
ROCKETNET_TRANSPORT( IOCP, "iocp", true, rocketnet_iocp_transport, ROCKETNET_TRANSPORT_WINDOWS, 40, 10 )
ROCKETNET_TRANSPORT( LOOPBACK, "loopback", true, rocketnet_loopback_transport, ROCKETNET_TRANSPORT_ALL_PLATFORMS, ROCKETNET_TRANSPORT_PIN_ONLY, ROCKETNET_TRANSPORT_PIN_ONLY )
ROCKETNET_TRANSPORT( IO_URING, "io_uring", true, rocketnet_io_uring_transport, ROCKETNET_TRANSPORT_IO_URING_PLATFORMS, 25, 0 )
ROCKETNET_TRANSPORT( RIO, "rio", true, rocketnet_rio_transport, ROCKETNET_TRANSPORT_WINDOWS, 45, 0 )

#if defined( ROCKETNET_TRANSPORT_REGISTRY_HEADER )
#include ROCKETNET_TRANSPORT_REGISTRY_HEADER
#endif
