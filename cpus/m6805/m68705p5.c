/*
 * Motorola MC68705P5 - see m68705p5.h for porting notes.
 *
 * Port register read-back (port_r), DDR/latch write behaviour
 * (port_latch_w/port_ddr_w/port_cb_w) and the timer/counter
 * (m6805_timer::update/tcr_w) are ported from MAME's m68705.cpp
 * (m6805_hmos_device / m6805_timer - license:BSD-3-Clause,
 * copyright-holders:Vas Crabb).
 */

#include <string.h>
#include "m68705p5.h"

/* ============================================================
 * m6805_config for the M68705P5
 *
 * From m68705p_device's constructor chain (m68705.cpp):
 *   m68705p_device(...) : m68705_device(..., addr_width=11, ram_size=112)
 *   m6805_hmos_device(..., addr_width, ram_size)
 *     : m6805_hmos_device(..., { addr_width>13 ? s_hmos_b_ops : s_hmos_s_ops,
 *                                 s_hmos_cycles, addr_width, 0x007f, 0x0060,
 *                                 M6805_VECTOR_SWI }, ram_size)
 * addr_width=11 <= 13, so it's the "small" HMOS op table (irrelevant
 * distinction in this port - see m6805.h, we always mask through
 * addr_mask instead of templating). sp_mask=0x7F, sp_floor=0x60,
 * vector_swi = M6805_VECTOR_SWI(0xfffc) & vector_mask((1<<11)-1=0x7ff)
 * = 0x7fc. The other vectors (reset/int/timer) are MAME constants
 * M6805_VECTOR_RESET=0xfffe, M6805_VECTOR_INT=0xfffa,
 * M6805_VECTOR_TIMER=0xfff8, all masked the same way.
 * ============================================================ */

static const m6805_config M68705P5_CONFIG = {
    0x7ff,  /* addr_mask:    11-bit address space              */
    0x7f,   /* sp_mask:      SP wraps within page 0             */
    0x60,   /* sp_floor:     stack occupies 0x60-0x7F           */
    0x7fe,  /* vector_reset: 0xfffe & 0x7ff                     */
    0x7fc,  /* vector_swi:   0xfffc & 0x7ff                     */
    0x7fa,  /* vector_int:   0xfffa & 0x7ff (M6805_VECTOR_INT)  */
    0x7f8   /* vector_timer: 0xfff8 & 0x7ff (M6805_VECTOR_TIMER)*/
};

/* Port C is only 4 bits wide on Px parts (set_port_mask<2>(0xf0) in
 * m68705p_device's ctor): bits 4-7 always read as 1 and are not
 * affected by ddr/latch. Ports A and B are full 8 bits (mask 0x00). */
static const uint8_t PORT_MASK[3] = { 0x00, 0x00, 0xf0 };

/* ============================================================
 * Reset
 * ============================================================ */

