# LAN inventory for the transport throughput and 1000-client phases

This is the read-only inventory of the LAN machines that docs/LAN-BENCH.md
chooses its hosts from. It records what each machine's NICs negotiate today;
it chooses no host and designs nothing.

Labels follow the shapes of docs/LIVE-XDP.md section 8 (`linux`,
`windows-interop`, `macos`) with a `lan-` prefix. The label-to-host map lives
in the coordinator's inventory file outside the repository; this document
carries no host name, address or MAC. The dedicated test server is not a LAN
machine (the LAN reaches it over the shared internet uplink), so it has no row
here.

Every command ran read-only under `timeout 20` (on macOS without GNU timeout,
a `perl -e 'alarm 20; exec @ARGV'` equivalent), from script files piped over
ssh, on 2026-10-18. The table lists the wired physical interfaces. Wi-Fi
adapters and unplugged USB Ethernet adapters carry no default route on any
machine and are listed in each machine's notes instead; virtual, tunnel and
Thunderbolt-bridge adapters are left out. "Uplink" is the interface of the
default route. The `ip -s link` error counters are from the second read of the
same command (script `inv-linux-modes.sh`), a few minutes after the first.

## Machines

| Label | Platform | Interface | Speed | Duplex | Driver, version | Uplink |
|---|---|---|---|---|---|---|
| lan-linux-1 | Linux 6.8.0-142-generic | enp9s0f1 (10GBASE-T) | 10000Mb/s | Full | ixgbe 6.8.0-142-generic, fw 0x80000369 | yes |
| | | enp9s0f0 (10GBASE-T) | no link | - | ixgbe 6.8.0-142-generic, fw 0x80000369 | no |
| | | enp4s0, enp6s0 (1000BASE-T) | no link | - | igb 6.8.0-142-generic | no |
| lan-linux-2 | Linux 7.0.0-34-generic | enp9s0f0 (10GBASE-T) | 1000Mb/s | Full | ixgbe 7.0.0-34-generic, fw 0x80000389 | yes |
| | | enp9s0f1 (10GBASE-T) | no link | - | ixgbe 7.0.0-34-generic, fw 0x80000389 | no |
| | | enp4s0, enp6s0 (1000BASE-T) | no link | - | igb 7.0.0-34-generic | no |
| lan-linux-3 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| lan-linux-4 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| lan-linux-5 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| lan-linux-6 | unreachable | - | - | - | - | - |
| lan-windows-interop-1 | Windows, WSL2 6.18.33.2-microsoft-standard-WSL2 | Ethernet 2 (Intel I211) | 1 Gbps | Full | Intel e1r68x64.sys 12.18.11.1 | yes |
| | | Ethernet (Intel I211) | no link | - | Intel e1r68x64.sys 12.18.11.1 | no |
| lan-macos-1 | macOS 27.2 (Darwin 27.2.0) | en0 (Apple AQC113) | 10Gbase-T | Full | com.apple.driver.AppleEthernetAquantiaAqtion 1.0.64, fw 1.4.33 | yes |
| lan-macos-2 | macOS 27.0.1 (Darwin 27.0.0) | en0 (Realtek 8127, 10 GbE) | 10Gbase-T | Full | com.apple.driver.AppleEthernetRL 1.0.0d1, fw 0.1.0.202607081 | yes |
| lan-macos-3 | unreachable | - | - | - | - | - |
| lan-macos-4 | unreachable | - | - | - | - | - |

### lan-linux-1

- `timeout 20 ip route show default`: `default via <gateway> dev enp9s0f1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool enp9s0f1`: `Speed: 10000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes end at `10000baseT/Full`
- `timeout 20 ethtool -i enp9s0f1`: `driver: ixgbe`, `version: 6.8.0-142-generic`, `firmware-version: 0x80000369`
- `timeout 20 ethtool enp9s0f0`: `Speed: Unknown!`, `Link detected: no`
- `timeout 20 ethtool enp4s0` and `enp6s0`: `Link detected: no`; `ethtool -i`: `driver: igb`, `version: 6.8.0-142-generic`
- `timeout 20 ethtool wlp5s0`: `Link detected: no`; `ethtool -i`: `driver: iwlwifi` (Wi-Fi)
- `timeout 20 ip -s link show dev enp9s0f1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 3, dropped 153; TX errors 0

