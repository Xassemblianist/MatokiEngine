/* app/scene_kuyu.c — "Kuyu" retro FPS: karakter ızgarasından seviye (duvar/zemin/lav/kapı, 4×4 karolu
 * parçalara bölünmüş dünya-uzayı UV'li ağlar + birleştirilmiş AABB çarpışma), kapsül oyuncu (basamak
 * çıkma, coyote, zıplama tamponu), iki düşman türü (yakın dövüşçü Gölge, uzaktan tüküren Tükürgen),
 * üç silah (hitscan, saçma, patlayan kor), parçacıklar, parça başına en güçlü 4 nokta ışık seçimi,
 * 320×180 iç çizim → retro_present, Matoki Sans HUD, synth efektleri ve özgün karanlık tema.
 */
#include "app/scene_kuyu.h"
#include "core/math.h"
#include "core/log.h"
#include "core/rng.h"
#include "core/profiler.h"
#include "plat/platform.h"
#include "raster/texture.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/retro.h"
#include "phys/world.h"
#include "audio/audio.h"
#include "audio/synth.h"
#include "font/ttf.h"
#include "font/atlas.h"
#include "font/text.h"
#include "font/matoki_sans_data.h"

#define KEY_SPACE 32
#define KEY_LEFT  37
#define KEY_UP    38
#define KEY_RIGHT 39
#define KEY_DOWN  40
#define KEY_1     49
#define KEY_2     50
#define KEY_3     51
#define KEY_A     65
#define KEY_D     68
#define KEY_F     70
#define KEY_G     71
#define KEY_N     78
#define KEY_R     82
#define KEY_S     83
#define KEY_V     86
#define KEY_W     87

#define INNER_W     320u
#define INNER_H     180u
#define TILE        2.0f
#define WALL_H      5.0f
#define SKY_H       12.0f
#define LAVA_Y      (-0.8f)
#define LINTEL_Y    3.2f
#define SHAFT_H     11.0f
#define CHUNK       4
#define STEP_DT     (1.0f / 120.0f)
#define GRAVITY     (-20.0f)
#define JUMP_SPEED  7.6f
#define RUN_SPEED   8.5f
#define ACC_GROUND  80.0f
#define ACC_AIR     22.0f
#define STEP_H      0.55f
#define COYOTE_TIME 0.10f
#define JUMP_BUFFER 0.12f
#define PLAYER_R    0.35f
#define PLAYER_H    1.6f
#define EYE_H       1.45f
#define LAVA_DPS    40.0f
#define MAX_HP      100.0f

#define MAX_ENEMIES   16u
#define MAX_PICKUPS   24u
#define MAX_DOORS     8u
#define MAX_PROJ      48u
#define MAX_PARTICLES 192u
#define MAX_LIGHTS    96u
#define MAX_STATIC_LIGHTS 64u

/* Harita: # duvar, . zemin, ~ lav, 1-4 yükseltilmiş zemin (0.5 m adım), d kapı, D mühürlü kapı,
 * P başlangıç, g Gölge, s Tükürgen, e Gözcü (uçan), h can, a saçma, c kor, w saçmalı, f kor atar, k mühür,
 * X çıkış, i mangal */
static const char* const k_map1[] = {
    "################################",
    "#i...h#i.......i#i....~~~~~~~~i#",
    "#.....#.........#.....~~~~~~~~~#",
    "#..P..d...g.....d...s.~~1~1~k~~#",
    "#.....#.........#.....~~~~~~~~~#",
    "#i...a#i...w...i#i....~~~~~~~~i#",
    "###d##########d######d##########",
    "#i.....g...#i...s..i#...i#....i#",
    "#..........#........#....#..X..#",
    "#..1122....d...g....d..s.D.....#",
    "#..1122.g..#........#....#.....#",
    "#i..s..c..i#i..f...i#i..h#....i#",
    "################################",
};

/* 2. bölüm "Derin Kuyu": lav köprülü mağara, uçan Gözcüler, yükseltilmiş yürüme yolu ile çevrili çıkış kulübesi */
static const char* const k_map2[] = {
    "####################################",
    "#i....#i...........#i......e......i#",
    "#..P..#..g.........d...............#",
    "#.....d.....####...#...~~~~.~~~~...#",
    "#i...h#..g..#a.#...#...~~~~.~~~~...#",
    "###.###.....#..#.s.#.e.~~~~.~~~~.e.#",
    "#.....#.....##d#...#...~~~~.~~~~...#",
    "#.c...#............#i..~~~~.~~~~..i#",
    "#.....##########d###########d#######",
    "#i.......g.....#.....#i...........i#",
    "#.11122........#..k..d.....s....e..#",
    "#.11122....s...#.....#.............#",
    "#.......####...##d####.12222222....#",
    "#i...a..#..#.......f#...2#####2..h.#",
    "######d##..#.........d..2#.X.#2....#",
    "#..........#....g...#...2#...D2....#",
    "#i....s....#i......i#i..2#####2..i.#",
    "####################################",
};

typedef struct {
    const char* const* rows;
    i32         w, h;
    const char* title;
    const char* intro;
} LevelDef;

static const LevelDef k_levels[] = {
    { k_map1, 32, (i32)(sizeof(k_map1) / sizeof(k_map1[0])), "1. Bolum: Kuyunun Dibi", "Kuyunun dibindesin. Isik bacasini bul." },
    { k_map2, 36, (i32)(sizeof(k_map2) / sizeof(k_map2[0])), "2. Bolum: Derin Kuyu", "Daha derindesin. Gozculere dikkat: ucarlar." },
};
#define LEVEL_COUNT ((u32)(sizeof(k_levels) / sizeof(k_levels[0])))
#define LEVEL_ARENA MB(8)

#define MAP_W    (g_k.map_w)
#define MAP_H    (g_k.map_h)
#define CHUNKS_X (g_k.chunks_x)
#define CHUNKS_Z (g_k.chunks_z)
#define FLOW_INF 0xFFFFu

typedef enum { KS_PLAY = 0, KS_WIN = 1, KS_DEAD = 2 } KuyuState;
typedef enum { EN_GOLGE = 0, EN_TUKURGEN = 1, EN_GOZCU = 2 } EnemyKind;
typedef enum { PK_HEALTH = 0, PK_SHELLS, PK_KOR, PK_W_SACMALI, PK_W_KOR, PK_KEY } PickupKind;
typedef enum { W_CAKMAK = 0, W_SACMALI = 1, W_KOR = 2, W_COUNT = 3 } Weapon;

typedef struct { Vec3 pos; Vec3 col; f32 radius; } Light;

typedef struct {
    EnemyKind kind;
    Vec3 feet, vel;
    f32  hp;
    b32  alive, alert, grounded;
    f32  cooldown, pain, dead_t, anim, yaw;
    f32  steer_t, steer_sign;
    Vec3 last_seen;
    u32  burst;             /* Gözcü: kalan ok */
    f32  burst_t;
} Enemy;

typedef struct { PickupKind kind; Vec3 pos; b32 taken; } Pickup;

typedef struct {
    i32  tx, tz;
    b32  locked, along_x;   /* along_x: geçiş x ekseni boyunca (kapı x'te ince) */
    f32  open;              /* 0 kapalı → 1 açık */
    u32  phys;              /* PhysWorld statik indeksi */
    Aabb closed;
} Door;

typedef struct {
    Vec3 pos, vel;
    f32  life, dmg, splash;
    b32  from_player, active;
    u32  kind;              /* 0 kor (oyuncu), 1 tükürük, 2 Gözcü oku */
} Proj;

typedef struct { Vec3 pos, vel; f32 life, max_life, size; Vec4 col; b32 grav; } Particle;

typedef struct {
    Mesh* wall; Mesh* floor; Mesh* ceil; Mesh* lava;
    Vec3  bmin, bmax;
} Chunk;

typedef struct {
    /* Kaynaklar */
    Framebuffer low;
    Texture     tex_wall, tex_floor, tex_ceil, tex_door, tex_lava, tex_white, tex_skin;
    Mesh*       cube;
    Mesh*       sphere;
    Arena       lv_arena;       /* seviye ağları, fizik, akış alanları: her bölüm yüklenişinde sıfırlanır */
    u32         level;
    const char* const* map_rows;
    i32         map_w, map_h, chunks_x, chunks_z;
    Chunk*      chunks;
    u16*        flow_ground;    /* oyuncu karosuna yürüyerek adım sayısı (FLOW_INF ulaşılamaz) */
    u16*        flow_air;       /* uçanlar için (lav ve yükseklik engel değil) */
    i32         flow_tx, flow_tz;
    f32         flow_timer;
    b32         snap_on;
    u32         pending_level;  /* debug: bir sonraki karede yüklenecek bölüm + 1 (0 yok) */
    u8          prev_g;
    PhysWorld   world;
    TtfFont     font;
    FontAtlas   font_small, font_big;
    b32         font_ok;
    Rng         rng;

    /* Seviye nesneleri */
    Light   static_lights[MAX_STATIC_LIGHTS];
    u32     static_light_count;
    Light   lights[MAX_LIGHTS];
    u32     light_count;
    Enemy   enemies[MAX_ENEMIES];
    u32     enemy_count;
    Pickup  pickups[MAX_PICKUPS];
    u32     pickup_count;
    Door    doors[MAX_DOORS];
    u32     door_count;
    Proj    proj[MAX_PROJ];
    Particle parts[MAX_PARTICLES];
    u32     part_next;
    Vec3    start_feet;
    Vec3    exit_pos;

    /* Oyuncu */
    Vec3  feet, vel;
    f32   yaw, pitch;
    b32   grounded;
    f32   coyote, jump_buf, step_acc;
    f32   hp;
    b32   god;
    b32   has_weapon[W_COUNT];
    u32   shells, kor;
    Weapon weapon;
    f32   fire_cd, recoil, flash_t, bob_phase, step_snd;
    f32   hurt_flash, shake, pickup_flash;
    b32   has_key;
    u32   kills;          /* bu bölümde */
    u32   total_kills, total_enemies;
    KuyuState state;
    f32   play_time, msg_time, lock_msg_cd;
    char  msg[96];
    b32   music_on, music_playing, retro_on;
    u8    prev_r, prev_n, prev_space, prev_v;
    f32   anim_time;
    f32   ms_draw, ms_present;
    G3dStats stats;

    /* Çizim durumu (kare başına) */
    G3dContext*  ctx;
    Arena*       frame;
    G3dMaterial  mat_lit, mat_emis;
    Vec3         eye;
} Kuyu;

static Kuyu g_k;

/* ---------- Ses ---------- */

static const SynthSfx k_sfx_cakmak  = { WAVE_SQUARE, 520.0f, 140.0f, 0.3f, 0.001f, 0.06f, 0.2f, 0.01f, 0.08f, 0.26f, 0.0f, 0.0f };
static const SynthSfx k_sfx_sacma   = { WAVE_NOISE, 1400.0f, 120.0f, 0.0f, 0.001f, 0.16f, 0.3f, 0.04f, 0.22f, 0.42f, 0.0f, 0.0f };
static const SynthSfx k_sfx_kor     = { WAVE_SAW, 220.0f, 70.0f, 0.0f, 0.002f, 0.18f, 0.4f, 0.05f, 0.2f, 0.30f, 0.0f, 0.0f };
static const SynthSfx k_sfx_boom    = { WAVE_NOISE, 600.0f, 40.0f, 0.0f, 0.002f, 0.45f, 0.3f, 0.1f, 0.5f, 0.48f, 0.0f, 0.0f };
static const SynthSfx k_sfx_hit     = { WAVE_SQUARE, 180.0f, 90.0f, 0.4f, 0.001f, 0.05f, 0.2f, 0.01f, 0.05f, 0.18f, 0.0f, 0.0f };
static const SynthSfx k_sfx_g_die   = { WAVE_SAW, 300.0f, 40.0f, 0.0f, 0.005f, 0.5f, 0.3f, 0.1f, 0.4f, 0.32f, 9.0f, 1.0f };
static const SynthSfx k_sfx_t_die   = { WAVE_TRIANGLE, 160.0f, 30.0f, 0.0f, 0.005f, 0.6f, 0.3f, 0.1f, 0.5f, 0.36f, 14.0f, 2.0f };
static const SynthSfx k_sfx_g_alert = { WAVE_SAW, 90.0f, 160.0f, 0.0f, 0.05f, 0.3f, 0.5f, 0.1f, 0.3f, 0.22f, 6.0f, 0.8f };
static const SynthSfx k_sfx_spit    = { WAVE_NOISE, 300.0f, 900.0f, 0.0f, 0.01f, 0.15f, 0.3f, 0.02f, 0.12f, 0.24f, 0.0f, 0.0f };
static const SynthSfx k_sfx_e_die   = { WAVE_SQUARE, 900.0f, 120.0f, 0.2f, 0.002f, 0.35f, 0.3f, 0.05f, 0.3f, 0.28f, 18.0f, 1.5f };
static const SynthSfx k_sfx_bolt    = { WAVE_SQUARE, 1300.0f, 700.0f, 0.15f, 0.001f, 0.05f, 0.2f, 0.01f, 0.05f, 0.14f, 0.0f, 0.0f };
static const SynthSfx k_sfx_claw    = { WAVE_NOISE, 2400.0f, 800.0f, 0.0f, 0.001f, 0.08f, 0.2f, 0.01f, 0.06f, 0.22f, 0.0f, 0.0f };
static const SynthSfx k_sfx_hurt    = { WAVE_SQUARE, 140.0f, 70.0f, 0.5f, 0.001f, 0.12f, 0.3f, 0.02f, 0.1f, 0.30f, 0.0f, 0.0f };
static const SynthSfx k_sfx_pick    = { WAVE_TRIANGLE, 660.0f, 1320.0f, 0.0f, 0.001f, 0.05f, 0.5f, 0.04f, 0.12f, 0.26f, 0.0f, 0.0f };
static const SynthSfx k_sfx_key     = { WAVE_SQUARE, 440.0f, 880.0f, 0.5f, 0.005f, 0.2f, 0.6f, 0.3f, 0.4f, 0.26f, 6.0f, 0.3f };
static const SynthSfx k_sfx_door    = { WAVE_SAW, 70.0f, 55.0f, 0.0f, 0.05f, 0.4f, 0.6f, 0.4f, 0.3f, 0.22f, 3.0f, 0.2f };
static const SynthSfx k_sfx_locked  = { WAVE_SQUARE, 110.0f, 100.0f, 0.5f, 0.002f, 0.1f, 0.4f, 0.05f, 0.1f, 0.22f, 0.0f, 0.0f };
static const SynthSfx k_sfx_step    = { WAVE_NOISE, 300.0f, 120.0f, 0.0f, 0.001f, 0.03f, 0.1f, 0.0f, 0.03f, 0.06f, 0.0f, 0.0f };
static const SynthSfx k_sfx_jump    = { WAVE_NOISE, 500.0f, 250.0f, 0.0f, 0.001f, 0.04f, 0.1f, 0.0f, 0.04f, 0.08f, 0.0f, 0.0f };
static const SynthSfx k_sfx_win     = { WAVE_TRIANGLE, 330.0f, 990.0f, 0.0f, 0.05f, 0.6f, 0.7f, 0.8f, 0.8f, 0.30f, 5.0f, 0.3f };
static const SynthSfx k_sfx_death   = { WAVE_SAW, 220.0f, 30.0f, 0.0f, 0.01f, 1.0f, 0.3f, 0.2f, 0.8f, 0.34f, 4.0f, 0.6f };

static void sfx(const SynthSfx* s) { synth_play(audio_synth(), s); }

/* Uzaklığa göre kısılan efekt (kaynak oyuncudan uzaksa sessizleşir) */
static void sfx_at(const SynthSfx* s, Vec3 p) {
    f32 d = vec3_len(vec3_sub(p, g_k.feet));
    f32 g = 1.0f / (1.0f + d * d * 0.012f);
    if (g < 0.05f) return;
    SynthSfx c = *s;
    c.volume *= g;
    synth_play(audio_synth(), &c);
}

