# Linux thirty-minute soak per transport

## Status

Verdict: every cell PASSES under the applied impairment, and all five cells
ran at one source head, `adca4345bf963c7bcb5396ce5cd80a5111793533`. The rows
under "Cell results" are the record.

Two earlier sets of rows are superseded:

- The original rows (source head
  `720393a0aadd60ac43a8b5bcf887c7c51c6be999`) ran on an unimpaired link. That
  version of `scripts/soak-per-transport` set environment names the library
  does not read, and soak's transport creates dropped the link simulator
  fields, so none of the five cells applied the loss, latency, jitter or
  duplication the record claimed. They are not evidence of an impaired run.
- The first impaired re-run (source head
  `e8d209cc3f445b9dba7f8f585d7fd88f054e4b50`) passed the four socket cells
  and failed the xdp cell (it never connected under the impairment, a
  library defect on the xdp transport with the server's link simulator
  active, since fixed). The library and soak code changed after that head,
  so those rows no longer describe the code under test. They are kept under
  "Superseded rows" and are replaced by the rows below.

The fixed harness sets the library's `ROCKETNET_DEV_SERVER_*` and
`ROCKETNET_DEV_CLIENT_*` link simulator names, soak prints the setting each
end actually applied (`soak link simulator: <end> ...`), and `--assert-flat`
refuses an impaired run in which the impairment leaves no trace (nothing
missing, or no already-received refusal, in either direction).

## Setup

- Source head for every cell: `adca4345bf963c7bcb5396ce5cd80a5111793533`.
  It contains the soak impairment fix and the xdp fix for replies the link
  simulator holds (`c37fb637`). Every machine checked out and built that
  head, and each cell's log was taken from that build.
- Date (`date -u`): Sun Oct 25 2026 UTC. gso ran from 12:32:14 to 13:02:14,
  gso-plain from 13:08:43 to 13:38:44, io_uring from 13:39:14 to 14:09:14,
  portable from 14:12:25 to 14:42:26, and xdp from 15:21:53 to 15:51:58.
- Impairment, at both ends of the link simulator: loss 5%, latency 30 ms,
  jitter 20 ms, duplication 2%.
- Every cell ran `scripts/soak-per-transport --seconds 1800 --cell <cell>
  --log-dir <dir>` (which runs `bin/soak --seconds 1800 --assert-flat`)
  under the bench slot lock (`benchslot -- ...`), one soak per machine at a
  time, with the machine's one-minute load sampled every ten seconds. A run
  whose machine went over its core count is not evidence and was discarded
  (see "Discarded runs").
- Socket machine A (gso): 16 cores, `uname -r` = `7.0.0-38-generic`.
  Socket machine B (gso-plain, io_uring, portable, one after another):
  32 cores, `uname -r` = `6.8.0-139-generic`. Both have
  `/proc/sys/kernel/io_uring_disabled` = `0`. All socket traffic used
  loopback, each cell on its own port (`SOAK_SERVER_ADDRESS=127.0.0.1:<port>`).
- XDP machine: the dedicated XDP bench, 17 cores, `uname -r` =
  `6.8.0-142-generic`. The script's virtme-ng guest of that kernel, a
  guest-only veth pair, server on `10.77.4.1:40412`, clients in the guest's
  far namespace. Nothing was attached to any interface outside the guest.
  For the xdp cell the script's guest commands also ran a read-only counter
  capture before, every 30 seconds during, and after the soak; the soak
  command line and its environment are the script's, unchanged. The XDP
  machine's one-minute load during the run peaked at 7.87, no sample over
  its 17 cores.
- Every machine built that head with `make -j4` under the slot lock; each
  build exited 0 with the last line `[100%] Built target functional`.

## Applied settings

