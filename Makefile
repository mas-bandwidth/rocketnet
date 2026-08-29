.PHONY: build
build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. >/dev/null && $(MAKE) -j --no-print-directory

# driver selection is runtime now (#106): one build carries every driver its
# platform has, and ROCKETNET_DRIVER (or the config's driver field) selects
# at create -- the full suite against each rung is the matrix proving the
# abstraction, e.g. ROCKETNET_DRIVER=portable ./bin/test

.PHONY: clean
clean:
	rm -rf build
	rm -rf bin/*