/* Özgün karanlık tema: Mi frig modu, yavaş; derin bas dronu, seyrek titreşimli melodi, alçak nabız */
static void start_music(void) {
    Synth* s = audio_synth();
    synth_song_stop(s);
    s->song.track_count = 0;
    SynthSfx bass  = { WAVE_TRIANGLE, 0.0f, 0.0f, 0.0f, 0.10f, 0.50f, 0.8f, 1.2f, 0.60f, 0.26f, 0.0f, 0.0f };
    SynthSfx lead  = { WAVE_TRIANGLE, 0.0f, 0.0f, 0.0f, 0.12f, 0.40f, 0.5f, 0.40f, 0.70f, 0.10f, 5.0f, 0.25f };
    SynthSfx pulse = { WAVE_SQUARE, 0.0f, 0.0f, 0.12f, 0.004f, 0.08f, 0.15f, 0.01f, 0.10f, 0.035f, 0.0f, 0.0f };
    synth_song_add_track(s,
        "E2 - - - - - - - - - - - - - - - F2 - - - - - - - E2 - - - - - - - "
        "E2 - - - - - - - - - - - - - - - D2 - - - - - - - C2 - - - B1 - - - ", &bass);
    synth_song_add_track(s,
        "- - - - B4 - - - - - - - C5 - - - B4 - - - - - - - - - - - A4 - - - "
        "G4 - - - - - - - F4 - - - - - - - E4 - - - - - - - - - - - - - - - ", &lead);
    synth_song_add_track(s,
        "E3 - - E3 - - B3 - E3 - - E3 - - C4 - ", &pulse);
    synth_song_start(s, 70.0f, 4);
    s->song.volume = 0.85f;
    g_k.music_playing = TRUE;
}

static void stop_music(void) {
    if (g_k.music_playing) synth_song_stop(audio_synth());
    g_k.music_playing = FALSE;
}

/* ---------- Dokular (prosedürel) ---------- */

static u32 rgb_clamp(f32 r, f32 g, f32 b) {
    return FB_RGBA((u32)m_clamp(r, 0.0f, 255.0f), (u32)m_clamp(g, 0.0f, 255.0f), (u32)m_clamp(b, 0.0f, 255.0f), 255);
}

/* Taş tuğla: 64×64 doku = 2 m; 4 sıra, satır kaydırmalı, harç + taş başına ton + benek */
static void gen_bricks(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    f32 tone[64];
    for (u32 i = 0; i < 64; i++) tone[i] = 0.75f + 0.35f * rng_f32(r);
    for (u32 y = 0; y < h; y++) {
        u32 row = y / 16u;
        for (u32 x = 0; x < w; x++) {
            u32 xs = (x + (row & 1u) * 12u) % w;
            u32 col = xs / 24u;
            b32 mortar = (y % 16u) < 2u || (xs % 24u) < 2u;
            f32 n = 0.85f + 0.3f * rng_f32(r);
            f32 k = mortar ? 0.35f : tone[(row * 7u + col * 3u) % 64u] * n;
            t->px[y * w + x] = rgb_clamp(120.0f * k, 104.0f * k, 92.0f * k);
        }
    }
}

/* Döşeme taşı: 2×2 büyük levha, kenarları koyu, hafif yosun lekeleri */
static void gen_flags(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 cx = x % 32u, cy = y % 32u;
            b32 seam = cx < 1u || cy < 1u;
            f32 n = 0.8f + 0.35f * rng_f32(r);
            f32 tone = 0.85f + 0.15f * (f32)(((x / 32u) * 3u + (y / 32u) * 5u) % 4u) / 3.0f;
            f32 k = seam ? 0.3f : tone * n;
            f32 moss = (((x * 7u + y * 13u) % 29u) < 3u) ? 1.0f : 0.0f;
            t->px[y * w + x] = rgb_clamp(96.0f * k, (92.0f + 30.0f * moss) * k, 84.0f * k);
        }
    }
}

/* Tavan: koyu kalas */
static void gen_planks(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            b32 seam = (x % 16u) < 1u;
            f32 grain = 0.8f + 0.2f * m_sin((f32)y * 0.7f + (f32)(x / 16u) * 2.3f) + 0.15f * rng_f32(r);
            f32 k = seam ? 0.25f : grain;
            t->px[y * w + x] = rgb_clamp(70.0f * k, 48.0f * k, 30.0f * k);
        }
    }
}

/* Kapı: paslı demir levha + perçinler + yatay bant */
static void gen_door(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            f32 n = 0.8f + 0.3f * rng_f32(r);
            b32 band = (y % 21u) < 3u;
            b32 rivet = band && (x % 10u) == 5u && (y % 21u) == 1u;
            f32 rust = (((x * 5u + y * 3u) % 17u) < 4u) ? 1.0f : 0.0f;
            f32 k = rivet ? 1.6f : band ? 0.7f * n : n;
            t->px[y * w + x] = rgb_clamp((90.0f + 50.0f * rust) * k, (84.0f + 10.0f * rust) * k, 80.0f * k);
        }
    }
}

/* Lav: sıcak kırmızı-turuncu, kabuk damarları */
static void gen_lava(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            f32 v = m_sin((f32)x * 0.35f + m_sin((f32)y * 0.21f) * 2.0f) * m_sin((f32)y * 0.3f + (f32)x * 0.1f);
            f32 k = 0.6f + 0.4f * v + 0.1f * rng_f32(r);
            f32 crust = v < -0.55f ? 0.25f : 1.0f;
            t->px[y * w + x] = rgb_clamp(255.0f * k * crust, 110.0f * k * k * crust, 30.0f * k * crust);
        }
    }
}

/* Yaratık derisi: benekli */
static void gen_skin(Texture* t, Rng* r) {
    u32 w = t->w, h = t->h;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            f32 k = 0.7f + 0.3f * rng_f32(r);
            if (((x * 3u + y * 7u) % 11u) < 2u) k *= 0.6f;
            t->px[y * w + x] = rgb_clamp(255.0f * k, 255.0f * k, 255.0f * k);
        }
    }
}

static void make_tex(Texture* t, Arena* perm, u32 size, void (*gen)(Texture*, Rng*)) {
    tex_init_mipmapped(t, perm, size, size, 0);
    gen(t, &g_k.rng);
    tex_build_mips(t);
}

/* ---------- Seviye ---------- */

static char tile_at(i32 x, i32 z) {
    if (x < 0 || z < 0 || x >= MAP_W || z >= MAP_H) return '#';
    return g_k.map_rows[z][x];
}

static f32 tile_floor(char c) {
    if (c == '#') return WALL_H;
    if (c == '~') return LAVA_Y;
    if (c >= '1' && c <= '4') return (f32)(c - '0') * 0.5f;
    return 0.0f;
}

static f32 tile_ceil(char c) {
    if (c == '#') return WALL_H;
    if (c == 'd' || c == 'D') return LINTEL_Y;
    if (c == 'X') return SHAFT_H;
    return WALL_H;
}

static Vec3 tile_center(i32 x, i32 z, f32 y) {
    return vec3(((f32)x + 0.5f) * TILE, y, ((f32)z + 0.5f) * TILE);
}

/* Geçici quad yığını (parça ağı kurulurken) */
typedef struct {
    f32* pos;   /* 12 / quad */
    f32* nrm;   /* 3 / quad */
    f32* uv;    /* 8 / quad */
    u32  count, cap;
} QuadList;

static void ql_init(QuadList* q, Arena* a, u32 cap) {
    q->pos = ARENA_PUSH_ARRAY(a, f32, cap * 12u);
    q->nrm = ARENA_PUSH_ARRAY(a, f32, cap * 3u);
    q->uv  = ARENA_PUSH_ARRAY(a, f32, cap * 8u);
    q->count = 0;
    q->cap = cap;
}

/* Dört köşe (herhangi bir dönme sırasıyla) + dış normal; sarım normalle CCW olacak şekilde düzeltilir.
 * UV dünya uzayından: normalin baskın eksenine göre düzlemsel izdüşüm (0.5 = 2 m'de bir tekrar). */
static void ql_add(QuadList* q, Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Vec3 n) {
    if (q->count >= q->cap) return;
    Vec3 c = vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0));
    if (vec3_dot(c, n) < 0.0f) { Vec3 t = p1; p1 = p3; p3 = t; }
    Vec3 p[4] = { p0, p1, p2, p3 };
    u32 k = q->count++;
    for (u32 i = 0; i < 4; i++) {
        q->pos[k * 12u + i * 3u + 0] = p[i].x;
        q->pos[k * 12u + i * 3u + 1] = p[i].y;
        q->pos[k * 12u + i * 3u + 2] = p[i].z;
        f32 u, v;
        if (m_abs(n.y) > 0.5f)      { u = p[i].x * 0.5f; v = p[i].z * 0.5f; }
        else if (m_abs(n.x) > 0.5f) { u = p[i].z * 0.5f; v = -p[i].y * 0.5f; }
        else                        { u = p[i].x * 0.5f; v = -p[i].y * 0.5f; }
        q->uv[k * 8u + i * 2u + 0] = u;
        q->uv[k * 8u + i * 2u + 1] = v;
    }
    q->nrm[k * 3u + 0] = n.x; q->nrm[k * 3u + 1] = n.y; q->nrm[k * 3u + 2] = n.z;
}

static Mesh* ql_to_mesh(const QuadList* q, Arena* perm) {
    if (q->count == 0) return NULL;
    Mesh* m = mesh_alloc(perm, q->count * 4u, q->count * 6u, FALSE);
    for (u32 k = 0; k < q->count; k++) {
        for (u32 i = 0; i < 4; i++) {
            u32 vi = k * 4u + i;
            m->px[vi] = q->pos[k * 12u + i * 3u + 0];
            m->py[vi] = q->pos[k * 12u + i * 3u + 1];
            m->pz[vi] = q->pos[k * 12u + i * 3u + 2];
            m->nx[vi] = q->nrm[k * 3u + 0];
            m->ny[vi] = q->nrm[k * 3u + 1];
            m->nz[vi] = q->nrm[k * 3u + 2];
            m->u[vi]  = q->uv[k * 8u + i * 2u + 0];
            m->v[vi]  = q->uv[k * 8u + i * 2u + 1];
        }
        u32 b = k * 4u;
        u32* ix = &m->indices[k * 6u];
        ix[0] = b; ix[1] = b + 1u; ix[2] = b + 2u; ix[3] = b; ix[4] = b + 2u; ix[5] = b + 3u;
    }
    mesh_compute_bounds(m);
    return m;
}

/* Bir açık karonun (x,z) yüz geometrisi: zemin üstü, tavan altı, komşulara bakan dikey yüzler */
static void build_tile(i32 x, i32 z, QuadList* qw, QuadList* qf, QuadList* qc, QuadList* ql) {
    char c = tile_at(x, z);
    if (c == '#') return;
    f32 x0 = (f32)x * TILE, x1 = x0 + TILE, z0 = (f32)z * TILE, z1 = z0 + TILE;
    f32 fa = tile_floor(c), ca = tile_ceil(c);
    if (c == '~') {
        f32 y = LAVA_Y + 0.25f;
        ql_add(ql, vec3(x0, y, z0), vec3(x0, y, z1), vec3(x1, y, z1), vec3(x1, y, z0), vec3(0, 1, 0));
    } else {
        ql_add(qf, vec3(x0, fa, z0), vec3(x0, fa, z1), vec3(x1, fa, z1), vec3(x1, fa, z0), vec3(0, 1, 0));
    }
    if (c != 'X') {
        ql_add(qc, vec3(x0, ca, z0), vec3(x1, ca, z0), vec3(x1, ca, z1), vec3(x0, ca, z1), vec3(0, -1, 0));
    }
    /* Komşular: -x, +x, -z, +z; yüz komşu sınırında, normal bu karoya doğru */
    static const i32 dx[4] = { -1, 1, 0, 0 }, dz[4] = { 0, 0, -1, 1 };
    for (u32 s = 0; s < 4; s++) {
        char nb = tile_at(x + dx[s], z + dz[s]);
        f32 fb = tile_floor(nb), cb = tile_ceil(nb);
        Vec3 n = vec3((f32)-dx[s], 0.0f, (f32)-dz[s]);
        f32 ax, az, bx, bz;
        if (s == 0)      { ax = x0; az = z0; bx = x0; bz = z1; }
        else if (s == 1) { ax = x1; az = z0; bx = x1; bz = z1; }
        else if (s == 2) { ax = x0; az = z0; bx = x1; bz = z0; }
        else             { ax = x0; az = z1; bx = x1; bz = z1; }
        f32 lo = (c == '~') ? LAVA_Y : fa;
        /* Alt katı: komşunun zemini bu karonun açık aralığına taşıyor */
        if (fb > lo) {
            f32 top = m_min(fb, ca);
            QuadList* q = (nb == '#') ? qw : qw;
            ql_add(q, vec3(ax, lo, az), vec3(bx, lo, bz), vec3(bx, top, bz), vec3(ax, top, az), n);
        }
        /* Üst katı: komşunun tavanı bu karonun tavanından alçak (kapı lentosu, baca) */
        if (nb != '#' && cb < ca) {
            f32 bot = m_max(cb, lo);
            ql_add(qw, vec3(ax, bot, az), vec3(bx, bot, bz), vec3(bx, ca, bz), vec3(ax, ca, az), n);
        }
    }
}

/* Aynı sınıftaki karoları açgözlü dikdörtgenlere birleştirip AABB statik ekler */
static void add_merged_statics(Arena* scratch, i32 (*cls)(char), f32 (*ylo)(i32), f32 (*yhi)(i32)) {
    u8* used = ARENA_PUSH_ARRAY(scratch, u8, (usize)(MAP_W * MAP_H));
    __builtin_memset(used, 0, (usize)(MAP_W * MAP_H));
    for (i32 z = 0; z < MAP_H; z++) {
        for (i32 x = 0; x < MAP_W; x++) {
            i32 k = cls(tile_at(x, z));
            if (k < 0 || used[z * MAP_W + x]) continue;
            i32 x1 = x;
            while (x1 + 1 < MAP_W && !used[z * MAP_W + x1 + 1] && cls(tile_at(x1 + 1, z)) == k) x1++;
            i32 z1 = z;
            for (;;) {
                if (z1 + 1 >= MAP_H) break;
                b32 ok = TRUE;
                for (i32 xi = x; xi <= x1 && ok; xi++) {
                    if (used[(z1 + 1) * MAP_W + xi] || cls(tile_at(xi, z1 + 1)) != k) ok = FALSE;
                }
                if (!ok) break;
                z1++;
            }
            for (i32 zi = z; zi <= z1; zi++) for (i32 xi = x; xi <= x1; xi++) used[zi * MAP_W + xi] = 1;
            Aabb b = aabb_make(vec3((f32)x * TILE, ylo(k), (f32)z * TILE),
                               vec3((f32)(x1 + 1) * TILE, yhi(k), (f32)(z1 + 1) * TILE));
            phys_world_add_static(&g_k.world, shape_from_aabb(b), 0.0f, 0.0f);
        }
    }
}

static i32 cls_wall(char c) { return c == '#' ? 0 : -1; }
static i32 cls_floor(char c) {
    if (c == '#' || c == '~') return -1;
    f32 f = tile_floor(c);
    return (i32)(f * 2.0f + 0.5f) + 1;   /* yükseklik sınıfı (0 m → 1) */
}
static f32 wall_lo(i32 k) { (void)k; return LAVA_Y; }
static f32 wall_hi(i32 k) { (void)k; return WALL_H; }
static f32 floor_lo(i32 k) { (void)k; return LAVA_Y; }
static f32 floor_hi(i32 k) { return (f32)(k - 1) * 0.5f; }

