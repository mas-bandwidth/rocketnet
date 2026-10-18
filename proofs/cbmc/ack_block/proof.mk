# The Ack reader is checked on the nightly lane with the actual SDK sources.
NIGHTLY += proofs-ack_block

ACK_BLOCK_SOURCE = ack_block/proof_ack_block.c
ACK_BLOCK_FLAGS = --std c++17 --z3 --overflow-check -DROCKETNET_ESBMC -DNDEBUG -DACK_READ_MAX_BYTES=512 -DACK_APPLY_SENT_BOUND=1 -DACK_BUILD_HIGHEST_BOUND=8 -I$(SDK) -I$(SDK)/../sodium

.PHONY: proofs-ack_block
proofs-ack_block:
	@echo proofs-ack_block; set -e; \
	source_cpp=$$(mktemp ack_block/proof_ack_block.XXXXXX.cpp); \
	cp $(ACK_BLOCK_SOURCE) "$$source_cpp"; \
	trap 'rm -f "$$source_cpp"' EXIT; \
	failed=0; \
	for entry in \
	    harness_ack_read:128 \
	    harness_ack_reconstruct:2 \
	    harness_ack_apply:4 \
	    harness_ack_build:9 \
	    harness_ack_write:256; do \
	    harness=$${entry%%:*}; unwind=$${entry##*:}; \
	    output=$$(mktemp); status=0; \
	    timeout 1800s prlimit --as=8589934592 -- esbmc $(ACK_BLOCK_FLAGS) --unwind $$unwind --function $$harness "$$source_cpp" > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then verdict="VERIFICATION FAILED"; cat "$$output"; \
	    elif grep -qi "unwinding assertion" "$$output"; then verdict="unwinding assertion failed"; cat "$$output"; \
	    else verdict="FAILED (exit $$status)"; cat "$$output"; fi; \
	    echo "$$harness $$verdict (unwind $$unwind)"; \
	    rm -f "$$output"; \
	    if [ $$status -ne 0 ]; then failed=1; fi; \
	done; exit $$failed