### lan-linux-2

- `timeout 20 ip route show default`: `default via <gateway> dev enp9s0f0 proto dhcp src <address> metric 1024`
- `timeout 20 ethtool enp9s0f0`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes end at `10000baseT/Full`
- `timeout 20 ethtool -i enp9s0f0`: `driver: ixgbe`, `version: 7.0.0-34-generic`, `firmware-version: 0x80000389`
- `timeout 20 ethtool enp9s0f1`: `Speed: Unknown!`, `Link detected: no`
- `timeout 20 ethtool enp4s0` and `enp6s0`: `Link detected: no`; `ethtool -i`: `driver: igb`, `version: 7.0.0-34-generic`
- `timeout 20 ethtool wlp5s0`: `Link detected: no`; `ethtool -i`: `driver: iwlwifi` (Wi-Fi)
- `timeout 20 ip -s link show dev enp9s0f0`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 38, dropped 261; TX errors 0

The 10G-capable port negotiates 1G. ixgbe prints no `Link partner advertised`
line, so whether the switch port or the cable holds it at 1G is not known.

### lan-linux-3

- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `driver: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 41, dropped 66; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no`; `ethtool -i`: `driver: iwlwifi` (Wi-Fi)

### lan-linux-4

- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `driver: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 0, dropped 704; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no`; `ethtool -i`: `driver: iwlwifi` (Wi-Fi)

### lan-linux-5

- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `driver: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 196, dropped 692; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no`; `ethtool -i`: `driver: iwlwifi` (Wi-Fi)

### lan-linux-6

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: No route to host`, exit 255

### lan-windows-interop-1

- `timeout 20 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Format-Table InterfaceAlias,RouteMetric,InterfaceMetric -AutoSize" </dev/null`: `Ethernet 2  0  25` (the other default route is a virtual overlay port at metric 9999)
- `timeout 20 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "Get-NetAdapter | Format-List Name,InterfaceDescription,Status,LinkSpeed,FullDuplex,MediaConnectionState,DriverProvider,DriverVersion,DriverFileName,Virtual" </dev/null`: `Name : Ethernet 2`, `InterfaceDescription : Intel(R) I211 Gigabit Network Connection #2`, `Status : Up`, `LinkSpeed : 1 Gbps`, `FullDuplex : True`, `DriverVersion : 12.18.11.1`, `DriverFileName : e1r68x64.sys`, `Virtual : False`
- the same command: `Name : Ethernet`, `Intel(R) I211 Gigabit Network Connection`, `Status : Disconnected`; `Name : Wi-Fi`, `Intel(R) Dual Band Wireless-AC 3168`, `Status : Disconnected`
- `timeout 20 ethtool eth2` (WSL2 side): `Speed: 10000Mb/s`, `driver: hv_netvsc`; this is the virtual adapter's nominal speed, not a physical link, so the row records the Windows adapter

### lan-macos-1

- `timeout 20 route -n get default`: `interface: en0`
- `timeout 20 ifconfig -v en0`: `media: autoselect (10Gbase-T <full-duplex,flow-control,energy-efficient-ethernet>)`, `status: active`, `link rate: 10.00 Gbps`
- `timeout 20 system_profiler SPEthernetDataType`: `Apple AQC113:`, `Firmware Version: 1.4.33`, `Driver: com.apple.driver.AppleEthernetAquantiaAqtion`, `Maximum Link Speed: 10 Gb/s`
- `timeout 20 kextstat -l`: `com.apple.driver.AppleEthernetAquantiaAqtion (1.0.64)`
- `timeout 20 networksetup -listallhardwareports`: `Hardware Port: Ethernet`, `Device: en0`; six more `Ethernet Adapter` ports, `en8` to `en13`
- `timeout 20 ifconfig -v en8` (and `en9` to `en13`): `type: USB Ethernet`, `media: none`, `status: inactive`
- `timeout 20 ifconfig -v en1` (Wi-Fi): `status: inactive`

