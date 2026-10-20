# LAN inventory for the transport throughput and 1000-client phases

This is the read-only inventory of the LAN machines that docs/LAN-BENCH.md
chooses its hosts from. It records what each machine's NICs negotiate today;
it chooses no host and designs nothing.

Labels follow the shapes of docs/LIVE-XDP.md section 8 (`linux`,
`windows-interop`, `macos`) with a `lan-` prefix. The label-to-host map lives
in the coordinator's inventory file outside the repository; this document
carries no host name, address or MAC. The dedicated test server is not a LAN
machine (the owner's ruling on the card places it outside the LAN; nothing
here measured that), so it has no row here.

Every command ran read-only under `timeout 20` (on macOS without GNU timeout,
a `perl -e 'alarm 20; exec @ARGV'` equivalent), from script files piped over
ssh, on 2026-10-18. The table has a row for every physical interface the
commands list: wired Ethernet, Wi-Fi, USB Ethernet and IP over Thunderbolt. A
field the commands did not print is `not reported`; a duplex of `-` means the
port has no link (ethtool prints `Duplex: Unknown! (255)`, Get-NetAdapter an
empty `FullDuplex`, ifconfig `media: none`). The Platform column comes from
`uname -sr` on Linux and WSL2 and `uname -sr; sw_vers -productVersion` on
macOS, run first in each machine's script. Virtual and tunnel
adapters (Windows `Virtual : True`, the Hyper-V, overlay and tunnel ports, the
WSL2 `hv_netvsc` port, the macOS `bridge0` Thunderbolt Bridge) are not
physical NICs and have no row. "Uplink" is the interface of the default route. The `ip -s link` error counters are from the second read of the
same command (script `inv-linux-modes.sh`), a few minutes after the first.

## Machines

| Label | Platform | Interface | Speed | Duplex | Transport, version | Uplink |
|---|---|---|---|---|---|---|
| lan-linux-1 | Linux 6.8.0-142-generic | enp9s0f1 (10GBASE-T) | 10000Mb/s | Full | ixgbe 6.8.0-142-generic, fw 0x80000369 | yes |
| | | enp9s0f0 (10GBASE-T) | no link | - | ixgbe 6.8.0-142-generic, fw 0x80000369 | no |
| | | enp4s0, enp6s0 (1000BASE-T) | no link | - | igb 6.8.0-142-generic | no |
| | | wlp5s0 (Wi-Fi) | no link | not reported | iwlwifi 6.8.0-142-generic, fw 29.198743027.0 3168-29.ucode | no |
| lan-linux-2 | Linux 7.0.0-34-generic | enp9s0f0 (10GBASE-T) | 1000Mb/s | Full | ixgbe 7.0.0-34-generic, fw 0x80000389 | yes |
| | | enp9s0f1 (10GBASE-T) | no link | - | ixgbe 7.0.0-34-generic, fw 0x80000389 | no |
| | | enp4s0, enp6s0 (1000BASE-T) | no link | - | igb 7.0.0-34-generic | no |
| | | wlp5s0 (Wi-Fi) | no link | not reported | iwlwifi 7.0.0-34-generic, fw 29.0bd893f3.0 3168-29.ucode | no |
| lan-linux-3 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| | | wlp2s0 (Wi-Fi) | no link | not reported | iwlwifi 7.0.0-38-generic, fw 77.f39cc7f9.0 cc-a0-77.ucode | no |
| lan-linux-4 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| | | wlp2s0 (Wi-Fi) | no link | not reported | iwlwifi 7.0.0-38-generic, fw 77.f39cc7f9.0 cc-a0-77.ucode | no |
| lan-linux-5 | Linux 7.0.0-38-generic | eno1 (AQC113, up to 10GBASE-T) | 1000Mb/s | Full | atlantic 7.0.0-38-generic, fw 1.5.45 | yes |
| | | wlp2s0 (Wi-Fi) | no link | not reported | iwlwifi 7.0.0-38-generic, fw 77.f39cc7f9.0 cc-a0-77.ucode | no |
| lan-linux-6 | unreachable | - | - | - | - | - |
| lan-windows-interop-1 | Windows, WSL2 6.18.33.2-microsoft-standard-WSL2 | Ethernet 2 (Intel I211) | 1 Gbps | Full | Intel e1r68x64.sys 12.18.11.1 | yes |
| | | Ethernet (Intel I211) | no link | - | Intel e1r68x64.sys 12.18.11.1 | no |
| | | Wi-Fi (Intel Dual Band Wireless-AC 3168) | no link | no link (capability Full) | Intel Netwtw04.sys 19.51.50.2 | no |
| lan-macos-1 | macOS 27.2 (Darwin 27.2.0) | en0 (Apple AQC113) | 10Gbase-T | Full | com.apple.transport.AppleEthernetAquantiaAqtion 1.0.64, fw 1.4.33 | yes |
| | | en1 (Wi-Fi) | no link | not reported | not reported | no |
| | | en8 to en13 (USB Ethernet) | no link | - | not reported | no |
| | | en2 to en7 (IP over Thunderbolt) | no link | no link (media `<full-duplex>`) | not reported | no |
| lan-macos-2 | macOS 27.0.1 (Darwin 27.0.0) | en0 (Realtek 8127, 10 GbE) | 10Gbase-T | Full | com.apple.transport.AppleEthernetRL 1.0.0d1, fw 0.1.0.202607081 | yes |
| | | en1 (Wi-Fi) | 748.80 Mbps max, 456.77 Mbps effective | not reported | not reported | no |
| | | en4, en5, en6 (USB Ethernet) | no link | - | not reported | no |
| | | en2, en3, en7 (IP over Thunderbolt) | no link | no link (media `<full-duplex>`) | not reported | no |
| lan-macos-3 | unreachable | - | - | - | - | - |
| lan-macos-4 | unreachable | - | - | - | - | - |

