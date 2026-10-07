"""Follow grw telemetry live: newest runtime/telemetry/*.jsonl file (default) or the named pipe."""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

PIPE = r"\\.\pipe\grw-telemetry"
STAGES = ("camera", "entities", "bones", "aim", "draw", "total")
DEFAULT_DIR = Path(__file__).resolve().parents[1] / "runtime" / "telemetry"


def latest_file(directory: Path) -> Path | None:
    directory = Path(directory)
    if not directory.is_dir():
        return None
    files = sorted(directory.glob("telemetry_*.jsonl"), key=lambda p: p.stat().st_mtime)
    return files[-1] if files else None


def matches(event: dict, types: list[str]) -> bool:
    return not types or event.get("type") in types


def format_event(e: dict) -> str:
    head = f"[{float(e.get('t', 0)) / 1000.0:6.1f}s] {e.get('type', '?')}"
    kind = e.get("type")
    if kind == "status":
        return f"{head} {e.get('event', '?')}: {e.get('detail', '')}"
    if kind == "frame_stats":
        parts = [f"frames={e.get('frames', 0)}", f"dropped={e.get('dropped_render', 0)}"]
        for stage in STAGES:
            if f"{stage}_p50" in e:
                parts.append(f"| {stage} p50={e[f'{stage}_p50']:.3f} p99={e[f'{stage}_p99']:.3f}")
        return f"{head} " + " ".join(parts) + " ms"
    rest = " ".join(f"{k}={v}" for k, v in e.items() if k not in ("type", "t", "frame"))
    frame = f" #{e['frame']}" if "frame" in e else ""
    return f"{head}{frame} {rest}".rstrip()


def emit(line: str, types: list[str]) -> None:
    line = line.strip()
    if not line:
        return
    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        print(f"[bad line] {line[:120]}", flush=True)
        return
    if matches(event, types):
        print(format_event(event), flush=True)


def follow_files(directory: Path, types: list[str], from_start: bool) -> None:
    current, handle = None, None
    while True:
        newest = latest_file(directory)
        if newest and newest != current:
            if handle:
                handle.close()
            current = newest
            handle = open(current, "r", encoding="utf-8")
            if not from_start:
                handle.seek(0, 2)
            from_start = True  # files that appear later are read from their start
            print(f"--- {current.name}", flush=True)
        if handle:
            for line in handle.readlines():
                emit(line, types)
        time.sleep(0.2)


def follow_pipe(types: list[str]) -> None:
    while True:
        try:
            with open(PIPE, "r", encoding="utf-8") as pipe:
                print("--- pipe connected", flush=True)
                for line in pipe:
                    emit(line, types)
        except OSError:
            time.sleep(1.0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    ap.add_argument("--pipe", action="store_true", help="read the named pipe instead of the files")
    ap.add_argument("--type", action="append", default=[], help="only these event types (repeatable)")
    ap.add_argument("--from-start", action="store_true", help="print the current file from its beginning")
    args = ap.parse_args()
    try:
        if args.pipe:
            follow_pipe(args.type)
        else:
            follow_files(args.dir, args.type, args.from_start)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