### lan-macos-2

- `timeout 20 route -n get default`: `interface: en0`
- `timeout 20 ifconfig -v en0`: `media: autoselect (10Gbase-T <full-duplex,flow-control,energy-efficient-ethernet>)`, `status: active`, `link rate: 10.00 Gbps`
- `timeout 20 system_profiler SPEthernetDataType`: `Realtek 8127 (10 GbE):`, `Firmware Version: 0.1.0.202607081`, `Driver: com.apple.driver.AppleEthernetRL`, `Maximum Link Speed: 10 Gb/s`
- `timeout 20 kextstat -l`: `com.apple.driver.AppleEthernetRL (1.0.0d1)`
- `timeout 20 ifconfig -v en4` (and `en5`, `en6`, the `Ethernet Adapter` ports of `networksetup -listallhardwareports`): `media: none`, `status: inactive`
- `timeout 20 ifconfig -v en1` (Wi-Fi): `status: active`, `uplink rate: 456.77 Mbps [eff] / 748.80 Mbps`; not the default route

### lan-macos-3

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: Operation timed out`, exit 255

### lan-macos-4

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: Operation timed out`, exit 255

## Machines with 10G NICs

| Label | 10G NIC, driver line | Negotiated on the uplink |
|---|---|---|
| lan-linux-1 | 2 x 10GBASE-T, `driver: ixgbe` `version: 6.8.0-142-generic` `firmware-version: 0x80000369` | 10G |
| lan-linux-2 | 2 x 10GBASE-T, `driver: ixgbe` `version: 7.0.0-34-generic` `firmware-version: 0x80000389` | 1G |
| lan-linux-3 | AQC113, `driver: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-linux-4 | AQC113, `driver: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-linux-5 | AQC113, `driver: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-macos-1 | Apple AQC113, `Driver: com.apple.driver.AppleEthernetAquantiaAqtion` `Firmware Version: 1.4.33` | 10G |
| lan-macos-2 | Realtek 8127, `Driver: com.apple.driver.AppleEthernetRL` `Firmware Version: 0.1.0.202607081` | 10G |

lan-windows-interop-1 has no 10G NIC on its Windows side (two Intel I211, 1G).
Every Linux driver above is the in-tree driver: its version string is the
running kernel's release.

Machines that run at 10G today: lan-linux-1, lan-macos-1, lan-macos-2.

## Switch path between machines

All reachable machines use one default gateway: the same `default via
<gateway>` on every Linux row and on the WSL2 side of lan-windows-interop-1,
and the same `gateway: <gateway>` from `route -n get default` on both macOS
rows (compared on the raw output; the address is not recorded). Each pair is
therefore on one switched LAN: machine A's link to its switch port, then
the switch port to machine B. Each end's hop speed is read from that end's
negotiated link. The links between switches, if there is more than one switch,
are not visible from the hosts and are not recorded: a pair with 10G at both
ends is 10G end to end only if every switch link between them is.

| Pair | A's hop | B's hop | Limit from the two ends |
|---|---|---|---|
| lan-linux-1 to lan-macos-1 | 10G | 10G | 10G at both ends; switch links unknown |
| lan-linux-1 to lan-macos-2 | 10G | 10G | 10G at both ends; switch links unknown |
| lan-macos-1 to lan-macos-2 | 10G | 10G | 10G at both ends; switch links unknown |
| lan-linux-1, lan-macos-1 or lan-macos-2 to lan-linux-2, -3, -4, -5 or lan-windows-interop-1 | 10G | 1G | 1G |
| any two of lan-linux-2, -3, -4, -5 and lan-windows-interop-1 | 1G | 1G | 1G |

lan-linux-6, lan-macos-3 and lan-macos-4 did not answer and have no hop speed.

## Not recorded

- The rows marked `unreachable` (see their commands above).
- Switch-to-switch links and switch identities: the card's commands (ethtool, ip and the platform equivalents) do not show them; LLDP is not read.
- Whether any machine is held for XDP: no row carries `xdp-held`; that mark is
  the coordinator's, when a machine is held for the xdp run.
