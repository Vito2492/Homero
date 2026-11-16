// Mansion: a native front end for the SNES game The Addams Family (USA).
// It runs the player's own ROM through a libretro core, takes its menu art
// from that ROM at start-up, and adds a start menu, an always-available pause
// menu (F1 / Esc / Select+Start / Back), cheats that write the game's RAM,
// save states and a Spanish/English interface. The game itself is never patched.
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "libretro.h"

#ifdef _WIN32
#define CORE_NAME "snes9x_libretro.dll"
#else
#define CORE_NAME "snes9x_libretro.so"
#endif

#define W 256
#define H 224
#define ROM_CRC 0x2e8034abu

/* ------------------------------------------------------------------ */
/* Core                                                                 */
/* ------------------------------------------------------------------ */

static struct {
    void (*init)(void), (*deinit)(void), (*run)(void), (*reset)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*get_av)(struct retro_system_av_info *);
    void *(*mem_data)(unsigned);
    size_t (*mem_size)(unsigned);
    size_t (*ser_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
} core;

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint32_t game[512 * 512];     // last frame, ARGB
static int gw = W, gh = H;
static uint16_t pad;                 // held buttons (RETRO_DEVICE_ID_JOYPAD_* bits)
static bool layer_on[5] = { true, true, true, true, true };
static bool layers_dirty;             // any core option changed
static bool overclock;                // "no slowdown"
static bool scripting;                // menus driven by the program: never overclocked
static char layer_val[5][16];
static SDL_AudioDeviceID audio;
static bool mute;
static char base[1024];

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd & 0xffff) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixfmt = *(enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = base;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        int n;
        if (sscanf(v->key, "snes9x_layer_%d", &n) == 1 && n >= 1 && n <= 5) {
            strcpy(layer_val[n - 1], layer_on[n - 1] ? "enabled" : "disabled");
            v->value = layer_val[n - 1];
            return true;
        }
        if (!strcmp(v->key, "snes9x_overclock_cycles")) {
            v->value = overclock && !scripting ? "compatible" : "disabled";
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = layers_dirty;
        layers_dirty = false;
        return true;
    }
    return false;
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || w > 512 || h > 512) return;
    gw = w; gh = h;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        uint32_t *out = game + y * w;
        for (unsigned x = 0; x < w; x++) {
            uint32_t r, g, b;
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                out[x] = 0xff000000u | ((const uint32_t *)row)[x];
                continue;
            }
            uint16_t p = ((const uint16_t *)row)[x];
            if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                r = p >> 11; g = p >> 5 & 63; b = p & 31;
                r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2;
            } else {
                r = p >> 10 & 31; g = p >> 5 & 31; b = p & 31;
                r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
            }
            out[x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
}

static size_t audio_batch_cb(const int16_t *d, size_t frames)
{
    if (audio && !mute) SDL_QueueAudio(audio, d, (Uint32)(frames * 4));
    return frames;
}
static void audio_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_batch_cb(s, 1); }
static void poll_cb(void) {}
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pad;
    return id < 16 ? (pad >> id & 1) : 0;
}

static void set_layers(int only)       // 0 = all, 1..5 = just that one
{
    for (int i = 0; i < 5; i++) layer_on[i] = !only || i + 1 == only;
    layers_dirty = true;
}

static uint8_t *wram(void) { return core.mem_data(RETRO_MEMORY_SYSTEM_RAM); }

static bool load_core(const char *path)
{
    void *so = SDL_LoadObject(path);
    if (!so) return false;
#define SYM(field, name) if (!(*(void **)&core.field = SDL_LoadFunction(so, name))) return false
    SYM(init, "retro_init"); SYM(deinit, "retro_deinit"); SYM(run, "retro_run"); SYM(reset, "retro_reset");
    SYM(set_environment, "retro_set_environment"); SYM(set_video_refresh, "retro_set_video_refresh");
    SYM(set_audio_sample, "retro_set_audio_sample"); SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(set_input_poll, "retro_set_input_poll"); SYM(set_input_state, "retro_set_input_state");
    SYM(load_game, "retro_load_game"); SYM(get_av, "retro_get_system_av_info");
    SYM(mem_data, "retro_get_memory_data"); SYM(mem_size, "retro_get_memory_size");
    SYM(ser_size, "retro_serialize_size"); SYM(serialize, "retro_serialize"); SYM(unserialize, "retro_unserialize");
#undef SYM
    core.set_environment(env_cb);
    core.init();
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(poll_cb);
    core.set_input_state(input_cb);
    return true;
}

/* ------------------------------------------------------------------ */
/* Art taken from the ROM                                               */
/* ------------------------------------------------------------------ */

static uint32_t bg[W * H];           // stone frame with vines (password screen, BG1)
static uint8_t font[64][8][8];       // ASCII 0x20-0x5f, values 0-3
static uint32_t cursor[32 * 32];     // pointing hand from the title menu
static int curw, curh;

static void run(int n) { while (n--) core.run(); }
static void tap(int btn, int wait) { pad = 1 << btn; run(4); pad = 0; run(wait); }
static bool visible(void)
{
    for (int i = 0; i < gw * gh; i++) if (game[i] & 0xffffff) return true;
    return false;
}

static bool extract_art(void)
{
    mute = true;
    run(60 * 12);                                    // legal screens, Ocean logo
    tap(RETRO_DEVICE_ID_JOYPAD_START, 180);          // skip the intro
    tap(RETRO_DEVICE_ID_JOYPAD_START, 90);           // title menu

    set_layers(5); run(2);                           // sprites only: the hand
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (game[y * gw + x] & 0xffffff) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    if (x1 < 0 || x1 - x0 >= 32 || y1 - y0 >= 32) goto fail;
    curw = x1 - x0 + 1; curh = y1 - y0 + 1;
    for (int y = 0; y < curh; y++)
        for (int x = 0; x < curw; x++) {
            uint32_t p = game[(y0 + y) * gw + x0 + x];
            cursor[y * curw + x] = (p & 0xffffff) ? p : 0;
        }

    set_layers(0);
    tap(RETRO_DEVICE_ID_JOYPAD_DOWN, 30);
    tap(RETRO_DEVICE_ID_JOYPAD_START, 30);           // password screen
    for (int i = 0; i < 60 && visible(); i++) run(10);
    for (int i = 0; i < 60 && !visible(); i++) run(10);
    run(40);
    set_layers(1); run(2);
    for (int y = 0; y < H; y++) memcpy(bg + y * W, game + y * gw, W * 4);

    const uint8_t *v = core.mem_data(RETRO_MEMORY_VIDEO_RAM);
    if (!v || core.mem_size(RETRO_MEMORY_VIDEO_RAM) < 0x10000) goto fail;
    int ink = 0;
    for (int c = 0; c < 64; c++) {
        const uint8_t *t = v + (0x200 + 0x20 + c) * 16;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                font[c][y][x] = (t[y * 2] >> (7 - x) & 1) | (t[y * 2 + 1] >> (7 - x) & 1) << 1;
                ink += font[c][y][x] == 3;
            }
    }
    if (ink < 200) goto fail;

    set_layers(0);
    core.reset();
    mute = false;
    return true;
fail:
    set_layers(0);
    mute = false;
    return false;
}

/* ------------------------------------------------------------------ */
/* Drawing on the 256x224 menu layer                                    */
/* ------------------------------------------------------------------ */

static uint32_t ui[W * H];
typedef struct { uint32_t c[4]; } pal_t;
static const pal_t WHITE = { { 0, 0xff000000, 0xffdedede, 0xffffffff } };
static const pal_t RED = { { 0, 0xff000000, 0xff8b0000, 0xffe60000 } };
static const pal_t GOLD = { { 0, 0xff000000, 0xffb47814, 0xfffac83c } };
static const pal_t DIM = { { 0, 0xff000000, 0xff5a6e8c, 0xff7891af } };

static void text(int x, int y, const char *s, pal_t p)
{
    for (; *s; s++, x += 8) {
        int c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c >= 0x60) c = '?';
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                int v = font[c - 0x20][yy][xx], px = x + xx, py = y + yy;
                if (v && px >= 0 && px < W && py >= 0 && py < H) ui[py * W + px] = p.c[v];
            }
    }
}
static void text_c(int y, const char *s, pal_t p) { text(W / 2 - (int)strlen(s) * 4, y, s, p); }
static void text_r(int x, int y, const char *s, pal_t p) { text(x - (int)strlen(s) * 8, y, s, p); }

static void shade(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = ui[y * W + x];
            uint32_t r = (p >> 16 & 255) * 2 / 5 + 4, g = (p >> 8 & 255) * 2 / 5 + 8, b = (p & 255) * 2 / 5 + 16;
            ui[y * W + x] = 0xff000000u | r << 16 | g << 8 | b;
        }
}

static void draw_cursor(int x, int y)
{
    for (int yy = 0; yy < curh; yy++)
        for (int xx = 0; xx < curw; xx++)
            if (cursor[yy * curw + xx]) ui[(y + yy) * W + x + xx] = cursor[yy * curw + xx];
}

/* ------------------------------------------------------------------ */
/* Settings, language, cheats                                           */
/* ------------------------------------------------------------------ */

