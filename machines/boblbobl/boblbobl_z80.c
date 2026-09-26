// boblbobl's own build of the shared Z80 core (src/cpus/z80/Z80.c, included
// unchanged below) - the mpatrol recipe (debug/speed hacks.txt section 6).
// The shared core reaches the machine through OpZ80_INL / RdZ80 / WrZ80
// (emulation.cpp): two calls, a virtual call and a switch on current_cpu per
// memory byte. Here every byte goes through the running CPU's 2KB page
// table instead (bbz_rd_cur / bbz_wr_cur, 32 pages each, set by
// boblbobl::set_cpu): a page is either a direct pointer to the bytes the
// machine's main_rd / sub_rd / audio_rd (main_wr / ...) would use, or 0 for
// "call the machine" (I/O, latches, palette, anything with a side effect).
// The tables are built and kept current in boblbobl.cpp (reset, bankswitch,
// game_started).
//
// BOBLBOBL_Z80_IRAM (below): the core's hot functions in IRAM. That fits
// when boblbobl is the only machine built (the shared core is then unused
// and left out by the linker); set it to 0 for a multi-machine build that
// overflows IRAM.
//
// Jump tables: the ESP32 Arduino build compiles with -fno-jump-tables, so
// every opcode switch (StepZ80, CodesCB/ED/DD/FD) became a binary search -
// about 8 compare-and-branch levels per opcode, twice for IX/IY opcodes
// (device benchmark: 140 cycles per simple Z80 instruction, 257 per IX/IY
// one). With jump tables an opcode is one table load and one jump. Same C
// code, same behaviour; the tables (1KB each) are in flash .rodata, which
// stays in core 1's cache while it emulates. Also ~4.7KB less IRAM.

#pragma GCC optimize("-O2")
#pragma GCC optimize("-fjump-tables")

#include <stdint.h>
#include "../../cpus/z80/Z80.h"
#include "boblbobl_z80.h"

#ifndef BOBLBOBL_Z80_IRAM
#define BOBLBOBL_Z80_IRAM 1
#endif
#if !BOBLBOBL_Z80_IRAM
#undef IRAM_ATTR
#define IRAM_ATTR
#endif

const unsigned char *bbz_rd_page[3][32];
unsigned char *bbz_wr_page[3][32];
const unsigned char *const *bbz_rd_cur = bbz_rd_page[0];
unsigned char *const *bbz_wr_cur = bbz_wr_page[0];

static __inline byte bbz_Rd(word A)
{
  const unsigned char *p = bbz_rd_cur[A >> 11];
  if (p)
    return p[A & 0x07ff];
  return RdZ80(A);                                  // boblbobl::rdZ80
}

static __inline void bbz_Wr(word A, byte V)
{
  unsigned char *p = bbz_wr_cur[A >> 11];
  if (p)
  {
    p[A & 0x07ff] = V;
    return;
  }
  WrZ80(A, V);                                      // boblbobl::wrZ80
}

static word bbz_Loop(Z80 *R) { (void)R; return INT_NONE; }   // only ExecZ80/RunZ80 (unused)

IRAM_ATTR void bbz_Step(Z80 *R);
IRAM_ATTR void bbz_Int(Z80 *R, word Vector);

// the shared core's names -> this build's
#define OpZ80_INL bbz_Rd
#define RdZ80     bbz_Rd
#define WrZ80     bbz_Wr
#define StepZ80   bbz_Step
#define IntZ80    bbz_Int
#define ResetZ80  bbz_Reset
#define ExecZ80   bbz_Exec
#define RunZ80    bbz_Run
#define LoopZ80   bbz_Loop

#include "../../cpus/z80/Z80.c"

