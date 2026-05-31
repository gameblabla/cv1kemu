# DDPSDOJ v16 progress note

v16 is another boot/probe pass, not a title-screen milestone.

The useful change is the gated experimental path:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --dcache --wide-p0-alias --aggressive-assists --run-frames 8000
```

This reaches:

```text
dma=54955 dma_bytes=116063998 illegal=0 unmapped_r=0 unmapped_w=0
```

The v15 experimental path reached 20,447 DMA transfers and 43,183,102 copied bytes before stopping at `0c30b71a`. v16's aggressive NAND-copy status bridge advances to 54,955 transfers and 116,063,998 copied bytes without illegal opcodes at the 8,000-frame checkpoint.

The bridge is deliberately narrow but still not hardware-accurate. It treats the `0c30b716/0c30b71a` helper loop as a failed status result caused by the incomplete SH7709S/DMAC/NAND/cache-coherency model, sets the helper status nonzero, and lets the copied-RAM boot path expose the next blocker.

Longer experimental runs eventually fall into copied payload/invalid control flow after near-full NAND copying. That confirms the real remaining work is still SH7709S cache/TLB/MMU semantics, exact DMAC modes, NAND bad-block/OOB reconstruction, IRQ/timer/peripheral behavior, RTC9701, YMZ770, and FPGA/blitter completion behavior.