### lan-linux-1

- `uname -sr`: `Linux 6.8.0-142-generic`
- `timeout 20 ip route show default`: `default via <gateway> dev enp9s0f1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool enp9s0f1`: `Speed: 10000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes end at `10000baseT/Full`
- `timeout 20 ethtool -i enp9s0f1`: `transport: ixgbe`, `version: 6.8.0-142-generic`, `firmware-version: 0x80000369`
- `timeout 20 ethtool enp9s0f0`: `Speed: Unknown!`, `Duplex: Unknown! (255)`, `Link detected: no`; `ethtool -i enp9s0f0`: `transport: ixgbe`, `version: 6.8.0-142-generic`, `firmware-version: 0x80000369`
- `timeout 20 ethtool enp4s0` and `enp6s0`: `Speed: Unknown!`, `Duplex: Unknown! (255)`, `Link detected: no`; `ethtool -i`: `transport: igb`, `version: 6.8.0-142-generic`
- `timeout 20 ethtool wlp5s0`: `Link detected: no` (no `Speed` or `Duplex` line); `ethtool -i wlp5s0`: `transport: iwlwifi`, `version: 6.8.0-142-generic`, `firmware-version: 29.198743027.0 3168-29.ucode`
- `timeout 20 ip -s link show dev enp9s0f1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 3, dropped 153; TX errors 0

### lan-linux-2

