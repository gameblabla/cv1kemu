#!/usr/bin/env python3
"""Deterministic DDPSDOJ input/screenshot capture helper for cv1k_sandbox.

The helper runs from reset for each requested screenshot frame.  That is slower
than taking screenshots from one interactive run, but it makes every output
independently reproducible from the ROM, binary, command-line flags, and the
same generated input-tap list.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

Event = Tuple[str, int, int]


def positive_int(text: str) -> int:
    v = int(text, 0)
    if v <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return v


def nonnegative_float(text: str) -> float:
    v = float(text)
    if v < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return v


def frame_at(seconds: float, fps: int) -> int:
    return int(round(seconds * fps))


def build_events(args: argparse.Namespace) -> List[Event]:
    total_frames = frame_at(args.total_seconds, args.fps)
    events: List[Event] = []

    coin_frame = frame_at(args.coin_at, args.fps)
    if 0 <= coin_frame <= total_frames:
        events.append(("coin1", coin_frame, args.coin_frames))

    start_first = frame_at(args.start_first, args.fps)
    start_step = frame_at(args.start_every, args.fps)
    f = start_first
    while f <= total_frames:
        events.append(("p1_start", f, args.start_frames))
        f += start_step

    button_first = frame_at(args.button_first, args.fps)
    button_step = frame_at(args.button_every, args.fps)
    f = button_first
    toggle = 0
    while f <= total_frames:
        events.append(("p1_b1" if toggle == 0 else "p1_b2", f, args.button_frames))
        toggle ^= 1
        f += button_step

    events.sort(key=lambda e: (e[1], e[0]))
    if len(events) > args.max_events:
        raise SystemExit(
            f"generated {len(events)} input events, but this cv1k_sandbox build accepts "
            f"only {args.max_events}; reduce --total-seconds or increase intervals"
        )
    return events


def write_events(path: Path, events: Sequence[Event], fps: int) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("# name,start_frame,duration_frames,start_seconds\n")
        for name, start, dur in events:
            f.write(f"{name},{start},{dur},{start / fps:.3f}\n")


def segment_events(events: Sequence[Event], start_frame: int, end_frame: int) -> List[Event]:
    out: List[Event] = []
    for name, ev_start, dur in events:
        ev_end = ev_start + dur
        if ev_start < end_frame and ev_end > start_frame:
            seg_start = max(ev_start, start_frame) - start_frame
            seg_end = min(ev_end, end_frame) - start_frame
            if seg_end > seg_start:
                out.append((name, seg_start, seg_end - seg_start))
    return out


def tap_args_from_events(events: Sequence[Event]) -> List[str]:
    out: List[str] = []
    for name, start, dur in events:
        out.extend(["--tap-input", f"{name},{start},{dur}"])
    return out


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def maybe_convert_to_png(ppm: Path, png: Path) -> bool:
    tool = shutil.which("magick") or shutil.which("convert")
    if tool is None:
        return False
    cmd = [tool, str(ppm), str(png)] if Path(tool).name == "convert" else [tool, str(ppm), str(png)]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return png.exists() and png.stat().st_size > 0
    except (OSError, subprocess.CalledProcessError):
        return False


def main(argv: Sequence[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="Run deterministic DDPSDOJ coin/start/A/B input taps and dump screenshots every N seconds."
    )
    p.add_argument("--binary", default="./cv1k_sandbox", help="path to cv1k_sandbox")
    p.add_argument("--romset", required=True, help="path to ddpsdoj.zip")
    p.add_argument("--out-dir", required=True, help="output directory for PPM/PNG captures and manifest")
    p.add_argument("--model", default="d", choices=["b", "d"], help="CV1000 model, default d")
    p.add_argument("--fps", type=positive_int, default=60, help="frame/second conversion, default 60")
    p.add_argument("--total-seconds", type=nonnegative_float, default=50.0, help="capture duration, default 50")
    p.add_argument("--screenshot-interval", type=nonnegative_float, default=2.0, help="seconds between screenshots")
    p.add_argument("--coin-at", type=nonnegative_float, default=27.0, help="insert coin1 at this time")
    p.add_argument("--coin-frames", type=positive_int, default=18, help="coin tap duration")
    p.add_argument("--start-first", type=nonnegative_float, default=29.0, help="first p1_start tap time")
    p.add_argument("--start-every", type=nonnegative_float, default=5.0, help="repeat p1_start every N seconds")
    p.add_argument("--start-frames", type=positive_int, default=12, help="start tap duration")
    p.add_argument("--button-first", type=nonnegative_float, default=31.0, help="first A/B tap time")
    p.add_argument("--button-every", type=nonnegative_float, default=2.0, help="alternate A/B every N seconds")
    p.add_argument("--button-frames", type=positive_int, default=12, help="A/B tap duration")
    p.add_argument("--max-events", type=positive_int, default=64, help="sandbox MAX_INPUT_SCRIPT_EVENTS")
    p.add_argument("--mode", choices=["series", "reset", "chain"], default="series", help="series runs once using emulator multi-dump support; reset reruns; chain uses save states")
    p.add_argument("--keep-states", action="store_true", help="keep intermediate chain-mode save states")
    p.add_argument("--backend", choices=["c23", "interp"], default="c23", help="CPU backend for captures; default c23")
    p.add_argument("--strict-cache-ops", action="store_true", help="add --strict-cache-ops; use --backend interp on builds where c23+strict is unstable")
    p.add_argument("--extra-arg", action="append", default=[], help="extra emulator argument; repeat for each token")
    p.add_argument("--no-png", action="store_true", help="do not try ImageMagick PPM-to-PNG conversion")
    p.add_argument("--raw-dump", action="store_true", help="use --dump-ppm instead of --dump-display-ppm")
    args = p.parse_args(argv)

    binary = Path(args.binary)
    romset = Path(args.romset)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not binary.exists():
        raise SystemExit(f"binary not found: {binary}")
    if not romset.exists():
        raise SystemExit(f"romset not found: {romset}")
    binary = binary.resolve()
    romset = romset.resolve()

    events = build_events(args)
    write_events(out_dir / "input_taps.txt", events, args.fps)

    total_frames = frame_at(args.total_seconds, args.fps)
    step = frame_at(args.screenshot_interval, args.fps)
    if step <= 0:
        raise SystemExit("--screenshot-interval must be greater than zero")
    capture_frames = list(range(0, total_frames + 1, step))
    if capture_frames[-1] != total_frames:
        capture_frames.append(total_frames)

    manifest_path = out_dir / "manifest.jsonl"
    dump_flag = "--dump-ppm" if args.raw_dump else "--dump-display-ppm"
    backend_args = ["--c23-jit"] if args.backend == "c23" else ["--cpu-backend", "interp"]
    cache_args = ["--dcache", "--mame-speedup", "--vblank-irq-and-tick"]
    if args.strict_cache_ops:
        cache_args.append("--strict-cache-ops")
    common = [
        str(binary), "--model", args.model, "--romset", str(romset),
        *backend_args, *cache_args,
        *args.extra_arg,
    ]

    if args.mode == "series":
        if total_frames % step != 0:
            raise SystemExit("series mode requires --total-seconds to be an exact multiple of --screenshot-interval")
        series_flag = "--dump-ppm-series-dir" if args.raw_dump else "--dump-display-ppm-series-dir"
        cmd = [*common, *tap_args_from_events(events), "--run-frames", str(total_frames), series_flag, str(out_dir), "--dump-series-every", str(step)]
        print(f"capture series frames=0..{total_frames} every={step}", flush=True)
        run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run.returncode != 0:
            sys.stderr.write(run.stdout)
            sys.stderr.write(run.stderr)
            raise SystemExit(f"emulator failed with exit code {run.returncode}; this binary may not support --dump-*-series-dir, use --mode reset")
        with manifest_path.open("w", encoding="utf-8") as manifest:
            for frame in capture_frames:
                ppm = out_dir / f"frame_{frame:06d}.ppm"
                png = out_dir / f"frame_{frame:06d}.png"
                if not ppm.exists():
                    raise SystemExit(f"expected capture missing: {ppm}")
                rec = {
                    "frame": frame,
                    "seconds": round(frame / args.fps, 6),
                    "ppm": ppm.name,
                    "ppm_sha256": sha256_file(ppm),
                    "command": cmd,
                    "mode": args.mode,
                }
                if not args.no_png and maybe_convert_to_png(ppm, png):
                    rec["png"] = png.name
                    rec["png_sha256"] = sha256_file(png)
                manifest.write(json.dumps(rec, sort_keys=True) + "\n")
        print(f"wrote {manifest_path}")
        print(f"wrote {out_dir / 'input_taps.txt'}")
        return 0

    state_dir = out_dir / "states"
    if args.mode == "chain":
        state_dir.mkdir(exist_ok=True)

    with manifest_path.open("w", encoding="utf-8") as manifest:
        previous_frame = 0
        previous_state: Path | None = None
        for idx, frame in enumerate(capture_frames):
            ppm = out_dir / f"frame_{frame:06d}.ppm"
            png = out_dir / f"frame_{frame:06d}.png"
            if args.mode == "reset":
                run_events = events
                run_frames = frame
                state_args: List[str] = []
            else:
                run_events = segment_events(events, previous_frame, frame)
                run_frames = frame - previous_frame
                state_args = []
                if previous_state is not None:
                    state_args.extend(["--load-state", str(previous_state)])
                next_state = state_dir / f"state_{frame:06d}.bin"
                state_args.extend(["--save-state", str(next_state)])

            cmd = [*common, *state_args, *tap_args_from_events(run_events), "--run-frames", str(run_frames), dump_flag, str(ppm)]
            print(f"capture frame={frame} seconds={frame / args.fps:.3f}", flush=True)
            run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if run.returncode != 0:
                sys.stderr.write(run.stdout)
                sys.stderr.write(run.stderr)
                raise SystemExit(f"emulator failed at frame {frame} with exit code {run.returncode}")
            rec = {
                "frame": frame,
                "seconds": round(frame / args.fps, 6),
                "ppm": ppm.name,
                "ppm_sha256": sha256_file(ppm),
                "command": cmd,
                "mode": args.mode,
            }
            if args.mode == "chain":
                rec["run_frames_this_segment"] = run_frames
                rec["segment_start_frame"] = previous_frame
                rec["segment_input_events"] = [f"{n},{s},{d}" for n, s, d in run_events]
                if previous_state is not None and not args.keep_states:
                    previous_state.unlink(missing_ok=True)
                previous_state = next_state
                previous_frame = frame
            if not args.no_png and maybe_convert_to_png(ppm, png):
                rec["png"] = png.name
                rec["png_sha256"] = sha256_file(png)
            manifest.write(json.dumps(rec, sort_keys=True) + "\n")
            manifest.flush()

    if args.mode == "chain" and not args.keep_states:
        shutil.rmtree(state_dir, ignore_errors=True)

    print(f"wrote {manifest_path}")
    print(f"wrote {out_dir / 'input_taps.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