// The SNES buttons the player can remap, in the order the Controls page lists them.
static const int snes_btn[12] = { RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
    RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
    RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_START,
    RETRO_DEVICE_ID_JOYPAD_SELECT };
static const char *snes_name[12] = { "ARRIBA", "ABAJO", "IZQ", "DER", "B", "A", "Y", "X", "L", "R", "START", "SELECT" };
static const char *snes_name_en[12] = { "UP", "DOWN", "LEFT", "RIGHT", "B", "A", "Y", "X", "L", "R", "START", "SELECT" };
static const int kb_default[12] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_W,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT };
static const int gp_default[12] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,  // SNES layout
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_BACK };

static struct {
    int lang, fullscreen, hp, lives, fez, sword, golf, fastcpu, god, timer, easy;
    int beaten;                                       // the ending was reached: boss rush unlocked
    int r_hearts, r_hp, r_lives, r_sword, r_golf;     // boss rush rules
    int kb[12], gp[12];
} cfg = { -1, .r_hearts = 5, .r_lives = 3, .r_sword = 1 };
#define T(es, en) (cfg.lang == 1 ? (en) : (es))

static void cfg_path(char *out, size_t n) { snprintf(out, n, "%smansion.cfg", base); }
static void cfg_load(void)
{
    char p[1100], k[32]; int v, n; cfg_path(p, sizeof p);
    memcpy(cfg.kb, kb_default, sizeof cfg.kb);
    memcpy(cfg.gp, gp_default, sizeof cfg.gp);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fscanf(f, " %31[^=]=%d", k, &v) == 2) {
        if (!strcmp(k, "lang")) cfg.lang = v;
        else if (!strcmp(k, "fullscreen")) cfg.fullscreen = v;
        else if (!strcmp(k, "infinite_health")) cfg.hp = v;
        else if (!strcmp(k, "infinite_lives")) cfg.lives = v;
        else if (!strcmp(k, "flying_hat")) cfg.fez = v;
        else if (!strcmp(k, "sword")) cfg.sword = v;
        else if (!strcmp(k, "golf_ball")) cfg.golf = v;
        else if (!strcmp(k, "no_slowdown")) cfg.fastcpu = v;
        else if (!strcmp(k, "god_mode")) cfg.god = v;
        else if (!strcmp(k, "easy_mode")) cfg.easy = v;
        else if (!strcmp(k, "show_timer")) cfg.timer = v;
        else if (!strcmp(k, "game_beaten")) cfg.beaten = v;
        else if (!strcmp(k, "rush_hearts")) cfg.r_hearts = v;
        else if (!strcmp(k, "rush_infinite_health")) cfg.r_hp = v;
        else if (!strcmp(k, "rush_lives")) cfg.r_lives = v;
        else if (!strcmp(k, "rush_sword")) cfg.r_sword = v;
        else if (!strcmp(k, "rush_golf")) cfg.r_golf = v;
        else if (sscanf(k, "key_%d", &n) == 1 && n >= 0 && n < 12) cfg.kb[n] = v;
        else if (sscanf(k, "pad_%d", &n) == 1 && n >= 0 && n < 12) cfg.gp[n] = v;
    }
    fclose(f);
}
static void cfg_save(void)
{
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "lang=%d\nfullscreen=%d\ninfinite_health=%d\ninfinite_lives=%d\nflying_hat=%d\n"
               "sword=%d\ngolf_ball=%d\nno_slowdown=%d\ngod_mode=%d\nshow_timer=%d\n",
            cfg.lang, cfg.fullscreen, cfg.hp, cfg.lives, cfg.fez, cfg.sword, cfg.golf, cfg.fastcpu, cfg.god, cfg.timer);
    fprintf(f, "easy_mode=%d\n", cfg.easy);
    fprintf(f, "game_beaten=%d\nrush_hearts=%d\nrush_infinite_health=%d\nrush_lives=%d\nrush_sword=%d\nrush_golf=%d\n",
            cfg.beaten, cfg.r_hearts, cfg.r_hp, cfg.r_lives, cfg.r_sword, cfg.r_golf);
    for (int i = 0; i < 12; i++) fprintf(f, "key_%d=%d\npad_%d=%d\n", i, cfg.kb[i], i, cfg.gp[i]);
    fclose(f);
}

static void guess_language(void)
{
    if (cfg.lang >= 0) return;
    cfg.lang = 1;
    SDL_Locale *l = SDL_GetPreferredLocales();
    if (l && l[0].language && !strcmp(l[0].language, "es")) cfg.lang = 0;
    if (l) SDL_free(l);
}

// RAM (7E:00xx). From the game's Pro Action Replay codes, checked in new games
// and with passwords: C4 holds the number of heart containers (2-5), EE/EF are 1
// when Gomez has the sword / golf balls.
// C6 counts down from 80 after a hit (Gomez can't be hurt while it runs).
// 100-10B: progress set by passwords; 100 = Giant Budgie beaten, 107/109/10B =
// the family members rescued (all three set by the "whole family" codes).
// Writing 8A while a game loads starts it in that zone (00-0E).
// A1/A2: Gomez's position in 16-pixel blocks inside the zone (TASVideos table).
// F6: the boss's health, FF when there is no boss (0 = beaten, per the game's PAR code).
enum { RAM_FEZ = 0x64, RAM_ZONE = 0x8a, RAM_X = 0xa1, RAM_Y = 0xa2, RAM_LIVES = 0xac, RAM_HEALTH = 0xc3,
       RAM_MAXHP = 0xc4, RAM_INVULN = 0xc6, RAM_SWORD = 0xee, RAM_GOLF = 0xef, RAM_BOSS = 0xf6,
       RAM_PROGRESS = 0x100 };
#define NZONES 15
static const char *zones[NZONES] = { "HALL OF STAIRS", "ADDAMS RESIDENCE", "THE OLD TREE", "THE CONSERVATORY",
    "THE CHAIN ROOM", "THE KITCHEN", "PORTRAIT GALLERY", "THE GAMES ROOM", "THE UNDERGROUND", "THE FURNACE",
    "THE ARMOURY", "SPOOKY DROP", "THE CAVES", "THE LIBRARY", "ADDAMS' VAULT" };
static uint32_t total_frames, zone_frames[NZONES];
static uint8_t lives_hold;

static bool rush;                        // a boss rush is running (its own rules, no cheats)
static void cheats(void)
{
    uint8_t *m = wram();
    if (!m) return;
    if (rush) {
        uint8_t mx = m[RAM_MAXHP];
        if (cfg.r_hp && mx >= 1 && mx <= 9 && m[RAM_HEALTH] < mx) m[RAM_HEALTH] = mx;
        if (cfg.r_lives == 99 && m[RAM_LIVES] < 9) m[RAM_LIVES] = 9;
        return;
    }
    if (cfg.hp) {               // always full: as many hearts as Gomez has containers
        uint8_t mx = m[RAM_MAXHP];
        if (mx >= 1 && mx <= 9 && m[RAM_HEALTH] < mx) m[RAM_HEALTH] = mx;
    }
    if (cfg.lives) {
        if (m[RAM_LIVES] > lives_hold && m[RAM_LIVES] < 0xa0) lives_hold = m[RAM_LIVES];
        if (lives_hold && m[RAM_LIVES] < lives_hold) m[RAM_LIVES] = lives_hold;
    }
    if (cfg.fez) m[RAM_FEZ] = 0xff;
    if (cfg.sword) m[RAM_SWORD] = 1;
    if (cfg.golf) m[RAM_GOLF] = 1;
    if (cfg.god) m[RAM_INVULN] = 2;      // always "just hit": no damage; 2 doesn't make Gomez blink
}

/* Easy mode: a hit costs a quarter of a heart. The game only knows whole hearts,
   so before every frame the program keeps a snapshot; when a hit lands and the
   current heart still has quarters left, it goes back to that snapshot, gives
   Gomez the game's own after-hit invulnerability and plays the frame again, so
   the hit never happens. The fourth hit is let through and costs the heart.
   (Restoring the heart after a lethal hit is too late: the game takes the life
   in the same frame.) */
static int quarters;                     // quarters lost from the partly used heart (0-3)
static int easy_hearts;                  // hearts Gomez has, counting the partly used one
static bool easy_sync = true;            // take the game's numbers as they are (new game, load, death...)
static uint8_t easy_lives, easy_zone;
static void *easy_snap;
static size_t easy_size;
static int easy_test_pickup;             // --test only: pretend the game gave hearts this frame

// While a heart is partly used, the game is told one heart less, so a heart picked
// up shows as a rise even when Gomez was "full" (the bug where a heart didn't heal).
// With only the partly used heart left the game keeps 1, or it would kill Gomez.
static int easy_game_hearts(void) { return quarters && easy_hearts >= 2 ? easy_hearts - 1 : easy_hearts; }

