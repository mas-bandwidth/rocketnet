# The RocketNet standard: style guide

This file is for the people who write the standard. It fixes, once, every
choice a section writer would otherwise have to make, so that twenty sections
written by twenty hands read as one document. It is not part of the standard
and binds no implementation. `tools/conformance/check_rn_standard.py` enforces
every rule below that a program can check; the rest are for the writer and
the reviewer.

## 1. The three documents

| Document | Tag | Status | Subject |
|---|---|---|---|
| `standard/TRANSPORT.md` | RN-1 | normative | the connection layer: keys, connect token, handshake, sealed packets, session, replay, acknowledgement |
| `standard/MESSAGES.md` | RN-2 | normative | the message layer carried inside RN-1 payloads: payload forms, records, reliability, blocks, channels, CONFIG, receipts |
| `standard/RATIONALE.md` | RN-R | informative | why each normative section says what it says, one heading per normative section id |

RN-1 and RN-2 state requirements and nothing else. Reasons, history,
alternatives considered and comparisons with other protocols go to RN-R under
the same section id.

## 2. Vocabulary

One word for one thing, everywhere:

| Word | Means | Never |
|---|---|---|
| transport | one selectable packet carrier below the connection layer, chosen by name: `xdp`, `gso`, `io_uring`, `msgx`, `portable`, `iocp`, `rio`, `loopback` | driver |
| `transport` (config field) | the ordered candidate list of transports for one endpoint; environment `ROCKETNET_SERVER_TRANSPORT` and `ROCKETNET_CLIENT_TRANSPORT` | driver, transport list |
| client, server | the two endpoints of a connection, by wire role; each is configured by one config struct | host |
| connection layer | what RN-1 specifies | transport tier |
| message layer | what RN-2 specifies | channels tier |
| maximum packet size | the largest UDP payload a sender emits (RN-1, bytes) | send budget |
| send budget | the per-connection rate limit of the message layer (RN-2, kilobits per second) | bandwidth cap |
| send rate | the RN-1 handshake retransmission and keep-alive rate (hertz) | tick rate |
| sequence | the RN-1 per-packet counter | packet id, frame number |
| number | the RN-2 reliable message counter | sequence |
| vector | a known-answer byte string a conforming implementation reproduces | test case |

A transport is selected by its name in the config string, and it never
changes a byte on the wire; neither RN-1 nor RN-2 varies by transport.

## 3. Section ids, headings and the outline

Every top-level section has an id: `t1` to `t20` except `t16`, `ta` to `tc`
in RN-1, `m1` to `m13` and `ma` in RN-2, and the same ids again in RN-R, plus
`rh`. A removed section's id stays unassigned and is never reused.
The heading is written

    <a id="t6"></a>
    ## t6. Connect token

The anchor line is the link target; the heading text after `t6. ` is exactly
the outline's heading. Subsections are `### t6.1. Private connect token data`
and so on, with an anchor `t6.1`; a subsection id is never reused or
renumbered once published.

The outline below is the one source of the section list. The checker reads
it from this file and fails when a document's headings differ from it in
text, id or order. Adding, removing or renaming a section is an edit here
first. The checker also rejects a subsection with a missing or mismatched
explicit anchor, or a repeated subsection id.

<!-- outline:begin -->
| Id | Heading | Document |
|---|---|---|
| t1 | Introduction, scope, terminology and conventions | RN-1 |
| t2 | Architecture and key inventory | RN-1 |
| t3 | Constants and registries | RN-1 |
| t4 | Wire encoding primitives | RN-1 |
| t5 | Prefilter header | RN-1 |
| t6 | Connect token | RN-1 |
| t7 | Key schedule | RN-1 |
| t8 | Packet formats | RN-1 |
| t9 | Packet protection | RN-1 |
| t10 | Receive processing | RN-1 |
| t11 | Handshake: client | RN-1 |
| t12 | Handshake: server | RN-1 |
| t13 | Session: keep-alive, timeout, disconnect, slot reset | RN-1 |
| t14 | Replay protection | RN-1 |
| t15 | Acknowledgement | RN-1 |
| t17 | Timing and defaults | RN-1 |
| t18 | Error handling and counters | RN-1 |
| t19 | Security considerations | RN-1 |
| t20 | Conformance | RN-1 |
| ta | Appendix A: test vectors | RN-1 |
| tb | Appendix B: changes from netcode 1.02 | RN-1 |
| tc | Appendix C (informative): implementation notes | RN-1 |
| m1 | Scope, terminology and relation to RN-1 | RN-2 |
| m2 | Payload forms and sender size rules | RN-2 |
| m3 | Record format | RN-2 |
| m4 | Receiver checks | RN-2 |
| m5 | Message numbering and the reliable window | RN-2 |
| m6 | Delivery: unreliable codes 0 and 1 | RN-2 |
| m7 | Delivery: reliable codes 2, 3 and 4 | RN-2 |
| m8 | Blocks | RN-2 |
| m9 | Acknowledgement mapping, resend and send budget | RN-2 |
| m10 | Channels and priorities | RN-2 |
| m11 | CONFIG control record | RN-2 |
| m12 | Receipts | RN-2 |
| m13 | Security considerations | RN-2 |
| ma | Appendix A: test vectors | RN-2 |
<!-- outline:end -->