static void add_static_light(Vec3 p, Vec3 col, f32 radius) {
    if (g_k.static_light_count >= MAX_STATIC_LIGHTS) return;
    Light* l = &g_k.static_lights[g_k.static_light_count++];
    l->pos = p; l->col = col; l->radius = radius;
}

static void build_level(Arena* perm, Arena* scratch) {
    usize mark = arena_mark(scratch);
    g_k.chunks_x = (MAP_W + CHUNK - 1) / CHUNK;
    g_k.chunks_z = (MAP_H + CHUNK - 1) / CHUNK;
    g_k.chunks = ARENA_PUSH_ARRAY(perm, Chunk, (usize)(CHUNKS_X * CHUNKS_Z));
    g_k.flow_ground = ARENA_PUSH_ARRAY(perm, u16, (usize)(MAP_W * MAP_H));
    g_k.flow_air    = ARENA_PUSH_ARRAY(perm, u16, (usize)(MAP_W * MAP_H));
    g_k.flow_tx = g_k.flow_tz = -1;
    for (i32 cz = 0; cz < CHUNKS_Z; cz++) {
        for (i32 cx = 0; cx < CHUNKS_X; cx++) {
            QuadList qw, qf, qc, ql;
            ql_init(&qw, scratch, 256);
            ql_init(&qf, scratch, 64);
            ql_init(&qc, scratch, 64);
            ql_init(&ql, scratch, 64);
            for (i32 z = cz * CHUNK; z < (cz + 1) * CHUNK && z < MAP_H; z++)
                for (i32 x = cx * CHUNK; x < (cx + 1) * CHUNK && x < MAP_W; x++)
                    build_tile(x, z, &qw, &qf, &qc, &ql);
            Chunk* ch = &g_k.chunks[cz * CHUNKS_X + cx];
            ch->wall  = ql_to_mesh(&qw, perm);
            ch->floor = ql_to_mesh(&qf, perm);
            ch->ceil  = ql_to_mesh(&qc, perm);
            ch->lava  = ql_to_mesh(&ql, perm);
            ch->bmin = vec3((f32)(cx * CHUNK) * TILE, LAVA_Y, (f32)(cz * CHUNK) * TILE);
            ch->bmax = vec3((f32)((cx + 1) * CHUNK) * TILE, SHAFT_H, (f32)((cz + 1) * CHUNK) * TILE);
        }
    }
    arena_pop_to(scratch, mark);

    phys_world_init(&g_k.world, perm, 4, 256, 1);
    g_k.world.gravity = vec3(0.0f, GRAVITY, 0.0f);
    phys_world_add_static(&g_k.world, shape_from_plane(plane_make(vec3(0.0f, 1.0f, 0.0f), LAVA_Y)), 0.0f, 0.0f);
    add_merged_statics(scratch, cls_wall, wall_lo, wall_hi);
    add_merged_statics(scratch, cls_floor, floor_lo, floor_hi);
    arena_pop_to(scratch, mark);

    /* Kapılar, ışıklar */
    g_k.door_count = 0;
    g_k.static_light_count = 0;
    for (i32 z = 0; z < MAP_H; z++) {
        for (i32 x = 0; x < MAP_W; x++) {
            char c = tile_at(x, z);
            if ((c == 'd' || c == 'D') && g_k.door_count < MAX_DOORS) {
                Door* d = &g_k.doors[g_k.door_count++];
                d->tx = x; d->tz = z;
                d->locked = (c == 'D');
                d->along_x = tile_at(x, z - 1) == '#' && tile_at(x, z + 1) == '#';
                Vec3 ctr = tile_center(x, z, 0.0f);
                Vec3 half = d->along_x ? vec3(0.2f, 0.0f, TILE * 0.5f) : vec3(TILE * 0.5f, 0.0f, 0.2f);
                d->closed = aabb_make(vec3(ctr.x - half.x, 0.0f, ctr.z - half.z), vec3(ctr.x + half.x, LINTEL_Y, ctr.z + half.z));
                d->open = 0.0f;
                d->phys = phys_world_add_static(&g_k.world, shape_from_aabb(d->closed), 0.0f, 0.0f);
            } else if (c == 'i') {
                add_static_light(tile_center(x, z, 1.7f), vec3(2.6f, 1.45f, 0.6f), 9.0f);
            } else if (c == 'X') {
                g_k.exit_pos = tile_center(x, z, 0.0f);
                add_static_light(tile_center(x, z, 3.0f), vec3(1.1f, 1.7f, 2.6f), 11.0f);
            }
        }
    }
    /* Lav kızıllığı: seyrek ızgarada lav karolarının üstüne ışık */
    for (i32 z = 0; z < MAP_H; z++) {
        for (i32 x = 0; x < MAP_W; x++) {
            if (tile_at(x, z) == '~' && (x % 3) == 1 && (z % 2) == 0) {
                add_static_light(tile_center(x, z, 0.8f), vec3(3.6f, 1.2f, 0.3f), 10.0f);
            }
        }
    }
}

/* ---------- Oyun kurulumu ---------- */

static void say(const char* text, f32 seconds) {
    u32 i = 0;
    while (text[i] && i + 1 < sizeof(g_k.msg)) { g_k.msg[i] = text[i]; i++; }
    g_k.msg[i] = 0;
    g_k.msg_time = seconds;
}

static void spawn_level(b32 keep_inventory) {
    g_k.enemy_count = 0;
    g_k.pickup_count = 0;
    for (i32 z = 0; z < MAP_H; z++) {
        for (i32 x = 0; x < MAP_W; x++) {
            char c = tile_at(x, z);
            Vec3 p = tile_center(x, z, tile_floor(c));
            if (c == 'P') {
                g_k.start_feet = p;
            } else if ((c == 'g' || c == 's' || c == 'e') && g_k.enemy_count < MAX_ENEMIES) {
                Enemy* e = &g_k.enemies[g_k.enemy_count++];
                __builtin_memset(e, 0, sizeof(*e));
                e->kind = (c == 'g') ? EN_GOLGE : (c == 's') ? EN_TUKURGEN : EN_GOZCU;
                e->feet = (c == 'e') ? vec3(p.x, p.y + 2.4f, p.z) : p;
                e->hp = (c == 'g') ? 50.0f : (c == 's') ? 75.0f : 40.0f;
                e->alive = TRUE;
                e->anim = (f32)(x * 7 + z * 3) * 0.37f;
                e->steer_sign = ((x + z) & 1) ? 1.0f : -1.0f;
            } else if (g_k.pickup_count < MAX_PICKUPS) {
                PickupKind k;
                switch (c) {
                case 'h': k = PK_HEALTH; break;
                case 'a': k = PK_SHELLS; break;
                case 'c': k = PK_KOR; break;
                case 'w': k = PK_W_SACMALI; break;
                case 'f': k = PK_W_KOR; break;
                case 'k': k = PK_KEY; break;
                default:  continue;
                }
                Pickup* pk = &g_k.pickups[g_k.pickup_count++];
                pk->kind = k;
                pk->pos = vec3(p.x, p.y + 0.6f, p.z);
                pk->taken = FALSE;
            }
        }
    }
    for (u32 i = 0; i < g_k.door_count; i++) {
        g_k.doors[i].open = 0.0f;
        g_k.world.statics[g_k.doors[i].phys].shape.as.aabb = g_k.doors[i].closed;
    }
    __builtin_memset(g_k.proj, 0, sizeof(g_k.proj));
    __builtin_memset(g_k.parts, 0, sizeof(g_k.parts));
    g_k.feet = g_k.start_feet;
    g_k.vel = vec3(0.0f, 0.0f, 0.0f);
    g_k.yaw = -M_PI * 0.5f;      /* +x yönüne bak (ilk kapıya) */
    g_k.pitch = 0.0f;
    g_k.grounded = FALSE;
    g_k.coyote = g_k.jump_buf = g_k.step_acc = 0.0f;
    if (!keep_inventory) {
        g_k.hp = MAX_HP;
        g_k.has_weapon[W_CAKMAK] = TRUE;
        g_k.has_weapon[W_SACMALI] = FALSE;
        g_k.has_weapon[W_KOR] = FALSE;
        g_k.shells = 0; g_k.kor = 0;
        g_k.weapon = W_CAKMAK;
        g_k.total_kills = 0;
        g_k.total_enemies = 0;
        g_k.play_time = 0.0f;
    }
    g_k.kills = 0;
    g_k.fire_cd = g_k.recoil = g_k.flash_t = 0.0f;
    g_k.hurt_flash = g_k.shake = g_k.pickup_flash = 0.0f;
    g_k.has_key = FALSE;
    g_k.state = KS_PLAY;
    g_k.flow_tx = g_k.flow_tz = -1;
    say(k_levels[g_k.level].intro, 4.0f);
}

/* Bölümü (yeniden) kurar: seviye arenası sıfırlanır, ağlar/fizik/akış alanı yeniden üretilir */
static void load_level(u32 idx, b32 keep_inventory, Arena* scratch) {
    if (idx >= LEVEL_COUNT) idx = 0;
    const LevelDef* L = &k_levels[idx];
    g_k.level = idx;
    g_k.map_rows = L->rows;
    g_k.map_w = L->w;
    g_k.map_h = L->h;
    arena_reset(&g_k.lv_arena);
    build_level(&g_k.lv_arena, scratch);
    spawn_level(keep_inventory);
}

static void reset_game(Arena* scratch) {
    load_level(0, FALSE, scratch);
}

void scene_kuyu_init(Arena* perm, Arena* scratch) {
    __builtin_memset(&g_k, 0, sizeof(g_k));
    rng_seed(&g_k.rng, 0x4B555955u);
    fb_init(&g_k.low, perm, INNER_W, INNER_H);
    fb_init_depth(&g_k.low, perm);

    arena_init(&g_k.lv_arena, ARENA_PUSH_ARRAY(perm, u8, LEVEL_ARENA), LEVEL_ARENA);
    make_tex(&g_k.tex_wall, perm, 64, gen_bricks);
    make_tex(&g_k.tex_floor, perm, 64, gen_flags);
    make_tex(&g_k.tex_ceil, perm, 32, gen_planks);
    make_tex(&g_k.tex_door, perm, 64, gen_door);
    make_tex(&g_k.tex_lava, perm, 64, gen_lava);
    make_tex(&g_k.tex_skin, perm, 32, gen_skin);
    tex_init(&g_k.tex_white, perm, 4, 4, 0);
    tex_gen_checker(&g_k.tex_white, 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 255, 255, 255));

    g_k.cube   = mesh_cube(perm, 0.5f);
    g_k.sphere = mesh_sphere(perm, 1.0f, 12, 8);

    usize mark = arena_mark(scratch);
    g_k.font_ok = ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &g_k.font) &&
                  font_atlas_build(&g_k.font, 14.0f, 0, 0, perm, scratch, &g_k.font_small) &&
                  font_atlas_build(&g_k.font, 30.0f, 0, 0, perm, scratch, &g_k.font_big);
    arena_pop_to(scratch, mark);
    if (!g_k.font_ok) LOG("Kuyu: Matoki Sans atlas kurulamadi");

    g_k.music_on = TRUE;
    g_k.retro_on = TRUE;
    g_k.snap_on = TRUE;
    reset_game(scratch);
}

void scene_kuyu_leave(void) {
    stop_music();
}

/* ---------- Yardımcılar ---------- */

static Capsule body_capsule(Vec3 feet, f32 r, f32 h) {
    return capsule_make(vec3(feet.x, feet.y + r, feet.z), vec3(feet.x, feet.y + h - r, feet.z), r);
}

static f32 approach(f32 v, f32 target, f32 step) {
    if (v < target) return m_min(v + step, target);
    return m_max(v - step, target);
}

static Vec3 eye_pos(void) { return vec3(g_k.feet.x, g_k.feet.y + EYE_H, g_k.feet.z); }

static Vec3 view_dir(f32 yaw, f32 pitch) {
    f32 cp = m_cos(pitch);
    return vec3(-m_sin(yaw) * cp, m_sin(pitch), -m_cos(yaw) * cp);
}

/* Işın–küre: t (>= 0) veya -1 */
static f32 ray_sphere(Vec3 o, Vec3 d, Vec3 c, f32 r) {
    Vec3 oc = vec3_sub(o, c);
    f32 b = vec3_dot(oc, d);
    f32 cc = vec3_dot(oc, oc) - r * r;
    f32 disc = b * b - cc;
    if (disc < 0.0f) return -1.0f;
    f32 s = m_sqrt(disc);
    f32 t = -b - s;
    if (t < 0.0f) t = -b + s;
    return t >= 0.0f ? t : -1.0f;
}

static f32 world_ray(Vec3 o, Vec3 d, f32 tmax, Vec3* out_n) {
    RayHit h;
    if (phys_world_raycast(&g_k.world, ray_make(o, d), tmax, &h)) {
        if (out_n) *out_n = h.normal;
        return h.t;
    }
    return tmax;
}

/* Gözcü için feet = küre merkezi (uçar) */
static Vec3 enemy_center(const Enemy* e) {
    if (e->kind == EN_GOZCU) return e->feet;
    return vec3(e->feet.x, e->feet.y + (e->kind == EN_GOLGE ? 1.0f : 0.75f), e->feet.z);
}
static f32 enemy_radius(const Enemy* e) {
    return e->kind == EN_GOLGE ? 0.62f : e->kind == EN_TUKURGEN ? 0.72f : 0.55f;
}

static b32 line_of_sight(Vec3 a, Vec3 b) {
    Vec3 d = vec3_sub(b, a);
    f32 l = vec3_len(d);
    if (l < 1e-4f) return TRUE;
    return world_ray(a, vec3_scale(d, 1.0f / l), l, NULL) >= l - 0.05f;
}

static void spawn_particles(Vec3 p, Vec3 base_vel, u32 n, Vec4 col, f32 speed, f32 life, f32 size, b32 grav) {
    for (u32 i = 0; i < n; i++) {
        Particle* q = &g_k.parts[g_k.part_next];
        g_k.part_next = (g_k.part_next + 1u) % MAX_PARTICLES;
        Vec3 r = vec3(rng_f32(&g_k.rng) * 2.0f - 1.0f, rng_f32(&g_k.rng) * 2.0f - 1.0f, rng_f32(&g_k.rng) * 2.0f - 1.0f);
        q->pos = p;
        q->vel = vec3_add(base_vel, vec3_scale(r, speed));
        q->life = q->max_life = life * (0.6f + 0.8f * rng_f32(&g_k.rng));
        q->size = size;
        q->col = col;
        q->grav = grav;
    }
}

static void hurt_player(f32 dmg, Vec3 from) {
    if (g_k.state != KS_PLAY || g_k.god) { g_k.hurt_flash = m_max(g_k.hurt_flash, 0.15f); return; }
    g_k.hp -= dmg;
    g_k.hurt_flash = m_min(g_k.hurt_flash + dmg * 0.016f, 0.5f);
    g_k.shake = m_min(g_k.shake + dmg * 0.012f, 0.35f);
    Vec3 push = vec3_sub(g_k.feet, from);
    push.y = 0.0f;
    f32 l = vec3_len(push);
    if (l > 1e-3f) g_k.vel = vec3_add(g_k.vel, vec3_scale(push, dmg * 0.18f / l));
    sfx(&k_sfx_hurt);
    if (g_k.hp <= 0.0f) {
        g_k.hp = 0.0f;
        g_k.state = KS_DEAD;
        sfx(&k_sfx_death);
        say("OLDUN.  R: yeniden dene", 1e9f);
    }
}

