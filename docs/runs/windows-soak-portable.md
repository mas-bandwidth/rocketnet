# Windows portable transport soak

## Pending run record

The 30-minute Windows run has not been made. No result is inferred from the
workflow or from another platform.

The run will build one head in Release and execute this command under the
exclusive Windows lock:

```text
env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable \
    ROCKETNET_SERVER_SIMULATED_LOSS_PERCENT=5 ROCKETNET_CLIENT_SIMULATED_LOSS_PERCENT=5 \
    ROCKETNET_SERVER_SIMULATED_LATENCY_MS=30 ROCKETNET_CLIENT_SIMULATED_LATENCY_MS=30 \
    ROCKETNET_SERVER_SIMULATED_JITTER_MS=20 ROCKETNET_CLIENT_SIMULATED_JITTER_MS=20 \
    ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT=2 ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT=2 \
    ./bin/Release/soak.exe --seconds 1800 --assert-flat
```

The completed record will include the UTC date, exact head, system identity,
granted server and client transports, soak summary, and handle, thread and
private-byte start, end and peak lines. A failure will name the cell and its
retained log.