Every cell's log carries, for both ends, exactly the setting the cell
asked for:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
```

The granted transports, from each log's `soak transport:` lines: server
`portable`, `gso`, `gso` (gso-plain), `io_uring` and `xdp`; client
`portable` in every cell.

## Cell results

Witness counts are soak's `soak link witness:` lines: payloads missing in
each direction, and the receiving end's already-received refusals of the
planted duplicates.

| Cell | Granted server/client | Iterations / connects | Sent / received / verified | Missing c->s / s->c | Duplicates refused at server / client | Max load (cores) | Result |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| portable | portable / portable | 1,648,340 / 60 | 3,262,860 / 3,096,886 / 3,096,886 | 79,959 / 81,531 | 50,101 / 49,413 | 18.86 (32) | PASS |
| gso | gso / portable | 1,591,288 / 60 | 3,150,902 / 2,990,996 / 2,990,996 | 77,121 / 78,430 | 47,486 / 50,452 | 4.79 (16) | PASS |
| gso-plain | gso / portable | 1,650,393 / 60 | 3,268,646 / 3,102,751 / 3,102,751 | 79,886 / 81,470 | 49,334 / 52,297 | 15.60 (32) | PASS |
| io_uring | io_uring / portable | 1,641,499 / 60 | 3,251,788 / 3,086,956 / 3,086,956 | 79,123 / 81,281 | 49,093 / 50,070 | 21.38 (32) | PASS |
| xdp | xdp / portable | 1,589,029 / 60 | 3,136,954 / 2,898,290 / 2,898,290 | 76,711 / 152,838 | 43,539 / 44,645 | 7.87 (17) | PASS |

Every cell printed `conservation ok (server and every client)`, 60 connects
(59 reconnects, the planned cycles) and zero failures, and the script's
judge line named `link=impaired`. The xdp log has no `SEND_NO_LINK_ADDRESS`
line.

The cells' `SOAK` lines:

```text
SOAK cell=portable PASS server=portable client=portable seconds=1800 verified=3096886 fds=8/8/10 threads=3/3/4 resident_bytes=140107776/131743744/140738560 link=impaired missing=79959/81531 duplicates_refused=50101/49413
SOAK cell=gso PASS server=gso client=portable seconds=1800 verified=2990996 fds=10/10/12 threads=6/6/7 resident_bytes=293654528/293654528/294313984 link=impaired missing=77121/78430 duplicates_refused=47486/50452
SOAK cell=gso-plain PASS server=gso client=portable seconds=1800 verified=3102751 fds=10/10/12 threads=6/6/7 resident_bytes=343330816/334966784/343965696 link=impaired missing=79886/81470 duplicates_refused=49334/52297
SOAK cell=io_uring PASS server=io_uring client=portable seconds=1800 verified=3086956 fds=10/10/12 threads=1/1/2 resident_bytes=1277648896/1269284864/1278337024 link=impaired missing=79123/81281 duplicates_refused=49093/50070
SOAK cell=xdp PASS server=xdp client=portable seconds=1800 verified=2898290 fds=18/18/20 threads=2/2/3 resident_bytes=584273920/575909888/588943360 link=impaired missing=76711/152838 duplicates_refused=43539/44645
```

### The xdp cell's server-to-client loss

The xdp cell's server-to-client missing count (152,838 of 1,568,477 sent,
about 9.7%) is about twice its client-to-server count and twice the socket
cells'. The run sampled the guest's counters before, every 30 seconds
during, and after the soak (`ethtool -S` on both veth ends, `ip -s -s link`,
`ss --xdp -e` for the AF_XDP socket's statistics, `/proc/net/snmp` in both
namespaces, `/proc/net/softnet_stat`). The split:

- The link simulator's own drops: the library keeps no count of what its
  simulator drops (see soak.c, "the library counts none of its simulator's
  drops"), so there is no applied-drop counter to quote. The
  client-to-server direction, where every other counter below is zero, lost
  76,711 (4.9% of 1,568,477), which is what 5% loss leaves; the same share
  of the server's sends is about 76,000 to 78,000.
- The client's UDP socket receive buffer: the client namespace's `Udp:`
  line went from all zero to `InDatagrams 2283570 NoPorts 2698 InErrors
  76521 RcvbufErrors 76521`. Those 76,521 datagrams reached the client's
  namespace and were dropped by its kernel because the socket's receive
  buffer was full (the client's socket showed `rb425984`).
- AF_XDP and the veth pair: no drops. The server socket's final in-run
  `stats(rx dropped:0,rx invalid:0,rx queue full:0,rx fill ring empty:0,tx
  invalid:0,...)`; both veth ends show zero `rx_queue_0_drops`,
  `xdp_drops`, `xdp_tx_errors` and `xdp_xmit_errors`, and zero RX/TX errors
  and drops in `ip -s -s link`; softnet's dropped column stayed zero. The
  server's queue redirected 2,296,884 frames, equal to the client
  namespace's `OutDatagrams 2296884`.
- The library's counters: no `SEND_RING_FULL`, `SEND_LEASE_FULL` or
  `SEND_NO_LINK_ADDRESS`; `server_SEND_HISTORY_LOST 178139` and
  `client_SEND_HISTORY_LOST 105124`.

76,711 + 76,521 = 153,232, within 0.3% of the 152,838 missing, so the
excess over 5% is the client's receive buffer overflowing, not a second
simulator drop. The receive-buffer drops count every server-to-client
datagram (payloads, acks, keep-alives), not payloads alone, so the split is
an estimate, not an exact per-payload count. Why the client's buffer fills
on this cell and not on the socket cells (the xdp server's send burst
pattern into the guest's veth is the candidate) is not measured here.

## Resource start / end / peak

The post-warmup process-resource lines (`soak resources:`). Equal fd and
thread starts and ends, and resident and library memory ends no greater
than starts, satisfied `--assert-flat` in every cell.

| Cell | fds | threads | resident_bytes | library_bytes |
| --- | --- | --- | --- | --- |
| portable | 8 / 8 / 10 | 3 / 3 / 4 | 140,107,776 / 131,743,744 / 140,738,560 | 105,681,257 / 105,681,257 / 125,553,094 |
| gso | 10 / 10 / 12 | 6 / 6 / 7 | 293,654,528 / 293,654,528 / 294,313,984 | 283,739,481 / 283,739,481 / 303,611,318 |
| gso-plain | 10 / 10 / 12 | 6 / 6 / 7 | 343,330,816 / 334,966,784 / 343,965,696 | 283,739,481 / 283,739,481 / 303,611,318 |
| io_uring | 10 / 10 / 12 | 1 / 1 / 2 | 1,277,648,896 / 1,269,284,864 / 1,278,337,024 | 1,259,989,695 / 1,259,989,695 / 1,279,861,532 |
| xdp | 18 / 18 / 20 | 2 / 2 / 3 | 584,273,920 / 575,909,888 / 588,943,360 | 558,094,915 / 558,094,915 / 577,966,752 |

## Discarded runs

Each of these passed, but its machine's one-minute load went over its core
count from unrelated work during the run, so it is not evidence:

- portable on a 16-core machine (`verified=2974083`): eleven samples over 16,
  peak 36.92.
- io_uring on a 16-core machine (`verified=3004588`): eight samples over 16,
  peak 27.32.
- gso-plain on machine B (`verified=3110583`): one sample over 32, 35.51.
- portable on a 16-core machine (`verified=2993620`): one sample over 16,
  16.73.
- io_uring on a 16-core machine (`verified=3001154`): thirteen samples over
  16, peak 31.61.
- portable on a 16-core machine (`verified=3033110`): fourteen samples over
  16, peak 34.44.

## Superseded rows

### First impaired re-run, socket cells (`e8d209cc`)

| Cell | Iterations / connects | Verified | Missing c->s / s->c | Duplicates refused at server / client | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| portable | 1,591,080 / 60 | 2,992,676 | 77,295 / 78,883 | 48,450 / 48,542 | PASS, superseded |
| gso | 1,595,706 / 60 | 2,998,619 | 77,293 / 78,677 | 47,575 / 50,563 | PASS, superseded |
| gso-plain | 1,591,831 / 60 | 2,990,872 | 77,430 / 78,576 | 47,417 / 50,428 | PASS, superseded |
| io_uring | 1,580,446 / 60 | 2,970,122 | 76,767 / 78,016 | 47,139 / 48,181 | PASS, superseded |

### First impaired re-run, xdp cell (`e8d209cc`)

`SOAK cell=xdp FAIL soak exited 1: soak: client connection failed
(ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2)`,
superseded by the xdp row above. The run ended after 15 seconds; its log
tail:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server xdp
soak transport: client portable
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak: client connection failed (ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2)
soak: insufficient coverage: require a connection and verified payloads in both directions
soak: bounded run ended before its duration target
soak: REFUSAL on a clean run at server: SEND_NO_LINK_ADDRESS = 93
soak link witness: client_to_server missing=0 late=0 repeated=0 duplicates_refused=0
soak link witness: server_to_client missing=0 late=0 repeated=0 duplicates_refused=0
soak: --assert-flat FAILED: link impairment not observed: client loss_percent=5 and client_to_server missing=0
soak: --assert-flat FAILED: link impairment not observed: server loss_percent=5 and server_to_client missing=0
soak: --assert-flat FAILED: no post-warmup and end samples (the run ended inside the warm cycle)
soak finished: 9342 iterations, 0 connects, 0 payloads verified, 9 failures
soak summary: server_CONNECTION_REQUEST_RECEIVED 94
soak summary: server_CONNECTION_REQUEST_NO_COOKIE 94
soak summary: server_SEND_NO_LINK_ADDRESS 93
```