static void hurt_enemy(Enemy* e, f32 dmg, Vec3 dir) {
    if (!e->alive) return;
    e->hp -= dmg;
    e->pain = 0.12f;
    e->alert = TRUE;
    e->last_seen = g_k.feet;
    e->vel = vec3_add(e->vel, vec3_scale(vec3(dir.x, 0.0f, dir.z), dmg * 0.06f));
    Vec4 blood = (e->kind == EN_GOLGE) ? vec4(0.35f, 0.1f, 0.5f, 1.0f)
               : (e->kind == EN_TUKURGEN) ? vec4(0.4f, 0.75f, 0.15f, 1.0f) : vec4(0.85f, 0.1f, 0.08f, 1.0f);
    spawn_particles(enemy_center(e), vec3_scale(dir, 2.0f), 5, blood, 3.0f, 0.5f, 0.07f, TRUE);
    if (e->hp <= 0.0f) {
        e->alive = FALSE;
        e->dead_t = 0.0f;
        g_k.kills++;
        g_k.total_kills++;
        spawn_particles(enemy_center(e), vec3(0, 2.0f, 0), 18, blood, 4.0f, 0.8f, 0.1f, TRUE);
        sfx_at(e->kind == EN_GOLGE ? &k_sfx_g_die : e->kind == EN_TUKURGEN ? &k_sfx_t_die : &k_sfx_e_die, e->feet);
    } else {
        sfx_at(&k_sfx_hit, e->feet);
    }
}

/* ---------- Silahlar ---------- */

static Proj* proj_alloc(void) {
    for (u32 i = 0; i < MAX_PROJ; i++) if (!g_k.proj[i].active) return &g_k.proj[i];
    return NULL;
}

/* Hitscan: en yakın düşman (ışın–küre) veya duvar; isabet noktasına kıvılcım */
static void fire_ray(Vec3 o, Vec3 d, f32 dmg) {
    Vec3 wn = vec3(0, 1, 0);
    f32 tw = world_ray(o, d, 80.0f, &wn);
    Enemy* best = NULL;
    f32 tb = tw;
    for (u32 i = 0; i < g_k.enemy_count; i++) {
        Enemy* e = &g_k.enemies[i];
        if (!e->alive) continue;
        f32 t = ray_sphere(o, d, enemy_center(e), enemy_radius(e));
        if (t >= 0.0f && t < tb) { tb = t; best = e; }
    }
    if (best) {
        hurt_enemy(best, dmg, d);
    } else if (tw < 80.0f) {
        Vec3 p = vec3_add(o, vec3_scale(d, tw - 0.02f));
        spawn_particles(p, vec3_scale(wn, 1.5f), 3, vec4(1.0f, 0.85f, 0.4f, 1.0f), 1.8f, 0.25f, 0.05f, TRUE);
    }
}

static void fire_weapon(void) {
    Vec3 o = eye_pos();
    Vec3 f = view_dir(g_k.yaw, g_k.pitch);
    Vec3 rt = vec3(m_cos(g_k.yaw), 0.0f, -m_sin(g_k.yaw));
    Vec3 up = vec3_cross(rt, f);
    switch (g_k.weapon) {
    case W_CAKMAK:
        fire_ray(o, f, 18.0f);
        g_k.fire_cd = 0.38f; g_k.recoil = 0.6f;
        sfx(&k_sfx_cakmak);
        break;
    case W_SACMALI:
        if (g_k.shells == 0) return;
        g_k.shells--;
        for (u32 i = 0; i < 8; i++) {
            f32 a = rng_f32(&g_k.rng) * M_TAU, s = m_sqrt(rng_f32(&g_k.rng)) * 0.075f;
            Vec3 d = vec3_add(f, vec3_add(vec3_scale(rt, m_cos(a) * s), vec3_scale(up, m_sin(a) * s)));
            fire_ray(o, vec3_norm(d), 9.0f);
        }
        g_k.fire_cd = 0.85f; g_k.recoil = 1.0f;
        g_k.shake = m_max(g_k.shake, 0.08f);
        sfx(&k_sfx_sacma);
        break;
    case W_KOR: {
        if (g_k.kor == 0) return;
        Proj* p = proj_alloc();
        if (!p) return;
        g_k.kor--;
        p->active = TRUE;
        p->from_player = TRUE;
        p->pos = vec3_add(o, vec3_add(vec3_scale(f, 0.5f), vec3_scale(rt, 0.15f)));
        p->vel = vec3_scale(f, 24.0f);
        p->life = 4.0f;
        p->dmg = 55.0f;
        p->splash = 3.2f;
        p->kind = 0u;
        g_k.fire_cd = 0.9f; g_k.recoil = 0.9f;
        sfx(&k_sfx_kor);
        break;
    }
    default: break;
    }
    g_k.flash_t = 0.06f;
}

static void explode(Vec3 p, f32 dmg, f32 radius) {
    spawn_particles(p, vec3(0, 1.5f, 0), 26, vec4(1.0f, 0.55f, 0.15f, 1.0f), 6.0f, 0.5f, 0.12f, FALSE);
    spawn_particles(p, vec3(0, 0.5f, 0), 10, vec4(1.0f, 0.9f, 0.5f, 1.0f), 3.0f, 0.35f, 0.18f, FALSE);
    sfx_at(&k_sfx_boom, p);
    g_k.shake = m_max(g_k.shake, 0.25f / (1.0f + vec3_len(vec3_sub(p, g_k.feet)) * 0.2f));
    for (u32 i = 0; i < g_k.enemy_count; i++) {
        Enemy* e = &g_k.enemies[i];
        if (!e->alive) continue;
        Vec3 d = vec3_sub(enemy_center(e), p);
        f32 l = vec3_len(d);
        if (l < radius && line_of_sight(p, enemy_center(e))) {
            hurt_enemy(e, dmg * (1.0f - l / radius), l > 1e-3f ? vec3_scale(d, 1.0f / l) : vec3(0, 1, 0));
        }
    }
    Vec3 pc = vec3(g_k.feet.x, g_k.feet.y + 0.8f, g_k.feet.z);
    f32 lp = vec3_len(vec3_sub(pc, p));
    if (lp < radius) hurt_player(dmg * 0.4f * (1.0f - lp / radius), p);
    /* geçici ışık parçacığı olarak sayılır (bkz. collect_lights: büyük turuncu parçacıklar) */
}

static void update_projectiles(f32 dt) {
    for (u32 i = 0; i < MAX_PROJ; i++) {
        Proj* p = &g_k.proj[i];
        if (!p->active) continue;
        p->life -= dt;
        Vec3 step = vec3_scale(p->vel, dt);
        f32 len = vec3_len(step);
        Vec3 dir = vec3_scale(step, 1.0f / m_max(len, 1e-6f));
        f32 tw = world_ray(p->pos, dir, len, NULL);
        f32 th = len + 1.0f;
        Enemy* he = NULL;
        b32 hit_player = FALSE;
        if (p->from_player) {
            for (u32 k = 0; k < g_k.enemy_count; k++) {
                Enemy* e = &g_k.enemies[k];
                if (!e->alive) continue;
                f32 t = ray_sphere(p->pos, dir, enemy_center(e), enemy_radius(e) + 0.1f);
                if (t >= 0.0f && t <= len && t < th) { th = t; he = e; }
            }
        } else {
            Vec3 pc = vec3(g_k.feet.x, g_k.feet.y + 0.8f, g_k.feet.z);
            f32 t = ray_sphere(p->pos, dir, pc, 0.55f);
            if (t >= 0.0f && t <= len) { th = t; hit_player = TRUE; }
        }
        if (th <= len && th <= tw) {
            Vec3 hp = vec3_add(p->pos, vec3_scale(dir, th));
            if (p->splash > 0.0f) explode(hp, p->dmg, p->splash);
            else if (he) hurt_enemy(he, p->dmg, dir);
            else if (hit_player) {
                hurt_player(p->dmg, p->pos);
                spawn_particles(hp, vec3(0, 0, 0), 8, vec4(0.5f, 1.0f, 0.3f, 1.0f), 2.5f, 0.35f, 0.06f, TRUE);
            }
            if (he && p->splash > 0.0f) hurt_enemy(he, p->dmg * 0.5f, dir);
            p->active = FALSE;
        } else if (tw < len || p->life <= 0.0f) {
            Vec3 hp = vec3_add(p->pos, vec3_scale(dir, m_max(tw - 0.05f, 0.0f)));
            if (p->splash > 0.0f) explode(hp, p->dmg, p->splash);
            else spawn_particles(hp, vec3(0, 0, 0), 8, vec4(0.5f, 1.0f, 0.3f, 1.0f), 2.5f, 0.35f, 0.06f, TRUE);
            p->active = FALSE;
        } else {
            p->pos = vec3_add(p->pos, step);
        }
    }
}

/* ---------- Hareket (oyuncu + yürüyen düşmanlar) ---------- */

/* Kapsül kaydırma + basamak çıkma: yerdeyken duvara takılırsa STEP_H yukarıdan tekrar dener,
 * yatayda daha çok ilerlerse kabul eder. feet/vel güncellenir. */
static SlideResult move_body(Vec3* feet, Vec3* vel, f32 r, f32 h, b32 was_grounded, f32 dt) {
    Vec3 start = *feet;
    Vec3 v = *vel;
    Capsule cap = body_capsule(*feet, r, h);
    SlideResult res = phys_move_and_slide(&g_k.world, &cap, &v, dt, PHYS_DEFAULT_SLOPE_COS);
    if (was_grounded && res.hit_wall) {
        Capsule c2 = body_capsule(*feet, r, h);
        Vec3 up = vec3(0.0f, STEP_H / dt, 0.0f);
        phys_move_and_slide(&g_k.world, &c2, &up, dt, PHYS_DEFAULT_SLOPE_COS);
        /* Yatay deneme en az yarıçap kadar ilerler: yoksa alt yarım küre basamak kenarına oturur ve
         * kenar normali (y < cos 45°) zemin sayılmaz */
        Vec3 hv = vec3(vel->x, 0.0f, vel->z);
        f32 hl = m_sqrt(hv.x * hv.x + hv.z * hv.z) * dt;
        Vec3 probe = (hl > 1e-5f && hl < r + 0.02f) ? vec3_scale(hv, (r + 0.02f) / hl) : hv;
        phys_move_and_slide(&g_k.world, &c2, &probe, dt, PHYS_DEFAULT_SLOPE_COS);
        Vec3 dn = vec3(0.0f, -(STEP_H + 0.05f) / dt, 0.0f);
        SlideResult r3 = phys_move_and_slide(&g_k.world, &c2, &dn, dt, PHYS_DEFAULT_SLOPE_COS);
        f32 d1x = cap.a.x - start.x, d1z = cap.a.z - start.z;
        f32 d2x = c2.a.x - start.x, d2z = c2.a.z - start.z;
        if (r3.on_ground && d2x * d2x + d2z * d2z > d1x * d1x + d1z * d1z + 1e-6f) {
            cap = c2;
            v = vec3(hv.x, 0.0f, hv.z);
            res = r3;
        }
    }
    *vel = v;
    *feet = vec3(cap.a.x, cap.a.y - r, cap.a.z);
    if (res.on_ground && vel->y < 0.0f) vel->y = 0.0f;
    if (res.hit_ceiling && vel->y > 0.0f) vel->y = 0.0f;
    return res;
}

/* ---------- Yol bulma: oyuncu karosundan BFS akış alanı ---------- */

static b32 tile_walkable(char c) { return c != '#' && c != '~' && c != 'D'; }
static b32 tile_flyable(char c)  { return c != '#' && c != 'D'; }

/* f[karo] = oyuncuya adım sayısı. Kenar komşudan (nb) mevcut karoya yürünebilir olmalı:
 * yerde yükselme STEP_H'yi aşamaz (inmek serbest), havada yalnız duvar/mühürlü kapı engel. */
static void flow_build(u16* f, b32 air, i32 tx, i32 tz, Arena* scratch) {
    i32 w = MAP_W, h = MAP_H;
    for (i32 i = 0; i < w * h; i++) f[i] = FLOW_INF;
    if (tx < 0 || tz < 0 || tx >= w || tz >= h) return;
    usize mark = arena_mark(scratch);
    i32* q = ARENA_PUSH_ARRAY(scratch, i32, (usize)(w * h));
    i32 head = 0, tail = 0;
    f[tz * w + tx] = 0;
    q[tail++] = tz * w + tx;
    static const i32 dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
    while (head < tail) {
        i32 cur = q[head++];
        i32 cx = cur % w, cz = cur / w;
        f32 hc = tile_floor(tile_at(cx, cz));
        for (u32 k = 0; k < 4; k++) {
            i32 nx = cx + dx[k], nz = cz + dz[k];
            if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
            i32 ni = nz * w + nx;
            if (f[ni] != FLOW_INF) continue;
            char c = tile_at(nx, nz);
            if (air ? !tile_flyable(c) : !tile_walkable(c)) continue;
            if (!air && hc - tile_floor(c) > STEP_H + 0.01f) continue;
            f[ni] = (u16)(f[cur] + 1u);
            q[tail++] = ni;
        }
    }
    arena_pop_to(scratch, mark);
}

static void flow_update(f32 dt, Arena* scratch) {
    i32 tx = (i32)m_floor(g_k.feet.x / TILE), tz = (i32)m_floor(g_k.feet.z / TILE);
    g_k.flow_timer -= dt;
    /* Oyuncu lavda / havadaysa son geçerli alan korunur */
    if (!tile_walkable(tile_at(tx, tz))) return;
    if (tx == g_k.flow_tx && tz == g_k.flow_tz && g_k.flow_timer > 0.0f) return;
    flow_build(g_k.flow_ground, FALSE, tx, tz, scratch);
    flow_build(g_k.flow_air, TRUE, tx, tz, scratch);
    g_k.flow_tx = tx;
    g_k.flow_tz = tz;
    g_k.flow_timer = 0.5f;
}

static u16 flow_at(const u16* f, i32 x, i32 z) {
    if (x < 0 || z < 0 || x >= MAP_W || z >= MAP_H) return FLOW_INF;
    return f[z * MAP_W + x];
}

/* Akış alanında bulunduğu karodan daha yakın komşu karonun merkezine yön (xz).
 * Çapraz yalnız iki dik komşu da geçilebilirse (köşe kesme yok). FALSE: aynı karo veya ulaşılamaz. */
static b32 flow_dir(const u16* f, Vec3 pos, Vec3* out, u16* out_steps) {
    i32 tx = (i32)m_floor(pos.x / TILE), tz = (i32)m_floor(pos.z / TILE);
    u16 cur = flow_at(f, tx, tz);
    if (out_steps) *out_steps = cur;
    if (cur == FLOW_INF || cur == 0) return FALSE;
    i32 bx = tx, bz = tz;
    u16 best = cur;
    for (i32 dz = -1; dz <= 1; dz++) {
        for (i32 dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0) continue;
            u16 v = flow_at(f, tx + dx, tz + dz);
            if (dx != 0 && dz != 0) {
                if (flow_at(f, tx + dx, tz) == FLOW_INF || flow_at(f, tx, tz + dz) == FLOW_INF) continue;
                if (v != FLOW_INF && v + 1u > best) continue;   /* çapraz yalnız gerçekten kısaltıyorsa */
            }
            if (v < best) { best = v; bx = tx + dx; bz = tz + dz; }
        }
    }
    if (bx == tx && bz == tz) return FALSE;
    Vec3 c = tile_center(bx, bz, 0.0f);
    Vec3 d = vec3(c.x - pos.x, 0.0f, c.z - pos.z);
    f32 l = vec3_len(d);
    if (l < 1e-4f) return FALSE;
    *out = vec3_scale(d, 1.0f / l);
    return TRUE;
}

