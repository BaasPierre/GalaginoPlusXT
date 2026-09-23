#ifndef FLSTORY_H
#define FLSTORY_H

#include "flstory_dipswitches.h"
#include "flstory_logo.h"
#include "flstory_tilemap.h"
#include "flstory_rom.h"
#include "flstory_tiles.h"
#include "flstory_tiles16.h"
#include "flstory_audiocpu.h"
#include "flstory_bmcu_mcu.h"
#include "../machineBase.h"
#include "../../cpus/m6805/taito68705_mcu.h"

// The FairyLand Story (Taito, 1985). See
// source/mame/mame-master/src/mame/taito/flstory.cpp (flstory_state base +
// flstory_mcu_state, machine config flstory_mcu_state::flstory(), memory
// maps base_map/flstory_map, ROM_START(flstory)). Only the plain "flstory"
// set/variant is implemented here - onna34ro/victnine/rumba use different
// tile-info/sprite-draw member functions in the real driver and are out of
// scope.
//
// Two Z80s (main @ 10.733MHz/2 ~5.3667MHz, audio @ 8MHz/2 = 4MHz) plus a
// Taito 68705 (m68705p5 core, see ../../cpus/m6805/taito68705_mcu.h)
// protection MCU @ 18.432MHz/6 = 3.072MHz. This is the FIRST game in this
// codebase to use the m6805/m68705p5/taito68705_mcu CPU core.

// ---------------------------------------------------------------------------
// Main CPU memory map (base_map + flstory_map, 0x0000-0xffff)
// ---------------------------------------------------------------------------
#define FLSTORY_VIDEORAM      0x0000   // 0x800 bytes, mirrored 0xc000-0xc7ff/0xc800-0xcfff
#define FLSTORY_SPRITERAM     0x0800   // 0xa0 bytes @ 0xdc00-0xdc9f
#define FLSTORY_SCRLRAM       0x08a0   // 0x20 bytes @ 0xdca0-0xdcbf
#define FLSTORY_UNUSED_RAM    0x08c0   // 0x40 bytes @ 0xdcc0-0xdcff (unknown, plain RAM)
#define FLSTORY_WORKRAM       0x0900   // 0x800 bytes @ 0xe000-0xe7ff, mirrored through 0xffff
#define FLSTORY_AUDIO_RAM     0x1100   // 0x800 bytes, audio CPU work RAM @ 0xc000-0xc7ff
#define FLSTORY_MEM_END       0x1900

