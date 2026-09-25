#ifndef _EMULATION_H_
#define _EMULATION_H_

#include "cpus/z80/Z80.h"
#include "cpus/i8048/i8048.h"
#include "cpus/m6809/m6809.h"

 #define DEBUG_TIMING  // enable for debug

#ifdef DEBUG_TIMING
static int counter;
static unsigned long timeTotal = 0;
// BUG FIX (2026-09-20, flstory "game is very slow" investigation - see
// src/machines/flstory/notes.txt): cpuStart is written by emulation_task()
// (one FreeRTOS task/core) and read by emulation_videoRendered() (called
// from main.cpp's own task, a DIFFERENT task/core) with no synchronization
// - a plain (non-volatile) shared variable read/written across tasks can
// be cached/reordered by the compiler and is not guaranteed visible
// across cores on a dual-core target, producing nonsensical debug numbers
// (observed: "Video: 5760ms" inside a logged 1-second window, impossible
// as real elapsed time). Marked volatile (matching the existing giveCount
// pattern below) so both tasks always see the other's latest write - not
// a full mutex (this is best-effort debug instrumentation, not something
// gameplay-correctness-critical), but removes the compiler/cache-coherency
// source of garbage readings. Does not fix any REAL gameplay slowdown by
// itself - only the debug print's own honesty.
static volatile unsigned long cpuStart;
static unsigned long cpuSum = 0;
static volatile unsigned long videoSum = 0;
static unsigned long runFrameCount = 0;  // total run_frame() calls, incl. catch-up frames
static volatile unsigned long giveCount = 0;  // total emulation_notifyGive() calls (video/main core)
#endif

void emulation_start(void);
void emulation_stop(void);
void emulation_videoRendered(void);
void emulation_notifyGive(void);
void emulation_task(void *p);

#ifdef __cplusplus
extern "C" void OutZ80(unsigned short Port, unsigned char Value);
extern "C" unsigned char InZ80(unsigned short Port);
extern "C" void WrZ80(unsigned short Addr, unsigned char Value);
extern "C" unsigned char RdZ80(unsigned short Addr);
extern "C" void StepZ80(Z80 *R);
extern "C" unsigned char OpZ80_INL(unsigned short Addr);
extern "C" void PatchZ80(Z80 *R);
#endif

#ifdef __cplusplus
extern "C" void i8048_reset(i8048_state_S *state);
extern "C" void i8048_step(i8048_state_S *state);

extern "C" void i8048_port_write(i8048_state_S *state, unsigned char port, unsigned char pos);
extern "C" unsigned char i8048_port_read(i8048_state_S *state, unsigned char port);
extern "C" unsigned char i8048_xdm_read(i8048_state_S *state, unsigned char addr);
extern "C" void i8048_xdm_write(i8048_state_S *state, unsigned char addr, unsigned char data);
extern "C" unsigned char i8048_rom_read(i8048_state_S *state, unsigned short addr);
#endif

#ifdef __cplusplus
extern "C" unsigned char m6809_read(m6809_state *s, uint16_t addr);
extern "C" void m6809_write(m6809_state *s, uint16_t addr, uint8_t val);
extern "C" unsigned char m6809_read_opcode(m6809_state *s, uint16_t addr);
#endif
#endif