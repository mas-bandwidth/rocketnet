# Reliable receive pressure

When a receiver has no room to deliver a reliable message, the connection
stays up. The rejected receive number is left uncommitted, the sender keeps the
message, and once capacity returns a retransmission on a fresh packet sequence
delivers it exactly once and retires the sender's state. No allocation occurs
after create.

The functional cases reach that boundary by setting the SDK's receive delivery
capacity to zero. This is fault injection: normal delivery storage reserves
`W + Du` entries, so a conforming sender does not reach it naturally. The
cases run the real message objects and encrypted client/server protocol
engines in both directions.

The functional cases also cover all reliable delivery modes and receipt
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

Payload ACK eligibility follows message processing, after the transport's
update. With no outgoing payload to carry the ACK, standalone acknowledgement
may take the next update, so the deterministic loopback RTT test
(`tests/test_unit_loopback_rtt.cpp`) allows up to four simulated ticks, with
exact delivery, ordering and zero-resend checks on its lossless link.
