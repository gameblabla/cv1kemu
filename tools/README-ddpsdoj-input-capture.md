# DDPSDOJ deterministic input/screenshot regression capture

These tools generate a repeatable headless input script and run `cv1k_sandbox`
to fixed screenshot frames.  The default `--mode series` uses the emulator's multi-dump option and runs the
whole test once, which is fast and deterministic.  Use `--mode reset` to re-run
from reset for every screenshot frame when you want each image to be
independently reproducible without relying on the multi-dump path.  `--mode
chain` advances by save state between captures, but it is usually slower because
the complete machine state is large.

Default input schedule, assuming 60 FPS:

- insert one `coin1` tap with key/button 5 at 27 seconds;
- press `p1_start` (key/button 1) every 5 seconds starting at 29 seconds;
- alternate `p1_b1` / `p1_b2` (A/B) every 2 seconds starting at 31 seconds;
- dump a screenshot every 2 seconds.

The helper defaults to the fast deterministic DDPSDOJ path:

```sh
--model d --c23-jit --dcache --mame-speedup --vblank-irq-and-tick
```

`--strict-cache-ops` is available as a tool option.  The C23 JIT now defers
code-cache destruction when a strict cache operation invalidates the currently
executing generated block, so `--c23-jit --dcache --strict-cache-ops` is valid
for deterministic regression runs.  Use `--backend interp --strict-cache-ops`
when you want to remove JIT-specific behavior from a cache-control test.

## Capture one build

The source also exposes direct emulator options used by the helper:

```sh
--dump-display-ppm-series-dir /tmp/captures --dump-series-every 120
```

For raw, non-frontend-oriented captures use `--dump-ppm-series-dir`.


From the project root after building `cv1k_sandbox`:

```sh
python3 tools/run_ddpsdoj_input_capture.py \
  --binary ./cv1k_sandbox \
  --romset /path/to/ddpsdoj.zip \
  --out-dir /tmp/ddpsdoj_candidate \
  --total-seconds 50
```

Outputs:

- `input_taps.txt`: generated event list in `name,start_frame,duration,start_seconds` form;
- `manifest.jsonl`: one JSON record per screenshot, including SHA-256 hashes and the full command;
- `frame_*.ppm`: deterministic screenshot dumps;
- `frame_*.png`: optional PNG conversions if ImageMagick `magick` or `convert` is available.

## Compare two builds

```sh
python3 tools/compare_cv1k_captures.py \
  /tmp/ddpsdoj_baseline \
  /tmp/ddpsdoj_candidate \
  --diff-pixels
```

The comparator exits with code `0` only when every screenshot hash matches and
no frames are missing.  If hashes differ, it writes `compare_summary.json` in the
candidate capture directory with per-frame pixel-difference counts.
