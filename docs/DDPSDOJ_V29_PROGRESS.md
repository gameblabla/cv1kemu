# DDPSDOJ v29 progress

v29 is another MAME-source-alignment pass. It does not reach the DDPSDOJ title screen.

## MAME-derived changes

- Corrected the blitter DSW read at register `0x18000050`: MAME's `cv1k_blitter_device::blitter_r` returns the `DSW` input port there. For the default CV1000 input definition, the low four DIP bits are off and the remaining bits read active/unknown, so the sandbox now returns `0xfffffff0` rather than zero.
- Added a MAME-faithful SH7709S TRAPA path behind `--mame-trapa`. MAME stores `TRA = imm << 2`, `SSR`, `SPC`, and `SGR`, sets `MD/RB/BL`, records `EXPEVT = 0x160`, and vectors to `VBR + 0x100`. The sandbox retains the older non-vectoring diagnostic behavior by default because it still exposes later boot stages more effectively.
- Changed blitter execution to snapshot the main RAM image before executing a command list. MAME copies the blit command stream to `m_ram16_copy` before queuing the worker thread; the sandbox's full-RAM snapshot is slower but simpler and avoids in-flight command mutation.
- Bumped save states to `CV1KSS29` and persisted the `--mame-trapa` flag.

## Verification

Build verification: `docs/VERIFY_BUILD_V29.txt`

State save/load smoke tests:

- `docs/VERIFY_DDPSDOJ_V29_STATE_SAVE.txt`
- `docs/VERIFY_DDPSDOJ_V29_STATE_LOAD.txt`

10,000-frame conservative DDPSDOJ run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

10,000-frame `--mame-trapa` DDPSDOJ run:

```text
model=CV1000-D pc=0c1d1348 sr=40000101 frames=10000 cycles=241671466 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=307769 nand_w=0 blit_ops=7 up=7 draw=0 clip=0 unk=0/0000 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=152 dma_bytes=307615 last_dma=b0000000>0c197494/1868/00004421/m01/00004422 np=36491-36491 nb=570-570 nc=0-1868 irq_req=0/0/00000000 ex=5/00000160/0000003c assists=8 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=266813913/88 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 mtrap=1 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=36491 col=1868 rnd=0 spr=9280 nmap=1024/454/568/36309 ce=1 alias=21181444/308/0/0/0/0 tlb=0/0/391944865:00000000>00000000 unmapped_r=0 last_r=00000000 unmapped_w=0 last_w=00000000:00
```

ROM CRC verification still matches the supplied `ddpsdoj.zip` against the bundled manifest.

## Result

The conservative checkpoint remains unchanged at the copied-RAM loop around `0c1fb3dc-0c1fb42c`.

The MAME-TRAPA experiment changes behavior materially: by 10,000 frames it is instead at the earlier vblank wait around `0c1d1348`, with seven upload blits observed. This is closer to MAME's exception semantics but still not a title screen; the remaining missing pieces are IRQ/timer scheduling, SH7709S MMU/TLB/cache coherency, DMAC/cache interaction, and the rest of the platform timing model.
