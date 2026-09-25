/*
 * M6805 CPU Emulator for Galagino
 *
 * Ported from MAME's m6805_base_device (src/devices/cpu/m6805/m6805.cpp,
 * m6805.h, m6805defs.h, 6805ops.hxx - license:BSD-3-Clause,
 * copyright-holders:Aaron Giles, Vas Crabb), adapted to this project's
 * plain-struct + step()-loop + external-callback style (see m6809.h for
 * the pattern this follows).
 *
 * This header is the GENERAL 6805 execution engine: the instruction set,
 * addressing modes and interrupt mechanics shared by every 6805/68HC05/
 * 68705 family member. A concrete chip (e.g. M68705P5, see m68705p5.h)
 * supplies the parts MAME splits into m6805_base_device::configuration_params
 * (op table selection, cycle table, address width, stack pointer mask/
 * floor, vector mask) plus its own peripherals (ports, timer, EPROM) on
 * top of this core.
 *
 * Two op-table families exist in MAME for the instruction set itself:
 *   - "HMOS" tables (s_hmos_s_ops/s_hmos_b_ops): plain 6805/68705 - no
 *     MUL, no STOP/WAIT implemented (fatalerror stubs in MAME).
 *   - "HC"   tables (s_hc_s_ops/s_hc_b_ops): 68HC05 - adds MUL at $42.
 * The M68705P5 used by Taito's flstory (and by the taito68705_mcu glue)
 * is HMOS, so only the HMOS opcode semantics are implemented here for
 * now (MUL therefore is NOT implemented - matches MAME's s_hmos_*_ops,
 * where $42 is illegal). Extending to HC-family chips later means adding
 * MUL and switching the config's op-table selection; nothing else in
 * this core changes (see 6805ops.hxx's `mul` in MAME for reference).
 *
 * The "s"/"b" (small/big) address-space split MAME uses to select 13-bit
 * vs 16-bit memory access templates is instead handled here by mapping
 * PC/EA through addr_mask on every memory access - simpler at the cost
 * of one extra AND per access, which is irrelevant at 6805 instruction
 * rates. Address width and vector locations remain per-variant (config).
 *
 * NOT ported (unneeded by any Taito-68705-MCU game, MAME marks them
 * fatalerror/"unimplemented" too):
 *   - STOP ($8E), WAIT ($8F): MAME's own HMOS handlers are stubs that
 *     abort emulation; flstory's MCU program never executes them.
 *
 * Interrupt model implemented is the M6805_hmos_device 2-source scheme
 * (external /INT latched + level-sensitive timer/counter), which is what
 * every 68705Px/Rx/Ux chip uses (see m68705.cpp's m6805_hmos_device::
 * interrupt); the simpler 1-source m6805_base_device::interrupt (plain
 * mask-rom 6805/6805U-family parts with only M6805_IRQ_LINE) is not
 * needed for the 68705 family this port targets and is not implemented.
 */

#ifndef M6805_H
#define M6805_H

#include <stdint.h>
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Condition Code register bits (m6805.h CC masks: H INZC, bits 7654 3210) */
#define M6805_CC_C  0x01  /* Carry */
#define M6805_CC_Z  0x02  /* Zero */
#define M6805_CC_N  0x04  /* Negative */
#define M6805_CC_I  0x08  /* IRQ mask */
#define M6805_CC_H  0x10  /* Half carry */
/* bits 5-7 unused, MAME's state_string_export shows them as '?' */

/* Interrupt source indices, matching MAME's m6805_hmos_device scheme:
 *   M6805_IRQ_LINE (0)      - external /INT pin, edge-latched
 *   M6805_INT_TIMER (1)     - on-chip timer/counter, level-sensitive
 * (M68705_VPP_LINE/VIHTP_LINE from m68705.h are EPROM-programming-only
 * inputs, not used during normal program execution, and are not modeled
 * here - this core always runs with Vpp low / VIHTP clear, i.e. normal
 * "execute the mask/EPROM program" mode, never bootstrap/programming
 * mode.) */