RN-R carries one section per id above, in the same order and with the same
heading, followed by `rh Revision history`.

## 4. Pending and written

A section not yet written carries, directly under its heading, the line

    Status: pending

followed by a `Scope:` paragraph naming what the section will contain and
any requirement already decided for it. A section is written when that line
is gone. The checker lists pending sections and does not fail on them; a
pending section's scope paragraph is not normative.

## 5. Keywords

RN-1 and RN-2 use the BCP 14 key words (RFC 2119, RFC 8174): MUST, MUST NOT,
REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT, RECOMMENDED, NOT RECOMMENDED,
MAY and OPTIONAL. The rules:

- A key word is in capitals exactly where it binds, and in no other place.
- The lower-case words "must", "shall", "should" and "required" do not appear
  in RN-1 or RN-2 at all: write the requirement in capitals, or rephrase as a
  plain statement of fact. The checker refuses them.
- The BCP 14 boilerplate sentence appears once, in RN-1 t1. RN-2 m1 adopts it
  by reference.
- RN-R and RN-1 tc are informative and contain no capitalised key word. The
  checker refuses one there.
- Each requirement names its actor: "A sender MUST", "The server MUST NOT".
  Never "it is required that".

## 6. Conventions are stated once

RN-1 t1 states the encoding conventions: little-endian byte order everywhere,
including addresses, ports, nonces and associated data; structures packed with
no padding unless a layout names a padding field; every size in bytes; time an
input in seconds, never read by the protocol from a clock it owns. No other
section restates them, and no section departs from them without saying so in
its rule.

## 7. Normative text carries no history

RN-1 and RN-2 never contain: history, past rulings, corrections, "previously",
comparisons with other protocols (Appendix B of RN-1 is the one place a
difference from netcode 1.02 is listed, as a list of facts), source file names
or `file:line`, change identifiers, TLA+ module names, test function names, or
names of people, machines or customers. A requirement carries at most one
sentence of reason; the rest goes to RN-R under the same section id. The
checker refuses file paths, `TLA+` and `test_` identifiers in RN-1 and RN-2.

## 8. The section shape

Every written section of RN-1 and RN-2 is, in this order, each part present
only when the section has one:

1. **Rule.** The requirements, numbered when there are more than two, each a
   complete sentence with a BCP 14 key word.
2. **Fields.** An RFC-style ASCII box diagram, 32 bits to a row, offsets in
   bytes across the top, then a field table:

       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |  packet type  |  filter epoch |      filter value ...         |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

   | Name | Offset | Size | Type | Meaning | Valid values |
   |---|---|---|---|---|---|

   Types are `u8`, `u16`, `u24`, `u32`, `u64` (little-endian unsigned),
   `i32` (little-endian two's complement), `bytes[n]` and `zero[n]` (bytes
   that a sender sets to zero and a receiver checks). Offsets and sizes are
   decimal bytes.

   RN-1 packet tables use additive offsets: a byte count, optionally followed
   by `plus the ack block size` or `and the payload size`. The checker verifies that each field begins at the end of the
   previous field, including those variable lengths. It does not establish
   that every field's semantic range is correct.
3. **State table**, where the section has states:

   | State | Event | Guard | Action | Next state |
   |---|---|---|---|---|

   One row per (state, event, guard). An event a state does not list is
   ignored in that state, and the section says so once.
4. **Vectors**, in the format of section 9.

A rule that refers to a constant names its t3 row (RN-1) or its m-section row
(RN-2) and does not repeat the number.

## 9. Vectors

A vector is a fenced block whose info string is `vector <id>`. The id is
`<section id>.<name>`, with a name of lower-case letters, digits and hyphens,
unique across the three documents. Inside the fence, input lines come first,
one per input, as `name: value`; then the bytes, lower-case hex, 16 bytes to a
row separated by single spaces, the last row 1 to 16 bytes:

    ```vector t3.token-identifier
    input: the 576 signed connect data bytes of the frozen connect token
    f5 20 da 9d 95 fd 07 22 56 b4 7d 75 5f 81 0f 61
    ```

