# DDPSDOJ v24 progress

v24 still does not reach the real DDPSDOJ title screen. This revision is a conservative accuracy/diagnostics pass rather than a risky bypass pass.

## What changed

- Checked the MAME `cv1k.cpp` and `sh7709s` sources at commit `acad9ca235f4026b1765f62fec340f6d95b2e9ab` for the requested CPU/cache dependency details.
- Added an opt-in `--mame-cache-meta` diagnostic path that tracks MAME-style SH7709S cache metadata: 16 KiB cache, 16-byte lines, 4-way set associativity, LRU updates, dirty-bit tracking, and dirty-eviction counts.
- Kept the existing execution cache behavior unchanged by default. A direct attempt to force the older sandbox instruction-cache line size from 0x200 to 16 bytes regressed the boot path, so it was not kept.
- Bumped save states to `CV1KSS24` and included the new cache metadata/counters in save/load.
- Added v24 verification logs and a 320x240 diagnostic framebuffer capture.

## Verified default run

Command:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --run-frames 10000 --dump-ppm ddpsdoj_v24_default_10000.ppm
```

Result:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6850368 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9805 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

The supplied `ddpsdoj.zip` loaded correctly and matched the sandbox's MAME-derived ROM manifest:

```text
set=ddpsdoj source=/mnt/data/ddpsdoj.zip zip=1 ok=1
u2 present=1 crc=668e4cd6 expected=668e4cd6 size=138412032 load=138412032 ignore=0
u4 present=1 crc=e2a4411c expected=e2a4411c size=4194560 load=4194304 ignore=256
u23 present=1 crc=ac94801c expected=ac94801c size=4194560 load=4194304 ignore=256
u24 present=1 crc=f593045b expected=f593045b size=4194560 load=4194304 ignore=256
ddpsdoj romset loaded and CRC matched MAME manifest
```

The framebuffer at this point is still the diagnostic/probe pattern, not a game title screen. See `DDPSDOJ_V24_PROGRESS.png` and `DDPSDOJ_V24_PROGRESS.ppm`.

## Verified `--mame-cache-meta` run

Command:

```sh
./cv1k_sandbox --model d --romset /mnt/data/ddpsdoj.zip --mame-cache-meta --run-frames 1000 --save-state ddpsdoj_v24_mcache_1000.sav
```

Result:

```text
model=CV1000-D pc=0c002b16 sr=700000f0 frames=1000 cycles=23914569 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=0/0/00000000 ex=0/00000000/00000000 assists=0 fpga_bits=1 fpga_done=0 fpga_sum=00 icache=25424124/1 dcache=0/0 stale=0 mcache=1/26676711/30/0/25838997/833744/4000 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=5926065/26/0/0/0/0 tlb=0/0/26676741:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

Save-state reload and single-instruction trace were also checked. The loaded state resumed at the expected PC and preserved the cache counters:

```text
model=CV1000-D pc=0c002b18 sr=700000f0 frames=1000 cycles=23914570 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=0 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=0 dma=0 dma_bytes=0 last_dma=00000000>00000000/0/00000000/m00/00000000 np=0-0 nb=0-0 nc=0-0 irq_req=0/0/00000000 ex=0/00000000/00000000 assists=0 fpga_bits=1 fpga_done=0 fpga_sum=00 icache=25424125/1 dcache=0/0 stale=0 mcache=1/26676714/30/0/25838998/833746/4000 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=ff pg=0 col=0 rnd=0 spr=0 nmap=1024/454/568/36309 ce=1 alias=5926066/26/0/0/0/0 tlb=0/0/26676744:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

## Current blocker

The conservative boot path still converges on the copied-RAM loop around `0c1fb3dc-0c1fb42c` after long NAND/DMAC traffic. At 10000 frames the last status is `pc=0c1fb3e0`, with no illegal opcode, 6,850,368 NAND data reads, 3,244 DMA operations, and 6,850,366 DMA bytes. That is forward progress through the boot/NAND path, but still not graphics/title initialization.

The likely remaining blockers are unchanged: accurate SH7709S MMU/TLB/cache/coherency behavior, exact DMAC timing/side effects, NAND bad-block/OOB reconstruction, SH timers/IRQ acceptance, and a real CV1000 FPGA/blitter/YMZ770 implementation.
