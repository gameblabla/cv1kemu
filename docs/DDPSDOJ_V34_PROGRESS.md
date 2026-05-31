# DDPSDOJ v34 MAME-matching progress

This pass focused on a bounded SH7709S subsystem that was still modeled as a CV1000-specific shortcut: DMAC transfers.

Changes:

1. Ported the MAME SH-3 DMAC transfer-size table from `src/devices/cpu/sh/sh4dmac.cpp`: `sh3_dmasize[(CHCR >> 3) & 3] = { 1, 2, 4, 16 }`. Earlier sandbox builds always performed byte transfers. The observed DDPSDOJ NAND transfer path still decodes to one-byte transfers (`CHCR=00004421`), so the default endpoint is intentionally unchanged, but RAM/RAM and later device DMA paths now match MAME more closely.

2. Matched MAME's SH DMA address update order: decrement modes pre-decrement before the access; increment modes post-increment after the access; invalid mode 3 remains fixed in the ANSI C scaffold.

3. Applied MAME's 29-bit SH physical-address mask to DMAC source/destination access addresses before the transfer body.

4. Matched MAME's DDPSDOJ speedup handler condition more closely by recognizing both `idlepc` and `idlepc+2`. MAME's `speedup_r()` tests `pc == m_idlepc || pc == m_idlepc + 2`.

Verification summary:

Default 10,000-frame endpoint remains the conservative copied-RAM loop:

```text
pc=0c1fb3e0 nand_r=6853614 dma=3244 dma_bytes=6850366 blit_ops=1 up=1 draw=0
```

MAME-speedup 10,000-frame endpoint remains the MAME-style idle wait:

```text
pc=0c1d1346 nand_r=307769 dma=152 dma_bytes=307615 blit_ops=7 up=7 draw=0 mspeed=1/4494
```

Result: still no title screen. This is another fidelity correction rather than a boot hack.
