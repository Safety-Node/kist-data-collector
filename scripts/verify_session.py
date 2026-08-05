#!/usr/bin/env python3
"""Verify a recorded session's integrity.

For every <camera>/{color,depth} stream in the session dir, checks that:
  - index rows are seq-contiguous (reports wire gaps / duplicates)
  - offsets are monotonic, offset+size chains exactly to the next row,
    and the last row ends exactly at the blob file size

Usage: verify_session.py <session_dir>

Exit 0 = all checks passed. Wire gaps (frames lost before arrival) are
reported in the summary line but don't fail the run — they're upstream of
the recorder; dropped/write_errors in meta.yaml cover the recorder itself.
"""

import argparse
import csv
import sys
from pathlib import Path

STREAMS = {
    "color": ("color.h264", "color.idx.csv"),
    "depth": ("depth.rvl", "depth.idx.csv"),
}


def check_stream(cam_dir: Path, stream: str) -> list[str]:
    blob_path, idx_path = (cam_dir / n for n in STREAMS[stream])
    tag = f"{cam_dir.name}/{stream}"
    if not blob_path.exists() or not idx_path.exists():
        return [f"{tag}: missing {blob_path.name} or {idx_path.name}"]

    errors: list[str] = []
    rows = list(csv.DictReader(idx_path.open()))
    blob_size = blob_path.stat().st_size

    if not rows:
        return [f"{tag}: empty index"]

    prev_seq, prev_end = None, 0
    gaps = dupes = 0
    for row in rows:
        seq, off, size = int(row["seq"]), int(row["offset"]), int(row["size"])
        if off != prev_end:
            errors.append(f"{tag}: seq {seq} offset {off} != previous end {prev_end}")
        prev_end = off + size
        if prev_seq is not None:
            if seq <= prev_seq:
                dupes += 1
            elif seq != prev_seq + 1:
                gaps += seq - prev_seq - 1
        prev_seq = seq
    if prev_end != blob_size:
        errors.append(f"{tag}: index ends at {prev_end} but blob is {blob_size} bytes")
    if dupes:
        errors.append(f"{tag}: {dupes} out-of-order/duplicate seq rows")

    first, last = int(rows[0]["seq"]), int(rows[-1]["seq"])
    note = f"{tag}: {len(rows)} frames (seq {first}..{last}), {gaps} wire gap(s), {blob_size/1e6:.1f} MB"
    print(("FAIL  " if errors else "ok    ") + note)
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_dir", type=Path)
    args = ap.parse_args()

    cam_dirs = sorted(d for d in args.session_dir.iterdir() if d.is_dir())
    if not cam_dirs:
        print(f"no camera dirs under {args.session_dir}", file=sys.stderr)
        return 1

    errors: list[str] = []
    for cam_dir in cam_dirs:
        for stream in STREAMS:
            errors += check_stream(cam_dir, stream)

    if errors:
        print(f"\n{len(errors)} error(s):")
        for e in errors:
            print("  " + e)
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