static void frame_easy(void)
{
    uint8_t *m = wram();
    bool on = cfg.easy && !cfg.god && !cfg.hp && !rush && m && m[RAM_ZONE] < 0x0f;
    if (!on) { cheats(); core.run(); cheats(); easy_sync = true; return; }
    if (easy_sync || m[RAM_LIVES] != easy_lives || m[RAM_ZONE] != easy_zone ||
        easy_hearts < 1 || easy_hearts > 9 || m[RAM_HEALTH] == 0) {
        easy_hearts = m[RAM_HEALTH]; quarters = 0; easy_sync = false;
    }
    easy_lives = m[RAM_LIVES]; easy_zone = m[RAM_ZONE];
    int expect = easy_game_hearts();
    if (expect >= 1) m[RAM_HEALTH] = (uint8_t)expect;
    if (!easy_snap) { easy_size = core.ser_size(); easy_snap = malloc(easy_size); }
    bool snap = easy_snap && core.serialize(easy_snap, easy_size);
    cheats(); core.run(); cheats();
    m = wram();
    if (easy_test_pickup) { m[RAM_HEALTH] = (uint8_t)(m[RAM_HEALTH] + easy_test_pickup); easy_test_pickup = 0; }
    int got = m[RAM_HEALTH];
    bool died = m[RAM_LIVES] != easy_lives;              // a lethal hit takes the life in the same frame
    if (m[RAM_ZONE] != easy_zone) { easy_sync = true; return; }
    bool last = easy_hearts == 1 && quarters == 3;       // the very last quarter: let the game have it
    if (died && !last) got = expect - 1;
    if (got < expect && last) { easy_sync = true; return; }
    if (got > expect) {                                    // picked up hearts
        int n = got - expect;
        if (quarters) { quarters = 0; n--; }               // the first one fills the partly used heart
        easy_hearts += n;
        if (easy_hearts > m[RAM_MAXHP]) easy_hearts = m[RAM_MAXHP];
    } else if (got < expect) {                             // hit: undo it, the program keeps the count
        if (++quarters == 4) { quarters = 0; easy_hearts--; }
        if (snap && core.unserialize(easy_snap, easy_size)) {
            m = wram();
            m[RAM_INVULN] = 80;                            // what the game gives after a hit
            m[RAM_HEALTH] = (uint8_t)easy_game_hearts();
            mute = true; cheats(); core.run(); cheats(); mute = false;
        }
    }
    m = wram();
    expect = easy_game_hearts();
    if (expect >= 1 && m[RAM_HEALTH] != 0) m[RAM_HEALTH] = (uint8_t)expect;
}

// The partly used heart on the game's HUD (hearts at x 9-21, one every 16 px from y 33).
// When the game shows one heart less, the missing one is copied from the first heart;
// then the lost quarters go dark, clockwise from the top right.
static void draw_easy_heart(void)
{
    if (!cfg.easy || !quarters || easy_hearts < 1 || easy_hearts > 5) return;
    int top = 33 + 16 * (easy_hearts - 1);
    for (int yy = 0; yy < 12; yy++)
        for (int x = 9; x <= 21; x++) {
            int y = top + yy;
            uint32_t g = game[y * gw + x];
            if (easy_hearts >= 2) {                        // the game isn't drawing this one
                uint32_t src = game[(33 + yy) * gw + x];
                bool red = (src >> 16 & 255) >= 150 && (src >> 8 & 255) <= 110;
                bool white = (src >> 16 & 255) > 200 && (src >> 8 & 255) > 200 && (src & 255) > 200;
                if (!red && !white) continue;
                ui[y * W + x] = src;
                g = src;
                if (white) continue;
            } else if ((g >> 16 & 255) < 150 || (g >> 8 & 255) > 110) continue;     // only the red of the heart
            bool right = x >= 15, low = yy >= 6;
            int q = right ? (low ? 1 : 0) : (low ? 2 : 3);
            if (q < quarters) ui[y * W + x] = 0xff3c2a30;
        }
}

static int zone_now(void) { uint8_t *m = wram(); return m && m[RAM_ZONE] < NZONES ? m[RAM_ZONE] : -1; }
static const char *zone_name(void) { int z = zone_now(); return z < 0 ? "" : zones[z]; }

static void count_frame(void)            // game time, total and per zone
{
    total_frames++;
    int z = zone_now();
    if (z >= 0) zone_frames[z]++;
}
static void reset_times(void) { total_frames = 0; memset(zone_frames, 0, sizeof zone_frames); }

/* Automap: every block that has been on screen, per zone (1 bit per 16x16 block). */
#define MAP_W 256
#define MAP_H 128
static uint8_t seen[NZONES][MAP_H][MAP_W / 8];
static bool map_dirty;

static void map_mark(void)
{
    uint8_t *m = wram();
    int z = zone_now();
    if (z < 0) return;
    int gx = m[RAM_X], gy = m[RAM_Y];
    static int lx = -1, ly = -1, lz = -1;
    if (gx == lx && gy == ly && z == lz) return;
    lx = gx; ly = gy; lz = z;
    for (int y = gy - 7; y <= gy + 6; y++)          // roughly what the screen shows around Gomez
        for (int x = gx - 8; x <= gx + 7; x++)
            if (x >= 0 && x < MAP_W && y >= 0 && y < MAP_H && !(seen[z][y][x >> 3] & (1 << (x & 7)))) {
                seen[z][y][x >> 3] |= (uint8_t)(1 << (x & 7));
                map_dirty = true;
            }
}
static bool map_has(int z, int x, int y) { return seen[z][y][x >> 3] >> (x & 7) & 1; }
static void map_io(bool save)
{
    char p[1100]; snprintf(p, sizeof p, "%smap.bin", base);
    FILE *f = fopen(p, save ? "wb" : "rb");
    if (!f) return;
    if (save) fwrite(seen, 1, sizeof seen, f); else if (fread(seen, 1, sizeof seen, f) != sizeof seen) memset(seen, 0, sizeof seen);
    fclose(f);
    map_dirty = false;
}

/* Boss rush: the program saves a state the first time each boss fight starts. */
static bool boss_found[NZONES];
static void boss_path(char *out, size_t n, int z) { snprintf(out, n, "%sboss_%02x.state", base, z); }
static void boss_scan(void)
{
    char p[1100];
    for (int z = 0; z < NZONES; z++) {
        boss_path(p, sizeof p, z);
        FILE *f = fopen(p, "rb");
        boss_found[z] = f != NULL;
        if (f) fclose(f);
    }
}
static uint8_t boss_prev = 0xff;
static void *boss_snap;                  // state taken when the boss's health first appears
static int boss_snap_zone = -1, boss_frames;
static void say(const char *s);
static void boss_watch(void)
{
    uint8_t *m = wram();
    int z = zone_now();
    if (!m || z < 0) return;
    uint8_t b = m[RAM_BOSS];
    if (b != 0xff && boss_prev == 0xff && !boss_found[z]) {
        size_t n = core.ser_size();
        free(boss_snap);
        boss_snap = malloc(n);
        if (boss_snap && core.serialize(boss_snap, n)) { boss_snap_zone = z; boss_frames = 0; }
    }
    if (b != 0xff && boss_snap_zone == z && ++boss_frames == 60) {      // still a boss a second later: keep it
        char p[1100]; boss_path(p, sizeof p, z);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(boss_snap, 1, core.ser_size(), f); fclose(f); boss_found[z] = true; say("BOSS RUSH +1"); }
        boss_snap_zone = -1;
    }
    if (b == 0xff) boss_snap_zone = -1;
    boss_prev = b;
}

static int rush_at, rush_next_in;
static uint32_t rush_frames;
static bool rush_done;
static void open_rush_result(void);
static bool rush_load(int from);

