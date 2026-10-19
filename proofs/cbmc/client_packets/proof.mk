# Client packet reader proofs under ESBMC 8.5: the dispatcher, the per-type
# readers for packets from the server, and the record consumer. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the readers on the nightly list. Each harness runs on the full bound,
# 0..ROCKETNET_MAX_PACKET_BYTES bytes; results.md records each result.
#
# The harness is copied to a .cpp under a fixed name so ESBMC selects its
# C++ front end and the named loop bounds below match its static functions.
# A harness that proves runs again with -DPROOF_WITNESS and only its own
# assertions checked (--multi-property): every failed property must be one
# of its "witness:" assertions, so its accepting paths are reachable.
# One line per harness: VERIFICATION SUCCESSFUL, VERIFICATION FAILED,
# unwinding assertion failed, out of memory, solver failed, timed out, or
# not proved. Each run is niced, under a 12 GiB address-space cap.

NIGHTLY += proofs-client_packets

ESBMC ?= esbmc
CLIENT_PACKETS_SOURCE = client_packets/proof_client_packets.c
CLIENT_PACKETS_FLAGS = --std c++17 --z3 --overflow-check --unsigned-overflow-check \
    --ub-shift-check -DROCKETNET_ESBMC -I. -I$(SDK) -I$(SDK)/../sodium
# 65 is meant to cover every sdk loop on these paths (the longest seen is the
# 32-byte key write); 1473 is meant to cover a copy, fill or havoc of up
# to one maximum packet (1472 bytes). The unwinding assertions check both;
# no run has reached them yet (results.md).
CLIENT_PACKETS_UNWIND = 65
CLIENT_PACKETS_LOOPS = --unwindsetname proof_client_packets.cpp@proof_memcpy:0:1473,proof_client_packets.cpp@proof_memset:0:1473,esbmc_shim.h@rocketnet_esbmc_havoc_slice:0:1473
CLIENT_PACKETS_TIMEOUT = 1800
CLIENT_PACKETS_MEMORY = 12884901888

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
	dir=$$(mktemp -d); \
	cp $(CLIENT_PACKETS_SOURCE) "$$dir/proof_client_packets.cpp"; \
	for harness in $(CLIENT_PACKETS_HARNESSES); do \
	    output=$$(mktemp); \
	    status=0; \
	    timeout $(CLIENT_PACKETS_TIMEOUT) prlimit --as=$(CLIENT_PACKETS_MEMORY) -- nice -n 10 \
	        $(ESBMC) $(CLIENT_PACKETS_FLAGS) --unwind $(CLIENT_PACKETS_UNWIND) $(CLIENT_PACKETS_LOOPS) \
	        --function $$harness "$$dir/proof_client_packets.cpp" > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "unwinding assertion" "$$output" && grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="unwinding assertion failed"; cat "$$output"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; cat "$$output"; \
	    elif [ $$status -eq 124 ]; then \
	        verdict="timed out after $(CLIENT_PACKETS_TIMEOUT)s"; \
	    elif grep -qi "out of memory" "$$output"; then \
	        verdict="out of memory"; \
	    elif grep -q "SMT solver failed" "$$output"; then \
	        verdict="solver failed"; \
	    else \
	        verdict="not proved (esbmc exit $$status)"; tail -20 "$$output"; \
	    fi; \
	    rm -f "$$output"; \
	    if [ "$$verdict" = "VERIFICATION SUCCESSFUL" ]; then \
	        output=$$(mktemp); \
	        status=0; \
	        timeout $(CLIENT_PACKETS_TIMEOUT) prlimit --as=$(CLIENT_PACKETS_MEMORY) -- nice -n 10 \
	            $(ESBMC) $(CLIENT_PACKETS_FLAGS) -DPROOF_WITNESS --unwind $(CLIENT_PACKETS_UNWIND) $(CLIENT_PACKETS_LOOPS) \
	            --no-bounds-check --no-pointer-check --no-div-by-zero-check --no-align-check --no-unwinding-assertions \
	            --multi-property --function $$harness "$$dir/proof_client_packets.cpp" > "$$output" 2>&1 || status=$$?; \
	        failures=$$(grep -c "^  FAILED " "$$output"); \
	        witnesses=$$(grep "^  FAILED " "$$output" | grep -c "witness: "); \
	        if [ $$status -eq 124 ]; then \
	            verdict="VERIFICATION FAILED (the witness pass timed out)"; \
	        elif grep -qi "out of memory\|SMT solver failed" "$$output"; then \
	            cat "$$output"; verdict="VERIFICATION FAILED (the witness pass did not finish)"; \
	        elif [ $$failures -lt 1 ] || [ $$failures -ne $$witnesses ]; then \
	            cat "$$output"; verdict="VERIFICATION FAILED (a witness was not reached)"; \
	        fi; \
	        rm -f "$$output"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then failed=1; fi; \
	    echo "$$harness $$verdict"; \
	done; \
	rm -rf "$$dir"; \
	exit $$failed
