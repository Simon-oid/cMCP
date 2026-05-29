CC         ?= cc
AR         ?= ar
PKG_CONFIG ?= pkg-config

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude $(CURL_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= $(CURL_LIBS) -lpthread -lm

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

# cmcp-tee: thin stdio proxy that tees wire traffic to a JSONL log.
# Pure plumbing — links neither libcurl nor cMCP libs, only pthreads.
TEE_BIN      := tools/cmcp-tee/cmcp-tee
TEE_SRC      := $(wildcard tools/cmcp-tee/cmcp-tee.c)

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
     $(if $(TEE_SRC),$(TEE_BIN)) \
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

# Tee links no cMCP libs — keep it independent so it can wrap servers
# built against older revisions of the lib without ABI surprises.
$(TEE_BIN): $(TEE_SRC)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

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

# --- Replay-based conformance gate (axis 5.3) -----------------------------
# Replays every wire transcript under conformance/fixtures/ at a freshly
# spawned server and asserts the recorded responses still match (modulo
# environmentally-variable fields, normalised via per-fixture masks in
# conformance/replay/fixtures.json). Hermetic: no network, no SDK install.
# Built and run as part of CI. Fixtures whose prerequisites are missing
# (e.g. crag-mcp not built, or no $CRAG_TEST_DB pointing at an indexed
# corpus) skip cleanly — non-fatal.
replay: all
	@command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }
	@echo "=== replay: wire-fixture regression gate ==="
	@python3 conformance/replay/replay.py

# --- Spec-version drift watch (axis 5.7) -----------------------------------
# Compares CMCP_PROTOCOL_VERSION against the newest dated revision
# under modelcontextprotocol/modelcontextprotocol@main:schema/. Exit 1
# if drift, 0 if in sync, 2 on infrastructure failure. Driven from CI
# on a weekly schedule + workflow_dispatch; runnable locally to
# preview. Does not auto-upgrade — see docs/spec-version-upgrade.md.
check-spec-drift:
	@./scripts/check-spec-version.sh

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

# --- Performance baselines (Tier 6 axis 6.6.1) ----------------------------
# Opt-in: in-process micro-benches over stdio and HTTP that emit CSV rows
# with p50/p95/p99/p999/mean latency + throughput. `make bench` runs all
# of them and concatenates output into bench/results.csv via run.sh.
# Methodology + interpretation lives in docs/perf-baselines.md.
BENCH_DIR  := bench
BENCH_BINS := $(BENCH_DIR)/bench_server_inline \
              $(BENCH_DIR)/bench_server_pool \
              $(BENCH_DIR)/bench_http

$(BENCH_DIR)/bench_%: $(BENCH_DIR)/bench_%.c $(BENCH_DIR)/bench_util.h $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

bench-build: $(BENCH_BINS)

bench: $(BENCH_BINS)
	@./$(BENCH_DIR)/run.sh

# --- Comparison bench (Tier 6 axis 6.6.2) --------------------------------
# Drives the cMCP client against an arbitrary MCP server binary (cMCP,
# TS-SDK, Python-SDK). cMCP runs unconditionally; TS/Python skip gracefully
# in run.sh if the toolchain isn't installed. The bench_compare binary
# itself links only cMCP — no SDK dependency at C level.
COMPARE_DIR := $(BENCH_DIR)/compare
COMPARE_BIN := $(COMPARE_DIR)/bench_compare

$(COMPARE_BIN): $(COMPARE_DIR)/bench_compare.c $(BENCH_DIR)/bench_util.h $(BUILT_LIBS)
	$(CC) $(CFLAGS) -o $@ $< $(BUILT_LIBS) $(LDFLAGS) $(LDLIBS)

bench-compare-build: $(COMPARE_BIN) examples/echo-server

bench-compare: bench-compare-build
	@./$(COMPARE_DIR)/run.sh