void m68705p5_reset(m68705p5_state *s, const uint8_t *rom) {
    s->user_rom = rom;

    memset(s->port_latch, 0xff, sizeof(s->port_latch)); /* m_port_latch{0xff,...} */
    memset(s->port_ddr, 0x00, sizeof(s->port_ddr));       /* port_ddr_w<N>(0x00) on reset */
    memset(s->port_input, 0xff, sizeof(s->port_input));   /* m_port_input{0xff,...} */

    s->pcr = 0xff;
    /* m68705_device::device_reset(): m_pcr |= 0xfb (bit 2 /VPON driven
       externally & unaffected by reset - we never assert it, so this
       collapses to pcr=0xff, matching the ctor default anyway) */

    /* Timer: get_mask_options() reads rom[0x0784] for Px parts
       (m68705p3_device masks off bit 3/SNM, m68705p5_device does not -
       see get_mask_options() overrides in m68705.cpp). MOR_TOPT
       (0x40) selects between TIMER_MOR (mask-option-configured) and
       TIMER_PGM (software-configured via TCR) - real EPROM-programmed
       68705 firmware always ships with a MOR byte matching what the
       target board's timer usage expects, so we always honour it
       (matches MAME: m68705_device::device_start() checks options &
       MOR_TOPT unconditionally). */
    {
        uint8_t mor = rom[0x784];
        if (mor & 0x40 /* MOR_TOPT */) {
            s->timer_divisor = mor & 0x07;       /* MOR_PS */
            s->timer_source_ext = (uint8_t)((mor >> 5) & 0x01); /* MOR_CLS bit */
            /* MOR_TIE (bit 4) would select CLOCK_TIMER vs CLOCK when
               CLS=0; Taito hookups don't wire the TIMER pin, so we
               only need to distinguish "runs off the internal clock
               unconditionally" (CLS=0, i.e. timer_source_ext=0) from
               "needs the external pin too" (CLS=1) - see update(). */
        } else {
            /* TIMER_PGM: divisor/source come from TCR writes instead
               (tcr_w below); start at divisor=7/internal, matching
               m6805_timer's ctor defaults (m_divisor(7), m_source(CLOCK)). */
            s->timer_divisor = 7;
            s->timer_source_ext = 0;
        }
    }
    s->timer_prescale = 0x7f;
    s->timer_tdr = 0xff;
    s->timer_tcr = 0x7f;

    m6805_reset(&s->cpu, &M68705P5_CONFIG);

    /* DEFENSE IN DEPTH (found 2026-09-19 while debugging flstory's first
     * boot - see flstory.cpp's own, much longer writeup of the REAL bug
     * and fix, at its m6805_read()/m6805_write() definitions): the generic
     * m6805_reset() above does `s->PC = rd16(s, cfg->vector_reset)`, which
     * goes through rd()/m6805_read() - the GLOBAL per-CPU-family extern
     * callback that m6805.h's header comment says can be provided either
     * by the machine OR by "the variant/glue layer sitting between the
     * machine and this core" (this file, m68705p5.c, is exactly that
     * layer - but does NOT actually define m6805_read/m6805_write itself,
     * per m68705p5.h's own "Integration model" note: it exposes
     * m68705p5_mem_read/write instead, for the OWNING MACHINE to wire up).
     * flstory.cpp originally left its own m6805_read/m6805_write as dead
     * stubs returning 0xFF unconditionally, wrongly believing
     * m68705p5_mem_read/write "fully decode the MCU's address space before
     * falling through to this" - they don't; that path is HOST-side only
     * (main CPU reads/writes the MCU's data port), while m6805_step()'s own
     * instruction fetches (and this reset-vector read, and every
     * interrupt-vector read / return-address push in m6805.c's
     * service_interrupt()) go through the plain global hook directly, with
     * NO m68705p5 involvement at all - so with the stubs returning 0xFF,
     * the reset vector read as 0xFFFF and every subsequent opcode fetch
     * read 0xFF (a real opcode, "STX indexed") instead of the MCU's actual
     * firmware. The real fix is flstory.cpp now correctly forwarding its
     * m6805_read/m6805_write to m68705p5_mem_read/write on the single
     * active MCU instance - once that's done, m6805_reset()'s rd16() call
     * above already resolves correctly on its own, making this override
     * redundant for flstory today. It is kept anyway as a narrow safety
     * net specifically for THIS one register (PC at reset): a FUTURE
     * m68705p5-based machine that repeats flstory's original mistake (an
     * m6805_read/m6805_write stub that doesn't forward to
     * m68705p5_mem_read/write) would still get a broken reset vector
     * silently, which is a uniquely hard failure mode to notice (the CPU
     * "runs" and its PC moves, it just never executes anything real - see
     * flstory.cpp's comment for exactly how long that took to diagnose
     * here). This does NOT fix the equivalent gap for interrupt vectors or
     * interrupt-time register pushes (service_interrupt() in m6805.c) -
     * those still depend on the owning machine wiring m6805_read/write
     * correctly; only the reset vector gets a redundant local guarantee
     * here, since it's the one value this function already has everything
     * it needs to compute independently. Vectors are stored big-endian,
     * matching rd16()'s own (hi<<8)|lo assembly and M6805_VECTOR_RESET's
     * documented byte order. */
    s->cpu.PC = (uint16_t)((rom[M68705P5_CONFIG.vector_reset] << 8) |
                            rom[(M68705P5_CONFIG.vector_reset + 1) & M68705P5_CONFIG.addr_mask]);
}

/* ============================================================
 * Port read-back (m6805_hmos_device::port_r<N>)
 *   return mask | (latch & ddr) | (input & ~ddr)
 * ============================================================ */

static uint8_t port_r(const m68705p5_state *s, unsigned n) {
    uint8_t mask = PORT_MASK[n];
    return (uint8_t)(mask | (s->port_latch[n] & s->port_ddr[n]) | (s->port_input[n] & (uint8_t)~s->port_ddr[n]));
}

