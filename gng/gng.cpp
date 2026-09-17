#include "gng.h"

// Ghosts'n Goblins (Capcom, 1985). See source/mame/mame-master/src/mame/capcom/gng.cpp
// v2026-09-17 14

static_assert(GNG_MEM_END <= RAMSIZE, "RAMSIZE too low for gng");
static void ym_tables_init();

void gng::reset() {
  machineBase::reset();

  m_bg_scrollx = m_bg_scrolly = 0;
  m_flip = false;
  m_bank = 0;
  m_bankptr = gng_maincpu + 0x10000;
  sound_latch = 0;
  audio_running = false;
  ym_addr[0] = ym_addr[1] = 0;
  memset(ym_regs, 0, sizeof(ym_regs));
  memset(fm, 0, sizeof(fm));
  for (int ch = 0; ch < 6; ch++) {
    fm[ch].pending_key[0] = -1;
    fm[ch].pending_key[1] = -1;
  }
  fm_last = 0;
  memset(gng_palette_cpu, 0, sizeof(gng_palette_cpu));
  memset(gng_palette_shadow, 0, sizeof(gng_palette_shadow));
  palette_front = 0;
  memset(spriteram_shadow, 0, sizeof(spriteram_shadow));
  spriteram_front = 0;
  memset(gng_vram_shadow, 0, sizeof(gng_vram_shadow));
  vram_front = 0;
  coinBackup = 0;
  coinFrameCounter = 0;

  m6809_reset(&main_cpu);
  install_rom_direct();

  ResetZ80(&cpu[0]);
  current_cpu = 1;

  ym_tables_init();
}

void gng::stop() {
}

void gng::install_rom_direct(void) {
  main_cpu.rom_direct = gng_maincpu + 0x6000;
  main_cpu.rom_base   = 0x6000;
  main_cpu.rom_size   = 0xA000;
}

void gng::set_bank(unsigned char data) {
  if (data == 4) {
    m_bank = 4;
    m_bankptr = gng_maincpu + 0x4000;
  } else {
    m_bank = data & 0x03;
    m_bankptr = gng_maincpu + 0x10000 + m_bank * 0x2000;
  }
}

void gng::mainlatch_w(unsigned char bit, unsigned char val) {
  switch (bit) {
    case 0:
      m_flip = !val;
      break;
    case 1:
      if (val && !audio_running) {
        ResetZ80(&cpu[0]);
        ym_addr[0] = ym_addr[1] = 0;
        memset(ym_regs, 0, sizeof(ym_regs));
      }
      audio_running = (val != 0);
      break;
    case 2:
    case 3:
      break;
  }
}

unsigned char IRAM_ATTR gng::m6809_read(m6809_state *s, uint16_t addr) {
  if (addr < 0x3000) {
    return memory[addr];
  }

  if (addr >= 0x6000)
    return gng_maincpu[addr];

  if (addr >= 0x4000)
    return m_bankptr[addr - 0x4000];

  switch (addr) {
    case 0x3000: {
      unsigned char k = input->buttons_get();
      unsigned char r = 0xff;
      if (k & BUTTON_START) r &= ~0x01;
      if ((k & BUTTON_COIN) && !coinBackup) { coinFrameCounter = 4; coinBackup = 1; }
      if (coinFrameCounter > 0) r &= ~0x40;
      else if (!(k & BUTTON_COIN)) coinBackup = 0;
      return r;
    }
    case 0x3001: {
      unsigned char k = input->buttons_get();
      unsigned char r = 0xff;
      if (k & BUTTON_RIGHT) r &= ~0x01;
      if (k & BUTTON_LEFT)  r &= ~0x02;
      if (k & BUTTON_DOWN)  r &= ~0x04;
      if (k & BUTTON_UP)    r &= ~0x08;
      if (k & BUTTON_FIRE)  r &= ~0x10;
      if (k & BUTTON_EXTRA) r &= ~0x20;
      return r;
    }
    case 0x3002:
      return 0xff;
    case 0x3003:
      return GNG_DSW1_DEFAULT |
             (input->demoSoundsOff() ? GNG_DSW1_DEMO_SOUNDS_OFF : 0);
    case 0x3004:
      return GNG_DSW2_DEFAULT;
  }
  return 0xff;
}