# --- Profile harness (Tier 6 axis 6.6.3) ---------------------------------
# Opt-in: cpu.sh + heap.sh capture call-graph and allocation profiles of
# bench_server_inline. Prefer perf / heaptrack; fall back to callgrind /
# massif. Findings + before/after callgrind dumps committed under
# bench/profile/baseline/.
bench-profile-cpu: $(BENCH_DIR)/bench_server_inline
	@./$(BENCH_DIR)/profile/cpu.sh

bench-profile-heap: $(BENCH_DIR)/bench_server_inline
	@./$(BENCH_DIR)/profile/heap.sh

bench-profile: bench-profile-cpu bench-profile-heap

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

# --- Coverage (Tier 6 axis 6.2.1) -----------------------------------------
# `make coverage` rebuilds the library + test suite with gcov instrumentation,
# runs the full hermetic suite to populate .gcda files, then produces three
# views of the result:
#
#   1. coverage/coverage.info   — lcov tracefile (input to genhtml / lcov CLI)
#   2. coverage/html/index.html — browsable line/branch coverage report
#   3. coverage/summary.txt     — gcovr text summary (drop-in for CI logs)
#
# Coverage is measured for the library code under src/ only. tests/, fuzz/,
# conformance/, examples/, and tools/ are excluded — they are exercisers, not
# subjects. Each run does a full clean rebuild because gcov .gcno files are
# tied 1:1 to the .o that produced them, and stale .gcno from a previous
# CFLAGS produces wildly misleading branch counts.
COV_BASE_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O0 -g -Iinclude \
                   $(CURL_CFLAGS) --coverage -fno-inline
COV_LDFLAGS     := --coverage
COV_DIR         := coverage

coverage:
	@command -v lcov    >/dev/null || { echo "lcov not installed";    exit 1; }
	@command -v genhtml >/dev/null || { echo "genhtml not installed"; exit 1; }
	@command -v gcovr   >/dev/null || { echo "gcovr not installed";   exit 1; }
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test \
	    CFLAGS="$(COV_BASE_CFLAGS)" LDFLAGS="$(COV_LDFLAGS)"
	@mkdir -p $(COV_DIR)
	@echo "=== lcov: capture (src/ only) ==="
	@lcov --rc geninfo_unexecuted_blocks=1 --capture --directory src \
	      --output-file $(COV_DIR)/coverage.info --quiet \
	      --ignore-errors mismatch,gcov,unused,negative,inconsistent
	@lcov --remove $(COV_DIR)/coverage.info \
	      '*/tests/*' '*/fuzz/*' '*/conformance/*' \
	      '*/examples/*' '*/tools/*' '/usr/*' \
	      --output-file $(COV_DIR)/coverage.info --quiet \
	      --ignore-errors unused,inconsistent
	@echo "=== genhtml: writing $(COV_DIR)/html/ ==="
	@genhtml $(COV_DIR)/coverage.info \
	    --output-directory $(COV_DIR)/html --quiet \
	    --title "cMCP coverage" --legend --branch-coverage \
	    --ignore-errors negative,inconsistent,source
	@echo "=== gcovr summary ==="
	@gcovr --root . --filter 'src/.*' \
	    --exclude '.*/tests/.*' --exclude '.*/fuzz/.*' \
	    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
	    --print-summary --txt-metric branch \
	    --output $(COV_DIR)/summary.txt
	@cat $(COV_DIR)/summary.txt
	@echo
	@echo "HTML report: $(COV_DIR)/html/index.html"

# --- Static analysis matrix (Tier 6 axis 6.2.2) ---------------------------
# `make analyze` runs three independent checkers in sequence:
#
#   1. clang-tidy    (per-TU, .clang-tidy config; checks bugprone/cert/
#                     clang-analyzer/portability/etc.)
#   2. scan-build    (clang static analyzer driving a full build;
#                     interprocedural path-sensitive analysis)
#   3. cppcheck      (independent open-source analyzer; suppressions
#                     in .cppcheck-suppressions)
#
# Each tool gets the same source set: src/*.c plus the reference-binary
# entry points under tools/ (excluding crag-mcp, which depends on the
# external cRAG tree). Tests / fuzz / examples / conformance harnesses
# are excluded — they are exercisers, not subjects.
#
# A fourth checker, CodeQL, runs only in CI via the GitHub-OSS lane.
# A fifth, gcc -fanalyzer, runs weekly (slow + false-positive-prone).
ANALYZE_SRC := $(CORE_SRC) $(SERVER_SRC) $(CLIENT_SRC) \
               tools/cmcp-inspect/main.c \
               tools/filesystem-mcp/main.c

