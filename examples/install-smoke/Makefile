# Standalone Makefile for the install-smoke consumer (pkg-config flavor).
# Builds outside cMCP's tree against an installed cMCP discovered via
# pkg-config. Invoke from examples/install-smoke/run.sh.

CC         ?= cc
PKG_CONFIG ?= pkg-config

PKG_MODULES := cmcp-server cmcp-client cmcp-core

CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PKG_MODULES))
LDFLAGS :=
LDLIBS  := $(shell $(PKG_CONFIG) --libs $(PKG_MODULES))

pkgconfig-smoke: smoke.c
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f pkgconfig-smoke

.PHONY: clean
