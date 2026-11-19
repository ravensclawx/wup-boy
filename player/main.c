/* WUP Boy's player: Game Boy and Game Boy Color games on Peanut-GB (the cgb branch) with minigb_apu's sound, both MIT.
 * Started by WUP Boy (../main.c) with the game's path; HOME pauses (Resume, Save State, Load State, Restart,
 * Quit); leaving goes back
 * to WUP Boy. The save (the cartridge's battery memory) is kept in saves/<game>.sav,
 * written at each pause and on leaving; the save state (the whole machine at one moment) in saves/<game>.state.
 *   Controls: A and B (X and Y too), + Start, - Select, the D-pad or the left stick. Hold ZL: rewind. Hold ZR: fast
 *     forward (as fast as the GamePad manages, without sound).
 *   Rewind: every 8th frame the machine's state (50 KB and the battery memory) is compared with the one before; what
 *     changed (XOR, the zeros run-length packed: a few KB) goes on a ring of 144 KB, the oldest falling off. Going back
 *     applies the newest difference to the last state: the same XOR undoes it. Usually well over 10 seconds.
 *   Picture: the firmware's 8-bit layer, no RGB565 screen: 160x144 three times as big (480x432), centred. A Game Boy
 *     Color frame has at most 64 colours (8 background and 8 sprite palettes of 4): palette entries 0..63; the first
 *     Game Boy's shades are 64..79. The pause screen's own colours are 208 and up.
 *   Memory: the whole game (up to 1 MB) and its battery memory in this app's .bss, which fills WUP_BIGMEM: there is
 *     no RGB565 screen in it (app.cfg: screen8 = yes).
 *   Sound: one frame's worth of 48 kHz stereo for each frame. */
#include <stdio.h>
#include <string.h>
#include "wup.h"

#define PEANUT_FULL_GBC_SUPPORT 1
#define ENABLE_SOUND 1
#define PEANUT_GB_IS_LITTLE_ENDIAN 1
#include "minigb_apu.h"
#include "peanut_gb.h"

WUP_APP("WUP Boy Player");

static uint8_t ROM[1 << 20], CART_RAM[1 << 15];
unsigned audio_state(uint8_t *p, int load);                   /* (gen.py adds it to minigb_apu.c) */
#define SCALE 3
#define X0 ((GFX_W - 160 * SCALE) / 2)
#define Y0 ((GFX_H - 144 * SCALE) / 2)
enum { BLACK = 207, PANEL = 208, HILITE = 224 };              /* PANEL..+15: its text, dark to white; HILITE..+15 the same on blue */

static struct gb_s gb;
static uint32_t rom_size, ram_size;
static uint8_t *fb8; static uint32_t stride;
static char error_text[48];

static uint8_t rom_read(struct gb_s *g, const uint_fast32_t a) { (void)g; return a < rom_size ? ROM[a] : 0xff; }
static uint8_t ram_read(struct gb_s *g, const uint_fast32_t a) { (void)g; return a < ram_size ? CART_RAM[a] : 0xff; }
static void ram_write(struct gb_s *g, const uint_fast32_t a, const uint8_t v) { (void)g; if (a < ram_size) CART_RAM[a] = v; }
static void on_error(struct gb_s *g, const enum gb_error_e e, const uint16_t addr) {
    (void)g; snprintf(error_text, sizeof error_text, "It stopped at %04X (%d)", addr, (int)e);
    printf("[gb] %s\n", error_text);
}

