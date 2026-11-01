# Packet prefilter and sealed-packet opener proofs. Included by
# proofs/cbmc/Makefile through the reader scaffold. The four harnesses finish
# in seconds, so the reader is in the per-commit `all` set (cbmc.yml) rather
# than on the nightly list: a change that stops rocketnet_protocol.cpp from
# parsing in the C front end goes red on the pull request that makes it.

PACKET_OPEN_SOURCE = packet_open/proof_packet_open.c
PACKET_OPEN_FLAGS = --object-bits 12 -DNDEBUG -I$(SDK) -I$(SDK)/../sodium

.PHONY: proofs-packet_open

proofs-packet_open:
	@echo "proofs-packet_open"; \
	set -e; \
	for harness in \
	    harness_prefilter \
	    harness_open_sealed_packet \
	    harness_write_packet_associated_data \
	    harness_sequence_nonce; do \
	    output=$$(mktemp); \
	    status=0; \
	    $(CBMC) $(CBMC_FLAGS) --unwind 64 $(PACKET_OPEN_FLAGS) --function $$harness \
	        $(PACKET_OPEN_SOURCE) > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; cat "$$output"; \
	    elif grep -q "unwinding assertion" "$$output"; then \
	        verdict="unwinding assertion failed"; cat "$$output"; \
	    else \
	        verdict="FAILED (exit $$status)"; cat "$$output"; \
	    fi; \
	    echo "$$harness $$verdict"; \
	    rm -f "$$output"; \
	    if [ $$status -ne 0 ]; then exit 1; fi; \
	done
