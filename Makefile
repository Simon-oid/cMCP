CC         ?= cc
AR         ?= ar
PKG_CONFIG ?= pkg-config

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude $(CURL_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= $(CURL_LIBS) -lpthread

# --- Source partitioning into three link targets ---------------------------
# Lists are wildcard-matched so the Makefile naturally grows as phases land.
CORE_CANDIDATES   := src/json.c src/rpc.c src/schema.c src/types.c \
                     src/http_parser.c \
                     src/transport_stdio.c src/transport_http.c \
                     src/transport_http_client.c
SERVER_CANDIDATES := src/server.c src/worker.c
CLIENT_CANDIDATES := src/client.c src/session.c

CORE_SRC   := $(wildcard $(CORE_CANDIDATES))
SERVER_SRC := $(wildcard $(SERVER_CANDIDATES))
CLIENT_SRC := $(wildcard $(CLIENT_CANDIDATES))

CORE_OBJ   := $(CORE_SRC:.c=.o)
SERVER_OBJ := $(SERVER_SRC:.c=.o)
CLIENT_OBJ := $(CLIENT_SRC:.c=.o)

CORE_LIB   := libcmcp_core.a
SERVER_LIB := libcmcp_server.a
CLIENT_LIB := libcmcp_client.a

# Link order: consumers (server/client) before provider (core), so the
# single-pass linker can resolve core symbols pulled in by server.o and
# client.o after their archives are scanned.
BUILT_LIBS := $(if $(SERVER_OBJ),$(SERVER_LIB)) \
              $(if $(CLIENT_OBJ),$(CLIENT_LIB)) \
              $(if $(CORE_OBJ),$(CORE_LIB))

# --- Reference binaries ----------------------------------------------------
INSPECT_BIN  := tools/cmcp-inspect/cmcp-inspect
INSPECT_SRC  := $(wildcard tools/cmcp-inspect/main.c)

# filesystem-mcp has no external dependency, so unlike crag-mcp it is
# built by `make all`.
FS_MCP_BIN   := tools/filesystem-mcp/filesystem-mcp
FS_MCP_SRC   := $(wildcard tools/filesystem-mcp/main.c)

# crag-mcp links cRAG statically. Override CRAG_DIR if cRAG isn't a sibling.
CRAG_DIR     ?= ../cRAG
CRAG_MCP_BIN := tools/crag-mcp/crag-mcp
CRAG_MCP_SRC := $(wildcard tools/crag-mcp/main.c)

# --- Examples --------------------------------------------------------------
EXAMPLE_SRC  := $(wildcard examples/*.c)
EXAMPLE_BINS := $(EXAMPLE_SRC:.c=)

# --- Tests -----------------------------------------------------------------
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:tests/%.c=tests/%)

# --- Targets ---------------------------------------------------------------
all: $(BUILT_LIBS) \
     $(if $(INSPECT_SRC),$(INSPECT_BIN)) \
     $(if $(FS_MCP_SRC),$(FS_MCP_BIN)) \
     $(EXAMPLE_BINS)

$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(SERVER_LIB): $(SERVER_OBJ)
	$(AR) rcs $@ $^

$(CLIENT_LIB): $(CLIENT_OBJ)
	$(AR) rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(INSPECT_BIN): $(INSPECT_SRC) $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

$(FS_MCP_BIN): $(FS_MCP_SRC) $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

examples/%: examples/%.c $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

# crag-mcp: separate target — only built when explicitly requested, since it
# requires cRAG to be present at $(CRAG_DIR).
crag-mcp: $(CRAG_MCP_BIN)

$(CRAG_MCP_BIN): $(CRAG_MCP_SRC) $(BUILT_LIBS)
	@test -d $(CRAG_DIR) || { echo "cRAG not found at $(CRAG_DIR) — set CRAG_DIR=..."; exit 1; }
	$(CC) $(CFLAGS) -I$(CRAG_DIR)/include -o $@ $< \
	    $(BUILT_LIBS) \
	    $(filter-out $(CRAG_DIR)/src/main.o, $(wildcard $(CRAG_DIR)/src/*.o)) \
	    $(CRAG_DIR)/third_party/sqlite3.o \
	    $(LDFLAGS) $(LDLIBS) -lm -ldl

# --- Conformance harness ---------------------------------------------------
# Opt-in: exercises cMCP against the MCP TypeScript reference implementation,
# in both wire roles. Needs Node + npm + network (first run installs the
# pinned reference SDKs into conformance/node_modules). Deliberately kept out
# of `make test`, which stays hermetic and offline. See conformance/README.md.
CONF_DIR   := conformance
CONF_C_BIN := $(CONF_DIR)/client_vs_ts

conformance: all $(CONF_C_BIN)
	@command -v npm >/dev/null || { echo "npm not found — install Node.js"; exit 1; }
	@echo "=== conformance: installing pinned reference SDKs ==="
	npm install --prefix $(CONF_DIR) --silent --no-audit --no-fund
	@echo
	@echo "=== conformance A: cMCP client  vs  TS server-everything ==="
	./$(CONF_C_BIN)
	@echo
	@echo "=== conformance B: TS client  vs  cMCP echo-server ==="
	node $(CONF_DIR)/client_vs_cmcp.mjs

$(CONF_C_BIN): $(CONF_DIR)/client_vs_ts.c $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

# --- Soak / long-running stability harness --------------------------------
# Opt-in: spawns the in-tree echo-server, runs a steady tools/call workload
# for $SOAK_DURATION seconds (default 120, nightly target 21600 = 6h),
# samples /proc metrics for both parent and child, and applies awk drift
# criteria. Kept OUT of `make test`. See tests/soak/run.sh for env knobs.
SOAK_DIR := tests/soak
SOAK_BIN := $(SOAK_DIR)/soak_driver

$(SOAK_BIN): $(SOAK_DIR)/soak_driver.c $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

soak: $(SOAK_BIN) examples/echo-server
	@./$(SOAK_DIR)/run.sh

soak-churn: $(SOAK_BIN) examples/echo-server
	@SOAK_CHURN=1 ./$(SOAK_DIR)/run.sh

# test_fs_server spawns the built filesystem-mcp binary as a child, so
# it depends on that binary in addition to the libs. This specific rule
# overrides the generic tests/% pattern below.
tests/test_fs_server: tests/test_fs_server.c $(BUILT_LIBS) $(FS_MCP_BIN)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

tests/%: tests/%.c $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "=== $$t ==="; ./$$t || exit 1; done

valgrind: $(TEST_BIN)
	@command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 1; }
	@for t in $(TEST_BIN); do \
	    echo "=== valgrind $$t ==="; \
	    valgrind --leak-check=full --errors-for-leak-kinds=definite \
	             --error-exitcode=1 --quiet ./$$t || exit 1; \
	done

# --- Sanitizer runs (Tier-5 quality gate) ---------------------------------
# Sanitizers can't combine — ASan and TSan own shadow memory differently —
# so we provide two separate targets. Each does a full clean rebuild so
# every .o is instrumented, then runs the existing suite. -O1 + frame
# pointers give readable stacks; -fno-sanitize-recover=all turns every UBSan
# finding into a hard abort so make notices.
SAN_BASE_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
                   -fno-omit-frame-pointer -Iinclude $(CURL_CFLAGS)
ASAN_CFLAGS := $(SAN_BASE_CFLAGS) -fno-sanitize-recover=all \
               -fsanitize=address,undefined
TSAN_CFLAGS := $(SAN_BASE_CFLAGS) -fsanitize=thread

# Surface every UBSan finding with a stack trace; tell ASan to abort hard.
ASAN_OPTS := halt_on_error=1:abort_on_error=1:print_stacktrace=1
UBSAN_OPTS := halt_on_error=1:print_stacktrace=1
TSAN_OPTS := halt_on_error=1:second_deadlock_stack=1

test-asan:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test CFLAGS="$(ASAN_CFLAGS)" \
	    ASAN_OPTIONS="$(ASAN_OPTS)" UBSAN_OPTIONS="$(UBSAN_OPTS)"

test-tsan:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test CFLAGS="$(TSAN_CFLAGS)" \
	    TSAN_OPTIONS="$(TSAN_OPTS)"

# --- libFuzzer harnesses (Tier-5 axis 5.4) --------------------------------
# Each harness in fuzz/fuzz_*.c is a tiny LLVMFuzzerTestOneInput wrapper
# around one parser entry point. libFuzzer is clang-only (gcc has no
# -fsanitize=fuzzer), so these targets pin CC to clang. The build is
# self-contained — we don't link against the static libs to keep the
# instrumentation uniform across every translation unit the harness
# touches, and to avoid sanitizer ABI mismatches.
#
# Run a single 60-second smoke (catches most low-hanging bugs):
#   make fuzz-json && ./fuzz/fuzz_json fuzz/corpus_json -max_total_time=60
# Run a 24-hour baseline (the cadence the agentic-readiness plan calls for):
#   ./fuzz/fuzz_json fuzz/corpus_json -max_total_time=86400 \
#       -print_final_stats=1 -artifact_prefix=fuzz/artifacts_json/
# Reproduce a crash:
#   ./fuzz/fuzz_json fuzz/artifacts_json/crash-<hash>
FUZZ_CC      := clang
FUZZ_CFLAGS  := -std=c11 -Wall -Wextra -O1 -g -Iinclude \
                -fno-omit-frame-pointer \
                -fsanitize=address,undefined,fuzzer \
                -fno-sanitize-recover=all

FUZZ_BINS    := fuzz/fuzz_json fuzz/fuzz_rpc fuzz/fuzz_schema fuzz/fuzz_http

# Each harness compiles the matching parser source directly + any
# dependencies. Keeping the dep lists explicit beats wildcarding the
# whole src/ tree — the harness is meant to fuzz one parser at a time,
# not the whole transport stack.
FUZZ_JSON_SRC   := src/json.c
FUZZ_RPC_SRC    := src/json.c src/rpc.c src/types.c
FUZZ_SCHEMA_SRC := src/json.c src/schema.c
FUZZ_HTTP_SRC   := src/http_parser.c

fuzz/fuzz_json: fuzz/fuzz_json.c $(FUZZ_JSON_SRC)
	@command -v $(FUZZ_CC) >/dev/null || { echo "$(FUZZ_CC) not installed"; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz/fuzz_rpc: fuzz/fuzz_rpc.c $(FUZZ_RPC_SRC)
	@command -v $(FUZZ_CC) >/dev/null || { echo "$(FUZZ_CC) not installed"; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz/fuzz_schema: fuzz/fuzz_schema.c $(FUZZ_SCHEMA_SRC)
	@command -v $(FUZZ_CC) >/dev/null || { echo "$(FUZZ_CC) not installed"; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz/fuzz_http: fuzz/fuzz_http.c $(FUZZ_HTTP_SRC)
	@command -v $(FUZZ_CC) >/dev/null || { echo "$(FUZZ_CC) not installed"; exit 1; }
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

fuzz-build: $(FUZZ_BINS)

# Quick smoke: 60s per harness with the checked-in seed corpus. Meant
# for "did I just regress the parser?", not for finding new bugs.
fuzz-smoke: $(FUZZ_BINS)
	@for h in $(FUZZ_BINS); do \
	    corpus=fuzz/corpus_$${h##*/fuzz_}; \
	    echo "=== fuzz-smoke $$h (60s) ==="; \
	    ./$$h $$corpus -max_total_time=60 -print_final_stats=0 || exit 1; \
	done

clean:
	rm -f $(CORE_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) \
	      $(CORE_LIB) $(SERVER_LIB) $(CLIENT_LIB) \
	      $(INSPECT_BIN) $(CRAG_MCP_BIN) $(FS_MCP_BIN) \
	      $(EXAMPLE_BINS) $(TEST_BIN) $(CONF_C_BIN) \
	      $(FUZZ_BINS) $(SOAK_BIN) \
	      tools/crag-mcp/*.o tools/cmcp-inspect/*.o \
	      tools/filesystem-mcp/*.o examples/*.o

.PHONY: all test valgrind test-asan test-tsan fuzz-build fuzz-smoke \
        soak soak-churn clean crag-mcp conformance