The server refused 93 of its 94 cookie replies with `SEND_NO_LINK_ADDRESS`.
The guest's veth has no gateway, so the xdp driver could address a reply
only to a client whose MAC it had learned, or as the answer to a frame in
the receive view it was processing. A cookie reply held by the server's link
simulator was sent later from `rocketnet_server_link_drain`, outside any
receive view, before the client's MAC was learned, so every cookie reply was
refused. The library fix (`c37fb637`) removed this; the xdp row above has 60
connects and no `SEND_NO_LINK_ADDRESS`.

## Raw logs

The kept raw soak logs' SHA-256 digests:

```text
portable   40330743ff0cc1447ea2e76958d1dc0d889dedda861968c1a071c13c336a2d53
gso        1211a06a5b9db38c72c8ea726caba04218f92fdc48f95b6592c296f036a52fa3
gso-plain  bd435c3cfa4efe40eeb1c0f1c2dd01328fe6d1dcc7156449f4780251fded746a
io_uring   672cd37d8e72cf8e78e1562013caca306dcd57ce2ecdc68e1ab4c589c20ebe4c
xdp        ac44c9d39a7b2ff27d28b880d79a3f5064b4102315e8bcbdbffc7589778df558
```

Superseded xdp run at `77178a490babc8902508e0f24c546f5a2f1cf67f` (a passing
1800 s run at a different source head from the socket cells, replaced by
the xdp row above so that every cell shares one head):
`e29d34105b01ffed2b2ad0cc658d67db84a89c0e48f67736f2c74e260ab40d34`, `SOAK
cell=xdp PASS ... verified=2896529 ... missing=77109/165137
duplicates_refused=43434/44302`.

Superseded (`e8d209cc`): portable
`7444caa191c5bef4a0209f1da7209618f26a3f7af95b35184f93fe0de026487a`, gso
`4a8904070e90e741b15c5f40de46f948379aa90f21b344760885d6df90ca47e4`,
gso-plain `97884f77fcc9dd780f2ee13ea9416f8d775175c34243b3b9d9dca8c0128a8064`,
io_uring `1a0e7658b449302a133531ec1d4dc2ace340d7d16d5e9b47821bbba8f63d81ff`,
xdp `631fe5c9c2f7689a2eb3af3282c5126c4a252e09b4261483548c464ef8b8b551`.