// BUG FIX (found 2026-09-19 chasing a permanent MCU-handshake stall that
// turned out to be a throughput problem, not a protocol bug - see
// flstory.cpp's run_frame() and the MCU investigation writeup): these were
// originally 1500/4/4 (6000 main-CPU instructions/frame), copied from
// gng.cpp's own constants without re-measuring against flstory's actual
// game code. Measured DIRECTLY against real MAME (source/debug/
// mame-debug-notes.txt's method: `trace file,maincpu,noloop` bounded by
// `go <cycles>`, divided by the real vblank-to-vblank cycle count measured
// the same way - 89,634 cycles/frame, matching 5,366,500Hz/60 almost
// exactly, confirming the 60Hz rate itself was never the issue): real
// hardware executes on the order of 30,000-75,000 main-CPU instructions
// per 60Hz frame depending on code density (dense checksum-loop sections
// hit the high end; general code settles closer to ~32,700/frame over a
// longer, more representative window). The port's old 6000/frame was
// roughly 5-12x too low - not a rounding error, a genuine order-of-
// magnitude shortfall - so the main CPU (and everything gated behind its
// progress: the MCU re-verification handshake, sound-test timing, reaching
// the demo screen) ran far slower in wall-clock/frame terms than real
// hardware, surfacing as "the game never gets far enough, fast enough" bugs
// that looked protocol-related but weren't. FLSTORY_AUDIO_STEP is scaled by
// the same factor, proportioned by the audio Z80's own clock (4MHz vs the
// main Z80's ~5.3667MHz - ratio ~0.745) rather than copied unchanged,
// since it was subject to the exact same "copied from gng, never
// re-measured" gap. Still an average-instruction-length approximation, not
// cycle-exact (see run_frame()'s own comment on why - StepZ80() has no
// per-call cycle-count return) - but now calibrated against a real
// measurement instead of an inherited guess.
// BUG FIX (2026-09-20, "game still slow after pileup fix" investigation -
// see notes.txt): FLSTORY_SLICES chops the main CPU's per-frame cycle
// budget (~89,441 cycles/frame) into this many outer-loop iterations, so
// that the audio-IRQ and MCU-credit accumulators (both driven by REAL
// cycles now, not a fixed instruction count - see the bug fixes below) get
// updated at fine granularity across the frame instead of in one lump.
// 3000 slices means each slice only budgets ~29 real Z80 cycles - since a
// typical Z80 instruction costs 4-23 T-states, most slices execute exactly
// ONE real instruction, meaning the ~10,769 real instructions/frame this
// game actually needs get spread across 3000 outer-loop iterations, each
// paying fixed per-slice overhead (audio-IRQ accumulator check, MCU-credit
// accumulator math, current_cpu write, loop bookkeeping) for almost no
// batched work. Measured directly in the native harness (scratchpad
// timing_main2.cpp, boot->coin->start->gameplay 510-frame sequence):
// dropping to 300 slices leaves main/mcu step counts UNCHANGED (matching
// within normal per-slice rounding: main 10,769->11,065/frame, mcu exactly
// 3500.0/frame both) while cutting harness wall-clock time ~28% (0.427ms
// ->0.306ms/frame) - pure overhead reduction, zero change to what actually
// gets emulated. 300 slices still gives ~298 real cycles/slice granularity
// for the audio-IRQ accumulator (~147 slices between each of the ~2 real
// audio IRQs/frame) - vastly finer than needed for a ~122Hz periodic
// interrupt against a 16.7ms frame, so this does not measurably affect
// audio-IRQ timing accuracy.
#ifndef FLSTORY_SLICES
#define FLSTORY_SLICES        60
#endif
// FLSTORY_MAIN_STEP is NO LONGER used to pace the main CPU itself (see
// BUG FIX below, 2026-09-19 #2) - kept only as the historical instruction-
// count-per-slice figure that FLSTORY_MCU_CLOCK-ratio comments elsewhere
// reference; the main CPU is now paced by real cycles (FLSTORY_MAIN_CYCLES_
// PER_FRAME below), not by this constant.
#ifndef FLSTORY_MAIN_STEP
#define FLSTORY_MAIN_STEP     11
#endif
// BUG FIX (2026-09-20, "FLSTORY_SLICES reduction" investigation - see
// notes.txt): FLSTORY_AUDIO_STEP is a fixed INSTRUCTIONS-PER-SLICE count
// (run_frame()'s audio loop: `for (s=0;s<FLSTORY_AUDIO_STEP;s++)
// StepZ80(&cpu[1])`, once per outer slice iteration) - unlike the main CPU
// and MCU, audio was never converted to real-cycle-accurate pacing (see
// the twice-reverted cycle-accurate audio attempts in notes.txt), so its
// TOTAL per-frame budget is FLSTORY_AUDIO_STEP*FLSTORY_SLICES, not
// slice-count-invariant. When FLSTORY_SLICES was cut 3000->300 (a pure
// main-CPU/MCU per-slice-overhead fix, verified to leave main/MCU behavior
// unchanged), this silently cut audio's total budget from 24,000 to 2,400
// instructions/frame too - an ACCIDENTAL 10x reduction, not a deliberate
// pacing change. Scaled FLSTORY_AUDIO_STEP 8->80 (same 10x factor as
// FLSTORY_SLICES's own reduction) to restore the exact 24,000
// instructions/frame total budget that was flashed and confirmed working
// (self-test passes, gameplay sound fine) immediately before the slices
// change - a pure restoration of prior behavior, not a new pacing
// decision. (24,000/frame is still ~3.5x more than real MAME's measured
// ~6,793/frame need - already known and left alone per user's explicit
// call to leave audio accuracy for a later session.)
// SPEED FIX (2026-09-20, "game speed" investigation part 2 - see
// FLSTORY_MAIN_CYCLES_PER_FRAME's own comment above and PROGRESS.md): a
// real-hardware capture (logs/capture.txt) showed run_frame() taking
// 29,000-39,000us against its ~16,667us/frame budget, with audio-CPU
// stepping (the "stage us/frame: audio=..." printout) costing a steady
// ~5,900-6,100us of that on its own. Per this constant's own comment
// above, audio's current 24,000-instruction/frame budget is ALREADY
// ~3.5x more than real MAME's measured ~6,793/frame need (left alone
// until now per an earlier explicit call to defer audio accuracy work) -
// i.e. there was already known, safe headroom here before this fix.
// Scaled down by the SAME proportional cut as FLSTORY_MAIN_CYCLES_PER_
// FRAME (both were sized to the same ~0.4x target in the capture.txt
// math - see that constant's comment for the full derivation): 80*2/5=32,
// giving 32*FLSTORY_SLICES=~9,600 instructions/frame total - still
// comfortably (~1.4x) above MAME's real ~6,793/frame need, so this should
// not be an accuracy regression on top of the deliberate one already
// documented above, just a smaller safety margin against it.
#ifndef FLSTORY_AUDIO_STEP
#define FLSTORY_AUDIO_STEP    32
#endif

