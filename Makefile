# the child make's job count: CMAKE_BUILD_PARALLEL_LEVEL when it is set (CI sets it on
# every fleet runner), else the processors capped at 8; overridden by `make JOBS=<n>` or by
# the caller's own -j.
JOBS ?= $(if $(CMAKE_BUILD_PARALLEL_LEVEL),$(CMAKE_BUILD_PARALLEL_LEVEL),$(shell n=$$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4); [ "$$n" -gt 8 ] && n=8; echo $$n))

# make builds the default development library (ROCKETNET_DEV=ON); make
# ROCKETNET_DEV=OFF builds the production library (BUILDING.md, "Production builds").
.PHONY: build
build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=$(if $(ROCKETNET_DEV),$(ROCKETNET_DEV),ON) .. && $(MAKE) $(if $(filter -j%,$(MAKEFLAGS)),,-j$(JOBS)) --no-print-directory

# the end-to-end check: build the real binaries, then drive them over loopback.
# the tool is built from its .go file because the two C programs share its
# directory; ./bin/happy_path is not committed.
.PHONY: happy-path
happy-path: build
	cd tools/happy_path && nice -n 19 go build -o ../../bin/happy_path happy_path.go
	./bin/happy_path

# transport selection is runtime now (#106): one build carries every transport its
# platform has, and ROCKETNET_SERVER_TRANSPORT / ROCKETNET_CLIENT_TRANSPORT (or
# the config's transport field) select at create -- the functional suite against
# each rung is the matrix proving the abstraction, e.g.
# ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional

.PHONY: docs docs-undocumented
docs:
	@sh tools/docs/mrdocs.sh

docs-undocumented:
	@if [ ! -f build/docs/mrdocs.log ]; then sh tools/docs/mrdocs.sh >/dev/null; fi
	@sh tools/docs/undocumented.sh build/docs/mrdocs.log

# clang-format check over tracked sources; the CI format-check job runs it.
.PHONY: format-check
format-check:
	@sh tools/format/check_format.sh

.PHONY: clean
clean:
	rm -rf build
	rm -rf bin/*
