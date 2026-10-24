# Reliable receive pressure regression

The ACK admission regression compares RocketNet SDK `ff1cccaf5e38416c4e0c25dfa2a62d04a62b104f`
with the receive-admission change. The deterministic functional harness runs the
real message objects and encrypted client/server protocol engines.

The test temporarily sets the SDK receive delivery capacity to zero. This is
explicit fault injection: normal delivery storage reserves `W + Du` entries,
so this does not establish a naturally reachable overflow with a conforming sender.
It verifies the failure boundary even when storage cannot accept a message.

Before the change, both directions fail: the connection disconnects with
message memory exhaustion and the rejected message advances the receive floor.
The server-to-client run also retires the sender's message before delivery.

After the change, the same two tests keep the connection alive, leave the
rejected receive number uncommitted, and retain the message at the sender.
Restoring capacity allows a fresh-sequence retransmission to deliver exactly
once and retire the sender's retained state. No allocation occurs after create.

Additional functional cases cover all reliable delivery modes and receipt
variants, 8192-byte messages fragmented at the message layer, a partially
accepted packet whose prefix must not be delivered twice, and an ordered gap
whose arrival would release two previously held messages.

Run these registered cases with `bin/functional --run <name>`:

- `test_reliable_receive_full_recovers_client_to_server`
- `test_reliable_receive_full_recovers_server_to_client`
- `test_reliable_receive_full_recovers_all_modes`
- `test_reliable_receive_full_partial_packet_recovers`
- `test_reliable_receive_full_ordered_gap_recovers`

Set `ROCKETNET_TEST_VERBOSE=1` for detailed successful receipts.

Security replay history still records every successful decryption. ACK-eligible
history is separate and commits type-5 payloads only after message processing
succeeds. A refused packet cannot be reopened; reliable retries use fresh packet
sequences. Malformed messages remain protocol errors, and the configured reliable
message timeout still applies if receive capacity never returns.

Payload ACK eligibility now follows message processing, after the transport's
update. With no outgoing payload to carry the ACK, standalone acknowledgement
may take the next update. The deterministic loopback RTT test consequently
allows four simulated ticks instead of three, while retaining exact delivery,
ordering, and zero-resend checks on its lossless link.
