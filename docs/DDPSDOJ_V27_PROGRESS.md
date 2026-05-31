# DDPSDOJ v27 progress

v27 is a MAME-source-alignment pass focused on the CV1000 blitter register map. It is not a completed MAME-equivalent emulator and it still does not reach the DDPSDOJ title screen.

## MAME-derived blitter-register changes

MAME installs the CV1000 blitter at `0x18000000-0x18000057` and implements the side effects in `cv1k_blitter_device::blitter_r` / `blitter_w` in `src/mame/cave/cv1k_v.cpp`. v27 adapts the register side-effect map into the ANSI C scaffold:

- `0x18000004` low-byte bit 0 starts a blitter command-list execution.
- `0x18000008` supplies the command-list address.
- `0x18000010` reads the ready value `0x00000010` when not busy.
- `0x18000024` and `0x18000028` read back as `0xffffffff`.
- `0x18000014` / `0x18000018` are scroll X/Y.
- `0x18000040` / `0x18000044` are clip X/Y with the MAME 32-pixel clip margin.

The actual pixel renderer remains the simplified C renderer already present in the sandbox. This is not a wholesale port of MAME's generated blit-function matrix.

## Verification

Build verification: `docs/VERIFY_BUILD_V27.txt`

State save/load smoke tests:

- `docs/VERIFY_DDPSDOJ_V27_STATE_SAVE.txt`
- `docs/VERIFY_DDPSDOJ_V27_STATE_LOAD.txt`

10,000-frame conservative DDPSDOJ run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

ROM CRC verification still matches the MAME manifest for the supplied `ddpsdoj.zip`.

## Result

v27 is a real improvement over v26: the DDPSDOJ run now reaches the MAME-mapped blitter execute path and records one upload operation (`blit_ops=1 up=1`), whereas v26 recorded no blitter operations. The uploaded content is still black and the emulator remains in the copied-RAM loop around `0c1fb3dc-0c1fb42c`, so this is not yet a title-screen build.

The remaining blockers are still concentrated in the SH7709S CPU/MMU/TLB/cache and exception/timer/IRQ/scheduler side, plus the incomplete full CV1000 pixel pipeline and YMZ770 AMM/MPEG audio decoding.
