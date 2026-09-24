/*
 * M6805 CPU Emulator for Galagino
 *
 * Faithful port of MAME's m6805_base_device execution core (HMOS opcode
 * table s_hmos_s_ops/s_hmos_b_ops + s_hmos_cycles, and the 2-source
 * interrupt scheme from m6805_hmos_device::interrupt in m68705.cpp).
 * Source: src/devices/cpu/m6805/{m6805.cpp,m6805defs.h,6805ops.hxx},
 * src/devices/cpu/m6805/m68705.cpp - license:BSD-3-Clause,
 * copyright-holders:Aaron Giles, Vas Crabb. See m6805.h for the porting
 * notes (what was and wasn't carried over, and why).
 *
 * Dispatch is a flat switch on the opcode byte, matching this project's
 * m6809.c/m6803.c house style, rather than MAME's per-opcode function-
 * pointer table - behaviorally identical, just expressed differently.
 *
 * Addressing-mode note: MAME selects between a 13-bit ("small") and a
 * 16-bit ("big") memory-access template at compile time per opcode table
 * (the `big` boolean template parameter). Every 68705 variant this port
 * targets uses an address space <= 13 bits' worth of code+data mapped
 * within a wider CPU-visible address range (M68705P5: 11-bit, 0x000-
 * 0x7FF) but the *external* address bus written to m6805_read/write is
 * always a plain uint16_t - so that split is irrelevant here; we always
 * mask through cfg->addr_mask instead (see header comment).
 */

#include "m6805.h"

/* ============================================================
 * Memory access helpers
 * ============================================================ */

static inline uint16_t addr_mask(const m6805_state *s, uint16_t addr) {
    return (uint16_t)(addr & s->cfg->addr_mask);
}

static inline uint8_t fetch8(m6805_state *s) {
    uint16_t pc = addr_mask(s, s->PC);
    s->PC = (uint16_t)(s->PC + 1);
    return m6805_read(s, pc);
}

static inline uint16_t fetch16(m6805_state *s) {
    uint8_t hi = fetch8(s);
    uint8_t lo = fetch8(s);
    return (uint16_t)((hi << 8) | lo);
}

static inline uint8_t rd(m6805_state *s, uint16_t addr) {
    return m6805_read(s, addr_mask(s, addr));
}

static inline void wr(m6805_state *s, uint16_t addr, uint8_t val) {
    m6805_write(s, addr_mask(s, addr), val);
}

static inline uint16_t rd16(m6805_state *s, uint16_t addr) {
    uint8_t hi = rd(s, addr);
    uint8_t lo = rd(s, (uint16_t)(addr + 1));
    return (uint16_t)((hi << 8) | lo);
}

/* Stack: S is 8-bit, wraps between cfg->sp_floor and cfg->sp_mask (see
 * m6805defs.h SP_INC/SP_DEC/SP_ADJUST; stack lives in page 0, e.g.
 * 0x60-0x7F on the M68705P family). Stack memory access always goes
 * through the plain 0x00xx page, not through addr_mask - matches MAME
 * (S is a PAIR whose .w.l is used directly as the memory address, and
 * page 0 is always within any variant's address_mask). */
static inline void push8(m6805_state *s, uint8_t v) {
    wr(s, s->S, v);
    if (--s->S < s->cfg->sp_floor) s->S = s->cfg->sp_mask;
}

static inline void push16(m6805_state *s, uint16_t v) {
    push8(s, (uint8_t)(v & 0xff));
    push8(s, (uint8_t)(v >> 8));
}

static inline uint8_t pull8(m6805_state *s) {
    if (++s->S > s->cfg->sp_mask) s->S = s->cfg->sp_floor;
    return rd(s, s->S);
}

static inline uint16_t pull16(m6805_state *s) {
    uint8_t hi = pull8(s);
    uint8_t lo = pull8(s);
    return (uint16_t)((hi << 8) | lo);
}

/* for treating an unsigned uint8_t as a signed int16_t (6805defs.h SIGNED) */
#define SIGNED(b) ((int16_t)((b) & 0x80 ? (b) | 0xff00 : (b)))

/* ============================================================
 * CC flag helpers (6805.h clr_nz/clr_nzc/clr_hc/clr_hnzc,
 * set_z8/set_n8/set_h/set_c8/set_nz8/set_nzc8/set_hnzc8)
 * ============================================================ */

static inline void clr_nz(m6805_state *s)   { s->CC &= (uint8_t)~(M6805_CC_N | M6805_CC_Z); }
static inline void clr_nzc(m6805_state *s)  { s->CC &= (uint8_t)~(M6805_CC_N | M6805_CC_Z | M6805_CC_C); }
static inline void clr_hc(m6805_state *s)   { s->CC &= (uint8_t)~(M6805_CC_H | M6805_CC_C); }
static inline void clr_hnzc(m6805_state *s) { s->CC &= (uint8_t)~(M6805_CC_H | M6805_CC_N | M6805_CC_Z | M6805_CC_C); }

