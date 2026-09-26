#ifndef BOBLBOBL_H
#define BOBLBOBL_H

#include "boblbobl_maincpu.h"
#include "boblbobl_subcpu.h"
#include "boblbobl_audiocpu.h"
#include "boblbobl_gfx.h"
#include "boblbobl_proms.h"
#include "boblbobl_dipswitches.h"
#include "boblbobl_logo.h"
#include "boblbobl_fm.h"
#include "../machineBase.h"
#include <atomic>

#define BOBLBOBL_VIDEORAM 0x0000
#define BOBLBOBL_OBJECTRAM 0x1d00
#define BOBLBOBL_SHARERAM 0x2000
#define BOBLBOBL_PALETTERAM 0x3800
#define BOBLBOBL_IC43RAM 0x3a00
#define BOBLBOBL_EXTRARAM 0x3b00
#define BOBLBOBL_AUDIO_RAM 0x3c00
#define BOBLBOBL_MEM_END 0x4c00

// CPU interleave per frame. MAME runs this board with
// config.set_maximum_quantum(attotime::from_hz(6000)) = 101 slices per frame.
// 50 gives identical sound-latch commands and YM register writes to MAME's
// log in the harness (ymlog + ymdiff3.py); 25 starts to shift them by a frame.
#ifndef BOBLBOBL_SLICES
#define BOBLBOBL_SLICES 50
#endif

#define BOBLBOBL_MAIN_CLOCK 6000000UL
#define BOBLBOBL_VIDEO_HZ_NUM 6000000UL
#define BOBLBOBL_VIDEO_HZ_DEN (384UL * 264UL)

#define BOBLBOBL_MAIN_CYCLES_PER_FRAME \
  (BOBLBOBL_VIDEO_HZ_DEN)

#define BOBLBOBL_MAME_FULL_W 256
#define BOBLBOBL_MAME_FULL_H 224

// On-device stage timing (CPU cycle counter), printed once a second from the
// emulation task: us per frame for each CPU and for video/audio rendering.
// Set to 0 once the port runs at speed.
// Sound render cost on the device. 0 = bit-exact MAME/ymfm output (proven in
// the harness). 1 = same chip state stepping at the native 41.7kHz rate, but
// the output mix is computed once per 24kHz sample instead of for every chip
// sample, and a silent SSG is not clocked. Pitch and envelope timing are
// unchanged; operator self-feedback is sampled less often. Harness tests run
// the exact path in both settings.
#ifndef BOBLBOBL_SND_FAST
#define BOBLBOBL_SND_FAST 1
#endif

// 0 = off (default since the speed work is done, 2026-09-26). 1 = the once a
// second "bb us/frame ..." line on the serial port; the device benchmark
// (GalaginoPlusS3 envs bench_*) sets it to 1 itself.
#ifndef BOBLBOBL_PROFILE
#define BOBLBOBL_PROFILE 0
#endif

// Sound render on the emulation core (boblbobl.cpp renderFmSample): after
// each frame core 1 renders the frame's samples ahead into a ring until the
// next frame tick is waiting, or at most this many us after the frame
// started; core 0 (video + audio) renders only what is left.
// 0 = everything on core 0 (the old behaviour).
#ifndef BOBLBOBL_SND_CORE1_US
#define BOBLBOBL_SND_CORE1_US 40000
#endif

// Run at the game's own 59.19Hz instead of one frame per display tick
// (~60Hz): run_frame only emulates when real time has caught up, so the
// emulation makes exactly the 24000 samples/s the audio core plays. 0 = one
// frame per display tick (1.4% fast; the audio timeline then has to skip).
#ifndef BOBLBOBL_REAL_SPEED
#define BOBLBOBL_REAL_SPEED 1
#endif

// When the emulation runs slower than real time the audio core runs out of
// emulated sound. 1 = repeat the last sample (cheap, but the sound cuts out:
// "repeated" in the profile line). 0 = keep stepping the chips, so notes
// ring on - every such sample costs a full render on the audio core (this
// locked gameplay at 26Hz with the old frame pacing, pass 7a).
#ifndef BOBLBOBL_SND_REPEAT
#define BOBLBOBL_SND_REPEAT 1
#endif

#ifndef BOBLBOBL_ROW_OFFSET
#define BOBLBOBL_ROW_OFFSET ((288 - BOBLBOBL_MAME_FULL_W) / 2)
#endif

class boblbobl : public machineBase
{
public:
  boblbobl() {}
  ~boblbobl();

  signed char machineType() override { return MCH_BOBLBOBL; }

  void reset() override;
  void stop() override;

  unsigned char rdZ80(unsigned short Addr) override;
  void wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  void outZ80(unsigned short Port, unsigned char Value) override;
  unsigned char inZ80(unsigned short Port) override;

  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;

