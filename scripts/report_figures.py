#!/usr/bin/env python3
"""Render the figures for docs/cmcp-engineering-report.tex.

All numbers are the published snapshot from the hermetic `make test` /
`make bench-compare` run (see the report body for date + machine). Output
is a set of PNGs written to the directory given as argv[1]. Keep the
matplotlib aesthetic deliberately plain (white ground, value labels,
no chartjunk) to match the cRAG performance report.
"""
import sys
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)

# cRAG-report palette: green = this work, grey = reference systems, blue accent.
GREEN = "#2e7d32"
GREY = "#9e9e9e"
BLUE = "#1565c0"

plt.rcParams.update({
    "font.size": 9,
    "axes.titlesize": 10,
    "axes.titleweight": "bold",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.dpi": 200,
})


def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, name), bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------- throughput
def fig_throughput():
    labels = ["cMCP\n(C11)", "TypeScript SDK\n(Node)", "Python SDK\n(FastMCP)"]
    vals = [43494, 8035, 974]
    colors = [GREEN, GREY, GREY]
    fig, ax = plt.subplots(figsize=(6.4, 2.5))
    bars = ax.barh(labels, vals, color=colors, height=0.62)
    ax.invert_yaxis()
    ax.set_xlabel("tools/call echo  —  calls/s  (higher is better)")
    ax.set_title("Throughput over stdio (10 000 iters, same client, server swapped)")
    ax.set_xlim(0, max(vals) * 1.15)
    for b, v in zip(bars, vals):
        ax.text(b.get_width() + max(vals) * 0.01, b.get_y() + b.get_height() / 2,
                f"{v:,}", va="center", ha="left", fontsize=9, fontweight="bold")
    ax.grid(axis="x", color="#e0e0e0", lw=0.6)
    ax.set_axisbelow(True)
    save(fig, "fig_throughput.png")


# ------------------------------------------------------------------- latency
def fig_latency():
    impls = ["cMCP", "TypeScript SDK", "Python SDK"]
    p50 = [21, 95, 994]
    p99 = [31, 189, 1315]
    mean = [22, 123, 1025]
    x = range(len(impls))
    w = 0.26
    fig, ax = plt.subplots(figsize=(6.4, 2.6))
    b1 = ax.bar([i - w for i in x], p50, w, label="p50", color=GREEN)
    b2 = ax.bar(list(x), mean, w, label="mean", color=BLUE)
    b3 = ax.bar([i + w for i in x], p99, w, label="p99", color=GREY)
    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(impls)
    ax.set_ylabel("latency — µs (log)")
    ax.set_title("Per-call round-trip latency (lower is better)")
    for bars in (b1, b2, b3):
        for b in bars:
            ax.text(b.get_x() + b.get_width() / 2, b.get_height() * 1.05,
                    f"{int(b.get_height())}", ha="center", va="bottom", fontsize=7.5)
    ax.legend(frameon=False, ncol=3, loc="upper left", fontsize=8)
    ax.set_ylim(10, 3000)
    ax.grid(axis="y", color="#e0e0e0", lw=0.6)
    ax.set_axisbelow(True)
    save(fig, "fig_latency.png")


# ----------------------------------------------------------------------- RSS
def fig_rss():
    labels = ["cMCP\n(328 KB binary)", "Python SDK\n(FastMCP)", "TypeScript SDK\n(Node)"]
    vals = [2.2, 60.0, 74.0]
    colors = [GREEN, GREY, GREY]
    fig, ax = plt.subplots(figsize=(6.4, 2.3))
    bars = ax.barh(labels, vals, color=colors, height=0.6)
    ax.invert_yaxis()
    ax.set_xlabel("idle resident set size  —  MB  (lower is better)")
    ax.set_title("Idle memory footprint of a stdio server")
    ax.set_xlim(0, max(vals) * 1.15)
    for b, v in zip(bars, vals):
        ax.text(b.get_width() + max(vals) * 0.01, b.get_y() + b.get_height() / 2,
                f"{v:g} MB", va="center", ha="left", fontsize=9, fontweight="bold")
    ax.grid(axis="x", color="#e0e0e0", lw=0.6)
    ax.set_axisbelow(True)
    save(fig, "fig_rss.png")


# ------------------------------------------------------------------- pyramid
def fig_pyramid():
    labels = ["L1 — Unit\n(json, rpc, schema)",
              "L2 — Integration\n(transports, lifecycle, …)",
              "Security / Hardening\n(cross-cutting)"]
    vals = [1520, 2170, 195]
    colors = ["#f9a825", GREEN, "#c62828"]
    fig, ax = plt.subplots(figsize=(6.4, 2.3))
    bars = ax.barh(labels, vals, color=colors, height=0.6)
    ax.invert_yaxis()
    ax.set_xlabel("assertions in the hermetic `make test` run")
    ax.set_title("Assertion distribution — 3 885 across 30 binaries (L3 eval is opt-in)")
    ax.set_xlim(0, max(vals) * 1.15)
    for b, v in zip(bars, vals):
        ax.text(b.get_width() + max(vals) * 0.01, b.get_y() + b.get_height() / 2,
                f"{v:,}", va="center", ha="left", fontsize=9, fontweight="bold")
    ax.grid(axis="x", color="#e0e0e0", lw=0.6)
    ax.set_axisbelow(True)
    save(fig, "fig_pyramid.png")


if __name__ == "__main__":
    fig_throughput()
    fig_latency()
    fig_rss()
    fig_pyramid()
    print(f"wrote figures to {OUT}", file=sys.stderr)
