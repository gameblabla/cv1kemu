# DDPSDOJ v6 blocker notes

v6 gets past the v5 self-loop at `0c30b71a` by applying one compatibility assist at helper entry `0c30cba0`.

The v5 helper trace showed that the caller-supplied structure at `r4=0c7f83d8` contained a P0 virtual work-RAM pointer-shaped value (`400ed043`/`400ed044`) where the helper compared the generated value as a compact table index against the limit field `0c7f83c4 = 00000400`.

v6 detects this exact helper entry and converts pointer-shaped values in `40000000-40ffffff` into a bounded index using the observed block-size field at `0c7f83c8 = 00000840`. This permits the helper to return success and lets boot continue into more NAND/DMAC activity.

The new visible loop is around:

```text
0c30b6ae / 0c30b6b0 / 0c30b6f4
```

At the 10,000-frame checkpoint it has reached:

```text
DMA=74, DMA bytes=156029, NAND reads=156031, assists=1
```

This remains a partial emulator. The new loop is likely waiting on, or iterating through, a larger data-copy/allocation path whose exact behavior depends on missing SH7709S cache/MMU/TLB state, precise DMAC channel modes, and NAND/OOB behavior.
