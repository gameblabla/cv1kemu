# DDPSDOJ v47 progress

v47 adds a guarded accelerator for a deterministic SH copied-RAM loop observed after the v46 DMAC changes.

The accelerator recognizes the exact instruction sequence at `0c30b6ae-0c30b6f6` and applies the equivalent SH-visible effects in one host step:

- subtract the stride read from the same literal path used by the program;
- update the stack counter at `[R14+0x18]`;
- mirror the final counter at `[R14+0x14]`;
- increment `[R14+0x10]` only for post-subtract non-negative iterations;
- leave `R1`, `R2`, `PC`, and `SR.T` as the non-accelerated loop would when it exits.

This is an execution-throughput fix for the standalone ANSI C interpreter, not a title-screen fix.  The default 10,000-frame endpoint remains inside the same copied-RAM helper family, but the status line now shows the new loop accelerator firing (`assists=743` in the default verification run).

Default 10,000-frame endpoint:

```text
model=CV1000-D pc=0c30b6d2 sr=40000100 frames=10000 cycles=217994686 illegal=0 nand_r=7115757 blit_ops=1 up=1 draw=0 assists=743
```

MAME-TRAPA plus MAME-speedup 10,000-frame endpoint:

```text
model=CV1000-D pc=0c1d1346 sr=40000100 frames=10000 cycles=122545295 illegal=0 nand_r=307769 blit_ops=7 up=7 draw=0 assists=95 mspeed=1/4469
```

Result: still not title-screen capable.  The visible framebuffer remains diagnostic/noisy data, not valid DDPSDOJ graphics.
