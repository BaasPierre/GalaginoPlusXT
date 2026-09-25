/*
 * Motorola MC68705P5 - concrete 6805-family variant for Galagino
 *
 * Ported from MAME's m68705p_device / m68705_device / m6805_hmos_device
 * (src/devices/cpu/m6805/m68705.h, m68705.cpp - license:BSD-3-Clause,
 * copyright-holders:Vas Crabb), layered on top of the general m6805
 * execution core (m6805.h/.c - see that header for what "general core
 * + variant config" means in this port).
 *
 * The P5 is a 28-pin EPROM part: 11-bit address space (0x000-0x7FF),
 * Ports A/B/C (C is 4 bits wide), one programmable timer/counter, and
 * (on real silicon) a UV-erasable EPROM holding the user program plus
 * a factory bootstrap-programmer ROM. This port only EXECUTES an
 * already-dumped program (e.g. Taito's a45-20.mcu / a52_17.54c used by
 * flstory/onna34ro - see taito68705_mcu.h) - it never programs an
 * EPROM, so the PCR programming-latch/PGE/PLE machinery from MAME's
 * m68705_device (eprom_r/eprom_w/pcr_w's write-EPROM side effect) is
 * NOT ported: user_rom here is a plain fixed ROM image, always
 * readable, never written by the CPU. PCR itself is still modeled as
 * a plain read/write register (games poll bit values), just without
 * the "writes actually alter ROM contents" side effect - the "Vpp
 * high" MAME comment path (EPROM programming) is simply never taken
 * because Vpp is always modeled as off (m_vihtp / VPP_LINE inputs are
 * not wired - see m6805.h's M6805_IRQ_LINE/M6805_INT_TIMER comment).
 * The bootstrap ROM (0x785-0x7F7, loaded from bootstrap.bin in MAME)
 * is likewise not needed - it only runs when VIHTP asserts the
 * bootstrap reset vector, which never happens here.
 *
 * Integration model: this project routes ALL memory accesses for a
 * CPU family through one pair of global extern callbacks per family
 * (m6805_read/m6805_write - see m6805.h), trampolined by
 * emulation.cpp to the single currentMachine instance (see how
 * m6809_read/m6809_write work in emulation.cpp; gng.cpp is a worked
 * example of a machine's own memory-map switch). There is no separate
 * per-peripheral callback layer anywhere else in this codebase, so
 * this variant follows suit: it does NOT install itself as the
 * m6805_read/m6805_write extern. Instead it exposes:
 *   - m68705p5_mem_read()/m68705p5_mem_write(): decode ONE m68705p5's
 *     register/RAM/ROM address space. The owning machine's own
 *     m6805_read/m6805_write override calls these directly (same
 *     pattern as gng::m6809_read dispatching into its own switch).
 *   - plain public fields (port_latch/port_ddr/port_input) the machine
 *     reads/writes directly to interact with the ports from the
 *     "outside" (e.g. the Taito MCU handshake glue in
 *     taito68705_mcu.h pokes port_input[0] and reads port_latch[1]),
 *     instead of a devcb-style function-pointer indirection.
 *
 * Register map (m68705p_syms in m68705.cpp), address 0x00-0x0B:
 *   0x00 PORTA (RW)      0x01 PORTB (RW)      0x02 PORTC (RW, 4 bits)
 *   0x04 DDRA (W)        0x05 DDRB (W)        0x06 DDRC (W, 4 bits)
 *   0x08 TDR (RW)        0x09 TCR (RW)
 *   0x0B PCR (RW)
 *   0x0C-0x7F: internal RAM (112 bytes total incl. stack 0x60-0x7F -
 *              m68705p_device ctor: ram_size=112, so RAM spans
 *              0x80-112=0x10 through 0x7F; 0x0C-0x0F unused/reads 0xFF)
 *   0x80-0x7FF: user ROM (EPROM in real hardware, plain ROM here)
 *   0x7F8-0x7FF: interrupt vectors (big-endian, part of the ROM image)
 */

#ifndef M68705P5_H
#define M68705P5_H

