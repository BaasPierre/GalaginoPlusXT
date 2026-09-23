// boblbobl sound chips: YM2203 (FM + SSG) and YM3526 (OPL), transcribed from
// MAME's ymfm (mame/mame-master/3rdparty/ymfm/src, BSD-3-Clause, Aaron Giles).
//
// This is a line-by-line transcription of the ymfm code paths that MAME runs
// for these two chips, reduced to plain C++11 structs (no STL, no virtuals, no
// heap) so it builds with the ESP32 Arduino toolchain:
//   ymfm_fm.ipp  fm_operator / fm_channel / fm_engine_base
//   ymfm_opn.cpp opn_registers_base<false> (YM2203), ym2203::clock_fm
//   ymfm_opl.cpp opl_registers_base<1> (YM3526), ym3526::generate
//   ymfm_ssg.cpp ssg_engine
//   ymfm.h       roundtrip_fp
// The function names below match the ymfm ones so each can be checked
// against its source. The native harness (boblbobl_debug_harness_external,
// "fmcmp" mode) runs this code and the real ymfm side by side on the same
// register writes and compares every sample.
//
// Timers, status and IRQ are NOT here: they run on the emulation side in
// boblbobl.cpp (same ymfm rules), because they drive the sound CPU.
//
// Not transcribed (unused by this hardware, checked against the game's
// register writes): YM2203 CSM key-on (0x27 mode 2), prescale 3 and 2
// (the sound program only selects 0x2d = prescale 6), SSG I/O ports.
#ifndef BOBLBOBL_FM_H
#define BOBLBOBL_FM_H

#include <stdint.h>

namespace bbfm
{

enum
{
  EG_DEPRESS = 0,
  EG_ATTACK = 1,
  EG_DECAY = 2,
  EG_SUSTAIN = 3,
  EG_RELEASE = 4,
  EG_STATES = 6
};
enum
{
  KEYON_NORMAL = 0,
  KEYON_RHYTHM = 1,
  KEYON_CSM = 2
};
static const uint32_t EG_QUIET = 0x380;

// fm_operator state + opdata_cache
struct op_t
{
  uint32_t phase;           // 10.10 phase
  uint16_t env_attenuation; // 10-bit attenuation (ymfm: uint16_t)
  uint8_t env_state;
  uint8_t ssg_inverted;
  uint8_t key_state;
  uint8_t keyon_live;
  uint16_t opoffs;
  // opdata_cache
  bool phase_dynamic;       // PHASE_STEP_DYNAMIC
  uint32_t phase_step;
  uint32_t block_freq;
  int32_t detune;
  uint32_t multiple;
  uint32_t total_level;
  uint32_t eg_sustain;
  uint8_t eg_rate[EG_STATES];
};

// fm_channel state
struct ch_t
{
  int16_t feedback[2];
  int16_t feedback_in;
  uint8_t choffs;
};

// YM2203 FM part: fm_engine_base<opn_registers_base<false>>
struct opn_t
{
  uint8_t regs[0x100];
  op_t op[12];
  ch_t ch[3];
  uint32_t env_counter;
  uint8_t total_clocks;
  uint32_t active_channels;
  uint32_t modified_channels;
  uint32_t prepare_count;
  int32_t last_out;          // ym2203 m_last_fm.data[0] after roundtrip_fp

  void construct();          // fm_engine_base constructor state
  void reset();              // fm_engine_base::reset + opn_registers reset
  void write(uint8_t reg, uint8_t data);
  void clock_fm(bool out = true);  // ym2203::clock_fm: clock + (out) output + roundtrip_fp
  void prepare_if_modified(uint32_t ticks);
  void step_fast(uint32_t n);      // BOBLBOBL_SND_FAST: n chip samples, output once
};

// YM3526 : fm_engine_base<opl_registers_base<1>>
struct opl_t
{
  uint8_t regs[0x100];
  op_t op[18];
  ch_t ch[9];
  uint32_t env_counter;
  uint8_t total_clocks;
  uint32_t active_channels;
  uint32_t modified_channels;
  uint32_t prepare_count;
  uint16_t lfo_am_counter;
  uint16_t lfo_pm_counter;
  uint32_t noise_lfsr;
  uint8_t lfo_am;
  int32_t last_out;          // ym3526::generate output after roundtrip_fp

  void construct();
  void reset();
  void write(uint8_t reg, uint8_t data);
  void generate(bool out = true);  // one ym3526::generate() sample (out: compute output)
  void prepare_if_modified(uint32_t ticks);
  void step_fast(uint32_t n);      // BOBLBOBL_SND_FAST: n chip samples, output once
};

// YM2203 SSG part: ssg_engine
struct ssg_t
{
  uint8_t regs[0x10];
  uint32_t tone_count[3];
  uint32_t tone_state[3];
  uint32_t envelope_count;
  uint32_t envelope_state;
  uint32_t noise_count;
  uint32_t noise_state;
  int16_t out[3];

  void construct();
  void reset();
  void write(uint8_t reg, uint8_t data);
  void clock();
  void output();
};

} // namespace bbfm

#endif