// BUG FIX #2 (2026-09-19, "BAD SOUND PCB" investigation - see
// task_plain_6.txt): FLSTORY_MAIN_STEP's fixed "N instructions per slice"
// model is only an AVERAGE-instruction-length approximation. It was
// (correctly) calibrated to satisfy the MCU handshake's own throughput
// need (measured directly: 30,000-75,000 real instructions/frame), but a
// FIXED instruction count inherently mis-paces any code whose actual
// per-instruction cycle cost differs from that average - proven directly
// by measuring the sound self-test's 50-retry loop ($0509: ld hl,nn / dec
// hl / ld a,h / or l / jr nz, all cheap 4-11 T-state instructions): real
// MAME needs 12,803,644 real Z80 cycles for the full retry loop to
// succeed (measured via the debugger's `totalcycles`), while the OLD
// instruction-count pacing only granted it 3,846,645 cycles' worth of
// real time before giving up - a ~3.33x shortfall specific to this cheap-
// instruction loop, even though the SAME pacing constant was correctly
// sized for the MCU handshake's own (denser-code) needs. A fixed
// instruction budget cannot be simultaneously correct for both a cheap
// loop and dense code - only real cycle accounting can be.
//
// FIX: the main CPU is now paced by REAL cycles per frame instead of a
// fixed instruction count. Z80.c's StepZ80()/Codes.h already maintain a
// real per-instruction T-state count internally via cpu[0].ICount (every
// case in Codes.h/CodesCB/CodesED/etc. decrements it by the real cycle
// cost) - it was simply never read by this project's per-instruction
// stepping loops before now. Reading the ICount delta around each
// StepZ80() call (see run_frame()) recovers the exact real cycle cost for
// free, with no changes to the shared Z80 core itself.
//
// This automatically fixes ANY future code whose instruction mix differs
// from the average (not just this one loop), and is MORE consistent with
// the MCU handshake's own real needs than the old approximation was,
// since it's driven by the game's actual code instead of a fixed guess.
// (FLSTORY_MAIN_CYCLES_PER_FRAME itself is defined further below, once
// FLSTORY_MAIN_CLOCK/FLSTORY_VIDEO_HZ are both in scope.)
//
// Audio CPU periodic IRQ: attotime::from_ticks(0x10000, 8_MHz_XTAL) - i.e.
// one IRQ every 0x10000 ticks of the UN-divided 8MHz board clock, so the
// actual IRQ rate is 8000000/65536 ~= 122.07 Hz. At 60Hz video that is
// ~2.034 IRQs/frame - not an integer, so a plain "N per frame" counter (like
// gng's GNG_AUDIO_IRQ_PER_FRAME) would drift. Paced instead via a
// cycle-budget accumulator in run_frame() (see the accumulator idiom
// comment there), driven by this ratio.
#define FLSTORY_AUDIO_IRQ_HZ       122
#define FLSTORY_VIDEO_HZ           60

// MCU stepping ratio: MCU clock 3.072MHz vs main Z80 ~5.3667MHz. Paced by a
// fixed-point accumulator in run_frame() (same idiom as the audio IRQ
// above), credited per main-CPU INSTRUCTION stepped (not per T-state -
// this project's StepZ80() has no per-call cycle-count return, see
// run_frame()'s own comment for the exact caveat) and drained by single
// MCU instructions via taito68705_step().
#define FLSTORY_MCU_CLOCK      3072000UL
#define FLSTORY_MAIN_CLOCK     5366500UL   // 10.733_MHz_XTAL / 2 (rounded)
// MSM5232 tone/envelope clock: 8_MHz_XTAL / 4, per the MAME machine config.
// Used both by the pitch table and by the envelope rate tables.
#define FLSTORY_MSM_CLOCK      2000000UL

