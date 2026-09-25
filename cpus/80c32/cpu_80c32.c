#include "cpu_80c32.h"
#include <string.h>

/* ---------- PSW bits ---------- */
#define C   0x80
#define AC  0x40
#define F0  0x20
#define RS1 0x10
#define RS0 0x08
#define OV  0x04
#define P   0x01

/* ---------- Common SFRs ---------- */
#define SFR_P0   0x80
#define SFR_SP   0x81
#define SFR_DPL  0x82
#define SFR_DPH  0x83
#define SFR_PCON 0x87
#define SFR_TCON 0x88
#define SFR_TMOD 0x89
#define SFR_TL0  0x8A
#define SFR_TL1  0x8B
#define SFR_TH0  0x8C
#define SFR_TH1  0x8D
#define SFR_P1   0x90
#define SFR_SCON 0x98
#define SFR_SBUF 0x99
#define SFR_P2   0xA0
#define SFR_IE   0xA8
#define SFR_P3   0xB0
#define SFR_IP   0xB8
/* 8052/80C32-only: Timer 2, absent on plain 8051/80C31. "80c32" in this
   core's name means this specific extra hardware -- confirmed the real
   pepp0127 ROM actually configures and depends on it (RCAP2L/H=0, then
   T2CON=0x04 to start it in free-running 16-bit auto-reload mode, then a
   boot-time self-test loop spins reading TL2/TH2 waiting to see the count
   actually change before proceeding) -- previously completely unimplemented
   here, meaning TL2/TH2 could never move on their own and that self-test
   loop had no real way to complete correctly. */
#define SFR_T2CON  0xC8
#define SFR_RCAP2L 0xCA
#define SFR_RCAP2H 0xCB
#define SFR_TL2    0xCC
#define SFR_TH2    0xCD
#define SFR_PSW  0xD0
#define SFR_ACC  0xE0
#define SFR_B    0xF0

/* TCON 0x88 */
#define TF1 0x80
#define TR1 0x40
#define TF0 0x20
#define TR0 0x10
#define IE1 0x08
#define IT1 0x04
#define IE0 0x02
#define IT0 0x01

/* TMOD 0x89 */
/* low nibble = Timer0, high = Timer1 */
/* M1 M0: 00=mode0, 01=mode1, 10=mode2, 11=mode3 */
#define GATE0 0x08
#define GATE1 0x80

/* T2CON 0xC8 (8052/80C32 Timer 2) -- bit numbers per Intel/real hardware
   and confirmed against MAME's mcs51/i8052.h enum (T2CON_TF2=7 .. CP=0) */
#define T2CON_TF2   0x80
#define T2CON_EXF2  0x40
#define T2CON_RCLK  0x20
#define T2CON_TCLK  0x10
#define T2CON_EXEN2 0x08
#define T2CON_TR2   0x04
#define T2CON_CT2   0x02
#define T2CON_CP    0x01

/* IE 0xA8 */
#define EA   0x80
#define ET2  0x20
#define ET1  0x08
#define EX1  0x04
#define ET0  0x02
#define EX0  0x01

/* IP 0xB8 -- per-source priority-LEVEL select (1 = high, 0 = low). Same bit
   positions as IE (minus EA), matching Intel/real hardware and MAME's
   mcs51 IP_* enum. A source whose IP bit is set can preempt a running
   low-level ISR; see check_interrupts. */
#define PT2  0x20
#define PS   0x10
#define PT1  0x08
#define PX1  0x04
#define PT0  0x02
#define PX0  0x01

/* Set to 0 to fall back to the pre-2026-08-31 behaviour: a single `in_irq`
   boolean, NO priority levels, NO nesting (any ISR blocks every interrupt
   until its RETI). Kept as a one-line escape hatch because every 80c32
   machine here (cm99 excluded -- it's Z80) drives IP, and the priority /
   nesting rewrite changes interrupt *timing* for all of them:
     - peps0043 / peps0040 / pex0827s: REQUIRED -- without it their
       frame-handler coroutine (runs inside the Timer0 ISR) is never
       preempted by VSYNC/INT0 and the game desyncs onto a setup screen
       after ~3 frames.
     - pepp0127: it worked before this existed. If its video pipeline
       (paytable recolour / bet-up sweep tearing) or boot regresses after
       this change, flip this to 0 and re-test to confirm the interrupt
       rewrite is the cause before chasing it elsewhere. The healthy
       MAME-verified reference is Timer0 ISR ~3.2 kHz, Timer1(=TH0) ISR
       ~1.6 kHz, INT0 ~48 Hz with ~2% of INT0s preempting a timer ISR. */
#define CPU_80C32_IRQ_PRIORITY 1

/* ---------- helpers ---------- */
static inline uint8_t parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
}

static inline void set_psw_p(struct cpu_80c32 *c) {
    if (parity8(c->a)) c->psw |= P; else c->psw &= ~P;
    c->ram[SFR_PSW] = c->psw;
    c->ram[SFR_ACC] = c->a;
}

static inline void sync_a(struct cpu_80c32 *c) {
    c->ram[SFR_ACC] = c->a;
    set_psw_p(c);
}

/* MUL AB / DIV AB write the B register through the fast c->b shadow only.
   Every *read* of B via direct addressing (MOV A,B / MOV Rn,B / ADD A,B /
   XCH A,B / INC B / MOV dir,B ...) goes through dir_r -> sfr_r -> c->ram[0xF0],
   so the shadow and the SFR-RAM copy must be kept in step or the classic
   "MUL AB then use B as the high byte" idiom reads a stale value. That idiom
   is all over this ROM's tile-address math (e.g. row*40 at 0xCD7C), and a
   stale high byte scatters whole tile rows to the wrong videoram addresses --
   the cause of the "tables and cards all over the screen" corruption. */
static inline void sync_b(struct cpu_80c32 *c) {
    c->ram[SFR_B] = c->b;
}

void cpu_80c32_init(struct cpu_80c32 *c) {
    memset(c, 0, sizeof(*c));
}

void cpu_80c32_reset(struct cpu_80c32 *c) {
    c->pc = 0; c->a = 0; c->b = 0; c->sp = 0x07; c->psw = 0; c->dptr = 0;
    c->cycles = 0;
    c->in_irq = 0;
    c->irq_depth = 0;
    c->irq_level[0] = c->irq_level[1] = 0;
    c->irq_pending = 0;   /* bit0 = last INT0 pin state, for the edge detect */
    /* Zero all three live timer counters so none inherits a stale count
       across a reset (MAME's device_reset() clears m_tl0/th0/tl1/th1/m_t2
       and the m_t*_cnt edge counters). c->ram[]'s copies of TL/TH get
       zeroed by the memset below. */
    c->t0 = c->t1 = c->t2 = 0;
    memset(c->ram, 0, 256);
    memset(c->iram_hi, 0, sizeof(c->iram_hi));
    c->ram[SFR_SP] = 0x07;
    c->ram[SFR_P0] = c->ram[SFR_P1] = c->ram[SFR_P2] = c->ram[SFR_P3] = 0xFF;
}