/* Yerdeki düşman için: yönün 0.8 m önü lav veya uçurumsa (STEP_H'den fazla iniş değil, lav) yönü iptal et */
static Vec3 avoid_hazard(Vec3 feet, Vec3 wish) {
    if (vec3_len(wish) < 1e-3f) return wish;
    Vec3 p = vec3_add(feet, vec3_scale(vec3_norm(wish), 0.8f));
    char c = tile_at((i32)m_floor(p.x / TILE), (i32)m_floor(p.z / TILE));
    if (c == '~') return vec3(0.0f, 0.0f, 0.0f);
    return wish;
}

/* ---------- Düşmanlar ---------- */

static void enemy_fire(Enemy* e, Vec3 from, Vec3 target, f32 speed, f32 dmg, u32 kind, f32 spread) {
    Proj* p = proj_alloc();
    if (!p) return;
    Vec3 ad = vec3_norm(vec3_sub(target, from));
    if (spread > 0.0f) {
        ad = vec3_norm(vec3_add(ad, vec3((rng_f32(&g_k.rng) - 0.5f) * spread, (rng_f32(&g_k.rng) - 0.5f) * spread,
                                         (rng_f32(&g_k.rng) - 0.5f) * spread)));
    }
    p->active = TRUE;
    p->from_player = FALSE;
    p->kind = kind;
    p->pos = vec3_add(from, vec3_scale(ad, 0.7f));
    p->vel = vec3_scale(ad, speed);
    p->life = 5.0f;
    p->dmg = dmg;
    p->splash = 0.0f;
    (void)e;
}

static void enemy_think(Enemy* e, u32 idx, f32 dt) {
    e->anim += dt;
    if (e->pain > 0.0f) e->pain -= dt;
    if (!e->alive) {
        e->dead_t += dt;
        if (e->kind == EN_GOZCU) {   /* düşen Gözcü yere iner */
            f32 fl = tile_floor(tile_at((i32)m_floor(e->feet.x / TILE), (i32)m_floor(e->feet.z / TILE))) + 0.3f;
            if (fl < LAVA_Y) fl = LAVA_Y + 0.3f;
            e->feet.y = m_max(e->feet.y - 7.0f * dt, fl);
        }
        return;
    }
    e->cooldown = m_max(e->cooldown - dt, 0.0f);
    b32 fly = e->kind == EN_GOZCU;

    Vec3 eye = fly ? e->feet : vec3(e->feet.x, e->feet.y + (e->kind == EN_GOLGE ? 1.5f : 1.0f), e->feet.z);
    Vec3 pe = eye_pos();
    Vec3 to = vec3_sub(pe, eye);
    f32 dist = vec3_len(to);
    b32 sees = g_k.state == KS_PLAY && dist < 30.0f && line_of_sight(eye, pe);
    if (sees) {
        if (!e->alert) sfx_at(&k_sfx_g_alert, e->feet);
        e->alert = TRUE;
        e->last_seen = g_k.feet;
    }

    Vec3 wish = vec3(0.0f, 0.0f, 0.0f);
    f32 speed = e->kind == EN_GOLGE ? 5.2f : e->kind == EN_TUKURGEN ? 2.6f : 4.6f;
    if (e->alert && g_k.state == KS_PLAY) {
        Vec3 tgt = vec3_sub(g_k.feet, e->feet);
        tgt.y = 0.0f;
        f32 tl = vec3_len(tgt);
        Vec3 dir = tl > 1e-3f ? vec3_scale(tgt, 1.0f / tl) : vec3(0, 0, 0);
        if (tl > 1e-3f && (sees || fly)) e->yaw = m_atan2(-dir.x, -dir.z);

        /* Yol: akış alanı; aynı karo / ulaşılamazsa doğrudan (görüyorsa) */
        Vec3 path;
        u16 steps = FLOW_INF;
        b32 has_path = flow_dir(fly ? g_k.flow_air : g_k.flow_ground, e->feet, &path, &steps);
        Vec3 chase = has_path ? path : (sees || steps == 0 ? dir : vec3(0, 0, 0));
        if (!sees && has_path) e->yaw = m_atan2(-path.x, -path.z);

        if (e->kind == EN_GOLGE) {
            if (tl > 1.1f) wish = (sees && steps <= 1u) ? dir : chase;
            if (sees && dist < 1.9f && e->cooldown <= 0.0f) {
                e->cooldown = 0.9f;
                sfx_at(&k_sfx_claw, e->feet);
                hurt_player(12.0f, e->feet);
            }
        } else if (e->kind == EN_TUKURGEN) {
            /* 6–13 m aralığını korur, görürse tükürür */
            if (!sees || tl > 13.0f) wish = chase;
            else if (tl < 6.0f) wish = vec3_scale(dir, -1.0f);
            else wish = vec3(-dir.z * e->steer_sign * 0.6f, 0.0f, dir.x * e->steer_sign * 0.6f);
            if (sees && e->cooldown <= 0.0f && dist < 26.0f) {
                Vec3 aim = vec3_add(pe, vec3_scale(g_k.vel, dist / 12.0f * 0.5f));
                aim.y -= 0.35f;
                enemy_fire(e, eye, aim, 12.0f, 14.0f, 1u, 0.0f);
                sfx_at(&k_sfx_spit, e->feet);
                e->cooldown = 1.7f + rng_f32(&g_k.rng) * 0.6f;
            }
        } else {
            /* Gözcü: 5–10 m mesafede süzülür, yan kayar, 3'lü kızıl ok yağdırır */
            if (!sees || tl > 10.0f) wish = chase;
            else if (tl < 5.0f) wish = vec3_scale(dir, -1.0f);
            else wish = vec3(-dir.z * e->steer_sign, 0.0f, dir.x * e->steer_sign);
            if (e->burst > 0u) {
                e->burst_t -= dt;
                if (e->burst_t <= 0.0f) {
                    if (sees) enemy_fire(e, eye, vec3(pe.x, pe.y - 0.3f, pe.z), 17.0f, 7.0f, 2u, 0.06f);
                    sfx_at(&k_sfx_bolt, e->feet);
                    e->burst--;
                    e->burst_t = 0.13f;
                }
            } else if (sees && e->cooldown <= 0.0f && dist < 24.0f) {
                e->burst = 3u;
                e->burst_t = 0.0f;
                e->cooldown = 2.2f + rng_f32(&g_k.rng) * 0.8f;
            }
            if (rng_f32(&g_k.rng) < dt * 0.4f) e->steer_sign = -e->steer_sign;
        }
        if (!fly && e->steer_t > 0.0f) {
            e->steer_t -= dt;
            wish = vec3_norm(vec3_add(wish, vec3(-wish.z * e->steer_sign * 1.5f, 0.0f, wish.x * e->steer_sign * 1.5f)));
        }
    }

    /* Ayrışma: düşmanlar birbirine ve oyuncuya girmesin */
    for (u32 j = 0; j < g_k.enemy_count; j++) {
        if (j == idx || !g_k.enemies[j].alive) continue;
        Vec3 d = vec3_sub(e->feet, g_k.enemies[j].feet);
        d.y = 0.0f;
        f32 l = vec3_len(d);
        if (l < 1.2f && l > 1e-4f) wish = vec3_add(wish, vec3_scale(d, (1.2f - l) / l));
    }
    Vec3 dp = vec3_sub(e->feet, g_k.feet);
    dp.y = 0.0f;
    f32 lp = vec3_len(dp);
    if (lp < 1.0f && lp > 1e-4f) wish = vec3_add(wish, vec3_scale(dp, (1.0f - lp) / lp * 2.0f));

    if (fly) {
        /* İrtifa: oyuncunun gözünün ~1 m üstü, zeminin en az 1.6 m üstü, tavanın 0.7 m altı */
        i32 tx = (i32)m_floor(e->feet.x / TILE), tz = (i32)m_floor(e->feet.z / TILE);
        char tc = tile_at(tx, tz);
        f32 fl = tile_floor(tc) < LAVA_Y + 0.1f ? 0.0f : tile_floor(tc);
        f32 want_y = e->alert ? pe.y + 1.0f : fl + 2.4f;
        want_y = m_clamp(want_y + 0.25f * m_sin(e->anim * 2.3f), fl + 1.6f, tile_ceil(tc) - 0.7f);
        e->vel.x = approach(e->vel.x, wish.x * speed, 14.0f * dt);
        e->vel.z = approach(e->vel.z, wish.z * speed, 14.0f * dt);
        e->vel.y = approach(e->vel.y, m_clamp((want_y - e->feet.y) * 3.0f, -4.0f, 4.0f), 12.0f * dt);
        Sphere sp = sphere_make(e->feet, 0.45f);
        phys_move_and_slide_sphere(&g_k.world, &sp, &e->vel, dt, PHYS_DEFAULT_SLOPE_COS);
        e->feet = sp.center;
        return;
    }

    wish = avoid_hazard(e->feet, wish);
    f32 acc = e->grounded ? 30.0f : 6.0f;
    e->vel.x = approach(e->vel.x, wish.x * speed, acc * dt);
    e->vel.z = approach(e->vel.z, wish.z * speed, acc * dt);
    e->vel.y += GRAVITY * dt;
    f32 r = e->kind == EN_GOLGE ? 0.45f : 0.6f;
    SlideResult res = move_body(&e->feet, &e->vel, r, e->kind == EN_GOLGE ? 1.9f : 1.4f, e->grounded, dt);
    e->grounded = res.on_ground;
    if (res.hit_wall && e->steer_t <= 0.0f && vec3_len(wish) > 0.1f) {
        e->steer_t = 0.4f;
        e->steer_sign = -e->steer_sign;
    }
    /* Lav düşmanı da yakar */
    i32 tx = (i32)m_floor(e->feet.x / TILE), tz = (i32)m_floor(e->feet.z / TILE);
    if (tile_at(tx, tz) == '~' && e->feet.y < -0.3f) hurt_enemy(e, 60.0f * dt, vec3(0, 1, 0));
}

/* ---------- Oyuncu ---------- */

static void player_step(Vec3 wish) {
    f32 acc = g_k.grounded ? ACC_GROUND : ACC_AIR;
    g_k.vel.x = approach(g_k.vel.x, wish.x * RUN_SPEED, acc * STEP_DT);
    g_k.vel.z = approach(g_k.vel.z, wish.z * RUN_SPEED, acc * STEP_DT);
    g_k.vel.y += GRAVITY * STEP_DT;

    g_k.coyote   = g_k.grounded ? COYOTE_TIME : m_max(g_k.coyote - STEP_DT, 0.0f);
    g_k.jump_buf = m_max(g_k.jump_buf - STEP_DT, 0.0f);
    if (g_k.jump_buf > 0.0f && g_k.coyote > 0.0f) {
        g_k.vel.y = JUMP_SPEED;
        g_k.jump_buf = 0.0f;
        g_k.coyote = 0.0f;
        g_k.grounded = FALSE;
        sfx(&k_sfx_jump);
    }
    SlideResult res = move_body(&g_k.feet, &g_k.vel, PLAYER_R, PLAYER_H, g_k.grounded, STEP_DT);
    g_k.grounded = res.on_ground;
}

static void take_pickups(void) {
    for (u32 i = 0; i < g_k.pickup_count; i++) {
        Pickup* p = &g_k.pickups[i];
        if (p->taken) continue;
        Vec3 d = vec3_sub(p->pos, vec3(g_k.feet.x, g_k.feet.y + 0.7f, g_k.feet.z));
        if (vec3_dot(d, d) > 1.1f * 1.1f) continue;
        b32 take = TRUE;
        switch (p->kind) {
        case PK_HEALTH:
            if (g_k.hp >= MAX_HP) { take = FALSE; break; }
            g_k.hp = m_min(g_k.hp + 25.0f, MAX_HP);
            say("+25 can", 1.5f);
            break;
        case PK_SHELLS:
            g_k.shells = (u32)m_min((f32)(g_k.shells + 10u), 50.0f);
            say("+10 sacma", 1.5f);
            break;
        case PK_KOR:
            g_k.kor = (u32)m_min((f32)(g_k.kor + 5u), 25.0f);
            say("+5 kor", 1.5f);
            break;
        case PK_W_SACMALI:
            g_k.has_weapon[W_SACMALI] = TRUE;
            g_k.shells += 12u;
            g_k.weapon = W_SACMALI;
            say("SACMALI! (2)", 2.5f);
            break;
        case PK_W_KOR:
            g_k.has_weapon[W_KOR] = TRUE;
            g_k.kor += 8u;
            g_k.weapon = W_KOR;
            say("KOR ATAR! (3) — patlar, dikkat", 2.5f);
            break;
        case PK_KEY:
            g_k.has_key = TRUE;
            say("Kizil muhur alindi. Muhurlu kapi acilir.", 3.0f);
            sfx(&k_sfx_key);
            break;
        }
        if (!take) continue;
        p->taken = TRUE;
        g_k.pickup_flash = 0.25f;
        if (p->kind != PK_KEY) sfx(&k_sfx_pick);
    }
}

static void update_doors(f32 dt) {
    for (u32 i = 0; i < g_k.door_count; i++) {
        Door* d = &g_k.doors[i];
        Vec3 c = tile_center(d->tx, d->tz, 0.0f);
        b32 want = FALSE;
        Vec3 dp = vec3_sub(g_k.feet, c);
        b32 player_near = dp.x * dp.x + dp.z * dp.z < 3.2f * 3.2f;
        if (player_near) {
            if (!d->locked || g_k.has_key) want = TRUE;
            else if (g_k.lock_msg_cd <= 0.0f) {
                say("Kapi muhurlu. Kizil muhru bul.", 2.0f);
                sfx(&k_sfx_locked);
                g_k.lock_msg_cd = 2.5f;
            }
        }
        for (u32 k = 0; k < g_k.enemy_count && !want; k++) {
            const Enemy* e = &g_k.enemies[k];
            if (!e->alive || !e->alert || d->locked) continue;
            Vec3 de = vec3_sub(e->feet, c);
            if (de.x * de.x + de.z * de.z < 2.6f * 2.6f) want = TRUE;
        }
        f32 prev = d->open;
        d->open = approach(d->open, want ? 1.0f : 0.0f, dt * 1.6f);
        if (prev == 0.0f && d->open > 0.0f) sfx_at(&k_sfx_door, c);
        /* Kapanırken içeride biri varsa geri aç */
        Aabb b = d->closed;
        f32 lift = d->open * (LINTEL_Y - 0.15f);
        b.min.y += lift; b.max.y += lift;
        if (d->open < prev) {
            Capsule pc = body_capsule(g_k.feet, PLAYER_R, PLAYER_H);
            if (pc.a.y - PLAYER_R < b.max.y && pc.b.y + PLAYER_R > b.min.y &&
                g_k.feet.x + PLAYER_R > b.min.x && g_k.feet.x - PLAYER_R < b.max.x &&
                g_k.feet.z + PLAYER_R > b.min.z && g_k.feet.z - PLAYER_R < b.max.z) {
                d->open = prev;
                continue;
            }
        }
        g_k.world.statics[d->phys].shape.as.aabb = b;
    }
}

static void game_logic(f32 dt, Arena* scratch) {
    if (g_k.state != KS_PLAY) return;
    g_k.play_time += dt;
    g_k.lock_msg_cd -= dt;
    take_pickups();
    /* Lav */
    i32 tx = (i32)m_floor(g_k.feet.x / TILE), tz = (i32)m_floor(g_k.feet.z / TILE);
    if (tile_at(tx, tz) == '~' && g_k.feet.y < -0.3f) {
        if (!g_k.god) {
            g_k.hp -= LAVA_DPS * dt;
            g_k.hurt_flash = m_max(g_k.hurt_flash, 0.35f);
            if (g_k.hp <= 0.0f) {
                g_k.hp = 0.0f;
                g_k.state = KS_DEAD;
                sfx(&k_sfx_death);
                say("Lav seni yuttu.  R: yeniden dene", 1e9f);
            }
        }
    }
    /* Çıkış: ışık bacası */
    Vec3 de = vec3_sub(g_k.feet, g_k.exit_pos);
    if (de.x * de.x + de.z * de.z < 0.9f * 0.9f) {
        sfx(&k_sfx_win);
        g_k.total_enemies += g_k.enemy_count;
        if (g_k.level + 1u < LEVEL_COUNT) {
            load_level(g_k.level + 1u, TRUE, scratch);
            g_k.hp = m_min(g_k.hp + 25.0f, MAX_HP);
            char buf[96];
            fmt_format(buf, sizeof(buf), "%s  -  %s", k_levels[g_k.level].title, k_levels[g_k.level].intro);
            say(buf, 5.0f);
            return;
        }
        g_k.state = KS_WIN;
        char buf[96];
        fmt_format(buf, sizeof(buf), "KUYUDAN CIKTIN!  %.1f sn, %u/%u yaratik  (R)", (f64)g_k.play_time,
                   g_k.total_kills, g_k.total_enemies);
        say(buf, 1e9f);
    }
}

