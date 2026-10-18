# ack block read proofs — HOLD

Not proved. `make -n -C proofs/cbmc nightly` fails at BASE (`No rule to make
target 'nightly'`): the first CBMC card's nightly scaffold (`NIGHTLY :=`,
`-include $(wildcard */proof.mk)`, `nightly` target) is absent from
proofs/cbmc/Makefile, so proofs-ack_block has no make target to attach to and
no nightly entry. No harnesses were written or run. See RESULT.md.