uint8_t cpu_80c32_sfr_get(struct cpu_80c32 *c, uint8_t a) {
    if (c->sfr_r) return c->sfr_r(c, a);
    return c->ram[a];
}

void cpu_80c32_sfr_set(struct cpu_80c32 *c, uint8_t a, uint8_t v) {
    if (c->sfr_w) c->sfr_w(c, a, v);
    else          c->ram[a] = v;
    if (a == SFR_ACC) { c->a = v; set_psw_p(c); }
    if (a == SFR_B)   c->b = v;
    /* PSW.P (bit 0) is hardware-controlled (parity of ACC) and NOT
       writable -- a direct write to PSW (MOV PSW,#x / POP PSW / etc.)
       leaves it as the CPU computed it. Matches MAME's psw_w():
       m_psw = (m_psw & 0x01) | (data & 0xfe). Without this, code that
       PUSH/POPs PSW around a routine could restore a stale P and then
       mis-branch on JB/JNB PSW.0. */
    if (a == SFR_PSW) {
        c->psw = (uint8_t)((c->psw & P) | (v & (uint8_t)~P));
        c->ram[SFR_PSW] = c->psw;
    }
    if (a == SFR_SP)  c->sp = v;
    if (a == SFR_DPL) c->dptr = (c->dptr & 0xFF00) | v;
    if (a == SFR_DPH) c->dptr = (c->dptr & 0x00FF) | ((uint16_t)v << 8);
        if (a == SFR_TL0) c->t0 = (c->t0 & 0xFF00) | v;
    if (a == SFR_TH0) c->t0 = (c->t0 & 0x00FF) | (v << 8);
    if (a == SFR_TL1) c->t1 = (c->t1 & 0xFF00) | v;
    if (a == SFR_TH1) c->t1 = (c->t1 & 0x00FF) | (v << 8);
    if (a == SFR_TL2) c->t2 = (c->t2 & 0xFF00) | v;
    if (a == SFR_TH2) c->t2 = (c->t2 & 0x00FF) | ((uint16_t)v << 8);
}

/* memory */
static inline uint8_t fetch(struct cpu_80c32 *c) {
    if (!c->code || c->pc >= c->code_size) { c->pc++; return 0; }
    return c->code[c->pc++];
}
static inline uint8_t codeat(struct cpu_80c32 *c, uint16_t a) {
    return (c->code && a < c->code_size) ? c->code[a] : 0;
}
/* Indirect internal-RAM access (@R0/@R1, PUSH/POP via SP). On real 8052
   silicon this is a physically separate 128-byte bank from the SFRs that
   happen to share the same 0x80-0xFF numbers -- see iram_hi's comment in
   cpu_80c32.h. Direct-addressed SFR access goes through dir_r/dir_w below,
   never through here. */
static inline uint8_t ir(struct cpu_80c32 *c, uint8_t a) {
    return (a < 0x80) ? c->ram[a] : c->iram_hi[a - 0x80];
}
static inline void    iw(struct cpu_80c32 *c, uint8_t a, uint8_t v) {
    if (a < 0x80) {
        c->ram[a] = v;
        if (c->iram_write) c->iram_write(c, a, v);
    } else {
        c->iram_hi[a - 0x80] = v;
    }
}

static inline uint8_t xr(struct cpu_80c32 *c, uint16_t a) {
    if (c->xread) return c->xread(c, a);
    if (c->xdata && a < c->xdata_size) return c->xdata[a];
    return 0xFF;
}
static inline void xw(struct cpu_80c32 *c, uint16_t a, uint8_t v) {
    if (c->xwrite) c->xwrite(c, a, v);
    else if (c->xdata && a < c->xdata_size) c->xdata[a] = v;
}

static inline uint8_t rbase(struct cpu_80c32 *c) {
    return (uint8_t)((c->psw >> 3) & 3) << 3;
}
static inline uint8_t rr(struct cpu_80c32 *c, uint8_t n) { return ir(c, rbase(c) + n); }
static inline void    rw(struct cpu_80c32 *c, uint8_t n, uint8_t v) { iw(c, rbase(c) + n, v); }

static inline void push(struct cpu_80c32 *c, uint8_t v) {
    iw(c, ++c->sp, v); c->ram[SFR_SP] = c->sp;
}
static inline uint8_t pop(struct cpu_80c32 *c) {
    uint8_t v = ir(c, c->sp--); c->ram[SFR_SP] = c->sp; return v;
}

/* direct read/write (handles SFR) */
static inline uint8_t dir_r(struct cpu_80c32 *c, uint8_t a) {
    return (a >= 0x80) ? cpu_80c32_sfr_get(c, a) : ir(c, a);
}
static inline void dir_w(struct cpu_80c32 *c, uint8_t a, uint8_t v) {
    if (a >= 0x80) cpu_80c32_sfr_set(c, a, v); else iw(c, a, v);
}
/* Read-modify-write variant of dir_r, for the specific opcodes the 8051
   architecture documents as RMW on ports (ANL/ORL/XRL dir, INC/DEC dir,
   DJNZ dir, JBC/CPL/SETB/CLR bit, MOV bit,C): those must read back the
   SFR's own output LATCH, not the live pin state a plain read (MOV A,dir,
   JB/JNB, ...) would see through sfr_r/dir_r. A machine's sfr_r callback
   can legitimately answer a plain read with something other than the last
   written value (e.g. this project's pepp0127 driver returns a live VBLANK
   bit for P3.bit0 and a hardcoded 0xFF for P1) -- an RMW that read through
   that same callback would silently corrupt/misrepresent the latch on its
   own write-back. */
static inline uint8_t dir_rmw_r(struct cpu_80c32 *c, uint8_t a) {
    return (a >= 0x80) ? c->ram[a] : ir(c, a);
}
static inline void sync_psw(struct cpu_80c32 *c) {
    cpu_80c32_sfr_set(c, SFR_PSW, c->psw);
}

/* bit address -> byte/bit */
static inline void bit_addr(uint8_t bit, uint8_t *byte, uint8_t *mask) {
    if (bit < 0x80) { *byte = 0x20 + (bit >> 3); *mask = 1u << (bit & 7); }
    else            { *byte = bit & 0xF8;         *mask = 1u << (bit & 7); }
}
static inline int bit_get(struct cpu_80c32 *c, uint8_t bit) {
    uint8_t b, m; bit_addr(bit, &b, &m);
    return (dir_r(c, b) & m) != 0;
}
/* RMW counterpart of bit_get, for JBC/CPL bit's own bit test -- see
   dir_rmw_r's comment. */
