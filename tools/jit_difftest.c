/*
 * SH-3 JIT differential tester.
 *
 * Generates random sequences of JIT-supported register/ALU instructions, runs
 * them through (a) the reference interpreter and (b) a freshly compiled JIT
 * block from identical CPU state, and asserts the resulting architectural state
 * is bit-identical.  This is the safety net for register-allocation / block
 * chaining work on the DRC: any codegen bug shows up here instead of as silent
 * in-game corruption.
 *
 * v1 covers register-only ops (no memory / no PC-relative / no branches), which
 * is exactly the set the recent fallback-reduction work added (DIV1, ROTCL,
 * etc.).  Memory ops need RAM snapshot/restore between the two runs and are a
 * planned extension.
 */
#include "emu.h"
#include "cpu_sh7709s.h"
#include "bus.h"
#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include "sh3_jit/cv1k_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_PC   (CV1K_ADDR_WORK_RAM + 0x1000UL)
#define MAX_OPS   8

enum tkind { K_NM, K_N, K_NONE, K_NIMM, K_IMM8 };

struct tmpl { cv1k_u16 base; enum tkind kind; const char *name; };

static const struct tmpl g_tmpl[] = {
    /* n+m register ALU */
    { 0x300c, K_NM, "ADD" },  { 0x3008, K_NM, "SUB" },  { 0x300e, K_NM, "ADDC" }, { 0x300a, K_NM, "SUBC" },
    { 0x2009, K_NM, "AND" },
    { 0x200b, K_NM, "OR" },   { 0x200a, K_NM, "XOR" },  { 0x2008, K_NM, "TST" },
    { 0x3000, K_NM, "CMPEQ" },{ 0x3002, K_NM, "CMPHS" },{ 0x3003, K_NM, "CMPGE" },
    { 0x3006, K_NM, "CMPHI" },{ 0x3007, K_NM, "CMPGT" },{ 0x6003, K_NM, "MOV" },
    { 0x6007, K_NM, "NOT" },  { 0x600b, K_NM, "NEG" },  { 0x600c, K_NM, "EXTUB" },
    { 0x600d, K_NM, "EXTUW" },{ 0x600e, K_NM, "EXTSB" },{ 0x600f, K_NM, "EXTSW" },
    /* newly JIT'd ops (the risky ones being validated) */
    { 0x3004, K_NM, "DIV1" }, { 0x2007, K_NM, "DIV0S" },{ 0x0007, K_NM, "MULL" },
    { 0x3005, K_NM, "DMULU" },{ 0x300d, K_NM, "DMULS" },
    { 0x400c, K_NM, "SHAD" }, { 0x400d, K_NM, "SHLD" },
    { 0x6008, K_NM, "SWAPB" },{ 0x6009, K_NM, "SWAPW" },{ 0x600a, K_NM, "NEGC" },
    /* n-only shifts/rotates */
    { 0x4000, K_N, "SHLL" },  { 0x4001, K_N, "SHLR" },  { 0x4020, K_N, "SHAL" },
    { 0x4021, K_N, "SHAR" },  { 0x4008, K_N, "SHLL2" }, { 0x4009, K_N, "SHLR2" },
    { 0x4018, K_N, "SHLL8" }, { 0x4019, K_N, "SHLR8" }, { 0x4028, K_N, "SHLL16" },
    { 0x4029, K_N, "SHLR16" },{ 0x4010, K_N, "DT" },    { 0x4011, K_N, "CMPPZ" },
    { 0x4015, K_N, "CMPPL" }, { 0x4024, K_N, "ROTCL" }, { 0x4025, K_N, "ROTCR" },
    { 0x4004, K_N, "ROTL" },  { 0x4005, K_N, "ROTR" },
    /* no-field flag ops */
    { 0x0019, K_NONE, "DIV0U" }, { 0x0008, K_NONE, "CLRT" },
    { 0x0018, K_NONE, "SETT" },
    /* special-register moves */
    { 0x0029, K_N, "MOVT" },
    { 0x0002, K_N, "STCSR" },  { 0x0012, K_N, "STCGBR" }, { 0x0022, K_N, "STCVBR" },
    { 0x000a, K_N, "STSMACH" },{ 0x001a, K_N, "STSMACL" },{ 0x002a, K_N, "STSPR" },
    { 0x401e, K_N, "LDCGBR" }, { 0x402e, K_N, "LDCVBR" },
    { 0x400a, K_N, "LDSMACH" },{ 0x401a, K_N, "LDSMACL" },{ 0x402a, K_N, "LDSPR" },
    { 0xc700, K_IMM8, "MOVA" },
    /* n + imm8 */
    { 0x7000, K_NIMM, "ADDI" },  { 0xe000, K_NIMM, "MOVI" },
};
#define NTMPL ((int)(sizeof(g_tmpl) / sizeof(g_tmpl[0])))