ANALYZE_INCLUDES := -Iinclude $(CURL_CFLAGS)

analyze: analyze-clang-tidy analyze-scan-build analyze-cppcheck

analyze-clang-tidy:
	@command -v clang-tidy >/dev/null || { echo "clang-tidy not installed"; exit 1; }
	@echo "=== analyze: clang-tidy ==="
	@clang-tidy --quiet $(ANALYZE_SRC) -- \
	    -std=c11 $(ANALYZE_INCLUDES)

analyze-scan-build:
	@command -v scan-build >/dev/null || { echo "scan-build not installed"; exit 1; }
	@echo "=== analyze: scan-build ==="
	$(MAKE) --no-print-directory clean
	@# unix.BlockInCriticalSection fires on the read(/dev/urandom, ...)
	@# inside mint_session_id (transport_http.c) — /dev/urandom doesn't
	@# block once the CSPRNG is seeded; rationale tracked in the inline
	@# NOLINT comment for the clang-tidy lane.
	@scan-build --status-bugs -o /tmp/cmcp-scan-build \
	    -disable-checker unix.BlockInCriticalSection \
	    $(MAKE) --no-print-directory all CC=clang

analyze-cppcheck:
	@command -v cppcheck >/dev/null || { echo "cppcheck not installed"; exit 1; }
	@echo "=== analyze: cppcheck ==="
	@# --enable: warning + performance + portability. `style` is dominated
	@# by constParameter*/constVariable* suggestions — useful as a future
	@# const-correctness sweep but not a bug class for this gate. Re-enable
	@# if a more aggressive review pass wants it.
	@cppcheck --enable=warning,performance,portability \
	    --suppressions-list=.cppcheck-suppressions \
	    --inline-suppr \
	    --error-exitcode=1 \
	    --quiet \
	    --std=c11 \
	    -Iinclude \
	    $(ANALYZE_SRC)

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

# --- Packaging & install (Tier 6 axis 6.4) --------------------------------
# Standard GNU install layout. PREFIX is /usr/local by default; DESTDIR
# wraps every install path (for staged builds / distro packaging).
# Headers ship flat under $PREFIX/include — every header already carries
# a `cmcp_` prefix, so a subdirectory would only add a -I/dance for
# consumers without buying namespace separation.
PREFIX     ?= /usr/local
DESTDIR    ?=
exec_prefix = $(PREFIX)
BINDIR      = $(exec_prefix)/bin
LIBDIR      = $(exec_prefix)/lib
INCLUDEDIR  = $(PREFIX)/include
PKGCONFDIR  = $(LIBDIR)/pkgconfig
CMAKEDIR    = $(LIBDIR)/cmake/cmcp

INSTALL         ?= install
INSTALL_DATA    ?= $(INSTALL) -m 644
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DIR     ?= $(INSTALL) -d -m 755

# Single source of truth for the package version: CMCP_VERSION in
# include/cmcp.h. Awk extracts it so we don't fork bumping the version.
VERSION       := $(shell awk '/^#define CMCP_VERSION[ \t]/ {gsub(/"/, "", $$3); print $$3; exit}' include/cmcp.h)
VERSION_MAJOR := $(firstword $(subst ., ,$(VERSION)))

PUBLIC_HEADERS := $(wildcard include/cmcp*.h)

# --- Shared libraries (opt-in via ENABLE_SHARED=1) -------------------------
# Static is the default per the project's posture (linker-resolved,
# no symbol-versioning headaches, no LD_LIBRARY_PATH dance). The
# shared variant is for distro packagers; SONAME = libcmcp_<x>.so.<MAJOR>,
# with the standard real-file / SONAME-link / dev-link triple. Builds
# objects with -fPIC (independent of the static .o set) so the static
# and shared link targets can coexist in one tree.
ENABLE_SHARED ?= 0