#include <stdint.h>
#include "m6805.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register offsets (m68705p_syms) */
#define M68705P5_REG_PORTA  0x00
#define M68705P5_REG_PORTB  0x01
#define M68705P5_REG_PORTC  0x02
#define M68705P5_REG_DDRA   0x04
#define M68705P5_REG_DDRB   0x05
#define M68705P5_REG_DDRC   0x06
#define M68705P5_REG_TDR    0x08
#define M68705P5_REG_TCR    0x09
#define M68705P5_REG_PCR    0x0B

/* TCR bits (m6805_timer::tcr_mask in m68705.cpp) */
#define M68705P5_TCR_PS   0x07 /* prescaler value */
#define M68705P5_TCR_PSC  0x08 /* prescaler clear (write-only, reads 0) */
#define M68705P5_TCR_TIE  0x10 /* timer external input enable */
#define M68705P5_TCR_TIN  0x20 /* timer input select */
#define M68705P5_TCR_TIM  0x40 /* timer interrupt mask */
#define M68705P5_TCR_TIR  0x80 /* timer interrupt request */

#define M68705P5_ROM_SIZE 0x800 /* 11-bit address space, 0x000-0x7FF */

/* Port indices into port_latch/port_ddr/port_input */
#define M68705P5_PORT_A 0
#define M68705P5_PORT_B 1
#define M68705P5_PORT_C 2

typedef struct m68705p5_state_S {
    m6805_state cpu;

    /* Full 0x000-0x7FF image as dumped from a real chip (the low
     * register/RAM area of such a dump is blank and never read from
     * here - reads under 0x80 are served by the peripheral registers/
     * RAM below, matching MAME's internal_map overriding those
     * addresses ahead of the EPROM map). Only bytes 0x080-0x7FF (code
     * + vectors) matter. Must stay valid for the MCU's lifetime
     * (caller-owned, e.g. the machine's loaded .mcu ROM - same
     * lifetime convention as m6809's rom_direct window). */
    const uint8_t *user_rom;

    /* Ports: latch (written by CPU), ddr (direction, 1=output), input
     * (driven by the owning machine to reflect what's wired to the
     * pins). Read-back combines them exactly like MAME's port_r():
     * mask | (latch & ddr) | (input & ~ddr). Port A is open-drain with
     * pull-ups (set_port_open_drain<0>(true) in MAME): m68705p5_port_
     * output() applies that behaviour (driven bits are only the ones
     * latched to 0) for glue code that needs the "wire-AND" view, e.g.
     * the Taito MCU handshake's PA0-7 host/MCU shared data latch. Port
     * C is 4 bits wide - bits 4-7 always read as 1 (set_port_mask<2>
     * (0xf0) in MAME's m68705p_device ctor). */
    uint8_t port_latch[3]; /* A, B, C */
    uint8_t port_ddr[3];
    uint8_t port_input[3];

    uint8_t pcr; /* Programming Control Register - see header note: no
                    EPROM-write side effect is modeled, it's a plain
                    register games can poll (Taito games don't use it) */

    /* Internal RAM, addresses 0x10-0x7F (112 bytes - m68705p_device's
     * ram_size; stack pointer wraps within 0x60-0x7F of this same
     * array, per cfg->sp_mask/sp_floor - see m68705p5_reset()).
     * Indexed as ram[addr - 0x10] by m68705p5_mem_read/write. */
    uint8_t ram[0x70];

    /* Timer/counter (m6805_timer in m68705.cpp, TIMER_MOR-configured:
     * divisor/source come from the Mask Option Register byte at ROM
     * offset 0x784, exactly like real EPROM-part firmware expects -
     * see m68705p5_reset()). update() runs once per m68705p5_step()
     * call proportional to cycles consumed, mirroring MAME's
     * burn_cycles()->m_timer.update(count) called after every opcode. */
    uint8_t  timer_tdr;
    uint8_t  timer_tcr;
    uint8_t  timer_prescale;
    unsigned timer_divisor;    /* MOR_PS bits 0-2, 0-7 */
    uint8_t  timer_source_ext; /* MOR_CLS: 0 = internal clock, 1 = needs
                                   external TIMER pin - Taito hookups
                                   never wire this pin, so with CLS=1
                                   the timer never fires (matches
                                   silicon/MAME: CLOCK_TIMER source
                                   needs m_timer(external level) too) */

    /* Counts the places where MAME's m6805_timer calls set_input_line(
     * M6805_INT_TIMER, ...): every TCR write (tcr_w) and every unmasked
     * timer interrupt (update). In MAME that call is synced: it puts a timer
     * at that moment and ends the MCU's timeslice. A machine that models
     * MAME's scheduler watches these to do the same; nothing in the core
     * depends on them. timer_sync_off is how many cycles into the current
     * m68705p5_step() the first such call happened (0xff = none yet; the
     * caller resets it before each step). */
    uint8_t  timer_line_syncs;
    uint8_t  timer_sync_off;
} m68705p5_state;