// Called after every frame of play.
static void play_hooks(void)
{
    count_frame();
    map_mark();
    uint8_t *mm = wram();
    if (!cfg.beaten && !scripting && mm && (mm[RAM_ZONE] == 0x0f || mm[RAM_ZONE] == 0x10)) {   // the ending scenes
        cfg.beaten = 1; cfg_save(); say(T("BOSS RUSH DESBLOQUEADO", "BOSS RUSH UNLOCKED"));
    }
    if (!rush) { boss_watch(); return; }
    rush_frames++;
    uint8_t *m = wram();
    if (rush_next_in == 0 && m[RAM_BOSS] == 0) rush_next_in = 150;     // beaten: a moment to see it
    if (rush_next_in && --rush_next_in == 0) {
        if (!rush_load(rush_at + 1)) { rush = false; rush_done = true; open_rush_result(); }
    }
}
static bool rush_load(int from)
{
    for (int z = from; z < NZONES; z++) {
        if (!boss_found[z]) continue;
        char p[1100]; boss_path(p, sizeof p, z);
        size_t n = core.ser_size(), got = 0;
        void *b = malloc(n);
        FILE *f = fopen(p, "rb");
        if (f) { got = fread(b, 1, n, f); fclose(f); }
        bool ok = b && got == n && core.unserialize(b, n);
        free(b);
        if (ok) {                                    // the rush rules, not what the save had
            uint8_t *m = wram();
            m[RAM_MAXHP] = m[RAM_HEALTH] = (uint8_t)cfg.r_hearts;
            m[RAM_SWORD] = (uint8_t)cfg.r_sword;
            m[RAM_GOLF] = (uint8_t)cfg.r_golf;
            m[RAM_LIVES] = (uint8_t)(cfg.r_lives == 99 ? 9 : cfg.r_lives);
            rush_at = z; boss_prev = 0xff;
            return true;
        }
    }
    return false;
}
static const char *fmt_time(uint32_t frames)
{
    static char b[4][16];
    static int k;
    char *o = b[k++ % 4];
    uint32_t cs = (uint32_t)(frames * 100ull * 1000 / 60099), s = cs / 100;   // NTSC: 60.099 fps
    if (s >= 3600) snprintf(o, 16, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(o, 16, "%u:%02u.%u", s / 60, s % 60, cs % 100 / 10);
    return o;
}

/* ------------------------------------------------------------------ */
/* Scripts: the program drives the game's own old menus for the player  */
/* ------------------------------------------------------------------ */

static bool started;                 // a game is in progress (Continue makes sense)
static void set_scripting(bool on) { scripting = on; mute = on; layers_dirty = true; }

static void boot_to_title(void)              // from power-on to the title menu (deterministic)
{
    core.reset();
    run(60 * 12);
    tap(RETRO_DEVICE_ID_JOYPAD_START, 180);
    tap(RETRO_DEVICE_ID_JOYPAD_START, 90);
}

static void script_new_game(void)
{
    set_scripting(true);
    boot_to_title();
    tap(RETRO_DEVICE_ID_JOYPAD_START, 30);       // START GAME
    set_scripting(false);
}

// Types a password on the game's password screen. Returns false if the game
// refused it (it goes back to the title).
static bool script_password(const char *code)
{
    static const char *grid[4] = { "12345678", "9BCDFGHJ", "KLMNPRST", "VWXYZ#?&" };
    static uint32_t title[W * H];
    set_scripting(true);
    boot_to_title();
    for (int y = 0; y < H; y++) memcpy(title + y * W, game + y * gw, W * 4);
    tap(RETRO_DEVICE_ID_JOYPAD_DOWN, 30);
    tap(RETRO_DEVICE_ID_JOYPAD_START, 30);       // PASSWORD
    for (int i = 0; i < 60 && visible(); i++) run(10);
    for (int i = 0; i < 60 && !visible(); i++) run(10);
    run(40);
    int r = 0, c = 0;
    for (const char *p = code; *p; p++) {
        int tr = -1, tc = -1;
        for (int y = 0; y < 4; y++) { const char *q = strchr(grid[y], *p); if (q) { tr = y; tc = (int)(q - grid[y]); } }
        if (tr < 0) continue;
        for (; r < tr; r++) tap(RETRO_DEVICE_ID_JOYPAD_DOWN, 6);
        for (; r > tr; r--) tap(RETRO_DEVICE_ID_JOYPAD_UP, 6);
        for (; c < tc; c++) tap(RETRO_DEVICE_ID_JOYPAD_RIGHT, 6);
        for (; c > tc; c--) tap(RETRO_DEVICE_ID_JOYPAD_LEFT, 6);
        tap(RETRO_DEVICE_ID_JOYPAD_B, 10);
    }
    tap(RETRO_DEVICE_ID_JOYPAD_START, 30);
    for (int i = 0; i < 60 && visible(); i++) run(10);
    for (int i = 0; i < 80 && !visible(); i++) run(10);
    run(60);
    int same = 0;                                // refused codes drop back to the title
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) same += game[y * gw + x] == title[y * W + x];
    bool ok = same < W * H / 2;
    set_scripting(false);
    return ok;
}

// Starts a game directly in a zone, keeping hearts, items, lives and the
// progress table the player had.
static void script_warp(int zone)
{
    uint8_t keep[0x10], hp = 0, maxhp = 0, sword = 0, golf = 0, lives = 0;
    uint8_t *m = wram();
    bool had = started && m && m[RAM_MAXHP] >= 2 && m[RAM_MAXHP] <= 5;
    if (had) {
        memcpy(keep, m + RAM_PROGRESS, sizeof keep);
        hp = m[RAM_HEALTH]; maxhp = m[RAM_MAXHP]; sword = m[RAM_SWORD]; golf = m[RAM_GOLF]; lives = m[RAM_LIVES];
    }
    set_scripting(true);
    boot_to_title();
    pad = 1 << RETRO_DEVICE_ID_JOYPAD_START; run(4); pad = 0;
    for (int i = 0; i < 400; i++) { wram()[RAM_ZONE] = (uint8_t)zone; core.run(); }
    m = wram();
    if (had) {
        memcpy(m + RAM_PROGRESS, keep, sizeof keep);
        m[RAM_MAXHP] = maxhp; m[RAM_HEALTH] = hp ? hp : maxhp; m[RAM_SWORD] = sword; m[RAM_GOLF] = golf; m[RAM_LIVES] = lives;
    }
    set_scripting(false);
}

/* ------------------------------------------------------------------ */
/* Menus                                                                */
/* ------------------------------------------------------------------ */

enum { ST_START, ST_PLAY, ST_PAUSE } state = ST_START;
enum { P_START, P_PAUSE, P_CHEATS, P_OPTIONS, P_CODES, P_CONTROLS, P_LEVELS, P_PROGRESS, P_MAP, P_RUSH } page = P_START;
static int map_zone;
static int sel, from_page;           // from_page: where Back returns to from Options / Codes
static bool quit;
static int binding = -1;             // Controls: row waiting for a key or button
static char toast[64];
static Uint32 toast_until;
static SDL_Window *win;

static void say(const char *s) { snprintf(toast, sizeof toast, "%s", s); toast_until = SDL_GetTicks() + 1800; }

static void state_path(char *out, size_t n) { snprintf(out, n, "%sslot1.state", base); }
static void save_state(void)
{
    size_t n = core.ser_size();
    void *b = malloc(n);
    char p[1100]; state_path(p, sizeof p);
    FILE *f = (b && core.serialize(b, n)) ? fopen(p, "wb") : NULL;
    if (f) { fwrite(b, 1, n, f); fclose(f); say(T("ESTADO GUARDADO", "STATE SAVED")); }
    else say(T("NO SE PUDO GUARDAR", "COULD NOT SAVE"));
    free(b);
}
static void load_state(void)
{
    size_t n = core.ser_size();
    void *b = malloc(n);
    char p[1100]; state_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    bool ok = f && b && fread(b, 1, n, f) == n && core.unserialize(b, n);
    if (f) fclose(f);
    free(b);
    say(ok ? T("ESTADO CARGADO", "STATE LOADED") : T("NO HAY ESTADO GUARDADO", "NO SAVED STATE"));
    if (ok) { lives_hold = 0; started = true; boss_prev = 0xff; }
}

static void set_fullscreen(void) { if (win) SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }

static void open_page(int p) { if (p != P_CHEATS && p != P_CONTROLS) from_page = page; page = p; sel = 0; }
static void resume(void) { state = ST_PLAY; if (audio) SDL_ClearQueuedAudio(audio); }
static void pause_game(void)
{
    if (state != ST_PLAY) return;
    state = ST_PAUSE; page = P_PAUSE; sel = 0;
    if (map_dirty) map_io(true);
}

static const char *codes[][3] = {            // checked by typing each one in; 99966 is refused
    { "11111", "100 VIDAS", "100 LIVES" },
    { "71117", "3 CORAZ+ESPADA", "3 HEARTS+SWORD" },
    { "W111W", "4 CORAZ+ESPADA", "4 HEARTS+SWORD" },
    { "21112", "5 CORAZ+ESPADA", "5 HEARTS+SWORD" },
    { "B211T", "5 CORAZ+GOLF", "5 HEARTS+GOLF" },
    { "BB11L", "5 CORAZONES", "5 HEARTS" },
    { "&191?", "BUDGIE VENCIDO", "BUDGIE BEATEN" },
};
#define NCODES (int)(sizeof codes / sizeof codes[0])

typedef struct { const char *label, *value, *value2; } item_t;

static const char *key_name(int sc)
{
    static char b[12][10];
    static int k;
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    char *o = b[k++ % 12];
    snprintf(o, 9, "%s", *n ? n : "-");
    if (!strncmp(n, "Right ", 6)) snprintf(o, 9, "R%s", n + 6);      // "Right Shift" -> "RShift"
    else if (!strncmp(n, "Left ", 5)) snprintf(o, 9, "L%s", n + 5);
    return o;
}
static const char *pad_name(int b)
{
    static const char *n[] = { "A", "B", "X", "Y", "SELECT", "GUIDE", "START", "L3", "R3", "LB", "RB",
                               "ARRIBA", "ABAJO", "IZQ", "DER" };
    static const char *e[] = { "A", "B", "X", "Y", "SELECT", "GUIDE", "START", "L3", "R3", "LB", "RB",
                               "UP", "DOWN", "LEFT", "RIGHT" };
    if (b < 0 || b > 14) return "-";
    return cfg.lang == 1 ? e[b] : n[b];
}