/* ---------- Işık seçimi ---------- */

static void push_light(Vec3 p, Vec3 c, f32 r) {
    if (g_k.light_count >= MAX_LIGHTS) return;
    Light* l = &g_k.lights[g_k.light_count++];
    l->pos = p; l->col = c; l->radius = r;
}

static void collect_lights(Vec3 eye, Vec3 fwd) {
    g_k.light_count = 0;
    f32 t = g_k.anim_time;
    for (u32 i = 0; i < g_k.static_light_count; i++) {
        const Light* s = &g_k.static_lights[i];
        f32 fl = 0.88f + 0.12f * m_sin(t * 11.0f + (f32)i * 2.1f) * m_sin(t * 4.3f + (f32)i);
        push_light(s->pos, vec3_scale(s->col, fl), s->radius);
    }
    if (g_k.flash_t > 0.0f) {
        Vec3 c = g_k.weapon == W_KOR ? vec3(3.0f, 1.4f, 0.4f) : vec3(3.2f, 2.4f, 1.2f);
        push_light(vec3_add(eye, vec3_scale(fwd, 0.8f)), c, 10.0f);
    }
    for (u32 i = 0; i < MAX_PROJ; i++) {
        const Proj* p = &g_k.proj[i];
        if (!p->active) continue;
        if (p->kind == 0u) push_light(p->pos, vec3(3.0f, 1.3f, 0.35f), 7.0f);
        else if (p->kind == 1u) push_light(p->pos, vec3(0.7f, 2.2f, 0.4f), 5.0f);
        else push_light(p->pos, vec3(2.4f, 0.35f, 0.3f), 4.0f);
    }
    /* Patlamalar: büyük, taze turuncu parçacıklar */
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle* q = &g_k.parts[i];
        if (q->life > 0.0f && q->size >= 0.12f && q->life > q->max_life * 0.6f && q->col.y < 0.95f) {
            push_light(q->pos, vec3(4.0f, 1.8f, 0.5f), 9.0f);
            i += 7;   /* patlama başına birkaç ışık yeter */
        }
    }
}

/* Kutuya (bmin..bmax) en çok katkı veren 4 ışığı uniform bloğuna yazar */
typedef struct {
    _Alignas(16) Vec4 amb;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
    _Alignas(16) Vec4 l[8];
} DungeonUniforms;

typedef struct {
    _Alignas(16) Vec4 color;
    _Alignas(16) Vec4 fog;
} UnlitUniforms;

static const Vec3 k_fog_lin  = { 0.010f, 0.008f, 0.012f };
static const f32  k_fog_dens = 1.0f / 30.0f;

static void pick_lights(DungeonUniforms* u, Vec3 bmin, Vec3 bmax) {
    f32 best_s[4] = { 0, 0, 0, 0 };
    i32 best_i[4] = { -1, -1, -1, -1 };
    for (u32 i = 0; i < g_k.light_count; i++) {
        const Light* l = &g_k.lights[i];
        Vec3 q = vec3(m_clamp(l->pos.x, bmin.x, bmax.x), m_clamp(l->pos.y, bmin.y, bmax.y), m_clamp(l->pos.z, bmin.z, bmax.z));
        Vec3 d = vec3_sub(l->pos, q);
        f32 d2 = vec3_dot(d, d);
        if (d2 >= l->radius * l->radius) continue;
        f32 s = (l->col.x + l->col.y + l->col.z) / (1.0f + d2 * 0.25f);
        for (u32 k = 0; k < 4; k++) {
            if (s > best_s[k]) {
                for (u32 j = 3; j > k; j--) { best_s[j] = best_s[j - 1]; best_i[j] = best_i[j - 1]; }
                best_s[k] = s; best_i[k] = (i32)i;
                break;
            }
        }
    }
    for (u32 k = 0; k < 4; k++) {
        if (best_i[k] < 0) { u->l[k * 2] = vec4(0, -1000.0f, 0, 0); u->l[k * 2 + 1] = vec4(0, 0, 0, 0); continue; }
        const Light* l = &g_k.lights[best_i[k]];
        u->l[k * 2]     = vec4(l->pos.x, l->pos.y, l->pos.z, 1.0f / (l->radius * l->radius));
        u->l[k * 2 + 1] = vec4(l->col.x, l->col.y, l->col.z, 0.0f);
    }
}

static void draw_lit_box(const Mesh* m, const Mat4* model, const Texture* tex, Vec4 color, Vec3 amb,
                         Vec3 bmin, Vec3 bmax, G3dCull cull) {
    if (!m) return;
    DungeonUniforms* u = ARENA_PUSH_STRUCT(g_k.frame, DungeonUniforms);
    u->amb  = vec4(amb.x, amb.y, amb.z, 0.0f);
    u->base = color;
    u->fog  = vec4(k_fog_lin.x, k_fog_lin.y, k_fog_lin.z, k_fog_dens);
    pick_lights(u, bmin, bmax);
    G3dMaterial mt = g_k.mat_lit;
    mt.uniforms = u;
    mt.textures = tex;
    mt.cull = cull;
    g3d_draw(g_k.ctx, m, model, &mt);
}

static const Vec3 k_amb = { 0.042f, 0.036f, 0.046f };

static void draw_lit_at(const Mesh* m, const Mat4* model, const Texture* tex, Vec4 color, Vec3 at, f32 r) {
    draw_lit_box(m, model, tex, color, k_amb, vec3_sub(at, vec3(r, r, r)), vec3_add(at, vec3(r, r, r)), G3D_CULL_BACK);
}

/* Silah modeli: oyuncunun etrafındaki ışıklar + okunurluk için biraz daha güçlü ortam */
static void draw_held(const Mesh* m, const Mat4* model, Vec4 color) {
    static const Vec3 k_amb_held = { 0.10f, 0.09f, 0.085f };
    Vec3 c = g_k.eye;
    draw_lit_box(m, model, &g_k.tex_door, color, k_amb_held, vec3_sub(c, vec3(2, 2, 2)), vec3_add(c, vec3(2, 2, 2)), G3D_CULL_BACK);
}

static void draw_emissive(const Mesh* m, const Mat4* model, Vec4 color) {
    if (!g_k.mat_emis.shade_fn) return;
    UnlitUniforms* u = ARENA_PUSH_STRUCT(g_k.frame, UnlitUniforms);
    u->color = color;
    u->fog = vec4(m_sqrt(k_fog_lin.x), m_sqrt(k_fog_lin.y), m_sqrt(k_fog_lin.z), k_fog_dens);
    G3dMaterial mt = g_k.mat_emis;
    mt.uniforms = u;
    g3d_draw(g_k.ctx, m, model, &mt);
}

/* Sağ/yukarı/geri eksenleri + ölçek + konumdan model matrisi (sütun öncelikli) */
static Mat4 basis_mat(Vec3 rt, Vec3 up, Vec3 back, Vec3 s, Vec3 pos) {
    Mat4 m = mat4_identity();
    m.m[0] = rt.x * s.x;   m.m[1] = rt.y * s.x;   m.m[2] = rt.z * s.x;
    m.m[4] = up.x * s.y;   m.m[5] = up.y * s.y;   m.m[6] = up.z * s.y;
    m.m[8] = back.x * s.z; m.m[9] = back.y * s.z; m.m[10] = back.z * s.z;
    m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
    return m;
}

/* ---------- Çizim ---------- */

