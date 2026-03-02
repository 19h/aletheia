.PHONY: all release debug plugin idump test clean purge install install-debug

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

plugin-debug: .configure-debug
	cmake --build --preset debug --target aletheia_ida

idump: .configure-release
	cmake --build --preset release-optimized --target idump

# Build and run tests
test: .configure-release
	cmake --build --preset release-optimized --target aletheia_tests test_idiom_resolver
	ctest --preset release-optimized

test-debug: .configure-debug
	cmake --build --preset debug --target aletheia_tests test_idiom_resolver
	ctest --preset debug

# ── IDA plugin install ───────────────────────────────────────────────────
# Resolves the plugin artifact (dylib on macOS, so on Linux) from each build
# preset, copies it into $HOME/.idapro/plugins/, and on macOS ad-hoc
# codesigns it so IDA will load it without Gatekeeper complaints.

IDAPRO_PLUGINS_DIR := $(HOME)/.idapro/plugins

# Determine per-platform artifact name produced by ida_add_plugin().
# On macOS the CMake idax helper names the output aletheia_ida.dylib;
# on Linux it produces aletheia_ida.so.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PLUGIN_ARTIFACT := aletheia_ida.dylib
else
    PLUGIN_ARTIFACT := aletheia_ida.so
endif

install: plugin
	@mkdir -p "$(IDAPRO_PLUGINS_DIR)"
	@echo "Installing $(PLUGIN_ARTIFACT) -> $(IDAPRO_PLUGINS_DIR)/"
	@cp "build-release-optimized/$(PLUGIN_ARTIFACT)" "$(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"
ifeq ($(UNAME_S),Darwin)
	@echo "Ad-hoc codesigning $(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)..."
	@codesign -f -s - "$(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"
endif
	@echo "Done. Plugin installed to $(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"

install-debug: plugin-debug
	@mkdir -p "$(IDAPRO_PLUGINS_DIR)"
	@echo "Installing $(PLUGIN_ARTIFACT) (debug) -> $(IDAPRO_PLUGINS_DIR)/"
	@cp "build-debug/$(PLUGIN_ARTIFACT)" "$(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"
ifeq ($(UNAME_S),Darwin)
	@echo "Ad-hoc codesigning $(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)..."
	@codesign -f -s - "$(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"
endif
	@echo "Done. Debug plugin installed to $(IDAPRO_PLUGINS_DIR)/$(PLUGIN_ARTIFACT)"

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