static inline int bit_get_rmw(struct cpu_80c32 *c, uint8_t bit) {
    uint8_t b, m; bit_addr(bit, &b, &m);
    return (dir_rmw_r(c, b) & m) != 0;
}
static inline void bit_set(struct cpu_80c32 *c, uint8_t bit, int v) {
    uint8_t b, m; bit_addr(bit, &b, &m);
    uint8_t x = dir_rmw_r(c, b);
    dir_w(c, b, v ? (x | m) : (x & ~m));
}

/* arithmetic helpers */
static inline void add8(struct cpu_80c32 *c, uint8_t v, int with_c) {
    /* Carry-in must be captured before PSW.C gets overwritten below with
       the result's own carry-out -- the AC computation used to re-read
       c->psw & C AFTER that overwrite, silently using the wrong operand
       (the new carry-out instead of the original carry-in) whenever
       ADDC's incoming carry differed from the result's outgoing carry. */
    int carryin = with_c && (c->psw & C) ? 1 : 0;
    uint16_t r = (uint16_t)c->a + v + carryin;
    uint8_t  a7 = c->a >> 7, v7 = v >> 7, r7 = (r >> 7) & 1;
    if (r > 0xFF) c->psw |= C; else c->psw &= ~C;
    if (((c->a & 0x0F) + (v & 0x0F) + carryin) > 0x0F)
        c->psw |= AC; else c->psw &= ~AC;
    /* OV: signed overflow */
    if ((a7 == v7) && (a7 != r7)) c->psw |= OV; else c->psw &= ~OV;
    c->a = (uint8_t)r; sync_a(c);
}
static inline void sub8(struct cpu_80c32 *c, uint8_t v) {
    int borrow = (c->psw & C) ? 1 : 0;
    int16_t r = (int16_t)c->a - v - borrow;
    if (r < 0) c->psw |= C; else c->psw &= ~C;
    if (((c->a & 0x0F) - (v & 0x0F) - borrow) < 0) c->psw |= AC; else c->psw &= ~AC;
    uint8_t a7 = c->a >> 7, v7 = v >> 7, r7 = ((uint8_t)r) >> 7;
    if ((a7 != v7) && (a7 != r7)) c->psw |= OV; else c->psw &= ~OV;
    c->a = (uint8_t)r; sync_a(c);
}

static void timers_tick(struct cpu_80c32 *c, int cycles)
{
    uint8_t tcon = c->ram[SFR_TCON];
    uint8_t tmod = c->ram[SFR_TMOD];
    int mode0 = tmod & 0x03;   /* Timer 0's mode bits, needed by both blocks below */

    /* ---- Timer 0 ---- */
    /* GATEx: when set, the timer only counts while the external INTx pin is
       also high. The external-pin-high case (GATE=1 AND pin high) isn't
       modeled (same as the reference emu8051 core), so GATE=1 simply stops
       the timer here -- run condition is "!(TMOD & GATE) && TRx". */
    if (mode0 == 3) {
        /* Mode 3: TL0 and TH0 become two INDEPENDENT 8-bit timers.
           TL0 keeps TR0/TF0/INT0 exactly as normal. TH0 is "borrowed" --
           it's gated by TR1 and sets TF1 on overflow instead of its own
           bit, which is why the Timer 1 block below must NOT also run
           while Timer 0 is in mode 3 (handled after this block). */
        if (!(tmod & GATE0) && (tcon & TR0)) {
            uint16_t tl = c->ram[SFR_TL0] + (uint16_t)cycles;
            if (tl > 0xFF) { tcon |= TF0; }
            c->ram[SFR_TL0] = (uint8_t)tl;
            c->t0 = c->ram[SFR_TL0];
        }
        /* TH0's borrowed 8-bit counter has no gate control at all in this
           mode (confirmed against MAME's update_timer_t0(), which comments
           this exact branch "No gate control or counting!" and tests only
           TR1) -- unlike every other timer path here, GATE1 must NOT stop
           it. */
        if (tcon & TR1) {
            uint16_t th = c->ram[SFR_TH0] + (uint16_t)cycles;
            if (th > 0xFF) { tcon |= TF1; }
            c->ram[SFR_TH0] = (uint8_t)th;
        }
    } else if (!(tmod & GATE0) && (tcon & TR0)) {
        int ct   = tmod & 0x04;   /* C/T# : 0=timer, 1=counter (we treat as timer) */

        if (mode0 == 0) {
            /* 13-bit: the hardware counter is (TH0<<5)|(TL0&0x1F) -- TL0
               only contributes its low 5 bits (its top 3 bits are real,
               addressable register bits that just aren't part of the
               count, and must be preserved on write-back), TH0 supplies
               all 8 high bits. Previously packed byte-aligned instead
               (TL0=low 8 bits, TH0=high 5), which is only coincidentally
               right when both start at 0 -- any firmware-written reload
               value would land in the wrong bit positions entirely. */
            uint16_t count = ((uint16_t)c->ram[SFR_TH0] << 5) | (c->ram[SFR_TL0] & 0x1F);
            count = (count + cycles) & 0x1FFF;
            if (count < (uint16_t)cycles) tcon |= TF0; /* wrapped */
            c->ram[SFR_TH0] = (uint8_t)(count >> 5);
            c->ram[SFR_TL0] = (c->ram[SFR_TL0] & 0xE0) | (uint8_t)(count & 0x1F);
            c->t0 = count;
        } else if (mode0 == 1) { /* 16-bit */
            uint32_t v = c->t0 + cycles;
            if (v > 0xFFFF) {
                tcon |= TF0;
            }
            c->t0 = (uint16_t)v;
            c->ram[SFR_TL0] = c->t0 & 0xFF;
            c->ram[SFR_TH0] = c->t0 >> 8;
        } else if (mode0 == 2) { /* 8-bit auto-reload */
            uint8_t th = c->ram[SFR_TH0];
            int left = cycles;
            while (left--) {
                c->t0 = (c->t0 + 1) & 0xFF;
                if (c->t0 == 0) {
                    c->t0 = th;
                    tcon |= TF0;
                }
            }
            c->ram[SFR_TL0] = c->t0 & 0xFF;
        }
    }

    /* ---- Timer 1 (same pattern) ---- */
    /* Skipped entirely when Timer 0 is in mode 3: TR1/TF1 belong to TH0
       above in that case, and Timer 1's own mode bits are meaningless
       until Timer 0 leaves mode 3 (matches real 8051 behavior). */
    if (mode0 != 3 && !(tmod & GATE1) && (tcon & TR1)) {
        int mode1 = (tmod >> 4) & 0x03;
        if (mode1 == 0) { /* 13-bit -- see the Timer0 mode-0 comment above */
            uint16_t count = ((uint16_t)c->ram[SFR_TH1] << 5) | (c->ram[SFR_TL1] & 0x1F);
            count = (count + cycles) & 0x1FFF;
            if (count < (uint16_t)cycles) tcon |= TF1;
            c->ram[SFR_TH1] = (uint8_t)(count >> 5);
            c->ram[SFR_TL1] = (c->ram[SFR_TL1] & 0xE0) | (uint8_t)(count & 0x1F);
            c->t1 = count;
        } else if (mode1 == 1) {
            uint32_t v = c->t1 + cycles;
            if (v > 0xFFFF) {
                tcon |= TF1;
            }
            c->t1 = (uint16_t)v;
            c->ram[SFR_TL1] = c->t1 & 0xFF;
            c->ram[SFR_TH1] = c->t1 >> 8;
        } else if (mode1 == 2) {
            uint8_t th = c->ram[SFR_TH1];
            int left = cycles;
            while (left--) {
                c->t1 = (c->t1 + 1) & 0xFF;
                if (c->t1 == 0) {
                    c->t1 = th;
                    tcon |= TF1;
                }
            }
            c->ram[SFR_TL1] = c->t1 & 0xFF;
        }
    }

    c->ram[SFR_TCON] = tcon;

    /* ---- Timer 2 (8052/80C32-only, previously entirely unimplemented) ----
       Unlike Timer0/1, Timer2 has no TMOD mode bits or gate control at
       all -- it's always a free-running 16-bit counter whose behavior is
       governed entirely by T2CON. CT2 (counter vs timer) is ignored here,
       same simplification this core already makes for T0/T1's C/T# bit
       (always "treat as timer", counting machine cycles). EXEN2/CP
       (external T2EX pin capture/reload) aren't modeled, same as other
       unmodeled external-pin features elsewhere in this core -- no
       external pin is wired to this emulation, so that path just never
       triggers, matching the existing GATE-pin-high case being unmodeled
       too. */
    uint8_t t2con = c->ram[SFR_T2CON];
    if (t2con & T2CON_TR2) {
        uint32_t v = (uint32_t)c->t2 + (uint32_t)cycles;
        if (v > 0xFFFF) {
            /* 16-bit auto-reload from RCAP2L/RCAP2H on overflow -- always
               happens in this mode (no separate "reload enable" bit),
               matching real 8052 hardware. TF2 is set on overflow UNLESS
               Timer2 is running as the serial baud-rate generator
               (RCLK or TCLK set), which the datasheet documents as never
               setting TF2 -- confirmed against MAME's i8052.h comments
               ("RCLK: Receive Clock", "TCLK: Transmit Clock" sharing this
               same T2CON). Baud-rate generation itself isn't modeled
               (no serial port consumer here), but the flag-suppression
               rule is cheap to get right regardless. */
            uint16_t reload = ((uint16_t)c->ram[SFR_RCAP2H] << 8) | c->ram[SFR_RCAP2L];
            v = (uint32_t)reload + (v - 0x10000);
            if (!(t2con & (T2CON_RCLK | T2CON_TCLK)))
                t2con |= T2CON_TF2;
        }
        c->t2 = (uint16_t)v;
        c->ram[SFR_TL2] = c->t2 & 0xFF;
        c->ram[SFR_TH2] = c->t2 >> 8;
        c->ram[SFR_T2CON] = t2con;
    }
}

