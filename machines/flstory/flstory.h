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

#ifndef FLSTORY_SLICES
#define FLSTORY_SLICES        60
#endif
#ifndef FLSTORY_MAIN_STEP
#define FLSTORY_MAIN_STEP     11
#endif
#ifndef FLSTORY_AUDIO_STEP
#define FLSTORY_AUDIO_STEP    32
#endif

#define FLSTORY_AUDIO_IRQ_HZ       122
#define FLSTORY_VIDEO_HZ           60

#define FLSTORY_MCU_CLOCK      3072000UL
#define FLSTORY_MAIN_CLOCK     5366500UL   // 10.733_MHz_XTAL / 2 (rounded)
// MSM5232 tone/envelope clock: 8_MHz_XTAL / 4, per the MAME machine config.
// Used both by the pitch table and by the envelope rate tables.
#define FLSTORY_MSM_CLOCK      2000000UL

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
  const unsigned short *logo(void) override;

  #ifdef LED_PIN
    void menuLeds(CRGB *leds) override;
    void gameLeds(CRGB *leds) override;
#endif

public:
  int renderFmSample() override;
  unsigned char ayDeviceVolume() override { return snd_ctrl2 >> 4; }

  const char *hiscoreKey() override { return "flstory"; }
  const hiscore_region_S *hiscoreRegions(unsigned char *count) override;
  unsigned char hiscoreRead(unsigned short addr) override {
    return (addr >= 0xe000) ? memory[FLSTORY_WORKRAM + (addr & 0x7ff)] : 0xff;
  }
  void hiscoreWrite(unsigned short addr, unsigned char value) override {
    if (addr >= 0xe000) memory[FLSTORY_WORKRAM + (addr & 0x7ff)] = value;
  }

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

    unsigned char priority_buffer[224 * 8];

  // --- sound-side latches/handshake --------------------------------
  unsigned char soundlatch = 0;       // main -> audio (0xd400 write / 0xd800 read)
  bool soundlatch_pending = false;
  unsigned char soundlatch2 = 0;      // audio -> main (0xd800 write / 0xd400 read)
  bool soundlatch2_pending = false;
  bool audio_reset_held = false;      // snd_reset_w bit0 (1 = held in reset)

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
  // ENVELOPE: now a direct port of MAME's own
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
