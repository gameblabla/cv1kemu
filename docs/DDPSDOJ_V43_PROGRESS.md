# DDPSDOJ v43 MAME-matching progress

v43 fixes a concrete SH7709S CCN/MMU register-map mismatch against MAME's `sh3_base_device::ccn_map`.

Older sandbox builds accidentally treated the BSC wait-state window at `0xffffff60/64/70` as the software-TLB approximation's `PTEH`, `PTEL`, and `MMUCR` registers.  In MAME the SH3 CCN map is:

- `MMUCR` at `0xffffffe0`
- `CCR` at `0xffffffec`
- `PTEH` at `0xfffffff0`
- `PTEL` at `0xfffffff4`
- `TTB` at `0xfffffff8`
- `TEA` at `0xfffffffc`

v43 updates `cv1k_bus_ldtlb()` to read `PTEH/PTEL` from the MAME offsets and updates MMUCR.TI invalidation to the correct `0xffffffe0` window.  This prevents BCR2/WCR1/RTCOR state from being misinterpreted as TLB state.

Verification remains unchanged for DDPSDOJ because the observed path still reports `tlb_loads=0`; the game path in this scaffold is still using coarse virtual-address aliases rather than a real SH7709S MMU.  The correction is nevertheless necessary before a stricter MMU/TLB port can be attempted.

Default 10,000-frame endpoint:

```text
model=CV1000-D pc=0c30b6ca sr=40000100 frames=10000 cycles=216392318 illegal=0 nand_r=7115757 dma=3245 dma_bytes=7112510 blit_ops=1 up=1 draw=0 tlb=0/0/482772715:00000000>00000000
```

MAME-TRAPA plus MAME-speedup 10,000-frame endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=121931185 illegal=0 nand_r=307769 dma=152 dma_bytes=307615 blit_ops=7 up=7 draw=0 mspeed=1/4494 tlb=0/0/150929778:00000000>00000000
```

Result: still not title-screen capable.  The remaining blocker is the large SH7709S platform-fidelity gap: true MMU/TLB translation and exceptions, cache coherency, scheduler/timer/interrupt timing, DMAC coherency, and remaining CV1000 FPGA/blitter/audio fidelity.
