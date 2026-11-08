#!/bin/sh
# test-stats.sh — keep the README's test counts honest.
#
# Reads a `make test` log, counts the test binaries and the assertions
# they ran, and compares or rewrites the README sentence between the
# markers
#     <!-- test-stats -->...<!-- /test-stats -->
#
# Every test binary prints one summary line on stderr (tests/test.h):
#     "  <passed>/<total> assertions passed"
# so the binary count is the number of summary lines and the assertion
# count is the sum of <total>. A failing binary aborts `make test`, so
# a complete log always comes from a green run.
#
# Usage:
#   make test 2>&1 | tee test.log
#   scripts/test-stats.sh test.log            # print the counts
#   scripts/test-stats.sh --check test.log    # exit 1 if README differs
#   scripts/test-stats.sh --update test.log   # rewrite README in place
#
# Env:
#   README   path of the file to check/update (default README.md)

set -eu

README=${README:-README.md}
MODE=print

case "${1:-}" in
    --check)  MODE=check;  shift ;;
    --update) MODE=update; shift ;;
esac

if [ $# -ne 1 ] || [ ! -r "$1" ]; then
    echo "usage: $0 [--check|--update] <make-test-log>" >&2
    exit 2
fi
LOG=$1

# "<binaries> <assertions>", assertions with thousands separators.
COUNTS=$(awk '
    /^[ \t]*[0-9]+\/[0-9]+ assertions passed/ {
        split($1, frac, "/")
        if (frac[1] != frac[2]) bad = 1
        total += frac[2]
        bins++
    }
    END {
        if (bins == 0 || bad) exit 1
        s = sprintf("%d", total); out = ""
        while (length(s) > 3) {
            out = "," substr(s, length(s) - 2) out
            s = substr(s, 1, length(s) - 3)
        }
        print bins, s out
    }' "$LOG") || {
    echo "test-stats: no complete green run found in $LOG" >&2
    exit 1
}

BINS=${COUNTS% *}
ASSERTS=${COUNTS#* }
SENTENCE="$ASSERTS assertions across $BINS test binaries"

if [ "$MODE" = print ]; then
    echo "$SENTENCE"
    exit 0
fi

# Same pattern twice: grep needs a bare "/", sed needs it escaped.
MARKED_GREP='<!-- test-stats -->[^<]*<!-- /test-stats -->'
MARKED='<!-- test-stats -->[^<]*<!-- \/test-stats -->'
if ! grep -q "$MARKED_GREP" "$README"; then
    echo "test-stats: no <!-- test-stats --> marker in $README" >&2
    exit 1
fi

CURRENT=$(sed -n "s/.*<!-- test-stats -->\([^<]*\)<!-- \/test-stats -->.*/\1/p" "$README" | head -n 1)

if [ "$MODE" = check ]; then
    if [ "$CURRENT" = "$SENTENCE" ]; then
        echo "test-stats: README is current ($SENTENCE)"
        exit 0
    fi
    echo "test-stats: README says '$CURRENT', make test ran '$SENTENCE'." >&2
    echo "test-stats: run scripts/test-stats.sh --update <log> and commit." >&2
    exit 1
fi

TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
sed "s/$MARKED/<!-- test-stats -->$SENTENCE<!-- \/test-stats -->/" "$README" > "$TMP"
cat "$TMP" > "$README"
echo "test-stats: README updated ($SENTENCE)"