static void draw_world(void) {
    f32 t = g_k.anim_time;
    Mat4 id = mat4_identity();
    Vec3 lava_amb = vec3(1.25f + 0.15f * m_sin(t * 1.3f), 0.55f, 0.18f);
    for (i32 cz = 0; cz < CHUNKS_Z; cz++) {
        for (i32 cx = 0; cx < CHUNKS_X; cx++) {
            const Chunk* ch = &g_k.chunks[cz * CHUNKS_X + cx];
            draw_lit_box(ch->wall, &id, &g_k.tex_wall, vec4(1, 1, 1, 1), k_amb, ch->bmin, ch->bmax, G3D_CULL_BACK);
            draw_lit_box(ch->floor, &id, &g_k.tex_floor, vec4(1, 1, 1, 1), k_amb, ch->bmin, ch->bmax, G3D_CULL_BACK);
            draw_lit_box(ch->ceil, &id, &g_k.tex_ceil, vec4(1, 1, 1, 1), k_amb, ch->bmin, ch->bmax, G3D_CULL_BACK);
            if (ch->lava) {
                Mat4 lm = mat4_translate(vec3(0.0f, 0.06f * m_sin(t * 0.9f), 0.0f));
                draw_lit_box(ch->lava, &lm, &g_k.tex_lava, vec4(1, 1, 1, 1), lava_amb, ch->bmin, ch->bmax, G3D_CULL_BACK);
            }
        }
    }

    /* Kapılar */
    for (u32 i = 0; i < g_k.door_count; i++) {
        const Door* d = &g_k.doors[i];
        Aabb b = g_k.world.statics[d->phys].shape.as.aabb;
        Vec3 c = vec3_scale(vec3_add(b.min, b.max), 0.5f);
        Vec3 s = vec3_sub(b.max, b.min);
        Mat4 m = mat4_trs(c, 0.0f, 0.0f, s);
        Vec4 col = d->locked ? vec4(1.0f, 0.55f, 0.5f, 1.0f) : vec4(1, 1, 1, 1);
        draw_lit_box(g_k.cube, &m, &g_k.tex_door, col, k_amb, b.min, b.max, G3D_CULL_BACK);
        if (d->locked && d->open < 0.01f) {
            Vec3 n = d->along_x ? vec3(1, 0, 0) : vec3(0, 0, 1);
            for (i32 sd = -1; sd <= 1; sd += 2) {
                Vec3 p = vec3_add(vec3(c.x, 1.6f, c.z), vec3_scale(n, 0.22f * (f32)sd));
                Mat4 sm = mat4_trs(p, 0.0f, 0.0f, vec3(0.18f, 0.18f, 0.18f));
                draw_emissive(g_k.sphere, &sm, vec4(1.0f, 0.15f + 0.1f * m_sin(t * 4.0f), 0.1f, 1.0f));
            }
        }
    }

    /* Mangallar: ayak + alev */
    for (i32 z = 0; z < MAP_H; z++) {
        for (i32 x = 0; x < MAP_W; x++) {
            if (tile_at(x, z) != 'i') continue;
            Vec3 c = tile_center(x, z, 0.0f);
            Mat4 m = mat4_trs(vec3(c.x, 0.5f, c.z), 0.0f, 0.0f, vec3(0.25f, 1.0f, 0.25f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_door, vec4(0.6f, 0.55f, 0.5f, 1.0f), c, 1.5f);
            m = mat4_trs(vec3(c.x, 1.05f, c.z), 0.0f, 0.0f, vec3(0.7f, 0.15f, 0.7f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_door, vec4(0.6f, 0.55f, 0.5f, 1.0f), c, 1.5f);
            f32 ph = t * 9.0f + (f32)(x * 3 + z);
            for (u32 k = 0; k < 3; k++) {
                f32 s = (0.22f - 0.05f * (f32)k) * (0.85f + 0.15f * m_sin(ph + (f32)k * 2.0f));
                Mat4 fm = mat4_trs(vec3(c.x + 0.06f * m_sin(ph * 0.7f + (f32)k), 1.3f + 0.18f * (f32)k, c.z),
                                   ph * 0.2f, 0.0f, vec3(s, s * 1.6f, s));
                draw_emissive(g_k.sphere, &fm, k == 0 ? vec4(1.0f, 0.45f, 0.1f, 1.0f) : vec4(1.0f, 0.8f, 0.3f, 1.0f));
            }
        }
    }

    /* Çıkış bacası: ışık sütunu */
    {
        Mat4 m = mat4_trs(vec3(g_k.exit_pos.x, 5.5f, g_k.exit_pos.z), t * 0.3f, 0.0f, vec3(0.9f, 11.0f, 0.9f));
        draw_emissive(g_k.cube, &m, vec4(0.55f, 0.75f, 1.0f, 1.0f));
    }

    /* Toplanabilirler */
    for (u32 i = 0; i < g_k.pickup_count; i++) {
        const Pickup* p = &g_k.pickups[i];
        if (p->taken) continue;
        f32 bob = 0.12f * m_sin(t * 2.5f + (f32)i);
        Vec3 c = vec3(p->pos.x, p->pos.y + bob, p->pos.z);
        f32 ry = t * 1.6f + (f32)i;
        Mat4 m;
        switch (p->kind) {
        case PK_HEALTH:
            m = mat4_trs(c, ry, 0.0f, vec3(0.5f, 0.35f, 0.5f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_white, vec4(0.9f, 0.9f, 0.85f, 1.0f), c, 1.0f);
            m = mat4_trs(vec3(c.x, c.y + 0.2f, c.z), ry, 0.0f, vec3(0.3f, 0.06f, 0.1f));
            draw_emissive(g_k.cube, &m, vec4(1.0f, 0.15f, 0.15f, 1.0f));
            m = mat4_trs(vec3(c.x, c.y + 0.2f, c.z), ry, 0.0f, vec3(0.1f, 0.06f, 0.3f));
            draw_emissive(g_k.cube, &m, vec4(1.0f, 0.15f, 0.15f, 1.0f));
            break;
        case PK_SHELLS:
            m = mat4_trs(c, ry, 0.0f, vec3(0.45f, 0.3f, 0.3f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_white, vec4(0.75f, 0.3f, 0.15f, 1.0f), c, 1.0f);
            break;
        case PK_KOR:
            m = mat4_trs(c, ry, 0.0f, vec3(0.4f, 0.4f, 0.4f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_door, vec4(0.5f, 0.45f, 0.4f, 1.0f), c, 1.0f);
            m = mat4_trs(vec3(c.x, c.y + 0.28f, c.z), ry, 0.0f, vec3(0.14f, 0.14f, 0.14f));
            draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.5f, 0.1f, 1.0f));
            break;
        case PK_W_SACMALI:
        case PK_W_KOR: {
            Vec4 col = p->kind == PK_W_SACMALI ? vec4(0.45f, 0.35f, 0.3f, 1.0f) : vec4(0.35f, 0.3f, 0.35f, 1.0f);
            m = mat4_trs(c, ry, 0.0f, vec3(0.2f, 0.2f, 0.9f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_door, col, c, 1.0f);
            m = mat4_trs(vec3(c.x, c.y - 0.15f, c.z), ry, 0.0f, vec3(0.14f, 0.3f, 0.2f));
            draw_lit_at(g_k.cube, &m, &g_k.tex_door, vec4(0.4f, 0.25f, 0.15f, 1.0f), c, 1.0f);
            break;
        }
        case PK_KEY:
            m = mat4_trs(vec3(c.x, c.y + 0.3f, c.z), ry * 2.0f, 0.6f, vec3(0.35f, 0.35f, 0.08f));
            draw_emissive(g_k.cube, &m, vec4(1.0f, 0.2f, 0.15f, 1.0f));
            m = mat4_trs(vec3(c.x, c.y + 0.3f, c.z), ry * 2.0f, 0.6f, vec3(0.18f, 0.18f, 0.12f));
            draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.8f, 0.4f, 1.0f));
            break;
        }
    }

    /* Düşmanlar */
    for (u32 i = 0; i < g_k.enemy_count; i++) {
        const Enemy* e = &g_k.enemies[i];
        f32 dead = e->alive ? 0.0f : m_min(e->dead_t / 0.4f, 1.0f);
        f32 sy = 1.0f - 0.8f * dead;
        b32 flash = e->pain > 0.0f;
        Vec3 fw = vec3(-m_sin(e->yaw), 0.0f, -m_cos(e->yaw));
        Vec3 rt = vec3(m_cos(e->yaw), 0.0f, -m_sin(e->yaw));
        Vec3 c = enemy_center(e);
        Mat4 m;
        if (e->kind == EN_GOLGE) {
            f32 sway = m_sin(e->anim * 7.0f) * 0.08f * (e->alive ? 1.0f : 0.0f);
            Vec4 col = flash ? vec4(1.0f, 0.8f, 1.0f, 1.0f) : vec4(0.42f, 0.30f, 0.55f, 1.0f);
            m = mat4_trs(vec3(e->feet.x, e->feet.y + 0.95f * sy, e->feet.z), e->yaw + sway, 0.0f, vec3(0.5f, 0.95f * sy, 0.42f));
            draw_lit_at(g_k.sphere, &m, &g_k.tex_skin, col, c, 1.2f);
            if (e->alive) {
                m = mat4_trs(vec3(e->feet.x, e->feet.y + 1.72f, e->feet.z), e->yaw, 0.0f, vec3(0.3f, 0.3f, 0.3f));
                draw_lit_at(g_k.sphere, &m, &g_k.tex_skin, col, c, 1.2f);
                for (i32 s = -1; s <= 1; s += 2) {
                    Vec3 ep = vec3_add(vec3(e->feet.x, e->feet.y + 1.76f, e->feet.z),
                                       vec3_add(vec3_scale(fw, 0.24f), vec3_scale(rt, 0.11f * (f32)s)));
                    m = mat4_trs(ep, 0.0f, 0.0f, vec3(0.06f, 0.05f, 0.06f));
                    draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.2f, 0.1f, 1.0f));
                    f32 swing = (e->cooldown > 0.6f) ? 1.0f : 0.25f * m_sin(e->anim * 7.0f + (f32)s);
                    Vec3 ap = vec3_add(vec3(e->feet.x, e->feet.y + 1.05f, e->feet.z),
                                       vec3_add(vec3_scale(rt, 0.55f * (f32)s), vec3_scale(fw, 0.25f + 0.35f * swing)));
                    m = mat4_trs(ap, e->yaw, -0.6f - swing, vec3(0.12f, 0.12f, 0.75f));
                    draw_lit_at(g_k.cube, &m, &g_k.tex_skin, col, c, 1.2f);
                }
            }
        } else if (e->kind == EN_GOZCU) {
            /* Gözcü: göz küresi + kızıl iris (oyuncuya bakar) + çırpan zar kanatlar */
            Vec4 col = flash ? vec4(1.0f, 0.9f, 0.9f, 1.0f) : vec4(0.85f, 0.78f, 0.72f, 1.0f);
            f32 sd = e->alive ? 1.0f : 1.0f - 0.5f * dead;
            m = mat4_trs(e->feet, e->yaw, 0.0f, vec3(0.45f * sd, 0.45f * sd, 0.45f * sd));
            draw_lit_at(g_k.sphere, &m, &g_k.tex_skin, col, c, 1.0f);
            if (e->alive) {
                Vec3 to = vec3_sub(g_k.eye, e->feet);
                f32 tl = vec3_len(to);
                Vec3 lk = tl > 1e-3f ? vec3_scale(to, 1.0f / tl) : fw;
                f32 charge = e->burst > 0u ? 1.0f : (e->cooldown < 0.6f && e->alert ? 1.0f - e->cooldown / 0.6f : 0.0f);
                Vec3 ip = vec3_add(e->feet, vec3_scale(lk, 0.4f));
                f32 is = 0.17f + 0.05f * charge;
                m = mat4_trs(ip, 0.0f, 0.0f, vec3(is, is, is));
                draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.15f + 0.5f * charge, 0.1f, 1.0f));
                m = mat4_trs(vec3_add(e->feet, vec3_scale(lk, 0.5f)), 0.0f, 0.0f, vec3(0.07f, 0.07f, 0.07f));
                draw_emissive(g_k.sphere, &m, vec4(0.05f, 0.02f, 0.02f, 1.0f));
                f32 flap = m_sin(e->anim * 18.0f) * 0.7f;
                for (i32 s = -1; s <= 1; s += 2) {
                    Vec3 wp = vec3_add(e->feet, vec3_add(vec3_scale(rt, 0.62f * (f32)s), vec3(0.0f, 0.1f + 0.15f * flap, 0.0f)));
                    m = mat4_trs(wp, e->yaw + 1.5708f, (f32)s * flap * 0.6f, vec3(0.08f, 0.04f, 0.6f));
                    draw_lit_at(g_k.cube, &m, &g_k.tex_skin, vec4(0.45f, 0.2f, 0.22f, 1.0f), c, 1.0f);
                }
            }
        } else {
            f32 breathe = 1.0f + 0.06f * m_sin(e->anim * 3.0f);
            Vec4 col = flash ? vec4(1.0f, 1.0f, 0.8f, 1.0f) : vec4(0.55f, 0.62f, 0.32f, 1.0f);
            m = mat4_trs(vec3(e->feet.x, e->feet.y + 0.72f * sy, e->feet.z), e->yaw, 0.0f,
                         vec3(0.72f * breathe, 0.7f * sy, 0.72f * breathe));
            draw_lit_at(g_k.sphere, &m, &g_k.tex_skin, col, c, 1.3f);
            if (e->alive) {
                f32 charge = e->cooldown < 0.5f ? 1.0f - e->cooldown * 2.0f : 0.0f;
                f32 ms = 0.16f + 0.1f * charge;
                Vec3 mp = vec3_add(vec3(e->feet.x, e->feet.y + 0.95f, e->feet.z), vec3_scale(fw, 0.62f));
                m = mat4_trs(mp, e->yaw, 0.0f, vec3(ms * 1.4f, ms, ms));
                draw_emissive(g_k.sphere, &m, vec4(0.5f + 0.5f * charge, 1.0f, 0.3f, 1.0f));
                for (i32 s = -1; s <= 1; s += 2) {
                    Vec3 ep = vec3_add(vec3(e->feet.x, e->feet.y + 1.25f, e->feet.z),
                                       vec3_add(vec3_scale(fw, 0.5f), vec3_scale(rt, 0.2f * (f32)s)));
                    m = mat4_trs(ep, 0.0f, 0.0f, vec3(0.07f, 0.07f, 0.07f));
                    draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.95f, 0.4f, 1.0f));
                }
            }
        }
    }

    /* Mermiler */
    for (u32 i = 0; i < MAX_PROJ; i++) {
        const Proj* p = &g_k.proj[i];
        if (!p->active) continue;
        f32 s = p->kind == 0u ? 0.16f : p->kind == 1u ? 0.2f : 0.1f;
        Vec4 col = p->kind == 0u ? vec4(1.0f, 0.6f, 0.15f, 1.0f) : p->kind == 1u ? vec4(0.55f, 1.0f, 0.3f, 1.0f)
                                                                 : vec4(1.0f, 0.25f, 0.2f, 1.0f);
        Mat4 m = mat4_trs(p->pos, t * 8.0f, 0.0f, vec3(s, s, p->kind == 2u ? s * 2.5f : s));
        if (p->kind == 2u) {
            Vec3 d = vec3_norm(p->vel);
            m = mat4_trs(p->pos, m_atan2(-d.x, -d.z), m_atan2(d.y, m_sqrt(d.x * d.x + d.z * d.z)), vec3(s, s, s * 3.0f));
        }
        draw_emissive(g_k.sphere, &m, col);
    }

    /* Parçacıklar */
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle* q = &g_k.parts[i];
        if (q->life <= 0.0f) continue;
        f32 k = q->life / q->max_life;
        f32 s = q->size * (0.4f + 0.6f * k);
        Mat4 m = mat4_trs(q->pos, (f32)i, 0.0f, vec3(s, s, s));
        draw_emissive(g_k.cube, &m, vec4(q->col.x * (0.5f + 0.5f * k), q->col.y * (0.5f + 0.5f * k), q->col.z * (0.5f + 0.5f * k), 1.0f));
    }
}

/* Silah modeli: kameraya bağlı, ayrı derinlik geçişinde (duvara gömülmez) */
static void draw_viewmodel(Vec3 eye, Vec3 fwd) {
    Vec3 rt = vec3(m_cos(g_k.yaw), 0.0f, -m_sin(g_k.yaw));
    Vec3 up = vec3_cross(rt, fwd);
    Vec3 back = vec3_scale(fwd, -1.0f);
    f32 hs = m_sqrt(g_k.vel.x * g_k.vel.x + g_k.vel.z * g_k.vel.z);
    f32 bob = g_k.grounded ? m_min(hs / RUN_SPEED, 1.0f) : 0.0f;
    f32 bx = m_sin(g_k.bob_phase) * 0.018f * bob, by = -m_abs(m_cos(g_k.bob_phase)) * 0.014f * bob;
    Vec3 base = vec3_add(eye, vec3_add(vec3_scale(fwd, 0.42f - 0.07f * g_k.recoil),
                    vec3_add(vec3_scale(rt, 0.2f + bx), vec3_scale(up, -0.19f + by))));
    Vec4 metal = vec4(0.45f, 0.43f, 0.42f, 1.0f), wood = vec4(0.45f, 0.28f, 0.16f, 1.0f);
    Mat4 m;
    f32 muzzle = 0.3f;
    switch (g_k.weapon) {
    case W_CAKMAK:
        m = basis_mat(rt, up, back, vec3(0.04f, 0.05f, 0.17f), base);
        draw_held(g_k.cube, &m, metal);
        m = basis_mat(rt, up, back, vec3(0.036f, 0.1f, 0.05f), vec3_add(base, vec3_add(vec3_scale(up, -0.065f), vec3_scale(fwd, -0.055f))));
        draw_held(g_k.cube, &m, wood);
        m = basis_mat(rt, up, back, vec3(0.02f, 0.02f, 0.02f), vec3_add(base, vec3_add(vec3_scale(up, 0.035f), vec3_scale(fwd, 0.07f))));
        draw_emissive(g_k.cube, &m, vec4(1.0f, 0.6f, 0.2f, 1.0f));
        muzzle = 0.1f;
        break;
    case W_SACMALI:
        for (i32 s = -1; s <= 1; s += 2) {
            m = basis_mat(rt, up, back, vec3(0.045f, 0.045f, 0.5f), vec3_add(base, vec3_scale(rt, 0.026f * (f32)s)));
            draw_held(g_k.cube, &m, metal);
        }
        m = basis_mat(rt, up, back, vec3(0.1f, 0.08f, 0.26f), vec3_add(base, vec3_add(vec3_scale(fwd, -0.3f), vec3_scale(up, -0.02f))));
        draw_held(g_k.cube, &m, wood);
        muzzle = 0.27f;
        break;
    case W_KOR:
    default:
        m = basis_mat(rt, up, back, vec3(0.11f, 0.11f, 0.42f), base);
        draw_held(g_k.cube, &m, vec4(0.35f, 0.3f, 0.32f, 1.0f));
        m = basis_mat(rt, up, back, vec3(0.07f, 0.07f, 0.07f), vec3_add(base, vec3_add(vec3_scale(fwd, 0.1f), vec3_scale(up, 0.07f))));
        draw_emissive(g_k.sphere, &m, g_k.kor > 0 ? vec4(1.0f, 0.5f, 0.1f, 1.0f) : vec4(0.2f, 0.1f, 0.05f, 1.0f));
        muzzle = 0.23f;
        break;
    }
    if (g_k.flash_t > 0.0f) {
        f32 s = 0.06f + 0.5f * g_k.flash_t;
        m = basis_mat(rt, up, back, vec3(s, s, s * 1.4f), vec3_add(base, vec3_scale(fwd, muzzle)));
        draw_emissive(g_k.sphere, &m, vec4(1.0f, 0.85f, 0.4f, 1.0f));
    }
}

static void setup_materials(const ShaderManager* sm, b32 use_jit) {
    __builtin_memset(&g_k.mat_lit, 0, sizeof(g_k.mat_lit));
    __builtin_memset(&g_k.mat_emis, 0, sizeof(g_k.mat_emis));
    const ManagedShader* sh = shaders_find(sm, "dungeon3d.mtk");
    if (sh) {
        b32 fused = FALSE;
        g_k.mat_lit.shade_fn   = shaders_get_fn(sh, use_jit, BLEND_NONE, &fused);
        g_k.mat_lit.fused      = fused;
        g_k.mat_lit.attrs[0]   = G3D_ATTR_UV;
        g_k.mat_lit.attrs[1]   = G3D_ATTR_NORMAL;
        g_k.mat_lit.attrs[2]   = G3D_ATTR_VIEW_DEPTH;
        g_k.mat_lit.attrs[3]   = G3D_ATTR_WORLD_POS;
        g_k.mat_lit.attr_count = 4;
        g_k.mat_lit.cull       = G3D_CULL_BACK;
        g_k.mat_lit.depth_test = TRUE;
        g_k.mat_lit.depth_write = TRUE;
    }
    const ManagedShader* esh = shaders_find(sm, "unlit3d.mtk");
    if (esh) {
        b32 fused = FALSE;
        g_k.mat_emis.shade_fn   = shaders_get_fn(esh, use_jit, BLEND_NONE, &fused);
        g_k.mat_emis.fused      = fused;
        g_k.mat_emis.attrs[0]   = G3D_ATTR_NORMAL;
        g_k.mat_emis.attrs[1]   = G3D_ATTR_VIEW_DEPTH;
        g_k.mat_emis.attr_count = 2;
        g_k.mat_emis.cull       = G3D_CULL_BACK;
        g_k.mat_emis.depth_test = TRUE;
        g_k.mat_emis.depth_write = TRUE;
    }
}