CORE_PIC_OBJ   := $(CORE_SRC:.c=.pic.o)
SERVER_PIC_OBJ := $(SERVER_SRC:.c=.pic.o)
CLIENT_PIC_OBJ := $(CLIENT_SRC:.c=.pic.o)

CORE_SO   := libcmcp_core.so.$(VERSION)
SERVER_SO := libcmcp_server.so.$(VERSION)
CLIENT_SO := libcmcp_client.so.$(VERSION)

CORE_SONAME   := libcmcp_core.so.$(VERSION_MAJOR)
SERVER_SONAME := libcmcp_server.so.$(VERSION_MAJOR)
CLIENT_SONAME := libcmcp_client.so.$(VERSION_MAJOR)

src/%.pic.o: src/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(CORE_SO): $(CORE_PIC_OBJ)
	$(CC) -shared -Wl,-soname,$(CORE_SONAME) -o $@ $^ $(LDLIBS)
	@ln -sf $(CORE_SO) $(CORE_SONAME)
	@ln -sf $(CORE_SO) libcmcp_core.so

$(SERVER_SO): $(SERVER_PIC_OBJ) $(CORE_SO)
	$(CC) -shared -Wl,-soname,$(SERVER_SONAME) -o $@ $(SERVER_PIC_OBJ) \
	    -L. -lcmcp_core $(LDLIBS)
	@ln -sf $(SERVER_SO) $(SERVER_SONAME)
	@ln -sf $(SERVER_SO) libcmcp_server.so

$(CLIENT_SO): $(CLIENT_PIC_OBJ) $(CORE_SO)
	$(CC) -shared -Wl,-soname,$(CLIENT_SONAME) -o $@ $(CLIENT_PIC_OBJ) \
	    -L. -lcmcp_core $(LDLIBS)
	@ln -sf $(CLIENT_SO) $(CLIENT_SONAME)
	@ln -sf $(CLIENT_SO) libcmcp_client.so

ifeq ($(ENABLE_SHARED),1)
SHARED_LIBS := $(if $(CORE_OBJ),$(CORE_SO)) \
               $(if $(SERVER_OBJ),$(SERVER_SO)) \
               $(if $(CLIENT_OBJ),$(CLIENT_SO))
all: $(SHARED_LIBS)
endif

# --- pkg-config + CMake config: template substitution ---------------------
# Templates live in packaging/. Substitution happens inline in the
# install-pkgconfig / install-cmake rules below — NOT as a separate
# build-time step — so each `make install PREFIX=X` re-renders against
# the active PREFIX. (Staging through build/ would let make cache the
# first-seen PREFIX and silently bake the wrong prefix into a second
# install with a different PREFIX, which is exactly what the install-
# smoke regression gate caught the first time around.)
PC_TEMPLATES := packaging/pkgconfig/cmcp-core.pc.in \
                packaging/pkgconfig/cmcp-server.pc.in \
                packaging/pkgconfig/cmcp-client.pc.in

CMAKE_TEMPLATES := packaging/cmake/cmcpConfig.cmake.in \
                   packaging/cmake/cmcpConfigVersion.cmake.in

# --- Install / uninstall --------------------------------------------------
install: install-headers install-libs install-bins install-pkgconfig install-cmake

install-headers: $(PUBLIC_HEADERS)
	$(INSTALL_DIR) "$(DESTDIR)$(INCLUDEDIR)"
	@for h in $(PUBLIC_HEADERS); do \
	    echo "  install $$h -> $(DESTDIR)$(INCLUDEDIR)/"; \
	    $(INSTALL_DATA) $$h "$(DESTDIR)$(INCLUDEDIR)/"; \
	done

install-libs: $(BUILT_LIBS) $(if $(filter 1,$(ENABLE_SHARED)),$(SHARED_LIBS))
	$(INSTALL_DIR) "$(DESTDIR)$(LIBDIR)"
	@for l in $(BUILT_LIBS); do \
	    echo "  install $$l -> $(DESTDIR)$(LIBDIR)/"; \
	    $(INSTALL_DATA) $$l "$(DESTDIR)$(LIBDIR)/"; \
	done
