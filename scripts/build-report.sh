#!/usr/bin/env bash
# Render docs/cmcp-engineering-report.tex to a polished LaTeX PDF, generating
# the matplotlib figures from the published snapshot numbers on the way.
#
# Pipeline:  scripts/report_figures.py ──▶ PNG charts (into a build dir)
#            docs/cmcp-engineering-report.tex + PNGs ──pdflatex──▶ pdf
#
# The .tex is the single source of truth; figures are derived. Deps (already
# on the dev box): python3 + matplotlib, pdflatex (TeX Live).
#
# Usage:  scripts/build-report.sh [input.tex] [output.pdf]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:-$ROOT/docs/cmcp-engineering-report.tex}"
OUT="${2:-$ROOT/docs/cmcp-engineering-report.pdf}"

command -v pdflatex >/dev/null || { echo "pdflatex (TeX Live) not found" >&2; exit 1; }
python3 -c "import matplotlib" 2>/dev/null || { echo "python3 + matplotlib not found" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1. Figures.
python3 "$ROOT/scripts/report_figures.py" "$WORK"

# 2. Compile (twice, so \vfill / refs settle) in the work dir.
cp "$IN" "$WORK/report.tex"
( cd "$WORK"
  pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null
  pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null
)

cp "$WORK/report.pdf" "$OUT"
echo "wrote $OUT"