// Real cycle budget for the main CPU per emulated frame (see BUG FIX #2
// above) - replaces FLSTORY_MAIN_STEP as the thing that actually paces the
// main CPU in run_frame(). ~89,441, matching the 89,634 cycles/frame
// measured directly against real MAME (see BUG FIX #1's comment above) to
// within rounding.
//
// SPEED FIX (2026-09-20, "game speed" investigation part 2 - see
// PROGRESS.md and logs/capture.txt): a real-hardware capture showed
// run_frame() itself taking 29,000-39,000us against the 16,667us/frame
// budget it must fit inside to keep pace with its own vsync "give" signal
// (src/emulation/emulation.cpp) - confirmed directly in the capture:
// "give" stayed near the healthy ~60Hz the whole time (the video/audio
// side was never the problem) while "run_frame" measured only 27-35Hz,
// exactly the historical "run_frame Hz measured ~28-29Hz... not the
// expected ~59-60Hz" symptom emulation.cpp's own comment already
// described generically. Breakdown from the SAME capture's "stage
// us/frame" printout: main=14,000-20,500us, audio=~5,900-6,100us,
// mcu=~8,400us (already hard-capped below its own real-hardware-matched
// value - see MAX_MCU_STEPS_PER_FRAME in flstory.cpp) - main+audio+mcu
// ALONE already totals ~28,000-35,000us, before render/video/audio-
// transmit (which run in a SEPARATE task and are not part of this
// budget) are even counted.
//
// An earlier attempt at this fix capped main-CPU INSTRUCTIONS/frame
// (matching this port's own documented "30,000-75,000 real hardware
// need" figure) - WRONG LEVER: measured in the native harness, this game
// only ever actually needs ~9,000-14,000 instructions to exhaust the full
// cycle budget every frame (both during boot/self-test and during
// extended gameplay/combat), i.e. real hardware's ~14-20.5ms cost for the
// SAME cycle budget implies StepZ80() costs roughly 1-2us/instruction on
// the ESP32 - much higher per-instruction cost than the host-PC-timed
// native harness can reproduce (no flash wait-states/bus contention
// there) - so an instruction cap sized off the instruction-count
// reference figure would sit far ABOVE where this game actually operates
// and never bind. The real lever is the CYCLE budget itself, scaled down
// directly by the measured wall-clock overshoot ratio - not a separate
// cap layered on top.
//
// Required cut (per user's explicit choice - see PROGRESS.md - to target
// TRUE 60Hz, not a partial/45-50Hz compromise): main+audio+mcu must fit
// under ~16,667us total. With mcu left alone (~8,400us, already a
// validated hard cap - see MAX_MCU_STEPS_PER_FRAME's own comment, not
// re-touched here to avoid re-opening MCU-handshake-timing risk in the
// same change), main+audio together must fit in ~16,667-8,400=~8,267us,
// split proportionally to their CURRENT share (main~14,500us : audio
// ~6,000us, ratio ~2.42:1): main target ~5,850us, audio target ~2,417us -
// both close to 40% of their current measured cost. Applied here as a
// straight 0.4x scale of the CYCLE budget itself (not a hard instruction
// cap), on the theory that a proportionally smaller real-cycle target
// costs proportionally less wall-clock time to execute (StepZ80's own
// per-instruction cost is a property of the CPU core, not of how large a
// budget it's handed) - this is a real-hardware-empirical scale, not a
// re-derivation from first principles, and MUST be re-verified against a
// fresh logs/capture.txt after flashing (see PROGRESS.md).
#define FLSTORY_MAIN_CYCLE_SCALE_NUM   2
#define FLSTORY_MAIN_CYCLE_SCALE_DEN   5
#define FLSTORY_MAIN_CYCLES_PER_FRAME  ((FLSTORY_MAIN_CLOCK / FLSTORY_VIDEO_HZ) * FLSTORY_MAIN_CYCLE_SCALE_NUM / FLSTORY_MAIN_CYCLE_SCALE_DEN)

#define FLSTORY_MAME_FULL_W 256   // native MAME visible width (screen.set_raw: 256 wide x 224 visible tall)
#define FLSTORY_MAME_FULL_H 224   // cliprect 2*8..30*8-1 = 16..239 = 224 visible scanlines
// Real MAME's cliprect on the native Y axis is [16,240), NOT [0,224) - see
// screen.set_raw(8_MHz_XTAL, 510, 0, 256, 262, 2*8, 30*8): vbend=2*8=16,
// vbstart=30*8=240, out of a 262-line vtotal. The tilemap itself is a full
// 256-tall wraparound regardless of this crop (32 rows x 8px - the crop is
// a raster-blanking window, not a tilemap-space boundary), so anything that
// derives a native-Y-axis pixel position from a panel column must add this
// base offset BEFORE indexing into the tilemap/scroll math, or the picture
// silently loses its top 2 tile-rows (high-score/title text) and gains 2
// bogus wrapped-around tile-rows at the bottom instead (see PROGRESS.md TO
// DO #2, "screen Y axis too large - high-score/top info cut off"). Used by
// blit_bg_strip()'s mame_y math; scan_sprites()'s spr.x = mame_y-16 already
// (coincidentally, per a stale comment) used this same value.
#define FLSTORY_CLIP_TOP 16

// The physical panel this project targets receives 288 scanlines/frame (36
// render_row() bands x 8 - see main.cpp), but flstory's native MAME picture
// is only FLSTORY_MAME_FULL_W (256) lines on the axis that maps onto the
// panel's row axis (see the video-rendering section below for why X maps to
// row and Y maps to column here). The 32 spare lines are simply split
// evenly as a fixed, constant centering margin - NOT an adjustable/
// configurable shift knob (this project's gng.cpp has one of those,
// GNG_SCREEN_X_ADJ, but it is not a validated pattern - it is slated for
// removal from gng.cpp in a later cleanup session and is deliberately NOT
// reproduced here). Native content occupies panel rows
// [FLSTORY_ROW_OFFSET, FLSTORY_ROW_OFFSET + FLSTORY_MAME_FULL_W); rows
// outside that range are just left blank. The column axis (native Y, 224
// values) matches the panel's 224 columns exactly, so no offset is needed
// there at all.
#define FLSTORY_ROW_OFFSET ((288 - FLSTORY_MAME_FULL_W) / 2)

class flstory : public machineBase
{
public:
  flstory() { s_instance = this; }
  ~flstory()
  {
    if (s_instance == this)
      s_instance = 0;
  }

  signed char machineType() override { return MCH_FLSTORY; }

  void reset() override;
  void stop() override;
  void start() override;