static int build(item_t *it)
{
    int n = 0;
#define ADD(l, v) (it[n].label = (l), it[n].value = (v), it[n].value2 = NULL, n++)
    const char *yes = T("SI", "ON"), *no = T("NO", "OFF");
    switch (page) {
    case P_START:
        ADD(started ? T("CONTINUAR", "CONTINUE") : T("JUGAR", "PLAY"), NULL);
        if (started) ADD(T("PARTIDA NUEVA", "NEW GAME"), NULL);
        ADD(T("IR A NIVEL", "LEVEL SELECT"), NULL);
        ADD("BOSS RUSH", NULL);
        ADD(T("CODIGOS", "PASSWORDS"), NULL);
        ADD(T("CARGAR ESTADO", "LOAD STATE"), NULL);
        ADD(T("OPCIONES", "OPTIONS"), NULL);
        ADD(T("SALIR", "QUIT"), NULL);
        break;
    case P_PAUSE:
        ADD(T("CONTINUAR", "CONTINUE"), NULL);
        ADD(T("IR A NIVEL", "LEVEL SELECT"), NULL);
        ADD(T("TRUCOS", "CHEATS"), NULL);
        ADD(T("MAPA", "MAP"), NULL);
        ADD(T("PROGRESO", "PROGRESS"), NULL);
        ADD("BOSS RUSH", NULL);
        ADD(T("GUARDAR ESTADO", "SAVE STATE"), NULL);
        ADD(T("CARGAR ESTADO", "LOAD STATE"), NULL);
        ADD(T("CODIGOS", "PASSWORDS"), NULL);
        ADD(T("OPCIONES", "OPTIONS"), NULL);
        ADD(T("MENU DE INICIO", "START MENU"), NULL);
        ADD(T("SALIR", "QUIT"), NULL);
        break;
    case P_CHEATS:
        ADD(T("MODO FACIL", "EASY MODE"), cfg.easy ? yes : no);
        ADD(T("MODO DIOS", "GOD MODE"), cfg.god ? yes : no);
        ADD(T("VIDA INFINITA", "INFINITE HEALTH"), cfg.hp ? yes : no);
        ADD(T("VIDAS INFINITAS", "INFINITE LIVES"), cfg.lives ? yes : no);
        ADD(T("SOMBRERO VOLADOR", "FLYING HAT"), cfg.fez ? yes : no);
        ADD(T("ESPADA", "SWORD"), cfg.sword ? yes : no);
        ADD(T("PELOTAS DE GOLF", "GOLF BALLS"), cfg.golf ? yes : no);
        ADD(T("SIN SLOWDOWN", "NO SLOWDOWN"), cfg.fastcpu ? yes : no);
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_OPTIONS:
        ADD(T("IDIOMA", "LANGUAGE"), cfg.lang == 1 ? "ENGLISH" : "ESPANOL");
        ADD(T("PANTALLA COMPLETA", "FULL SCREEN"), cfg.fullscreen ? yes : no);
        ADD(T("TIMER EN PANTALLA", "ON-SCREEN TIMER"), cfg.timer ? yes : no);
        ADD(T("CONTROLES", "CONTROLS"), NULL);
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_CODES:
        for (int i = 0; i < NCODES; i++) ADD(codes[i][0], codes[i][1 + (cfg.lang == 1)]);
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_CONTROLS:
        for (int i = 0; i < 12; i++) {
            ADD(cfg.lang == 1 ? snes_name_en[i] : snes_name[i], binding == i ? "..." : key_name(cfg.kb[i]));
            it[n - 1].value2 = binding == i ? "..." : pad_name(cfg.gp[i]);
        }
        ADD(T("RESTAURAR", "DEFAULTS"), NULL);
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_LEVELS:
        for (int i = 0; i < NZONES; i++) ADD(zones[i], NULL);
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_PROGRESS: case P_MAP:
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    case P_RUSH: {
        int found = 0;
        static char hearts[4], lives[12];
        for (int z = 0; z < NZONES; z++) found += boss_found[z];
        if (cfg.beaten) {
            snprintf(hearts, sizeof hearts, "%d", cfg.r_hearts);
            if (cfg.r_lives == 99) snprintf(lives, sizeof lives, "%s", T("INFINITAS", "INFINITE"));
            else snprintf(lives, sizeof lives, "%d", cfg.r_lives);
            ADD(T("CORAZONES", "HEARTS"), hearts);
            ADD(T("VIDA INFINITA", "INFINITE HEALTH"), cfg.r_hp ? yes : no);
            ADD(T("VIDAS", "LIVES"), lives);
            ADD(T("ESPADA", "SWORD"), cfg.r_sword ? yes : no);
            ADD(T("PELOTAS DE GOLF", "GOLF BALLS"), cfg.r_golf ? yes : no);
            if (found) ADD(rush_done ? T("OTRA VEZ", "AGAIN") : T("EMPEZAR", "START"), NULL);
        }
        ADD(T("VOLVER", "BACK"), NULL);
        break;
    }
    }
    return n;
#undef ADD
}

static void back(void)
{
    binding = -1;
    if (page == P_PAUSE) resume();
    else if (page == P_CHEATS) { page = P_PAUSE; sel = 2; }
    else if (page == P_CONTROLS) { page = P_OPTIONS; sel = 3; }
    else { page = from_page; sel = 0; }
}

static void activate(void)
{
    item_t it[20];
    int n = build(it);
    if (sel >= n) return;
    const char *l = it[sel].label;
    switch (page) {
    case P_START:
        if (!strcmp(l, T("JUGAR", "PLAY"))) { script_new_game(); started = true; reset_times(); resume(); }
        else if (!strcmp(l, T("CONTINUAR", "CONTINUE"))) resume();
        else if (!strcmp(l, T("PARTIDA NUEVA", "NEW GAME"))) { script_new_game(); reset_times(); lives_hold = 0; resume(); }
        else if (!strcmp(l, T("IR A NIVEL", "LEVEL SELECT"))) open_page(P_LEVELS);
        else if (!strcmp(l, "BOSS RUSH")) { rush_done = false; open_page(P_RUSH); }
        else if (!strcmp(l, T("CODIGOS", "PASSWORDS"))) open_page(P_CODES);
        else if (!strcmp(l, T("CARGAR ESTADO", "LOAD STATE"))) { load_state(); if (started) resume(); }
        else if (!strcmp(l, T("OPCIONES", "OPTIONS"))) open_page(P_OPTIONS);
        else quit = true;
        break;
    case P_PAUSE:
        if (!strcmp(l, T("CONTINUAR", "CONTINUE"))) resume();
        else if (!strcmp(l, T("IR A NIVEL", "LEVEL SELECT"))) open_page(P_LEVELS);
        else if (!strcmp(l, T("TRUCOS", "CHEATS"))) open_page(P_CHEATS);
        else if (!strcmp(l, T("PROGRESO", "PROGRESS"))) open_page(P_PROGRESS);
        else if (!strcmp(l, T("MAPA", "MAP"))) { int z = zone_now(); map_zone = z < 0 ? 0 : z; open_page(P_MAP); }
        else if (!strcmp(l, "BOSS RUSH")) { rush_done = false; open_page(P_RUSH); }
        else if (!strcmp(l, T("GUARDAR ESTADO", "SAVE STATE"))) save_state();
        else if (!strcmp(l, T("CARGAR ESTADO", "LOAD STATE"))) load_state();
        else if (!strcmp(l, T("CODIGOS", "PASSWORDS"))) open_page(P_CODES);
        else if (!strcmp(l, T("OPCIONES", "OPTIONS"))) open_page(P_OPTIONS);
        else if (!strcmp(l, T("MENU DE INICIO", "START MENU"))) { state = ST_START; page = P_START; sel = 0; }
        else quit = true;
        break;
    case P_CHEATS:
        if (sel == 0) { cfg.easy ^= 1; quarters = 0; }
        else if (sel == 1) cfg.god ^= 1;
        else if (sel == 2) cfg.hp ^= 1;
        else if (sel == 3) { cfg.lives ^= 1; lives_hold = 0; }
        else if (sel == 4) cfg.fez ^= 1;
        else if (sel == 5) cfg.sword ^= 1;
        else if (sel == 6) cfg.golf ^= 1;
        else if (sel == 7) { cfg.fastcpu ^= 1; overclock = cfg.fastcpu; layers_dirty = true; }
        else back();
        cfg_save();
        break;
    case P_OPTIONS:
        if (sel == 0) cfg.lang ^= 1;
        else if (sel == 1) { cfg.fullscreen ^= 1; set_fullscreen(); }
        else if (sel == 2) cfg.timer ^= 1;
        else if (sel == 3) { page = P_CONTROLS; sel = 0; }
        else back();
        cfg_save();
        break;
    case P_CODES:
        if (sel < NCODES) {
            if (script_password(codes[sel][0])) {
                started = true; reset_times(); lives_hold = 0; resume();
            } else say(T("EL JUEGO RECHAZO EL CODIGO", "THE GAME REFUSED THE CODE"));
        } else back();
        break;
    case P_LEVELS:
        if (sel < NZONES) { script_warp(sel); if (!started) reset_times(); started = true; lives_hold = 0; resume(); }
        else back();
        break;
    case P_PROGRESS: case P_MAP:
        back();
        break;
    case P_RUSH:
        if (!strcmp(l, T("VOLVER", "BACK"))) { rush_done = false; back(); break; }
        if (!strcmp(l, T("CORAZONES", "HEARTS"))) { cfg.r_hearts = cfg.r_hearts >= 5 ? 2 : cfg.r_hearts + 1; cfg_save(); break; }
        if (!strcmp(l, T("VIDA INFINITA", "INFINITE HEALTH"))) { cfg.r_hp ^= 1; cfg_save(); break; }
        if (!strcmp(l, T("VIDAS", "LIVES"))) {
            cfg.r_lives = cfg.r_lives == 1 ? 3 : cfg.r_lives == 3 ? 5 : cfg.r_lives == 5 ? 9 : cfg.r_lives == 9 ? 99 : 1;
            cfg_save(); break;
        }
        if (!strcmp(l, T("ESPADA", "SWORD"))) { cfg.r_sword ^= 1; cfg_save(); break; }
        if (!strcmp(l, T("PELOTAS DE GOLF", "GOLF BALLS"))) { cfg.r_golf ^= 1; cfg_save(); break; }
        if (rush_load(0)) {
            rush = true; rush_done = false; rush_frames = 0; rush_next_in = 0; started = true;
            from_page = P_START; resume();
        }
        break;
    case P_CONTROLS:
        if (sel < 12) { binding = sel; say(T("PULSA UNA TECLA O BOTON", "PRESS A KEY OR BUTTON")); }
        else if (sel == 12) {
            memcpy(cfg.kb, kb_default, sizeof cfg.kb); memcpy(cfg.gp, gp_default, sizeof cfg.gp);
            cfg_save(); say(T("CONTROLES RESTAURADOS", "CONTROLS RESET"));
        } else back();
        break;
    }
}

