#ifndef CPU_80C32_H
#define CPU_80C32_H

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

struct cpu_80c32 {
    uint16_t pc;
    uint8_t  a;
    uint8_t  b;
    uint8_t  sp;
    uint8_t  psw;
    uint16_t dptr;
    uint16_t t0;
    uint16_t t1;
    uint16_t t2; /* Timer 2 (8052/80C32-only) live 16-bit count, mirrors
                    SFRs TL2/TH2 (0xCC/0xCD) the same way t0/t1 mirror
                    TL0/TH0 and TL1/TH1. */
    uint32_t cycles;
    uint8_t  irq_pending;
    uint8_t  in_irq;          /* != 0 while ANY ISR is in service. Kept as a
                                 shadow of (irq_depth != 0) so external readers
                                 and debug hooks that predate the priority
                                 support still work. Do NOT gate new logic on
                                 this -- use irq_depth / irq_level instead. */

    /* ---- 2-level interrupt priority / nesting (see check_interrupts) ----
       Real 8051/8052 interrupts have two priority levels selected per-source
       by the IP SFR (0xB8). A HIGH-level request preempts a running LOW-level
       ISR; nothing preempts a HIGH-level ISR; a LOW request cannot preempt a
       LOW ISR (nor HIGH-vs-HIGH). So at most two ISRs are ever nested.
       irq_level[] is that in-service stack (irq_level[0] = outer, [1] = inner),
       irq_depth is how many entries are valid (0, 1 or 2). RETI pops one.
       A machine that drove interrupts fine before this existed still works:
       with IP left at 0 every source is LOW, depth caps at 1, and the
       behaviour collapses back to the old single-in_irq model. */
    uint8_t  irq_depth;
    uint8_t  irq_level[2];

    uint8_t  ram[256];          /* 0x00-0x7F: lower internal RAM (direct/indirect).
                                    0x80-0xFF: SFR space (direct addressing only). */
    uint8_t  iram_hi[128];      /* 0x80-0xFF indirect-addressed upper internal RAM.
                                    On real 8052/80C32 silicon this range is dual-mapped:
                                    direct addressing (MOV dir, SETB bit, ...) hits the
                                    SFRs above; indirect addressing (@R0/@R1, and the
                                    stack via PUSH/POP/SP) hits this physically separate
                                    128-byte bank instead. 8052 code routinely runs its
                                    stack up past 0x7F relying on that separation -- without
                                    it, a deep call chain silently corrupts live SFRs. */
    const uint8_t *code;
    uint32_t code_size;
    uint8_t  *xdata;
    uint32_t xdata_size;
    
    /* Callbacks */
    uint8_t  (*xread)(struct cpu_80c32 *c, uint16_t addr);
    void     (*xwrite)(struct cpu_80c32 *c, uint16_t addr, uint8_t val);
    uint8_t  (*sfr_r)(struct cpu_80c32 *c, uint8_t addr);
    void     (*sfr_w)(struct cpu_80c32 *c, uint8_t addr, uint8_t val);
    /* Optional debug hook: fires on every write to internal RAM (addr<0x80,
       direct or @Ri indirect and register-bank writes). Lets a machine
       watch a specific low-RAM flag byte without patching the core. */
    void     (*iram_write)(struct cpu_80c32 *c, uint8_t addr, uint8_t val);
    /* Optional debug hook: fires after RETI, once c->pc has been restored
       to the interrupted mainline address. Lets a machine see where the
       *non-interrupt* code sits between ISR firings, even when the ISR
       runs so often that PC-sampling never catches anything else. */
    void     (*post_reti)(struct cpu_80c32 *c);
    /* Optional debug hook: fires at the start of every instruction, before
       it executes, with c->pc still pointing at the not-yet-fetched
       opcode. Lets a machine set PC "checkpoints" to empirically confirm
       whether/how far execution reaches a hand-disassembled code path,
       without needing an xdata access to piggyback on. */
    void     (*pc_trace)(struct cpu_80c32 *c);
    void     *userdata;
};

/* Function prototypes */
void cpu_80c32_init(struct cpu_80c32 *c);
void cpu_80c32_reset(struct cpu_80c32 *c);
int  cpu_80c32_step(struct cpu_80c32 *c);
void cpu_80c32_run(struct cpu_80c32 *c, int cycles);
void cpu_80c32_set_irq0(struct cpu_80c32 *c, int state);

#ifdef __cplusplus
}
#endif

#endif /* CPU_80C32_H */