SHELL := /bin/bash

# Nimbus — weather app for Leaf (Miniloong Pocket 1), Catastrophe UI.
# Port of the NextUI Nimbus: Apostrophe+PakKit -> Catastrophe, WeatherAPI -> Open-Meteo.
# See PLAN.md.

APP_NAME  := nimbus
PAK_NAME  := Nimbus
BUILD_DIR := build

# Desktop dev build resolves the toolkit from the workspace sibling; cJSON is
# vendored here and #included directly by main.c (single translation unit).
CATASTROPHE_DIR ?= ../Catastrophe
CJSON_DIR       ?= third_party/cjson

NATIVE_BIN := $(BUILD_DIR)/native/$(APP_NAME)
MLP1_BIN   := ports/mlp1/pak/bin/$(APP_NAME)
MLP1_PACKAGE := $(BUILD_DIR)/mlp1/package/$(PAK_NAME).pak

CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Isrc -I$(CJSON_DIR) -I$(CATASTROPHE_DIR)/include

.PHONY: all native run-native mlp1 package-platform package-mlp1 clean help

all: native

# Desktop dev build against the sibling Catastrophe + brew SDL2 (gets a window up).
native:
	@mkdir -p $(BUILD_DIR)/native
	cc $(CFLAGS_COMMON) -O0 -g -DPLATFORM_MAC \
		$(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image) \
		-o $(NATIVE_BIN) src/main.c \
		$(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image) -lcurl -lm -lpthread \
		-framework Cocoa

run-native: native
	./$(NATIVE_BIN)

# Cross-compile the aarch64 binary (Docker mlp1-toolchain). [Phase 4]
mlp1:
	@./scripts/build-mlp1.sh

package-platform:
	@test -n "$(PLATFORM)" || { echo "usage: make package-platform PLATFORM=mlp1" >&2; exit 1; }
	@case "$(PLATFORM)" in \
		mlp1) $(MAKE) package-mlp1 ;; \
		*) echo "unsupported package platform: $(PLATFORM)" >&2; exit 1 ;; \
	esac

# Assemble the staged pak: launch.sh + pak.json + res + the built binary. [Phase 4]
package-mlp1: mlp1
	@rm -rf "$(MLP1_PACKAGE)"
	@mkdir -p "$(MLP1_PACKAGE)/bin"
	@cp launch.sh pak.json "$(MLP1_PACKAGE)/"
	@if [ -f LICENSE ]; then cp LICENSE "$(MLP1_PACKAGE)/"; fi
	@if [ -d res ]; then cp -R res "$(MLP1_PACKAGE)/"; fi
	@cp "$(MLP1_BIN)" "$(MLP1_PACKAGE)/bin/$(APP_NAME)"
	@echo "=== Packaged: $(MLP1_PACKAGE) ==="

clean:
	rm -rf $(BUILD_DIR) ports/*/pak/bin

help:
	@echo "make native       desktop dev build (sibling Catastrophe + brew SDL2)"
	@echo "make run-native   build + run the desktop dev build"
	@echo "make package-mlp1 build aarch64 + assemble the Leaf pak  [Phase 4]"
