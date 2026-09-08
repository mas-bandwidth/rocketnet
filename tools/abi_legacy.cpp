// Simulates a pre-introspection native library. Never shipped.
#include "../sdk/rocketnet.h"
#include <stdlib.h>
ROCKETNET_EXPORT_FUNC void rocketnet_client_transport_default_config(rocketnet_client_transport_config_t *) { abort(); }
