# Glassbox developer shortcuts. The product itself is the GUI launcher, not a
# CLI; these targets only build and verify it.
BUILD ?= build
JOBS ?= 8

.PHONY: all configure build test run demo bench fuzz asan tsan clean

all: build

configure:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

run: build
	./$(BUILD)/glassbox

demo: build
	./$(BUILD)/glassbox_demo

bench: build
	./$(BUILD)/bench/glassbox_bench

fuzz: build
	./$(BUILD)/fuzz/glassbox_fuzz reader 500
	./$(BUILD)/fuzz/glassbox_fuzz query 2000
	./$(BUILD)/fuzz/glassbox_fuzz json 2000
	./$(BUILD)/fuzz/glassbox_fuzz http 200
	./$(BUILD)/fuzz/glassbox_fuzz roundtrip 500

asan:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
	  -DGBX_SANITIZER=address,undefined
	cmake --build build-asan -j $(JOBS)
	ctest --test-dir build-asan --output-on-failure

tsan:
	cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
	  -DGBX_SANITIZER=thread
	cmake --build build-tsan -j $(JOBS)
	ctest --test-dir build-tsan --output-on-failure

clean:
	rm -rf build build-asan build-tsan
