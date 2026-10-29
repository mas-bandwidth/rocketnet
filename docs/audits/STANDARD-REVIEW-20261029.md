# RocketNet standard review, 2026-10-29

The standard has a strong foundation: separate normative and explanatory
documents, named actors, stable section IDs, field and state tables, compiled
constant pins, and executable vector fixtures. The review found substantive
ambiguities despite green conformance checks. The corrections below preserve
the current wire format and SDK behavior; no SDK sources change in this work.

This is a targeted consistency and usability review, with executable checks
of the affected behavior. It is not an exhaustive cryptographic audit or a
proof that every execution satisfies every requirement.

The review began from layout commit `14f00539c`, which includes the preceding
documentation and directory cleanup. The standard changes are separate from
that cleanup.

## Findings and corrections

| Area | Problem found | Correction |
| --- | --- | --- |
| RN-1 key verification | Strict Ed25519 verification was said to forbid two distinct valid signatures for one message. Canonical verification does not provide that property. | Retain strict verification, remove the uniqueness claim, and explicitly identify tokens by signed content. Add a vector with two valid signatures, two refusal cases and unchanged token identity. |
| RN-1 ephemeral keys | The key schedule allowed a rotated server ephemeral key, while the server handshake required a fresh keypair for each mapping. | Align both sections with fresh per-mapping keypairs, as the SDK already does. |
| RN-1 configuration | The text said the server key could not be supplied and omitted existing environment exemptions. | Describe generated or supplied `server_secret_key`, its ownership and erasure, and the exempt size marker and secret field. Clarify redacted diagnostics. |
| RN-2 unreliable forms | SINGLE capacity allowed 1415 bytes while promotion was mandatory above the 1411-byte record limit. Another sentence required a record even when SINGLE was allowed. | Separate encoding capacity from application send policy; retain promotion above 1411 and allow unfragmented SINGLE where applicable. Exercise every size from 1412 through 1415 in the promotion test. |
| RN-2 fragments | `fragments` was described as a count although it encodes count minus one. The copy formula could add a fragment offset twice. | Define encoding and decoding explicitly, distinguish fragment index from count, and calculate the destination from the block base in bytes. |
| RN-2 block limits | The receiver-check prose implied the fragment-count check enforced the exact completed message length. | State the separate completion check against the configured message maximum and its refusal counter. |
| RN-2 timing | Formulas combined second-valued estimates with millisecond configuration fields, and called resend delays timeouts. | Show explicit real-valued conversion and distinguish resend scheduling from connection lifetime. |
| RN-1 replay | A vector description rejected every probe below the high-water mark, although an unseen in-window packet is valid. Some table rows omitted authentication guards. | Describe the retained window accurately and make successful opening explicit before marking. |
| Security and conformance claims | The rationale treated finite random values as collision-free and described forward secrecy and vector agreement too broadly. | State the randomness and erasure assumptions, distinguish session from handshake protection, and limit evidence claims to the cases checked. |
| Normative independence | Implementation paths survived inside inline code even though the style checker purported to prohibit them. | Remove those citations, move tooling commands to the guide, and add checker regressions that reject inline file, test and model citations. |

The signature correction follows the distinction between signing and
verification in [RFC 8032, sections 5.1.6 and 5.1.7](https://www.rfc-editor.org/rfc/rfc8032.html#section-5.1.7).
The committed fixture demonstrates the result against the configured sodium
implementation using public synthetic input. It does not demonstrate a forgery
by someone without the signing key.

## Human readability

[The reading guide](../STANDARD.md) now offers routes for backend authors,
endpoint implementers and reviewers. A normal handshake sequence connects the
individual sections into an end-to-end example. Tables distinguish packet
sequences from message numbers, replay acceptance from ACK publication, and
transport fragmentation from message blocks. Worked arithmetic explains the
fragment-offset and timing rules.

The rationale's explanations now render as separate paragraphs instead of
long runs of adjacent Markdown lines. Its key-schedule discussion is rewritten
around the actual assumptions and guarantees. Existing section IDs remain
available for links and review references.

## AI readability

The most valuable improvements are precise operands, units, indexes and
guards, shared with human readers. The guide tells an agent to retrieve whole
sections and their prerequisites, cite the document and rule, and identify
observable outcomes. It explicitly forbids inferring sender permissions from
a format-capacity vector or treating an implementation detail as a requirement.

The checker now enforces the intended inline-citation boundary. Pins and
vectors remain machine-readable. They complement prose review; they cannot
detect every contradiction in prose.

## Validation

- The standard checker passes: 220 compiled pins, 166 matched vectors, and no
  pending normative or rationale sections.
- All 46 standard test registrations pass in the Linux development build.
  Together with promotion, resend-timing and in-place-send cases, 70 tests pass.
- The new signature vector passes in the macOS production build as part of
  the standard key-schedule test.
- Seven standard-checker regressions and eight witness-checker tests pass.
- All 60 witness rows resolve to their named anchors. The 28 existing `none`
  rows remain explicitly recorded as gaps.
- Maintained documentation links, owned-header guards, the source audit,
  changed-test clang-format 18 checks, and `git diff --check` pass.

## Remaining confidence limits

The witness ledger is a partial inventory. A resolvable test name does not
prove the whole rule, and a model reference does not prove that its current
configuration has executed. Erasure, constant-time behavior, resource policy
and several lifecycle guarantees still need stronger evidence where the ledger
records gaps. No gap was relabelled as covered merely to improve the count.

Independent client/server interoperability is still a useful next acceptance
gate. It should include loss, reordering, retries, malformed packets, boundary
sizes, configuration disagreement and resource exhaustion. That would test
whether a second implementation reaches the same behavior from the document.
This review did not produce that second implementation.

I regard the revised standard as substantially clearer and more dependable.
I would still distinguish those improvements from an assertion of complete
protocol correctness.
