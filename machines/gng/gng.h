#ifndef GNG_H
#define GNG_H

#include "gng_maincpu.h"
#include "gng_audiocpu.h"
#include "gng_chars.h"
#include "gng_tiles.h"
#include "gng_sprites.h"
#include "gng_dipswitches.h"
#include "gng_logo.h"
#include "../machineBase.h"
#include "../../cpus/m6809/m6809.h"

// See source/mame/mame-master/src/mame/capcom/gng.cpp

#define GNG_WORK_RAM      0x0000
#define GNG_SPRITE_RAM    0x1e00
#define GNG_FG_VRAM       0x2000
#define GNG_BG_VRAM       0x2800
#define GNG_SPRITE_BUF    0x3000
#define GNG_AUDIO_RAM     0x3200
#define GNG_MEM_END       0x3a00

#ifndef GNG_SLICES
#define GNG_SLICES        1500
#endif
#ifndef GNG_MAIN_STEP
#define GNG_MAIN_STEP     4
#endif
#ifndef GNG_AUDIO_STEP
#define GNG_AUDIO_STEP    4
#endif
#ifndef GNG_AUDIO_IRQ_PER_FRAME
#define GNG_AUDIO_IRQ_PER_FRAME 4
#endif

#define GNG_MAME_FULL_W 256

// Sprite column-axis (screen-vertical) correction when the game's own
#ifndef GNG_SPR_FLIP_ON_Y_ADJ
#define GNG_SPR_FLIP_ON_Y_ADJ 0 // floating air bug adjust to 0, 16 or -16 px depending on dip flip
#endif

// Whole-screen shift on the row axis (mame_x - see blit_bg_strip()/
// blit_fg_tile()/scan_sprites() in gng.cpp for the exact math). Content
// that was at mame_x now appears at mame_x + GNG_SCREEN_X_ADJ, applied
// identically to sprites and to bg/fg tiles so the layers stay in
// registration. This is a genuine shift, not a re-crop of a fixed window:
// a nonzero value necessarily crops GNG_SCREEN_X_ADJ px of real picture off
// the edge the content moves away from (there is no wrap - the picture is
// only 256px of native content, and the panel only ever showed 256px of
// output, so shifting always trades a strip of image on one edge for a
// strip of blank on the other). SIGN: not verified against a real cabinet -
// if the picture moves the wrong physical direction, flip the sign.
#ifndef GNG_SCREEN_X_ADJ
#define GNG_SCREEN_X_ADJ 16 // 0 moves example right
#endif

class gng : public machineBase
{
public:
  gng() { memset(&main_cpu, 0, sizeof(main_cpu)); }
  ~gng() { }

  signed char machineType() override { return MCH_GNG; }
  signed char useVideoHalfRate() override { return 1; }

  void reset() override;
  void stop() override;

  unsigned char m6809_read(m6809_state *s, uint16_t addr) override;
  void m6809_write(m6809_state *s, uint16_t addr, uint8_t val) override;
  unsigned char m6809_read_opcode(m6809_state *s, uint16_t addr) override;

  unsigned char rdZ80(unsigned short Addr) override;
  void wrZ80(unsigned short Addr, unsigned char Value) override;
  unsigned char opZ80(unsigned short Addr) override;
  void outZ80(unsigned short Port, unsigned char Value) override;
  unsigned char inZ80(unsigned short Port) override;

  void run_frame(void) override;
  void prepare_frame(void) override;
  void render_row(short row) override;
  const unsigned short *logo(void) override { return gng_logo; }
  const char *hiscoreKey() override { return "gng"; }
  const hiscore_region_S *hiscoreRegions(unsigned char *count) override;
  unsigned char hiscoreRead(unsigned short addr) override {
    return memory[GNG_WORK_RAM + addr];
  }
  void hiscoreWrite(unsigned short addr, unsigned char value) override {
    memory[GNG_WORK_RAM + addr] = value;
  }

#ifdef LED_PIN
  void menuLeds(CRGB *leds) override;
  void gameLeds(CRGB *leds) override;
#endif

#if GNG_DBG_TOMBSTONE
  bool dbg_m_flip() const { return m_flip; }
#endif

public:
  struct fm_op_S {
    uint32_t phase;
    uint32_t inc;
    int      env;
    uint32_t sl_units;
    uint8_t  stage;
    uint8_t  mul;
    uint8_t  tl;
    uint8_t  ar, dr, sr, rr, sl;
    uint8_t  ks;
  };

private:
  void palette_write(unsigned short offset, bool ext, unsigned char value);
  void scan_sprites(void);
  void blit_bg_strip(short row, char prio);
  void blit_fg_tile(short row, char col_mame_row);
  void blit_sprite(short row, unsigned char s_idx);

  uint16_t gng_palette_cpu[256];
  uint16_t gng_palette_shadow[2][256];
  volatile unsigned char palette_front = 0;
  void publish_palette(void);

  unsigned char gng_vram_shadow[2][0x1000];
  volatile unsigned char vram_front = 0;
  void publish_vram(void);

  unsigned char spriteram_shadow[2][0x200];
  volatile unsigned char spriteram_front = 0;

  unsigned char m_render_palette = 0;
  unsigned char m_render_vram = 0;
  unsigned char m_render_spriteram = 0;

  short m_bg_scrollx = 0;
  short m_bg_scrolly = 0;
  bool  m_flip = false;

  unsigned char m_bank = 0;
  const unsigned char *m_bankptr = gng_maincpu + 0x4000;

  unsigned char sound_latch = 0;
  bool audio_running = false;

  unsigned char ym_addr[2] = { 0, 0 };
  unsigned char ym_regs[2][256];

  struct fm_ch_S {
    fm_op_S op[4];
    uint16_t fnum;
    uint8_t  block;
    uint8_t  algo;
    uint8_t  fb;
    int16_t  feedback[2];
    bool     keyon;
    volatile int8_t pending_key[2];
    volatile uint8_t pending_head, pending_tail;
  };
  fm_ch_S fm[6];
  int fm_last = 0;

public:
  int renderFmSample() override;
private:
  void ym_write(int chip, unsigned char reg, unsigned char val);
  void ym_frame(void);
  void ym_key(int ch, bool on);
  void ym_recalc_inc(int ch);

  m6809_state main_cpu;
  void install_rom_direct(void);
  void mainlatch_w(unsigned char bit, unsigned char val);
  void set_bank(unsigned char data);

  unsigned char coinBackup = 0;
  unsigned char coinFrameCounter = 0;

#ifdef LED_PIN
  const CRGB menu_leds[7] = { LED_RED, LED_MAGENTA, LED_RED, LED_YELLOW,
                              LED_RED, LED_MAGENTA, LED_RED };
#endif
};

#endif
