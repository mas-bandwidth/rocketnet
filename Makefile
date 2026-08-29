.PHONY: build
build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j --no-print-directory

# the same build with the server transport on the GSO driver (linux only):
# the seam means everything above the driver is byte-identical, so the full
# test suite against this build is the matrix proving the abstraction

.PHONY: build-gso
build-gso:
	@mkdir -p build-gso && cd build-gso && cmake -DCMAKE_BUILD_TYPE=Release -DROCKETNET_SERVER_GSO=1 .. && make -j --no-print-directory

.PHONY: clean
clean:
	rm -rf build
	rm -rf build-gso
	rm -rf bin/*
