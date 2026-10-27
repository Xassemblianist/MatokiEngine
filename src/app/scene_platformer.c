/* app/scene_platformer.c — "Matoki Koşusu": küçük platform oyunu (M7_3).
 * Motorun 2D temellerini birlikte kullanır: Tilemap + çarpışma, Camera2D (ölü bölge, piksel hizalama,
 * paralaks), AnimPlayer, prosedürel pixel-art atlas, sprite.mtk JIT shader'ı ve C synth (efekt + müzik).
 * Fizik sabit 1/120 s adımla ilerler (kare hızından bağımsız, deterministik).
 */
#include "app/scene_platformer.h"
#include "app/plat_art.h"
#include "core/math.h"
#include "core/log.h"
#include "core/rng.h"
#include "gfx2d/batch2d.h"
#include "gfx2d/tilemap.h"
#include "gfx2d/camera2d.h"
#include "gfx2d/anim.h"
#include "audio/audio.h"
#include "ui/font.h"

#define TS          16.0f
#define LEVEL_W     128
#define LEVEL_H     20
#define MAX_COINS   64
#define MAX_SLIMES  16
#define MAX_PARTS   256
#define STEP        (1.0f / 120.0f)
#define TOP_BAR     20

/* Oyuncu fiziği (piksel, saniye) */
#define RUN_SPEED     150.0f
#define ACCEL_GROUND  1500.0f
#define ACCEL_AIR     950.0f
#define FRICTION      1800.0f
#define GRAVITY       980.0f
#define MAX_FALL      420.0f
#define JUMP_SPEED    395.0f   /* tepe ≈ 80 px = 5 karo (seviye tasarımı buna göre) */
#define JUMP_CUT      130.0f
#define COYOTE_TIME   0.09f
#define JUMP_BUFFER   0.12f
#define STOMP_BOUNCE  240.0f

/* Tuş kodları */
#define K_LEFT  37
#define K_UP    38
#define K_RIGHT 39
#define K_DOWN  40
#define K_SPACE 32
#define K_A     65
#define K_D     68
#define K_N     78
#define K_R     82
#define K_S     83
#define K_W     87
#define K_Z     90

static const char* k_level =
    "................................................................................................................................\n"
    "................................................................................................................................\n"
    "................................................................................................................................\n"
    "................................................................................................................................\n"
    "................................................................................................................................\n"
    "..........................................................................................................oo....................\n"
    ".........................o...............................o......................................................................\n"
    ".........................................ooo..................................................oo..........BB....................\n"
    "........................BBB..oo.........................BBB........oo..........................................ooo..............\n"
    "...................ooo..................=====............................................o...====...............................\n"
    "............................===.................o.............o...====...o...........................o........=====.............\n"
    "..................====.............o.................o.................................oSSS.....................................\n"
    "..............##...............................===............BB........===.............SSS.........===.........................\n"
    ".....ooo......##.................SSSS...............==........BB...............o......SSSSS.............................oo......\n"
    "............####.................SSSS.......................BBBB......................SSSSS.....................................\n"
    ".*.P,...f*.,####.*..f......,.*ef.SSSS,e.^^..*............*e.BBBB,.e..........*.,^^e.SSSSSSS.*...e.,......*..e.,...e.*.^...,.F.f.\n"
    "#######################...####################....##...################.....########################....########################\n"
    "#######################...####################....##...################.....########################....########################\n"
    "#######################...####################....##...################.....########################....########################\n"
    "#######################...####################^^^^##...################.....########################....########################\n";

/* Karo tanımları (indeks = TileDef sırası) */
enum { T_EMPTY, T_GRASS, T_DIRT, T_BRICK, T_STONE, T_PLAT, T_SPIKES, T_BUSH, T_TUFT, T_FLOWER, T_POLE, T_FLAGTOP, T_COUNT };
static const TileDef k_defs[T_COUNT] = {
    { 0, 0 },
    { ART_GRASS, TILE_SOLID },
    { ART_DIRT, TILE_SOLID },
    { ART_BRICK, TILE_SOLID },
    { ART_STONE, TILE_SOLID },
    { ART_PLATFORM, TILE_ONEWAY },
    { ART_SPIKES, TILE_HAZARD },
    { ART_BUSH, 0 },
    { ART_TUFT, 0 },
    { ART_FLOWER, 0 },
    { ART_FLAG_POLE, 0 },
    { ART_FLAG_TOP, 0 },
};

