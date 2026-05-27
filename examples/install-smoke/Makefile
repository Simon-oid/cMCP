# Standalone Makefile for the install-smoke consumer (pkg-config flavor).
# Builds outside cMCP's tree against an installed cMCP discovered via
# pkg-config. Invoke from examples/install-smoke/run.sh.

CC         ?= cc
PKG_CONFIG ?= pkg-config

PKG_MODULES := cmcp-server cmcp-client cmcp-core

# Static-by-default library: pass --static so pkg-config emits the
# transitive private deps (libcurl, pthread, libm) declared in
# cmcp-core's Libs.private. Without --static the link line is just
# `-lcmcp_*` and the static archive's transitive symbols (fmod from
# libm, curl_easy_init from libcurl, ...) fail to resolve.
CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PKG_MODULES))
LDFLAGS :=
LDLIBS  := $(shell $(PKG_CONFIG) --libs --static $(PKG_MODULES))

pkgconfig-smoke: smoke.c
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f pkgconfig-smoke

.PHONY: clean
