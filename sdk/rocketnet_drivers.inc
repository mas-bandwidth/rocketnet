/* Native driver registry: symbol, name, discovers port, operations,
   platforms, server preference, client preference. Lower ranks win; zero
   means unavailable for that endpoint. Keep existing rows in ID order.
   Add a backend by defining its operations and adding one row here. */
ROCKETNET_DRIVER(PORTABLE, "portable", true,  rocketnet_portable_driver, ROCKETNET_DRIVER_ALL_PLATFORMS, 30, 30)
ROCKETNET_DRIVER(GSO,      "gso",      true,  rocketnet_gso_driver,      ROCKETNET_DRIVER_LINUX,         20,  0)
ROCKETNET_DRIVER(XDP,      "xdp",      false, rocketnet_xdp_driver,      ROCKETNET_DRIVER_LINUX,         10,  0)
ROCKETNET_DRIVER(MSGX,     "msgx",     true,  rocketnet_msgx_driver,     ROCKETNET_DRIVER_MAC,           10, 10)
ROCKETNET_DRIVER(IOCP,     "iocp",     true,  rocketnet_iocp_driver,     ROCKETNET_DRIVER_WINDOWS,       40,  0)
ROCKETNET_DRIVER(LOOPBACK, "loopback", true,  rocketnet_loopback_driver, ROCKETNET_DRIVER_ALL_PLATFORMS, ROCKETNET_DRIVER_PIN_ONLY, ROCKETNET_DRIVER_PIN_ONLY)
ROCKETNET_DRIVER(IO_URING, "io_uring", true,  rocketnet_io_uring_driver, ROCKETNET_DRIVER_IO_URING_PLATFORMS, 25, 0)
ROCKETNET_DRIVER(RIO,      "rio",      true,  rocketnet_rio_driver,      ROCKETNET_DRIVER_WINDOWS,       45,  0)

#if defined(ROCKETNET_DRIVER_REGISTRY_HEADER)
#include ROCKETNET_DRIVER_REGISTRY_HEADER
#endif
