#!/usr/bin/env python3
"""
color_replay.py — Host-side color pipeline replay and regression testing.

Reads raw sensor captures from a JSON file (see tests/host/capture_samples.json
for the format) and feeds them to the C++ host replay tools
(color_replay_inspect or color_replay_batch) that must already be built under
the host build directory.

Usage
-----
Single-capture inspection (verbose):
  python3 scripts/color_replay.py inspect --json tests/host/capture_samples.json \\
          --id green_wall_paint \\
          --build /tmp/shade_sayer_host_build

Batch / regression run (CSV output + pass/fail):
  python3 scripts/color_replay.py batch --json tests/host/capture_samples.json \\
          --build /tmp/shade_sayer_host_build

  # Save CSV to a file:
  python3 scripts/color_replay.py batch --json tests/host/capture_samples.json \\
          --build /tmp/shade_sayer_host_build \\
          --output results.csv

  # Exit non-zero when any expected category fails (useful for CI):
  python3 scripts/color_replay.py batch --json tests/host/capture_samples.json \\
          --build /tmp/shade_sayer_host_build --check

Text-format conversion only (generate a .cfg file for color_replay_batch):
  python3 scripts/color_replay.py dump --json tests/host/capture_samples.json

Environment
-----------
If --build is omitted the script looks for the executables in the following
locations in order:
  1. BUILD_DIR environment variable
  2. /tmp/shade_sayer_host_build
  3. build/         (relative to repo root)

Input JSON format
-----------------
See tests/host/capture_samples.json for a fully-annotated example.  The
minimal schema is:

  {
    "schema_version": 1,
    "captures": [
      {
        "id": "my_capture",
        "led_enabled": true,
        "raw": {
          "x": 11517952, "y": 12179200, "z": 7376128,
          "ir": 249344,  "clear": 8603648,
          "gain": 4, "integration_ms": 100
        },
        "expected": {          // optional
          "category": "Green"  // compared against pipeline output
        }
      }
    ]
  }

For more details see docs/replay-harness.md.
"""

import argparse
import json
import os
import subprocess
import sys


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _find_binary(name: str, build_dir: str | None) -> str:
    """Return the absolute path to a built host binary, or raise FileNotFoundError."""
    candidates = []
    if build_dir:
        candidates.append(os.path.join(build_dir, name))
    env_dir = os.environ.get("BUILD_DIR")
    if env_dir:
        candidates.append(os.path.join(env_dir, name))
    candidates += [
        f"/tmp/shade_sayer_host_build/{name}",
        os.path.join(os.path.dirname(__file__), "..", "build", name),
    ]
    for path in candidates:
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return os.path.abspath(path)
    searched = ", ".join(dict.fromkeys(candidates))  # dedup while preserving order
    raise FileNotFoundError(
        f"Host binary '{name}' not found. Build it first with:\n"
        f"  cmake -S host -B /tmp/shade_sayer_host_build\n"
        f"  cmake --build /tmp/shade_sayer_host_build\n"
        f"Searched: {searched}"
    )


def _load_captures(json_path: str) -> list[dict]:
    """Load and validate captures from a JSON file."""
    with open(json_path) as fh:
        data = json.load(fh)
    captures = data.get("captures")
    if not isinstance(captures, list):
        raise ValueError(f"{json_path}: missing or invalid 'captures' list")
    return captures


def _capture_to_text_line(cap: dict) -> str | None:
    """
    Convert a capture dict to one line of the space-delimited text format
    consumed by color_replay_inspect and color_replay_batch.

    Returns None and prints a warning if required fields are missing.
    """
    raw = cap.get("raw", {})
    required = ("x", "y", "z", "ir", "clear", "gain", "integration_ms")
    missing = [k for k in required if k not in raw]
    if missing:
        print(f"WARNING: capture '{cap.get('id', '?')}' missing raw fields: {missing}",
              file=sys.stderr)
        return None

    cap_id = cap.get("id", "unknown")
    led    = 1 if cap.get("led_enabled", True) else 0
    x, y, z   = raw["x"],   raw["y"],   raw["z"]
    ir, clear  = raw["ir"],  raw["clear"]
    gain       = raw["gain"]
    int_ms     = raw["integration_ms"]

    # expected_category is appended as the 10th field (batch format) or
    # as the 9th field after led_enabled (inspect format — treated as label).
    expected = cap.get("expected", {})
    cat = expected.get("category", "")

    # Batch line: id x y z ir clear gain int_ms led [expected_category]
    parts = [cap_id, x, y, z, ir, clear, gain, int_ms, led]
    if cat:
        parts.append(cat)
    return " ".join(str(v) for v in parts)


