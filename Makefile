.PHONY: build
build:
	@mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j --no-print-directory

.PHONY: clean
clean:
	rm -rf build
	rm -rf bin/*
