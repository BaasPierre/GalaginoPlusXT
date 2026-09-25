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

#pragma GCC optimize("-O2")

#include <stdint.h>
#include "../../cpus/z80/Z80.h"

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