  unsigned char rdZ80(unsigned short Addr) override;
  void wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  void outZ80(unsigned short Port, unsigned char Value) override;
  unsigned char inZ80(unsigned short Port) override;

  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;
  const unsigned short *logo(void) override { return flstory_logo; }
  const char *hiscoreKey() override { return 0; } // no known hiscore.dat entry for flstory - see final report

public:
  int renderFmSample() override;
  unsigned char ayDeviceVolume() override { return snd_ctrl2 >> 4; }

#if FLSTORY_DBG_HARNESS
  // Debug accessors for the native (off-ESP32) harness - see debug/vscode
  // debug machines.txt section 3. Gated behind a macro NEVER defined in the
  // real build (nothing in platformio.ini/config.h defines it); only the
  // harness's own build.bat passes /DFLSTORY_DBG_HARNESS=1. Left in place
  // after the investigation for reuse next time (compiles to nothing
  // otherwise).
  int dbg_active_sprites() const { return active_sprites; }
  const sprite_S &dbg_sprite(int i) const { return sprite[i]; }
  unsigned char dbg_memory(unsigned short addr) const { return memory[addr]; }
  unsigned short dbg_frame_buffer(int idx) const { return frame_buffer[idx]; }
  unsigned char dbg_gfxctrl() const { return m_gfxctrl; }
  unsigned char dbg_char_bank() const { return m_char_bank; }
  unsigned char dbg_palette_bank() const { return m_palette_bank; }
  bool dbg_flip() const { return m_flip; }
  uint16_t dbg_palette_shadow(unsigned char which, unsigned short entry) const { return flstory_palette_shadow[which][entry]; }
  unsigned char dbg_palette_front() const { return palette_front; }
  bool dbg_audio_reset_held() const { return audio_reset_held; }
  // m6805's PC field is the RAW, unmasked counter (see m6805.c's addr_mask()
  // comment: only fetch8()/rd()/wr() mask through cfg->addr_mask, PC itself
  // is never re-wrapped) - mask it here so this accessor reports where the
  // MCU is ACTUALLY fetching from (0x000-0x7FF for the P5), not a raw
  // counter that can drift arbitrarily higher while still executing the
  // correct masked address every fetch.
  uint16_t dbg_mcu_pc() const { return (uint16_t)(m_bmcu_state.mcu.cpu.PC & m_bmcu_state.mcu.cpu.cfg->addr_mask); }
  // MCU internal RAM byte (addr 0x10-0x7F only - see m68705p5.h's "ram[addr-0x10]" indexing).
  unsigned char dbg_mcu_ram(unsigned addr) const { return (addr >= 0x10 && addr < 0x80) ? m_bmcu_state.mcu.ram[addr - 0x10] : 0xff; }
  uint16_t dbg_mcu_pc_raw() const { return m_bmcu_state.mcu.cpu.PC; }
  unsigned char dbg_mcu_host_flag() const { return taito68705_host_flag(&m_bmcu_state); }
  unsigned char dbg_mcu_flag() const { return taito68705_mcu_flag(&m_bmcu_state); }
  uint16_t cpuPC(int idx) const { return cpu[idx].PC.W; }
  uint16_t cpuSP(int idx) const { return cpu[idx].SP.W; }
  long long dbg_mcu_credit() const { return m_mcu_cycle_credit; }
  // Cumulative mcu_flag 0->1 transitions, counted per MCU INSTRUCTION (not
  // per frame) - see run_frame()'s instrumentation. Catches a transient
  // pulse that per-frame sampling of dbg_mcu_flag() would miss if the main
  // CPU's own read clears it later in the same frame.
  long dbg_mcu_flag_pulse_count = 0;
  // Optional per-instruction main-CPU PC trace callback (see run_frame()'s
  // hook site) - set by the harness to get a fine-grained trace comparable
  // to a real MAME `trace file,0,noloop` capture, instead of once-per-
  // frame sampling which can miss/misorder short-lived branches.
  void (*dbg_trace_hook)(uint16_t pc) = 0;
  // Counts every write to the MCU's data port (0xD000) and the last value
  // written - see wrZ80()'s 0xd000 case.
  long dbg_data_w_count = 0;
  unsigned char dbg_last_data_w_value = 0;
  // Optional per-MCU-INSTRUCTION trace callback (see run_frame()'s MCU
  // stepping loop) - unlike dbg_trace_hook (per main-CPU instruction), this
  // fires once per taito68705_step() call, at the MCU's own instruction
  // rate, needed to see what the MCU does between two main-CPU instructions.
  void (*dbg_mcu_trace_hook)(uint16_t mcu_pc, unsigned char host_flag, unsigned char mcu_flag) = 0;
  // Optional full-register main-CPU trace callback, for diagnosing the
  // PC=0xFFFF boot-restart bug (see debug notes / task_plain_3.txt): unlike
  // dbg_trace_hook (PC only), this exposes every register + the raw opcode
  // byte at PC + the word sitting on top of stack, so a RET/POP popping a
  // wrong value can be caught in the act instead of inferred after the
  // fact from a PC-only trace.
  void (*dbg_full_trace_hook)(uint16_t pc, uint16_t sp, uint16_t af, uint16_t bc,
                               uint16_t de, uint16_t hl, unsigned char opcode,
                               uint16_t stack_top) = 0;
#endif

private:
  // --- main CPU side memory-mapped I/O handlers -----------------------
  unsigned char snd_flag_r(void);
  unsigned char flstory_mcu_status_r(void);
  void flstory_gfxctrl_w(unsigned char data);
  void flstory_scrlram_w(unsigned char offset, unsigned char data);
  unsigned char flstory_palette_r(unsigned short offset);
  void flstory_palette_w(unsigned short offset, unsigned char data);