A vector sits in the section that defines what it encodes. Each document's
Appendix A explains its vector identifiers. The tooling prints an index on
demand with `--print-index`; that generated index is not committed.

Every vector has a twin in a test file `test_unit_standard_<topic>.cpp` under
`tests/`:

    RN_STANDARD_VECTOR( "t3.token-identifier", "f520da9d95fd072256b47d755f810f61" )

The hex is the same bytes, with or without spaces, and may be split across
adjacent string literals. A twin counts only outside comments, in a file that
calls `rn_standard_vectors_walk`. The test regenerates the bytes from the inputs
through the implementation and compares them (`test_unit_standard.h`). The
checker fails a vector present in a document and in no test file, a twin in
a test file and in no document, and any byte that differs between the two.

The checker compares the document and fixture; it does not run the C++ vector
generator. Execute the corresponding tests before reporting runtime agreement.
A boundary vector distinguishes format capacity from sender policy whenever
those limits differ. A verification vector states whether its inputs represent
an admissible protocol object or only exercise one cryptographic operation.

## 10. Constants, configuration fields and pins

Every number an implementation has to match is a row in exactly one table:

- RN-1 t3 for the constants of the connection layer. A t3 row's kind is
  `version`, `packet type`, `registry`, `key derivation label`, `size` or
  `protocol value` (a rate, count or size the protocol fixes; not
  configurable).
- RN-1 t2.3 for every configuration field of the connection layer, with its
  default, type, environment variable and valid range.
- For the message layer, the table of the m-section that owns the value or
  the configuration field, in the same shape as t2.3 for a field.

Such a table has a first column `Name` and a column `Value`. The value cell
holds one value only, in backticks: a decimal or `0x` hexadecimal integer, a
decimal real, or a quoted string; or `none` for a field with no numeric
default. Units go in their own column. Row names are unique within a
document.

A default is stated once, in its field's row, with its pin. RN-1 t17 states
the timing rules and refers to the rows; it does not repeat their numbers.

Every row with a value is pinned in a pin file `standard/pins/<name>.tsv`:

    # comment lines start with '#'
    #include "rocketnet_wire.h"
    <document name>	<sdk expression>	<value>

A pin row is three tab-separated fields: the row's `Name` cell, a C or C++
constant expression over the real sdk headers (or libsodium's), and the value
as the document writes it. A configuration default is pinned through the
checker's names for the sdk's configuration tables:
`rn_pin_server_default_<field>`, `rn_pin_client_default_<field>` and
`rn_pin_message_default_<field>`. An `#include` comment line adds a header to
the translation unit the checker generates. The checker compiles one
translation unit that prints every pin's expression, and fails when the
compiler's value, the pin file's value and the document's cell are not all
equal, when a pinned row is missing from its document, and when a document
row with a value has no pin.

A number with no sdk expression is a finding, never a pinned literal and
never a declaration: the writer reports it, names the sdk constant it needs,
and leaves the section pending until the sdk names the value. There is no
unpinned row; the checker fails every `#unpinned` line in a pin file.

Pin files are owned by sections. `transport.tsv` holds rows of RN-1 t1 to t3
and `messages.tsv` rows of RN-2 m1, the sections this skeleton wrote. A
section change adds rows only to its own pin file, `standard/pins/<section id>.tsv` (for example `t8.tsv`), never to a shared one, and every row of that
file lies in that section; the document is RN-1 for a `t` id and RN-2 for an
`m` id. The checker fails a row outside its file's sections.

## 11. Cross-references

Every link in the three documents is a reference to a section or
subsection, and its text is the target's id and heading, prefixed by the
document tag when it crosses documents:

    [t6 Connect token](#t6)
    [t2.3 Configuration surface](#t2.3)
    [RN-1 t14 Replay protection](TRANSPORT.md#t14)
    [RN-R t6 Connect token](RATIONALE.md#t6)

The checker fails a link to an id that does not exist, a link whose text is
not that id's heading, a link to anything but a section of the three
documents, and a bare section id in running text. External standards are
cited by name and number (`RFC 8439`) with no link.

## 12. Visibility of behaviour

A requirement that changes what the application asked for (an unreliable
message promoted to reliable, a send that waits, a message dropped for budget)
states that the change is visible to the application as a named outcome or
counter. Nothing the protocol does to a message is silent.