  const char *hiscoreKey() override { return "boblbobl"; }
  const hiscore_region_S *hiscoreRegions(unsigned char *count) override;
  // High score RAM is the main CPU's (e000-f7ff): read/write through its map
  // directly - rdZ80/wrZ80 route by current_cpu, which may be sub or sound.
  unsigned char hiscoreRead(unsigned short addr) override { return main_rd(addr); }
  void hiscoreWrite(unsigned short addr, unsigned char value) override { main_wr(addr, value); }
#ifdef LED_PIN
  void menuLeds(CRGB *leds) override;
  void gameLeds(CRGB *leds) override;
  // Bub (green) and Bob (blue)
  const CRGB menu_leds[7] = { LED_GREEN, LED_BLUE, LED_GREEN, LED_CYAN, LED_GREEN, LED_BLUE, LED_GREEN };
#endif
const unsigned short *logo(void) override;

public:
  int renderFmSample() override;

private:
  // ---- sound ----------------------------------------------------------
  // Emulation side keeps its own copy of the chip registers: the timers,
  // status/IRQ and the SSG register read-back all run in emulation time.
  unsigned char ym_addr = 0;
  unsigned char ym_regs[256];
  unsigned char opl_addr = 0;
  unsigned char opl_regs[256];
  void ym_write(unsigned char reg, unsigned char val);
  void opl_write(unsigned char reg, unsigned char val);

  // Chip models (boblbobl_fm.h), owned by the audio render path.
  bbfm::opn_t snd_opn;
  bbfm::ssg_t snd_ssg;
  bbfm::opl_t snd_opl;
  void snd_chips_reset(void);
  void snd_apply(unsigned char chip, unsigned char reg, unsigned char val);

  // Register writes travel from the emulation core to the audio core through
  // this single-producer/single-consumer ring, stamped with the chip clock
  // (3MHz, ym_chip_clocks) at which the sound CPU made them.
  struct snd_ev_S
  {
    uint32_t clk;
    uint8_t chip;   // 0 = YM2203, 1 = YM3526, 2 = reset both
    uint8_t reg;
    uint8_t val;
  };
  static const int SND_RING = 1024;
  snd_ev_S snd_ring[SND_RING];
  volatile uint16_t snd_head = 0;
  volatile uint16_t snd_tail = 0;
  volatile uint32_t snd_emu_clk_pub = 0;  // ym_chip_clocks at the end of the last frame
  void snd_push(unsigned char chip, unsigned char reg, unsigned char val);

  // Audio render timeline (chip clocks)
  uint32_t snd_evt_clk = 0;   // emulation chip-clock time of the current output sample
  bool snd_evt_started = false;
  uint8_t snd_ssg_cd = 16;    // clocks to the next SSG step (ssg_engine runs at clock/16)
  uint8_t snd_fm_cd = 72;     // clocks to the next FM sample (clock / (prescale 6 * 12))
  void snd_apply_due(uint32_t now);
  void snd_step_clocks(uint32_t clocks, int32_t &fm_sum, int32_t &opl_sum, int &fm_n, int32_t &ssg_sum, int &ssg_n);

  // Harness only: apply writes immediately and tick the chips from
  // ym_timers_advance(), so their output lines up with emulation time.
  bool snd_sync = false;

public:
  bool snd_repeat_when_behind = BOBLBOBL_SND_REPEAT;   // see BOBLBOBL_SND_REPEAT
private:

  // Output samples rendered ahead on core 1 (renderFmSample, snd_render_ahead);
  // snd_lock guards the chip state and the event clock above.
  static const int SND_OUT = 2048;
  short snd_out[SND_OUT];
  volatile uint16_t snd_out_wr = 0;
  volatile uint16_t snd_out_rd = 0;
  short snd_last = 0;              // last sample played (repeated when nothing emulated is ready)
  volatile bool snd_core1_busy = false;   // core 1 is in snd_render_ahead (it has priority)
  std::atomic<uint32_t> snd_lock{0};
  int snd_render_one(void);
  void snd_render_ahead(uint32_t t0);

  // BOBLBOBL_REAL_SPEED
  bool pace_init = false;
  uint32_t pace_last = 0;
  int32_t pace_credit = 0;

  unsigned char main_rd(unsigned short addr);
  void main_wr(unsigned short addr, unsigned char val);
  unsigned char sub_rd(unsigned short addr);
  void sub_wr(unsigned short addr, unsigned char val);
  unsigned char audio_rd(unsigned short addr);
  void audio_wr(unsigned short addr, unsigned char val);

  void bankswitch_w(unsigned char data);
  unsigned char m_bank = 0;
  const unsigned char *m_bankptr = boblbobl_maincpu + 0x10000;

