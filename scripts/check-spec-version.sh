#!/usr/bin/env bash
# check-spec-version.sh — phase 5.7 spec-version drift watch.
#
# Lists the dated revision directories under
# modelcontextprotocol/modelcontextprotocol@main:schema/ and compares
# the newest one to CMCP_PROTOCOL_VERSION in include/cmcp.h.
#
# Exit 0 if matching, 1 if drift, 2 on infrastructure failure (can't
# reach the API, header missing, etc). On drift, prints both versions
# and points at the upgrade-workflow doc.
#
# Driven from CI on a weekly cron and on workflow_dispatch. Also
# runnable locally (`make check-spec-drift`).

set -euo pipefail

REPO=modelcontextprotocol/modelcontextprotocol
HEADER=include/cmcp.h

# --- 1. Current pin ---------------------------------------------------------
if [ ! -f "$HEADER" ]; then
    echo "check-spec-version: header not found at $HEADER (run from repo root)" >&2
    exit 2
fi
CURRENT=$(sed -nE 's/^#define[[:space:]]+CMCP_PROTOCOL_VERSION[[:space:]]+"([^"]+)".*/\1/p' "$HEADER")
if [ -z "$CURRENT" ]; then
    echo "check-spec-version: CMCP_PROTOCOL_VERSION not found in $HEADER" >&2
    exit 2
fi

# --- 2. Latest published revision ------------------------------------------
# Prefer gh (auths automatically in CI via GITHUB_TOKEN, raises the
# 60/hr → 5000/hr rate limit locally if the user is logged in).
# Fall back to curl for unattended local use.
fetch_dirs() {
    if command -v gh >/dev/null 2>&1; then
        gh api "repos/$REPO/contents/schema" 2>/dev/null
    else
        curl -fsSL -H 'Accept: application/vnd.github+json' \
            "https://api.github.com/repos/$REPO/contents/schema"
    fi
}

JSON=$(fetch_dirs) || {
    echo "check-spec-version: could not list $REPO@main:schema/" >&2
    echo "(network down, rate-limited, or repo layout changed)" >&2
    exit 2
}

# Newest dated dir (filter type=dir, drop 'draft', sort lex — dates are
# ISO-8601 so lex = chronological).
LATEST=$(printf '%s' "$JSON" | python3 -c '
import json, sys, re
entries = json.load(sys.stdin)
dates = []
for e in entries:
    if e.get("type") != "dir":
        continue
    name = e.get("name", "")
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", name):
        dates.append(name)
if not dates:
    sys.exit(2)
print(sorted(dates)[-1])
') || {
    echo "check-spec-version: no dated schema/ revisions found" >&2
    exit 2
}

# --- 3. Compare ------------------------------------------------------------
echo "  pinned:  CMCP_PROTOCOL_VERSION = $CURRENT  ($HEADER)"
echo "  latest:  $LATEST  ($REPO@main:schema/)"

if [ "$CURRENT" = "$LATEST" ]; then
    echo "  status:  in sync"
    exit 0
fi

echo
echo "  status:  DRIFT — the MCP spec has cut a newer dated revision."
echo
echo "  Next step: read the new schema and the changelog at"
echo "    https://github.com/$REPO/tree/main/schema/$LATEST"
echo "    https://modelcontextprotocol.io/specification/$LATEST"
echo "  then follow docs/spec-version-upgrade.md to plan the bump."
echo
echo "  This is informational, not urgent — the wire is still spec-"
echo "  compliant against $CURRENT, and the lifecycle code already"
echo "  handles cross-version handshakes."
exit 1