void IRAM_ATTR gng::m6809_write(m6809_state *s, uint16_t addr, uint8_t val) {
  if (addr < 0x3000) {
    memory[addr] = val;
    if (!game_started && addr >= 0x2000 && val != 0)
      game_started = 1;
    return;
  }

  if (addr >= 0x6000) return;

  if (addr >= 0x4000) return;

  if (addr >= 0x3800 && addr <= 0x38ff) { palette_write(addr - 0x3800, true,  val); return; }
  if (addr >= 0x3900 && addr <= 0x39ff) { palette_write(addr - 0x3900, false, val); return; }

  switch (addr) {
    case 0x3a00:
      sound_latch = val;
      return;
    case 0x3b08: m_bg_scrollx = (m_bg_scrollx & 0xff00) | val;              return;
    case 0x3b09: m_bg_scrollx = (m_bg_scrollx & 0x00ff) | (val << 8);       return;
    case 0x3b0a: m_bg_scrolly = (m_bg_scrolly & 0xff00) | val;              return;
    case 0x3b0b: m_bg_scrolly = (m_bg_scrolly & 0x00ff) | (val << 8);       return;
    case 0x3c00: {
      unsigned char back = spriteram_front ^ 1;
      memcpy(spriteram_shadow[back], &memory[GNG_SPRITE_RAM], 0x200);
      spriteram_front = back;
      return;
    }
    case 0x3e00:
      set_bank(val);
      return;
  }
  if (addr >= 0x3d00 && addr <= 0x3d07) {
    mainlatch_w(addr & 0x07, val & 0x01);
    return;
  }
}

unsigned char IRAM_ATTR gng::m6809_read_opcode(m6809_state *s, uint16_t addr) {
  if (addr >= 0x6000)
    return gng_maincpu[addr];
  if (addr >= 0x4000)
    return m_bankptr[addr - 0x4000];
  return m6809_read(s, addr);
}

unsigned char gng::opZ80(unsigned short Addr) {
  return (Addr < 0x8000) ? gng_audiocpu[Addr] : 0xff;
}

unsigned char gng::rdZ80(unsigned short Addr) {
  if (Addr < 0x8000)
    return gng_audiocpu[Addr];
  if (Addr >= 0xc000 && Addr <= 0xc7ff)
    return memory[GNG_AUDIO_RAM + (Addr - 0xc000)];
  if (Addr == 0xc800)
    return sound_latch;
  return 0xff;
}

void gng::wrZ80(unsigned short Addr, unsigned char Value) {
  if (Addr >= 0xc000 && Addr <= 0xc7ff) {
    memory[GNG_AUDIO_RAM + (Addr - 0xc000)] = Value;
    return;
  }
  if (Addr >= 0xe000 && Addr <= 0xe003) {
    int chip = (Addr >= 0xe002) ? 1 : 0;
    if (Addr & 1)
      ym_write(chip, ym_addr[chip], Value);
    else
      ym_addr[chip] = Value;
    return;
  }
}

void gng::outZ80(unsigned short Port, unsigned char Value) { (void)Port; (void)Value; }
unsigned char gng::inZ80(unsigned short Port) { (void)Port; return 0xff; }