uint8_t m68705p5_port_output(const m68705p5_state *s, unsigned port) {
    uint8_t ddr = s->port_ddr[port];
    uint8_t latch = s->port_latch[port];
    if (port == M68705P5_PORT_A) {
        /* open-drain w/ pull-up: driven low only where ddr=1 AND
           latch=0 (m6805_hmos_device::port_cb_w<N> open-drain case:
           data = latch | ~ddr ; only bits that are 0 in `data` pull
           the shared line low - i.e. ddr=1 & latch=0). Everywhere
           else floats high (pulled up), matching a real open-drain
           bus with pull-ups. */
        return (uint8_t)(latch | (uint8_t)~ddr);
    }
    /* push-pull ports B/C: driven bits show their latch value,
       floating (input/undriven) bits read high. */
    return (uint8_t)((latch & ddr) | (uint8_t)~ddr);
}

/* ============================================================
 * Timer/counter (m6805_timer::update/tcr_w in m68705.cpp)
 * ============================================================ */

static void timer_tcr_w(m68705p5_state *s, uint8_t data) {
    /* if (m_options & TIMER_MOR) data |= TCR_TIE; - MOR-configured
       parts force TIE on for any TCR write (only TIR/PSC remain
       meaningfully writable-ish); we don't separately track "options"
       since m68705p5_reset() already folded MOR into timer_divisor/
       timer_source_ext once - TIMER_MOR games (the overwhelming
       majority of real EPROM firmware) never rely on TCR to change
       divisor/source at runtime, only to arm/clear the timer IRQ, so
       forcing TIE and leaving divisor/source alone here matches
       observable behaviour. */
    data |= M68705P5_TCR_TIE;

    if ((data & M68705P5_TCR_PSC))
        s->timer_prescale = 0;

    /* m_tcr = (m_tcr & (data & TCR_TIR)) | (data & ~(TCR_TIR|TCR_PSC));
       i.e.: TIR can only be CLEARED by this write (AND with the
       incoming TIR bit), never SET by a plain register write; every
       other bit (including TIE we just forced) is replaced outright. */
    s->timer_tcr = (uint8_t)((s->timer_tcr & (data & M68705P5_TCR_TIR)) |
                              (data & (uint8_t)~(M68705P5_TCR_TIR | M68705P5_TCR_PSC)));

    /* level-sensitive: raise/lower M6805_INT_TIMER to match TIR && !TIM right now */
    if ((s->timer_tcr & M68705P5_TCR_TIR) && !(s->timer_tcr & M68705P5_TCR_TIM))
        s->cpu.pending_interrupts |= (1u << M6805_INT_TIMER);
    else
        s->cpu.pending_interrupts &= (uint8_t)~(1u << M6805_INT_TIMER);
}

/* Advance the timer by `count` CPU cycles (called once per opcode
 * from m68705p5_step(), matching MAME's per-opcode burn_cycles()). */
static IRAM_ATTR void timer_update(m68705p5_state *s, unsigned count) {
    /* DISABLED source doesn't exist in this port (nothing sets it -
       TIMER_MOR/TIMER_PGM both start at CLOCK); the only gate is
       "needs external pin but nothing drives it", which for a Taito
       MCU hookup means timer_source_ext=1 => never runs (matches
       CLOCK_TIMER's `!m_timer` guard: m_timer starts true in MAME's
       ctor, meaning the external pin has to go through at least one
       actual transition to end up false and gate the clock; since
       nothing ever calls a timer_w() here, source_ext=1 in practice
       behaves like DISABLED for the games this port targets). */
    if (s->timer_source_ext)
        return;

    unsigned prescale = (s->timer_prescale & ((1u << s->timer_divisor) - 1)) + count;
    unsigned decrements = prescale >> s->timer_divisor;

    int interrupt = (s->timer_tdr ? (int)s->timer_tdr : 256) <= (int)decrements;

    s->timer_prescale = (uint8_t)(prescale & 0x7f);
    s->timer_tdr = (uint8_t)(s->timer_tdr - decrements);

    if (interrupt) {
        s->timer_tcr |= M68705P5_TCR_TIR;
        if (!(s->timer_tcr & M68705P5_TCR_TIM))
            s->cpu.pending_interrupts |= (1u << M6805_INT_TIMER);
    }
}

/* ============================================================
 * Memory decode (m6805_hmos_device::internal_map /
 * m68705p_device::internal_map, m68705_device::internal_map)
 * ============================================================ */

