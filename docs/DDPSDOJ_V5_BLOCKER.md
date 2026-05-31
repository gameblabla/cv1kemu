# DDPSDOJ v5 blocker notes

The current default probe reaches this stable state after 10,000 frames:

```text
PC=0c30b71a  opcode=affe  SR=40000101  illegal=0
```

The loop is not an unimplemented opcode. It is a deliberate `BRA` self-loop reached after code at `0c30b716` tests the return value from a helper at `0c30cba0`.

The helper sequence seen under trace:

```text
0c30cba0: push r14 / allocate locals
0c30cbaa: load first word from the caller-supplied pointer
0c30cbb4: load table base literal 0c7f83c0
0c30cbba: compare generated value against table limit
0c30cc20: failure path updates the value and returns 0
0c30b716: caller TST R0,R0
0c30b71a: failure self-loop
```

Relevant data in the 10,000-frame state:

```text
argument pointer r4 = 0c7f83d8
[0c7f83d8] = 400ed044
[0c7f83c0] = 00000002
[0c7f83c4] = 00000400
[0c7f83c8] = 00000840
[0c7f83cc] = 00000040
[0c7f83d0] = 00021000
```

The generated value is in the SH P0 virtual work-RAM range (`400xxxxx`), but this helper treats the first word as a bounded index/counter. That mismatch is now the primary boot blocker. The likely causes are still upstream: incomplete MMU/TLB/cache behavior, incomplete SH internal register side effects, a missing peripheral status/timing transition, or incorrect copied boot data.

Useful command when you have a state before the helper call:

```sh
./cv1k_sandbox --model d --romset /path/to/ddpsdoj.zip --load-state intermediate.sav --break-pc 0x0c30cba0 --break-max 1000000 --trace-steps 64
```
