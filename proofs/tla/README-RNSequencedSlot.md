# Sequenced block and sender-window composition

`RNSequencedSlot.tla` checks the receiver's `NoSlotCollision` with a sender
that numbers records only under `next + count <= U' + W`. The two-fragment
code-4 block O takes numbers 1 and 2 atomically. Ordered Y takes 0; ordered
Z takes 3, advancing the sender's ordered index from 1 to 2; newer code-4 N
takes 4. Numbers 5 through `MaxNum` are ordinary reliable records. O uses
ordering 1 and sequencing 0; N uses ordering 2 and sequencing 0. First writes
on the channel FIFO retain this order, while packets may arrive in any order.

The receiver tracks its receipt window, live descriptors, future and in-turn
code-4 slots, ordered epoch, delivery views, and ACK eligibility. `ReceiveY`
promotes an incomplete future O into the in-turn slot. `ReceiveN` supersedes
only a future O and releases that span. `ReceiveOLast` and `ReceiveOFirst`
check whether the sibling fragment of the same accepted span has already
been released before creating a new anchor. A delivered view remains live
until `EndUpdate`; only then may its packet's number be ACKed. The model
records a slot collision when a genuinely accepted number reaches a live
descriptor at the same `n % W`; admission never assumes `NoSlotCollision`.

The W=4 and W=8 controls enumerate every bounded interleaving of numbering,
individual packet arrival, update end, and causally eligible ACK for this
fixed workload. The control configurations allow either fragment arrival
order. The reversed W=8 configuration restricts O's first fragment to arrive
before its last, reproducing the C regression's direction while still
enumerating all other schedules. It disables only the accepted-and-released
span check. TLC reaches O#1, N#4, Y#0, O#2; the old path anchors O#2 as an
in-turn orphan. After `EndUpdate`, ACKs release U', so number 10 is admitted
and collides with live descriptor 2. The control marks O#2 stale instead.

W=4 cannot exhibit that reversed trace with an honest sender: Y0, O1/2,
Z3, and N4 require five numbers before the first ACK. If Y0 is delivered and
released to permit N4, the drain promotes O's incomplete future hold to the
in-turn slot before N arrives; future N cannot supersede that slot. The
implementation requires a power-of-two window, so W=8 is the smallest
concrete size for this witness. The W=4 control reaches aliases 5 and 6;
the W=8 control and reversed case reach aliases 9 and 10.

This is a bounded one-channel, one-O, one-N model. It abstracts packet IDs,
sent-map eviction, retries, multiple records per packet, ring-byte placement,
delivery capacity, and 24-bit wrap. The `received` set stands in for the
receiver's bitset plus prior accepted-number history. For this workload,
U' pins the block's first number while its fragments are unacked, preventing
reuse of the sibling descriptor before the late-fragment span check. These
checks establish this composition's bounded safety, not arbitrary workloads.

Source mapping: `rn_window_send_first_write_block` and
`rn_window_send_admit` in `sdk/rocketnet_window_send.cpp` implement atomic
block numbering and U'; `rn_channels_send_ordered` and
`rn_channels_send_sequenced` in `sdk/rocketnet_channel.cpp` stamp ordering;
`rn_window_receive_accept` in `sdk/rocketnet_window_receive.cpp` checks the
receive window; `rn_channels_dispatch_sequenced`, `rn_sequenced_block`, and
`rn_channels_drain_sequenced` in `sdk/rocketnet_channel_sequenced.cpp` implement
the two holds, supersede, promotion, and accepted-span guard; and
`rn_channels_end_update` in `sdk/rocketnet_channel.cpp` releases delivery
views before the next update's packet can be opened.
