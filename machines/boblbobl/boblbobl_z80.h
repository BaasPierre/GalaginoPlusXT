// boblbobl's own build of the shared Z80 core (boblbobl_z80.c), shared by
// that file and boblbobl.cpp.
#ifndef BOBLBOBL_Z80_H
#define BOBLBOBL_Z80_H

#include <stdbool.h>
#include "../../cpus/z80/Z80.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char *bbz_rd_page[3][32];
extern unsigned char *bbz_wr_page[3][32];
extern const unsigned char *const *bbz_rd_cur;
extern unsigned char *const *bbz_wr_cur;
void bbz_Step(Z80 *R);
void bbz_Int(Z80 *R, unsigned short Vector);
void bbz_Reset(Z80 *R);

#if BOBLBOBL_DBG_HARNESS
// harness counters (boblbobl.h dbg_*), updated per instruction
typedef struct {
  unsigned long *steps, *halt_steps, *pc_hist;   // pc_hist: 65536 entries
  void (*trace)(uint16_t pc);                   // after each instruction, 0 = none
} bbz_dbg_t;
#define BBZ_DBG_PARAM , const bbz_dbg_t *dbg
#else
#define BBZ_DBG_PARAM
#endif

// One slice of the main or sub CPU, exactly run_frame's former loop around
// bbz_Step: idle-loop skip, the instruction, cycle accounting, the vblank
// IRQ (irq0_line_hold: *irq_pending stays set until accepted). The caller
// selects the CPU's page tables (boblbobl::set_cpu) first. Returns the
// budget left (<= 0: the cycle debt carried into the next slice).
long bbz_RunSlice(Z80 *R, int cpu_n, long budget, bool *irq_pending BBZ_DBG_PARAM);

#ifdef __cplusplus
}
#endif

#endif