/* ---- the machine's state in one piece: the core, the battery memory, the sound. For save states and rewind */
#define STATE_MAX (sizeof(struct gb_s) + sizeof CART_RAM + 512)
static uint32_t state_size(void) { return (uint32_t)sizeof gb + ram_size + audio_state(0, 0); }
static void state_get(uint8_t *o) { memcpy(o, &gb, sizeof gb); memcpy(o + sizeof gb, CART_RAM, ram_size); audio_state(o + sizeof gb + ram_size, 0); }
static void state_set(const uint8_t *i) {
    struct gb_s was = gb;                                  /* (its pointers are this run's, not the state's) */
    memcpy(&gb, i, sizeof gb);
    gb.gb_rom_read = was.gb_rom_read; gb.gb_cart_ram_read = was.gb_cart_ram_read; gb.gb_cart_ram_write = was.gb_cart_ram_write;
    gb.gb_error = was.gb_error; gb.gb_serial_tx = was.gb_serial_tx; gb.gb_serial_rx = was.gb_serial_rx; gb.gb_bootrom_read = was.gb_bootrom_read;
    gb.display.lcd_draw_line = was.display.lcd_draw_line; gb.direct.priv = was.direct.priv;
    memcpy(CART_RAM, i + sizeof gb, ram_size); audio_state((uint8_t *)(uintptr_t)(i + sizeof gb + ram_size), 1);
}

/* ---- rewind: last = the state at the newest snapshot; ring = the differences, oldest first, each
   [u32 n][n bytes: (zeros, literals, the literal bytes)...][u32 n] */
#define RING (144 << 10)
#define REC_MAX (48 << 10)
/* one working copy of the state, for snapshots, saving and loading. Not in .bss with the rest (WUP_BIGMEM is full of
   the game): initialised, so it lives in the app's own area at 0x350000, which is nearly empty */
static uint8_t scratch[STATE_MAX] = { 1 };
#define EVERY 8
static uint8_t last[STATE_MAX], ring[RING]; static uint32_t used, have_last;
static uint8_t *put_n(uint8_t *o, uint32_t v) { while (v >= 128) { *o++ = (uint8_t)(v | 128); v >>= 7; } *o++ = (uint8_t)v; return o; }
static const uint8_t *get_n(const uint8_t *i, uint32_t *v) { uint32_t x = 0; int s = 0; while (*i & 128) { x |= (uint32_t)(*i++ & 127) << s; s += 7; } *v = x | (uint32_t)*i++ << s; return i; }
static void rewind_reset(void) { used = 0; have_last = 0; }
static void rewind_capture(void) {
    static uint8_t rec[REC_MAX + 64];
    uint8_t *now = scratch; uint32_t n = state_size(); state_get(now);
    if (!have_last) { memcpy(last, now, n); have_last = 1; used = 0; return; }
    uint8_t *o = rec; uint32_t i = 0;
    while (i < n && o - rec < REC_MAX) {
        uint32_t z = i; while (z < n && now[z] == last[z]) z++;
        uint32_t l = z; while (l < n && (now[l] != last[l] || (l + 1 < n && now[l + 1] != last[l + 1]))) l++;   /* (a lone equal byte stays in the literal) */
        o = put_n(o, z - i); o = put_n(o, l - z);
        if ((uint32_t)(o - rec) + (l - z) > REC_MAX) { o = rec + REC_MAX; break; }
        for (uint32_t k = z; k < l; k++) *o++ = now[k] ^ last[k];
        i = l;
    }
    uint32_t len = (uint32_t)(o - rec), need = len + 8;
    memcpy(last, now, n);
    if (len >= REC_MAX) { used = 0; return; }              /* (nearly everything changed: nothing to go back through) */
    uint32_t drop = 0;
    while (used - drop + need > RING) { uint32_t k; memcpy(&k, ring + drop, 4); drop += k + 8; }
    if (drop) { memmove(ring, ring + drop, used - drop); used -= drop; }
    memcpy(ring + used, &len, 4); memcpy(ring + used + 4, rec, len); memcpy(ring + used + 4 + len, &len, 4); used += need;
}
static int rewind_step(void) {                         /* one snapshot back: 1, or 0 at the oldest */
    if (!have_last || !used) return 0;
    uint32_t len; memcpy(&len, ring + used - 4, 4);
    const uint8_t *i = ring + used - 4 - len, *e = i + len; uint32_t at = 0;
    while (i < e) { uint32_t z, l; i = get_n(i, &z); i = get_n(i, &l); at += z; for (uint32_t k = 0; k < l; k++) last[at++] ^= *i++; }
    used -= len + 8;
    state_set(last);
    return 1;
}