#define M6805_IRQ_LINE    0
#define M6805_INT_TIMER   1

/* Per-variant configuration, filling the role of MAME's
 * m6805_base_device::configuration_params. A concrete chip (m68705p5.c)
 * fills one of these in and passes it to m6805_reset()/every step. */
typedef struct m6805_config_S {
    uint32_t addr_mask;     /* memory/PC address mask, e.g. 0x7FF for an 11-bit part */
    uint8_t  sp_mask;       /* stack pointer mask, e.g. 0x7F (S wraps within page 0) */
    uint8_t  sp_floor;      /* stack pointer low-water mark, e.g. 0x60 */
    uint16_t vector_reset;  /* reset vector address (already within addr_mask) */
    uint16_t vector_swi;    /* SWI vector address */
    uint16_t vector_int;    /* external /INT vector address */
    uint16_t vector_timer;  /* timer/counter interrupt vector address */
} m6805_config;

typedef struct m6805_state_S {
    uint8_t  A;           /* Accumulator */
    uint8_t  X;           /* Index register (8-bit on 6805, unlike 6809's 16-bit X) */
    uint16_t PC;          /* Program counter */
    uint8_t  S;            /* Stack pointer (low byte only ever used - see sp_mask) */
    uint8_t  CC;           /* Condition code register */

    const m6805_config *cfg; /* set once (e.g. by m68705p5_reset()) before stepping */

    int      cycles;       /* Cycles consumed by the last m6805_step() call */
    int      total_cycles; /* Total cycles consumed since reset */

    /* MAME latches interrupt requests internally (m_pending_interrupts) and
     * doesn't clear them until serviced, regardless of what the external
     * pin does afterwards - mirrored here with one pending flag per source
     * (bit 0 = M6805_IRQ_LINE, bit 1 = M6805_INT_TIMER). */
    uint8_t  pending_interrupts;

    /* Live level of the external /INT pin, separate from the edge-latched
     * pending_interrupts above. BIL/BIH ($2E/$2F) test the raw pin state
     * (MAME's test_il() reads m_irq_state[] directly, not the latch), so
     * this must be updated by m6805_irq() alongside the latch. */
    uint8_t  irq_line_level;

    /* Optional per-instance memory hooks. NULL (what m68705p5_reset() sets)
     * = use the global m6805_read/m6805_write below. A machine whose MCU must
     * not go through those globals (they are defined once, by flstory.cpp)
     * installs its own via m68705p5_reset_hooked(). */
    uint8_t (*rd_hook)(struct m6805_state_S *s, uint16_t addr);
    void    (*wr_hook)(struct m6805_state_S *s, uint16_t addr, uint8_t val);
} m6805_state;

/* Reset the CPU: clears registers, sets CC = I (interrupts masked, matches
 * m6805_base_device::device_reset()+SEI), and loads PC from cfg->vector_reset.
 * `cfg` must stay valid for the lifetime of the CPU (a variant's static/
 * const config, e.g. see m68705p5.h). */
void m6805_reset(m6805_state *s, const m6805_config *cfg);

/* Execute up to `count` instructions (same convention as m6809_step: a
 * count of instructions, not a cycle budget). Returns cycles consumed by
 * this call; s->cycles also holds that value, s->total_cycles accumulates. */
IRAM_ATTR int m6805_step(m6805_state *s, int count);

/* Assert/clear the external /INT line (M6805_IRQ_LINE). Edge-latched: a
 * pending request survives until serviced even if the line is deasserted
 * again before that happens (matches real 6805 silicon / MAME). */
void m6805_irq(m6805_state *s, int asserted);

/*
 * External callbacks - must be provided by the machine (or by the
 * variant/glue layer sitting between the machine and this core, e.g.
 * m68705p5.c intercepts these to serve its own ports/timer/EPROM before
 * forwarding anything else to the machine).
 */
extern IRAM_ATTR uint8_t m6805_read(m6805_state *s, uint16_t addr);
extern IRAM_ATTR void m6805_write(m6805_state *s, uint16_t addr, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* M6805_H */