// ---------------------------------------------------------------------------
// Whole slices (bbz_RunSlice, see boblbobl_z80.h). run_frame used to call
// bbz_Step once per Z80 instruction and do the idle check, cycle accounting
// and IRQ check around every call; here the same statements run in one
// loop, with the instruction (StepZ80's body, line for line) inlined, so
// there is no call per instruction and R stays in a register.
// Device benchmark before: ~210 ESP32 cycles per game instruction of which
// ~96 are the instruction itself.

// run_frame's bb_idle_loop_cycles, given the opcode byte b0 at pc (already
// fetched): 0x01ED "jr $" (main), 0x000A "jp $" (sub). Only JR / JP opcodes
// get here, so the ROM operand bytes are read for ~1 instruction in 20
// instead of 2-3 page-table reads before every instruction (device bench:
// that check was ~50 of the 149 cycles per simple instruction).
static __inline int bbz_idle_loop_cycles(int cpu_n, unsigned short pc, unsigned char b0)
{
  if (pc >= 0x7ffd)
    return 0;
  const unsigned char *const *rom = bbz_rd_page[cpu_n];
  unsigned short p1 = pc + 1, p2 = pc + 2;
  unsigned char b1 = rom[p1 >> 11][p1 & 0x7ff];
  if (b0 == 0x18 && b1 == 0xfe)                                          // jr $
    return 12;
  if (b0 == 0xc3 && (b1 | (rom[p2 >> 11][p2 & 0x7ff] << 8)) == pc)       // jp $
    return 10;
  return 0;
}

IRAM_ATTR long bbz_RunSlice(register Z80 *R, int cpu_n, long budget, bool *irq_pending BBZ_DBG_PARAM)
{
  register byte I;
  register pair J;

  while (budget > 0)
  {
    const bool iff = (R->IFF & IFF_1) != 0;
    const int icount_before = R->ICount;

    // ---- StepZ80 (opcode fetch)
    I = OpZ80_INL(R->PC.W++);

    // bb_skip_idle: at a "jr $" / "jp $" with no interrupt possible, burn
    // the rest of the slice in whole idle-loop passes. The same test as
    // before the fetch (the opcode byte is the ROM byte bb_skip_idle read:
    // idle loops are in fixed ROM, 0000-7fff); PC is put back unchanged.
    if ((I == 0x18 || I == 0xc3) && !(R->IFF & IFF_EI) && !(*irq_pending && (R->IFF & IFF_1)))
    {
      int cyc = bbz_idle_loop_cycles(cpu_n, (unsigned short)(R->PC.W - 1), I);
      if (cyc)
      {
        R->PC.W--;
        long n = (budget + cyc - 1) / cyc;
        budget -= n * cyc;
        R->ICount -= (int)(n * cyc);
        break;
      }
    }

    // ---- StepZ80 (the instruction)
    R->ICount -= Cycles[I];
    switch (I)
    {
#include "../../cpus/z80/Codes.h"
      case PFX_CB: CodesCB(R); break;
      case PFX_ED: CodesED(R); break;
      case PFX_FD: CodesFD(R); break;
      case PFX_DD: CodesDD(R); break;
    }
    if (R->IFF & IFF_EI)
      R->IFF = (R->IFF & ~IFF_EI) | IFF_1; /* Done with AfterEI state */
    // ----

#if BOBLBOBL_DBG_HARNESS
    (*dbg->steps)++;
    if (R->IFF & IFF_HALT) (*dbg->halt_steps)++;
    dbg->pc_hist[R->PC.W]++;
#endif
    long consumed = (long)(icount_before - R->ICount);
    if (consumed <= 0)
      consumed = 1;
    budget -= consumed;
    // bb_irq_ok: IFF1 set before and after the instruction
    if (*irq_pending && iff && (R->IFF & IFF_1))
    {
      *irq_pending = false;
      bbz_Int(R, INT_IRQ);
      budget -= 13;
    }
#if BOBLBOBL_DBG_HARNESS
    if (dbg->trace)
      dbg->trace(R->PC.W);
#endif
  }
  return budget;
}