static unsigned long g_rng = 0x12345678UL;
static cv1k_u32 rnd(void) { g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL; return (cv1k_u32)(g_rng >> 32); }

static cv1k_u16 gen_op(const struct tmpl *t)
{
    cv1k_u16 op = t->base;
    cv1k_u32 n = rnd() & 15U, m = rnd() & 15U;
    switch (t->kind) {
    case K_NM:   op |= (cv1k_u16)((n << 8) | (m << 4)); break;
    case K_N:    op |= (cv1k_u16)(n << 8); break;
    case K_NIMM: op |= (cv1k_u16)((n << 8) | (rnd() & 0xffU)); break;
    case K_IMM8: op |= (cv1k_u16)(rnd() & 0xffU); break;
    case K_NONE: break;
    }
    return op;
}

static void randomize_cpu(struct sh7709s_cpu *c)
{
    sh7709s_reset(c);
    for (int i = 0; i < 16; i++) c->r[i] = rnd();
    c->pr = rnd(); c->gbr = rnd(); c->mach = rnd(); c->macl = rnd();
    /* MD set, RB=0, BL=0, random T/S/Q/M, IMASK=15 */
    c->sr = 0x400000f0UL | (rnd() & (0x1U | 0x2U | 0x100U | 0x200U));
    c->pc = TEST_PC;
    c->cycles = 0; c->m_delay = 0; c->sleep_mode = 0; c->halted = 0; c->pend_mask = 0;
}

static int cmp_state(const struct sh7709s_cpu *a, const struct sh7709s_cpu *b,
                     const cv1k_u16 *ops, int nops, const char *names[])
{
    int bad = 0;
    char where[64] = "";
    for (int i = 0; i < 16; i++) if (a->r[i] != b->r[i]) { snprintf(where, sizeof where, "r[%d] i=%08lx j=%08lx", i, (unsigned long)a->r[i], (unsigned long)b->r[i]); bad = 1; break; }
    if (!bad && a->sr   != b->sr)   { snprintf(where, sizeof where, "sr %08lx/%08lx",   (unsigned long)a->sr,   (unsigned long)b->sr);   bad = 1; }
    if (!bad && a->macl != b->macl) { snprintf(where, sizeof where, "macl %08lx/%08lx", (unsigned long)a->macl, (unsigned long)b->macl); bad = 1; }
    if (!bad && a->mach != b->mach) { snprintf(where, sizeof where, "mach %08lx/%08lx", (unsigned long)a->mach, (unsigned long)b->mach); bad = 1; }
    if (!bad && a->pr   != b->pr)   { snprintf(where, sizeof where, "pr %08lx/%08lx",   (unsigned long)a->pr,   (unsigned long)b->pr);   bad = 1; }
    if (!bad && a->gbr  != b->gbr)  { snprintf(where, sizeof where, "gbr %08lx/%08lx",  (unsigned long)a->gbr,  (unsigned long)b->gbr);  bad = 1; }
    if (!bad && a->pc   != b->pc)   { snprintf(where, sizeof where, "pc %08lx/%08lx",   (unsigned long)a->pc,   (unsigned long)b->pc);   bad = 1; }
    if (bad) {
        fprintf(stderr, "MISMATCH (%s) ops:", where);
        for (int i = 0; i < nops; i++) fprintf(stderr, " %s(%04x)", names[i], ops[i]);
        fprintf(stderr, "\n");
    }
    return !bad;
}