static void draw_menu(void)
{
    memcpy(ui, bg, sizeof ui);
    shade(30, 52, 226, 206);
    static const char *title_es[] = { "LA MANSION", "PAUSA", "TRUCOS", "OPCIONES", "CODIGOS", "CONTROLES",
                                      "IR A NIVEL", "PROGRESO", "MAPA", "BOSS RUSH" };
    static const char *title_en[] = { "THE MANSION", "PAUSED", "CHEATS", "OPTIONS", "PASSWORDS", "CONTROLS",
                                      "LEVEL SELECT", "PROGRESS", "MAP", "BOSS RUSH" };
    text_c(30, cfg.lang == 1 ? title_en[page] : title_es[page], RED);

    item_t it[20];
    int n = build(it);
    int x = 60, step = 14, y0 = 60, vx = 212;
    if (page == P_PAUSE) { step = 11; y0 = 58; }
    if (page == P_CHEATS) { x = 52; step = 14; y0 = 58; }
    int first = 0, rows = n;                 // long lists scroll
    if (page == P_LEVELS) {
        x = 60; step = 13; y0 = 58; rows = 10;
        first = sel - rows / 2;
        if (first > n - rows) first = n - rows;
        if (first < 0) first = 0;
        if (first > 0) text_c(48, "...", DIM);
        if (first + rows < n) text_c(56 + rows * 13, "...", DIM);
    }
    if (page == P_MAP) {
        int z = map_zone, x0 = MAP_W, y0b = MAP_H, x1 = -1, y1 = -1;
        for (int y = 0; y < MAP_H; y++)
            for (int x = 0; x < MAP_W; x++)
                if (map_has(z, x, y)) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0b) y0b = y; if (y > y1) y1 = y; }
        text_c(46, zones[z], WHITE);
        const int AX = 38, AY = 60, AW = 180, AH = 104;     // drawing area inside the frame
        if (x1 < 0) text_c(112, T("SIN EXPLORAR", "NOT EXPLORED"), DIM);
        else {
            int w = x1 - x0 + 1, h = y1 - y0b + 1, dw = w, dh = h;
            if (w <= AW && h <= AH) {                   // grow by whole pixels...
                int sc = AW / w < AH / h ? AW / w : AH / h;
                if (sc > 6) sc = 6;
                dw = w * sc; dh = h * sc;
            } else {                                    // ...or shrink by sampling
                if (w > AW) dw = AW;
                if (h > AH) dh = AH;
            }
            int ox = AX + (AW - dw) / 2, oy = AY + (AH - dh) / 2;
            for (int py = 0; py < dh; py++)
                for (int px = 0; px < dw; px++)
                    if (map_has(z, x0 + px * w / dw, y0b + py * h / dh)) ui[(oy + py) * W + ox + px] = 0xffb9a477;
            uint8_t *m = wram();
            if (m && zone_now() == z && (SDL_GetTicks() / 300) % 2) {   // Gomez, blinking
                int gx = ox + (m[RAM_X] - x0) * dw / w + dw / w / 2, gy = oy + (m[RAM_Y] - y0b) * dh / h + dh / h / 2;
                for (int yy = -2; yy <= 2; yy++)
                    for (int xx = -2; xx <= 2; xx++) {
                        int px = gx + xx, py = gy + yy;
                        if (px >= AX && px < AX + AW && py >= AY && py < AY + AH) ui[py * W + px] = 0xffe60000;
                    }
            }
        }
        y0 = 188; x = 60;
    }
    if (page == P_RUSH) {
        int found = 0;
        for (int z = 0; z < NZONES; z++) found += boss_found[z];
        char line[32]; snprintf(line, sizeof line, "%s %d", T("JEFES ENCONTRADOS", "BOSSES FOUND"), found);
        if (!cfg.beaten) {
            text_c(64, T("BLOQUEADO", "LOCKED"), GOLD);
            text_c(82, T("SE DESBLOQUEA AL", "UNLOCKS WHEN YOU"), WHITE);
            text_c(93, T("TERMINAR EL JUEGO", "FINISH THE GAME"), WHITE);
            text_c(114, line, DIM);
            text_c(128, T("CADA JEFE SE GUARDA", "EACH BOSS IS SAVED"), DIM);
            text_c(139, T("SOLO AL LLEGAR A EL", "WHEN YOU REACH IT"), DIM);
            y0 = 166;
        } else {
            if (rush_done) {
                text(52, 58, T("TERMINADO", "FINISHED"), GOLD);
                text_r(212, 58, fmt_time(rush_frames), GOLD);
            } else text_c(58, line, DIM);
            x = 52; step = 14; y0 = 76;
        }
    }
    if (page == P_PROGRESS) {
        uint8_t *m = wram();
        int fam = 0, n = 0;
        if (m && started) fam = (m[RAM_PROGRESS + 7] & 1) + (m[RAM_PROGRESS + 9] & 1) + (m[RAM_PROGRESS + 11] & 1);
        char line[40];
        text(44, 58, T("TIEMPO", "TIME"), WHITE); text_r(212, 58, fmt_time(total_frames), GOLD);
        snprintf(line, sizeof line, "%d %s 3", fam, T("DE", "OF"));
        text(44, 70, T("FAMILIA", "FAMILY"), WHITE); text_r(212, 70, line, GOLD);
        snprintf(line, sizeof line, "%d %s 5", m && started ? m[RAM_MAXHP] : 0, T("DE", "OF"));
        text(44, 82, T("CORAZONES", "HEARTS"), WHITE); text_r(212, 82, line, GOLD);
        text(44, 98, T("TIEMPO POR ZONA", "TIME PER ZONE"), DIM);
        for (int z = 0; z < NZONES && n < 7; z++) {
            if (zone_frames[z] < 60) continue;
            const char *zn = strncmp(zones[z], "THE ", 4) ? zones[z] : zones[z] + 4;
            text(36, 110 + n * 10, zn, WHITE); text_r(216, 110 + n * 10, fmt_time(zone_frames[z]), WHITE);
            n++;
        }
        if (!n) text(44, 110, T("TODAVIA NADA", "NOTHING YET"), DIM);
        y0 = 184;
    }
    if (page == P_OPTIONS) { x = 52; step = 18; }
    if (page == P_CODES) { x = 48; step = 14; y0 = 58; }
    if (page == P_CONTROLS) { x = 42; step = 10; y0 = 68; vx = 160; }
    if (page == P_CONTROLS) { text_r(160, 56, T("TECLADO", "KEYS"), DIM); text(170, 56, T("MANDO", "PAD"), DIM); }
    for (int i = first; i < n && i < first + rows; i++) {
        int y = y0 + (i - first) * step;
        bool on = i == sel;
        bool plain = page == P_START || page == P_PAUSE;
        text(x, y, it[i].label, on ? GOLD : (plain ? DIM : WHITE));
        if (it[i].value) text_r(vx, y, it[i].value, page == P_CODES ? DIM : WHITE);
        if (it[i].value2) text(170, y, it[i].value2, WHITE);
        if (on) draw_cursor(x - curw - 3, y - (curh - 8) / 2);
    }

    char foot[48] = "";
    if (page == P_CHEATS) snprintf(foot, sizeof foot, "%s", T("SE APLICA SIN REINICIAR", "APPLIES WITHOUT RESTART"));
    else if (page == P_PAUSE) {
        ;
    } else if (page == P_START) snprintf(foot, sizeof foot, "%s", T("F1 O ESC: PAUSA", "F1 OR ESC: PAUSE"));
    else if (page == P_CODES) snprintf(foot, sizeof foot, "%s", T("SE ESCRIBE SOLO", "ENTERED FOR YOU"));
    else if (page == P_LEVELS) snprintf(foot, sizeof foot, "%s", T("CONSERVA CORAZONES Y OBJETOS", "KEEPS HEARTS AND ITEMS"));
    else if (page == P_MAP) snprintf(foot, sizeof foot, "%s", T("IZQ Y DER: OTRA ZONA", "LEFT RIGHT: OTHER ZONE"));
    if (*foot && page != P_CONTROLS && page != P_LEVELS) text_c(page == P_PAUSE ? 196 : page == P_MAP ? 172 : 190, foot, DIM);
}

