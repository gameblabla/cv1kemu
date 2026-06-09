#ifndef CV1K_IR_H
#define CV1K_IR_H

/*
 * Architecture-independent IR for the register-allocating, block-chaining DRC.
 * See docs/DRC_DESIGN.md.  A guest frontend (e.g. SH-3) lowers decoded ops into
 * this IR; the shared optimiser + linear-scan allocator annotate it; a per-host
 * backend (x64, arm64, …) emits native code with patchable chaining exits.
 *
 * This header is the design seam only — not yet wired into the build.  It is
 * intentionally small so the op set and lowering can grow incrementally, each
 * step gated by tools/jit_difftest.
 */

#include "cv1k_types.h"
#include <stddef.h>

/* ---- virtual registers --------------------------------------------------
 * vregs are SSA-ish temporaries produced/consumed inside one block.  The
 * allocator maps them (and any guest regs they shadow) to host registers or
 * spills them to the cpu struct (the always-memory path is the spill, so a
 * partial allocation is always correct). */
typedef cv1k_u16 cv1k_ir_vreg;
#define CV1K_IR_NOVREG ((cv1k_ir_vreg)0xffffU)

/* ---- IR opcodes ---------------------------------------------------------
 * Three-address form: dst = op(a, b).  `b` may be an immediate (IR_FLAG_IMMB). */
enum cv1k_ir_op {
    IR_NOP = 0,
    /* guest state <-> vreg */
    IR_GLOAD,    /* dst = guest_reg[imm]            */
    IR_GSTORE,   /* guest_reg[imm] = a              */
    IR_SLOAD,    /* dst = cpu field at byte off imm */
    IR_SSTORE,   /* cpu field at byte off imm = a   */
    IR_TLOAD,    /* dst = T bit (0/1)               */
    IR_TSTORE,   /* T bit = a (low bit)             */
    /* moves / consts */
    IR_MOVI,     /* dst = imm                       */
    IR_MOV,      /* dst = a                         */
    /* alu (b = vreg or imm) */
    IR_ADD, IR_SUB, IR_ADDC, IR_SUBC, IR_AND, IR_OR, IR_XOR, IR_NOT, IR_NEG,
    IR_MULU,     /* dst = a * b (low 32)            */
    IR_DMUL_MACL, /* MACH:MACL = (u/s32)a * (u/s32)b; flags bit0=signed */
    IR_MUL_MACL,  /* MACL = low product of guest regs in imm; aux selects width */
    IR_DECT,     /* dst = dst - 1; T = (dst == 0)  (SH DT)  */
    /* shifts / rotates; carry variants read/write the T pseudo via a/dst */
    IR_SHL, IR_SHR, IR_SAR, IR_ROL, IR_ROR, IR_ROCL, IR_ROCR,
    IR_SHAD, IR_SHLD, /* SH-3 dynamic shifts */
    /* compare: dst(T-like 0/1) = (a cc b) */
    IR_CMP,      /* aux = cv1k_ir_cc                */
    /* memory (aux = size 1/2/4; IR_FLAG_SEXT for sign-extend loads) */
    IR_LOAD,     /* dst = [a]                       */
    IR_STORE,    /* [a] = b                         */
    IR_LOADIDX,  /* dst = [a + b]                   */
    IR_STOREIDX, /* [a + b] = dst                   */
    /* control / block exits */
    IR_EXIT,     /* end block, next pc = imm (static)        */
    IR_EXITVAR,  /* end block, next pc = a (computed)        */
    IR_EXITCOND, /* if a: pc = imm ; else pc = imm2          */
    /* escape hatch: call C helper(cpu[, args]) for ops not lowered */
    IR_CALLH,
};

enum cv1k_ir_cc { IR_CC_EQ, IR_CC_HS, IR_CC_GE, IR_CC_HI, IR_CC_GT, IR_CC_PL, IR_CC_PZ };

#define CV1K_IR_FLAG_IMMB  0x01u  /* operand b is the immediate field         */
#define CV1K_IR_FLAG_SEXT  0x02u  /* IR_LOAD sign-extends                     */
#define CV1K_IR_FLAG_DIRTY 0x04u  /* allocator: dst guest reg must be flushed */

