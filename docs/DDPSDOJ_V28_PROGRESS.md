# DDPSDOJ v28 progress

v28 is another MAME-source-alignment pass. It does not reach the DDPSDOJ title screen.

## MAME-derived changes

- Added support for the CV1000 blitter command-stream `0xc000` clip opcode. MAME consumes the opcode and one parameter word; nonzero parameter selects the screen clip expanded by the 32-pixel margin and zero selects the full VRAM coordinate space.
- Changed undefined blitter register reads to return zero rather than local shadow register values, matching MAME's `blitter_r` fall-through.
- Added FPGA firmware checksum classification using MAME's checksum table: `0x03`, `0x3e`, `0xf9`, `0xe1`.
- Bumped save states to `CV1KSS28` and saved/restored the new blitter/firmware diagnostic fields.

## Verification

Build verification: `docs/VERIFY_BUILD_V28.txt`

State save/load smoke tests:

- `docs/VERIFY_DDPSDOJ_V28_STATE_SAVE.txt`
- `docs/VERIFY_DDPSDOJ_V28_STATE_LOAD.txt`

10,000-frame conservative DDPSDOJ run:

```text
model=CV1000-D pc=0c1fb3e0 sr=40000100 frames=10000 cycles=211325126 illegal=0 last_illegal=00000000:0000 irq_ack=0 nand=138412032B nand_r=6853614 nand_w=0 blit_ops=1 up=1 draw=0 clip=0 unk=0/0000 ymz_writes=6 ymz_reg=3 ymz_key=0/0 dma=3244 dma_bytes=6850366 last_dma=b0000000>0c7f7efe/2112/00004421/m01/00004422 np=9804-9804 nb=153-153 nc=0-0 irq_req=0/0/00000000 ex=4/00000160/0000003c assists=9 fpga_bits=2323240 fpga_done=1 fpga_sum=e0 fpga_fw=-1 icache=236829541/63 dcache=0/0 stale=0 mcache=0/0/0/0/0/0/0 cachectl=0 widep0=0 compact400=0 dmasync=0 dmainv=0 ndata=0 nandcmd=70 pg=9804 col=0 rnd=0 spr=207616 nmap=1024/454/568/36309 ce=1 alias=27787599/124/0/0/60807112/1 tlb=0/0/313510828:00000000>00000000 unmapped_r=17 last_r=31cdf5cc unmapped_w=0 last_w=00000000:00
```

ROM CRC verification still matches the supplied `ddpsdoj.zip` against the bundled manifest.

## Result

The v28 patch is correctness-oriented. The default DDPSDOJ checkpoint is unchanged at the copied-RAM loop around `0c1fb3dc-0c1fb42c`; the first observed blitter list contains no clip commands (`clip=0`) and no unknown commands (`unk=0/0000`).