/* External IRQ0 pin (INT0, active-low on real silicon -- here `state` is the
   logical "assert" level: 1 = requesting an interrupt). Driven by the CRTC
   VSYNC line in peplus.

   IT0 (TCON.0) selects the trigger mode, exactly as MAME's handle_irq() does:
     - IT0 = 1  EDGE-triggered: IE0 latches on the deassert->assert edge ONLY,
                and is NOT cleared when the line deasserts. It is cleared by
                hardware on interrupt entry (see check_interrupts).
     - IT0 = 0  LEVEL-triggered: IE0 simply follows the line -- set while
                asserted, cleared when it deasserts, and NOT cleared on entry
                (so the ISR keeps re-firing as long as the line is held).
   pepp0127 runs INT0 edge-triggered (TCON = 0x51, IT0 set). The previous
   implementation here was pure level behaviour with no entry-clear, so a
   VSYNC pulse held for ~100 cycles re-fired the tiny INT0 ISR ~7 times per
   frame instead of once -- wrong sound-square-wave rate and ~7x the
   interrupt overhead. `irq_pending` bit0 caches the last line state for the
   edge detect. */
void cpu_80c32_set_irq0(struct cpu_80c32 *c, int state)
{
    uint8_t prev = c->irq_pending & 0x01;
    uint8_t now  = state ? 1 : 0;
    c->irq_pending = (c->irq_pending & ~0x01) | now;

    if (c->ram[SFR_TCON] & IT0) {          /* edge-triggered */
        if (now && !prev) c->ram[SFR_TCON] |= IE0;   /* latch on the rising edge */
        /* falling edge: leave IE0 as-is (cleared on interrupt entry) */
    } else {                               /* level-triggered */
        if (now) c->ram[SFR_TCON] |= IE0;
        else     c->ram[SFR_TCON] &= ~IE0;
    }
}

/* check_interrupts tests the live TCON flag bits (IE0/TF0/TF1) directly,
   the same way MAME's check_irqs() and emu8051's handle_interrupts() do,
   so firmware that manually SETB/CLR's TF0/TF1/IE0 (to force or suppress
   an interrupt) is honoured. (c->irq_pending is NOT an interrupt shadow --
   its bit0 only caches the last INT0 pin state for set_irq0's edge detect.)

   ---- 2-level priority / nesting (added 2026-08-31) ----
   Before this, a single `in_irq` boolean blocked EVERY interrupt while any
   ISR ran. Real 8051/8052 has two priority levels chosen per source by IP
   (0xB8): a HIGH-level request preempts a running LOW-level ISR (nothing
   preempts HIGH; LOW cannot preempt LOW; HIGH cannot preempt HIGH), so up
   to two ISRs nest. The IGT "Player's Edge Plus" slots ROMs (peps0043 /
   peps0040 / pex0827s -- "Double Diamond" etc.) set IP=0x15, making INT0
   (VSYNC), INT1 (DUART) and the serial port HIGH while Timer0/Timer1 stay
   LOW, and their frame handler is a coroutine that RUNS INSIDE the Timer0
   ISR -- so a VSYNC that cannot preempt it desynced frame timing after a
   few frames and dumped the game onto a setup/error screen. MAME shows
   INT0 preempting a timer ISR ~21x per 18s run; this reproduces that.

   Design: irq_level[] is the in-service level stack (index 0 = outer ISR,
   1 = inner), irq_depth its count (0/1/2). A new request at level L is
   taken only when depth == 0, OR (depth == 1 AND L == 1 AND the running
   ISR is level 0). RETI (opcode 0x32) pops one entry. `in_irq` is kept as
   a mirror of (irq_depth != 0) purely for backward compatibility -- new
   code should look at irq_depth / irq_level, never in_irq.

   Same-level polling order matches Intel/MAME: INT0, T0, INT1, T1, serial,
   T2. (Serial RI/TI is not modelled here -- the 8051's on-chip UART has no
   consumer in these machines; the DUART at xdata 0xE000 is a separate
   external device reached through INT1. The PS/serial slot is left in the
   priority math so a future serial implementation drops straight in.) */