static void render(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm, b32 use_jit) {
    f64 t0 = plat_time_ms();
    g_k.frame = frame;
    setup_materials(sm, use_jit);

    /* Kamera: göz + sarsıntı + iniş/yürüme sallantısı */
    Vec3 eye = eye_pos();
    f32 hs = m_sqrt(g_k.vel.x * g_k.vel.x + g_k.vel.z * g_k.vel.z);
    if (g_k.grounded) eye.y += m_sin(g_k.bob_phase * 2.0f) * 0.035f * m_min(hs / RUN_SPEED, 1.0f);
    if (g_k.state == KS_DEAD) eye.y = g_k.feet.y + 0.35f;
    f32 sh = g_k.shake;
    f32 yaw = g_k.yaw + (rng_f32(&g_k.rng) - 0.5f) * sh * 0.12f;
    f32 pitch = g_k.pitch + (rng_f32(&g_k.rng) - 0.5f) * sh * 0.12f + g_k.recoil * 0.025f;
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = eye;
    cam.yaw = yaw;
    cam.pitch = pitch;
    cam.fov_y = 1.12f;
    cam.near_z = 0.05f;
    Vec3 fwd = view_dir(yaw, pitch);
    g_k.eye = eye;
    collect_lights(eye, fwd);

    Framebuffer* low = &g_k.low;
    Vec3 fs = vec3(m_sqrt(k_fog_lin.x), m_sqrt(k_fog_lin.y), m_sqrt(k_fog_lin.z));
    fb_clear(low, FB_RGBA((u32)(fs.x * 255.0f), (u32)(fs.y * 255.0f), (u32)(fs.z * 255.0f), 255));
    fb_clear_depth(low, 0.0f);

    PROF_BEGIN("kuyu_world");
    G3dContext ctx;
    raster_begin(r, low, frame);
    g3d_begin(&ctx, r, frame, camera_view(&cam), camera_proj(&cam, (f32)INNER_W / (f32)INNER_H), eye);
    if (g_k.snap_on) g3d_set_snap(&ctx, 1u);
    g_k.ctx = &ctx;
    draw_world();
    raster_flush(r);
    g_k.stats = ctx.stats;
    PROF_END();

    if (g_k.state != KS_DEAD) {
        PROF_BEGIN("kuyu_view");
        fb_clear_depth(low, 0.0f);
        G3dContext vctx;
        raster_begin(r, low, frame);
        Camera vc = cam;
        vc.yaw = g_k.yaw; vc.pitch = g_k.pitch;
        Vec3 vf = view_dir(g_k.yaw, g_k.pitch);
        g3d_begin(&vctx, r, frame, camera_view(&vc), camera_proj(&vc, (f32)INNER_W / (f32)INNER_H), eye);
        g_k.ctx = &vctx;
        draw_viewmodel(eye, vf);
        raster_flush(r);
        PROF_END();
    }
    f64 t1 = plat_time_ms();

    PROF_BEGIN("kuyu_present");
    RetroParams rp = retro_default_params();
    rp.inner_w = INNER_W;
    rp.inner_h = INNER_H;
    rp.scale = 0;
    if (g_k.retro_on) {
        rp.color_bits = 15;
        rp.dither = TRUE;
        rp.scanline = 0.18f;
        rp.crt_blur = FALSE;
    } else {
        rp.color_bits = 24;
        rp.dither = FALSE;
        rp.scanline = 0.0f;
        rp.crt_blur = FALSE;
    }
    rp.palette = NULL;
    retro_present(low, fb, &rp);
    PROF_END();
    f64 t2 = plat_time_ms();
    g_k.ms_draw = (f32)(t1 - t0);
    g_k.ms_present = (f32)(t2 - t1);
}

static b32 key_hit(const InputState* in, u32 key, u8* prev) {
    u8 down = in->keys[key];
    b32 hit = down && !*prev;
    *prev = down;
    return hit;
}

void scene_kuyu_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit, b32 pointer_locked) {
    if (dt > 0.1f) dt = 0.1f;
    g_k.anim_time += dt;
    if (key_hit(in, KEY_R, &g_k.prev_r)) reset_game(frame);
    if (g_k.pending_level) { load_level(g_k.pending_level - 1u, TRUE, frame); g_k.pending_level = 0; }
    if (key_hit(in, KEY_N, &g_k.prev_n)) {
        g_k.music_on = !g_k.music_on;
        if (!g_k.music_on) stop_music();
    }
    if (key_hit(in, KEY_V, &g_k.prev_v)) g_k.retro_on = !g_k.retro_on;
    if (key_hit(in, KEY_G, &g_k.prev_g)) g_k.snap_on = !g_k.snap_on;
    if (g_k.music_on && !g_k.music_playing) start_music();

    /* Bakış: fare kilidi açıkken ham hareket, değilse sağ tuş sürükleme; oklar da çevirir */
    if (pointer_locked || (in->mouse_buttons & 2u)) {
        g_k.yaw   -= (f32)in->mouse_dx * 0.0026f;
        g_k.pitch -= (f32)in->mouse_dy * 0.0026f;
    }
    if (in->keys[KEY_LEFT])  g_k.yaw += 2.4f * dt;
    if (in->keys[KEY_RIGHT]) g_k.yaw -= 2.4f * dt;
    if (in->keys[KEY_UP])    g_k.pitch += 1.6f * dt;
    if (in->keys[KEY_DOWN])  g_k.pitch -= 1.6f * dt;
    g_k.pitch = m_clamp(g_k.pitch, -1.45f, 1.45f);
    if (g_k.yaw > M_PI) g_k.yaw -= M_TAU;
    if (g_k.yaw < -M_PI) g_k.yaw += M_TAU;

    Vec3 wish = vec3(0.0f, 0.0f, 0.0f);
    if (g_k.state == KS_PLAY) {
        Vec3 f = vec3(-m_sin(g_k.yaw), 0.0f, -m_cos(g_k.yaw));
        Vec3 rt = vec3(m_cos(g_k.yaw), 0.0f, -m_sin(g_k.yaw));
        if (in->keys[KEY_W]) wish = vec3_add(wish, f);
        if (in->keys[KEY_S]) wish = vec3_sub(wish, f);
        if (in->keys[KEY_D]) wish = vec3_add(wish, rt);
        if (in->keys[KEY_A]) wish = vec3_sub(wish, rt);
        f32 l = vec3_len(wish);
        if (l > 0.0f) wish = vec3_scale(wish, 1.0f / l);
        if (key_hit(in, KEY_SPACE, &g_k.prev_space)) g_k.jump_buf = JUMP_BUFFER;

        if (in->keys[KEY_1] && g_k.has_weapon[W_CAKMAK]) g_k.weapon = W_CAKMAK;
        if (in->keys[KEY_2] && g_k.has_weapon[W_SACMALI]) g_k.weapon = W_SACMALI;
        if (in->keys[KEY_3] && g_k.has_weapon[W_KOR]) g_k.weapon = W_KOR;
        b32 trigger = in->keys[KEY_F] || (pointer_locked && (in->mouse_buttons & 1u));
        g_k.fire_cd = m_max(g_k.fire_cd - dt, 0.0f);
        if (trigger && g_k.fire_cd <= 0.0f) {
            b32 empty = (g_k.weapon == W_SACMALI && g_k.shells == 0) || (g_k.weapon == W_KOR && g_k.kor == 0);
            if (empty) {
                g_k.weapon = (g_k.weapon == W_KOR && g_k.has_weapon[W_SACMALI] && g_k.shells > 0) ? W_SACMALI : W_CAKMAK;
                g_k.fire_cd = 0.2f;
            } else {
                fire_weapon();
            }
        }
    } else {
        g_k.prev_space = in->keys[KEY_SPACE];
        g_k.jump_buf = 0.0f;
    }

    g_k.step_acc += dt;
    while (g_k.step_acc >= STEP_DT) {
        player_step(wish);
        g_k.step_acc -= STEP_DT;
    }
    if (g_k.feet.y < LAVA_Y - 4.0f) { g_k.feet = g_k.start_feet; g_k.vel = vec3(0, 0, 0); }

    /* Yürüme sallantısı + adım sesi */
    f32 hs = m_sqrt(g_k.vel.x * g_k.vel.x + g_k.vel.z * g_k.vel.z);
    if (g_k.grounded && hs > 0.5f) {
        g_k.bob_phase += dt * hs * 1.25f;
        g_k.step_snd -= dt * hs;
        if (g_k.step_snd <= 0.0f) { sfx(&k_sfx_step); g_k.step_snd = 3.0f; }
    }

    flow_update(dt, frame);
    for (u32 i = 0; i < g_k.enemy_count; i++) enemy_think(&g_k.enemies[i], i, dt);
    update_projectiles(dt);
    update_doors(dt);
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        Particle* q = &g_k.parts[i];
        if (q->life <= 0.0f) continue;
        q->life -= dt;
        if (q->grav) q->vel.y += GRAVITY * 0.6f * dt;
        q->pos = vec3_add(q->pos, vec3_scale(q->vel, dt));
        if (q->pos.y < LAVA_Y) q->life = 0.0f;
    }
    game_logic(dt, frame);

    g_k.flash_t = m_max(g_k.flash_t - dt, 0.0f);
    g_k.recoil = m_max(g_k.recoil - dt * 5.0f, 0.0f);
    g_k.shake = m_max(g_k.shake - dt * 1.2f, 0.0f);
    g_k.hurt_flash = m_max(g_k.hurt_flash - dt * 1.5f, 0.0f);
    g_k.pickup_flash = m_max(g_k.pickup_flash - dt, 0.0f);
    if (g_k.msg_time > 0.0f) g_k.msg_time -= dt;

    render(r, fb, frame, sm, use_jit);
}

/* ---------- HUD ---------- */

static void hud_text(Framebuffer* fb, const FontAtlas* a, i32 x, i32 y, const char* s, u32 col, TextAlign al) {
    TextStyle st = text_style_default(col);
    st.align = al;
    st.shadow_color = FB_RGBA(0, 0, 0, 200);
    st.shadow_dx = 1; st.shadow_dy = 1;
    text_draw(fb, a, x, y, s, &st);
}

static const char* const k_weapon_names[W_COUNT] = { "Cakmak", "Sacmali", "Kor Atar" };

void scene_kuyu_draw_hud(UiContext* ui, Framebuffer* fb) {
    i32 w = (i32)fb->w, h = (i32)fb->h;
    if (g_k.hurt_flash > 0.0f) {
        ui_draw_rect(ui, 0, 20, w, h - 20, FB_RGBA(200, 20, 10, (u32)(m_min(g_k.hurt_flash, 0.5f) * 200.0f)));
    }
    if (g_k.pickup_flash > 0.0f) {
        ui_draw_rect(ui, 0, 20, w, h - 20, FB_RGBA(255, 220, 120, (u32)(g_k.pickup_flash * 160.0f)));
    }
    /* Nişangâh */
    if (g_k.state == KS_PLAY) {
        i32 cx = w / 2, cy = h / 2;
        u32 cc = FB_RGBA(255, 240, 200, 200);
        ui_draw_rect(ui, cx - 6, cy, 4, 1, cc);
        ui_draw_rect(ui, cx + 3, cy, 4, 1, cc);
        ui_draw_rect(ui, cx, cy - 6, 1, 4, cc);
        ui_draw_rect(ui, cx, cy + 3, 1, 4, cc);
    }
    /* Alt bant */
    ui_draw_rect(ui, 0, h - 40, w, 40, FB_RGBA(12, 10, 12, 190));
    if (!g_k.font_ok) return;
    char line[96];
    u32 hp_col = g_k.hp > 50.0f ? FB_RGBA(240, 230, 210, 255) : g_k.hp > 25.0f ? FB_RGBA(255, 180, 80, 255) : FB_RGBA(255, 70, 50, 255);
    fmt_format(line, sizeof(line), "%u", (u32)(g_k.hp + 0.999f));
    hud_text(fb, &g_k.font_big, 44, h - 36, line, hp_col, TEXT_ALIGN_LEFT);
    hud_text(fb, &g_k.font_small, 10, h - 26, "CAN", FB_RGBA(180, 160, 150, 255), TEXT_ALIGN_LEFT);

    const char* wn = k_weapon_names[g_k.weapon];
    if (g_k.weapon == W_CAKMAK) fmt_format(line, sizeof(line), "--");
    else fmt_format(line, sizeof(line), "%u", g_k.weapon == W_SACMALI ? g_k.shells : g_k.kor);
    hud_text(fb, &g_k.font_big, w - 14, h - 36, line, FB_RGBA(255, 210, 140, 255), TEXT_ALIGN_RIGHT);
    hud_text(fb, &g_k.font_small, w - 90, h - 26, wn, FB_RGBA(200, 180, 160, 255), TEXT_ALIGN_RIGHT);

    fmt_format(line, sizeof(line), "Yaratik %u/%u", g_k.kills, g_k.enemy_count);
    hud_text(fb, &g_k.font_small, w / 2, h - 30, line, FB_RGBA(190, 180, 200, 255), TEXT_ALIGN_CENTER);
    if (g_k.has_key) {
        ui_draw_rect(ui, w / 2 - 70, h - 30, 10, 14, FB_RGBA(230, 40, 30, 255));
        hud_text(fb, &g_k.font_small, w / 2 - 56, h - 16, "muhur", FB_RGBA(255, 120, 100, 255), TEXT_ALIGN_LEFT);
    }
    /* Silah yuvaları */
    for (u32 i = 0; i < W_COUNT; i++) {
        u32 col = !g_k.has_weapon[i] ? FB_RGBA(70, 60, 60, 255) : (i == (u32)g_k.weapon ? FB_RGBA(255, 220, 120, 255) : FB_RGBA(170, 150, 130, 255));
        fmt_format(line, sizeof(line), "%u", i + 1u);
        hud_text(fb, &g_k.font_small, w / 2 + 40 + (i32)i * 14, h - 16, line, col, TEXT_ALIGN_LEFT);
    }

    if (g_k.msg[0] && g_k.msg_time > 0.0f) {
        hud_text(fb, &g_k.font_small, w / 2, h / 2 - 50, g_k.msg, FB_RGBA(255, 236, 190, 255), TEXT_ALIGN_CENTER);
    }
    if (g_k.state != KS_PLAY) {
        const char* big = g_k.state == KS_WIN ? "OZGURSUN" : "KUYU SENI ALDI";
        hud_text(fb, &g_k.font_big, w / 2, h / 2 - 90, big, g_k.state == KS_WIN ? FB_RGBA(180, 220, 255, 255) : FB_RGBA(255, 80, 60, 255), TEXT_ALIGN_CENTER);
    }
    fmt_format(line, sizeof(line), "%s %s  ciz %.1f ms  sun %.2f ms  V retro  G titreme  N muzik",
               k_levels[g_k.level].title, g_k.retro_on ? (g_k.snap_on ? "retro+snap" : "retro") : "duz",
               (f64)g_k.ms_draw, (f64)g_k.ms_present);
    hud_text(fb, &g_k.font_small, 6, 24, line, FB_RGBA(150, 150, 160, 200), TEXT_ALIGN_LEFT);
}

const f32* scene_kuyu_debug(void) {
    static f32 d[24];
    u32 alive = 0;
    for (u32 i = 0; i < g_k.enemy_count; i++) alive += g_k.enemies[i].alive ? 1u : 0u;
    d[0] = g_k.feet.x; d[1] = g_k.feet.y; d[2] = g_k.feet.z;
    d[3] = g_k.yaw; d[4] = g_k.pitch; d[5] = g_k.hp; d[6] = (f32)g_k.weapon;
    d[7] = (f32)g_k.shells; d[8] = (f32)g_k.kor; d[9] = (f32)g_k.kills; d[10] = (f32)alive;
    d[11] = (f32)g_k.state; d[12] = g_k.has_key ? 1.0f : 0.0f; d[13] = (f32)g_k.enemy_count;
    /* görüş hattındaki en yakın canlı düşman: x y z, uzaklık (-1 yok) */
    d[18] = d[19] = d[20] = 0.0f; d[21] = -1.0f;
    for (u32 i = 0; i < g_k.enemy_count; i++) {
        const Enemy* e = &g_k.enemies[i];
        if (!e->alive || !line_of_sight(eye_pos(), enemy_center(e))) continue;
        f32 l = vec3_len(vec3_sub(e->feet, g_k.feet));
        if (d[21] < 0.0f || l < d[21]) { d[18] = e->feet.x; d[19] = e->feet.y; d[20] = e->feet.z; d[21] = l; }
    }
    d[22] = (f32)g_k.level; d[23] = g_k.snap_on ? 1.0f : 0.0f;
    d[14] = g_k.ms_draw; d[15] = g_k.ms_present; d[16] = (f32)g_k.stats.tris_out; d[17] = (f32)g_k.light_count;
    return d;
}

void scene_kuyu_debug_cmd(u32 cmd, f32 a, f32 b, f32 c) {
    if (cmd == 1u) { g_k.yaw = a; g_k.pitch = b; }
    else if (cmd == 2u) { g_k.feet = vec3(a, b, c); g_k.vel = vec3(0, 0, 0); }
    else if (cmd == 3u) { g_k.god = a != 0.0f; }
    else if (cmd == 4u) { g_k.pending_level = (u32)a + 1u; }
}