int main(int argc, char **argv)
{
    long trials = (argc > 1) ? atol(argv[1]) : 2000000L;
    if (argc > 2) g_rng = strtoul(argv[2], NULL, 0);
    /* "ir" as any later arg selects the phase-1 IR compiler under test. */
    int ir_mode = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "ir") == 0) ir_mode = 1;

    int bench = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "bench") == 0) bench = 1;

    static struct cv1k_machine m;
    if (!cv1k_machine_init(&m, CV1K_MODEL_B)) { fprintf(stderr, "machine init failed\n"); return 2; }
    if (!sh7709s_c23jit_available()) { fprintf(stderr, "JIT backend unavailable on this host\n"); return 2; }

    if (bench) {
        /* A long ALU block over a few registers (R0..R3): boundary cost (entry
         * load / exit store) is amortised so the register-allocation win in the
         * body is visible.  Mix of ADD/SUB/XOR/AND/OR Rm,Rn. */
        cv1k_u16 blk[40]; unsigned nblk = 0;
        static const cv1k_u16 base[] = { 0x300c, 0x3008, 0x200a, 0x2009, 0x200b };
        for (unsigned i = 0; i < 40; i++) {
            unsigned n = i & 3U, mm = (i + 1) & 3U;       /* R0..R3 */
            blk[nblk++] = (cv1k_u16)(base[i % 5] | (n << 8) | (mm << 4));
        }
        cv1k_u32 boff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
        for (unsigned i = 0; i < nblk; i++) {
            m.main_ram[boff + i*2] = (cv1k_u8)(blk[i] >> 8); m.main_ram[boff + i*2 + 1] = (cv1k_u8)(blk[i] & 0xff);
        }
        m.main_ram[boff + nblk*2] = 0; m.main_ram[boff + nblk*2 + 1] = 0;
        unsigned nops_blk = nblk;
        cv1k_bus_invalidate_icache_all(&m.bus);
        struct sh7709s_cpu c; randomize_cpu(&c);
        const long iters = 80000000L;
        for (int pass = 0; pass < 2; pass++) {
            cv1k_ir_set_cache(pass == 0 ? 0 : 1);
            cv1k_ir_block_fn fn = NULL; void *mem = NULL; size_t cap = 0;
            cv1k_u32 cov = cv1k_ir_compile_block(&m.bus, TEST_PC, &fn, &mem, &cap);
            if (!fn || cov != nops_blk) { fprintf(stderr, "bench compile failed (cov=%u)\n", cov); return 2; }
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            for (long i = 0; i < iters; i++) { c.pc = TEST_PC; fn(&c, &m.bus); }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)iters;
            printf("bench %-10s %.2f ns/block (%.3f ns/op, %u ops)\n", pass == 0 ? "spilled" : "cached", ns, ns / nops_blk, nops_blk);
            cv1k_ir_free_block(mem, cap);
        }
        cv1k_machine_shutdown(&m);
        return 0;
    }

    int membench = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "membench") == 0) membench = 1;
    if (membench) {
        /* 40x MOV.L @R1,R0 with R1 a fixed RAM address; time helper-call vs
         * inline fast-RAM. */
        cv1k_u16 blk[40]; unsigned nblk = 40;
        for (unsigned i = 0; i < nblk; i++) blk[i] = 0x6012;  /* MOV.L @R1,R0 */
        cv1k_u32 boff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
        for (unsigned i = 0; i < nblk; i++) { m.main_ram[boff+i*2] = (cv1k_u8)(blk[i]>>8); m.main_ram[boff+i*2+1] = (cv1k_u8)(blk[i]&0xff); }
        m.main_ram[boff+nblk*2] = 0; m.main_ram[boff+nblk*2+1] = 0;
        cv1k_bus_invalidate_icache_all(&m.bus);
        struct sh7709s_cpu c; randomize_cpu(&c); c.r[1] = CV1K_ADDR_WORK_RAM + 0x4040U;
        const long iters = 40000000L;
        for (int pass = 0; pass < 2; pass++) {
            cv1k_ir_set_fastram(pass);   /* 0 = helper call, 1 = inline */
            cv1k_ir_block_fn fn = NULL; void *bm = NULL; size_t bc = 0;
            cv1k_u32 cov = cv1k_ir_compile_block(&m.bus, TEST_PC, &fn, &bm, &bc);
            if (!fn || cov != nblk) { fprintf(stderr, "membench compile failed (cov=%u)\n", cov); return 2; }
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            for (long i = 0; i < iters; i++) { c.pc = TEST_PC; fn(&c, &m.bus); }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)iters;
            printf("membench %-9s %.2f ns/block (%.3f ns/load)\n", pass == 0 ? "helper" : "inline", ns, ns / nblk);
            cv1k_ir_free_block(bm, bc);
        }
        cv1k_machine_shutdown(&m);
        return 0;
    }

    int loopbench = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "loopbench") == 0) loopbench = 1;
    if (loopbench) {
        /* chained: loop runs N iters inside ONE block call (regs resident,
         * back-branch is an internal jump). unchained: the same body compiled
         * without the branch, dispatched once per iteration (entry/exit each). */
        cv1k_ir_set_internal_loops(1);
        const cv1k_u32 N = 1000; const long M = 100000;
        cv1k_u32 boff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
        struct timespec t0, t1;
        /* chained loop block: ADD R1,R2 ; DT R0 ; BF loop ; stop */
        { static const cv1k_u16 p[] = { 0x321c, 0x4010, 0x8bfc, 0x0028 };
          for (unsigned i=0;i<4;i++){m.main_ram[boff+i*2]=(cv1k_u8)(p[i]>>8);m.main_ram[boff+i*2+1]=(cv1k_u8)(p[i]&0xff);}
          cv1k_bus_invalidate_icache_all(&m.bus);
          cv1k_ir_block_fn fn=NULL; void*bm=NULL; size_t bc=0;
          if (cv1k_ir_compile_block(&m.bus, TEST_PC, &fn, &bm, &bc)!=3||!fn){fprintf(stderr,"compile fail\n");return 2;}
          struct sh7709s_cpu c; randomize_cpu(&c);
          clock_gettime(CLOCK_MONOTONIC,&t0);
          for (long i=0;i<M;i++){ c.r[0]=N; c.pc=TEST_PC; fn(&c,&m.bus); }
          clock_gettime(CLOCK_MONOTONIC,&t1);
          double ns=((double)(t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/((double)M*N);
          printf("loopbench chained   %.3f ns/iter\n", ns);
          cv1k_ir_free_block(bm,bc); }
        /* unchained: body-only block ADD R1,R2 ; DT R0 ; stop, called per iter */
        { static const cv1k_u16 p[] = { 0x321c, 0x4010, 0x0028 };
          for (unsigned i=0;i<3;i++){m.main_ram[boff+i*2]=(cv1k_u8)(p[i]>>8);m.main_ram[boff+i*2+1]=(cv1k_u8)(p[i]&0xff);}
          cv1k_bus_invalidate_icache_all(&m.bus);
          cv1k_ir_block_fn fn=NULL; void*bm=NULL; size_t bc=0;
          if (cv1k_ir_compile_block(&m.bus, TEST_PC, &fn, &bm, &bc)!=2||!fn){fprintf(stderr,"compile fail2\n");return 2;}
          struct sh7709s_cpu c; randomize_cpu(&c); c.r[0]=N;
          clock_gettime(CLOCK_MONOTONIC,&t0);
          for (long i=0;i<M;i++) for (cv1k_u32 j=0;j<N;j++){ c.pc=TEST_PC; fn(&c,&m.bus); }
          clock_gettime(CLOCK_MONOTONIC,&t1);
          double ns=((double)(t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/((double)M*N);
          printf("loopbench unchained %.3f ns/iter\n", ns);
          cv1k_ir_free_block(bm,bc); }
        cv1k_machine_shutdown(&m);
        return 0;
    }

    int br_test = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "branch") == 0) br_test = 1;
    if (br_test) {
        /* Branch terminators, including delayed forms.  Run interpreter to a
         * STOP op and run IR block-by-block to the same PC, comparing final CPU
         * state. */
        cv1k_ir_set_internal_loops(0);
        long fail = 0, ran = 0; long n = trials ? trials : 1000000L;
        static const cv1k_u16 brhi[4] = { 0x8900, 0x8b00, 0x8d00, 0x8f00 };  /* BT BF BT/S BF/S */
        static const cv1k_u16 uncond[6] = { 0xa001, 0xb001, 0x412b, 0x410b, 0x000b, 0x002b }; /* BRA BSR JMP@R1 JSR@R1 RTS RTE */
        for (long t = 0; t < n; t++) {
            cv1k_u16 prog[5] = {0}; cv1k_u32 stop_pc;
            int is_uncond = (int)(rnd() & 1U);
            if (!is_uncond) {
                int sett = (int)(rnd() & 1U);             /* T value before the branch */
                cv1k_u16 bhi = brhi[rnd() & 3U];          /* which conditional branch  */
                int delayed = (bhi == 0x8d00U || bhi == 0x8f00U);
                /* target = brpc+4+disp*2; brpc=TEST_PC+2; want TEST_PC+8 (op4) => disp=1 */
                prog[0] = (cv1k_u16)(sett ? 0x0018 : 0x0008);
                prog[1] = (cv1k_u16)(bhi | 0x01);
                prog[2] = (delayed && (rnd() & 1U)) ? 0x4000 : 0x7001; /* sometimes SHLL R0: writes T in delay slot */
                prog[3] = 0x7002; prog[4] = 0x0028;
                stop_pc = TEST_PC + 8U;
            } else {
                cv1k_u16 op = uncond[rnd() % 6U];
                prog[0] = op; prog[1] = (op == 0x002bU) ? 0x0009U : 0x7001U; prog[2] = 0x7002; prog[3] = 0x0028; prog[4] = 0x0028;
                stop_pc = TEST_PC + 6U;
            }
            cv1k_u32 boff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
            for (int i = 0; i < 5; i++) { m.main_ram[boff+i*2] = (cv1k_u8)(prog[i]>>8); m.main_ram[boff+i*2+1] = (cv1k_u8)(prog[i]&0xff); }
            cv1k_bus_invalidate_icache_all(&m.bus);
            struct sh7709s_cpu base; randomize_cpu(&base);
            if (is_uncond) {
                if (prog[0] == 0x412b || prog[0] == 0x410b) base.r[1] = stop_pc;
                if (prog[0] == 0x000b) base.pr = stop_pc;
                if (prog[0] == 0x002b) { base.spc = stop_pc; base.ssr = base.sr; }
            }
            struct sh7709s_cpu ci = base, cj = base;
            int g = 0; while (ci.pc != stop_pc && g++ < 1000) sh7709s_step(&ci, &m.bus);
            int g2 = 0, used_ir = 0;
            while (cj.pc != stop_pc && g2++ < 1000) {
                cv1k_ir_block_fn fn = NULL; void *bm = NULL; size_t bc = 0;
                cv1k_ir_compile_block(&m.bus, cj.pc, &fn, &bm, &bc);
                if (fn) { fn(&cj, &m.bus); used_ir = 1; }
                else sh7709s_step(&cj, &m.bus);
                if (bm) cv1k_ir_free_block(bm, bc);
            }
            if (!used_ir) continue;
            ran++;
            cv1k_u16 o1[1]={is_uncond ? prog[0] : prog[1]}; const char*nm[1]={is_uncond ? "uncond-branch" : "branch"};
            if (!cmp_state(&ci, &cj, o1, 1, nm)) { fail++; if (fail>=20) break; }
        }
        printf("jit-difftest: mode=BRANCH ran=%ld fail=%ld\n", ran, fail);
        cv1k_machine_shutdown(&m); return fail ? 1 : 0;
    }

    int loop_test = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "loop") == 0) loop_test = 1;
    if (loop_test) {
        /* A real loop compiled as ONE chained block (back-branch is an internal
         * jump; R0/R1/R2 stay resident across iterations):
         *   loop: ADD R1,R2 ; DT R0 ; BF loop ; <stop>
         * Run the JIT block once (it iterates internally) vs the interpreter run
         * to the same exit PC; compare CPU state. */
        cv1k_ir_set_internal_loops(1);   /* the chaining feature under test */
        static const cv1k_u16 prog[] = { 0x321c, 0x4010, 0x8bfc, 0x0028 };
        cv1k_u32 boff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
        for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++) { m.main_ram[boff+i*2] = (cv1k_u8)(prog[i]>>8); m.main_ram[boff+i*2+1] = (cv1k_u8)(prog[i]&0xff); }
        cv1k_bus_invalidate_icache_all(&m.bus);
        const cv1k_u32 exit_pc = TEST_PC + 6U;     /* op index 3 (the stop op)   */
        long fail = 0, ran = 0; long n = trials ? trials : 2000000L;
        for (long t = 0; t < n; t++) {
            struct sh7709s_cpu base; randomize_cpu(&base);
            base.r[0] = 1U + (rnd() % 8U);          /* loop count 1..8 (>0)       */
            struct sh7709s_cpu ci = base, cj = base;
            int guard = 0;
            while (ci.pc != exit_pc && guard++ < 100000) sh7709s_step(&ci, &m.bus);
            cv1k_ir_block_fn fn = NULL; void *bm = NULL; size_t bc = 0;
            cv1k_u32 cov = cv1k_ir_compile_block(&m.bus, cj.pc, &fn, &bm, &bc);
            if (cov == 3 && fn) fn(&cj, &m.bus);
            cv1k_ir_free_block(bm, bc);
            if (cov != 3) { fprintf(stderr, "loop: unexpected cov=%u\n", cov); return 2; }
            ran++;
            cv1k_u16 ops1[1] = { prog[0] }; const char *nm[1] = { "loop" };
            if (cj.pc != exit_pc) { fprintf(stderr, "loop: jit pc=%08lx != exit\n", (unsigned long)cj.pc); fail++; }
            else if (!cmp_state(&ci, &cj, ops1, 1, nm)) fail++;
            if (fail >= 20) break;
        }
        printf("jit-difftest: mode=LOOP ran=%ld fail=%ld\n", ran, fail);
        cv1k_machine_shutdown(&m);
        return fail ? 1 : 0;
    }

    int mem_test = 0;
    for (int a = 1; a < argc; a++) if (strcmp(argv[a], "mem") == 0) mem_test = 1;
    if (mem_test) {
        /* Single memory op per trial; address constrained to a RAM window so the
         * access is well-defined.  RAM is snapshot/restored between the interp
         * and JIT runs; both CPU state and the RAM window are compared. */
        /* mode: 0 load @Rm, 1 store Rm,@Rn, 2 post-inc load @Rm+, 3 pre-dec store
         * Rm,@-Rn, 4 indexed load @(R0,Rm), 5 disp load @(d,Rm),Rn, 6 disp store
         * Rm,@(d,Rn), 7 disp load @(d,Rm),R0, 8 disp store R0,@(d,Rm), 9/10 PC-rel
         * load @(d,PC), 12 special store @-Rn, 13 special load @Rn+,
         * 14 indexed store Rm,@(R0,Rn). */
        static const struct { cv1k_u16 base; int mode, sz; const char *name; } mt[] = {
            { 0x6000, 0, 1, "MOVB@Rm" },  { 0x6001, 0, 2, "MOVW@Rm" },  { 0x6002, 0, 4, "MOVL@Rm" },
            { 0x2000, 1, 1, "MOVBRm@" },  { 0x2001, 1, 2, "MOVWRm@" },  { 0x2002, 1, 4, "MOVLRm@" },
            { 0x6004, 2, 1, "MOVB@Rm+" }, { 0x6005, 2, 2, "MOVW@Rm+" }, { 0x6006, 2, 4, "MOVL@Rm+" },
            { 0x2004, 3, 1, "MOVBRm@-" }, { 0x2005, 3, 2, "MOVWRm@-" }, { 0x2006, 3, 4, "MOVLRm@-" },
            { 0x000c, 4, 1, "MOVB@R0Rm" },{ 0x000d, 4, 2, "MOVW@R0Rm" },{ 0x000e, 4, 4, "MOVL@R0Rm" },
            { 0x0004,14, 1, "MOVBRm@R0Rn" },{ 0x0005,14, 2, "MOVWRm@R0Rn" },{ 0x0006,14, 4, "MOVLRm@R0Rn" },
            { 0x5000, 5, 4, "MOVL@dRm" }, { 0x1000, 6, 4, "MOVLRm@d" },
            { 0x8400, 7, 1, "MOVB@dRm0" },{ 0x8500, 7, 2, "MOVW@dRm0" },
            { 0x8000, 8, 1, "MOVB0@dRm" },{ 0x8100, 8, 2, "MOVW0@dRm" },
            { 0x9000, 9, 2, "MOVW@dPC" }, { 0xd000, 10, 4, "MOVL@dPC" },
            { 0x4002, 12, 4, "STSMACH@-" },{ 0x4012, 12, 4, "STSMACL@-" },{ 0x4022, 12, 4, "STSPR@-" },
            { 0x4003, 12, 4, "STCSR@-" },  { 0x4013, 12, 4, "STCGBR@-" }, { 0x4023, 12, 4, "STCVBR@-" },
            { 0x4006, 13, 4, "LDSMACH+" }, { 0x4016, 13, 4, "LDSMACL+" }, { 0x4026, 13, 4, "LDSPR+" },
            { 0x4017, 13, 4, "LDCGBR+" },  { 0x4027, 13, 4, "LDCVBR+" },
        };
        const int NMT = (int)(sizeof(mt) / sizeof(mt[0]));
        const cv1k_u32 WIN = CV1K_ADDR_WORK_RAM + 0x4000U, WINSZ = 256U;
        cv1k_u8 *winram = m.main_ram + (WIN - CV1K_ADDR_WORK_RAM);
        long fail = 0, ran = 0, skipped = 0; long n = trials ? trials : 4000000L;
        for (long t = 0; t < n; t++) {
            int k = (int)(rnd() % (cv1k_u32)NMT);
            int mode = mt[k].mode; unsigned sz = (unsigned)mt[k].sz;
            unsigned rn = rnd() & 15U, rm = rnd() & 15U;
            if (mode == 4) rm = 1U + (rnd() % 15U);          /* indexed load: Rm != R0 */
            if (mode == 14) rn = 1U + (rnd() % 15U);         /* indexed store: Rn != R0 */
            unsigned disp = (mode == 9 || mode == 10) ? (rnd() & 0x3fU) : (rnd() & 0xfU);
            cv1k_u16 op;
            switch (mode) {
            case 5: case 6: op = (cv1k_u16)(mt[k].base | (rn << 8) | (rm << 4) | disp); break;
            case 7: case 8: op = (cv1k_u16)(mt[k].base | (rm << 4) | disp); break;  /* dest/data = R0 */
            case 9: case 10:op = (cv1k_u16)(mt[k].base | (rn << 8) | disp); break;  /* PC-relative   */
            case 12: case 13:op = (cv1k_u16)(mt[k].base | (rn << 8)); break;          /* special @Rn   */
            default:        op = (cv1k_u16)(mt[k].base | (rn << 8) | (rm << 4));
            }
            cv1k_u32 toff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM);
            m.main_ram[toff] = (cv1k_u8)(op >> 8); m.main_ram[toff + 1] = (cv1k_u8)(op & 0xff);
            m.main_ram[toff + 2] = 0; m.main_ram[toff + 3] = 0;
            cv1k_bus_invalidate_icache_all(&m.bus);
            for (cv1k_u32 i = 0; i < WINSZ; i++) winram[i] = (cv1k_u8)rnd();
            cv1k_u8 snap[256]; memcpy(snap, winram, WINSZ);

            struct sh7709s_cpu base; randomize_cpu(&base);
            cv1k_u32 addr = WIN + (cv1k_u32)((rnd() % 56U) * 4U);    /* aligned, in-window */
            switch (mode) {
            case 0: case 2: base.r[rm] = addr; break;               /* access at Rm        */
            case 1:         base.r[rn] = addr; break;               /* access at Rn        */
            case 3:         base.r[rn] = addr + (cv1k_u32)sz; break;/* access at Rn-sz     */
            case 4:         base.r[0] = addr; base.r[rm] = 0; break;/* access at R0+Rm     */
            case 14:        base.r[0] = addr; base.r[rn] = 0; break;/* access at R0+Rn     */
            case 5:         base.r[rm] = addr - disp * 4U; break;   /* access at Rm+d*4    */
            case 6:         base.r[rn] = addr - disp * 4U; break;
            case 7: case 8: base.r[rm] = addr - disp * sz; break;   /* access at Rm+d*sz   */
            case 9: case 10: break;                                 /* address is PC-fixed */
            case 12:        base.r[rn] = addr + 4U; break;           /* access at Rn-4      */
            case 13:        base.r[rn] = addr; break;                /* access at Rn        */
            }
            struct sh7709s_cpu ci = base, cj = base;

            sh7709s_step(&ci, &m.bus);
            cv1k_u8 win_i[256]; memcpy(win_i, winram, WINSZ);
            memcpy(winram, snap, WINSZ);                              /* restore for JIT */

            cv1k_ir_block_fn fn = NULL; void *bm = NULL; size_t bc = 0;
            cv1k_u32 cov = cv1k_ir_compile_block(&m.bus, cj.pc, &fn, &bm, &bc);
            if (cov == 1 && fn) fn(&cj, &m.bus);
            cv1k_ir_free_block(bm, bc);
            if (cov != 1) { skipped++; continue; }
            ran++;
            cv1k_u16 ops1[1] = { op }; const char *nm[1] = { mt[k].name };
            if (!cmp_state(&ci, &cj, ops1, 1, nm)) { fail++; }
            else if (memcmp(win_i, winram, WINSZ) != 0) {
                fprintf(stderr, "MISMATCH (RAM) op %s(%04x)\n", mt[k].name, op); fail++;
            }
            if (fail >= 50) { fprintf(stderr, "...stop after 50\n"); break; }
        }
        printf("jit-difftest: mode=MEM ran=%ld skipped=%ld fail=%ld\n", ran, skipped, fail);
        cv1k_machine_shutdown(&m);
        return fail ? 1 : 0;
    }

    /* In IR mode, restrict generated ops to the phase-1 IR-lowerable subset. */
    int iridx[NTMPL]; int niridx = 0;
    for (int i = 0; i < NTMPL; i++)
        if (cv1k_ir_phase1_supported(g_tmpl[i].base)) iridx[niridx++] = i;
    if (ir_mode && niridx == 0) { fprintf(stderr, "no IR-supported templates\n"); return 2; }
    printf("jit-difftest: mode=%s ir-supported-templates=%d\n", ir_mode ? "IR" : "legacy", niridx);

    long fail = 0, skipped = 0, ran = 0;
    for (long t = 0; t < trials; t++) {
        int nops = 1 + (int)(rnd() % MAX_OPS);
        cv1k_u16 ops[MAX_OPS];
        const char *names[MAX_OPS];
        for (int i = 0; i < nops; i++) {
            const struct tmpl *tm = ir_mode ? &g_tmpl[iridx[rnd() % (cv1k_u32)niridx]]
                                            : &g_tmpl[rnd() % (cv1k_u32)NTMPL];
            ops[i] = gen_op(tm);
            names[i] = tm->name;
            cv1k_u32 off = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM) + (cv1k_u32)i * 2U;
            m.main_ram[off]     = (cv1k_u8)(ops[i] >> 8);  /* big-endian opcode */
            m.main_ram[off + 1] = (cv1k_u8)(ops[i] & 0xff);
        }
        /* 0x0000 terminator: not a terminal branch and not supported_linear, so
         * the JIT block ends after exactly nops ops. */
        cv1k_u32 toff = (cv1k_u32)(TEST_PC - CV1K_ADDR_WORK_RAM) + (cv1k_u32)nops * 2U;
        m.main_ram[toff] = 0; m.main_ram[toff + 1] = 0;
        /* The JIT compiles via cv1k_bus_fetch16 (icached) while the interpreter
         * fetches RAM directly; drop stale icache lines for the rewritten ops. */
        cv1k_bus_invalidate_icache_all(&m.bus);

        struct sh7709s_cpu base; randomize_cpu(&base);
        struct sh7709s_cpu ci = base, cj = base;

        for (int i = 0; i < nops; i++) sh7709s_step(&ci, &m.bus);

        cv1k_u32 covered;
        if (ir_mode) {
            cv1k_ir_block_fn fn = NULL; void *mem = NULL; size_t cap = 0;
            covered = cv1k_ir_compile_block(&m.bus, cj.pc, &fn, &mem, &cap);
            if (covered == (cv1k_u32)nops && fn) fn(&cj, &m.bus);
            cv1k_ir_free_block(mem, cap);
        } else {
            sh7709s_c23jit_reset();
            covered = cv1k_sh3_jit_test_run_block(&cj, &m.bus);
        }
        if (covered != (cv1k_u32)nops) { skipped++; continue; }

        ran++;
        if (!cmp_state(&ci, &cj, ops, nops, names)) { fail++; if (fail >= 50) { fprintf(stderr, "...stopping after 50 mismatches\n"); break; } }
    }

    printf("jit-difftest: trials=%ld ran=%ld skipped=%ld fail=%ld\n", trials, ran, skipped, fail);
    cv1k_machine_shutdown(&m);
    return fail ? 1 : 0;
}