/* Returns the number of machine cycles the interrupt dispatch consumed
   (0 if none was taken, 2 if one was -- the implicit LCALL to the vector,
   matching MAME's "m_inst_cycles += 2" in check_irqs()). */
static int check_interrupts(struct cpu_80c32 *c)
{
    uint8_t ie = c->ram[SFR_IE];
    if (!(ie & EA))
        return 0;

#if !CPU_80C32_IRQ_PRIORITY
    /* Legacy mode: no priority, no nesting -- any ISR blocks all interrupts. */
    if (c->in_irq)
        return 0;
    int cur_level = -1;
#else
    /* Highest in-service level right now (-1 = nothing running). A new
       request must be STRICTLY higher than this to be taken, and can only
       raise the nesting depth to 2. */
    int cur_level = (c->irq_depth == 0) ? -1 : c->irq_level[c->irq_depth - 1];
    if (c->irq_depth >= 2)
        return 0;                 /* already two deep: real HW nests no further */
#endif

    uint8_t tcon  = c->ram[SFR_TCON];
#if CPU_80C32_IRQ_PRIORITY
    uint8_t ip    = c->ram[SFR_IP];
#else
    uint8_t ip    = 0;            /* force every source to level 0 */
#endif
    uint8_t t2con = c->ram[SFR_T2CON];

    /* Build the candidate list in hardware polling order. `pending` is the
       raw request being asserted AND its enable bit; `lvl` is its IP level. */
    struct { int pending; int lvl; uint16_t vec; uint8_t clr; } cand[6] = {
        { (tcon & IE0)  && (ie & EX0), (ip & PX0) ? 1 : 0, 0x0003,
          (tcon & IT0) ? IE0 : 0 },                 /* INT0: auto-clear IE0 only if edge-triggered */
        { (tcon & TF0)  && (ie & ET0), (ip & PT0) ? 1 : 0, 0x000B, TF0 },
        { (tcon & IE1)  && (ie & EX1), (ip & PX1) ? 1 : 0, 0x0013,
          (tcon & IT1) ? IE1 : 0 },
        { (tcon & TF1)  && (ie & ET1), (ip & PT1) ? 1 : 0, 0x001B, TF1 },
        { 0 /* serial RI/TI: not modelled */,        (ip & PS)  ? 1 : 0, 0x0023, 0 },
        { (t2con & (T2CON_TF2 | T2CON_EXF2)) && (ie & ET2),
          (ip & PT2) ? 1 : 0, 0x002B, 0 },          /* T2: HW never auto-clears TF2/EXF2 */
    };

    /* One pass at the HIGH level, then (if nothing) one pass at LOW --
       exactly how real hardware resolves it: all level-1 sources beat all
       level-0 sources regardless of polling order. */
    for (int want = 1; want >= 0; want--) {
        if (want <= cur_level)
            break;                /* can't preempt an equal/higher ISR */
        for (int i = 0; i < 6; i++) {
            if (!cand[i].pending || cand[i].lvl != want)
                continue;
            if (cand[i].clr)
                c->ram[SFR_TCON] &= (uint8_t)~cand[i].clr;
            c->irq_level[c->irq_depth++] = (uint8_t)want;
            c->in_irq = 1;        /* legacy mirror: any ISR in service */
            push(c, c->pc & 0xFF);
            push(c, c->pc >> 8);
            c->pc = cand[i].vec;
            return 2;
        }
    }
    return 0;
}

/* ---------- main step ---------- */
int cpu_80c32_step(struct cpu_80c32 *c) {
    if (c->pc_trace) c->pc_trace(c);
    uint8_t op = fetch(c);
    int cy = 1;
    uint8_t n, addr, imm, hi, lo;
    int8_t rel;

    switch (op) {

    /* ---- 0x00-0x0F ---- */
    case 0x00: break;                                           /* NOP */
    case 0x01: case 0x21: case 0x41: case 0x61:
    case 0x81: case 0xA1: case 0xC1: case 0xE1:                 /* AJMP */
        lo = fetch(c);
        c->pc = (c->pc & 0xF800) | ((uint16_t)(op & 0xE0) << 3) | lo;
        cy = 2; break;
    case 0x02: hi = fetch(c); lo = fetch(c); c->pc = (hi<<8)|lo; cy = 2; break; /* LJMP */
    case 0x03: c->a = (c->a >> 1) | (c->a << 7); sync_a(c); break;             /* RR A */
    case 0x04: c->a++; sync_a(c); break;                                        /* INC A */
    case 0x05: addr = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) + 1); cy = 1; break; /* INC dir */
    case 0x06: {   /* INC @R0 */
  uint8_t addr = rr(c, 0);
  iw(c, addr, ir(c, addr) + 1);
} break;                                 

