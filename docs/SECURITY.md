# Security policy

## Reporting a vulnerability

Report vulnerabilities through GitHub's private vulnerability reporting for this
repository: open the **Security** tab, choose **Report a vulnerability**, and
submit the report there. Do not open a public issue, pull request, or discussion
for a suspected vulnerability, and do not include exploit details in any public
channel.

A useful report names the affected version or commit, the component, the impact,
the steps that reproduce it, and any proof of concept. If a detail is sensitive,
keep it inside the private report.

## What a reporter can expect, and when

- An acknowledgement within 3 business days.
- An initial assessment within 10 business days: whether the report is accepted
  as a vulnerability, the affected versions, and the severity.
- A progress update at least every 30 days while the report is open.
- A fix target of 90 days from acknowledgement for accepted vulnerabilities.
  Critical issues are targeted sooner. When a fix needs longer, the reason and a
  new target are stated in the private report and in the advisory.
- A written explanation when a report is not accepted as a vulnerability.
- Credit in the advisory under the name or handle the reporter chooses. No
  private contact details are published.

## Versions that receive fixes

The newest tagged minor release line receives security fixes in a patch release;
the first line is 1.0.x. Until v1.0.0 is tagged there is no supported
release. Previews such as v1.0.0-preview-1, the development branch and untagged
commits are not supported for production use. Older minor lines are not maintained: a fix ships in the newest
line and upgrading is the remedy. An advisory always names the affected versions
and the first fixed version.

## How a fix is disclosed

Fixes are coordinated privately until a patched release is available. The default
disclosure date is 90 days after acknowledgement, or an earlier date agreed with
the reporter. If a fix is not ready by that date, the advisory is published with
the available mitigation rather than extending the embargo indefinitely.

Disclosure is a GitHub Security Advisory on this repository. The advisory names
the affected versions, the fixed version, the impact, and any workaround, and it
links the release that carries the fix. The release is cut from `main` by
[docs/RELEASING.md](RELEASING.md). A security release also carries the
advisory reference in its release notes.

## Fuzz findings

A security-sensitive fuzz finding follows the private reporting and disclosure
process above. Its regression is prepared with the fix, without publishing an
unresolved vulnerability. A fuzz finding is minimized first
(`-minimize_crash=1`), then converted with
`tools/fuzz_finding_to_pcapng.py <harness> <input> --name <name>`, and lands
with its fix in the same pull request. The functional-tier replay
(`test_pcapng_corpus_replay` in `tests/functional/corpus_consistency.cpp`, run by
`./bin/functional`) reads `tests/pcapng/regress/` and holds the finding
thereafter. `tools/check_fuzz_regressions.py` refuses a regression seed without
its capture, a capture without its seed, and a capture that does not carry the
seed's bytes. A finding with no pcapng form keeps its `regress_` seed in the
harness corpus and names why in the pull request.

## Scope and security posture

This policy covers the rocketnet library and its wire protocol. The threat model,
the guarantees, and the explicit non-goals are normative in
[standard/TRANSPORT.md](../standard/TRANSPORT.md) and
[standard/MESSAGES.md](../standard/MESSAGES.md), each under **Security
considerations**. Under RN-1, session traffic has forward secrecy while the handshake does
not; a sealed box proves nothing about its sender; connection-request replay is a
denial of service against one client rather than an authentication break; no
response may be larger than the packet that provoked it; and volumetric floods
and attackers holding valid tokens are answered by the backend, not by this
layer. Those sections are the authority; this file does not restate their
reasoning.

The connect-token create and expire checks read the application's clock: the
epoch `token_epoch_unix_seconds` (or `ROCKETNET_CLIENT_TOKEN_EPOCH_UNIX_SECONDS`
and `ROCKETNET_SERVER_TOKEN_EPOCH_UNIX_SECONDS`) plus the time the caller
passes. An epoch of 0 resolves the epoch once, at client connect and at the
server's first update after start: wall-clock Unix seconds minus the caller's
elapsed seconds, with a positive fallback when subtraction is not possible.
Later checks add the caller's elapsed seconds to that epoch; they do not read
the wall clock again. Pausing the caller's clock therefore pauses token expiry.
The application must advance the supplied clock correctly; an explicit epoch
must represent Unix time at that clock's zero, not Unix time at the current
update.

The following are in scope for a report: packet parsing and generation, the
connect handshake and token handling, replay and acknowledgement state, fragment
reassembly, the public C API and ABI boundary, and the SDK's build and
configuration surface. The following are out of scope by design: volumetric
denial of service, the backend's identity and token-issuance policy, application
payloads above the transport, and deployment choices in the operating system or
network. A vulnerability in a vendored dependency is in scope for this repository
when the affected code is the copy built here; report it privately as above.
