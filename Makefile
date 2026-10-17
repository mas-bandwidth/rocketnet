.PHONY: build
build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && $(MAKE) -j --no-print-directory

# driver selection is runtime now (#106): one build carries every driver its
# platform has, and ROCKETNET_DRIVER (or the config's driver field) selects
# at create -- the functional suite against each rung is the matrix proving the
# abstraction, e.g. ROCKETNET_DRIVER=portable ./bin/functional

.PHONY: clean
clean:
	rm -rf build
	rm -rf bin/*

# ROADMAP.md is generated from docs/roadmap.sexp (make roadmap, make roadmap-check)
include tools/roadmap/roadmap.mk