  // The three fixed 32KB Z80 ROMs are copied to internal RAM in 8KB pieces
  // when the heap allows (flash reads on the emulation core stall behind the
  // video core's flash traffic); each piece falls back to flash on its own.
  // The CPUs reach them through the page tables (boblbobl_z80.c).
  unsigned char *rom_ram[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};   // audio, main, sub
  void roms_to_ram(void);
  void build_pages(void);
  void set_bank_pages(void);
  void set_cpu(int c);

  bool sub_held_reset = true;

  bool m_flip = false;
  bool m_video_enable = false;

  unsigned char coinBackup = 0;
  unsigned char coinFrameCounter = 0;

  int m_ic43_a = 0;
  int m_ic43_b = 0;
  unsigned char ic43_a_r(unsigned char offset);
  void ic43_a_w(unsigned char offset, unsigned char data);
  unsigned char ic43_b_r(unsigned char offset);
  void ic43_b_w(unsigned char offset, unsigned char data);

  // YM2203 / YM3526 timers, status and IRQ (ymfm fm_engine_base semantics).
  // Both chips are clocked at 3MHz, the same clock as the audio Z80, so timer
  // counters count audio-CPU cycles.
  struct ymtimer_S
  {
    int32_t remain;
    bool running;
  };
  ymtimer_S opn_timer[2];
  ymtimer_S opl_timer[2];
  unsigned char opn_status = 0;
  unsigned char opl_status = 0;
  unsigned char opl_mode = 0;
  void opn_update_timer(int tnum, bool enable, int32_t delta_samples);
  void opl_update_timer(int tnum, bool enable, int32_t delta_samples);
  uint32_t ym_chip_clocks = 0;
  void opn_mode_write(unsigned char data);
  void opl_mode_write(unsigned char data);
  void ym_timers_advance(int32_t clocks);
  bool sound_irq_line(void);
  bool opl_irq(void);

  // Sound CPU slice (run_frame): the YM timers are advanced lazily. Clocks of
  // executed instructions collect in ym_pending and are applied (ym_flush)
  // only before something can see them: a sound CPU access to the chips or
  // latches (audio_rd / audio_wr, 0x9000 up), a timer running out
  // (ym_next_expiry), the idle-loop check, the end of the slice. Applying
  // them later in one go gives the same state as after every instruction.
  // ym_irq_cached = sound_irq_line(), kept current at the same points.
  int32_t ym_pending = 0;
  int32_t ym_next_expiry = 0x7fffffff;   // clocks until the first running timer expires
  bool ym_irq_cached = false;
  bool ym_touched = false;               // audio_rd / audio_wr reached the chips or latches
  void ym_refresh(void);
  void ym_flush(void);
  unsigned char opl_read_status(void);
  void ym_chips_reset(void);

  // irq0_line_hold on main/sub: raised at vblank, dropped on acknowledge.
  bool main_irq_pending = false;
  long main_cycle_debt = 0, sub_cycle_debt = 0, audio_cycle_debt = 0;   // run_frame: cycles carried into the next slice
  bool sub_irq_pending = false;
  bool aud_prev_idle = false;          // sound CPU idle-loop skip (run_frame)
  uint32_t aud_prev_clk = 0;
  bool audio_idle_state(void);
  long audio_idle_passes(long budget);
  bool sound_nmi_line = false;
  void update_sound_nmi(void);
  void sound_sreset(bool assert_line);
  bool sreset_old = false;

  unsigned char main_to_sound = 0;
  bool main_to_sound_pending = false;
  unsigned char sound_to_main = 0;
  bool sound_to_main_pending = false;
  bool sound_nmi_enable = false;
  bool audio_held_reset = true;


  uint16_t pal_cpu[256];
  uint16_t pal_shadow[2][256];
  volatile unsigned char palette_front = 0;
  void palette_recalc(unsigned short entry);
  void publish_palette(void);

  // c000-dfff: videoram (0x0000-0x1cff) + objectram (0x1d00-0x1fff)
  unsigned char vram_shadow[2][0x2000];
  volatile unsigned char vram_front = 0;
  void publish_vram(void);

  unsigned char m_render_palette = 0;
  unsigned char m_render_vram = 0;
  bool m_render_flip = false;
  bool m_render_video_enable = false;

  // One visible, not fully transparent 8x8 cell. A cell that straddles two
  // 8-line bands is entered once per band; `next` links the entries of a band
  // in objectram (= MAME draw) order.
  struct span_S
  {
    short fb_row_top;
    short fb_col;
    unsigned short code;
    unsigned char color;     // bits 0-3 colour, bit 6 flip x, bit 7 flip y
    unsigned short next;
  };

