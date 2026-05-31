# DDPSDOJ v23 progress

v23 remains a boot/probe emulator, not a working title-screen emulator.

## Default probe

Command:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --probe-title --run-frames 10000 --dump-ppm docs/DDPSDOJ_V23_PROGRESS.ppm
```

The default path still stops in the copied-RAM status loop around `0c1fb3dc-0c1fb42c`. The loop reads `@(8,R10)` where the late-state `R10` is a `0x4bxxxxxx` high P0 virtual pointer; the current coarse alias maps it to payload-looking work RAM.

## v23 changes

* Fixed SH `TAS.B @Rn` semantics: T now reflects whether the original byte was zero before bit 7 is set.
* NAND status now returns `0xc0` for ready + not write-protected instead of only `0x40` ready.
* Added legacy NAND `01h` read-area-B address-phase handling, separate from `00h` and `50h`.
* Added `--compact-400-alias`, an off-by-default diagnostic address-translation experiment that maps `0x40000000-0x4fffffff` through a 2 MiB work-RAM aperture. It gets past the conservative `0c1fb3dc` loop but later diverges into invalid copied-RAM/payload execution, so it is not enabled by default.
* Save states are now `CV1KSS23`.

## Verified default result

See `VERIFY_DDPSDOJ_V23.txt`.

## Verified experiments

See `VERIFY_DDPSDOJ_V23_COMPACT400.txt` and `VERIFY_DDPSDOJ_V23_EXPERIMENTAL_8000.txt`.

## Current blocker

The conservative blocker is still upstream state feeding a high virtual work-RAM pointer into the copied-RAM status loop. The most likely remaining gaps are SH7709S TLB/cache/DMAC coherency, NAND bad-block/OOB reconstruction, and real IRQ/timer behavior.
