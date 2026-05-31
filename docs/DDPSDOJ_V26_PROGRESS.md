# DDPSDOJ v26 progress

v26 is a MAME-source-alignment pass focused on the Samsung-compatible NAND flash device used by the CV1000 U2 graphics ROM. It is not a completed MAME-equivalent emulator and it still does not reach the DDPSDOJ title screen.

## MAME-derived NAND changes

The previous sandbox NAND path was a permissive byte-cursor shortcut. v26 replaces it with an ANSI C adaptation of MAME's generic NAND flash device command model from `src/devices/machine/nandflash.cpp` / `nandflash.h`, specifically configured for the Samsung K9F1G08U0M geometry used by CV1000:

- 2048-byte page data area plus 64-byte spare/OOB area, 2112 bytes total per page.
- 64 * 1024 pages, matching the 0x08400000-byte NAND region used by the CV1000 driver.
- Four-byte Samsung ID sequence: `ec f1 00 15`.
- Explicit command modes for init, read, program, erase, status, ID read, read-confirm, random-data input, and random-data output.
- Explicit pointer modes A/B/C.
- Two column address cycles and two row address cycles.
- Page-register backed program/random-input behavior.
- Status masking compatible with MAME's `data_r()` status behavior.

The port is intentionally framework-free C89 code. It does not import MAME's device framework or scheduler.

## Verification

Build verification: `docs/VERIFY_BUILD_V26.txt`

State save/load smoke tests:

- `docs/VERIFY_DDPSDOJ_V26_STATE_SAVE.txt`
- `docs/VERIFY_DDPSDOJ_V26_STATE_LOAD.txt`

10,000-frame conservative DDPSDOJ run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=0 up=0 draw=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

ROM CRC verification still matches the MAME manifest for the supplied `ddpsdoj.zip`:

```text
u2  crc=668e4cd6 expected=668e4cd6
u4  crc=e2a4411c expected=e2a4411c
u23 crc=ac94801c expected=ac94801c
u24 crc=f593045b expected=f593045b
```

## Result

The new NAND state machine changes the NAND accounting slightly versus v25 (`nand_r=6853614`, last page `9804-9804`) but the emulator still reaches the same copied-RAM loop around `0c1fb3dc-0c1fb42c` and does not reach a real title screen.

The remaining title-screen blockers are not just missing NAND commands. The sandbox still lacks MAME-equivalent SH7709S CPU/MMU/TLB/cache behavior, exact TRAPA/exception behavior, DMAC/cache coherency, timer/IRQ scheduling, the full CV1000 blitter/FPGA path, and YMZ770 AMM/MPEG audio decoding.