  static const int MAX_SPANS = 2048;
  static const unsigned short SPAN_END = 0xffff;
  span_S spans[MAX_SPANS];
  int span_count = 0;
  unsigned short band_head[36];
  unsigned short band_tail[36];
  void scan_spans(void);
  void add_span(int band, short row_top, short col, unsigned short code, unsigned char color);
  void blit_span(int y_strip, const span_S &sp);

  const unsigned char *m_proms = boblbobl_proms;

#if BOBLBOBL_DBG_HARNESS
public:
  uint16_t cpuPC(int idx) const { return cpu[idx].PC.W; }
  uint16_t cpuSP(int idx) const { return cpu[idx].SP.W; }
  unsigned char dbg_memory(unsigned short addr) const { return memory[addr]; }
  uint16_t dbg_palette_shadow(unsigned char which, unsigned short entry) const { return pal_shadow[which][entry]; }
  unsigned char dbg_palette_front() const { return palette_front; }
  int dbg_span_count() const { return span_count; }
  const span_S &dbg_span(int i) const { return spans[i]; }
  static bool dbg_tile_empty(unsigned short code);
  bool dbg_sub_held_reset() const { return sub_held_reset; }
  bool dbg_audio_held_reset() const { return audio_held_reset; }
  bool dbg_video_enable() const { return m_video_enable; }
  bool dbg_flip() const { return m_flip; }
  unsigned char dbg_bank() const { return m_bank; }

  void (*dbg_trace_hook)(uint16_t pc) = 0;
  void (*dbg_snd_hook)(char kind, uint16_t addr, uint8_t data) = 0;
  // kind 0: FM tick (a = YM2203 FM, b = YM3526); kind 1: SSG tick (a, b, c = channels)
  void (*dbg_fm_hook)(int kind, int32_t a, int32_t b, int32_t c) = 0;
  void dbg_set_snd_sync(bool on) { snd_sync = on; }
  int dbg_snd_core1 = 0;   // 0 = off, 1 = snd_render_ahead renders everything it may, N >= 2 = at most N samples per frame
  unsigned long dbg_steps[3] = {0, 0, 0}, dbg_halt_steps[3] = {0, 0, 0};
  unsigned long dbg_pc_hist[3][65536];
  unsigned long dbg_loop_hist[4096];
  long dbg_snd_drops = 0, dbg_snd_applied = 0;
  int32_t dbg_snd_max_late = 0;
  uint32_t dbg_chip_clocks() const { return ym_chip_clocks; }

  // ESP32 timing simulation (harness "window" mode). Read-only views of the
  // sound path, and core 1's render-ahead run in small batches so the
  // simulation can interleave it with core 0 in time order.
  unsigned long dbg_snd_rendered = 0;   // samples made by snd_render_one (either core)
  int dbg_snd_out_count() const { return (snd_out_wr - snd_out_rd) & (SND_OUT - 1); }
  // operators that are not silent: the FM render cost grows with these
  int dbg_snd_live_ops() const
  {
    int n = 0;
    for (int k = 0; k < 12; k++)
      if (!(snd_opn.op[k].env_state == bbfm::EG_RELEASE && snd_opn.op[k].env_attenuation == 0x3ff)) n++;
    for (int k = 0; k < 18; k++)
      if (!(snd_opl.op[k].env_state == bbfm::EG_RELEASE && snd_opl.op[k].env_attenuation == 0x3ff)) n++;
    return n;
  }
  void dbg_set_core1_busy(bool on) { snd_core1_busy = on; }
  // snd_render_ahead limited to n (>= 2) samples; returns how many it made
  int dbg_render_ahead(int n)
  {
    int save = dbg_snd_core1, before = dbg_snd_out_count();
    dbg_snd_core1 = n < 2 ? 2 : n;
    snd_render_ahead(0);
    dbg_snd_core1 = save;
    return dbg_snd_out_count() - before;
  }

  // Load a MAME RAM dump of c000-f9ff (see debug/boblbobl_truth/bb_truth.lua)
  // so the renderer can be checked against a MAME snapshot of the same frame.
  void dbg_load_truth(const unsigned char *d, bool flip)
  {
    memcpy(&memory[BOBLBOBL_VIDEORAM], d, 0x1d00);
    memcpy(&memory[BOBLBOBL_OBJECTRAM], d + 0x1d00, 0x300);
    memcpy(&memory[BOBLBOBL_SHARERAM], d + 0x2000, 0x1800);
    memcpy(&memory[BOBLBOBL_PALETTERAM], d + 0x3800, 0x200);
    for (int e = 0; e < 256; e++)
      palette_recalc(e);
    publish_palette();
    publish_vram();
    m_video_enable = true;
    m_flip = flip;
  }
#endif
};

#endif
