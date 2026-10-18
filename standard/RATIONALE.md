# RN-R: RocketNet rationale

Status: informative

This document explains RN-1 (`TRANSPORT.md`) and RN-2 (`MESSAGES.md`). It
binds no implementation. Each heading carries the id and heading of the
normative section it explains.

<a id="t1"></a>
## t1. Introduction, scope, terminology and conventions

RN-1 and RN-2 state requirements only, and every reason lives here under the
same section id, so that an implementer reads the rules without the argument
and a reviewer finds the argument without hunting. The split into a
connection layer and a message layer follows the code's own seam: the
message layer is carried entirely inside connection layer payloads and
changes none of its bytes, so a connection layer implementation can conform
without implementing messages.

Little-endian order everywhere, including ports and IPv6 groups, is the
opposite of what socket interfaces hand an implementer, which is why the
convention names those cases. Time is an input so that every rule that
depends on elapsed time can be tested without a clock, and so that an
application that updates at any rate gets the same behaviour.

The lower-case words "must", "should" and "shall" are kept out of the
normative documents entirely. BCP 14 allows them with their ordinary English
meaning, but a reader who has to decide whether a lower-case "must" binds is
exactly the ambiguity a standard exists to remove.

<a id="t2"></a>
## t2. Architecture and key inventory

The key inventory is closed because the design's central property is a
negative one: there is no secret shared between the backend and the servers,
except the prefilter magic, which protects no session and no key. A server
holds only the backend's public key and its own keypair, so a compromised
server cannot mint tokens and exposes only its own sessions; that is what
allows servers on hardware the backend operator does not control. A closed
table makes an added key a visible change to the standard rather than an
implementation detail.

The configuration surface binds every field, not only the connection layer's,
because the audience of an override is the operator deploying an already
built binary, who has no code to edit. Refusing an out-of-range value instead
of clamping it keeps every running value one the operator chose; logging
every override keeps a forgotten variable in a service file from silently
diverging a fleet.

Transport selection is an ordered list so that one configuration line can be
deployed unchanged to machines with different capabilities, while a one-name
list stays a pin that refuses rather than falls back; a fallback nobody chose
can hide a provisioning error indefinitely.

<a id="t3"></a>
## t3. Constants and registries

One table holds every constant and default so that an implementer never
hunts for a number and no number is stated twice. A value stated in two
places is two values the moment one of them is edited. Each row is pinned to
the expression the reference implementation compiles, and the conformance
checker compares the document, the pin and the compiler, so the table cannot
drift from the code without a red check.

<a id="t4"></a>
## t4. Wire encoding primitives

Status: pending

<a id="t5"></a>
## t5. Prefilter header

Status: pending

<a id="t6"></a>
## t6. Connect token

Status: pending

<a id="t7"></a>
## t7. Key schedule

Status: pending

<a id="t8"></a>
## t8. Packet formats

Status: pending

<a id="t9"></a>
## t9. Packet protection

Status: pending

<a id="t10"></a>
## t10. Receive processing

Status: pending

<a id="t11"></a>
## t11. Handshake: client

Status: pending

<a id="t12"></a>
## t12. Handshake: server

Status: pending

<a id="t13"></a>
## t13. Session: keep-alive, timeout, disconnect, slot reset

Status: pending

<a id="t14"></a>
## t14. Replay protection

Status: pending

<a id="t15"></a>
## t15. Acknowledgement

Status: pending

<a id="t16"></a>
## t16. Fragmentation and reassembly

Status: pending

<a id="t17"></a>
## t17. Timing and defaults

Status: pending

<a id="t18"></a>
## t18. Error handling and counters

Status: pending

<a id="t19"></a>
## t19. Security considerations

Status: pending

<a id="t20"></a>
## t20. Conformance

Status: pending

<a id="ta"></a>
## ta. Appendix A: test vectors

Status: pending

<a id="tb"></a>
## tb. Appendix B: changes from netcode 1.02

Status: pending

<a id="tc"></a>
## tc. Appendix C (informative): implementation notes

Status: pending

<a id="m1"></a>
## m1. Scope, terminology and relation to RN-1

The preconditions in
[RN-2 m1 Scope, terminology and relation to RN-1](MESSAGES.md#m1) are the facts about the connection layer that the
message layer's correctness rests on. They are stated as requirements on the
connection layer, in RN-2, because RN-2 is the party that relies on them, and
an RN-1 implementation that carries only opaque payloads need not keep the
stronger ones. The replay window bound is the reason 24-bit message numbers
are enough: a stale resend cannot arrive from further back than the window
admits, and within that distance the signed half-range reading of a number
never aliases. Neither depth bound has a named constant in the reference
implementation yet, so both rows are declared unpinned until one exists.

<a id="m2"></a>
## m2. Payload forms and sender size rules

Status: pending

<a id="m3"></a>
## m3. Record format

Status: pending

<a id="m4"></a>
## m4. Receiver checks

Status: pending

<a id="m5"></a>
## m5. Message numbering and the reliable window

Status: pending

<a id="m6"></a>
## m6. Delivery: unreliable codes 0 and 1

Status: pending

<a id="m7"></a>
## m7. Delivery: reliable codes 2, 3 and 4

Status: pending

<a id="m8"></a>
## m8. Blocks

Status: pending

<a id="m9"></a>
## m9. Acknowledgement mapping, resend and send budget

Status: pending

<a id="m10"></a>
## m10. Channels and priorities

Status: pending

<a id="m11"></a>
## m11. CONFIG control record

Status: pending

<a id="m12"></a>
## m12. Receipts

Status: pending

<a id="m13"></a>
## m13. Security considerations

Status: pending

<a id="ma"></a>
## ma. Appendix A: test vectors

Status: pending

<a id="rh"></a>
## rh. Revision history

| Revision | Change |
|---|---|
| 1.0 draft 1 | The outline, the style guide, the pin checker, and the first three sections of RN-1 and the first section of RN-2 written. |