case 0x07: { /* INC @R1 */
  uint8_t addr = rr(c, 1);
  iw(c, addr, ir(c, addr) + 1);
} break;                                 

    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:                                /* INC Rn */
        n = op & 7; rw(c, n, rr(c, n) + 1); break;

    /* ---- 0x10-0x1F ---- */
    case 0x10: {                                                                /* JBC bit,rel */
        uint8_t bit = fetch(c); rel = (int8_t)fetch(c);
        if (bit_get_rmw(c, bit)) { bit_set(c, bit, 0); c->pc += rel; }
        cy = 2;
    } break;
    case 0x11: case 0x31: case 0x51: case 0x71:
    case 0x91: case 0xB1: case 0xD1: case 0xF1:                                 /* ACALL */
        lo = fetch(c);
        push(c, c->pc & 0xFF); push(c, c->pc >> 8);
        c->pc = (c->pc & 0xF800) | ((uint16_t)(op & 0xE0) << 3) | lo;
        cy = 2; break;
    case 0x12: hi = fetch(c); lo = fetch(c);                                    /* LCALL */
        push(c, c->pc & 0xFF); push(c, c->pc >> 8);
        c->pc = (hi << 8) | lo; cy = 2; break;
    case 0x13: {                                                                /* RRC A */
        uint8_t oldc = (c->psw & C) ? 1 : 0;
        if (c->a & 1) c->psw |= C; else c->psw &= ~C;
        c->a = (c->a >> 1) | (oldc << 7); sync_a(c);
    } break;
    case 0x14: c->a--; sync_a(c); break;                                        /* DEC A */
    case 0x15: addr = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) - 1); break;      /* DEC dir */
    case 0x16: {
  uint8_t addr = rr(c, 0);
  iw(c, addr, ir(c, addr) - 1);
} break;
case 0x17: {
  uint8_t addr = rr(c, 1);
  iw(c, addr, ir(c, addr) - 1);
} break;
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        n = op & 7; rw(c, n, rr(c, n) - 1); break;

    /* ---- 0x20-0x2F ---- */
    case 0x20: { uint8_t bit = fetch(c); rel = (int8_t)fetch(c);                /* JB */
        if (bit_get(c, bit)) c->pc += rel; cy = 2; } break;
    case 0x22: hi = pop(c); lo = pop(c); c->pc = (hi << 8) | lo; cy = 2; break; /* RET */
    case 0x23: c->a = (c->a << 1) | (c->a >> 7); sync_a(c); break;             /* RL A */
    case 0x24: add8(c, fetch(c), 0); break;                                     /* ADD A,# */
    case 0x25: add8(c, dir_r(c, fetch(c)), 0); break;                           /* ADD A,dir */
    case 0x26: add8(c, ir(c, rr(c, 0)), 0); break;
    case 0x27: add8(c, ir(c, rr(c, 1)), 0); break;
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        add8(c, rr(c, op & 7), 0); break;

    /* ---- 0x30-0x3F ---- */
    case 0x30: { uint8_t bit = fetch(c); rel = (int8_t)fetch(c);                /* JNB */
        if (!bit_get(c, bit)) c->pc += rel; cy = 2; } break;
        case 0x32: {
        uint8_t hi = pop(c), lo = pop(c);
        c->pc = (hi << 8) | lo;
        /* Pop one level off the in-service stack. RETI unwinds exactly one
           nesting level -- a HIGH ISR that preempted a LOW one returns to
           the LOW ISR, which is still in service until its own RETI. Guard
           against a stray RETI (firmware bug / our miscount) underflowing. */
        if (c->irq_depth) c->irq_depth--;
        c->in_irq = (c->irq_depth != 0);   /* legacy mirror */
        cy = 2;
        if (c->post_reti) c->post_reti(c);
    } break;                                                                    /* RETI */
    
    case 0x33: {                                                                /* RLC A */
        uint8_t oldc = (c->psw & C) ? 1 : 0;
        if (c->a & 0x80) c->psw |= C; else c->psw &= ~C;
        c->a = (c->a << 1) | oldc; sync_a(c);
    } break;
    case 0x34: add8(c, fetch(c), 1); break;                                     /* ADDC A,# */
    case 0x35: add8(c, dir_r(c, fetch(c)), 1); break;
    case 0x36: add8(c, ir(c, rr(c, 0)), 1); break;
    case 0x37: add8(c, ir(c, rr(c, 1)), 1); break;
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        add8(c, rr(c, op & 7), 1); break;

    /* ---- 0x40-0x4F ---- */
    case 0x40: rel = (int8_t)fetch(c); if (c->psw & C) c->pc += rel; cy = 2; break; /* JC */
    case 0x42: addr = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) | c->a); break;      /* ORL dir,A */
    case 0x43: addr = fetch(c); imm = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) | imm); cy = 2; break;
    case 0x44: c->a |= fetch(c); sync_a(c); break;                              /* ORL A,# */
    case 0x45: c->a |= dir_r(c, fetch(c)); sync_a(c); break;
    case 0x46: c->a |= ir(c, rr(c, 0)); sync_a(c); break;
    case 0x47: c->a |= ir(c, rr(c, 1)); sync_a(c); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->a |= rr(c, op & 7); sync_a(c); break;

    /* ---- 0x50-0x5F ---- */
    case 0x50: rel = (int8_t)fetch(c); if (!(c->psw & C)) c->pc += rel; cy = 2; break; /* JNC */
    case 0x52: addr = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) & c->a); break;          /* ANL dir,A */
    case 0x53: addr = fetch(c); imm = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) & imm); cy = 2; break;
    case 0x54: c->a &= fetch(c); sync_a(c); break;
    case 0x55: c->a &= dir_r(c, fetch(c)); sync_a(c); break;
    case 0x56: c->a &= ir(c, rr(c, 0)); sync_a(c); break;
    case 0x57: c->a &= ir(c, rr(c, 1)); sync_a(c); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        c->a &= rr(c, op & 7); sync_a(c); break;

    /* ---- 0x60-0x6F ---- */
    case 0x60: rel = (int8_t)fetch(c); if (c->a == 0) c->pc += rel; cy = 2; break; /* JZ */
    case 0x62: addr = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) ^ c->a); break;      /* XRL dir,A */
    case 0x63: addr = fetch(c); imm = fetch(c); dir_w(c, addr, dir_rmw_r(c, addr) ^ imm); cy = 2; break;
    case 0x64: c->a ^= fetch(c); sync_a(c); break;
    case 0x65: c->a ^= dir_r(c, fetch(c)); sync_a(c); break;
    case 0x66: c->a ^= ir(c, rr(c, 0)); sync_a(c); break;
    case 0x67: c->a ^= ir(c, rr(c, 1)); sync_a(c); break;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        c->a ^= rr(c, op & 7); sync_a(c); break;

    /* ---- 0x70-0x7F ---- */
    case 0x70: rel = (int8_t)fetch(c); if (c->a != 0) c->pc += rel; cy = 2; break; /* JNZ */
    case 0x72: { uint8_t bit = fetch(c);                                          /* ORL C,bit */
        if (bit_get(c, bit)) c->psw |= C; sync_psw(c); cy = 2; } break;
    case 0x73: c->pc = c->dptr + c->a; cy = 2; break;                             /* JMP @A+DPTR */
    case 0x74: c->a = fetch(c); sync_a(c); break;                                 /* MOV A,# */
    case 0x75: addr = fetch(c); imm = fetch(c); dir_w(c, addr, imm); cy = 2; break;
    case 0x76: iw(c, rr(c, 0), fetch(c)); break;                                  /* MOV @R0,# */
    case 0x77: iw(c, rr(c, 1), fetch(c)); break;
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        rw(c, op & 7, fetch(c)); break;

    /* ---- 0x80-0x8F ---- */
    case 0x80: rel = (int8_t)fetch(c); c->pc += rel; cy = 2; break;               /* SJMP */
    case 0x82: { uint8_t bit = fetch(c);                                          /* ANL C,bit */
        if (!bit_get(c, bit)) c->psw &= ~C; sync_psw(c); cy = 2; } break;
    case 0x83: c->a = codeat(c, c->pc + c->a); sync_a(c); cy = 2; break;          /* MOVC A,@A+PC */
    case 0x84: {                                                                  /* DIV AB */
        /* C is unconditionally cleared by DIV AB regardless of outcome
           (matches the reference core: PSW &= ~(C|OV) happens first,
           unconditionally, before the divide-by-zero check) -- this used
           to only clear C in the non-zero-divisor branch, leaving a
           stale C set if it was already 1 going into a divide-by-zero. */
        c->psw &= ~(C | OV);
        if (c->b == 0) { c->psw |= OV; }
        else {
            uint8_t q = c->a / c->b, r = c->a % c->b;
            c->a = q; c->b = r;
        }
        sync_a(c); sync_b(c); cy = 4;
    } break;
    case 0x85: { uint8_t s = fetch(c), d = fetch(c); dir_w(c, d, dir_r(c, s)); cy = 2; } break; /* MOV dir,dir */
    case 0x86: dir_w(c, fetch(c), ir(c, rr(c, 0))); cy = 2; break;                /* MOV dir,@R0 */
    case 0x87: dir_w(c, fetch(c), ir(c, rr(c, 1))); cy = 2; break;
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        dir_w(c, fetch(c), rr(c, op & 7)); cy = 2; break;

    /* ---- 0x90-0x9F ---- */
    case 0x90: hi = fetch(c); lo = fetch(c);                                      /* MOV DPTR,# */
        c->dptr = (hi << 8) | lo; c->ram[SFR_DPH] = hi; c->ram[SFR_DPL] = lo; cy = 2; break;
    case 0x92: bit_set(c, fetch(c), (c->psw & C) != 0); cy = 2; break;            /* MOV bit,C */
    case 0x93: c->a = codeat(c, c->dptr + c->a); sync_a(c); cy = 2; break;        /* MOVC A,@A+DPTR */
    case 0x94: sub8(c, fetch(c)); break;                                          /* SUBB A,# */
    case 0x95: sub8(c, dir_r(c, fetch(c))); break;
    case 0x96: sub8(c, ir(c, rr(c, 0))); break;
    case 0x97: sub8(c, ir(c, rr(c, 1))); break;
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        sub8(c, rr(c, op & 7)); break;

    /* ---- 0xA0-0xAF ---- */
    case 0xA0: { uint8_t bit = fetch(c);                                          /* ORL C,/bit */
        if (!bit_get(c, bit)) c->psw |= C; sync_psw(c); cy = 2; } break;
    case 0xA2: if (bit_get(c, fetch(c))) c->psw |= C; else c->psw &= ~C;         /* MOV C,bit */
               sync_psw(c); cy = 1; break;
    case 0xA3: c->dptr++; c->ram[SFR_DPH] = c->dptr >> 8; c->ram[SFR_DPL] = c->dptr; cy = 2; break;
    case 0xA4: {                                                                  /* MUL AB */
        uint16_t r = (uint16_t)c->a * c->b;
        c->a = r & 0xFF; c->b = r >> 8;
        if (c->b) c->psw |= OV; else c->psw &= ~OV;
        c->psw &= ~C; sync_a(c); sync_b(c); cy = 4;
    } break;
    /* MOV @Ri,dir and MOV Rn,dir are 2-cycle instructions (Intel MCS-51
       timing; MAME mcs51_cycles[0xA6..0xAF] == 2). Previously left at the
       default cy=1, making the emulated CPU run this group ~one machine
       cycle fast -- real drift for anything (peplus's coin/bill-validator
       pulse state machines) that times real-world windows against
       total_cycles(). */
    case 0xA6: iw(c, rr(c, 0), dir_r(c, fetch(c))); cy = 2; break;                /* MOV @R0,dir */
    case 0xA7: iw(c, rr(c, 1), dir_r(c, fetch(c))); cy = 2; break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        rw(c, op & 7, dir_r(c, fetch(c))); cy = 2; break;

    /* ---- 0xB0-0xBF ---- */
    case 0xB0: { uint8_t bit = fetch(c);                                          /* ANL C,/bit */
        if (bit_get(c, bit)) c->psw &= ~C; sync_psw(c); cy = 2; } break;
    case 0xB2: { uint8_t bit = fetch(c); bit_set(c, bit, !bit_get_rmw(c, bit)); } break; /* CPL bit */
    case 0xB3: c->psw ^= C; sync_psw(c); break;                      /* CPL C */
    case 0xB4: imm = fetch(c); rel = (int8_t)fetch(c);                            /* CJNE A,# */
        if (c->a < imm) c->psw |= C; else c->psw &= ~C;
        if (c->a != imm) c->pc += rel; cy = 2; break;
    case 0xB5: addr = fetch(c); rel = (int8_t)fetch(c); {                         /* CJNE A,dir */
        uint8_t v = dir_r(c, addr);
        if (c->a < v) c->psw |= C; else c->psw &= ~C;
        if (c->a != v) c->pc += rel; } cy = 2; break;
    case 0xB6: case 0xB7: {                                                       /* CJNE @Ri,# */
        uint8_t ri = (op & 1); imm = fetch(c); rel = (int8_t)fetch(c);
        uint8_t v = ir(c, rr(c, ri));
        if (v < imm) c->psw |= C; else c->psw &= ~C;
        if (v != imm) c->pc += rel; cy = 2;
    } break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {                                 /* CJNE Rn,# */
        n = op & 7; imm = fetch(c); rel = (int8_t)fetch(c);
        uint8_t v = rr(c, n);
        if (v < imm) c->psw |= C; else c->psw &= ~C;
        if (v != imm) c->pc += rel; cy = 2;
    } break;

    /* ---- 0xC0-0xCF ---- */
    case 0xC0: push(c, dir_r(c, fetch(c))); cy = 2; break;                        /* PUSH */
    case 0xC2: bit_set(c, fetch(c), 0); break;                                    /* CLR bit */
    case 0xC3: c->psw &= ~C; sync_psw(c); break;                     /* CLR C */
    case 0xC4: c->a = (c->a << 4) | (c->a >> 4); sync_a(c); break;                /* SWAP A */
    case 0xC5: addr = fetch(c); { uint8_t t = dir_r(c, addr); dir_w(c, addr, c->a); c->a = t; sync_a(c); } break; /* XCH A,dir */
    case 0xC6: { uint8_t t = ir(c, rr(c, 0)); iw(c, rr(c, 0), c->a); c->a = t; sync_a(c); } break;
    case 0xC7: { uint8_t t = ir(c, rr(c, 1)); iw(c, rr(c, 1), c->a); c->a = t; sync_a(c); } break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        n = op & 7; uint8_t t = rr(c, n); rw(c, n, c->a); c->a = t; sync_a(c);
    } break;

    /* ---- 0xD0-0xDF ---- */
    case 0xD0: addr = fetch(c); dir_w(c, addr, pop(c)); cy = 2; break;            /* POP */
    case 0xD2: bit_set(c, fetch(c), 1); break;                                    /* SETB bit */
    case 0xD3: c->psw |= C; sync_psw(c); break;                      /* SETB C */
    case 0xD4: {                                                                  /* DA A */
        /* Must stay a 16-bit intermediate across BOTH correction steps --
           an 8-bit c->a wraps silently if the low-nibble correction alone
           carries past 0xFF (e.g. A=0xFA: +0x06 wraps to 0x00 in 8 bits,
           losing the very overflow the second step needs to see), so the
           second step's "still needs correcting" test must include that
           carry-out, not just the post-wrap high nibble and original C. */
        uint16_t v = c->a;
        if ((v & 0x0F) > 9 || (c->psw & AC)) v += 0x06;
        if ((c->psw & C) || ((v & 0xF0) > 0x90) || (v > 0xFF)) {
            v += 0x60;
            c->psw |= C;
        }
        c->a = (uint8_t)v;
        sync_a(c);
    } break;
    case 0xD5: addr = fetch(c); rel = (int8_t)fetch(c); {                         /* DJNZ dir */
        uint8_t v = dir_rmw_r(c, addr) - 1; dir_w(c, addr, v);
        if (v) c->pc += rel; } cy = 2; break;
    case 0xD6: {                                                                  /* XCHD A,@R0 */
        uint8_t at = ir(c, rr(c, 0));
        uint8_t al = c->a & 0x0F, ah = c->a & 0xF0;
        c->a = ah | (at & 0x0F); iw(c, rr(c, 0), (at & 0xF0) | al); sync_a(c);
    } break;
    case 0xD7: {
        uint8_t at = ir(c, rr(c, 1));
        uint8_t al = c->a & 0x0F, ah = c->a & 0xF0;
        c->a = ah | (at & 0x0F); iw(c, rr(c, 1), (at & 0xF0) | al); sync_a(c);
    } break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        n = op & 7; rel = (int8_t)fetch(c);
        uint8_t v = rr(c, n) - 1; rw(c, n, v);
        if (v) c->pc += rel; cy = 2;
    } break;

    /* ---- 0xE0-0xEF ---- */
    case 0xE0: c->a = xr(c, c->dptr); sync_a(c); cy = 2; break;                   /* MOVX A,@DPTR */
    /* MOVX A,@Ri: real 8051/8052 hardware forms the 16-bit external
       address as {P2 : Ri} -- P2's output latch supplies the HIGH byte,
       Ri only supplies the low byte. This used to pass rr(c,n) (0-255)
       straight to xr() with P2 never consulted at all, silently masking
       every @Ri access to page 0x00xx regardless of what P2 held --
       confirmed via a real MAME debugger watchpoint that the ROM reaches
       the CRTC (0x2080/0x2081) exactly this way (MOVX @R0,A / @R1,A with
       P2=0x20), which this core had no means of ever reproducing. */
    case 0xE2: c->a = xr(c, (uint16_t)((c->ram[SFR_P2] << 8) | rr(c, 0))); sync_a(c); cy = 2; break;
    case 0xE3: c->a = xr(c, (uint16_t)((c->ram[SFR_P2] << 8) | rr(c, 1))); sync_a(c); cy = 2; break;
    case 0xE4: c->a = 0; sync_a(c); break;                                        /* CLR A */
    case 0xE5: c->a = dir_r(c, fetch(c)); sync_a(c); break;                       /* MOV A,dir */
    case 0xE6: c->a = ir(c, rr(c, 0)); sync_a(c); break;
    case 0xE7: c->a = ir(c, rr(c, 1)); sync_a(c); break;
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        c->a = rr(c, op & 7); sync_a(c); break;

    /* ---- 0xF0-0xFF ---- */
    case 0xF0: xw(c, c->dptr, c->a); cy = 2; break;                               /* MOVX @DPTR,A */
    case 0xF2: xw(c, (uint16_t)((c->ram[SFR_P2] << 8) | rr(c, 0)), c->a); cy = 2; break; /* MOVX @R0,A: {P2:R0} */
    case 0xF3: xw(c, (uint16_t)((c->ram[SFR_P2] << 8) | rr(c, 1)), c->a); cy = 2; break; /* MOVX @R1,A: {P2:R1} */
    case 0xF4: c->a = ~c->a; sync_a(c); break;                                    /* CPL A */
    case 0xF5: dir_w(c, fetch(c), c->a); break;                                   /* MOV dir,A */
    case 0xF6: iw(c, rr(c, 0), c->a); break;
    case 0xF7: iw(c, rr(c, 1), c->a); break;
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        rw(c, op & 7, c->a); break;

    default:
        /* reserved / unimplemented – treat as NOP */
        break;
    }

    /* Keep the PSW SFR RAM copy coherent with the live c->psw flags every
       instruction. Most flag-writers already sync it (add8/sub8/RRC/RLC/
       MUL/DIV/DA all end in sync_a()->set_psw_p() which stores c->psw; the
       explicit carry ops call sync_psw()), but **all 5 CJNE forms**
       (0xB4-0xBF) set PSW.C directly and do NOT -- and CJNE is what every
       loop-counter / state compare uses. `PUSH` reads c->ram[0xD0] for
       `PUSH PSW`; this ROM's Timer0 ISR (fires 3876x/sec) is
       `PUSH PSW ... POP PSW`, so a tick landing between a mainline
       `CJNE A,#n ; JNC/JC ...` pushed the STALE carry and the POP restored
       it, wiping the compare result -> the paytable-recolor loop
       (CJNE A,#$09 ; JNC -> draw box border) took the wrong branch and
       split the table at a timing-dependent row. One unconditional store
       here fixes every PSW reader at once; c->psw stays the source of
       truth. Cheap (one byte-store/instr). */
    c->ram[SFR_PSW] = c->psw;

    timers_tick(c, cy);
    /* Interrupt dispatch (if any) costs 2 extra machine cycles -- attribute
       them to this step and let the timers see that elapsed time too, the
       way MAME folds check_irqs()'s "+= 2" into the same burn_cycles(). */
    {
        int iack = check_interrupts(c);
        if (iack) {
            timers_tick(c, iack);
            cy += iack;
        }
    }
    c->cycles += cy;
    return cy;
}

void cpu_80c32_run(struct cpu_80c32 *c, int target) {
    int done = 0;
    while (done < target)
        done += cpu_80c32_step(c);
}

/*
********** Do not remove **********

Ported to C++ by Pierre Potgieter (BaasPierre)
Sole purpose is for emulation on ESP32 device

Licence
80c32 cpu source is https://github.com/jarikomppa/emu8051
The emulator core is written completely in ANSI C for portability, and the sources are available under the MIT license.
Copyright 2006 Jari Komppa
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.*/