/* Reset the MCU: clears ports/timer/PCR to power-on defaults and the
 * underlying m6805 core to its reset vector (loaded from user_rom at
 * offset 0x7FE/0x7FF - see m6805_reset()). `rom` must point to a
 * caller-owned buffer of at least M68705P5_ROM_SIZE bytes that stays
 * valid for the MCU's lifetime. Mask-option byte is read from
 * rom[0x784] (MOR, exactly where MAME's m68705p_device::
 * get_mask_options() reads it - see m68705.cpp). */
void m68705p5_reset(m68705p5_state *s, const uint8_t *rom);

/* Same reset, but the core's memory accesses go to rd/wr instead of the
 * global m6805_read/m6805_write (see m6805_state.rd_hook). `cpu` is the
 * first member of m68705p5_state, so a hook may cast its m6805_state* back
 * to the owning m68705p5_state* and call m68705p5_mem_read/write on it. */
void m68705p5_reset_hooked(m68705p5_state *s, const uint8_t *rom,
                           uint8_t (*rd)(m6805_state *, uint16_t),
                           void (*wr)(m6805_state *, uint16_t, uint8_t));

/* Execute up to `count` instructions. Internally drives the general
 * m6805 core's step loop one instruction at a time so the timer can
 * be advanced (and its interrupt line raised/lowered) after every
 * single opcode, matching MAME's per-opcode burn_cycles() call -
 * batching the timer update over the whole `count` would change
 * exactly when a timer interrupt becomes pending relative to
 * instruction boundaries. Returns total cycles consumed. */
IRAM_ATTR int m68705p5_step(m68705p5_state *s, int count);

/* Decode a memory access against ONE m68705p5 instance - call this
 * from the owning machine's own m6805_read/m6805_write override (see
 * header's "Integration model" note). */
IRAM_ATTR uint8_t m68705p5_mem_read(m68705p5_state *s, uint16_t addr);
IRAM_ATTR void m68705p5_mem_write(m68705p5_state *s, uint16_t addr, uint8_t val);

/* Feed external /INT line state (forwards to m6805_irq on the
 * underlying core - see m6805.h). */
void m68705p5_irq(m68705p5_state *s, int asserted);

/* Update what a port's INPUT-configured pins (ddr=0 bits) currently
 * read as - i.e. what something external is driving onto them. Call
 * this whenever the owning glue/machine changes what's wired to a
 * port from the "outside" (matches MAME's port_input_w<N>()/the
 * various *_r() device callbacks feeding m_port_input[N] before the
 * CPU's next port_r()). Does not affect output-configured (ddr=1)
 * bits' read-back at all - see port_r()'s mask|(latch&ddr)|(input&
 * ~ddr) formula in m68705p5.c. */
IRAM_ATTR void m68705p5_port_input_w(m68705p5_state *s, unsigned port, uint8_t data);

/* What an external device wired to a port's pins would currently read
 * (output-driven bits only; input-only bits read high/floating as 1,
 * since nothing is driving them). Port A additionally applies the
 * open-drain-with-pull-up behaviour (only bits latched to 0 pull the
 * line down; bits latched to 1, or configured as input, float high) -
 * this is the "wire-AND" view glue code like the Taito MCU handshake
 * needs for the shared PA0-7 data latch (mirrors MAME's pa_value()/
 * port_cb_w<N>() combination in taito68705_mcu_device_base /
 * m6805_hmos_device). Ports B/C are plain push-pull: any ddr=1 bit
 * drives its latch value directly, ddr=0 bits float high. */
uint8_t m68705p5_port_output(const m68705p5_state *s, unsigned port);

#ifdef __cplusplus
}
#endif

#endif /* M68705P5_H */