struct cv1k_ir_inst {
    cv1k_u8  op;       /* enum cv1k_ir_op  */
    cv1k_u8  aux;      /* cc / mem size / helper id */
    cv1k_u8  flags;    /* CV1K_IR_FLAG_*   */
    cv1k_u8  hostreg;  /* allocator output: host reg for dst (0xff = spilled) */
    cv1k_ir_vreg dst;
    cv1k_ir_vreg a;
    cv1k_ir_vreg b;
    cv1k_u32 imm;      /* immediate / guest-reg index / cpu-field offset / pc  */
    cv1k_u32 imm2;     /* second target for IR_EXITCOND                        */
};

#define CV1K_IR_MAX_INSTS 256u
#define CV1K_IR_MAX_VREGS 256u

struct cv1k_ir_block {
    cv1k_u32 guest_pc;                         /* block entry guest PC          */
    cv1k_u32 guest_cycles;                     /* total guest cycles in block   */
    cv1k_u16 inst_count;
    cv1k_u16 vreg_count;
    struct cv1k_ir_inst inst[CV1K_IR_MAX_INSTS];
    /* exit-target guest PCs, filled by the frontend, used by the chaining
     * manager to link to successor blocks once they are compiled. */
    cv1k_u32 exit_pc[2];
    cv1k_u8  exit_count;
};

/* ---- backend v2 interface (per host arch) -------------------------------
 * The chaining manager owns native code buffers and patch sites; the backend
 * only translates IR and rewrites exit branches. */
struct cv1k_ir_regplan;            /* opaque allocator output (phase 2)        */

struct cv1k_ir_backend {
    const char *name;
    int  (*available)(void);
    /* Emit native code for `blk` into `code` (capacity `cap`); record up to two
     * exit patch sites (byte offsets of the rel32) in `exit_site`.  Returns code
     * size, or 0 on failure (caller falls back to the legacy emitter). */
    size_t (*emit)(const struct cv1k_ir_block *blk, const struct cv1k_ir_regplan *plan,
                   cv1k_u8 *code, size_t cap, size_t exit_site[2]);
    /* Chaining: rewrite the rel32 at `site` within `code` to jump to `target`. */
    void (*patch_exit)(cv1k_u8 *code, size_t site, const cv1k_u8 *target);
    void (*unpatch_exit)(cv1k_u8 *code, size_t site, const cv1k_u8 *dispatch_trampoline);
};

const struct cv1k_ir_backend *cv1k_ir_select_backend(void);

/* ---- phase-1 isolated compiler (tools/jit_difftest --ir) ----------------- */
struct cv1k_bus;
struct sh7709s_cpu;
typedef cv1k_u32 (*cv1k_ir_block_fn)(struct sh7709s_cpu *, struct cv1k_bus *);

/* 1 if `op` is in the phase-1 IR-lowerable subset (straight-line ALU/move). */
int cv1k_ir_phase1_supported(cv1k_u16 op);
void cv1k_ir_set_cache(int on);   /* 1 = register-allocate guest regs (default) */
void cv1k_ir_set_fastram(int on); /* 1 = inline work-RAM access (default)       */
void cv1k_ir_set_internal_loops(int on); /* back-branch loop chaining (test only) */

/* ---- production wiring: cached run-frame + interpreter fallback ---------- */
int  cv1k_ir_enabled(void);
void cv1k_ir_enable(int on);
void cv1k_ir_reset(void);   /* drop the block cache (call on icache invalidation) */
cv1k_u32 cv1k_irjit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                              cv1k_u32 cycle_budget, cv1k_u32 tmu_interval);
/* Compile a fresh block at guest `pc`; returns ops covered (0 = nothing/failed)
 * and fills *out_fn plus the exec-mem handle for cv1k_ir_free_block. */
cv1k_u32 cv1k_ir_compile_block(struct cv1k_bus *bus, cv1k_u32 pc,
                               cv1k_ir_block_fn *out_fn, void **out_mem, size_t *out_cap);
void cv1k_ir_free_block(void *mem, size_t cap);

#endif /* CV1K_IR_H */