static inline void set_z8(m6805_state *s, uint8_t a)  { if (!a) s->CC |= M6805_CC_Z; }
static inline void set_n8(m6805_state *s, uint8_t a)  { s->CC = (uint8_t)(s->CC | ((a & 0x80) >> 5)); }
static inline void set_h(m6805_state *s, uint8_t a, uint8_t b, uint8_t r) { s->CC = (uint8_t)(s->CC | ((a ^ b ^ r) & 0x10)); }
static inline void set_c8(m6805_state *s, uint16_t a) { if (a & 0x100) s->CC |= M6805_CC_C; }

static inline void set_nz8(m6805_state *s, uint8_t a)   { set_n8(s, a); set_z8(s, a); }
static inline void set_nzc8(m6805_state *s, uint16_t a) { set_nz8(s, (uint8_t)a); set_c8(s, a); }
static inline void set_hnzc8(m6805_state *s, uint8_t a, uint8_t b, uint16_t r) { set_h(s, a, b, (uint8_t)r); set_nzc8(s, r); }

/* ============================================================
 * Addressing modes (6805defs.h DIRECT/EXTENDED/INDEXED/INDEXED1/INDEXED2
 * + the ARGADDR/ARGBYTE/DIRBYTE/... dispatch macros collapsed into one
 * addr_mode-driven pair of helpers: ea_addr() computes the effective
 * address (ARGADDR / *BYTE's addressing half), ea_read() additionally
 * loads the operand byte (ARGBYTE). Instructions needing only the
 * address (JMP/JSR/STA/STX/CLR/bit ops on DIRECT) call ea_addr();
 * instructions needing a value call ea_read(); ACCUMULATOR-only ops
 * (…A/…X, $40-$5F) touch s->A/s->X directly, no addressing mode.
 * ============================================================ */

typedef enum {
    ADDR_IM,   /* immediate:            operand is the next byte            */
    ADDR_DI,   /* direct:               operand is at 0x00-0xFF             */
    ADDR_EX,   /* extended:             operand is at a 16-bit address      */
    ADDR_IX,   /* indexed, no offset:   operand is at X                     */
    ADDR_IX1,  /* indexed, 1-byte off:  operand is at X + 8-bit unsigned    */
    ADDR_IX2   /* indexed, 2-byte off:  operand is at X + 16-bit unsigned   */
} addr_mode;

/* index register X is zero-extended to 16 bits for the EA computation
 * (X itself stays 8-bit storage - matches MAME m_x being u8 while EA
 * arithmetic happens in u16/PAIR). */
static uint16_t ea_addr(m6805_state *s, addr_mode m) {
    switch (m) {
        case ADDR_DI:  return fetch8(s);
        case ADDR_EX:  return fetch16(s);
        case ADDR_IX:  return s->X;
        case ADDR_IX1: return (uint16_t)(fetch8(s) + s->X);
        case ADDR_IX2: return (uint16_t)(fetch16(s) + s->X);
        case ADDR_IM:  /* no address - caller must use ea_read() only */
        default:       return 0;
    }
}

static uint8_t ea_read(m6805_state *s, addr_mode m) {
    if (m == ADDR_IM) return fetch8(s);
    return rd(s, ea_addr(s, m));
}

/* ============================================================
 * Interrupts (m68705.cpp m6805_hmos_device::interrupt - 2 sources:
 * external /INT edge-latched at M6805_IRQ_LINE, timer/counter
 * level-sensitive at M6805_INT_TIMER; /INT has priority when both are
 * pending, matching MAME's if/else-if order)
 * ============================================================ */

static void service_interrupt(m6805_state *s) {
    if (!(s->pending_interrupts & ((1u << M6805_IRQ_LINE) | (1u << M6805_INT_TIMER))))
        return;
    if (s->CC & M6805_CC_I)
        return;

    push16(s, s->PC);
    push8(s, s->X);
    push8(s, s->A);
    push8(s, s->CC);
    s->CC |= M6805_CC_I;

    if (s->pending_interrupts & (1u << M6805_IRQ_LINE)) {
        s->pending_interrupts = (uint8_t)(s->pending_interrupts & ~(1u << M6805_IRQ_LINE));
        s->PC = rd16(s, s->cfg->vector_int);
    } else /* M6805_INT_TIMER: level-sensitive, NOT cleared here - the
              variant's timer peripheral clears it by deasserting the
              line once its status register is serviced, same as MAME's
              m6805_hmos_device::execute_set_input's "this one is
              level-sensitive" handling for M6805_INT_TIMER */
    {
        s->PC = rd16(s, s->cfg->vector_timer);
    }

    s->cycles += 11;
}

void m6805_irq(m6805_state *s, int asserted) {
    s->irq_line_level = asserted ? 1 : 0;
    if (asserted)
        s->pending_interrupts |= (1u << M6805_IRQ_LINE);
    /* edge-latched: deasserting does NOT clear a pending request (see
       header comment / MAME's m_pending_interrupts handling) */
}

/* ============================================================
 * Reset
 * ============================================================ */

void m6805_reset(m6805_state *s, const m6805_config *cfg) {
    s->cfg = cfg;
    s->A = 0;
    s->X = 0;
    s->S = cfg->sp_mask;
    s->CC = M6805_CC_I; /* SEI - interrupts disabled out of reset */
    s->pending_interrupts = 0;
    s->irq_line_level = 0;
    s->cycles = 0;
    s->total_cycles = 0;
    s->PC = rd16(s, cfg->vector_reset);
}

