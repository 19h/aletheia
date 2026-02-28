.PHONY: all release debug plugin idump test clean purge

# Default target builds everything in release mode
all: release

# Configure targets using CMake Presets
.configure-release:
	cmake --preset release-optimized
	@touch .configure-release

.configure-debug:
	cmake --preset debug
	@touch .configure-debug

# Build the entire project
release: .configure-release
	cmake --build --preset release-optimized

debug: .configure-debug
	cmake --build --preset debug

# Build specific components (defaults to release mode for performance)
plugin: .configure-release
	cmake --build --preset release-optimized --target aletheia_ida

idump: .configure-release
	cmake --build --preset release-optimized --target idump

# Build and run tests
test: .configure-release
	cmake --build --preset release-optimized --target aletheia_tests test_idiom_resolver
	ctest --preset release-optimized

test-debug: .configure-debug
	cmake --build --preset debug --target aletheia_tests test_idiom_resolver
	ctest --preset debug

# Standard clean (keeps CMake cache and fetched dependencies like Z3)
clean:
	@echo "Cleaning release artifacts..."
	@if [ -d build-release-optimized ]; then cmake --build --preset release-optimized --target clean 2>/dev/null || true; fi
	@echo "Cleaning debug artifacts..."
	@if [ -d build-debug ]; then cmake --build --preset debug --target clean 2>/dev/null || true; fi

# Aggressive clean (wipes out build directories completely, will require re-fetching Z3)
purge:
	@echo "Purging all build directories and CMake configurations..."
	rm -rf build-release-optimized build-debug .configure-release .configure-debug
