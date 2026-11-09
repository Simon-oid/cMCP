#!/usr/bin/env python3
"""replay.py — wire-fixture replay gate for cMCP.

For each fixture listed in fixtures.json:

  1. Run any pre-test setup (e.g. populate a sandbox).
  2. Spawn the configured server.
  3. Stream all dir:"in" frames from the fixture into the server.
  4. Read the server's stdout until it produces as many frames as the
     fixture has dir:"out" frames (or the timeout fires).
  5. Compare each observed frame to the corresponding expected frame
     under JSON-equality. Optional output_masks zero out variable
     fields (DB paths, hostnames, sandbox paths the test creates
     fresh each run) so deterministic content is still strictly
     checked while environmental noise is ignored.

Exit code: 0 if every required fixture passes (including those that
were deliberately skipped because a prerequisite was absent), 1 on the
first hard failure. Skips for missing optional prerequisites print a
note but do not fail the run — the CI lane and the local lane share
this script and the local lane has more capabilities (e.g. Ollama).

Fixture format: produced by tools/cmcp-tee. One JSON object per line:
    {"t":<unix>, "dir":"in"|"out", "frame":"<raw JSON-RPC frame>"}
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = Path(__file__).with_name("fixtures.json")
DEFAULT_TIMEOUT_S = 10.0
# How long to keep stdin open *after* writing the last frame, to give
# the server time to flush the response. cmcp-tee captures used 0.2s
# in their recording incantations — we match that.
LINGER_S = 0.3


# ---------------------------------------------------------------------------
# Fixture loading
# ---------------------------------------------------------------------------

@dataclass
class Frame:
    direction: str   # "in" or "out"
    body: str        # raw JSON-RPC frame (no trailing \n)


def load_fixture(path: Path) -> list[Frame]:
    frames: list[Frame] = []
    with path.open() as f:
        for ln, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{ln}: not valid JSONL: {exc}")
            if rec.get("dir") not in ("in", "out"):
                raise SystemExit(f"{path}:{ln}: missing/invalid 'dir'")
            if not isinstance(rec.get("frame"), str):
                raise SystemExit(f"{path}:{ln}: 'frame' must be a string")
            frames.append(Frame(rec["dir"], rec["frame"]))
    return frames


# ---------------------------------------------------------------------------
# Frame-level masking + comparison
# ---------------------------------------------------------------------------

def apply_mask(obj: Any, path: str, repl: Any) -> Any:
    """Replace the value at the given JSON-pointer-ish path with `repl`.

    `path` is a slash-separated sequence of object keys / array indices,
    e.g. /result/serverInfo/version, /result/contents/0/text.
    A missing path is a no-op so a mask can safely target both success
    and error response shapes.
    """
    if not path or path == "/":
        return repl
    parts = [p.replace("~1", "/").replace("~0", "~")  # RFC 6901
             for p in path.split("/") if p != ""]
    cur = obj
    for p in parts[:-1]:
        if isinstance(cur, list):
            try:
                cur = cur[int(p)]
            except (ValueError, IndexError):
                return obj
        elif isinstance(cur, dict):
            if p not in cur:
                return obj
            cur = cur[p]
        else:
            return obj
    last = parts[-1]
    if isinstance(cur, list):
        try:
            cur[int(last)] = repl
        except (ValueError, IndexError):
            pass
    elif isinstance(cur, dict) and last in cur:
        cur[last] = repl
    return obj


def apply_text_regex(obj: Any, path: str, pattern: str, repl: str) -> Any:
    """Like apply_mask but treats the targeted leaf as a string and
    runs re.sub on it. Useful when only PART of a field is volatile
    (e.g. a temp-dir suffix inside a longer message)."""
    parts = [p.replace("~1", "/").replace("~0", "~")  # RFC 6901
             for p in path.split("/") if p != ""]
    cur = obj
    for p in parts[:-1]:
        if isinstance(cur, list):
            try:
                cur = cur[int(p)]
            except (ValueError, IndexError):
                return obj
        elif isinstance(cur, dict):
            if p not in cur:
                return obj
            cur = cur[p]
        else:
            return obj
    last = parts[-1]
    if isinstance(cur, dict) and isinstance(cur.get(last), str):
        cur[last] = re.sub(pattern, repl, cur[last])
    elif isinstance(cur, list):
        try:
            i = int(last)
            if isinstance(cur[i], str):
                cur[i] = re.sub(pattern, repl, cur[i])
        except (ValueError, IndexError):
            pass
    return obj


def normalise(frame_body: str, masks: list[dict]) -> Any:
    obj = json.loads(frame_body)
    for m in masks:
        kind = m.get("kind", "set")
        path = m["path"]
        if kind == "set":
            apply_mask(obj, path, m.get("value", "<masked>"))
        elif kind == "regex":
            apply_text_regex(obj, path, m["pattern"], m.get("replacement", ""))
        else:
            raise SystemExit(f"unknown mask kind: {kind}")
    return obj


def compare(expected: str, actual: str, masks: list[dict]) -> tuple[bool, str]:
    e = normalise(expected, masks)
    a = normalise(actual, masks)
    if e == a:
        return True, ""
    return False, (
        "expected:\n  "
        + json.dumps(e, indent=2, sort_keys=True).replace("\n", "\n  ")
        + "\nactual:\n  "
        + json.dumps(a, indent=2, sort_keys=True).replace("\n", "\n  ")
    )


# ---------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------

def check_prereqs(prereqs: list[dict]) -> str | None:
    for p in prereqs:
        kind = p["kind"]
        if kind == "cmd":
            if not shutil.which(p["name"]):
                return f"command not found: {p['name']}"
        elif kind == "env":
            if not os.environ.get(p["name"]):
                return f"env var not set: {p['name']}"
        elif kind == "file":
            if not Path(os.path.expandvars(p["path"])).exists():
                return f"file not present: {p['path']}"
        elif kind == "ollama_model":
            # A fixture recorded against one embedding model cannot be
            # replayed with another: skip, never fail or re-record.
            try:
                listing = subprocess.run(["ollama", "list"], capture_output=True,
                                         text=True, timeout=10).stdout
            except (OSError, subprocess.TimeoutExpired):
                return "ollama not reachable"
            names = [ln.split()[0] for ln in listing.splitlines()[1:] if ln.strip()]
            if not any(n.split(":")[0] == p["name"] for n in names):
                return f"ollama model not installed: {p['name']}"
        else:
            return f"unknown prereq kind: {kind}"
    return None


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def run_setup(cmds: list[str], cwd: Path) -> None:
    for cmd in cmds:
        subprocess.run(["bash", "-c", cmd], cwd=cwd, check=True)


def run_teardown(cmds: list[str], cwd: Path) -> None:
    for cmd in cmds:
        # Teardown is best-effort; don't fail the test on cleanup hiccups.
        subprocess.run(["bash", "-c", cmd], cwd=cwd, check=False)


def rewrite_outputs(path: Path, observed: list[str]) -> bool:
    """Replace the fixture's dir:'out' frames, in order, with `observed`.
    In frames, timestamps and frame order are kept. Returns True when
    anything changed."""
    lines = path.read_text().splitlines()
    out_i = 0
    changed = False
    new_lines = []
    for line in lines:
        if not line.strip():
            new_lines.append(line)
            continue
        rec = json.loads(line)
        if rec.get("dir") == "out" and out_i < len(observed):
            if rec["frame"] != observed[out_i]:
                rec["frame"] = observed[out_i]
                changed = True
            out_i += 1
            line = json.dumps(rec, separators=(",", ":"))
        new_lines.append(line)
    if changed:
        path.write_text("\n".join(new_lines) + "\n")
    return changed


def match_by_id(expected: list[str], observed: list[str],
                masks: list[dict]) -> tuple[str, str]:
    """Compare responses paired by JSON-RPC id instead of position."""
    def by_id(frames: list[str]) -> dict:
        return {json.dumps(json.loads(f).get("id")): f for f in frames}
    exp, obs = by_id(expected), by_id(observed)
    if set(exp) != set(obs):
        return ("FAIL", f"response ids differ: expected {sorted(exp)}, got {sorted(obs)}")
    for key in sorted(exp):
        ok, diff = compare(exp[key], obs[key], masks)
        if not ok:
            return ("FAIL", f"response id {key} mismatch:\n{diff}")
    return ("PASS", "")


def replay_one(name: str, spec: dict, update: bool = False) -> tuple[str, str]:
    """Returns (status, detail) where status is PASS / FAIL / SKIP, or
    UPDATED when `update` rewrote the fixture's expected frames."""
    fixture_path = ROOT / spec["fixture"]
    if not fixture_path.exists():
        return ("FAIL", f"fixture not found: {fixture_path}")

    reason = check_prereqs(spec.get("prerequisites", []))
    if reason:
        return ("SKIP", reason)

    frames = load_fixture(fixture_path)
    in_frames = [f for f in frames if f.direction == "in"]
    out_frames = [f for f in frames if f.direction == "out"]
    if not in_frames:
        return ("FAIL", "no dir:'in' frames in fixture")
    if not out_frames:
        return ("FAIL", "no dir:'out' frames in fixture")

    server = spec["server"]
    args = [str(server)] + [
        os.path.expandvars(a) for a in spec.get("args", [])
    ]
    env = os.environ.copy()
    for k, v in spec.get("env", {}).items():
        env[k] = os.path.expandvars(v)

    setup_cmds = spec.get("setup", [])
    teardown_cmds = spec.get("teardown", [])
    masks = spec.get("output_masks", [])
    timeout = float(spec.get("timeout_s", DEFAULT_TIMEOUT_S))

    if setup_cmds:
        try:
            run_setup(setup_cmds, ROOT)
        except subprocess.CalledProcessError as exc:
            return ("FAIL", f"setup failed: {exc}")

    proc = subprocess.Popen(
        args,
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=env,
        bufsize=0,
    )
    try:
        assert proc.stdin and proc.stdout
        for f in in_frames:
            proc.stdin.write(f.body.encode() + b"\n")
            proc.stdin.flush()
        time.sleep(LINGER_S)
        proc.stdin.close()

        deadline = time.monotonic() + timeout
        observed: list[str] = []
        while len(observed) < len(out_frames):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            line = proc.stdout.readline()
            if not line:
                break
            observed.append(line.decode("utf-8", errors="replace").rstrip("\n"))

        if len(observed) < len(out_frames):
            return (
                "FAIL",
                f"expected {len(out_frames)} response frames, got {len(observed)}",
            )

        if update:
            # Re-record: the server's current answers become the expected
            # frames. Review the diff before committing it.
            return ("UPDATED", "") if rewrite_outputs(fixture_path, observed) \
                else ("PASS", "")
        if spec.get("match_by_id"):
            # Inline methods (ping, capability errors) can overtake
            # pooled tool calls, so pair responses by JSON-RPC id.
            return match_by_id([f.body for f in out_frames], observed, masks)
        for i, (exp, act) in enumerate(zip(out_frames, observed)):
            ok, diff = compare(exp.body, act, masks)
            if not ok:
                return ("FAIL", f"frame {i} mismatch:\n{diff}")
        return ("PASS", "")
    finally:
        proc.kill()
        proc.wait(timeout=2)
        run_teardown(teardown_cmds, ROOT)


