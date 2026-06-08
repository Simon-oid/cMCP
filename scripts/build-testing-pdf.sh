#!/usr/bin/env bash
# Render docs/testing-overview.md to a polished PDF with the Mermaid
# diagrams rasterised to inline SVG (so they appear in the PDF instead of
# as code fences).
#
# Pipeline:  md ──marked──▶ html body
#            md ──mmdc────▶ one SVG per ```mermaid block
#            splice SVGs into the body, wrap in a print template
#            html ──google-chrome --print-to-pdf──▶ pdf
#
# The .md stays the single source of truth; this script is just a view.
#
# Deps (all already present on the dev box): mmdc (mermaid-cli),
# npx (marked, fetched on first run), node, google-chrome-stable.
#
# Usage:  scripts/build-testing-pdf.sh [input.md] [output.pdf]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:-$ROOT/docs/testing-overview.md}"
OUT="${2:-$ROOT/docs/testing-overview.pdf}"

CHROME="$(command -v google-chrome-stable || command -v google-chrome || command -v chromium || true)"
[ -n "$CHROME" ] || { echo "no chrome/chromium found" >&2; exit 1; }
command -v mmdc >/dev/null || { echo "mmdc (mermaid-cli) not found" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1. Strip <details>/<summary> so the collapsed tables render inline in the
#    PDF (you want everything visible on paper), then split out each
#    ```mermaid fenced block into its own .mmd file, in document order.
node - "$IN" "$WORK" <<'NODE'
const fs = require('fs');
const [, , inPath, work] = process.argv;
let md = fs.readFileSync(inPath, 'utf8');
md = md.replace(/^<\/?details>\s*$/gm, '')
       .replace(/^<summary>.*<\/summary>\s*$/gm, '');
let i = 0;
md = md.replace(/```mermaid\n([\s\S]*?)```/g, (_, body) => {
  fs.writeFileSync(`${work}/diagram_${i}.mmd`, body);
  return '```mermaid\n@@MMD' + (i++) + '@@\n```';
});
fs.writeFileSync(`${work}/clean.md`, md);
console.error(`mermaid blocks: ${i}`);
NODE

# 2. Rasterise each diagram to SVG (keeps the classDef colour coding).
n=0
while [ -f "$WORK/diagram_${n}.mmd" ]; do
  mmdc -i "$WORK/diagram_${n}.mmd" -o "$WORK/diagram_${n}.svg" -b white -q >/dev/null 2>&1
  n=$((n+1))
done

# 3. Markdown -> HTML body.
npx -y marked -i "$WORK/clean.md" -o "$WORK/body.html" 2>/dev/null

# 4. Splice the SVGs back in and wrap in the print template.
node - "$WORK" "$OUT" <<'NODE'
const fs = require('fs');
const [, , work, out] = process.argv;
let body = fs.readFileSync(`${work}/body.html`, 'utf8');

// Replace each rendered mermaid code block (now carrying @@MMDn@@) with the
// matching inline SVG, centred in a figure box.
body = body.replace(/<pre><code class="language-mermaid">@@MMD(\d+)@@\n?<\/code><\/pre>/g,
  (_, idx) => {
    const svg = fs.readFileSync(`${work}/diagram_${idx}.svg`, 'utf8')
                  .replace(/<\?xml[^>]*\?>/, '');
    return `<div class="fig">${svg}</div>`;
  });

const css = `
  @page { size: A4; margin: 11mm 12mm; }
  * { box-sizing: border-box; }
  body { font: 10pt/1.4 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;
         color: #1f2937; max-width: 100%; }
  h1 { font-size: 19pt; color: #0f172a; border-bottom: 3px solid #2563eb;
       padding-bottom: 5px; margin: 0 0 4px; }
  h2 { font-size: 13pt; color: #1e3a8a; margin: 13px 0 6px;
       border-left: 4px solid #2563eb; padding-left: 8px;
       page-break-after: avoid; }
  h3 { font-size: 10.5pt; color: #334155; margin: 9px 0 4px;
       page-break-after: avoid; }
  p, li { font-size: 9.3pt; margin: 4px 0; }
  hr { border: 0; border-top: 1px solid #e2e8f0; margin: 10px 0; }
  table { border-collapse: collapse; width: 100%; margin: 5px 0 9px;
          font-size: 8.6pt; page-break-inside: avoid; }
  th { background: #1e3a8a; color: #fff; text-align: left; padding: 4px 7px; }
  td { border-bottom: 1px solid #e5e7eb; padding: 3px 7px; vertical-align: top; }
  tr:nth-child(even) td { background: #f8fafc; }
  code { font-family: "DejaVu Sans Mono",ui-monospace,Menlo,Consolas,monospace;
         font-size: 8.4pt; background: #f1f5f9; padding: 1px 4px; border-radius: 3px;
         color: #0f172a; }
  td code { background: transparent; padding: 0; color: #2563eb; letter-spacing: -1.5px; }
  .fig { text-align: center; margin: 7px 0 10px; page-break-inside: avoid; }
  .fig svg { max-width: 100%; max-height: 340px; height: auto; }
  em { color: #64748b; font-size: 8.6pt; }
  a { color: #2563eb; text-decoration: none; }
`;

const html = `<!doctype html><html><head><meta charset="utf-8">
<title>cMCP Testing Overview</title><style>${css}</style></head>
<body>${body}</body></html>`;

fs.writeFileSync(`${work}/final.html`, html);
fs.writeFileSync('/tmp/_cmcp_pdf_outpath', out);
NODE

# 5. HTML -> PDF.
"$CHROME" --headless=new --no-sandbox --disable-gpu --no-pdf-header-footer \
  --virtual-time-budget=5000 \
  --print-to-pdf="$OUT" "$WORK/final.html" 2>/dev/null

echo "wrote $OUT"
