# AF_XDP zero-copy qualification

Verdict: HOLD. The XDP bench NIC refuses the zero-copy bind; no machine available
to this card negotiates zero copy. The load_split ladder was not made.

## Run

- Date (`date -u`): Sat Oct 24 02:33:34 AM UTC 2026
- Head: 010c8167de062d24dafcdf9153577b2b8e078760 (`git log --oneline -1`:
  `010c8167`, the land commit)
- Build: `make -j4` (Release, ROCKETNET_DEV=ON default, ROCKETNET_XDP=ON);
  last build line `[100%] Built target test`
- Machine: `hostname` <XDP bench>, `uname -r` 6.8.0-142-generic, NIC Intel X540-AT2
  (0000:09:00.0 and 0000:09:00.1)

## The NIC

`ethtool -i enp9s0f0` (enp9s0f1 is identical):

```text
driver: ixgbe
version: 6.2.5
firmware-version: 0x80000369
bus-info: 0000:09:00.0
```

The module is the out-of-tree Intel release, not the in-tree build:
`modinfo -F filename ixgbe` is
`/lib/modules/6.8.0-142-generic/updates/drivers/net/ethernet/intel/ixgbe/ixgbe.ko`,
built from `~/ethernet-linux-ixgbe` at `9824faf Intel official release 6.2.5`.
Its source compiles `HAVE_AF_XDP_ZC_SUPPORT` but never sets the netdev
xdp_features flags.

xdp_features, read from the kernel's netdev generic-netlink family
(`NETDEV_CMD_DEV_GET`, attribute `NETDEV_A_DEV_XDP_FEATURES`):

```text
enp9s0f0 ifindex=4 xdp_features=0x0 [none] xdp_zc_max_segs=None
enp9s0f1 ifindex=5 xdp_features=0x0 [none] xdp_zc_max_segs=None
```

No `xsk-zerocopy` bit (nor basic, redirect or ndo-xmit) is advertised.

## The port

The XDP bench has no dedicated cabled test port today: enp9s0f1 carries the default
route (192.168.1.42/24, the uplink) and enp9s0f0 has no carrier. The bind
probe below ran on enp9s0f0 (not the uplink; a zero-copy bind is negotiated
with the driver and needs no link), with a test address and a fixed gateway
MAC because the port has no route. It is mode-negotiation evidence only; no
traffic and no performance number come from it.

Setup, then restored after the probe:

```text
sudo ip addr add 10.99.0.1/24 dev enp9s0f0
sudo scripts/rocketnet-provision enp9s0f0 2      # run twice: R7 set 2 queues, the second run pinned irqs 179/180/181 to cpus 1/2/3
dead-man: sudo setsid nohup bash -c "sleep 90; ip link set dev enp9s0f0 xdp off; ip addr del 10.99.0.1/24 dev enp9s0f0" &
```

Before: combined 32, napi_defer_hard_irqs 0, gro_flush_timeout 0. After the
probe: `ethtool -L enp9s0f0 combined 32`, both sysfs values back to 0, no XDP
program attached (`ip -d link show enp9s0f0` shows no prog/xdp), the test
address removed.

## The bind

```text
sudo env ROCKETNET_SERVER_TRANSPORT=xdp ROCKETNET_SERVER_NUM_XDP_QUEUES=2 ROCKETNET_SERVER_NUM_OS_CPUS=1 ROCKETNET_GATEWAY_ETHERNET_ADDRESS=02:00:00:00:00:01 timeout -s INT 15 ./bin/load_split server 10 10 0123456789abcdef 10.99.0.1:40000 10.99.0.1 80000
```

Create log (sdk/rocketnet_server_socket_linux.cpp:1760 and :1898-1903):

```text
0.010768: info: server network interface is enp9s0f0
0.011004: info: attaching server_xdp to network interface enp9s0f0
0.800140: info: server xdp queue 0 refused the zero copy bind (Operation not supported), binding in copy mode
0.953381: info: server xdp queue 1 refused the zero copy bind (Operation not supported), binding in copy mode
0.976111: info: server xdp sockets bound in copy mode on enp9s0f0
0.976120: info: server started on 10.99.0.1:40000 [xdp]
```

## Cases

| Case | Result |
|---|---|
| XDP bench enp9s0f0, ixgbe 6.2.5 out-of-tree, queue 0 zero-copy bind | refused, EOPNOTSUPP ("Operation not supported"); bound copy |
| XDP bench enp9s0f0, ixgbe 6.2.5 out-of-tree, queue 1 zero-copy bind | refused, EOPNOTSUPP ("Operation not supported"); bound copy |
| XDP bench negotiated mode | copy mode on all queues |
| space (out-of-tree i40e) | not tried: held for the game server |
| load_split plan-server / plan-client-shard | not made |
| load_split ladder 10/100/250/500/1000, zero copy | not made: no zero-copy server |

## Decision owed (coordinator)

Which machine qualifies zero copy:

1. space's out-of-tree i40e, the NIC zero copy is known to bind on, today
   held for the game server; or
2. a driver on the XDP bench that negotiates zero copy (the X540's ixgbe with
   xdp_features carrying xsk-zerocopy, for example the in-tree ixgbe of a
   newer kernel or a fixed out-of-tree release), plus a dedicated cabled port
   on the XDP bench, since enp9s0f0 has no carrier and enp9s0f1 is the uplink.

The roadmap rows stay open: docs/roadmap.sexp line 382 (zero-copy half) and
row 157's note ("zero-copy remains unqualified") are unchanged by this run.