static void open_rush_result(void)
{
    state = ST_PAUSE; page = P_RUSH; from_page = P_START; sel = 0;
}

static void draw_toast(void)
{
    if (!*toast || SDL_TICKS_PASSED(SDL_GetTicks(), toast_until)) { *toast = 0; return; }
    int w = (int)strlen(toast) * 8 + 8;
    for (int y = 6; y < 20; y++)
        for (int x = W / 2 - w / 2; x < W / 2 + w / 2; x++) if (x >= 0 && x < W) ui[y * W + x] = 0xe0000000;
    text_c(9, toast, WHITE);
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

static SDL_GameController *pads[4];

static uint16_t read_pad(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    for (int i = 0; i < 12; i++)
        if (cfg.kb[i] > 0 && cfg.kb[i] < SDL_NUM_SCANCODES && k[cfg.kb[i]]) b |= 1 << snes_btn[i];
    for (int p = 0; p < 4; p++) {
        SDL_GameController *c = pads[p];
        if (!c) continue;
        for (int i = 0; i < 12; i++)
            if (cfg.gp[i] >= 0 && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)cfg.gp[i])) b |= 1 << snes_btn[i];
        Sint16 ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
        if (ax > 16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (ay < -16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
        if (ay > 16000) b |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
    }
    return b;
}

enum { M_NONE, M_UP, M_DOWN, M_LEFT, M_RIGHT, M_OK, M_BACK, M_PAUSE };

static void menu_input(int m)
{
    item_t it[20];
    int n = build(it);
    if (page == P_MAP && (m == M_LEFT || m == M_RIGHT)) {
        for (int i = 1; i <= NZONES; i++) {             // next zone with something explored
            int z = (map_zone + (m == M_RIGHT ? i : NZONES - i)) % NZONES;
            for (int y = 0; y < MAP_H; y++) for (int x = 0; x < MAP_W / 8; x++) if (seen[z][y][x]) { map_zone = z; return; }
        }
        return;
    }
    if (m == M_UP) sel = (sel + n - 1) % n;
    else if (m == M_DOWN) sel = (sel + 1) % n;
    else if (m == M_OK || ((m == M_LEFT || m == M_RIGHT) && it[sel].value && page != P_CODES && page != P_CONTROLS)) activate();
    else if (m == M_BACK) back();
}

static int key_action(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return M_UP;
    case SDLK_DOWN: return M_DOWN;
    case SDLK_LEFT: return M_LEFT;
    case SDLK_RIGHT: return M_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: case SDLK_z: case SDLK_SPACE: return M_OK;
    case SDLK_BACKSPACE: return M_BACK;
    case SDLK_ESCAPE: case SDLK_F1: case SDLK_AC_BACK: return M_PAUSE;
    }
    return M_NONE;
}

static int pad_action(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return M_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return M_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return M_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return M_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: return M_OK;
    case SDL_CONTROLLER_BUTTON_B: return M_BACK;
    case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_GUIDE: return M_PAUSE;
    }
    return M_NONE;
}

static void handle(int m)
{
    if (m == M_NONE) return;
    if (state == ST_PLAY) { if (m == M_PAUSE) pause_game(); return; }
    if (m == M_PAUSE) {                          // pause key in a menu: close it / go back
        if (page == P_START) return;
        back();
        return;
    }
    menu_input(m);
}

// A key or button pressed while a Controls row waits for one.
static void bind_key(int sc)
{
    if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F1) { binding = -1; *toast = 0; return; }
    for (int i = 0; i < 12; i++) if (cfg.kb[i] == sc) cfg.kb[i] = cfg.kb[binding];   // swap, no duplicates
    cfg.kb[binding] = sc;
    binding = -1; *toast = 0; cfg_save();
}
static void bind_pad(int b)
{
    if (b == SDL_CONTROLLER_BUTTON_GUIDE) { binding = -1; *toast = 0; return; }
    for (int i = 0; i < 12; i++) if (cfg.gp[i] == b) cfg.gp[i] = cfg.gp[binding];
    cfg.gp[binding] = b;
    binding = -1; *toast = 0; cfg_save();
}
/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *d++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}

static void fatal(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mansion", msg, NULL);
    exit(1);
}

