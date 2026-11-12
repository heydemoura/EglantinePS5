#!/usr/bin/env python3
"""Report a fastmem64 window A/B from the console's `heap.log`.

`EDEN_FASTMEM64=1` turns the 64-bit window on. Its cost and its refusals are written to
`/data/eglantineps5/logs/heap.log`: one `EDEN_VULKAN_FRAME` line per 5 s window and one
`EDEN_FASTMEM64` line with the counters. This tool compares one log without the window
against one with it and prints the table and the refusal breakdown, so a run can be read
without opening the log by hand.

    python3 tools/fastmem64-report.py heap-default.log heap-window.log
    python3 tools/fastmem64-report.py heap-default.log heap-window.log --markdown out.md

The frame lines only exist with the FPS overlay on (Settings > Video > FPS overlay).
The first two 5 s windows of each log are dropped: they cover the first scene's shader
build, not gameplay. The last `EDEN_FASTMEM64` line of the window log is used, because
its counters are cumulative.
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys

FRAME = re.compile(
    r"EDEN_VULKAN_FRAME frames=(\d+) seconds=([\d.]+) fps=([\d.]+) worst_ms=([\d.]+) total=(\d+)")
PHASE = re.compile(r"misaligned_phase=(\d+)/(\d+)/(\d+)")
AREAS = re.compile(r"areas=([0-9a-f:,\s]*)")

# Refusals of a whole 2 MiB region, and of a 16 KiB piece, as `Window64Report` names them.
REGION_REFUSALS = [
    ("no_region", "no slot at the address"),
    ("partial", "region not fully mapped"),
    ("inconsistent", "pages disagree on the backing"),
    ("blocked", "page blocked"),
    ("blocked_write", "page blocked for a write"),
    ("unaligned", "slot off its alignment"),
    ("slot", "slot refused the alias"),
    ("piece_cap", "page-table cap"),
]
PIECE_REFUSALS = [
    ("unmapped", "piece has an unmapped page"),
    ("blocked", "piece blocked"),
    ("scattered", "piece scattered in the backing"),
    ("misaligned", "piece 16 KiB-aligned in the backing? no"),
    ("slot", "slot refused the piece"),
]


class Session:
    def __init__(self, label: str) -> None:
        self.label = label
        self.windows: list[dict] = []
        self.counts: dict[str, int] = {}
        self.phases: tuple[int, int, int] = (0, 0, 0)
        self.areas: dict[str, int] = {}

    @property
    def steady(self) -> list[dict]:
        return self.windows[2:] if len(self.windows) > 3 else self.windows

    @property
    def fps_weighted(self) -> float | None:
        steady = self.steady
        seconds = sum(w["seconds"] for w in steady)
        return round(sum(w["frames"] for w in steady) / seconds, 2) if seconds else None

    @property
    def fps_median(self) -> float | None:
        steady = self.steady
        return round(statistics.median(w["fps"] for w in steady), 2) if steady else None

    @property
    def fps_min(self) -> float | None:
        steady = self.steady
        return round(min(w["fps"] for w in steady), 2) if steady else None

    @property
    def worst_frame_median(self) -> float | None:
        steady = self.steady
        return round(statistics.median(w["worst_ms"] for w in steady), 1) if steady else None


def read(path: str, label: str) -> Session:
    session = Session(label)
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            frame = FRAME.search(line)
            if frame:
                session.windows.append({
                    "frames": int(frame.group(1)),
                    "seconds": float(frame.group(2)),
                    "fps": float(frame.group(3)),
                    "worst_ms": float(frame.group(4)),
                })
                continue
            if "EDEN_FASTMEM64" not in line:
                continue
            counters = dict(re.findall(r"([a-z_]+)=(\d+)", line))
            if counters:
                session.counts = {key: int(value) for key, value in counters.items()}
            phase = PHASE.search(line)
            if phase:
                session.phases = tuple(int(value) for value in phase.groups())
            areas = AREAS.search(line)
            if areas:
                for item in areas.group(1).split(","):
                    if ":" in item:
                        key, value = item.split(":")
                        session.areas[key.strip()] = int(value)
    return session


def share(part: int, total: int) -> str:
    return f"{100.0 * part / total:.0f}%" if total else "-"


def fps_table(default: Session, window: Session) -> list[str]:
    rows = [
        "| Session | Windows | FPS weighted | FPS median | FPS min | Worst frame (median) |",
        "|---|---|---|---|---|---|",
        f"| {default.label} | {len(default.steady)} | {default.fps_weighted} | "
        f"{default.fps_median} | {default.fps_min} | {default.worst_frame_median} ms |",
        f"| {window.label} | {len(window.steady)} | {window.fps_weighted} | "
        f"{window.fps_median} | {window.fps_min} | {window.worst_frame_median} ms |",
    ]
    if default.fps_weighted and window.fps_weighted:
        delta = 100.0 * (window.fps_weighted - default.fps_weighted) / default.fps_weighted
        verdict = "gained" if delta >= 3 else ("lost" if delta <= -3 else "at parity (within noise)")
        rows += ["", f"Window vs default: **{delta:+.1f}%** - {verdict}"]
    return rows


def refusal_tables(window: Session) -> list[str]:
    counts = window.counts
    region_total = sum(counts.get(key, 0) for key, _ in REGION_REFUSALS)
    piece_total = sum(counts.get(key, 0) for key, _ in PIECE_REFUSALS)
    rows = ["| Region refusal | Count | Share |", "|---|---|---|"]
    for key, text in REGION_REFUSALS:
        value = counts.get(key, 0)
        if value:
            rows.append(f"| `{key}` - {text} | {value:,} | {share(value, region_total)} |")
    rows += ["", "| Piece refusal | Count | Share |", "|---|---|---|"]
    for key, text in PIECE_REFUSALS:
        value = counts.get(key, 0)
        if value:
            rows.append(f"| `{key}` - {text} | {value:,} | {share(value, piece_total)} |")
    rows += [
        "",
        f"Served: whole regions **{counts.get('whole', 0):,}** - pieces **{counts.get('pieces', 0):,}** - "
        f"read-only **{counts.get('read_only', 0):,}** - promoted **{counts.get('promoted', 0):,}**",
        f"Aliases: made **{counts.get('made', 0):,}** - dropped **{counts.get('dropped', 0):,}** - "
        f"refused **{counts.get('refused', 0):,}**",
    ]
    if any(window.phases):
        first, second, third = window.phases
        total = first + second + third
        rows += [
            "",
            f"Piece misalignment (4 KiB pages): phase 1 = {first:,} ({share(first, total)}), "
            f"phase 2 = {second:,} ({share(second, total)}), phase 3 = {third:,} ({share(third, total)})",
        ]
    if window.areas:
        top = sorted(window.areas.items(), key=lambda item: -item[1])[:5]
        rows += ["", "4 GiB areas losing the most pieces: " +
                 ", ".join(f"`{key}` = {value:,}" for key, value in top)]
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("default_log", help="heap.log of the run without EDEN_FASTMEM64")
    parser.add_argument("window_log", help="heap.log of the run with EDEN_FASTMEM64=1")
    parser.add_argument("--markdown", metavar="PATH", help="write the report here as well")
    parser.add_argument("--default-label", default="Default (no window)")
    parser.add_argument("--window-label", default="`EDEN_FASTMEM64=1`")
    args = parser.parse_args()

    default = read(args.default_log, args.default_label)
    window = read(args.window_log, args.window_label)

    if not default.windows or not window.windows:
        print("warning: no EDEN_VULKAN_FRAME line in one of the logs - the FPS overlay must be "
              "on (Settings > Video > FPS overlay) during the run", file=sys.stderr)

    lines = ["## fastmem64 window, measured on the console", ""]
    lines += fps_table(default, window)
    lines += ["", "### Why the window refused", ""]
    lines += refusal_tables(window)
    if window.counts:
        refused = window.counts.get("misaligned", 0)
        if refused and window.counts.get("pieces"):
            lines += ["", f"Misaligned pieces are {share(refused, window.counts['pieces'] + refused)} "
                          f"of all piece accesses the window saw."]
    lines += [
        "",
        "### Method",
        f"- Default: {len(default.windows)} windows of 5 s; window: {len(window.windows)} windows of 5 s.",
        "- First two windows of each session dropped (shader build, not gameplay).",
        "- Caches kept between sessions; arms interleaved (A B A B) where possible.",
        "- The window's first session learns the demoted-site list, so measure the second one.",
    ]

    report = "\n".join(lines)
    print(report)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8", newline="") as handle:
            handle.write(report + "\n")
        print(f"\nwrote {args.markdown}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