- `uname -sr`: `Linux 7.0.0-34-generic`
- `timeout 20 ip route show default`: `default via <gateway> dev enp9s0f0 proto dhcp src <address> metric 1024`
- `timeout 20 ethtool enp9s0f0`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes end at `10000baseT/Full`
- `timeout 20 ethtool -i enp9s0f0`: `transport: ixgbe`, `version: 7.0.0-34-generic`, `firmware-version: 0x80000389`
- `timeout 20 ethtool enp9s0f1`: `Speed: Unknown!`, `Duplex: Unknown! (255)`, `Link detected: no`; `ethtool -i enp9s0f1`: `transport: ixgbe`, `version: 7.0.0-34-generic`, `firmware-version: 0x80000389`
- `timeout 20 ethtool enp4s0` and `enp6s0`: `Speed: Unknown!`, `Duplex: Unknown! (255)`, `Link detected: no`; `ethtool -i`: `transport: igb`, `version: 7.0.0-34-generic`
- `timeout 20 ethtool wlp5s0`: `Link detected: no` (no `Speed` or `Duplex` line); `ethtool -i wlp5s0`: `transport: iwlwifi`, `version: 7.0.0-34-generic`, `firmware-version: 29.0bd893f3.0 3168-29.ucode`
- `timeout 20 ip -s link show dev enp9s0f0`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 38, dropped 261; TX errors 0

The 10G-capable port negotiates 1G. ixgbe prints no `Link partner advertised`
line, so whether the switch port or the cable holds it at 1G is not known.

### lan-linux-3

- `uname -sr`: `Linux 7.0.0-38-generic`
- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `transport: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 41, dropped 66; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no` (no `Speed` or `Duplex` line); `ethtool -i wlp2s0`: `transport: iwlwifi`, `version: 7.0.0-38-generic`, `firmware-version: 77.f39cc7f9.0 cc-a0-77.ucode`

### lan-linux-4

- `uname -sr`: `Linux 7.0.0-38-generic`
- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `transport: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 0, dropped 704; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no` (no `Speed` or `Duplex` line); `ethtool -i wlp2s0`: `transport: iwlwifi`, `version: 7.0.0-38-generic`, `firmware-version: 77.f39cc7f9.0 cc-a0-77.ucode`

### lan-linux-5

- `uname -sr`: `Linux 7.0.0-38-generic`
- `timeout 20 ip route show default`: `default via <gateway> dev eno1 proto dhcp src <address> metric 100`
- `timeout 20 ethtool eno1`: `Speed: 1000Mb/s`, `Duplex: Full`, `Link detected: yes`; supported modes include `10000baseT/Full`; `Link partner advertised link modes:` end at `1000baseT/Full`
- `timeout 20 ethtool -i eno1`: `transport: atlantic`, `version: 7.0.0-38-generic`, `firmware-version: 1.5.45`
- `timeout 20 ip -s link show dev eno1`: `<BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP`; RX errors 196, dropped 692; TX errors 0
- `timeout 20 ethtool wlp2s0`: `Link detected: no` (no `Speed` or `Duplex` line); `ethtool -i wlp2s0`: `transport: iwlwifi`, `version: 7.0.0-38-generic`, `firmware-version: 77.f39cc7f9.0 cc-a0-77.ucode`