void gng::palette_write(unsigned short offset, bool ext, unsigned char value) {
  static unsigned char pal_base[256];
  static unsigned char pal_ext[256];

  if (ext) pal_ext[offset]  = value;
  else     pal_base[offset] = value;

  unsigned char hi = pal_ext[offset];
  unsigned char lo = pal_base[offset];

  unsigned char r4 = (hi >> 4) & 0x0f;
  unsigned char g4 =  hi       & 0x0f;
  unsigned char b4 = (lo >> 4) & 0x0f;

  unsigned char r8 = (r4 << 4) | r4;
  unsigned char g8 = (g4 << 4) | g4;
  unsigned char b8 = (b4 << 4) | b4;

  unsigned short c = ((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3);
  gng_palette_cpu[offset] = (c >> 8) | (c << 8);
}

void gng::publish_palette(void) {
  unsigned char back = palette_front ^ 1;
  memcpy(gng_palette_shadow[back], gng_palette_cpu, sizeof(gng_palette_cpu));
  palette_front = back;
}

void gng::publish_vram(void) {
  unsigned char back = vram_front ^ 1;
  memcpy(gng_vram_shadow[back], &memory[GNG_FG_VRAM], sizeof(gng_vram_shadow[0]));
  vram_front = back;
}

void gng::run_frame(void) {
  const int audio_irq_step = GNG_SLICES / GNG_AUDIO_IRQ_PER_FRAME;

  for (int i = 0; i < GNG_SLICES; i++) {
    m6809_step(&main_cpu, GNG_MAIN_STEP);

    if (audio_running) {
      current_cpu = 1;
      for (int s = 0; s < GNG_AUDIO_STEP; s++)
        StepZ80(&cpu[0]);
      if ((i % audio_irq_step) == audio_irq_step - 1)
        IntZ80(&cpu[0], INT_IRQ);
    }
  }   // <-- loop now closes here, right after the CPU stepping

  ym_frame();

  m6809_irq(&main_cpu);

  publish_palette();
  publish_vram();

  if (coinFrameCounter > 0)
    coinFrameCounter--;

}

void gng::prepare_frame(void) {
  scan_sprites();
}

void IRAM_ATTR gng::scan_sprites(void) {
  active_sprites = 0;
  const unsigned char *sr = spriteram_shadow[m_render_spriteram];

  for (int offs = 0x200 - 4; offs >= 0 && active_sprites < 128; offs -= 4) {
    unsigned char b0   = sr[offs + 0];
    unsigned char attr = sr[offs + 1];

    unsigned short code10 = (b0 + ((attr << 2) & 0x300)) & 0x3ff;

    struct sprite_S spr;
    spr.code   = code10 & 0xff;         // low 8 bits (sprite_S.code is 8-bit)
    spr.color_block = code10 >> 8;      // bits 8-9 of the 10-bit sprite code
    spr.color  = (attr >> 4) & 3;
    spr.flip_x = (attr & 0x04) ? 1 : 0;
    spr.flip_y = (attr & 0x08) ? 1 : 0;
    spr.is_32x32 = 0;

    int mame_x = sr[offs + 3] - 0x100 * (attr & 0x01);
    int mame_y = sr[offs + 2];

    if (m_flip) {
      mame_x = 240 - mame_x;
      mame_y = 240 - mame_y;
      spr.flip_x ^= 1;
      spr.flip_y ^= 1;
    }

    mame_y = 240 - mame_y;
    spr.flip_y ^= 1;

    spr.y = mame_x + GNG_SCREEN_X_ADJ;   // ← horizontal shift applied here(native mame)
    spr.x = mame_y - 16;              // fb col base (native mame_y, mirrored)

    if (m_flip)
      spr.x += GNG_SPR_FLIP_ON_Y_ADJ;

    if (spr.y + 16 <= 0 || spr.y >= 288 || spr.x + 16 <= 0 || spr.x >= 224)
      continue;

    sprite[active_sprites++] = spr;
  }
}

#define GNG_PANEL_MIRROR_COL(mame_y) (255 - (mame_y))
void IRAM_ATTR gng::blit_bg_strip(short row, char front) {
  int line0 = row * 8;
  const unsigned char *vram   = gng_vram_shadow[m_render_vram];
  const unsigned char *bg_chr  = &vram[(GNG_BG_VRAM - GNG_FG_VRAM)];
  const unsigned char *bg_attr = &vram[(GNG_BG_VRAM - GNG_FG_VRAM) + 0x400];

  for (int r = 0; r < 8; r++) {
    int fb_line = line0 + r;
    int mame_x = fb_line;
    if (mame_x < 0 || mame_x >= GNG_MAME_FULL_W)
      continue;
    mame_x = m_flip ? (255 - mame_x) : mame_x;

    // Horizontal shift (left/right on the physical screen)
    mame_x += GNG_SCREEN_X_ADJ;
    if (mame_x < 0 || mame_x >= GNG_MAME_FULL_W)
      continue;

    unsigned short *ptr = frame_buffer + r * 224;

    int world_x = (mame_x + m_bg_scrollx) & 0x1ff;
    int tcol    = (world_x >> 4) & 0x1f;
    int px0     = world_x & 0x0f;

    if (!m_flip) {
      for (int scr_col = 223; scr_col >= 0; ) {
        int mame_y  = 239 - scr_col;
        int world_y = (mame_y + m_bg_scrolly) & 0x1ff;
        int trow    = (world_y >> 4) & 0x1f;
        int py0     = world_y & 0x0f;

        unsigned short idx = tcol * 32 + trow;
        unsigned char chr  = bg_chr[idx];
        unsigned char attr = bg_attr[idx];

        int run = 16 - py0;
        if (run > scr_col + 1) run = scr_col + 1;

        bool grp1 = (attr & 0x08);
        if (front && !grp1) { scr_col -= run; continue; }

        unsigned int tile_id = (chr + ((attr & 0xc0) << 2)) & 0x3ff;
        const unsigned short *colors = &gng_palette_shadow[m_render_palette][(attr & 0x07) << 3];
        bool flip_x = (attr & 0x10);
        bool flip_y = (attr & 0x20);
        int tile_x = flip_x ? (15 - px0) : px0;

        for (int k = 0; k < run; k++) {
          int py = py0 + k;
          int tile_y = flip_y ? (15 - py) : py;
          unsigned char pen = gng_tiles[tile_id][tile_y][tile_x];
          bool punch = (pen == 0 || pen == 6);
          if (front ? !punch : !(grp1 && !punch))
            ptr[scr_col - k] = colors[pen];
        }
        scr_col -= run;
      }
    } else {
      for (int scr_col = 0; scr_col <= 223; ) {
        int mame_y  = 16 + scr_col;
        int world_y = (mame_y + m_bg_scrolly) & 0x1ff;
        int trow    = (world_y >> 4) & 0x1f;
        int py0     = world_y & 0x0f;

        unsigned short idx = tcol * 32 + trow;
        unsigned char chr  = bg_chr[idx];
        unsigned char attr = bg_attr[idx];

        int run = 16 - py0;
        if (run > 224 - scr_col) run = 224 - scr_col;

        bool grp1 = (attr & 0x08);
        if (front && !grp1) { scr_col += run; continue; }

        unsigned int tile_id = (chr + ((attr & 0xc0) << 2)) & 0x3ff;
        const unsigned short *colors = &gng_palette_shadow[m_render_palette][(attr & 0x07) << 3];
        bool flip_x = (attr & 0x10);
        bool flip_y = (attr & 0x20);
        int tile_x = flip_x ? (15 - px0) : px0;

        for (int k = 0; k < run; k++) {
          int py = py0 + k;
          int tile_y = flip_y ? (15 - py) : py;
          unsigned char pen = gng_tiles[tile_id][tile_y][tile_x];
          bool punch = (pen == 0 || pen == 6);
          if (front ? !punch : !(grp1 && !punch))
            ptr[scr_col + k] = colors[pen];
        }
        scr_col += run;
      }
    }
  }
}

void IRAM_ATTR gng::blit_fg_tile(short row, char unused) {
  (void)unused;
  int line0 = row * 8;
  const unsigned char *vram   = gng_vram_shadow[m_render_vram];
  const unsigned char *fg_chr  = &vram[0];
  const unsigned char *fg_attr = &vram[0x400];

  for (int r = 0; r < 8; r++) {
    int fb_line = line0 + r;
    int mame_x = fb_line;
    if (mame_x < 0 || mame_x >= GNG_MAME_FULL_W)
      continue;
    mame_x = m_flip ? (255 - mame_x) : mame_x;

    // Horizontal shift (left/right on the physical screen)
    mame_x += GNG_SCREEN_X_ADJ;
    if (mame_x < 0 || mame_x >= GNG_MAME_FULL_W)
      continue;

    unsigned short *ptr = frame_buffer + r * 224;

    int tcol = (mame_x >> 3) & 0x1f;
    int px0  = mame_x & 7;

    if (!m_flip) {
      for (int scr_col = 223; scr_col >= 0; ) {
        int mame_y = 239 - scr_col;
        int trow = (mame_y >> 3) & 0x1f;
        int py0  = mame_y & 7;

        unsigned short idx = trow * 32 + tcol;
        unsigned char chr  = fg_chr[idx];
        unsigned char attr = fg_attr[idx];

        int run = 8 - py0;
        if (run > scr_col + 1) run = scr_col + 1;

        unsigned int tile_id = (chr + ((attr & 0xc0) << 2)) & 0x3ff;
        const unsigned short *colors = &gng_palette_shadow[m_render_palette][0x80 + ((attr & 0x0f) << 2)];
        bool flip_x = (attr & 0x10);
        bool flip_y = (attr & 0x20);
        int tile_x = flip_x ? (7 - px0) : px0;

        for (int k = 0; k < run; k++) {
          int py = py0 + k;
          int tile_y = flip_y ? (7 - py) : py;
          unsigned char pen = gng_chars[tile_id][tile_y][tile_x];
          if (pen != 3)
            ptr[scr_col - k] = colors[pen];
        }
        scr_col -= run;
      }
    } else {
      for (int scr_col = 0; scr_col <= 223; ) {
        int mame_y = 16 + scr_col;
        int trow = (mame_y >> 3) & 0x1f;
        int py0  = mame_y & 7;

        unsigned short idx = trow * 32 + tcol;
        unsigned char chr  = fg_chr[idx];
        unsigned char attr = fg_attr[idx];

        int run = 8 - py0;
        if (run > 224 - scr_col) run = 224 - scr_col;

        unsigned int tile_id = (chr + ((attr & 0xc0) << 2)) & 0x3ff;
        const unsigned short *colors = &gng_palette_shadow[m_render_palette][0x80 + ((attr & 0x0f) << 2)];
        bool flip_x = (attr & 0x10);
        bool flip_y = (attr & 0x20);
        int tile_x = flip_x ? (7 - px0) : px0;

        for (int k = 0; k < run; k++) {
          int py = py0 + k;
          int tile_y = flip_y ? (7 - py) : py;
          unsigned char pen = gng_chars[tile_id][tile_y][tile_x];
          if (pen != 3)
            ptr[scr_col + k] = colors[pen];
        }
        scr_col += run;
      }
    }
  }
}

void IRAM_ATTR gng::blit_sprite(short row, unsigned char s_idx) {
  const struct sprite_S *s = &sprite[s_idx];
  const int y_strip = row * 8;

  if (s->y + 16 <= y_strip || s->y >= y_strip + 8)
    return;
  unsigned short code = s->code | (s->color_block << 8);   // reassemble the 10-bit sprite code (sprite_S.code is only 8-bit)
  if (code >= 1024)
    return;

  const unsigned short *colors = &gng_palette_shadow[m_render_palette][0x40 + (s->color << 4)];

  int dy0 = (s->y < y_strip) ? (y_strip - s->y) : 0;
  int dy1 = (s->y + 16 > y_strip + 8) ? (y_strip + 8 - s->y) : 16;

  for (int dy = dy0; dy < dy1; dy++) {
    int fb_line = (s->y + dy) - y_strip;
    int tile_x = s->flip_x ? (15 - dy) : dy;
    unsigned short *fb = &frame_buffer[fb_line * 224];

    for (int dx = 0; dx < 16; dx++) {
      int scr_x = s->x + dx;
      if (scr_x < 0 || scr_x >= 224)
        continue;
      int tile_y = s->flip_y ? (15 - dx) : dx;
      unsigned char pen = gng_sprites[code][tile_y][tile_x];
      if (pen != 15)
        fb[scr_x] = colors[pen];
    }
  }
}

void IRAM_ATTR gng::render_row(short row) {
  int line0 = row * 8;
  if (line0 >= GNG_MAME_FULL_W)
    return;

  if (row == 0) {
    m_render_vram = vram_front;
    m_render_palette = palette_front;
    m_render_spriteram = spriteram_front;
    scan_sprites();
  }

  blit_bg_strip(row, 0);
  for (unsigned char s = 0; s < active_sprites; s++)
    blit_sprite(row, s);
  blit_bg_strip(row, 1);
  blit_fg_tile(row, 0);
}

#include "gng_ym.inc"

const hiscore_region_S *gng::hiscoreRegions(unsigned char *count) {
  *count = 0;
  return 0;
}

#ifdef LED_PIN
void gng::gameLeds(CRGB *leds) {
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
void gng::menuLeds(CRGB *leds) {
  memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
#endif
