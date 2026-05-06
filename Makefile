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
                     src/transport_stdio.c src/transport_http.c
SERVER_CANDIDATES := src/server.c src/notif.c
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

clean:
	rm -f $(CORE_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) \
	      $(CORE_LIB) $(SERVER_LIB) $(CLIENT_LIB) \
	      $(INSPECT_BIN) $(CRAG_MCP_BIN) \
	      $(EXAMPLE_BINS) $(TEST_BIN) \
	      tools/crag-mcp/*.o tools/cmcp-inspect/*.o examples/*.o

.PHONY: all test valgrind clean crag-mcp