ifeq ($(ENABLE_SHARED),1)
	@for so in $(SHARED_LIBS); do \
	    base=$$(echo $$so | sed -E 's/\.so\.[0-9][0-9.]*$$/.so/'); \
	    soname=$$(echo $$so | sed -E 's/(\.so\.[0-9]+).*$$/\1/'); \
	    echo "  install $$so + soname + dev-link -> $(DESTDIR)$(LIBDIR)/"; \
	    $(INSTALL_PROGRAM) $$so "$(DESTDIR)$(LIBDIR)/"; \
	    ln -sf $$so "$(DESTDIR)$(LIBDIR)/$$soname"; \
	    ln -sf $$so "$(DESTDIR)$(LIBDIR)/$$base"; \
	done
endif

install-bins:
	$(INSTALL_DIR) "$(DESTDIR)$(BINDIR)"
	@for b in $(INSPECT_BIN) $(FS_MCP_BIN) $(TEE_BIN); do \
	    if [ -x "$$b" ]; then \
	        echo "  install $$b -> $(DESTDIR)$(BINDIR)/"; \
	        $(INSTALL_PROGRAM) $$b "$(DESTDIR)$(BINDIR)/"; \
	    fi; \
	done

install-pkgconfig: $(PC_TEMPLATES)
	$(INSTALL_DIR) "$(DESTDIR)$(PKGCONFDIR)"
	@for tmpl in $(PC_TEMPLATES); do \
	    name=$$(basename $$tmpl .in); \
	    dest="$(DESTDIR)$(PKGCONFDIR)/$$name"; \
	    echo "  render $$tmpl -> $$dest"; \
	    sed -e 's|@PREFIX@|$(PREFIX)|g' \
	        -e 's|@VERSION@|$(VERSION)|g' \
	        "$$tmpl" > "$$dest"; \
	    chmod 644 "$$dest"; \
	done

install-cmake: $(CMAKE_TEMPLATES)
	$(INSTALL_DIR) "$(DESTDIR)$(CMAKEDIR)"
	@for tmpl in $(CMAKE_TEMPLATES); do \
	    name=$$(basename $$tmpl .in); \
	    dest="$(DESTDIR)$(CMAKEDIR)/$$name"; \
	    echo "  render $$tmpl -> $$dest"; \
	    sed -e 's|@PREFIX@|$(PREFIX)|g' \
	        -e 's|@VERSION@|$(VERSION)|g' \
	        "$$tmpl" > "$$dest"; \
	    chmod 644 "$$dest"; \
	done

# Uninstall removes the exact file set install would create. Empty
# directories left behind are best-effort rmdir'd (silently no-op if
# something else lives in them — we don't own /usr/local/lib).
uninstall:
	@for h in $(PUBLIC_HEADERS); do \
	    rm -f "$(DESTDIR)$(INCLUDEDIR)/$$(basename $$h)"; \
	done
	@for l in $(notdir $(BUILT_LIBS)); do rm -f "$(DESTDIR)$(LIBDIR)/$$l"; done
	@rm -f "$(DESTDIR)$(LIBDIR)/libcmcp_core.so"* \
	       "$(DESTDIR)$(LIBDIR)/libcmcp_server.so"* \
	       "$(DESTDIR)$(LIBDIR)/libcmcp_client.so"*
	@rm -f "$(DESTDIR)$(BINDIR)/cmcp-inspect" \
	       "$(DESTDIR)$(BINDIR)/filesystem-mcp" \
	       "$(DESTDIR)$(BINDIR)/cmcp-tee"
	@rm -f "$(DESTDIR)$(PKGCONFDIR)/cmcp-core.pc" \
	       "$(DESTDIR)$(PKGCONFDIR)/cmcp-server.pc" \
	       "$(DESTDIR)$(PKGCONFDIR)/cmcp-client.pc"
	@rm -f "$(DESTDIR)$(CMAKEDIR)/cmcpConfig.cmake" \
	       "$(DESTDIR)$(CMAKEDIR)/cmcpConfigVersion.cmake"
	@-rmdir "$(DESTDIR)$(CMAKEDIR)" 2>/dev/null || true
	@-rmdir "$(DESTDIR)$(PKGCONFDIR)" 2>/dev/null || true