uint8_t m68705p5_mem_read(m68705p5_state *s, uint16_t addr) {
    addr &= 0x7ff;
    switch (addr) {
        case M68705P5_REG_PORTA: return port_r(s, M68705P5_PORT_A);
        case M68705P5_REG_PORTB: return port_r(s, M68705P5_PORT_B);
        case M68705P5_REG_PORTC: return port_r(s, M68705P5_PORT_C);
        case 0x03: return 0xff; /* Port D doesn't exist on Px parts */
        case M68705P5_REG_DDRA: case M68705P5_REG_DDRB: case M68705P5_REG_DDRC:
            return 0xff; /* DDRs are write-only, read as 0xFF */
        case 0x07: return 0xff; /* unused */
        case M68705P5_REG_TDR: return s->timer_tdr;
        case M68705P5_REG_TCR: return s->timer_tcr;
        case 0x0a: return 0xff; /* MISC - Rx/Sx/Ux only, not on Px */
        case M68705P5_REG_PCR: return s->pcr;
        case 0x0c: case 0x0d: return 0xff; /* unused */
        case 0x0e: case 0x0f: return 0xff; /* A/D - Rx/Sx only, not on Px */
        default:
            if (addr < 0x80) {
                /* 0x10-0x7F internal RAM (0x0C-0x0F handled above as
                   "unused" per m68705p_device's ram_size=112 leaving a
                   0x10-wide hole - matches MAME's map(0x80-ram_size,
                   0x7f).ram() = map(0x10, 0x7f) for ram_size=112) */
                return s->ram[addr - 0x10];
            }
            /* 0x80-0x7FF: user ROM (EPROM), vectors included */
            return s->user_rom[addr];
    }
}

void m68705p5_mem_write(m68705p5_state *s, uint16_t addr, uint8_t val) {
    addr &= 0x7ff;
    switch (addr) {
        case M68705P5_REG_PORTA: {
            uint8_t data = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_A]);
            s->port_latch[M68705P5_PORT_A] = data;
            return;
        }
        case M68705P5_REG_PORTB: {
            uint8_t data = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_B]);
            s->port_latch[M68705P5_PORT_B] = data;
            return;
        }
        case M68705P5_REG_PORTC: {
            uint8_t data = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_C]);
            s->port_latch[M68705P5_PORT_C] = data;
            return;
        }
        case M68705P5_REG_DDRA:
            s->port_ddr[M68705P5_PORT_A] = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_A]);
            return;
        case M68705P5_REG_DDRB:
            s->port_ddr[M68705P5_PORT_B] = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_B]);
            return;
        case M68705P5_REG_DDRC:
            s->port_ddr[M68705P5_PORT_C] = (uint8_t)(val & (uint8_t)~PORT_MASK[M68705P5_PORT_C]);
            return;
        case M68705P5_REG_TDR:
            s->timer_tdr = val; /* acts as plain RAM while not counting, per datasheet - modeled as always-writable */
            return;
        case M68705P5_REG_TCR:
            timer_tcr_w(s, val);
            return;
        case M68705P5_REG_PCR:
            /* Plain register write, no EPROM-programming side effect -
               see m68705p5.h header note. Bit 0 (/PLE) forces bit 1
               (/PGE) same as MAME (data |= (data&1)<<1) so a reader
               polling /PGE sees consistent lockout behaviour even
               though nothing is actually programmed. */
            val = (uint8_t)(val | ((val & 0x01) << 1));
            s->pcr = (uint8_t)((s->pcr & 0xfc) | (val & 0x03));
            return;
        default:
            if (addr < 0x80) {
                /* 0x10-0x7F internal RAM - see read-side note */
                if (addr >= 0x10)
                    s->ram[addr - 0x10] = val;
                return;
            }
            /* ROM is not writable (EPROM programming not modeled) */
            return;
    }
}

/* ============================================================
 * Step
 * ============================================================ */

int m68705p5_step(m68705p5_state *s, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        int c = m6805_step(&s->cpu, 1);
        timer_update(s, (unsigned)c);
        total += c;
    }
    return total;
}

void m68705p5_irq(m68705p5_state *s, int asserted) {
    m6805_irq(&s->cpu, asserted);
}

void m68705p5_port_input_w(m68705p5_state *s, unsigned port, uint8_t data) {
    s->port_input[port] = data;
}