def main() -> int:
    if not REGISTRY.exists():
        print(f"registry not found: {REGISTRY}", file=sys.stderr)
        return 1
    with REGISTRY.open() as f:
        registry = json.load(f)

    args = sys.argv[1:]
    update = "--update" in args
    args = [a for a in args if a != "--update"]
    name_filter = args[0] if args else None
    if update and not name_filter:
        print("--update needs a fixture name filter", file=sys.stderr)
        return 1

    pass_n = fail_n = skip_n = 0
    failures: list[tuple[str, str]] = []
    for entry in registry["fixtures"]:
        name = entry["name"]
        if name_filter and name_filter not in name:
            continue
        status, detail = replay_one(name, entry, update)
        if status == "UPDATED":
            print(f"  UPDATED  {name}  (review: git diff {entry['fixture']})")
            pass_n += 1
        elif status == "PASS":
            print(f"  PASS  {name}")
            pass_n += 1
        elif status == "SKIP":
            print(f"  SKIP  {name}  ({detail})")
            skip_n += 1
        else:
            print(f"  FAIL  {name}")
            print("        " + detail.replace("\n", "\n        "))
            fail_n += 1
            failures.append((name, detail))

    print()
    print(f"  {pass_n} passed, {skip_n} skipped, {fail_n} failed")
    return 1 if fail_n else 0


if __name__ == "__main__":
    sys.exit(main())
