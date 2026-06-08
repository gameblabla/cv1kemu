#!/usr/bin/env python3
"""Compare two capture directories produced by run_ddpsdoj_input_capture.py."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Sequence, Tuple


def read_manifest(path: Path) -> Dict[int, dict]:
    data: Dict[int, dict] = {}
    with (path / "manifest.jsonl").open("r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                rec = json.loads(line)
                data[int(rec["frame"])] = rec
    return data


def read_ppm(path: Path) -> Tuple[int, int, bytes]:
    blob = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(blob) and blob[pos] in b" \t\r\n":
            pos += 1
        if pos < len(blob) and blob[pos] == ord("#"):
            while pos < len(blob) and blob[pos] not in b"\r\n":
                pos += 1
            return token()
        start = pos
        while pos < len(blob) and blob[pos] not in b" \t\r\n":
            pos += 1
        return blob[start:pos]

    magic = token()
    if magic != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM, got {magic!r}")
    w = int(token())
    h = int(token())
    maxval = int(token())
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval}")
    if pos < len(blob) and blob[pos] in b" \t\r\n":
        pos += 1
    pixels = blob[pos:]
    if len(pixels) != w * h * 3:
        raise ValueError(f"{path}: expected {w*h*3} bytes, got {len(pixels)}")
    return w, h, pixels


def pixel_diff(a: Path, b: Path) -> Tuple[int, int, int]:
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    if (wa, ha) != (wb, hb):
        raise ValueError(f"dimension mismatch: {a} {wa}x{ha} vs {b} {wb}x{hb}")
    byte_diffs = 0
    pixel_diffs = 0
    abs_sum = 0
    for i in range(0, len(pa), 3):
        dr = abs(pa[i] - pb[i])
        dg = abs(pa[i + 1] - pb[i + 1])
        db = abs(pa[i + 2] - pb[i + 2])
        if dr or dg or db:
            pixel_diffs += 1
            byte_diffs += (1 if dr else 0) + (1 if dg else 0) + (1 if db else 0)
            abs_sum += dr + dg + db
    return pixel_diffs, byte_diffs, abs_sum


def main(argv: Sequence[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Compare two deterministic capture manifests/directories.")
    p.add_argument("baseline")
    p.add_argument("candidate")
    p.add_argument("--diff-pixels", action="store_true", help="also compute pixel-diff counts for changed frames")
    p.add_argument("--summary", default="compare_summary.json", help="summary JSON filename in candidate dir")
    args = p.parse_args(argv)

    base_dir = Path(args.baseline)
    cand_dir = Path(args.candidate)
    base = read_manifest(base_dir)
    cand = read_manifest(cand_dir)
    frames = sorted(set(base) | set(cand))
    changed: List[dict] = []
    missing: List[int] = []
    for frame in frames:
        if frame not in base or frame not in cand:
            missing.append(frame)
            continue
        brec = base[frame]
        crec = cand[frame]
        if brec.get("ppm_sha256") != crec.get("ppm_sha256"):
            rec = {
                "frame": frame,
                "baseline_sha256": brec.get("ppm_sha256"),
                "candidate_sha256": crec.get("ppm_sha256"),
            }
            if args.diff_pixels:
                pd, bd, asum = pixel_diff(base_dir / brec["ppm"], cand_dir / crec["ppm"])
                rec.update({"pixel_diffs": pd, "byte_diffs": bd, "abs_channel_diff_sum": asum})
            changed.append(rec)

    summary = {
        "baseline": str(base_dir),
        "candidate": str(cand_dir),
        "frames_compared": len(frames) - len(missing),
        "changed_count": len(changed),
        "changed": changed,
        "missing_frames": missing,
    }
    out = cand_dir / args.summary
    out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 1 if changed or missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
