# client_packets CBMC proofs — NOT RUN

Status: HOLD on toolchain. The nightly scaffold is present at BASE
(`make -n -C proofs/cbmc nightly` lists `proofs-packet_open`), so the
blocker from attempt 1 is resolved. The proofs of this card cannot be run:
the bench has no CBMC toolchain. `command -v cbmc` succeeds on no path, a
filesystem-wide search finds no `cbmc` or `goto-cc` binary, and `cbmc --version`
does not run. CBMC 6.x is required by the card's TOOLCHAIN and DONE-WHEN lines.

Nothing of this card was proved or verified. The harnesses
(`proof_client_packets.c`), the make target (`proof.mk`), and the per-harness
property table are all not done; no VERIFICATION lines exist because no harness
was run.

Recording of the attempt:
- `proofs/cbmc/client_packets` was absent at BASE (red condition met).
- `make -n -C proofs/cbmc nightly` at BASE lists `proofs-packet_open` only, not
  `proofs-client_packets`.
- The gate command `command -v cbmc && cbmc --version` fails (exit 1); both
  `cbmc` and `goto-cc` are missing from PATH and from the filesystem.