/* ============================================================
 * Opcode dispatch
 *
 * Every case below mirrors one OP_HANDLER/OP_HANDLER_BIT/OP_HANDLER_BRA/
 * OP_HANDLER_MODE block in 6805ops.hxx, cross-referenced against the
 * s_hmos_s_ops/s_hmos_b_ops opcode table and s_hmos_cycles cycle table
 * in m6805.cpp for the opcode-to-(handler,mode,cycles) mapping. Cycle
 * counts are transcribed directly from s_hmos_cycles; illegal opcodes
 * use MAME's XX=4 placeholder (never executed by a correctly dumped
 * ROM, but kept non-zero so a bug elsewhere can't wedge the step loop).
 * ============================================================ */

static void step_one(m6805_state *s) {
    uint8_t op = fetch8(s);

    switch (op) {

    /* ---- $00-$1F: BRSET/BRCLR/BSET/BCLR direct,bit (bit ops) ---- */
    /* $00/02/04/06/08/0A/0C/0E BRSET b,direct,relative  ---* ; 10 cycles */
    case 0x00: case 0x02: case 0x04: case 0x06:
    case 0x08: case 0x0A: case 0x0C: case 0x0E: {
        unsigned bit = (unsigned)(op >> 1) & 7;
        uint16_t dir = fetch8(s);
        uint8_t r = rd(s, dir);
        uint8_t t = fetch8(s);
        s->CC &= (uint8_t)~M6805_CC_C;
        if (r & (1u << bit)) { s->CC |= M6805_CC_C; s->PC = (uint16_t)(s->PC + SIGNED(t)); }
        s->cycles += 10;
        break;
    }
    /* $01/03/05/07/09/0B/0D/0F BRCLR b,direct,relative  ---* ; 10 cycles */
    case 0x01: case 0x03: case 0x05: case 0x07:
    case 0x09: case 0x0B: case 0x0D: case 0x0F: {
        unsigned bit = (unsigned)(op >> 1) & 7;
        uint16_t dir = fetch8(s);
        uint8_t r = rd(s, dir);
        uint8_t t = fetch8(s);
        s->CC |= M6805_CC_C;
        if (!(r & (1u << bit))) { s->CC &= (uint8_t)~M6805_CC_C; s->PC = (uint16_t)(s->PC + SIGNED(t)); }
        s->cycles += 10;
        break;
    }
    /* $10/12/.../1E BSET b,direct ---- ; 7 cycles */
    case 0x10: case 0x12: case 0x14: case 0x16:
    case 0x18: case 0x1A: case 0x1C: case 0x1E: {
        unsigned bit = (unsigned)(op >> 1) & 7;
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        wr(s, dir, (uint8_t)(t | (1u << bit)));
        s->cycles += 7;
        break;
    }
    /* $11/13/.../1F BCLR b,direct ---- ; 7 cycles */
    case 0x11: case 0x13: case 0x15: case 0x17:
    case 0x19: case 0x1B: case 0x1D: case 0x1F: {
        unsigned bit = (unsigned)(op >> 1) & 7;
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        wr(s, dir, (uint8_t)(t & ~(1u << bit)));
        s->cycles += 7;
        break;
    }

    /* ---- $20-$2F: branches, relative, 4 cycles each ---- */
    case 0x20: /* BRA  ---- */ {
        uint8_t t = fetch8(s); s->PC = (uint16_t)(s->PC + SIGNED(t)); s->cycles += 4; break;
    }
    case 0x21: /* BRN  ---- (never taken) */ {
        (void)fetch8(s); s->cycles += 4; break;
    }
    case 0x22: /* BHI  !(C|Z) */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & (M6805_CC_C | M6805_CC_Z))) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x23: /* BLS  (C|Z) */ {
        uint8_t t = fetch8(s);
        if (s->CC & (M6805_CC_C | M6805_CC_Z)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x24: /* BCC  !C */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & M6805_CC_C)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x25: /* BCS  C */ {
        uint8_t t = fetch8(s);
        if (s->CC & M6805_CC_C) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x26: /* BNE  !Z */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & M6805_CC_Z)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x27: /* BEQ  Z */ {
        uint8_t t = fetch8(s);
        if (s->CC & M6805_CC_Z) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x28: /* BHCC !H */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & M6805_CC_H)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x29: /* BHCS H */ {
        uint8_t t = fetch8(s);
        if (s->CC & M6805_CC_H) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2A: /* BPL  !N */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & M6805_CC_N)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2B: /* BMI  N */ {
        uint8_t t = fetch8(s);
        if (s->CC & M6805_CC_N) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2C: /* BMC  !I */ {
        uint8_t t = fetch8(s);
        if (!(s->CC & M6805_CC_I)) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2D: /* BMS  I */ {
        uint8_t t = fetch8(s);
        if (s->CC & M6805_CC_I) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2E: /* BIL  test_il() - external /INT pin currently asserted.
                  MAME's test_il() reads m_irq_state[M6805_IRQ_LINE]
                  directly (the live pin level), NOT pending_interrupts
                  (the latched/edge request) - modelled the same way via
                  a dedicated live-level flag, see irq_line_level below. */ {
        uint8_t t = fetch8(s);
        if (s->irq_line_level) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }
    case 0x2F: /* BIH  !test_il() */ {
        uint8_t t = fetch8(s);
        if (!s->irq_line_level) s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 4; break;
    }

    /* ---- $30-$3F: read-modify-write, DIRECT addressing, 6 cycles
       (illegal opcodes use XX=4 per s_hmos_cycles) ---- */
    case 0x30: /* NEG direct -*** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        uint16_t r = (uint16_t)(-(int)t);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, dir, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x31: case 0x32: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x33: /* COM direct -**1 ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = (uint8_t)~rd(s, dir);
        clr_nz(s); set_nz8(s, t); s->CC |= M6805_CC_C;
        wr(s, dir, t);
        s->cycles += 6; break;
    }
    case 0x34: /* LSR direct -0** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)(t >> 1);
        set_z8(s, t);
        wr(s, dir, t);
        s->cycles += 6; break;
    }
    case 0x35: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x36: /* ROR direct -*** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        uint8_t r = (uint8_t)((s->CC & M6805_CC_C) << 7);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        r = (uint8_t)(r | (t >> 1));
        set_nz8(s, r);
        wr(s, dir, r);
        s->cycles += 6; break;
    }
    case 0x37: /* ASR direct -*** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)((t >> 1) | (t & 0x80));
        set_nz8(s, t);
        wr(s, dir, t);
        s->cycles += 6; break;
    }
    case 0x38: /* LSL direct -*** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        uint16_t r = (uint16_t)(t << 1);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, dir, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x39: /* ROL direct -*** ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        uint16_t r = (uint16_t)((s->CC & M6805_CC_C) | (t << 1));
        clr_nzc(s); set_nzc8(s, r);
        wr(s, dir, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x3A: /* DEC direct -**- ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = (uint8_t)(rd(s, dir) - 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, dir, t);
        s->cycles += 6; break;
    }
    case 0x3B: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x3C: /* INC direct -**- ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = (uint8_t)(rd(s, dir) + 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, dir, t);
        s->cycles += 6; break;
    }
    case 0x3D: /* TST direct -**- ; 6 */ {
        uint16_t dir = fetch8(s);
        uint8_t t = rd(s, dir);
        clr_nz(s); set_nz8(s, t);
        s->cycles += 6; break;
    }
    case 0x3E: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x3F: /* CLR direct -01- ; 6 */ {
        uint16_t dir = fetch8(s);
        (void)rd(s, dir); /* ARGADDR only computes EA, but DIRECT still fetches the operand byte from the instruction stream (the direct-page offset), not the memory it points to - matches MAME's DIRECT macro (immbyte into EA) */
        clr_nz(s); s->CC |= M6805_CC_Z;
        wr(s, dir, 0);
        s->cycles += 6; break;
    }

    /* ---- $40-$4F: accumulator-A inherent ops, 4 cycles (illegal=4) ---- */
    case 0x40: /* NEGA -*** */ {
        uint16_t r = (uint16_t)(-(int)s->A);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x41: case 0x42: /* illegal (no MUL: HMOS op table) */
        s->cycles += 4; break;
    case 0x43: /* COMA -**1 */
        s->A = (uint8_t)~s->A;
        clr_nz(s); set_nz8(s, s->A); s->CC |= M6805_CC_C;
        s->cycles += 4; break;
    case 0x44: /* LSRA -0** */
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->A & 1));
        s->A = (uint8_t)(s->A >> 1);
        set_z8(s, s->A);
        s->cycles += 4; break;
    case 0x45: /* illegal */
        s->cycles += 4; break;
    case 0x46: /* RORA -*** */ {
        uint8_t r = (uint8_t)((s->CC & M6805_CC_C) << 7);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->A & 1));
        r = (uint8_t)(r | (s->A >> 1));
        set_nz8(s, r); s->A = r;
        s->cycles += 4; break;
    }
    case 0x47: /* ASRA -*** */
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->A & 1));
        s->A = (uint8_t)((s->A & 0x80) | (s->A >> 1));
        set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0x48: /* LSLA -*** */ {
        uint16_t r = (uint16_t)(s->A << 1);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x49: /* ROLA -*** */ {
        uint16_t r = (uint16_t)((s->CC & M6805_CC_C) | (s->A << 1));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x4A: /* DECA -**- */
        s->A = (uint8_t)(s->A - 1);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0x4B: /* illegal */
        s->cycles += 4; break;
    case 0x4C: /* INCA -**- */
        s->A = (uint8_t)(s->A + 1);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0x4D: /* TSTA -**- */
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0x4E: /* illegal */
        s->cycles += 4; break;
    case 0x4F: /* CLRA -01- */
        s->A = 0;
        clr_nz(s); s->CC |= M6805_CC_Z;
        s->cycles += 4; break;

    /* ---- $50-$5F: accumulator-X (index register) inherent ops, 4 cycles ---- */
    case 0x50: /* NEGX -*** */ {
        uint16_t r = (uint16_t)(-(int)s->X);
        clr_nzc(s); set_nzc8(s, r); s->X = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x51: case 0x52: /* illegal */
        s->cycles += 4; break;
    case 0x53: /* COMX -**1 */
        s->X = (uint8_t)~s->X;
        clr_nz(s); set_nz8(s, s->X); s->CC |= M6805_CC_C;
        s->cycles += 4; break;
    case 0x54: /* LSRX -0** */
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->X & 1));
        s->X = (uint8_t)(s->X >> 1);
        set_z8(s, s->X);
        s->cycles += 4; break;
    case 0x55: /* illegal */
        s->cycles += 4; break;
    case 0x56: /* RORX -*** */ {
        uint8_t r = (uint8_t)((s->CC & M6805_CC_C) << 7);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->X & 1));
        r = (uint8_t)(r | (s->X >> 1));
        set_nz8(s, r); s->X = r;
        s->cycles += 4; break;
    }
    case 0x57: /* ASRX -*** */
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (s->X & 1));
        s->X = (uint8_t)((s->X & 0x80) | (s->X >> 1));
        set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0x58: /* LSLX -*** */ {
        uint16_t r = (uint16_t)(s->X << 1);
        clr_nzc(s); set_nzc8(s, r); s->X = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x59: /* ROLX -*** */ {
        uint16_t r = (uint16_t)((s->CC & M6805_CC_C) | (s->X << 1));
        clr_nzc(s); set_nzc8(s, r); s->X = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0x5A: /* DECX -**- */
        s->X = (uint8_t)(s->X - 1);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0x5B: /* illegal */
        s->cycles += 4; break;
    case 0x5C: /* INCX -**- */
        s->X = (uint8_t)(s->X + 1);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0x5D: /* TSTX -**- */
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0x5E: /* illegal */
        s->cycles += 4; break;
    case 0x5F: /* CLRX -01- */
        s->X = 0;
        clr_nz(s); s->CC |= M6805_CC_Z;
        s->cycles += 4; break;

    /* ---- $60-$6F: read-modify-write, INDEXED1 (X + 8-bit offset), 7 cycles ---- */
    case 0x60: /* NEG indexed,1 -*** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)(-(int)t);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 7; break;
    }
    case 0x61: case 0x62: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x63: /* COM indexed,1 -**1 ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = (uint8_t)~rd(s, ea);
        clr_nz(s); set_nz8(s, t); s->CC |= M6805_CC_C;
        wr(s, ea, t);
        s->cycles += 7; break;
    }
    case 0x64: /* LSR indexed,1 -0** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)(t >> 1);
        set_z8(s, t);
        wr(s, ea, t);
        s->cycles += 7; break;
    }
    case 0x65: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x66: /* ROR indexed,1 -*** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        uint8_t r = (uint8_t)((s->CC & M6805_CC_C) << 7);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        r = (uint8_t)(r | (t >> 1));
        set_nz8(s, r);
        wr(s, ea, r);
        s->cycles += 7; break;
    }
    case 0x67: /* ASR indexed,1 -*** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)((t >> 1) | (t & 0x80));
        set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 7; break;
    }
    case 0x68: /* LSL indexed,1 -*** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)(t << 1);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 7; break;
    }
    case 0x69: /* ROL indexed,1 -*** ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)((s->CC & M6805_CC_C) | (t << 1));
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 7; break;
    }
    case 0x6A: /* DEC indexed,1 -**- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = (uint8_t)(rd(s, ea) - 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 7; break;
    }
    case 0x6B: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x6C: /* INC indexed,1 -**- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = (uint8_t)(rd(s, ea) + 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 7; break;
    }
    case 0x6D: /* TST indexed,1 -**- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        uint8_t t = rd(s, ea);
        clr_nz(s); set_nz8(s, t);
        s->cycles += 7; break;
    }
    case 0x6E: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x6F: /* CLR indexed,1 -01- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        clr_nz(s); s->CC |= M6805_CC_Z;
        wr(s, ea, 0);
        s->cycles += 7; break;
    }

    /* ---- $70-$7F: read-modify-write, INDEXED (X, no offset), 6 cycles ---- */
    case 0x70: /* NEG indexed -*** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)(-(int)t);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x71: case 0x72: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x73: /* COM indexed -**1 ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = (uint8_t)~rd(s, ea);
        clr_nz(s); set_nz8(s, t); s->CC |= M6805_CC_C;
        wr(s, ea, t);
        s->cycles += 6; break;
    }
    case 0x74: /* LSR indexed -0** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)(t >> 1);
        set_z8(s, t);
        wr(s, ea, t);
        s->cycles += 6; break;
    }
    case 0x75: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x76: /* ROR indexed -*** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        uint8_t r = (uint8_t)((s->CC & M6805_CC_C) << 7);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        r = (uint8_t)(r | (t >> 1));
        set_nz8(s, r);
        wr(s, ea, r);
        s->cycles += 6; break;
    }
    case 0x77: /* ASR indexed -*** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        clr_nzc(s); s->CC = (uint8_t)(s->CC | (t & 1));
        t = (uint8_t)((t >> 1) | (t & 0x80));
        set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 6; break;
    }
    case 0x78: /* LSL indexed -*** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)(t << 1);
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x79: /* ROL indexed -*** ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        uint16_t r = (uint16_t)((s->CC & M6805_CC_C) | (t << 1));
        clr_nzc(s); set_nzc8(s, r);
        wr(s, ea, (uint8_t)r);
        s->cycles += 6; break;
    }
    case 0x7A: /* DEC indexed -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = (uint8_t)(rd(s, ea) - 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 6; break;
    }
    case 0x7B: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x7C: /* INC indexed -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = (uint8_t)(rd(s, ea) + 1);
        clr_nz(s); set_nz8(s, t);
        wr(s, ea, t);
        s->cycles += 6; break;
    }
    case 0x7D: /* TST indexed -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        uint8_t t = rd(s, ea);
        clr_nz(s); set_nz8(s, t);
        s->cycles += 6; break;
    }
    case 0x7E: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x7F: /* CLR indexed -01- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        clr_nz(s); s->CC |= M6805_CC_Z;
        wr(s, ea, 0);
        s->cycles += 6; break;
    }

    /* ---- $80-$8F: control ---- */
    case 0x80: /* RTI #### ; 9 */
        s->CC = pull8(s);
        s->A  = pull8(s);
        s->X  = pull8(s);
        s->PC = pull16(s);
        s->cycles += 9; break;
    case 0x81: /* RTS ---- ; 6 */
        s->PC = pull16(s);
        s->cycles += 6; break;
    case 0x82: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x83: /* SWI ---- ; 11 */
        push16(s, s->PC);
        push8(s, s->X);
        push8(s, s->A);
        push8(s, s->CC);
        s->CC |= M6805_CC_I;
        s->PC = rd16(s, s->cfg->vector_swi);
        s->cycles += 11; break;
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x8E: /* STOP - not implemented (MAME: fatalerror stub; no
                  68705-family game reachable from this port's target
                  set executes it - see m6805.h header note) ; 4 */
        s->cycles += 4; break;
    case 0x8F: /* WAIT - not implemented, same rationale as STOP ; 4 */
        s->cycles += 4; break;

    /* ---- $90-$9F: control / inherent flag ops ---- */
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x97: /* TAX ---- ; 2 */
        s->X = s->A;
        s->cycles += 2; break;
    case 0x98: /* CLC ---- ; 2 */
        s->CC &= (uint8_t)~M6805_CC_C;
        s->cycles += 2; break;
    case 0x99: /* SEC ---- ; 2 */
        s->CC |= M6805_CC_C;
        s->cycles += 2; break;
    case 0x9A: /* CLI ---- ; 2 */
        s->CC &= (uint8_t)~M6805_CC_I;
        s->cycles += 2; break;
    case 0x9B: /* SEI ---- ; 2 */
        s->CC |= M6805_CC_I;
        s->cycles += 2; break;
    case 0x9C: /* RSP ---- ; 2 */
        s->S = s->cfg->sp_mask;
        s->cycles += 2; break;
    case 0x9D: /* NOP ---- ; 2 */
        s->cycles += 2; break;
    case 0x9E: /* illegal ; 4 */
        s->cycles += 4; break;
    case 0x9F: /* TXA ---- ; 2 */
        s->A = s->X;
        s->cycles += 2; break;

    /* ---- $A0-$AF: accumulator ops, IMMEDIATE, 2 cycles (except
       $AC illegal=4, $AD BSR=8, $AF illegal=4) ---- */
    case 0xA0: /* SUBA imm -*** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 2; break;
    }
    case 0xA1: /* CMPA imm -*** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 2; break;
    }
    case 0xA2: /* SBCA imm -*** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 2; break;
    }
    case 0xA3: /* CPX imm -*** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 2; break;
    }
    case 0xA4: /* ANDA imm -**- ; 2 */ {
        uint8_t t = ea_read(s, ADDR_IM);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 2; break;
    }
    case 0xA5: /* BITA imm -**- ; 2 */ {
        uint8_t t = ea_read(s, ADDR_IM);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 2; break;
    }
    case 0xA6: /* LDA imm -**- ; 2 */
        s->A = ea_read(s, ADDR_IM);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 2; break;
    case 0xA7: /* illegal ; 4 (no STA immediate) */
        s->cycles += 4; break;
    case 0xA8: /* EORA imm -**- ; 2 */ {
        uint8_t t = ea_read(s, ADDR_IM);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 2; break;
    }
    case 0xA9: /* ADCA imm **** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 2; break;
    }
    case 0xAA: /* ORA imm -**- ; 2 */ {
        uint8_t t = ea_read(s, ADDR_IM);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 2; break;
    }
    case 0xAB: /* ADDA imm **** ; 2 */ {
        uint16_t t = ea_read(s, ADDR_IM);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 2; break;
    }
    case 0xAC: /* illegal ; 4 (no JMP immediate) */
        s->cycles += 4; break;
    case 0xAD: /* BSR ---- ; 8 */ {
        uint8_t t = fetch8(s);
        push16(s, s->PC);
        s->PC = (uint16_t)(s->PC + SIGNED(t));
        s->cycles += 8; break;
    }
    case 0xAE: /* LDX imm -**- ; 2 */
        s->X = ea_read(s, ADDR_IM);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 2; break;
    case 0xAF: /* illegal ; 4 (no STX immediate) */
        s->cycles += 4; break;

    /* ---- $B0-$BF: accumulator ops, DIRECT ---- */
    case 0xB0: /* SUBA direct -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xB1: /* CMPA direct -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 4; break;
    }
    case 0xB2: /* SBCA direct -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xB3: /* CPX direct -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 4; break;
    }
    case 0xB4: /* ANDA direct -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_DI);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xB5: /* BITA direct -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_DI);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 4; break;
    }
    case 0xB6: /* LDA direct -**- ; 4 */
        s->A = ea_read(s, ADDR_DI);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0xB7: /* STA direct -**- ; 5 */ {
        uint16_t ea = ea_addr(s, ADDR_DI);
        clr_nz(s); set_nz8(s, s->A);
        wr(s, ea, s->A);
        s->cycles += 5; break;
    }
    case 0xB8: /* EORA direct -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_DI);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xB9: /* ADCA direct **** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xBA: /* ORA direct -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_DI);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xBB: /* ADDA direct **** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_DI);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xBC: /* JMP direct -*** ; 3 */
        s->PC = ea_addr(s, ADDR_DI);
        s->cycles += 3; break;
    case 0xBD: /* JSR direct ---- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_DI);
        push16(s, s->PC);
        s->PC = ea;
        s->cycles += 7; break;
    }
    case 0xBE: /* LDX direct -**- ; 4 */
        s->X = ea_read(s, ADDR_DI);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0xBF: /* STX direct -**- ; 5 */ {
        uint16_t ea = ea_addr(s, ADDR_DI);
        clr_nz(s); set_nz8(s, s->X);
        wr(s, ea, s->X);
        s->cycles += 5; break;
    }

    /* ---- $C0-$CF: accumulator ops, EXTENDED ---- */
    case 0xC0: /* SUBA extended -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xC1: /* CMPA extended -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 5; break;
    }
    case 0xC2: /* SBCA extended -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xC3: /* CPX extended -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 5; break;
    }
    case 0xC4: /* ANDA extended -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_EX);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xC5: /* BITA extended -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_EX);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 5; break;
    }
    case 0xC6: /* LDA extended -**- ; 5 */
        s->A = ea_read(s, ADDR_EX);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    case 0xC7: /* STA extended -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_EX);
        clr_nz(s); set_nz8(s, s->A);
        wr(s, ea, s->A);
        s->cycles += 6; break;
    }
    case 0xC8: /* EORA extended -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_EX);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xC9: /* ADCA extended **** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xCA: /* ORA extended -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_EX);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xCB: /* ADDA extended **** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_EX);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xCC: /* JMP extended -*** ; 4 */
        s->PC = ea_addr(s, ADDR_EX);
        s->cycles += 4; break;
    case 0xCD: /* JSR extended ---- ; 8 */ {
        uint16_t ea = ea_addr(s, ADDR_EX);
        push16(s, s->PC);
        s->PC = ea;
        s->cycles += 8; break;
    }
    case 0xCE: /* LDX extended -**- ; 5 */
        s->X = ea_read(s, ADDR_EX);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 5; break;
    case 0xCF: /* STX extended -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_EX);
        clr_nz(s); set_nz8(s, s->X);
        wr(s, ea, s->X);
        s->cycles += 6; break;
    }

    /* ---- $D0-$DF: accumulator ops, INDEXED2 (X + 16-bit offset) ---- */
    case 0xD0: /* SUBA indexed,2 -*** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 6; break;
    }
    case 0xD1: /* CMPA indexed,2 -*** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 6; break;
    }
    case 0xD2: /* SBCA indexed,2 -*** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 6; break;
    }
    case 0xD3: /* CPX indexed,2 -*** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 6; break;
    }
    case 0xD4: /* ANDA indexed,2 -**- ; 6 */ {
        uint8_t t = ea_read(s, ADDR_IX2);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 6; break;
    }
    case 0xD5: /* BITA indexed,2 -**- ; 6 */ {
        uint8_t t = ea_read(s, ADDR_IX2);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 6; break;
    }
    case 0xD6: /* LDA indexed,2 -**- ; 6 */
        s->A = ea_read(s, ADDR_IX2);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 6; break;
    case 0xD7: /* STA indexed,2 -**- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX2);
        clr_nz(s); set_nz8(s, s->A);
        wr(s, ea, s->A);
        s->cycles += 7; break;
    }
    case 0xD8: /* EORA indexed,2 -**- ; 6 */ {
        uint8_t t = ea_read(s, ADDR_IX2);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 6; break;
    }
    case 0xD9: /* ADCA indexed,2 **** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 6; break;
    }
    case 0xDA: /* ORA indexed,2 -**- ; 6 */ {
        uint8_t t = ea_read(s, ADDR_IX2);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 6; break;
    }
    case 0xDB: /* ADDA indexed,2 **** ; 6 */ {
        uint16_t t = ea_read(s, ADDR_IX2);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 6; break;
    }
    case 0xDC: /* JMP indexed,2 -*** ; 5 */
        s->PC = ea_addr(s, ADDR_IX2);
        s->cycles += 5; break;
    case 0xDD: /* JSR indexed,2 ---- ; 9 */ {
        uint16_t ea = ea_addr(s, ADDR_IX2);
        push16(s, s->PC);
        s->PC = ea;
        s->cycles += 9; break;
    }
    case 0xDE: /* LDX indexed,2 -**- ; 6 */
        s->X = ea_read(s, ADDR_IX2);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 6; break;
    case 0xDF: /* STX indexed,2 -**- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX2);
        clr_nz(s); set_nz8(s, s->X);
        wr(s, ea, s->X);
        s->cycles += 7; break;
    }

    /* ---- $E0-$EF: accumulator ops, INDEXED1 (X + 8-bit offset) ---- */
    case 0xE0: /* SUBA indexed,1 -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xE1: /* CMPA indexed,1 -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 5; break;
    }
    case 0xE2: /* SBCA indexed,1 -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xE3: /* CPX indexed,1 -*** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 5; break;
    }
    case 0xE4: /* ANDA indexed,1 -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_IX1);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xE5: /* BITA indexed,1 -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_IX1);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 5; break;
    }
    case 0xE6: /* LDA indexed,1 -**- ; 5 */
        s->A = ea_read(s, ADDR_IX1);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    case 0xE7: /* STA indexed,1 -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        clr_nz(s); set_nz8(s, s->A);
        wr(s, ea, s->A);
        s->cycles += 6; break;
    }
    case 0xE8: /* EORA indexed,1 -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_IX1);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xE9: /* ADCA indexed,1 **** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xEA: /* ORA indexed,1 -**- ; 5 */ {
        uint8_t t = ea_read(s, ADDR_IX1);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 5; break;
    }
    case 0xEB: /* ADDA indexed,1 **** ; 5 */ {
        uint16_t t = ea_read(s, ADDR_IX1);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 5; break;
    }
    case 0xEC: /* JMP indexed,1 -*** ; 4 */
        s->PC = ea_addr(s, ADDR_IX1);
        s->cycles += 4; break;
    case 0xED: /* JSR indexed,1 ---- ; 8 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        push16(s, s->PC);
        s->PC = ea;
        s->cycles += 8; break;
    }
    case 0xEE: /* LDX indexed,1 -**- ; 5 */
        s->X = ea_read(s, ADDR_IX1);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 5; break;
    case 0xEF: /* STX indexed,1 -**- ; 6 */ {
        uint16_t ea = ea_addr(s, ADDR_IX1);
        clr_nz(s); set_nz8(s, s->X);
        wr(s, ea, s->X);
        s->cycles += 6; break;
    }

    /* ---- $F0-$FF: accumulator ops, INDEXED (X, no offset) ---- */
    case 0xF0: /* SUBA indexed -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xF1: /* CMPA indexed -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->A - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 4; break;
    }
    case 0xF2: /* SBCA indexed -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->A - t - (s->CC & M6805_CC_C));
        clr_nzc(s); set_nzc8(s, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xF3: /* CPX indexed -*** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->X - t);
        clr_nzc(s); set_nzc8(s, r);
        s->cycles += 4; break;
    }
    case 0xF4: /* ANDA indexed -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_IX);
        s->A = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xF5: /* BITA indexed -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_IX);
        uint8_t r = (uint8_t)(s->A & t);
        clr_nz(s); set_nz8(s, r);
        s->cycles += 4; break;
    }
    case 0xF6: /* LDA indexed -**- ; 4 */
        s->A = ea_read(s, ADDR_IX);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    case 0xF7: /* STA indexed -**- ; 5 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        clr_nz(s); set_nz8(s, s->A);
        wr(s, ea, s->A);
        s->cycles += 5; break;
    }
    case 0xF8: /* EORA indexed -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_IX);
        s->A = (uint8_t)(s->A ^ t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xF9: /* ADCA indexed **** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->A + t + (s->CC & M6805_CC_C));
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xFA: /* ORA indexed -**- ; 4 */ {
        uint8_t t = ea_read(s, ADDR_IX);
        s->A = (uint8_t)(s->A | t);
        clr_nz(s); set_nz8(s, s->A);
        s->cycles += 4; break;
    }
    case 0xFB: /* ADDA indexed **** ; 4 */ {
        uint16_t t = ea_read(s, ADDR_IX);
        uint16_t r = (uint16_t)(s->A + t);
        clr_hnzc(s); set_hnzc8(s, s->A, (uint8_t)t, r); s->A = (uint8_t)r;
        s->cycles += 4; break;
    }
    case 0xFC: /* JMP indexed -*** ; 3 */
        s->PC = ea_addr(s, ADDR_IX);
        s->cycles += 3; break;
    case 0xFD: /* JSR indexed ---- ; 7 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        push16(s, s->PC);
        s->PC = ea;
        s->cycles += 7; break;
    }
    case 0xFE: /* LDX indexed -**- ; 4 */
        s->X = ea_read(s, ADDR_IX);
        clr_nz(s); set_nz8(s, s->X);
        s->cycles += 4; break;
    case 0xFF: /* STX indexed -**- ; 5 */ {
        uint16_t ea = ea_addr(s, ADDR_IX);
        clr_nz(s); set_nz8(s, s->X);
        wr(s, ea, s->X);
        s->cycles += 5; break;
    }
    }
}

int m6805_step(m6805_state *s, int count) {
    s->cycles = 0;
    for (int i = 0; i < count; i++) {
        service_interrupt(s);
        step_one(s);
    }
    s->total_cycles += s->cycles;
    return s->cycles;
}
