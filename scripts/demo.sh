#!/usr/bin/env bash
# demo.sh — a paced, narrated terminal demo of cMCP, for screen recording.
#
# Five short scenes (~2 minutes with the default pause):
#   1. the same client talks to the same server in both MCP eras;
#   2. a sandboxed filesystem server refuses a symlink escape;
#   3. a schema violation comes back as a tool error the model can fix;
#   4. the cMCP client against the official Python SDK server,
#      including a multi round-trip request (elicitation);
#   5. the numbers: tests and conformance.
#
# Usage: scripts/demo.sh [pause-seconds]   (default 2.5)
# Needs: `make`, and for scene 4 `make conformance-py` run once (it
# creates the Python venv). Record the terminal with any screen
# recorder (e.g. `wf-recorder` on Wayland, OBS elsewhere).
set -euo pipefail
cd "$(dirname "$0")/.."

PAUSE="${1:-2.5}"
INSPECT=./tools/cmcp-inspect/cmcp-inspect
BOLD=$'\e[1m'; DIM=$'\e[2m'; OFF=$'\e[0m'

say()  { printf '\n%s# %s%s\n' "$BOLD" "$*" "$OFF"; sleep "$PAUSE"; }
run()  { printf '%s$ %s%s\n' "$DIM" "$*" "$OFF"; sleep 0.6; "$@" || true; sleep "$PAUSE"; }

make -s all >/dev/null
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say "cMCP: the Model Context Protocol in pure C11, client and server."

say "1. One server, two protocol eras. Legacy: initialize handshake."
run "$INSPECT" -c echo -a '{"text":"hello"}' -- ./examples/echo-server
say "Modern (MCP 2026-07-28): no handshake, every request self-contained."
run "$INSPECT" -m modern -c echo -a '{"text":"hello"}' -- ./examples/echo-server

say "2. A sandboxed filesystem server. A symlink that points outside..."
mkdir "$WORK/root"
echo "inside the sandbox" > "$WORK/root/note.txt"
ln -s /etc/passwd "$WORK/root/escape"
run "$INSPECT" -c fs_read -a '{"path":"note.txt"}' -- \
    ./tools/filesystem-mcp/filesystem-mcp --root "$WORK/root"
say "...is refused: the server never follows it out of the root."
run "$INSPECT" -c fs_read -a '{"path":"escape"}' -- \
    ./tools/filesystem-mcp/filesystem-mcp --root "$WORK/root"

say "3. Bad arguments come back as a tool error the model can correct."
run "$INSPECT" -c add -a '{"a":"two","b":40}' -- ./examples/echo-server

if [ -x conformance/py/.venv/bin/python ]; then
    make -s conformance/client_vs_py >/dev/null
    say "4. The cMCP client against the official Python SDK server."
    say "   'greet' needs the user's name: answered over a multi round-trip request."
    CMCP_CONF_PY=conformance/py/.venv/bin/python ./conformance/client_vs_py \
        > "$WORK/conf.log" 2>&1 || true
    grep -E '^  (stdio|http) +[a-z]+ +(pass|fail)|ok +greet' "$WORK/conf.log" || true
    sleep "$PAUSE"
else
    say "4. (skipped: run 'make conformance-py' once to set up the Python SDK)"
fi

say "5. Every number is measured by a script."
make -s test > "$WORK/test.log" 2>&1 || true
run scripts/test-stats.sh "$WORK/test.log"

say "Repository: github.com/Simon-oid/cMCP"