### lan-linux-6

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: No route to host`, exit 255

### lan-windows-interop-1

- `uname -sr` (WSL2 side): `Linux 6.18.33.2-microsoft-standard-WSL2`; the Windows version is not read, so the Platform column says only `Windows`
- `timeout 20 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "Get-NetRoute -DestinationPrefix 0.0.0.0/0 | Format-Table InterfaceAlias,RouteMetric,InterfaceMetric -AutoSize" </dev/null`: `Ethernet 2  0  25` (the other default route is a virtual overlay port at metric 9999)
- `timeout 20 /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "Get-NetAdapter | Format-List Name,InterfaceDescription,Status,LinkSpeed,FullDuplex,MediaConnectionState,TransportProvider,TransportVersion,TransportFileName,Virtual" </dev/null`: `Name : Ethernet 2`, `InterfaceDescription : Intel(R) I211 Gigabit Network Connection #2`, `Status : Up`, `LinkSpeed : 1 Gbps`, `FullDuplex : True`, `TransportVersion : 12.18.11.1`, `TransportFileName : e1r68x64.sys`, `Virtual : False`
- the same command: `Name : Ethernet`, `InterfaceDescription : Intel(R) I211 Gigabit Network Connection`, `Status : Disconnected`, `LinkSpeed : 0 bps`, `FullDuplex : ` (empty), `TransportVersion : 12.18.11.1`, `TransportFileName : e1r68x64.sys`
- the same command: `Name : Wi-Fi`, `InterfaceDescription : Intel(R) Dual Band Wireless-AC 3168`, `Status : Disconnected`, `LinkSpeed : 0 bps`, `FullDuplex : True` (the adapter's capability; there is no link to negotiate), `TransportVersion : 19.51.50.2`, `TransportFileName : Netwtw04.sys`, `Virtual : False`
- `timeout 20 ethtool eth2` (WSL2 side): `Speed: 10000Mb/s`, `transport: hv_netvsc`; this is the virtual adapter's nominal speed, not a physical link, so the row records the Windows adapter

### lan-macos-1

- `uname -sr; sw_vers -productVersion`: `Darwin 27.2.0`, `27.2`
- `timeout 20 route -n get default`: `interface: en0`
- `timeout 20 ifconfig -v en0`: `media: autoselect (10Gbase-T <full-duplex,flow-control,energy-efficient-ethernet>)`, `status: active`, `link rate: 10.00 Gbps`
- `timeout 20 system_profiler SPEthernetDataType`: `Apple AQC113:`, `Firmware Version: 1.4.33`, `Transport: com.apple.transport.AppleEthernetAquantiaAqtion`, `Maximum Link Speed: 10 Gb/s`
- `timeout 20 kextstat -l`: `com.apple.transport.AppleEthernetAquantiaAqtion (1.0.64)`
- `timeout 20 networksetup -listallhardwareports`: `Hardware Port: Ethernet`, `Device: en0`; six more `Ethernet Adapter` ports, `en8` to `en13`; `Hardware Port: Wi-Fi`, `Device: en1`; `Hardware Port: Thunderbolt 1` to `6`, `Device: en2` to `en7`
- `timeout 20 ifconfig -v en8` (and `en9` to `en13`): `type: USB Ethernet`, `media: none`, `status: inactive`
- `timeout 20 ifconfig -v en1`: `media: autoselect`, `status: inactive`, `type: Wi-Fi`; no duplex in `media`
- `timeout 20 ifconfig -v en2` (and `en3` to `en7`): `media: autoselect <full-duplex>`, `status: inactive`, `type: IP over Thunderbolt`
- `timeout 20 system_profiler SPEthernetDataType` lists no transport for the Wi-Fi, USB Ethernet or Thunderbolt ports by device name (its one USB entry, `Transport: com.apple.transport.usb.cdc.ncm`, a display's USB Ethernet function, is not tied to an `en` number by the commands), so their transport is `not reported`

### lan-macos-2

- `uname -sr; sw_vers -productVersion`: `Darwin 27.0.0`, `27.0.1`
- `timeout 20 route -n get default`: `interface: en0`
- `timeout 20 networksetup -listallhardwareports`: `Hardware Port: Ethernet`, `Device: en0`; `Hardware Port: Ethernet Adapter (en4)` to `(en6)`; `Hardware Port: Wi-Fi`, `Device: en1`; `Hardware Port: Thunderbolt 1` to `3`, `Device: en2`, `en7`, `en3`
- `timeout 20 ifconfig -v en0`: `media: autoselect (10Gbase-T <full-duplex,flow-control,energy-efficient-ethernet>)`, `status: active`, `link rate: 10.00 Gbps`
- `timeout 20 system_profiler SPEthernetDataType`: `Realtek 8127 (10 GbE):`, `Firmware Version: 0.1.0.202607081`, `Transport: com.apple.transport.AppleEthernetRL`, `Maximum Link Speed: 10 Gb/s`
- `timeout 20 kextstat -l`: `com.apple.transport.AppleEthernetRL (1.0.0d1)`
- `timeout 20 ifconfig -v en4` (and `en5`, `en6`): `media: none`, `status: inactive`, `type: USB Ethernet`
- `timeout 20 ifconfig -v en1`: `media: autoselect`, `status: active`, `type: Wi-Fi`, `uplink rate: 456.77 Mbps [eff] / 748.80 Mbps`, `downlink rate: 456.77 Mbps [eff] / 748.80 Mbps [max]`; no duplex in `media`; not the default route (`route -n get default` names `en0`)
- `timeout 20 ifconfig -v en2` (and `en3`, `en7`): `media: autoselect <full-duplex>`, `status: inactive`, `type: IP over Thunderbolt`
- `timeout 20 system_profiler SPEthernetDataType` lists only the Realtek 8127, so the Wi-Fi, USB Ethernet and Thunderbolt transports are `not reported`

### lan-macos-3

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: Operation timed out`, exit 255

### lan-macos-4

- `timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=15 <host> true`: `ssh: connect to host <address> port 22: Operation timed out`, exit 255

## Machines with 10G NICs

| Label | 10G NIC, transport line | Negotiated on the uplink |
|---|---|---|
| lan-linux-1 | 2 x 10GBASE-T, `transport: ixgbe` `version: 6.8.0-142-generic` `firmware-version: 0x80000369` | 10G |
| lan-linux-2 | 2 x 10GBASE-T, `transport: ixgbe` `version: 7.0.0-34-generic` `firmware-version: 0x80000389` | 1G |
| lan-linux-3 | AQC113, `transport: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-linux-4 | AQC113, `transport: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-linux-5 | AQC113, `transport: atlantic` `version: 7.0.0-38-generic` `firmware-version: 1.5.45` | 1G |
| lan-macos-1 | Apple AQC113, `Transport: com.apple.transport.AppleEthernetAquantiaAqtion` `Firmware Version: 1.4.33` | 10G |
| lan-macos-2 | Realtek 8127, `Transport: com.apple.transport.AppleEthernetRL` `Firmware Version: 0.1.0.202607081` | 10G |

lan-windows-interop-1 has no 10G NIC on its Windows side (two Intel I211, 1G).
Every Linux transport above is the in-tree transport: its version string is the
running kernel's release.

Machines that run at 10G today: lan-linux-1, lan-macos-1, lan-macos-2.

## Switch path between machines

The commands read each host's own link only: its negotiated speed to whatever
its cable reaches (the commands do not show whether that is a switch). They do not identify that switch, whether two
hosts are on one layer-2 segment, any switch-to-switch link, or the path
traffic takes between two hosts. So each pair's path below has three parts:
A's link (read), the middle (`unknown`), and B's link (read). No pair is shown
to be 10G end to end; a pair with 10G at both ends is 10G end to end only if
every hop in the middle is, and that needs switch or LLDP evidence, which this
inventory does not read. The slower end is an upper bound on the pair's path
speed, nothing more.

| Pair | A's link | Middle | B's link | Path |
|---|---|---|---|---|
| lan-linux-1 to lan-macos-1 | 10G | unknown | 10G | unknown; at most 10G |
| lan-linux-1 to lan-macos-2 | 10G | unknown | 10G | unknown; at most 10G |
| lan-macos-1 to lan-macos-2 | 10G | unknown | 10G | unknown; at most 10G |
| lan-linux-1, lan-macos-1 or lan-macos-2 to lan-linux-2, -3, -4, -5 or lan-windows-interop-1 | 10G | unknown | 1G | unknown; at most 1G |
| any two of lan-linux-2, -3, -4, -5 and lan-windows-interop-1 | 1G | unknown | 1G | unknown; at most 1G |

lan-linux-6, lan-macos-3 and lan-macos-4 did not answer and have no hop speed.

## Not recorded

- The rows marked `unreachable` (see their commands above).
- The middle of every pair's path: switch identities, layer-2 segments and switch-to-switch links. The card's commands (ethtool, ip and the platform equivalents) do not show them, and LLDP is not read; proving any pair 10G end to end needs that evidence from a separate, authorized read.
- Whether any machine is held for XDP: no row carries `xdp-held`; that mark is
  the coordinator's, when a machine is held for the xdp run.