/* ---- the picture: one Game Boy line -> three rows of 480 */
static void draw_line(struct gb_s *g, const uint8_t *px, const uint_fast8_t line) {
    uint8_t *row = fb8 + (Y0 + line * SCALE) * stride + X0;
    if (g->cgb.cgbMode) for (int x = 0; x < 160; x++) { uint8_t c = px[x]; row[3 * x] = c; row[3 * x + 1] = c; row[3 * x + 2] = c; }
    else for (int x = 0; x < 160; x++) { uint8_t c = (uint8_t)(64 + ((px[x] & LCD_PALETTE_ALL) >> 2) + (px[x] & 3)); row[3 * x] = c; row[3 * x + 1] = c; row[3 * x + 2] = c; }
    memcpy(row + stride, row, 160 * SCALE);
    memcpy(row + 2 * stride, row, 160 * SCALE);
}
static uint32_t colours[80]; static int dim;                  /* the game's 80 colours; dim: shown darker (paused) */
static void set_colour(int i, uint32_t rgb) {
    colours[i] = rgb;
    if (dim) rgb = (rgb >> 2) & 0x3f3f3f;
    wup_palette(i, rgb);
}
static uint16_t pal_shown[64]; static int pal_valid;
static void update_palette(void) {
    if (!gb.cgb.cgbMode) return;
    for (int i = 0; i < 64; i++) {
        uint16_t c = gb.cgb.fixPalette[i];
        if (pal_valid && c == pal_shown[i]) continue;
        pal_shown[i] = c;
        uint32_t r = c >> 10 & 31, gr = c >> 5 & 31, b = c & 31;
        set_colour(i, (r << 3 | r >> 2) << 16 | (gr << 3 | gr >> 2) << 8 | (b << 3 | b >> 2));
    }
    pal_valid = 1;
}
static void set_dim(int on) { dim = on; for (int i = 0; i < 80; i++) set_colour(i, colours[i]); }

