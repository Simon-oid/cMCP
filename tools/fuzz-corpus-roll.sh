#!/bin/sh
# tools/fuzz-corpus-roll.sh — Tier 7.2 corpus maintenance.
#
# libFuzzer accumulates discovered inputs in the corpus directory
# during a run. The nightly workflow uploads the post-run corpus as
# an artifact (90-day retention). This script folds those uploads
# back into the in-tree seed corpora — but selectively, since:
#
#   - Not every nightly discovery deserves to be tracked. Most are
#     duplicates or trivial-prefix variations of existing seeds.
#   - The repo seed corpus is meant to be small + high-signal:
#     each entry is a representative shape, not a corpus dump.
#   - Larger seed corpora slow down the per-PR `make fuzz-smoke`
#     gate (60s/harness — every extra seed eats into mutation time).
#
# Workflow (run weekly, locally or from a maintainer's box):
#
#   1. Download the four most recent fuzz-{json,rpc,schema,http}-corpus
#      artifacts from a recent green nightly run.
#   2. Unpack each into ./incoming/<harness>/.
#   3. Run this script. It drives libFuzzer's `-merge=1` mode for each
#      harness: merge=1 picks the minimal subset that preserves the
#      union's coverage. The resulting seed corpus is the strict
#      minimal cover.
#   4. Manually review the new entries (`git diff fuzz/corpus_<h>/`),
#      stage the ones that look meaningful (new shape, new edge), drop
#      the rest.
#
# Usage:
#   tools/fuzz-corpus-roll.sh                  # all four harnesses
#   tools/fuzz-corpus-roll.sh json schema      # subset
#
# Env:
#   INCOMING_DIR   dir holding unpacked artefacts (default ./incoming)
#   DRY_RUN        if "1", show what would change without writing

set -eu

INCOMING=${INCOMING_DIR:-./incoming}
DRY=${DRY_RUN:-0}
HARNESSES=${@:-json rpc schema http}

cd "$(git rev-parse --show-toplevel)"

for h in $HARNESSES; do
    bin="fuzz/fuzz_$h"
    seed="fuzz/corpus_$h"
    incoming="$INCOMING/$h"

    if [ ! -x "$bin" ]; then
        echo "fuzz/fuzz_$h not built — run: make fuzz/fuzz_$h" >&2
        echo "skipping $h" >&2
        continue
    fi
    if [ ! -d "$incoming" ]; then
        echo "$incoming missing — unpack the nightly artefact here" >&2
        echo "skipping $h" >&2
        continue
    fi

    echo "=== $h ==="
    echo "  seed:     $seed ($(find "$seed" -type f 2>/dev/null | wc -l) entries)"
    echo "  incoming: $incoming ($(find "$incoming" -type f 2>/dev/null | wc -l) entries)"

    # libFuzzer -merge=1: pick minimal subset of {incoming, seed}
    # whose coverage equals the union's. Output dir is a tmp dir
    # so we can diff against the current seed before committing.
    merged=$(mktemp -d)
    trap "rm -rf '$merged'" EXIT

    if ! $bin -merge=1 "$merged" "$seed" "$incoming" 2>&1 | tail -5; then
        echo "  libFuzzer merge failed; skipping $h" >&2
        continue
    fi

    if [ "$DRY" = "1" ]; then
        echo "  DRY_RUN — would replace $seed with $merged"
        echo "  (merged set size: $(find "$merged" -type f | wc -l) entries)"
    else
        # rsync would be cleaner but isn't always installed.
        rm -rf "$seed.new"
        cp -r "$merged" "$seed.new"
        echo "  staged $seed.new — review with:"
        echo "      diff -qr $seed $seed.new"
        echo "      git diff --no-index $seed $seed.new | less"
        echo "  promote with:"
        echo "      rm -rf $seed && mv $seed.new $seed"
    fi
    echo
done

cat <<'EOF'
Next: review the staged corpus_*.new directories, promote the ones that
look useful, drop the rest. Then:

    git add fuzz/corpus_*
    git commit -m "fuzz: fold nightly corpus week of <date>"

Per the docs (CLAUDE.md commit policy), the user runs this commit step.
EOF