# --- Source distribution tarball ------------------------------------------
# `make dist` produces cmcp-$(VERSION).tar.gz from HEAD. git archive
# naturally respects .gitignore (only tracked files end up in the
# tarball) and is reproducible across machines with the same HEAD.
DIST_NAME := cmcp-$(VERSION)
dist:
	@command -v git >/dev/null || { echo "git not found"; exit 1; }
	@test -d .git    || { echo "make dist requires a git checkout"; exit 1; }
	@echo "=== dist: $(DIST_NAME).tar.gz from HEAD ==="
	git archive --prefix=$(DIST_NAME)/ --format=tar.gz \
	    --output=$(DIST_NAME).tar.gz HEAD
	@ls -lh $(DIST_NAME).tar.gz

# --- API reference docs (Tier 6 axis 6.3) ---------------------------------
# `make docs` runs Doxygen against the public headers (and the README
# as the mainpage) to produce a browsable HTML reference under
# docs/api/html/. Doxyfile is tuned for C: typedef-of-struct hidden,
# EXTRACT_ALL=YES so every public declaration shows up even before
# its inline /** */ doc comment lands. Warning log lives at
# docs/api/doxygen.log; the target does NOT fail on undocumented
# decls (we ship a deliberately incremental doc surface — see CHANGELOG
# 6.3) but DOES fail on real parse errors.
docs:
	@command -v doxygen >/dev/null || { echo "doxygen not installed"; exit 1; }
	@mkdir -p docs/api
	@echo "=== doxygen: writing docs/api/html/ ==="
	@doxygen Doxyfile
	@echo
	@echo "HTML: docs/api/html/index.html"
	@echo "Warning log: docs/api/doxygen.log"

# --- install-smoke: end-to-end packaging gate -----------------------------
# Builds + installs into a throwaway temp prefix and exercises both
# packaging surfaces (pkg-config + CMake find_package) by building a
# tiny external consumer against the installed library. Pure regression
# gate: if the install / .pc / cmcpConfig.cmake plumbing breaks, this
# fails before any downstream notices.
install-smoke: all
	@./examples/install-smoke/run.sh

clean:
	rm -f $(CORE_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) \
	      $(CORE_PIC_OBJ) $(SERVER_PIC_OBJ) $(CLIENT_PIC_OBJ) \
	      $(CORE_LIB) $(SERVER_LIB) $(CLIENT_LIB) \
	      libcmcp_core.so* libcmcp_server.so* libcmcp_client.so* \
	      $(INSPECT_BIN) $(CRAG_MCP_BIN) $(FS_MCP_BIN) $(TEE_BIN) \
	      $(EXAMPLE_BINS) $(TEST_BIN) $(CONF_C_BIN) \
	      $(FUZZ_BINS) $(SOAK_BIN) $(BENCH_BINS) $(COMPARE_BIN) \
	      $(BENCH_DIR)/results.csv $(COMPARE_DIR)/results.csv \
	      tools/crag-mcp/*.o tools/cmcp-inspect/*.o \
	      tools/filesystem-mcp/*.o examples/*.o \
	      cmcp-*.tar.gz
	@find . -name '*.gcno' -delete -o -name '*.gcda' -delete 2>/dev/null || true
	@rm -rf $(COV_DIR) build/ docs/api/

.PHONY: all test valgrind test-asan test-tsan coverage \
        analyze analyze-clang-tidy analyze-scan-build analyze-cppcheck \
        fuzz-build fuzz-smoke docs \
        soak soak-churn bench bench-build bench-compare bench-compare-build \
        bench-profile bench-profile-cpu bench-profile-heap clean crag-mcp conformance replay check-spec-drift \
        install install-headers install-libs install-bins \
        install-pkgconfig install-cmake uninstall dist install-smoke