/* ---- the pause screen and messages, on the 8-bit layer: boxes, and libwup's fonts through two 16-step ramps */
static void ramp(int base, uint32_t from, uint32_t to) {
    for (int i = 0; i < 16; i++) {
        uint32_t c = 0;
        for (int s = 16; s >= 0; s -= 8) { uint32_t a = from >> s & 255, b = to >> s & 255; c |= (a + (b - a) * (uint32_t)i / 15) << s; }
        wup_palette(base + i, c);
    }
}
static void box(int x, int y, int w, int h, int c) { for (int j = y; j < y + h; j++) memset(fb8 + j * stride + x, c, (size_t)w); }
static int text_w(const font_t *f, const char *s) { return gfx_text_w(f, s); }
static void text(const font_t *f, int x, int base, const char *s, int ramp_base, int level) {   /* level: 15 white .. 8 quiet */
    int pen4 = x * 4;
    for (; *s; s++) {
        int ch = (unsigned char)*s; if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const glyph_t *g = &f->g[ch - FONT_FIRST]; int gx = (pen4 + 2) / 4 + g->x, gy = base + g->y, rowb = (g->w + 1) / 2;
        const unsigned char *b = f->bits + g->off;
        for (int j = 0; j < g->h; j++) {
            int yy = gy + j; if (yy < 0 || yy >= GFX_H) continue;
            for (int i = 0; i < g->w; i++) {
                int xx = gx + i, v = b[j * rowb + i / 2]; v = i & 1 ? v & 15 : v >> 4;
                if (v && xx >= 0 && xx < GFX_W) fb8[yy * stride + xx] = (uint8_t)(ramp_base + v * level / 15);
            }
        }
        pen4 += g->adv4;
    }
}
static void text_c(const font_t *f, int base, const char *s, int ramp_base, int level) { text(f, (GFX_W - text_w(f, s)) / 2, base, s, ramp_base, level); }
static void colours_init(void) {
    wup_palette(BLACK, 0x000000); ramp(PANEL, 0x1c1c1e, 0xffffff); ramp(HILITE, 0x2196f3, 0xffffff);
    static const uint32_t shades[4] = { 0xe3eec0, 0xa9b87c, 0x5d6b3e, 0x1f2a12 };      /* the first Game Boy's four */
    for (int p = 0; p < 4; p++) for (int s = 0; s < 4; s++) set_colour(64 + p * 4 + s, shades[s]);
}
static void clear(void) { for (uint32_t y = 0; y < GFX_H; y++) memset(fb8 + y * stride, BLACK, GFX_W); }
#define PW 440
#define PX ((GFX_W - PW) / 2)
enum { RESUME, SAVE_STATE, LOAD_STATE, RESTART, QUIT, ROWS };
static char state_path[160]; static int has_state, just_saved;
static void pause_draw(const char *name, int sel) {
    static const char *const rows[ROWS] = { "Resume", "Save State", "Load State", "Restart", "Quit" };
    int h = 76 + ROWS * 52 + 52, y = (GFX_H - h) / 2;
    box(PX, y, PW, h, PANEL);
    text_c(&font_label, y + 48, name, PANEL, 15);
    for (int i = 0; i < ROWS; i++) {
        int ry = y + 76 + i * 52, r = i == sel ? HILITE : PANEL, off = i == LOAD_STATE && !has_state;
        if (i == sel) box(PX, ry, PW, 48, HILITE);
        text(&font_label, PX + 28, ry + 33, rows[i], r, off ? 6 : 15);
        if (i == SAVE_STATE && just_saved) text(&font_body, PX + PW - 28 - text_w(&font_body, "Saved"), ry + 31, "Saved", r, 10);
    }
    text_c(&font_body, y + h - 20, "ZL  Rewind        ZR  Fast Forward", PANEL, 8);
    wup_present8(0, GFX_H);
}
static unsigned held;
static unsigned pressed(void) { wup_sleep(16); unsigned b = wup_buttons(), p = b & ~held; held = b; return p; }
static void state_save(void) {
    uint32_t n = state_size(), head[4] = { 0x54534247u /* "GBST" */, 1, n, gb.cgb.cgbMode };
    state_get(scratch);
    if (fs_stream_open(state_path)) return;
    int bad = fs_stream_write(head, 16) || fs_stream_write(scratch, n);
    if (!fs_stream_close() && !bad) { has_state = 1; just_saved = 1; } else fs_remove(state_path);
}
static int state_load(void) {
    uint32_t n = state_size(), head[4];
    if (fs_read(state_path, 0, head, 16) != 16 || head[0] != 0x54534247u || head[1] != 1 || head[2] != n) return -1;   /* (another version's, another game's) */
    if (fs_read(state_path, 16, scratch, n) != (int)n) return -1;
    state_set(scratch);
    return 0;
}
static int pause_menu(const char *name) {              /* -> RESUME, LOAD_STATE (loaded), RESTART or QUIT */
    int sel = RESUME; just_saved = 0; has_state = fs_filesize(state_path) > 16;
    set_dim(1); pause_draw(name, sel);
    for (;;) {
        unsigned p = pressed(); int old = sel;
        if (p & (WUP_B | WUP_HOME)) { sel = RESUME; break; }
        if (p & WUP_A) {
            if (sel == SAVE_STATE) { state_save(); pause_draw(name, sel); continue; }      /* (it says Saved; the menu stays) */
            if (sel == LOAD_STATE && (!has_state || state_load())) continue;
            break;
        }
        if (p & WUP_DOWN) sel = sel < ROWS - 1 ? sel + 1 : sel;
        if (p & WUP_UP) sel = sel > 0 ? sel - 1 : sel;
        if (sel != old) pause_draw(name, sel);
    }
    set_dim(0);
    return sel;
}
static void message(const char *title, const char *line) {   /* an outcome, as the menu shows them; B goes back */
    clear();
    text_c(&font_label, GFX_H / 2 - 8, title, PANEL, 15);
    if (line[0]) text_c(&font_body, GFX_H / 2 + 28, line, PANEL, 9);
    text(&font_body, GFX_W - 32 - text_w(&font_body, "B  Back"), GFX_H - 16, "B  Back", PANEL, 9);
    wup_present8(0, GFX_H);
    held = wup_buttons();
    while (!(pressed() & (WUP_B | WUP_HOME | WUP_A))) {}
}

