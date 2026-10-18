# Server per-type packet reader proofs. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the readers on the nightly list.

NIGHTLY += proofs-server_packets

SERVER_PACKETS_SOURCE = server_packets/proof_server_packets.c
SERVER_PACKETS_FLAGS = --object-bits 12 -DNDEBUG -I$(SDK) -I$(SDK)/../sodium

.PHONY: proofs-server_packets

proofs-server_packets:
	@echo "proofs-server_packets"; \
	set -e; \
	for harness in \
	    harness_connection_request \
	    harness_connection_response \
	    harness_sealed_packet \
	    harness_process_packet; do \
	    output=$$(mktemp); \
	    status=0; \
	    $(CBMC) $(CBMC_FLAGS) --unwind 64 $(SERVER_PACKETS_FLAGS) --function $$harness \
	        $(SERVER_PACKETS_SOURCE) > "$$output" 2>&1 || status=$$?; \
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