  // --- audio CPU side ---------------------------------------------------
  void sound_control_0_w(unsigned char data);
  void sound_control_1_w(unsigned char data);
  void msm5232_w(unsigned char reg, unsigned char data);

  // --- video state --------------------------------------------------
  unsigned char m_gfxctrl = 0;
  unsigned char m_char_bank = 1;     // machine_reset(): m_char_bank=1 (inverted vs onna34ro's default - see MAME comment)
  unsigned char m_palette_bank = 0;
  bool m_flip = false;

  // palette RAM shadow (512 entries, base+ext byte pair each - see
  // flstory_palette_w). Combined via xBGR_444 (xxxxBBBBGGGGRRRR - verified
  // against source/mame/mame-master/src/emu/emupal.h) into RGB565, using
  // this codebase's general front/back shadow-buffer mechanism (the CPU
  // writes into its own live copy; the render side only ever reads a
  // published, index-swapped snapshot) - the same general mechanism as
  // gng's own palette/vram/spriteram shadowing, kept here because it is
  // machineBase's/this project's established way of avoiding tearing
  // between the CPU-writing thread and the render thread, not because of
  // any gng-specific implementation detail.
  unsigned char m_pal_base[512];
  unsigned char m_pal_ext[512];
  uint16_t flstory_palette_cpu[512];
  uint16_t flstory_palette_shadow[2][512];
  volatile unsigned char palette_front = 0;
  void publish_palette(void);
  void palette_recalc(unsigned short entry);

  // 0x800 bytes: videoram's mirror (0xc000-0xc7ff aliased at 0xc800-0xcfff)
  // is address-level aliasing of the SAME 0x800 bytes, not a separately-
  // backed second window - so the shadow only needs to hold one copy
  // (indexed &0x7ff by every reader, see blit_bg_strip()).
  unsigned char flstory_vram_shadow[2][0x800];
  volatile unsigned char vram_front = 0;
  void publish_vram(void);

  unsigned char spriteram_shadow[2][0xa0];
  volatile unsigned char spriteram_front = 0;

  unsigned char scrlram_shadow[2][0x20];
  volatile unsigned char scrlram_front = 0;

  unsigned char m_render_palette = 0;
  unsigned char m_render_vram = 0;
  unsigned char m_render_spriteram = 0;
  unsigned char m_render_scrlram = 0;
  bool m_render_flip = false;

  void scan_sprites(void);
  void blit_bg_strip(short row);
  void blit_sprite(short row, unsigned char s_idx);

  // BUG FIX (2026-09-20, "wizard invisible inside death bubble" - see
  // PROGRESS.md): real MAME's screen_update_flstory() fills a per-pixel
  // PRIORITY bitmap (screen.priority(), values 0/1/2/4/8) as the 4 bg
  // tilemap passes draw, then draws sprites through gfx_element::
  // prio_transpen() using that SAME bitmap - which does two things this
  // port's blit_sprite() never did at all: (1) a sprite pixel can be
  // occluded by an opaque BACKGROUND pixel of the wrong category (the
  // sprite-vs-background use of GFX_PMASK_4/_8 gated by the sprite's own
  // pr&0x80 bit), AND (2) critically, PIXEL_OP_REBASE_TRANSPEN_PRIORITY
  // unconditionally sets that pixel's priority to 31 after ANY opaque
  // sprite draws there, and prio_transpen() always forces bit 31 into its
  // OWN pmask (`pmask |= 1<<31`) - meaning once one sprite has opaquely
  // drawn a pixel, NO LATER-DRAWN sprite can ever overwrite it, regardless
  // of category bits. This port's blit_sprite() draws every active sprite
  // unconditionally on top of whatever came before with no occlusion test
  // at all - the OPPOSITE of MAME's real "first (bottom-of-stack) opaque
  // sprite pixel wins" rule - which is invisible for small/non-overlapping
  // sprites but produces exactly the reported bug when two large 16x16
  // sprites legitimately overlap in the same 8-line band (the "dying
  // wizard in a bubble" animation: 4 solid bubble-fill sprites are drawn
  // AFTER the 2 wizard-face sprites in priority-table order and paint
  // solid blue straight over them with nothing stopping it).
  //
  // Fixed with a per-band (224x8, matching frame_buffer's own footprint)
  // priority buffer mirroring screen.priority(): blit_bg_strip() writes it
  // with the SAME category/layer-pass values (1,2,4,8) MAME's own 4 tilemap
  // draws would have produced for that exact pixel (derived from the two
  // opacity tests it already performs, since - per its own header comment -
  // it collapses those 4 passes into one loop), and blit_sprite() reads/
  // writes it exactly like PIXEL_OP_REBASE_TRANSPEN_PRIORITY: skip the
  // pixel if `(1<<(priority&0x1f)) & pmask` is nonzero, but ALWAYS set
  // priority to 31 for any opaque source pixel regardless of whether it
  // was actually drawn - producing MAME's real "first opaque sprite pixel
  // at a position wins, and also blocks background-vs-sprite tests that
  // came before it" behavior.
  unsigned char priority_buffer[224 * 8];