static void *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n);
    if (b && fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void save_shot(const char *path)   // the 256x224 picture the player sees
{
    static uint32_t out[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t u = ui[y * W + x], g = game[(y * gh / H) * gw + x * gw / W];
            unsigned a = u >> 24;
            uint32_t r = ((u >> 16 & 255) * a + (g >> 16 & 255) * (255 - a)) / 255;
            uint32_t gg = ((u >> 8 & 255) * a + (g >> 8 & 255) * (255 - a)) / 255;
            uint32_t b = ((u & 255) * a + (g & 255) * (255 - a)) / 255;
            out[y * W + x] = 0xff000000u | r << 16 | gg << 8 | b;
        }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(out, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

int main(int argc, char **argv)
{
    const char *rom_arg = NULL, *core_arg = NULL, *test_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_arg = argv[++i];
        else if (!strcmp(argv[i], "--test") && i + 1 < argc) test_dir = argv[++i];
        else rom_arg = argv[i];
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) fatal(SDL_GetError());
    char *bp = SDL_GetBasePath();
    snprintf(base, sizeof base, "%s", bp ? bp : "./");
    SDL_free(bp);
    cfg_load();
    guess_language();
    overclock = cfg.fastcpu;
    map_io(false);
    boss_scan();

    char path[1100];
    snprintf(path, sizeof path, "%s%s", base, CORE_NAME);
    if (!load_core(core_arg ? core_arg : path))
        fatal(T("Falta " CORE_NAME " junto al programa.", CORE_NAME " is missing next to the program."));

    static const char *names[] = { "Addams Family, The (USA).sfc", "addams.sfc", "Addams Family, The (USA).smc" };
    size_t rom_n = 0;
    void *rom = NULL;
    if (rom_arg) rom = read_file(rom_arg, &rom_n);
    for (int i = 0; !rom && i < 3; i++) { snprintf(path, sizeof path, "%s%s", base, names[i]); rom = read_file(path, &rom_n); }
    if (!rom) fatal(T("No encuentro la ROM. Pon \"Addams Family, The (USA).sfc\" junto al programa o arrastrala sobre el.",
                      "ROM not found. Put \"Addams Family, The (USA).sfc\" next to the program or drop it on it."));
    bool rom_ok = rom_n == 0x100000 && crc32(rom, rom_n) == ROM_CRC;

    struct retro_game_info gi = { "addams.sfc", rom, rom_n, NULL };
    if (!core.load_game(&gi)) fatal(T("El nucleo no pudo cargar la ROM.", "The core could not load the ROM."));
    struct retro_system_av_info av;
    core.get_av(&av);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = (int)(av.timing.sample_rate + 0.5);
    want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    if (!test_dir) audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    if (!rom_ok || !extract_art())
        fatal(T("Esta ROM no es The Addams Family (USA) sin modificar; no puedo sacar el arte de los menus.",
                "This ROM is not an unmodified The Addams Family (USA); the menu art can't be taken from it."));

    if (test_dir) {                          // pictures of every screen, for checking without a display
        char p[1200];
#define SHOT(name) (snprintf(p, sizeof p, "%s/" name ".bmp", test_dir), draw_menu(), save_shot(p))
        SHOT("01_start");
        script_new_game(); started = true; state = ST_PLAY; run(120);
        uint8_t *m = wram();
        cfg.hp = 1; m[RAM_HEALTH] = 1;
        for (int i = 0; i < 5; i++) { cheats(); core.run(); cheats(); count_frame(); }
        printf("infinite health after a hit: %d of %d\n", m[RAM_HEALTH], m[RAM_MAXHP]);
        cfg.hp = 0;
        // god mode: walk around the Conservatory (enemies) for a minute
        script_warp(3); m = wram();
        cfg.god = 1;
        int hp0 = m[RAM_HEALTH], lowest = hp0;
        for (int f = 0; f < 3600; f++) {
            static const int moves[6] = { 1 << 7, 1 << 6, (1 << 7) | 1, (1 << 6) | 1, 1, 0 };
            if (f % 40 == 0) pad = (uint16_t)moves[(f / 40 * 7) % 6];
            cheats(); core.run(); cheats(); play_hooks();
            if (m[RAM_HEALTH] < lowest) lowest = m[RAM_HEALTH];
        }
        pad = 0;
        printf("god mode, 1 minute in the Conservatory: health %d -> lowest %d\n", hp0, lowest);
        cfg.god = 0;
        for (int easy = 0; easy < 2; easy++) {   // the same walk with and without easy mode
            script_warp(3); m = wram();
            cfg.easy = easy; quarters = 0; m[RAM_LIVES] = 5;
            int lives0 = m[RAM_LIVES], hearts_lost = 0, hits = 0, maxq = 0;
            for (int f = 0; f < 3600; f++) {
                static const int moves[6] = { 1 << 7, 1 << 6, (1 << 7) | 1, (1 << 6) | 1, 1, 0 };
                if (f % 40 == 0) pad = (uint16_t)moves[(f / 40 * 7) % 6];
                int hp = easy ? easy_hearts : m[RAM_HEALTH], q = quarters, lv = m[RAM_LIVES];
                frame_easy(); play_hooks(); m = wram();
                if ((easy ? easy_hearts : m[RAM_HEALTH]) < hp || m[RAM_LIVES] < lv) hearts_lost++;
                if (quarters != q && quarters) hits++;
                if (quarters > maxq) maxq = quarters;
                if (easy && f == 3599) {}
            }
            pad = 0;
            printf("easy %d: quarter hits %d, hearts lost %d, lives %d -> %d, health %d/%d\n", easy, hits, hearts_lost,
                   lives0, m[RAM_LIVES], m[RAM_HEALTH], m[RAM_MAXHP]);
        }
        script_warp(3); run(60);
        // easy mode with 2 hearts, the second one half used, then a heart picked up while "full"
        cfg.easy = 1; state = ST_PLAY; easy_sync = true; m = wram(); m[RAM_MAXHP] = 2; m[RAM_HEALTH] = 2;
        frame_easy(); easy_hearts = 2; quarters = 2;
        for (int i = 0; i < 3; i++) frame_easy();
        m = wram();
        printf("easy, 2 hearts with half of one used: game shows %d\n", m[RAM_HEALTH]);
        memset(ui, 0, sizeof ui); draw_easy_heart();
        snprintf(p, sizeof p, "%s/02b_easy_hud.bmp", test_dir); save_shot(p);
        easy_test_pickup = 1;                // as if a heart were picked up
        frame_easy(); m = wram();
        printf("heart picked up: quarters %d, hearts %d, game shows %d\n", quarters, easy_hearts, m[RAM_HEALTH]);
        cfg.easy = 0; quarters = 0;
        cfg.timer = 1; state = ST_PLAY;
        memset(ui, 0, sizeof ui);
        { const char *t = fmt_time(total_frames); int w = (int)strlen(t) * 8 + 4;
          for (int y = 2; y < 13; y++) for (int x = W - 4 - w; x < W - 4; x++) ui[y * W + x] = 0xa0000000;
          text_r(W - 6, 4, t, WHITE); }
        snprintf(p, sizeof p, "%s/02_game_timer.bmp", test_dir); save_shot(p);
        pause_game();
        SHOT("03_pause");
        open_page(P_CHEATS); SHOT("04_cheats");
        page = P_PAUSE; open_page(P_LEVELS); SHOT("05_levels");
        page = P_PAUSE; open_page(P_PROGRESS); SHOT("06_progress");
        page = P_PAUSE; open_page(P_OPTIONS); SHOT("07_options");
        map_zone = 3; page = P_PAUSE; open_page(P_MAP); SDL_Delay(300); SHOT("10_map");
        cfg.beaten = 0;
        page = P_PAUSE; open_page(P_RUSH); SHOT("11_rush_locked");
        state = ST_PLAY; m = wram();
        // a pretend boss (the real F6 behaviour still needs a real fight)
        m[RAM_BOSS] = 0x20; for (int i = 0; i < 70; i++) { core.run(); m[RAM_BOSS] = 0x20; play_hooks(); }
        printf("pretend boss in %s saved: %s\n", zone_name(), boss_found[3] ? "yes" : "no");
        for (int i = 0; i < 3; i++) { core.run(); m[RAM_BOSS] = 0xff; m[RAM_ZONE] = 0x0f; play_hooks(); }
        printf("ending reached -> unlocked: %d\n", cfg.beaten);
        script_warp(3);
        cfg.r_hearts = 3; cfg.r_sword = 0; cfg.r_golf = 1; cfg.r_lives = 99;
        page = P_PAUSE; open_page(P_RUSH); SHOT("12_rush_rules");
        sel = 5; activate();
        m = wram();
        printf("rush running: %d at zone %02x, hearts %d/%d sword %d golf %d lives %d\n", rush, rush_at,
               m[RAM_HEALTH], m[RAM_MAXHP], m[RAM_SWORD], m[RAM_GOLF], m[RAM_LIVES]);
        for (int i = 0; i < 200; i++) { m[RAM_BOSS] = 0; core.run(); m[RAM_BOSS] = 0; cheats(); play_hooks(); }
        printf("after boss beaten: rush %d done %d time %s\n", rush, rush_done, fmt_time(rush_frames));
        SHOT("13_rush_result");
        bool ok = script_password("BB11L");
        reset_times();
        script_warp(13);                     // keep the family, hearts, items
        m = wram();
        printf("password BB11L %s; then warp to %s: hearts %d/%d family %d%d%d\n", ok ? "ok" : "refused",
               zone_name(), m[RAM_HEALTH], m[RAM_MAXHP], m[RAM_PROGRESS + 7], m[RAM_PROGRESS + 9], m[RAM_PROGRESS + 11]);
        for (int i = 0; i < 300; i++) { core.run(); count_frame(); }
        memset(ui, 0, sizeof ui);
        snprintf(p, sizeof p, "%s/08_after_warp.bmp", test_dir); save_shot(p);
        page = P_PAUSE; open_page(P_PROGRESS); SHOT("09_progress_family");
        return 0;
    }

    win = SDL_CreateWindow("Mansion", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) fatal(SDL_GetError());
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, 1024, 768);          // 4:3, like a TV
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Texture *tg = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 512, 512);
    SDL_Texture *tu = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureBlendMode(tu, SDL_BLENDMODE_BLEND);
    set_fullscreen();
    if (audio) SDL_PauseAudioDevice(audio, 0);

    double frame_ms = 1000.0 / av.timing.fps;
    Uint64 next = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    Uint32 audio_target = (Uint32)(want.freq * 4 * 3 / 60);  // keep ~3 frames of sound queued

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat && binding >= 0) bind_key(e.key.keysym.scancode);
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && binding >= 0) bind_pad(e.cbutton.button);
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    cfg.fullscreen ^= 1; set_fullscreen(); cfg_save();
                } else if (k == SDLK_F5 && state == ST_PLAY) save_state();
                else if (k == SDLK_F7 && state == ST_PLAY) load_state();
                else handle(key_action(k));
            } else if (e.type == SDL_KEYDOWN && e.key.repeat && state != ST_PLAY) {
                int m = key_action(e.key.keysym.sym);
                if (m == M_UP || m == M_DOWN) handle(m);
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e.cdevice.which); break; }
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (int i = 0; i < 4; i++)
                    if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which) {
                        SDL_GameControllerClose(pads[i]); pads[i] = NULL;
                    }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = e.cbutton.button;
                if (state == ST_PLAY && (b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK)) load_state();
                else handle(pad_action(b));
            } else if (e.type == SDL_CONTROLLERAXISMOTION && state == ST_PLAY &&
                       (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
                static bool down[2];         // LT / RT save, once per press
                int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
                if (e.caxis.value > 20000 && !down[t]) { down[t] = true; save_state(); }
                else if (e.caxis.value < 8000) down[t] = false;
            }
        }
        // Select + Start together also pauses (for pads without a Back button)
        uint16_t held = read_pad();
        bool fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] != 0;
        if (state == ST_PLAY && (held & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)) && (held & (1 << RETRO_DEVICE_ID_JOYPAD_START)))
            pause_game();

        if (state == ST_PLAY) {
            pad = held;
            mute = fast;
            for (int i = 0; i < (fast ? 4 : 1); i++) { frame_easy(); play_hooks(); }
            mute = false;
            memset(ui, 0, sizeof ui);
            uint8_t *mq = wram();
            if (mq) draw_easy_heart();
            if (cfg.timer) {                     // top right, clear of the game's own HUD
                const char *t = fmt_time(total_frames);
                int w = (int)strlen(t) * 8 + 4;
                for (int y = 2; y < 13; y++) for (int x = W - 4 - w; x < W - 4; x++) ui[y * W + x] = 0xa0000000;
                text_r(W - 6, 4, t, WHITE);
            }
        } else {
            pad = 0;
            draw_menu();
        }
        draw_toast();

        SDL_UpdateTexture(tg, NULL, game, gw * 4);
        SDL_UpdateTexture(tu, NULL, ui, W * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_Rect src = { 0, 0, gw, gh };
        SDL_RenderCopy(ren, tg, &src, NULL);
        SDL_RenderCopy(ren, tu, NULL, NULL);
        SDL_RenderPresent(ren);

        // pacing: the sound queue when playing, the clock otherwise
        if (state == ST_PLAY && audio && !fast) {
            while (SDL_GetQueuedAudioSize(audio) > audio_target) SDL_Delay(1);
            next = SDL_GetPerformanceCounter();
        } else {
            if (audio && state != ST_PLAY) SDL_ClearQueuedAudio(audio);
            next += (Uint64)(frame_ms * freq / 1000.0);
            Uint64 now = SDL_GetPerformanceCounter();
            if (next > now) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else next = now;
        }
    }
    cfg_save();
    map_io(true);
    SDL_Quit();
    return 0;
}
