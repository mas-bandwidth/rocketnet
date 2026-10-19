# Client packet reader proofs: the dispatcher, the per-type readers for
# packets from the server, and the record consumer. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the readers on the nightly list. Each harness runs on the full bound,
# 0..ROCKETNET_MAX_PACKET_BYTES bytes; results.md records each result.
# Each harness then runs again with -DPROOF_WITNESS, where it must fail
# exactly its "witness:" assertions, every one of them and nothing else
# (its accepting paths are reachable);
# otherwise its one line reads VERIFICATION FAILED. A timeout also reads
# VERIFICATION FAILED, with the reason.

NIGHTLY += proofs-client_packets

CLIENT_PACKETS_SOURCE = client_packets/proof_client_packets.c
CLIENT_PACKETS_FLAGS = --object-bits 12 --arrays-uf-always --slice-formula -I$(SDK) -I$(SDK)/../sodium
CLIENT_PACKETS_UNWIND = 64
CLIENT_PACKETS_TIMEOUT = 1800

CLIENT_PACKETS_HARNESSES = \
    harness_process_cookie \
    harness_process_challenge \
    harness_process_denied \
    harness_open_session_packet \
    harness_process_keep_alive \
    harness_process_payload \
    harness_process_ack \
    harness_process_fragment \
    harness_process_disconnect \
    harness_process_packet \
    harness_process_record

.PHONY: proofs-client_packets

proofs-client_packets:
	@echo "proofs-client_packets"; \
	failed=0; \
	for harness in $(CLIENT_PACKETS_HARNESSES); do \
	    output=$$(mktemp); \
	    status=0; \
	    timeout $(CLIENT_PACKETS_TIMEOUT) $(CBMC) $(CBMC_FLAGS) --unwind $(CLIENT_PACKETS_UNWIND) \
	        $(CLIENT_PACKETS_FLAGS) --function $$harness \
	        $(CLIENT_PACKETS_SOURCE) > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "unwinding assertion.*FAILURE" "$$output"; then \
	        verdict="unwinding assertion failed"; cat "$$output"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; cat "$$output"; \
	    elif [ $$status -eq 124 ]; then \
	        verdict="VERIFICATION FAILED (timed out after $(CLIENT_PACKETS_TIMEOUT)s)"; \
	    else \
	        verdict="VERIFICATION FAILED (cbmc exit $$status)"; cat "$$output"; \
	    fi; \
	    rm -f "$$output"; \
	    if [ $$status -ne 0 ]; then \
	        failed=1; \
	        if [ "$$verdict" = "VERIFICATION SUCCESSFUL" ]; then \
	            verdict="VERIFICATION FAILED (cbmc exit $$status)"; \
	        fi; \
	    fi; \
	    output=$$(mktemp); \
	    status=0; \
	    timeout $(CLIENT_PACKETS_TIMEOUT) $(CBMC) $(CBMC_FLAGS) --unwind $(CLIENT_PACKETS_UNWIND) \
	        $(CLIENT_PACKETS_FLAGS) -DPROOF_WITNESS --function $$harness \
	        $(CLIENT_PACKETS_SOURCE) > "$$output" 2>&1 || status=$$?; \
	    failures=$$(grep -c ": FAILURE$$" "$$output"); \
	    witnesses=$$(grep ": FAILURE$$" "$$output" | grep -c "witness: "); \
	    unreached=$$(grep "^\[$$harness\." "$$output" | grep "witness: " | grep -c ": SUCCESS$$"); \
	    if [ $$status -ne 10 ] || [ $$failures -lt 1 ] || [ $$failures -ne $$witnesses ] || [ $$unreached -ne 0 ]; then \
	        cat "$$output"; failed=1; \
	        if [ "$$verdict" = "VERIFICATION SUCCESSFUL" ]; then \
	            verdict="VERIFICATION FAILED (a witness was not reached)"; \
	        fi; \
	    fi; \
	    rm -f "$$output"; \
	    echo "$$harness $$verdict"; \
	done; \
	exit $$failed