# ---------------------------------------------------------------------------
# Sub-commands
# ---------------------------------------------------------------------------

def cmd_inspect(args: argparse.Namespace) -> int:
    """Run single-capture verbose inspection."""
    binary = _find_binary("color_replay_inspect", args.build)
    captures = _load_captures(args.json)

    # Select the capture by id or default to the first one.
    if args.id:
        target = next((c for c in captures if c.get("id") == args.id), None)
        if target is None:
            print(f"ERROR: capture id '{args.id}' not found in {args.json}",
                  file=sys.stderr)
            return 1
    else:
        target = captures[0]
        print(f"(no --id specified, using first capture: '{target.get('id')}')",
              file=sys.stderr)

    raw = target.get("raw", {})
    led = 1 if target.get("led_enabled", True) else 0

    # Build CLI args for color_replay_inspect.
    cli_args = [
        binary,
        str(raw.get("x",   0)),
        str(raw.get("y",   0)),
        str(raw.get("z",   0)),
        str(raw.get("ir",  0)),
        str(raw.get("clear", 0)),
        str(raw.get("gain", 0)),
        str(raw.get("integration_ms", 100)),
        str(led),
        str(target.get("id", "")),
    ]

    result = subprocess.run(cli_args)
    return result.returncode


def cmd_batch(args: argparse.Namespace) -> int:
    """Run batch/regression over all captures and output CSV."""
    binary = _find_binary("color_replay_batch", args.build)
    captures = _load_captures(args.json)

    lines = []
    for cap in captures:
        line = _capture_to_text_line(cap)
        if line:
            lines.append(line)

    if not lines:
        print("ERROR: no valid captures found in the JSON file.", file=sys.stderr)
        return 2

    batch_input = "\n".join(lines) + "\n"

    result = subprocess.run(
        [binary],
        input=batch_input,
        capture_output=False if args.output == "-" else True,
        text=True,
    )

    if args.output and args.output != "-":
        with open(args.output, "w") as fh:
            fh.write(result.stdout or "")
        print(f"CSV written to: {args.output}", file=sys.stderr)

    # Mirror stderr from the subprocess.
    if result.stderr:
        sys.stderr.write(result.stderr)

    if args.check and result.returncode != 0:
        print("REGRESSION CHECK FAILED — one or more captures did not match the"
              " expected category.", file=sys.stderr)
        return result.returncode

    return result.returncode if args.check else 0


def cmd_dump(args: argparse.Namespace) -> int:
    """Print captures in the space-delimited text format (for use with color_replay_batch)."""
    captures = _load_captures(args.json)

    for cap in captures:
        desc = cap.get("description", "")
        if desc:
            print(f"# {desc}")
        line = _capture_to_text_line(cap)
        if line:
            print(line)

    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Color pipeline replay and regression testing.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--build",
        metavar="DIR",
        help="Path to the cmake host build directory (default: /tmp/shade_sayer_host_build).",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    # --- inspect ---
    p_inspect = sub.add_parser("inspect",
                                help="Run single-capture verbose inspection.")
    p_inspect.add_argument("--json", required=True, metavar="FILE",
                            help="Path to the captures JSON file.")
    p_inspect.add_argument("--id", metavar="ID",
                            help="ID of the capture to inspect (default: first capture).")

    # --- batch ---
    p_batch = sub.add_parser("batch",
                              help="Run batch/regression over all captures (CSV output).")
    p_batch.add_argument("--json", required=True, metavar="FILE",
                          help="Path to the captures JSON file.")
    p_batch.add_argument("--output", metavar="FILE", default="-",
                          help="Write CSV output to FILE instead of stdout.")
    p_batch.add_argument("--check", action="store_true",
                          help="Exit non-zero if any expected category fails (CI mode).")

    # --- dump ---
    p_dump = sub.add_parser("dump",
                             help="Print captures as space-delimited text (no C++ binary needed).")
    p_dump.add_argument("--json", required=True, metavar="FILE",
                         help="Path to the captures JSON file.")

    args = parser.parse_args()

    dispatch = {
        "inspect": cmd_inspect,
        "batch":   cmd_batch,
        "dump":    cmd_dump,
    }
    return dispatch[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
