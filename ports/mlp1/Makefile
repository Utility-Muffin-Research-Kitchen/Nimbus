# Nimbus MLP1 cross-build. Runs inside the mlp1-toolchain container with the UMRK
# workspace mounted at /workspace (Nimbus, Catastrophe as siblings). cJSON is
# vendored in this repo and #included directly by main.c (single translation unit).
TC       := /opt/mlp1-toolchain
CC       := $(TC)/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT  := $(TC)/aarch64-buildroot-linux-gnu/sysroot
APP      ?= /workspace/Nimbus
CATA     ?= /workspace/Catastrophe
CJSON    ?= $(APP)/third_party/cjson
OUT      := $(APP)/ports/mlp1/pak/bin/nimbus

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -DPLATFORM_MLP1 \
           -I$(APP)/src -I$(CATA)/include -I$(CJSON) \
           -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL2
LDFLAGS := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf -lSDL2_image -lGLESv2 -lEGL \
           -lcurl -lm -lpthread

.PHONY: all clean
all:
	mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $(OUT) $(APP)/src/main.c $(LDFLAGS)
clean:
	rm -f $(OUT)