static void back_to_list(void) {
    char ui[96]; snprintf(ui, sizeof ui, "%s/wup_boy.wup", wup_app_dir()[0] ? wup_app_dir() : "/apps/wup_boy");
    wup_exec(ui, NULL);                                  /* (if it is not there: the menu) */
}
int main(void) {
    const char *path = wup_arg();
    fb8 = wup_screen8(); stride = wup_stride8();
    colours_init(); clear(); wup_present8(0, GFX_H);
    char name[64] = "", sav[160];
    const char *file = strrchr(path, '/'); file = file ? file + 1 : path;
    snprintf(name, sizeof name, "%s", file); if (strrchr(name, '.')) *strrchr(name, '.') = 0;
    snprintf(sav, sizeof sav, "%s/saves", wup_app_dir()[0] ? wup_app_dir() : "/apps/wup_boy"); lfs_mkdir(&fs_lfs, sav);
    snprintf(state_path, sizeof state_path, "%s/%s.state", sav, name);
    snprintf(sav + strlen(sav), sizeof sav - strlen(sav), "/%s.sav", name);
    int size = path[0] ? fs_filesize(path) : -1;
    if (size <= 0) { message("Can't Open Game", ""); back_to_list(); return 0; }
    if ((uint32_t)size > sizeof ROM) { message("Game Too Large", "Games up to 1 MB fit on the GamePad"); back_to_list(); return 0; }
    if (fs_read(path, 0, ROM, (uint32_t)size) != size) { message("Can't Open Game", ""); back_to_list(); return 0; }
    rom_size = (uint32_t)size;
restart:
    error_text[0] = 0; pal_valid = 0; rewind_reset();
    if (gb_init(&gb, rom_read, ram_read, ram_write, on_error, 0) != GB_INIT_NO_ERROR) { message("Can't Play Game", "Not a Game Boy game, or a cartridge type not supported"); back_to_list(); return 0; }
    ram_size = (uint32_t)gb_get_save_size(&gb);
    if (ram_size > sizeof CART_RAM) ram_size = sizeof CART_RAM;
    memset(CART_RAM, 0xff, ram_size);
    if (ram_size) fs_read(sav, 0, CART_RAM, ram_size);
    gb_init_lcd(&gb, draw_line);
    audio_init();
    int sound = wup_audio_open() == 0, quit = 0;
    static int16_t snd[AUDIO_SAMPLES * 2] __attribute__((aligned(4)));
    printf("[gb] %s (%s), %u KB, save %u\n", name, gb.cgb.cgbMode ? "colour" : "mono", (unsigned)(rom_size >> 10), (unsigned)ram_size);
    clear(); wup_present8(0, GFX_H);

    const uint32_t frame_us = 16743;                    /* 59.73 Hz */
    uint32_t next = wup_us(), t_stat = next; int frames = 0, late = 0, count = 0, mode = 0, back_wait = 0;
    void (*draw)(struct gb_s *, const uint8_t *, const uint_fast8_t) = gb.display.lcd_draw_line;
    held = wup_buttons();
    while (!error_text[0] && !quit) {
        unsigned b = wup_buttons(), p = b & ~held; held = b;
        if (p & WUP_HOME) {
            if (sound) wup_audio_close();
            if (ram_size) fs_write(sav, CART_RAM, ram_size);
            int r = pause_menu(name);
            if (r == QUIT) { quit = 1; break; }
            clear(); wup_present8(0, GFX_H); mode = 0;
            if (r == RESTART) goto restart;              /* (the save was just written: it starts from it) */
            if (r == LOAD_STATE) { pal_valid = 0; rewind_reset(); }
            sound = wup_audio_open() == 0; next = wup_us(); held = wup_buttons();
            continue;
        }
        int now = b & WUP_ZL ? 1 : b & WUP_ZR ? 2 : 0;       /* 1 rewinding, 2 fast forward */
        if (now != mode) {                                   /* said in the margin, quietly */
            box(0, GFX_H - 44, X0, 44, BLACK);
            if (now) text(&font_body, 24, GFX_H - 18, now == 1 ? "Rewind" : "Fast Forward", PANEL, 9);
            wup_present8(GFX_H - 44, 44); mode = now; next = wup_us(); back_wait = 0;
        }
        if (mode == 1) {                                     /* back a snapshot every 4th frame: twice as fast as it went */
            if (back_wait-- <= 0) { back_wait = 3; if (rewind_step()) { pal_valid = 0; gb.direct.joypad = 0xff; gb_run_frame(&gb); update_palette(); wup_present8(Y0, 144 * SCALE); have_last = 1; state_get(last); } }
            wup_sleep(16); count = 0;
            continue;
        }
        int sx = wup_stick(0, 0), sy = wup_stick(0, 1);
        uint8_t j = 0xff;                               /* active low: a b select start right left up down */
        if (b & (WUP_A | WUP_X)) j &= ~0x01;
        if (b & (WUP_B | WUP_Y)) j &= ~0x02;
        if (b & WUP_MINUS) j &= ~0x04;
        if (b & WUP_PLUS) j &= ~0x08;
        if ((b & WUP_RIGHT) || sx > 600) j &= ~0x10;
        if ((b & WUP_LEFT) || sx < -600) j &= ~0x20;
        if ((b & WUP_UP) || sy > 600) j &= ~0x40;
        if ((b & WUP_DOWN) || sy < -600) j &= ~0x80;
        gb.direct.joypad = j;
        if (mode == 2) {                                     /* three frames unseen, then one shown; no sound, no waiting */
            gb.direct.frame_skip = 0; gb.display.lcd_draw_line = 0;
            for (int k = 0; k < 3 && !error_text[0]; k++) { gb_run_frame(&gb); if (++count % EVERY == 0) rewind_capture(); }
            gb.display.lcd_draw_line = draw;
        }
        gb_run_frame(&gb);
        if (++count % EVERY == 0) rewind_capture();
        if (sound && mode != 2) {
            audio_callback(0, (uint8_t *)snd, (int)sizeof snd);
            if (wup_audio_queued() < 4 * (int)AUDIO_SAMPLES) wup_audio_queue(snd, AUDIO_SAMPLES);   /* (else: it catches up) */
        }
        update_palette();
        wup_present8(Y0, 144 * SCALE);
        frames++;
        if (mode == 2) { wup_sleep(1); next = wup_us(); continue; }
        next += frame_us;                               /* 59.73 a second; more than a frame behind: skip drawing one */
        int32_t ahead = (int32_t)(next - wup_us());
        if (ahead > 2000) wup_sleep(ahead / 1000);
        else { wup_sleep(1); if (ahead < -(int32_t)frame_us * 4) next = wup_us(); }
        gb.direct.frame_skip = ahead < -(int32_t)frame_us;
        late += ahead < 0;
        if (wup_us() - t_stat > 5000000) { printf("[gb] %u fps, %d late, rewind %u KB\n", (unsigned)((uint32_t)frames * 1000000u / (wup_us() - t_stat)), late, (unsigned)(used >> 10)); t_stat = wup_us(); frames = late = 0; }
    }
    if (!quit) { wup_audio_close(); if (ram_size) fs_write(sav, CART_RAM, ram_size); message("The Game Stopped", error_text); }
    back_to_list();
    return 0;
}
