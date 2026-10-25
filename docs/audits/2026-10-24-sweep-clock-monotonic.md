# Monotonic clock assumption sweep, 2026-10-24

A sweep of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd` (`main`) for one
bug class: code that assumes time never goes backwards. That covers a comparison or difference of two
time values that comes out wrong, negative, or huge after an unsigned subtraction, or that wipes state,
when the clock steps back; a bucket or window advance; and a timeout or deadline. Scope read: every
`.cpp` and `.h` in `sdk/` (`sdk/rocketnet_tests.cpp` excluded, because it defines only test cases and no
production code calls into it), every `.cpp`, `.h` and `.inc` in `functional/`, and every `tests/*.h`
helper. The public contract matters here. `sdk/rocketnet.h:972` lists a time that goes backwards as API
misuse. `USAGE.md` asks for a non-decreasing clock. Both public update calls pass the caller's time
through one clamp, `rocketnet_clamp_update_time` (`sdk/rocketnet_client_transport.cpp:55`), and so does
the message object's update (`sdk/rocketnet_message_server.cpp:593`). The clamp holds the last good time,
so no code past it ever sees game time decrease. Two clocks never pass through the clamp: the unix wall
clock (`::time( NULL )`), which the server and client read for token, cookie and budget checks when no
`game_clock_epoch` is configured, and the client's receive-stamp clock. Method: grep, then read each hit
inside its function and follow the value back to its source (the clamp, the platform monotonic clock, the
wall clock, a token field or a test clock) and forward to its use. For each hit, the question was what a
backwards step of one frame, one second or one hour does. The answers are: nothing; a stall until time
catches up (with its length); or a wrong answer. Search patterns and hit counts (comment lines and
`#include` lines dropped):
(P1) `(time|now|deadline|timeout|_ts|timestamp|last_[a-z_]*)\b` over `sdk/*.cpp sdk/*.h`, narrowed to
lines holding a comparison (`<`, `>`, `<=`, `>=`, with `->`, `<<` and `>>` excluded) or a subtraction
(` - `): 128 hits.
(P1b) the same pattern and narrowing over `functional/*.cpp functional/*.h functional/*.inc tests/*.h`:
289 hits.
(P2) a widening, added after reading `sdk/rocketnet_frame_sync.cpp` showed nanosecond deadlines that P1
missed: `\b[a-z_]*(_ns|elapsed|_age|period|expire|_since|_until|_at)\b` over `sdk/`, with the same
narrowing and without the P1 lines: 17 hits.
(P3) every subtraction on a unix timestamp, `(timestamp|unix_ts)\b` on a line holding ` - `, over all
three trees: 4 hits, all already among P1 and P1b.
(P4) the stats, loss, jitter and rate windows, read whole rather than grepped:
`sdk/rocketnet_stats_window.h`, `sdk/rocketnet_packet_loss_tracker.h`,
`sdk/rocketnet_jitter_tracker.h`, `sdk/rocketnet_out_of_order_tracker.h`, `sdk/rocketnet_send_budget.h`,
and the client's inline stats window. The loss and out-of-order trackers count sequences, not time.
Every hit is judged below: as a finding, as a row in the `Judged not a defect` table, or under
`Already filed`. No code was changed. Findings are ordered most severe first. Each one gives its
severity (`bug` / `risk` / `quality`), its `file:line` at 9775266a, a one-sentence defect, a
one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. risk — `sdk/rocketnet_client_transport.cpp:55`

- Defect: When the caller's time steps backwards by D, the update clamp returns the last good time (line 63) and keeps returning it until the caller's clock passes that value again, so the transport and message-object clocks stand still for the whole step instead of resuming from where they were.
- Failure or cost: For D seconds every time-driven rule on that end stops: the keep-alive and send-rate gates (`sdk/rocketnet_server_protocol.cpp:1803`, `sdk/rocketnet_client_protocol.cpp:346`) send nothing, reliable resends wait (`sdk/rocketnet_window_ack.cpp:820`), and the dead-connection timeout never fires (`sdk/rocketnet_server_protocol.cpp:1594`). A server whose caller corrects its clock back by one hour therefore holds every vanished client's slot for an hour plus the timeout, and does not resend a reliable message lost in that hour until the hour has passed. Live connections survive, because the other end's keep-alives still draw standalone acks, which no clock gates.
- Smallest fix: Keep the raw caller time beside the clamped time and advance the clamped time by `max( 0, time - previous_raw )` on each update. A backwards step then costs zero elapsed time rather than D, still counts as misuse, and leaves every comparison downstream monotonic.

### 2. risk — `sdk/rocketnet_server_protocol.cpp:44`

- Defect: The failed-signature-verify window rolls over only when `current_timestamp >= window_start + ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS`, and `current_timestamp` is the unix wall clock (`sdk/rocketnet_server_transport.cpp:2077`) whenever no `game_clock_epoch` is configured. If that clock steps back by H, the window stays open for H plus 10 seconds instead of 10.
- Failure or cost: If the budget is spent when the host clock is corrected backwards (an NTP step, a VM resume), every source address that failed a verify is refused at `sdk/rocketnet_server_protocol.cpp:538` for the length of the correction. That includes every honest client behind a NAT address an attacker shares, and it breaks the comment's promise of "for the rest of the window and no longer" (lines 36–39).
- Smallest fix: Also roll the window over when `current_timestamp < budget->window_start`, so a backwards step starts a fresh window.

### 3. risk — `sdk/rocketnet_jitter_tracker.h:63`

- Defect: The jitter sample is computed as `time - last_packet_time` on the receive-stamp clock with no check for a negative difference, and `fabs` at line 64 then treats a backwards stamp as a large positive deviation.
- Failure or cost: A stamp clock that steps back by D adds a sample of about D to the 1% running average, so the published jitter jumps by about D/100 (10 ms for a one-second step) and takes hundreds of packets to decay. The platform clock behind the default stamp is monotonic, so this is reachable today only through the testing `time_callback` (`sdk/rocketnet_testing.h:25`, installed at `sdk/rocketnet_client_transport.cpp:1699`). Nothing exercises it yet.
- Smallest fix: When `delta < 0.0`, re-seed `last_packet_time` and skip the sample, the way the ack RTT sample discards a negative value (`sdk/rocketnet_ack.cpp:1173`).

## Counts

- bug: 0
- risk: 3 (findings 1–3)
- quality: 0
- total: 3

## Judged not a defect

Every remaining hit of P1, P1b and P2, one row each. The lines named by a finding or under `Already filed`
are not repeated here.

| file:line | why it is safe |
|---|---|
| `sdk/rocketnet_ack.cpp:611` | sorted insert that walks back past later stamps, so any stamp order is placed correctly |
| `sdk/rocketnet_ack.cpp:760` | span used only when > 0.0 (line 761); low and high are the ends of a sorted list |
| `sdk/rocketnet_ack.cpp:764` | span used only when > 0.0 (line 765) |
| `sdk/rocketnet_ack.cpp:771` | span used only when > 0.0 (line 772); low and high are the ends of the sorted receive list |
| `sdk/rocketnet_ack.cpp:1172` | a negative sample is discarded (line 1173) |
| `sdk/rocketnet_ack.cpp:1392` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1394` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1409` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1411` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1424` | rate published only when finish > start |
| `sdk/rocketnet_ack.cpp:1427` | rate published only when finish > start |
| `sdk/rocketnet_ack.cpp:1463` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1465` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_ack.cpp:1470` | rate published only when finish > start |
| `sdk/rocketnet_channel.cpp:770` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_channel.cpp:1268` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_channel.cpp:1382` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_client_protocol.cpp:123` | a sentinel test against a negative or zero marker, not two time values |
| `sdk/rocketnet_client_protocol.cpp:126` | a negative sample is discarded (line 130) |
| `sdk/rocketnet_client_protocol.cpp:202` | token validation: refuses create > expire before anything subtracts them |
| `sdk/rocketnet_client_protocol.cpp:211` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_client_protocol.cpp:239` | seeds the send gate one interval in the past; an assignment, not a comparison |
| `sdk/rocketnet_client_protocol.cpp:240` | both operands are signed token fields ordered by the check at line 202, so the unsigned difference cannot wrap |
| `sdk/rocketnet_client_protocol.cpp:259` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_client_protocol.cpp:267` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_client_protocol.cpp:346` | the client send gate named in finding 1; not a separate defect |
| `sdk/rocketnet_client_protocol.cpp:364` | a sentinel test against a negative or zero marker, not two time values |
| `sdk/rocketnet_connect_token.cpp:241` | argument validation at token creation (refuses expire <= create) |
| `sdk/rocketnet_client_transport.cpp:665` | the smaller of two durations |
| `sdk/rocketnet_client_transport.cpp:733` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_client_transport.cpp:2064` | the jitter feed named in finding 3; not a separate defect |
| `sdk/rocketnet_client_transport.cpp:2316` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_client_transport.cpp:3141` | diagnostic age on the update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_endpoint.cpp:713` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_endpoint.cpp:752` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_endpoint.cpp:916` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_endpoint.cpp:1288` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_endpoint.cpp:1290` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_endpoint.cpp:1296` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_endpoint.cpp:1304` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_endpoint.cpp:1315` | a minimum or maximum over stored times; order-independent |
| `sdk/rocketnet_message_server.cpp:162` | diagnostic age on the message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_message_server.cpp:230` | a finiteness check on one time value |
| `sdk/rocketnet_message_server.cpp:632` | the origin is the first accepted time and the object clock never decreases, so the difference is never negative |
| `sdk/rocketnet_message_server.cpp:838` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_message_server.cpp:877` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_network_simulator.cpp:24` | comment text, no code |
| `sdk/rocketnet_network_simulator.cpp:61` | comment text, no code |
| `sdk/rocketnet_network_simulator.cpp:536` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:678` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:720` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:738` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:745` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:747` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:833` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_network_simulator.cpp:930` | simulator delivery time against its owning transport's clamped clock (DBL_MAX at stop); a backwards step only holds packets |
| `sdk/rocketnet_platform.cpp:60` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:67` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:91` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:97` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:104` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:727` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:1785` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:1787` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:1790` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:1795` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:2593` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:3247` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:3271` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:3302` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:3755` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:3757` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:3762` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:3915` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:3916` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_platform.cpp:4355` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:4357` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:4381` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_server_protocol.cpp:368` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_server_protocol.cpp:378` | ordered before subtracting (line 378), so the unsigned difference at line 379 cannot wrap |
| `sdk/rocketnet_server_protocol.cpp:379` | ordered before subtracting (line 378), so the unsigned difference cannot wrap |
| `sdk/rocketnet_server_protocol.cpp:467` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:480` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_server_protocol.cpp:817` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_server_protocol.cpp:1528` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:1532` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:1576` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:1582` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:1594` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_protocol.cpp:1803` | the server keep-alive gate named in finding 1; not a separate defect |
| `sdk/rocketnet_server_uring_linux.cpp:944` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_window_ack.cpp:302` | index arithmetic; stores the message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_window_ack.cpp:398` | a sentinel test against a negative or zero marker, not two time values; the object clock never decreases |
| `sdk/rocketnet_window_ack.cpp:401` | last_send is an earlier reading of the message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_window_ack.cpp:486` | a sentinel test against a negative or zero marker, not two time values |
| `sdk/rocketnet_window_ack.cpp:580` | a negative sample is clamped to 0.0 on the same line |
| `sdk/rocketnet_window_ack.cpp:698` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_window_ack.cpp:781` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_window_ack.cpp:820` | message-object clock, clamped at `sdk/rocketnet_message_server.cpp:593`, so it never decreases |
| `sdk/rocketnet_window_ack.cpp:847` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_server_socket_linux.cpp:1478` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_server_transport.cpp:1998` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_transport.cpp:3519` | diagnostic age on the update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_server_transport.cpp:3520` | diagnostic age on the update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_token_tables.cpp:179` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_token_tables.cpp:192` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_token_tables.cpp:240` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_token_tables.cpp:241` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock; binding records evict the oldest, so a full table never refuses |
| `sdk/rocketnet_token_tables.cpp:352` | absolute expiry against the unix clock; a backwards step of that clock lengthens acceptance by the step, bounded by the token's own expiry and, for cookies, by key rotation on the clamped clock |
| `sdk/rocketnet_token_tables.cpp:396` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_token_tables.cpp:429` | update clock after the clamp at `sdk/rocketnet_client_transport.cpp:55`, so it never decreases; a backwards step holds it still (finding 1) |
| `sdk/rocketnet_allocator.h:76` | a declaration; the comparison is in its trailing comment |
| `sdk/rocketnet_allocator.h:94` | a sentinel test against a negative or zero marker, not two time values |
| `sdk/rocketnet_platform.h:964` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_jitter_tracker.h:61` | sequence-adjacency and sentinel test; the time difference is finding 3 |
| `sdk/rocketnet_jitter_tracker.h:64` | the jitter sample named in finding 3; not a separate defect |
| `sdk/rocketnet_out_of_order_tracker.h:53` | not a time value: a sequence, count, index or size matched the name pattern |
| `sdk/rocketnet_send_budget.h:106` | a declaration; the comparison is in its trailing comment |
| `sdk/rocketnet_send_budget.h:189` | a finiteness check on one time value |
| `sdk/rocketnet_send_budget.h:218` | a time that does not advance refills nothing and returns before the subtraction |
| `sdk/rocketnet_send_budget.h:220` | line 218 returned for now <= last_time, so the elapsed time is positive |
| `sdk/rocketnet_frame_sync.cpp:52` | ordered before subtracting; frame sync reads its own CLOCK_MONOTONIC |
| `sdk/rocketnet_frame_sync.cpp:54` | ordered at line 52 before subtracting |
| `sdk/rocketnet_frame_sync.cpp:148` | ordered before subtracting; QueryPerformanceCounter clock |
| `sdk/rocketnet_frame_sync.cpp:151` | ordered at line 148 before subtracting |
| `sdk/rocketnet_frame_sync.cpp:163` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_frame_sync.cpp:307` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:309` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:320` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:331` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:332` | ordered before subtracting on the same line |
| `sdk/rocketnet_frame_sync.cpp:335` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:400` | one duration converted to a platform unit or checked for sign, not two time values |
| `sdk/rocketnet_frame_sync.cpp:479` | ordered before subtracting on the same line |
| `sdk/rocketnet_frame_sync.cpp:482` | ordered (woke > sleep_until) before the subtraction at line 487 |
| `sdk/rocketnet_frame_sync.cpp:487` | ordered at line 482 before subtracting |
| `sdk/rocketnet_frame_sync.cpp:489` | both sides read the platform monotonic clock (CLOCK_MONOTONIC(_RAW), mach_absolute_time or QueryPerformanceCounter) |
| `sdk/rocketnet_platform.cpp:3269` | a sentinel test against a negative or zero marker, not two time values; platform monotonic clock |
| `functional/channel_config.cpp:103` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:130` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:138` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:146` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:177` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:196` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:213` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_config.cpp:256` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_ordered.cpp:192` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_ordered.cpp:229` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_ordered.cpp:237` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_ordered.cpp:245` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_receipts.cpp:123` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_receipts.cpp:216` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_receipts_reliable.cpp:147` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_reliable.cpp:71` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_reliable_sequenced.cpp:132` | not a time value: a sequence, count, index or size matched the name pattern (delivery positions) |
| `functional/channel_reliable_sequenced.cpp:151` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_smoke.cpp:174` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_smoke.cpp:230` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:52` | comment text, no code |
| `functional/channel_resend_timing.cpp:220` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:236` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:242` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:401` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:464` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:488` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_resend_timing.cpp:495` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/cpp_transports.cpp:65` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/cpp_transports.cpp:85` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/cpp_transports.cpp:139` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/cpp_transports.cpp:163` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/platform_primitives.cpp:67` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:210` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:653` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:773` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:852` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:986` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:1049` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:1154` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:1167` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/live_matrix.cpp:1176` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:384` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:385` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:405` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:406` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:461` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:462` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:463` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:464` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:471` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:491` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:502` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:598` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:662` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:680` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:750` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:1002` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:1017` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:1098` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_hard_killed_client.cpp:1191` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_lifecycle_stress.cpp:276` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_lifecycle_stress.cpp:314` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_lifecycle_stress.cpp:426` | a shell command string, no code comparison |
| `functional/posix_lifecycle_stress.cpp:735` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_lifecycle_stress.cpp:767` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_lifecycle_stress.cpp:942` | not a time value: a sequence, count, index or size matched the name pattern |
| `functional/posix_lifecycle_stress.cpp:943` | not a time value: a sequence, count, index or size matched the name pattern |
| `functional/posix_lifecycle_stress.cpp:945` | not a time value: a sequence, count, index or size matched the name pattern |
| `functional/posix_lifecycle_stress.cpp:957` | not a time value: a sequence, count, index or size matched the name pattern |
| `functional/posix_lifecycle_stress.cpp:966` | not a time value: a sequence, count, index or size matched the name pattern |
| `functional/posix_lifecycle_stress.cpp:1145` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:367` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:381` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:637` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:823` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:886` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:911` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:980` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1029` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1106` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1297` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1355` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1394` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1438` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1473` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1624` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1658` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:1725` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:2018` | a shell command string, no code comparison |
| `functional/posix_socket_errors.cpp:2097` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:2244` | not a time value: a sequence, count, index or size matched the name pattern (a counter read held in a variable named now) |
| `functional/posix_socket_errors.cpp:2251` | not a time value: a sequence, count, index or size matched the name pattern (a counter read held in a variable named now) |
| `functional/posix_socket_errors.cpp:2253` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:2258` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/posix_socket_errors.cpp:2327` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/server_receive_backlog.cpp:183` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/server_receive_backlog.cpp:184` | a test clock that starts at 1000.0 and only increases, against a platform-clock duration |
| `functional/server_receive_backlog.cpp:194` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/server_receive_backlog.cpp:269` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/server_receive_backlog.cpp:273` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:180` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:194` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:238` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:255` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:323` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:402` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:438` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:512` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:570` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:624` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:663` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:722` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:798` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:837` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:894` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:934` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:990` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1077` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1156` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1181` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1225` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1248` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1255` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1338` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1358` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1412` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1433` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1480` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1497` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1599` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1667` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:1783` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1887` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1922` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1928` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:1970` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:1972` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:2188` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2237` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2332` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2372` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2483` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2531` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2591` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2669` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2722` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2758` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2821` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2838` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2911` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2941` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:2986` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3034` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3068` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3092` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3125` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3208` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3225` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3307` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3324` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3407` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3422` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3438` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3534` | a fixed test time; each protocol object sees its own nondecreasing sequence |
| `functional/rocketnet_functional_tests.cpp:3542` | a fixed test time; each protocol object sees its own nondecreasing sequence |
| `functional/rocketnet_functional_tests.cpp:3546` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3570` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3647` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3649` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3681` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3851` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3867` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3965` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:3981` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4033` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4088` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4174` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4289` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4344` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4378` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4395` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4420` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4449` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4618` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4656` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:4684` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5325` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5414` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5429` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5474` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5527` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5576` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5643` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5759` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5787` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:5965` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6005` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6034` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6054` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6147` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6159` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6221` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6292` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6338` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6482` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6603` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6661` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6690` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6741` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6773` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:6938` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7091` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7094` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:7107` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:7120` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7155` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:7208` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7211` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:7229` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7232` | test game clock built as a start plus a monotonic platform delta, or a counter that only increases |
| `functional/rocketnet_functional_tests.cpp:7378` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7483` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7686` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7709` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:7960` | a shell command string, no code comparison |
| `functional/rocketnet_functional_tests.cpp:8016` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:8035` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:8139` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:8179` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:8235` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:8373` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9167` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9169` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9187` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9189` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9251` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9253` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9367` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9425` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/rocketnet_functional_tests.cpp:9463` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:261` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:431` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:464` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:465` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:494` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:507` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:566` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:570` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:597` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:601` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:622` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:745` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:773` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_connreset.cpp:779` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:194` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:195` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:271` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:278` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:345` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:390` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:489` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:522` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:540` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `functional/windows_socket_errors.cpp:552` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/counters_expect.h:105` | not a time value: a sequence, count, index or size matched the name pattern (counter values; a decrease is handled explicitly) |
| `tests/counters_expect.h:106` | not a time value: a sequence, count, index or size matched the name pattern (counter values; a decrease is handled explicitly) |
| `functional/channel_harness.h:259` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_harness.h:524` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `functional/channel_harness.h:567` | channel harness clock, which only advances (`h->now += dt`, `functional/channel_harness.h:1014`, positive dt at every call) |
| `tests/dst_harness.h:331` | DST world clock, `1000.0 + k * 0.01` for increasing step k (`tests/dst_harness.h:1096`) |
| `tests/dst_harness.h:369` | DST world clock, `1000.0 + k * 0.01` for increasing step k (`tests/dst_harness.h:1096`) |
| `tests/dst_harness.h:1097` | DST world clock, `1000.0 + k * 0.01` for increasing step k (`tests/dst_harness.h:1096`) |
| `tests/dst_checks.h:245` | not a time value: a sequence, count, index or size matched the name pattern (message sequences) |
| `tests/dst_checks.h:907` | not a time value: a sequence, count, index or size matched the name pattern (message sequences) |
| `tests/dst_checks.h:924` | not a time value: a sequence, count, index or size matched the name pattern (message sequences) |
| `tests/suite_runner.h:845` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/suite_runner.h:1068` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/native_shutdown.h:19` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/native_shutdown.h:34` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/native_shutdown.h:72` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/native_shutdown.h:98` | monotonic platform clock (rocketnet_platform_time, rocketnet_time or steady_clock) bounding a test wait or measuring a duration |
| `tests/serialize.h:1563` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:1564` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:4112` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:4158` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:4176` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:4187` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |
| `tests/serialize.h:7842` | not a time value: a sequence, count, index or size matched the name pattern (bit and byte counts) |

## Already filed

- `sdk/rocketnet_stats_window.h:13` (and line 14), plus its client copy `sdk/rocketnet_client_transport.cpp:2281` (and line 2282): a backwards step advances and zeroes a stats bucket. Filed as finding 1 of `docs/audits/2026-10-24-code-quality-1.md`, and the client copy's duplication as finding 9 of the same document. This sweep adds one fact to that filing: both callers (`sdk/rocketnet_server_transport.cpp:2069`, `sdk/rocketnet_client_transport.cpp:2396`) pass the clamped update time. The public update calls therefore never reach the backwards disjunct; only a future internal caller with an unclamped clock could.