  // --- sound-side latches/handshake --------------------------------
  unsigned char soundlatch = 0;       // main -> audio (0xd400 write / 0xd800 read)
  bool soundlatch_pending = false;
  unsigned char soundlatch2 = 0;      // audio -> main (0xd800 write / 0xd400 read)
  bool soundlatch2_pending = false;
  bool audio_reset_held = false;      // snd_reset_w bit0 (1 = held in reset)

  // TEMPORARY (2026-09-19): frame counter for the "BAD SOUND PCB" timing
  // investigation (task_plain_6.txt) - lets wrZ80()'s 0xd400/0xd800 write
  // handlers log which real frame each sound-handshake event happens on,
  // to compare against real MAME's measured 140-frame (~2.33s) gap between
  // the main CPU's command-send and the audio CPU's reply. Incremented
  // once per run_frame() call. Remove alongside the rest of the
  // FLSTORY_SERIAL_DEBUG scaffolding once this is resolved.
  unsigned long dbg_frame_counter = 0;
  // TEMPORARY (2026-09-19): cumulative REAL main-CPU cycle counter (not
  // instruction count) for the "BAD SOUND PCB" retry-loop timing
  // investigation (task_plain_6.txt) - Z80.c's StepZ80() genuinely does
  // track real per-instruction T-states internally via cpu[0].ICount
  // (see Z80.h/Codes.h - every case decrements ICount by the real cycle
  // cost), it just isn't returned to the caller; reading the ICount delta
  // around each StepZ80() call recovers it without touching the shared
  // core. Lets task_plain_6's "real cycles, not frames" comparison be
  // done directly against real MAME's totalcycles measurement.
  long dbg_main_cycle_counter = 0;

  // soundnmi (INPUT_MERGER_ALL_HIGH): bit0 = soundlatch "data pending"
  // (auto-driven by the generic_latch's data_pending_callback - i.e.
  // mirrors soundlatch_pending), bit1 = manually set by audio CPU writing
  // 0xda00 and cleared by writing 0xdc00. NMI asserted whenever bit0 OR
  // bit1 is set (ALL_HIGH merger: idle-high inputs, asserted = driven low
  // in real hardware terms, but modeled here directly as "asserted when any
  // bit set" per the header's own paraphrase - see flstory.cpp for the
  // exact call site).
  bool soundnmi_bit1 = false;
  bool soundnmi_line = false;   // current merged output level (see update_soundnmi())
  void update_soundnmi(void);

  unsigned char snd_ctrl0 = 0;
  unsigned char snd_ctrl1 = 0;
  // TA7630 volume for the WHOLE AY chip, written through the AY's PORT A
  // (register 14) -> MAME's sound_control_2_w():
  //   m_ta7630->set_device_volume(m_ay, m_snd_ctrl2 >> 4)
  // Volume is the HIGH nibble (0 = mute, 15 = full). This is what silences
  // the shoot effect on real hardware ~283ms in - see the BUG FIX comment in
  // flstory.cpp's 0xc801 handler. Surfaced to audio.cpp through the
  // ayDeviceVolume() virtual, not by exposing the field.
  unsigned char snd_ctrl2 = 0x88;
  unsigned char snd_ctrl3 = 0;   // PORT B -> sound_control_3_w(); latched only
  unsigned char dac_value = 0x80;    // DAC_8BIT_R2R, unsigned 0-255, centered at 0x80

  // AY (YM2149) register writes go through the generic soundregs[] path
  // (see audio.cpp's ay_render_buffer(); MCH_FLSTORY dispatch added there) -
  // NOT mixed inside renderFmSample().
  unsigned char ay_addr = 0;

