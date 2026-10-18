# Nightly ESBMC proof of the real server packet readers.
NIGHTLY += proofs-server_packets

ESBMC ?= esbmc
SERVER_PACKETS_SOURCE = server_packets/proof_server_packets.c
SERVER_PACKETS_FLAGS = --std c++17 --z3 --overflow-check -DROCKETNET_ESBMC -DNDEBUG -I$(SDK) -I$(SDK)/../sodium -Iserver_packets

.PHONY: proofs-server_packets

proofs-server_packets:
	@set -e; failed=0; \
	for spec in harness_connection_request:64 harness_connection_response:32 harness_sealed_packet:256 harness_process_packet:256; do \
	    harness=$${spec%:*}; unwind=$${spec#*:}; \
	    output=$$(mktemp); source=$$(mktemp --suffix=.cpp); cp $(SERVER_PACKETS_SOURCE) "$$source"; status=0; \
	    timeout 1800s prlimit --as=8589934592 -- $(ESBMC) $(SERVER_PACKETS_FLAGS) --unwind $$unwind --unwindsetname __memcmp_impl:0:65,__memcpy_impl:0:577,__memset_impl:0:1905,esbmc_shim.h@rocketnet_esbmc_havoc_slice:0:385 --function $$harness "$$source" >"$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    elif grep -qi "unwinding assertion" "$$output"; then \
	        verdict="unwinding assertion failed"; \
	    elif [ $$status -eq 124 ]; then \
	        verdict="timed out"; \
	    else \
	        verdict="not proved (exit $$status)"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then cat "$$output"; fi; \
	    echo "$$harness $$verdict (unwind $$unwind)"; \
	    rm -f "$$output" "$$source"; \
	    if [ $$status -ne 0 ] || [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then failed=1; fi; \
	done; \
	exit $$failed