static const u16 k_idle_frames[2] = { ART_PLAYER_IDLE0, ART_PLAYER_IDLE0 + 1 };
static const u16 k_run_frames[4]  = { ART_PLAYER_RUN0, ART_PLAYER_RUN0 + 1, ART_PLAYER_RUN0 + 2, ART_PLAYER_RUN0 + 3 };
static const u16 k_jump_frames[1] = { ART_PLAYER_JUMP };
static const u16 k_fall_frames[1] = { ART_PLAYER_FALL };
static const u16 k_hurt_frames[1] = { ART_PLAYER_HURT };
static const u16 k_slime_frames[2] = { ART_SLIME0, ART_SLIME0 + 1 };
static const AnimClip k_idle  = { k_idle_frames, 2, 2.0f, TRUE };
static const AnimClip k_run   = { k_run_frames, 4, 12.0f, TRUE };
static const AnimClip k_jump  = { k_jump_frames, 1, 1.0f, FALSE };
static const AnimClip k_fall  = { k_fall_frames, 1, 1.0f, FALSE };
static const AnimClip k_hurt  = { k_hurt_frames, 1, 1.0f, FALSE };
static const AnimClip k_slime = { k_slime_frames, 2, 4.0f, TRUE };

/* Ses efektleri */
static const SynthSfx k_sfx_jump  = { WAVE_SQUARE, 330.0f, 720.0f, 0.25f, 0.002f, 0.05f, 0.4f, 0.03f, 0.06f, 0.30f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_coin1 = { WAVE_SQUARE, 988.0f, 0.0f, 0.5f, 0.001f, 0.02f, 0.6f, 0.04f, 0.05f, 0.25f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_coin2 = { WAVE_SQUARE, 1319.0f, 0.0f, 0.5f, 0.001f, 0.03f, 0.6f, 0.08f, 0.12f, 0.25f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_stomp = { WAVE_NOISE, 1800.0f, 300.0f, 0.0f, 0.001f, 0.08f, 0.2f, 0.0f, 0.08f, 0.45f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_boing = { WAVE_TRIANGLE, 180.0f, 420.0f, 0.0f, 0.001f, 0.06f, 0.5f, 0.02f, 0.08f, 0.45f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_hurt  = { WAVE_SAW, 440.0f, 110.0f, 0.0f, 0.001f, 0.10f, 0.5f, 0.10f, 0.15f, 0.35f, 8.0f, 0.6f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_land  = { WAVE_NOISE, 500.0f, 200.0f, 0.0f, 0.001f, 0.04f, 0.0f, 0.0f, 0.01f, 0.15f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_win   = { WAVE_SQUARE, 523.0f, 1047.0f, 0.5f, 0.01f, 0.2f, 0.7f, 0.5f, 0.4f, 0.35f, 6.0f, 0.3f , 0.0f, 0.0f, 0.0f};

typedef struct { Vec2 pos, vel; f32 dir; b32 alive; f32 flat_t; AnimPlayer anim; } Slime;
typedef struct { Vec2 pos; b32 taken; } Coin;
typedef struct { Vec2 pos, vel; f32 life, max_life; Vec4 color; f32 size; } Particle;

typedef enum { PS_PLAYING, PS_WON, PS_GAME_OVER } PlatState;

typedef struct {
    Vec2 pos, vel;
    Vec2 size;
    b32  on_ground;
    f32  coyote, jump_buf, drop_t, hurt_t, invuln_t;
    f32  facing;
    b32  jump_held;
    AnimPlayer anim;
} Player;

static PlatArt   g_art;
static Tilemap   g_map;
static Camera2D  g_cam;
static Player    g_pl;
static Coin      g_coins[MAX_COINS];
static u32       g_coin_count, g_coins_taken;
static Slime     g_slimes[MAX_SLIMES];
static u32       g_slime_count;
static Particle  g_parts[MAX_PARTS];
static Vec2      g_spawn;
static f32       g_goal_x;
static u32       g_lives;
static f32       g_time, g_acc, g_anim_t;
static PlatState g_state;
static Rng       g_rng;
static b32       g_music_on, g_music_playing;
static u8        g_prev[256];
static b32       g_inited;

static b32 pressed(const InputState* in, u32 k) { return in->keys[k] && !g_prev[k]; }

static void sfx(const SynthSfx* s) { synth_play(audio_synth(), s); }

static void spawn_parts(Vec2 p, u32 n, Vec4 color, f32 speed, f32 up) {
    for (u32 k = 0; k < n; k++) {
        for (u32 i = 0; i < MAX_PARTS; i++) {
            Particle* q = &g_parts[i];
            if (q->life > 0.0f) continue;
            f32 a = rng_f32(&g_rng) * M_TAU;
            f32 s = speed * (0.4f + 0.6f * rng_f32(&g_rng));
            q->pos = p;
            q->vel = vec2(m_cos(a) * s, m_sin(a) * s - up);
            q->max_life = q->life = 0.35f + 0.3f * rng_f32(&g_rng);
            q->color = color;
            q->size = 2.0f + rng_f32(&g_rng) * 2.0f;
            break;
        }
    }
}

static void start_music(void) {
    Synth* s = audio_synth();
    synth_song_stop(s);
    s->song.track_count = 0;
    SynthSfx lead = { WAVE_SQUARE, 0.0f, 0.0f, 0.25f, 0.005f, 0.06f, 0.5f, 0.05f, 0.06f, 0.10f, 5.0f, 0.12f , 0.0f, 0.0f, 0.0f};
    SynthSfx bass = { WAVE_TRIANGLE, 0.0f, 0.0f, 0.0f, 0.005f, 0.05f, 0.8f, 0.18f, 0.05f, 0.22f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
    SynthSfx hat  = { WAVE_NOISE, 0.0f, 0.0f, 0.0f, 0.001f, 0.02f, 0.0f, 0.0f, 0.01f, 0.05f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
    synth_song_add_track(s,
        "E5 - G5 - C6 - G5 - E5 - D5 - C5 - - - A4 - C5 - E5 - D5 - C5 - A4 - G4 - - - "
        "E5 - G5 - C6 - D6 - E6 - D6 - C6 - - - A5 - G5 - E5 - D5 - E5 - D5 - C5 - - -", &lead);
    synth_song_add_track(s,
        "C3 - - - G2 - - - A2 - - - F2 - - - C3 - - - G2 - - - F2 - - - G2 - - -", &bass);
    synth_song_add_track(s, "C8 - C8 C8 - C8 - C8", &hat);
    synth_song_start(s, 132.0f, 4);
    s->song.volume = 1.0f;
    g_music_playing = TRUE;
}

static void reset_level(void) {
    u32 x = 0, y = 0;
    g_coin_count = 0;
    g_slime_count = 0;
    for (u32 i = 0; i < LEVEL_W * LEVEL_H; i++) g_map.tiles[i] = T_EMPTY;
    for (const char* p = k_level; *p && y < LEVEL_H; p++) {
        if (*p == '\n') { x = 0; y++; continue; }
        i32 tx = (i32)x, ty = (i32)y;
        Vec2 tpos = vec2((f32)tx * TS, (f32)ty * TS);
        switch (*p) {
        case '#': {
            b32 top = (ty == 0) || (k_level[(u32)(ty - 1) * (LEVEL_W + 1) + (u32)tx] != '#');
            tilemap_set(&g_map, tx, ty, top ? T_GRASS : T_DIRT);
        } break;
        case 'B': tilemap_set(&g_map, tx, ty, T_BRICK); break;
        case 'S': tilemap_set(&g_map, tx, ty, T_STONE); break;
        case '=': tilemap_set(&g_map, tx, ty, T_PLAT); break;
        case '^': tilemap_set(&g_map, tx, ty, T_SPIKES); break;
        case '*': tilemap_set(&g_map, tx, ty, T_BUSH); break;
        case ',': tilemap_set(&g_map, tx, ty, T_TUFT); break;
        case 'f': tilemap_set(&g_map, tx, ty, T_FLOWER); break;
        case 'o':
            if (g_coin_count < MAX_COINS) {
                g_coins[g_coin_count].pos = vec2(tpos.x + 3.0f, tpos.y + 2.0f);
                g_coins[g_coin_count].taken = FALSE;
                g_coin_count++;
            }
            break;
        case 'e':
            if (g_slime_count < MAX_SLIMES) {
                Slime* s = &g_slimes[g_slime_count++];
                s->pos = vec2(tpos.x + 2.0f, tpos.y + TS - 9.0f);
                s->vel = vec2(0.0f, 0.0f);
                s->dir = (g_slime_count & 1u) ? -1.0f : 1.0f;
                s->alive = TRUE;
                s->flat_t = 0.0f;
                anim_restart(&s->anim, &k_slime);
                s->anim.t = (f32)g_slime_count * 0.13f;
            }
            break;
        case 'P':
            g_spawn = vec2(tpos.x + 3.0f, tpos.y + TS - 14.0f);
            break;
        case 'F':
            for (i32 k = 0; k < 5; k++) tilemap_set(&g_map, tx, ty - k, (k == 4) ? T_FLAGTOP : T_POLE);
            g_goal_x = tpos.x + 6.0f;
            break;
        default: break;
        }
        x++;
    }
    g_coins_taken = 0;
    g_lives = 3;
    g_time = 0.0f;
    g_acc = 0.0f;
    g_state = PS_PLAYING;
    for (u32 i = 0; i < MAX_PARTS; i++) g_parts[i].life = 0.0f;
}

static void respawn_player(void) {
    __builtin_memset(&g_pl, 0, sizeof(g_pl));
    g_pl.pos = g_spawn;
    g_pl.size = vec2(10.0f, 14.0f);
    g_pl.facing = 1.0f;
    g_pl.invuln_t = 1.0f;
    anim_restart(&g_pl.anim, &k_idle);
    g_cam.pos = vec2(g_spawn.x, g_spawn.y);
    cam2d_clamp(&g_cam);
}

void scene_plat_init(Arena* perm) {
    plat_art_build(&g_art, perm, 0xC0FFEEull);
    tilemap_init(&g_map, perm, LEVEL_W, LEVEL_H, TS);
    g_map.defs = k_defs;
    g_map.def_count = T_COUNT;
    g_map.border_tile = T_STONE;   /* yanlar duvar; alt sınır aşağıda ayrıca ölüm */
    g_map.atlas = &g_art.atlas;
    g_map.atlas_tile_px = 16;
    rng_seed(&g_rng, 7);
    cam2d_init(&g_cam, 640.0f, 340.0f);
    g_cam.zoom = 2.0f;
    g_cam.offset = vec2(0.0f, (f32)TOP_BAR);
    g_cam.bounds = rect(0.0f, 0.0f, LEVEL_W * TS, LEVEL_H * TS);
    g_cam.deadzone = vec2(20.0f, 18.0f);
    g_cam.follow_rate = 7.0f;
    reset_level();
    respawn_player();
    g_music_on = TRUE;
    g_music_playing = FALSE;
    __builtin_memset(g_prev, 0, sizeof(g_prev));
    g_inited = TRUE;
}

void scene_plat_leave(void) {
    synth_song_stop(audio_synth());
    g_music_playing = FALSE;
}

static void hurt_player(f32 from_x) {
    if (g_pl.invuln_t > 0.0f || g_state != PS_PLAYING) return;
    sfx(&k_sfx_hurt);
    spawn_parts(vec2(g_pl.pos.x + 5.0f, g_pl.pos.y + 7.0f), 12, vec4(1.0f, 0.4f, 0.4f, 1.0f), 90.0f, 40.0f);
    if (g_lives > 0) g_lives--;
    if (g_lives == 0) {
        g_state = PS_GAME_OVER;
        synth_song_stop(audio_synth());
        g_music_playing = FALSE;
    }
    f32 dir = (g_pl.pos.x + 5.0f < from_x) ? -1.0f : 1.0f;
    g_pl.vel = vec2(dir * 140.0f, -220.0f);
    g_pl.hurt_t = 0.35f;
    g_pl.invuln_t = 1.6f;
}

static b32 overlap(Vec2 ap, Vec2 as, Vec2 bp, Vec2 bs) {
    return ap.x < bp.x + bs.x && bp.x < ap.x + as.x && ap.y < bp.y + bs.y && bp.y < ap.y + as.y;
}

static b32 standing_on_oneway_only(void) {
    u32 f = tilemap_query(&g_map, vec2(g_pl.pos.x, g_pl.pos.y + g_pl.size.y), vec2(g_pl.size.x, 1.0f));
    return (f & TILE_ONEWAY) && !(f & TILE_SOLID);
}

static void step_player(const InputState* in, b32 jump_pressed) {
    Player* p = &g_pl;
    b32 left  = in->keys[K_LEFT] || in->keys[K_A];
    b32 right = in->keys[K_RIGHT] || in->keys[K_D];
    b32 down  = in->keys[K_DOWN] || in->keys[K_S];
    p->jump_held = in->keys[K_SPACE] || in->keys[K_UP] || in->keys[K_W] || in->keys[K_Z];
    b32 control = (p->hurt_t <= 0.0f) && (g_state == PS_PLAYING);

    f32 want = 0.0f;
    if (control) want = (f32)right - (f32)left;
    f32 accel = p->on_ground ? ACCEL_GROUND : ACCEL_AIR;
    if (want != 0.0f) {
        p->vel.x += want * accel * STEP;
        p->vel.x = m_clamp(p->vel.x, -RUN_SPEED, RUN_SPEED);
        p->facing = want;
    } else if (p->on_ground) {
        f32 f = FRICTION * STEP;
        p->vel.x = (p->vel.x > f) ? p->vel.x - f : (p->vel.x < -f) ? p->vel.x + f : 0.0f;
    }

    if (jump_pressed && control) p->jump_buf = JUMP_BUFFER;
    p->coyote   = p->on_ground ? COYOTE_TIME : p->coyote - STEP;
    p->jump_buf -= STEP;
    p->drop_t   -= STEP;
    p->hurt_t   -= STEP;
    p->invuln_t -= STEP;

    if (p->jump_buf > 0.0f && p->coyote > 0.0f) {
        if (down && standing_on_oneway_only()) {
            p->drop_t = 0.2f;
        } else {
            p->vel.y = -JUMP_SPEED;
            sfx(&k_sfx_jump);
        }
        p->coyote = 0.0f;
        p->jump_buf = 0.0f;
    }
    if (!p->jump_held && p->vel.y < -JUMP_CUT) p->vel.y = -JUMP_CUT;   /* değişken zıplama yüksekliği */

    f32 g = GRAVITY;
    if (p->jump_held && m_abs(p->vel.y) < 50.0f) g *= 0.55f;            /* tepe noktasında hafif süzülme */
    p->vel.y = m_min(p->vel.y + g * STEP, MAX_FALL);

    f32 vy_before = p->vel.y;
    TileHit h = tilemap_move(&g_map, &p->pos, p->size, vec2_scale(p->vel, STEP), p->drop_t > 0.0f);
    b32 was_ground = p->on_ground;
    p->on_ground = h.on_ground;
    if (h.on_ground || h.hit_ceiling) p->vel.y = 0.0f;
    if (h.hit_left || h.hit_right) p->vel.x = 0.0f;
    if (h.on_ground && !was_ground && vy_before > 260.0f) {
        sfx(&k_sfx_land);
        spawn_parts(vec2(p->pos.x + 5.0f, p->pos.y + 14.0f), 6, vec4(0.85f, 0.75f, 0.6f, 1.0f), 40.0f, 30.0f);
    }
    if (h.touched & TILE_HAZARD) hurt_player(p->pos.x + 5.0f - p->facing * 4.0f);

    /* Haritanın altına düştü */
    if (p->pos.y > LEVEL_H * TS + 32.0f) {
        g_pl.invuln_t = 0.0f;
        hurt_player(p->pos.x);
        if (g_state == PS_PLAYING) {
            f32 inv = 1.6f;
            respawn_player();
            g_pl.invuln_t = inv;
        }
    }

    /* Hedef bayrak */
    if (g_state == PS_PLAYING && p->pos.x + p->size.x >= g_goal_x) {
        g_state = PS_WON;
        synth_song_stop(audio_synth());
        g_music_playing = FALSE;
        sfx(&k_sfx_win);
        spawn_parts(vec2(g_goal_x, p->pos.y), 40, vec4(1.0f, 0.9f, 0.3f, 1.0f), 140.0f, 80.0f);
    }

    /* Animasyon seçimi */
    if (p->hurt_t > 0.0f) anim_play(&p->anim, &k_hurt);
    else if (!p->on_ground) anim_play(&p->anim, p->vel.y < 0.0f ? &k_jump : &k_fall);
    else if (m_abs(p->vel.x) > 10.0f) anim_play(&p->anim, &k_run);
    else anim_play(&p->anim, &k_idle);
    anim_update(&p->anim, STEP);
}

static void step_slimes(void) {
    for (u32 i = 0; i < g_slime_count; i++) {
        Slime* s = &g_slimes[i];
        if (!s->alive) {
            s->flat_t -= STEP;
            continue;
        }
        Vec2 size = vec2(12.0f, 9.0f);
        s->vel.x = s->dir * 32.0f;
        s->vel.y = m_min(s->vel.y + GRAVITY * STEP, MAX_FALL);
        TileHit h = tilemap_move(&g_map, &s->pos, size, vec2_scale(s->vel, STEP), FALSE);
        if (h.on_ground) s->vel.y = 0.0f;
        /* duvar veya uçurum kenarında dön */
        f32 fx = (s->dir > 0.0f) ? s->pos.x + size.x + 1.0f : s->pos.x - 1.0f;
        u32 below = tilemap_flags(&g_map, (i32)m_floor(fx / TS), (i32)m_floor((s->pos.y + size.y + 2.0f) / TS));
        if (h.hit_left || h.hit_right || (h.on_ground && !(below & (TILE_SOLID | TILE_ONEWAY)))) s->dir = -s->dir;
        anim_update(&s->anim, STEP);

        if (g_state != PS_PLAYING) continue;
        if (overlap(g_pl.pos, g_pl.size, s->pos, size)) {
            f32 pl_bottom = g_pl.pos.y + g_pl.size.y;
            if (g_pl.vel.y > 0.0f && pl_bottom - s->pos.y < 7.0f) {
                s->alive = FALSE;
                s->flat_t = 0.6f;
                g_pl.vel.y = -(g_pl.jump_held ? STOMP_BOUNCE * 1.3f : STOMP_BOUNCE);
                sfx(&k_sfx_stomp);
                sfx(&k_sfx_boing);
                spawn_parts(vec2(s->pos.x + 6.0f, s->pos.y + 6.0f), 10, vec4(0.5f, 0.9f, 0.4f, 1.0f), 80.0f, 30.0f);
            } else {
                hurt_player(s->pos.x + 6.0f);
            }
        }
    }
}

static void step_coins(void) {
    for (u32 i = 0; i < g_coin_count; i++) {
        Coin* c = &g_coins[i];
        if (c->taken || g_state != PS_PLAYING) continue;
        if (overlap(g_pl.pos, g_pl.size, c->pos, vec2(10.0f, 12.0f))) {
            c->taken = TRUE;
            g_coins_taken++;
            sfx(&k_sfx_coin1);
            sfx(&k_sfx_coin2);
            spawn_parts(vec2(c->pos.x + 5.0f, c->pos.y + 6.0f), 8, vec4(1.0f, 0.85f, 0.3f, 1.0f), 70.0f, 20.0f);
        }
    }
}

static void step_particles(void) {
    for (u32 i = 0; i < MAX_PARTS; i++) {
        Particle* q = &g_parts[i];
        if (q->life <= 0.0f) continue;
        q->life -= STEP;
        q->vel.y += 400.0f * STEP;
        q->pos = vec2_add(q->pos, vec2_scale(q->vel, STEP));
    }
}

static void draw_atlas_tile(Batch2D* b, u32 idx, Vec2 center, f32 flip, Vec4 tint) {
    Rect src = rect((f32)(idx % 16u) * 16.0f, (f32)(idx / 16u) * 16.0f, 16.0f, 16.0f);
    b2d_sprite(b, &g_art.atlas, src, center, vec2(16.0f * flip, 16.0f), 0.0f, tint);
}

static void draw_parallax(Batch2D* b, const Texture* tex, f32 factor, f32 base_y, f32 scale) {
    Mat3 m = cam2d_matrix_parallax(&g_cam, factor, TRUE);
    b->camera = m;
    b->cam_is_identity = FALSE;
    f32 tw = 256.0f * scale;
    Rect vis = cam2d_visible_rect(&g_cam);
    f32 cx = vis.x + (g_cam.pos.x * factor - g_cam.pos.x);   /* paralaks uzayında görünür sol kenar */
    i32 k0 = (i32)m_floor(cx / tw) - 1, k1 = (i32)m_floor((cx + vis.w) / tw) + 1;
    for (i32 k = k0; k <= k1; k++) {
        f32 x = (f32)k * tw + tw * 0.5f;
        b2d_sprite(b, tex, rect(0.0f, 0.0f, 1.0f, 1.0f), vec2(x, base_y), vec2(tw + 0.5f, 64.0f * scale), 0.0f, vec4(1, 1, 1, 1));
    }
}

void scene_plat_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit) {
    if (!g_inited) return;
    if (!g_music_playing && g_music_on && g_state == PS_PLAYING) start_music();
    if (pressed(in, K_N)) {
        g_music_on = !g_music_on;
        if (!g_music_on) { synth_song_stop(audio_synth()); g_music_playing = FALSE; }
    }
    if (pressed(in, K_R)) {
        reset_level();
        respawn_player();
        if (g_music_on) start_music();
    }
    b32 jump_pressed = pressed(in, K_SPACE) || pressed(in, K_UP) || pressed(in, K_W) || pressed(in, K_Z);

    g_acc += m_min(dt, 0.1f);
    while (g_acc >= STEP) {
        step_player(in, jump_pressed);
        jump_pressed = FALSE;
        step_slimes();
        step_coins();
        step_particles();
        if (g_state == PS_PLAYING) g_time += STEP;
        g_acc -= STEP;
    }
    g_anim_t += dt;
    cam2d_follow(&g_cam, vec2(g_pl.pos.x + 5.0f + g_pl.facing * 12.0f, g_pl.pos.y + 7.0f), dt);
    __builtin_memcpy(g_prev, in->keys, sizeof(g_prev));

    /* --- Çizim --- */
    const ManagedShader* sh = shaders_find(sm, "sprite.mtk");
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, use_jit, BLEND_NONE, &fused);

    raster_begin(r, fb, frame);
    Batch2D b;
    b2d_begin(&b, r, mat3_identity());
    b2d_set_shader_ex(&b, fn, fused);
    b2d_set_blend(&b, BLEND_NONE);

    /* Gökyüzü (ekran uzayı) */
    b2d_sprite(&b, &g_art.sky, rect(0.0f, 0.0f, 1.0f, 1.0f),
               vec2((f32)fb->w * 0.5f, (f32)TOP_BAR + (f32)(fb->h - TOP_BAR) * 0.5f),
               vec2((f32)fb->w, (f32)(fb->h - TOP_BAR)), 0.0f, vec4(1, 1, 1, 1));
    /* Paralaks tepeler */
    draw_parallax(&b, &g_art.hills_far, 0.25f, 84.0f, 1.5f);
    draw_parallax(&b, &g_art.hills_near, 0.5f, 150.0f, 1.25f);

    /* Dünya */
    b.camera = cam2d_matrix(&g_cam, TRUE);
    b.cam_is_identity = FALSE;
    Rect vis = cam2d_visible_rect(&g_cam);
    Vec4 white = vec4(1, 1, 1, 1);
    tilemap_draw(&g_map, &b, vis, white);

    u32 coin_frame = (u32)(g_anim_t * 8.0f) % 4u;
    for (u32 i = 0; i < g_coin_count; i++) {
        if (g_coins[i].taken) continue;
        Vec2 c = vec2(g_coins[i].pos.x + 5.0f, g_coins[i].pos.y + 6.0f + m_sin(g_anim_t * 4.0f + (f32)i) * 1.0f);
        draw_atlas_tile(&b, ART_COIN0 + coin_frame, c, 1.0f, white);
    }
    for (u32 i = 0; i < g_slime_count; i++) {
        const Slime* s = &g_slimes[i];
        if (!s->alive && s->flat_t <= 0.0f) continue;
        u32 fr = s->alive ? anim_frame(&s->anim) : ART_SLIME_FLAT;
        draw_atlas_tile(&b, fr, vec2(s->pos.x + 6.0f, s->pos.y + 1.0f), s->dir > 0.0f ? -1.0f : 1.0f, white);
    }
    b32 blink = (g_pl.invuln_t > 0.0f) && (((u32)(g_pl.invuln_t * 16.0f)) & 1u);
    if (!blink) {
        draw_atlas_tile(&b, anim_frame(&g_pl.anim), vec2(g_pl.pos.x + 5.0f, g_pl.pos.y + 6.0f), g_pl.facing, white);
    }
    for (u32 i = 0; i < MAX_PARTS; i++) {
        const Particle* q = &g_parts[i];
        if (q->life <= 0.0f) continue;
        f32 k = q->life / q->max_life;
        Rect src = rect((f32)(ART_WHITE % 16u) * 16.0f, (f32)(ART_WHITE / 16u) * 16.0f, 16.0f, 16.0f);
        b2d_sprite(&b, &g_art.atlas, src, q->pos, vec2(q->size * k, q->size * k), 0.0f, q->color);
    }
    b2d_end(&b);
}

void scene_plat_draw_hud(UiContext* ui, const Framebuffer* fb) {
    char line[128];
    fmt_format(line, sizeof(line), "Altin %u/%u   Can %u   Sure %.1f s", g_coins_taken, g_coin_count, g_lives, (f64)g_time);
    ui_draw_rect(ui, 4, TOP_BAR + 4, font_measure_width(line, 1) + 8, 12, FB_RGBA(10, 14, 24, 170));
    ui_draw_text(ui, 8, TOP_BAR + 6, line, FB_RGBA(255, 236, 140, 255), 1);
    const char* help = "<-/-> kos  Bosluk zipla  Asagi+zipla in  R yeniden  N muzik";
    ui_draw_text(ui, 8, (i32)fb->h - 12, help, FB_RGBA(230, 240, 255, 220), 1);
    if (!audio_is_running()) {
        const char* a = "Ses icin bir tusa bas / tikla";
        ui_draw_text(ui, (i32)fb->w - font_measure_width(a, 1) - 8, TOP_BAR + 6, a, FB_RGBA(255, 200, 200, 255), 1);
    }
    if (g_state != PS_PLAYING) {
        const char* t = (g_state == PS_WON) ? "TEBRIKLER!" : "OYUN BITTI";
        fmt_format(line, sizeof(line), "%u/%u altin, %.1f s  -  R ile tekrar", g_coins_taken, g_coin_count, (f64)g_time);
        i32 tw = font_measure_width(t, 3), lw = font_measure_width(line, 1);
        i32 cx = (i32)fb->w / 2, cy = (i32)fb->h / 2;
        ui_draw_rect(ui, cx - MAX(tw, lw) / 2 - 12, cy - 30, MAX(tw, lw) + 24, 56, FB_RGBA(10, 14, 24, 200));
        ui_draw_text(ui, cx - tw / 2, cy - 22, t, (g_state == PS_WON) ? FB_RGBA(255, 220, 90, 255) : FB_RGBA(255, 110, 110, 255), 3);
        ui_draw_text(ui, cx - lw / 2, cy + 10, line, FB_RGBA(230, 240, 255, 255), 1);
    }
}