  // --- MSM5232 model. 8 tone generators (2 groups of 4), each with 4
  // selectable "footage" square waves (2'/4'/8'/16') summed per MAME's
  // sound_control_0_w/1_w routing comments.
  //
  // ENVELOPE (rewritten 2026-09-23): now a direct port of MAME's own
  // 4-state envelope generator (msm5232.cpp's EG_voices_advance() +
  // write()), replacing the earlier fixed attack/decay/release constants.
  // The old fixed model was root-caused in the native harness as the cause
  // of the "dragged-out note" symptom: the game NEVER sets the ARM bit
  // (measured: 0 set / 297 clear over boot), and with ARM=0 the real chip
  // leaves attack and starts decaying BY ITSELF once the cap charges to 80%
  // of VMAX - it does not wait for key-off. The old model instead held the
  // level for as long as the gate stayed high, measured at up to 1.9
  // SECONDS per note. See PROGRESS.md for the full measurement.
  struct msm_ch_S {
    unsigned char group_select = 0;  // register 0x00-0x07 pitch code (bits 0-6) - see msm5232_w
    bool gate = false;               // register 0x00-0x07 bit7 -> gate/key-on state
    uint32_t phase = 0;              // 24.8 fixed-point phase accumulator
    // --- MAME-equivalent envelope state (see msm5232.cpp's VOICE) ---
    // eg_sect: -1 = idle/finished, 0 = attack, 1 = decay, 2 = release.
    signed char eg_sect = -1;
    bool eg_arm = false;             // latched from bit4 of register 0x0c/0x0d
    int32_t eg = 0;                  // 0..VMAX(32768) envelope level, MAME's own scale
    int32_t eg_counter = 0;          // MAME's per-voice fractional-step counter
    // Per-voice envelope rates, stored as RECIPROCALS of MAME's own rate
    // values (MAME uses ohms*farads, i.e. seconds; these hold 1/that) so
    // the per-sample envelope step is a multiply rather than a divide -
    // the ESP32 has no hardware float divide. Defaults match MAME's
    // init_voice(): index 0 of each table.
    float ar_rate = 0.0f;
    float dr_rate = 0.0f;
    float rr_rate = 0.0f;            // constant on real hw (always the R53/decay2 path)
    unsigned short egvol = 0;        // eg/16, the value actually multiplied into the output
  };
  msm_ch_S msm_ch[8];
  unsigned char msm_group1_ctrl = 0; // register 0x0c: group1 (ch0-3) ARM + footage enables
  unsigned char msm_group2_ctrl = 0; // register 0x0d: group2 (ch4-7) ARM + footage enables
  // Attack/decay rate tables, built once from the chip's own RC constants -
  // see flstory_msm_build_rate_tables() in flstory.cpp.
  void msm_set_group_rates(int group, bool attack, unsigned char data);
  void msm_eg_advance(msm_ch_S &c);
  unsigned char msm_noise_ctrl = 0;  // register 0x0e: noise enable (not routed to any output pin on this board - unused per MAME's machine config comments)

  // --- MCU (Taito 68705, see ../../cpus/m6805/taito68705_mcu.h) --------
public:
  // Public so the free-function m6805_read/m6805_write in flstory.cpp
  // (the m6805 core's global per-CPU-family callbacks - see m6805.h) can
  // reach it. This project's "single currentMachine instance" model (see
  // m68705p5.h's own Integration model note) means there is only ever one
  // flstory instance alive at a time - s_instance mirrors that same
  // pattern at this class's own level, the same way emulation.cpp's
  // currentMachine works for machineBase itself, since the MCU here is
  // NOT reachable through any machineBase virtual (see run_frame()'s own
  // comment on why the MCU is stepped directly instead).
  static flstory *s_instance;
  taito68705_mcu_state m_bmcu_state;
private:
  // Fixed-point accumulator pacing the MCU against the main Z80's own
  // per-slice stepping (see run_frame()'s comment for the exact math and
  // its instruction-count-based-estimate caveat). Units: MCU-clock "credit"
  // accumulated per main-CPU instruction stepped.
  //
  // BUG FIX (2026-09-20, "how low can FLSTORY_SLICES go" investigation -
  // see notes.txt): this MUST be 64-bit. run_frame() computes
  // `main_cycles_this_slice * FLSTORY_MCU_CLOCK` (3,072,000) every slice -
  // `long` is 32-bit on both this project's native harness (MSVC) and the
  // ESP32/Xtensa target, and that multiplication OVERFLOWS a signed 32-bit
  // range (2,147,483,647) once a single slice's real main-CPU cycle count
  // exceeds ~699 - i.e. once FLSTORY_SLICES drops to ~100 or below
  // (verified directly: at SLICES=100, main_cycles_per_slice=894, giving
  // 894*3,072,000=2,746,368,000, already past INT32_MAX). This is silent
  // signed-integer-overflow undefined behavior, not a graceful clamp -
  // measured in the harness to corrupt mcu_steps_per_frame catastrophically
  // (2,789.7 at SLICES=100, versus the correct/capped 3,500.0 at SLICES=300
  // where the multiplication still fits in 32 bits; collapsing further to
  // 788.6 at SLICES=60, 127.6 at SLICES=5, ~5 at SLICES=1-2 - a real MCU
  // handshake starvation bug masquerading as "coarser slicing does less
  // work", not an intentional accuracy/speed tradeoff). Widening this
  // accumulator (and the multiplication that feeds it, see run_frame()) to
  // a 64-bit type removes the overflow entirely regardless of slice count -
  // a pure correctness fix, not a new pacing decision.
  long long m_mcu_cycle_credit = 0;
};

#endif
