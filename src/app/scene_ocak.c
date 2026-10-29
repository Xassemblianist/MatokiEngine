/* app/scene_ocak.c — "Ocak" oyunu: seviye (sütun adacıklar + taş ocak), oyuncu kapsülü (phys_move_and_slide,
 * 1/120 s sabit adım, coyote + zıplama tamponu), kıvılcımlar ECS entity'leri, meşale/gün zamanlayıcısı,
 * üçüncü şahıs orbit kamera, r3d ile PBR + güneş gölgesi, synth ses efektleri ve özgün akşam teması.
 */
#include "app/scene_ocak.h"
#include "app/r3d.h"
#include "core/math.h"
#include "core/log.h"
#include "raster/texture.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "phys/world.h"
#include "scene/ecs.h"
#include "scene/transform.h"
#include "audio/audio.h"
#include "audio/synth.h"
#include "ui/font.h"
#include "gpu/rhi.h"
#include "gpu/gpu_scene.h"

#define KEY_SPACE 32
#define KEY_A 65
#define KEY_D 68
#define KEY_N 78
#define KEY_R 82
#define KEY_S 83
#define KEY_W 87

#define OCAK_SHADOW    1024u
#define STEP_DT        (1.0f / 120.0f)
#define GRAVITY        (-22.0f)
#define JUMP_SPEED     9.2f
#define RUN_SPEED      6.5f
#define ACC_GROUND     45.0f
#define ACC_AIR        14.0f
#define COYOTE_TIME    0.10f
#define JUMP_BUFFER    0.12f
#define PLAYER_R       0.4f
#define PLAYER_H       1.7f
#define HEARTH_R       3.3f      /* bu yarıçap içinde kıvılcım bırakılır, meşale dolar */
#define TORCH_SECONDS  30.0f
#define DAY_START      0.02f
#define DAY_END        0.50f     /* gün batımı: kaybetme */
#define DAY_SECONDS    210.0f
#define MAX_CARRY      3u
#define SPARK_COUNT    12u
#define PICK_R         1.1f

typedef enum { OCAK_PLAY = 0, OCAK_WIN = 1, OCAK_LOSE = 2 } OcakState;

typedef struct { f32 x, z, half, top; } Pillar;

/* Ocağın etrafında yükselip alçalan sütun yolu + iki yan kol */
static const Pillar k_pillars[] = {
    {   7.0f,   0.0f, 1.6f, 1.2f }, {  10.0f,   3.5f, 1.3f, 2.4f }, {   9.0f,   8.0f, 1.3f, 3.6f },
    {   5.0f,  10.5f, 1.2f, 4.8f }, {   0.0f,  11.0f, 1.2f, 6.0f }, {  -4.5f,   9.5f, 1.2f, 7.2f },
    {  -8.0f,   6.0f, 1.4f, 6.0f }, {  -9.5f,   1.5f, 1.4f, 4.8f }, {  -8.0f,  -3.0f, 1.3f, 3.6f },
    {  -4.0f,  -7.0f, 1.6f, 2.4f }, {   1.0f,  -9.0f, 1.6f, 1.2f }, {  13.0f,  -6.0f, 2.0f, 1.6f },
    {  16.0f,  -1.0f, 1.2f, 3.0f }, { -14.0f, -10.0f, 2.5f, 1.0f }, { -16.0f,  -4.0f, 1.2f, 2.2f },
};
#define PILLAR_COUNT (sizeof(k_pillars) / sizeof(k_pillars[0]))

/* Kıvılcımların yeri: sütun indeksi (>= 0, tepesinin üstü) veya -1 (verilen zemin noktası) */
typedef struct { i32 pillar; f32 x, z; } SparkSpot;
static const SparkSpot k_spots[SPARK_COUNT] = {
    {  1, 0, 0 }, {  3, 0, 0 }, {  4, 0, 0 }, {  5, 0, 0 }, {  7, 0, 0 }, {  9, 0, 0 },
    { 11, 0, 0 }, { 12, 0, 0 }, { 14, 0, 0 },
    { -1, 20.0f, 18.0f }, { -1, -21.0f, 14.0f }, { -1, 2.0f, -23.0f },
};

/* ECS bileşeni: kıvılcım */
typedef struct {
    Vec3 home;
    u32  state;     /* 0 yerinde, 1 taşınıyor, 2 ocakta */
    f32  phase;
} Spark;

typedef struct {
    /* Kaynaklar */
    Mesh*       cube;
    Mesh*       sphere;
    Mesh*       ground;
    Texture     tex_ground, tex_stone, tex_white;
    R3dRenderer r3d;
    PhysWorld   world;
    EcsWorld    ecs;
    EcsCompId   c_tr, c_spark;
    Camera      cam;

    /* Oyuncu */
    Vec3  feet;
    Vec3  vel;
    b32   grounded;
    f32   coyote, jump_buf;
    f32   facing;
    f32   step_acc;

    /* Oyun durumu */
    OcakState state;
    u32   carried, lit;
    f32   torch;          /* 1 dolu → 0 söndü */
    f32   tod;
    f32   play_time;
    f32   msg_time;
    char  msg[96];
    b32   music_on, music_playing;
    u8    prev_r, prev_n, prev_space;
    G3dStats stats_main, stats_shadow;
    f32   anim_time;
    f32   cam_dist;       /* istenen orbit uzaklığı (çarpışma kamerayı bundan yakına çekebilir) */
    f32   cam_dist_cur;   /* yumuşatılmış etkin uzaklık */
} Ocak;

static Ocak g_o;

/* ---------- Ses ---------- */

static const SynthSfx k_sfx_jump    = { WAVE_SQUARE, 300.0f, 600.0f, 0.25f, 0.002f, 0.05f, 0.4f, 0.03f, 0.06f, 0.22f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_pick    = { WAVE_TRIANGLE, 880.0f, 1760.0f, 0.0f, 0.001f, 0.05f, 0.5f, 0.05f, 0.15f, 0.30f, 7.0f, 0.2f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_deposit = { WAVE_SQUARE, 392.0f, 784.0f, 0.5f, 0.005f, 0.15f, 0.6f, 0.2f, 0.35f, 0.30f, 5.0f, 0.25f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_fire    = { WAVE_NOISE, 900.0f, 150.0f, 0.0f, 0.01f, 0.4f, 0.3f, 0.2f, 0.6f, 0.35f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_out     = { WAVE_SAW, 330.0f, 82.0f, 0.0f, 0.001f, 0.2f, 0.4f, 0.1f, 0.3f, 0.30f, 6.0f, 0.5f , 0.0f, 0.0f, 0.0f};
static const SynthSfx k_sfx_win     = { WAVE_SQUARE, 523.0f, 1047.0f, 0.5f, 0.01f, 0.3f, 0.7f, 0.6f, 0.6f, 0.30f, 5.0f, 0.3f , 0.0f, 0.0f, 0.0f};

static void sfx(const SynthSfx* s) { synth_play(audio_synth(), s); }

/* Özgün akşam teması (La minör, yavaş): Am – F – C – G; yumuşak üçgen arpej, titreşimli lead, derin bas */
static void start_music(void) {
    Synth* s = audio_synth();
    synth_song_stop(s);
    s->song.track_count = 0;
    SynthSfx lead = { WAVE_TRIANGLE, 0.0f, 0.0f, 0.0f, 0.06f, 0.20f, 0.6f, 0.25f, 0.45f, 0.13f, 4.5f, 0.18f , 0.0f, 0.0f, 0.0f};
    SynthSfx arp  = { WAVE_SQUARE, 0.0f, 0.0f, 0.18f, 0.004f, 0.10f, 0.25f, 0.02f, 0.18f, 0.045f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
    SynthSfx bass = { WAVE_TRIANGLE, 0.0f, 0.0f, 0.0f, 0.02f, 0.30f, 0.8f, 0.6f, 0.30f, 0.22f, 0.0f, 0.0f , 0.0f, 0.0f, 0.0f};
    synth_song_add_track(s,
        "E5 - - - - - D5 - C5 - - - A4 - - - "
        "C5 - - - - - D5 - E5 - - - - - - - "
        "G5 - - - - - E5 - D5 - - - C5 - - - "
        "B4 - - - D5 - - - G4 - - - - - - - ", &lead);
    synth_song_add_track(s,
        "A3 C4 E4 C4 A3 C4 E4 C4 F3 A3 C4 A3 F3 A3 C4 A3 "
        "C4 E4 G4 E4 C4 E4 G4 E4 G3 B3 D4 B3 G3 B3 D4 B3", &arp);
    synth_song_add_track(s,
        "A2 - - - - - - - F2 - - - - - - - C3 - - - - - - - G2 - - - - - - - ", &bass);
    synth_song_start(s, 76.0f, 4);
    s->song.volume = 0.9f;
    g_o.music_playing = TRUE;
}

static void stop_music(void) {
    if (g_o.music_playing) synth_song_stop(audio_synth());
    g_o.music_playing = FALSE;
}

/* ---------- Seviye ---------- */

static Vec3 spot_pos(const SparkSpot* sp) {
    if (sp->pillar >= 0) {
        const Pillar* p = &k_pillars[sp->pillar];
        return vec3(p->x, p->top + 0.9f, p->z);
    }
    return vec3(sp->x, 0.9f, sp->z);
}

static void build_level(Arena* perm) {
    PhysWorld* w = &g_o.world;
    phys_world_init(w, perm, 4, 64, 1);
    w->gravity = vec3(0.0f, GRAVITY, 0.0f);
    phys_world_add_static(w, shape_from_plane(plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f)), 0.0f, 0.8f);
    for (u32 i = 0; i < PILLAR_COUNT; i++) {
        const Pillar* p = &k_pillars[i];
        Aabb b = aabb_make(vec3(p->x - p->half, 0.0f, p->z - p->half), vec3(p->x + p->half, p->top, p->z + p->half));
        phys_world_add_static(w, shape_from_aabb(b), 0.0f, 0.8f);
    }
    /* Ocak taşları: 10 taşlık halka */
    for (u32 i = 0; i < 10; i++) {
        f32 a = (f32)i * (M_TAU / 10.0f);
        Vec3 c = vec3(m_cos(a) * 2.2f, 0.3f, m_sin(a) * 2.2f);
        phys_world_add_static(w, shape_from_aabb(aabb_from_center_half(c, vec3(0.35f, 0.3f, 0.35f))), 0.0f, 0.8f);
    }
}

static void reset_game(void) {
    g_o.feet     = vec3(0.0f, 0.0f, 5.0f);
    g_o.vel      = vec3(0.0f, 0.0f, 0.0f);
    g_o.grounded = FALSE;
    g_o.coyote   = 0.0f;
    g_o.jump_buf = 0.0f;
    g_o.facing   = 0.0f;
    g_o.step_acc = 0.0f;
    g_o.state    = OCAK_PLAY;
    g_o.carried  = 0;
    g_o.lit      = 0;
    g_o.torch    = 1.0f;
    g_o.tod      = DAY_START;
    g_o.play_time = 0.0f;
    g_o.msg[0]   = 0;
    g_o.cam.yaw   = 0.0f;
    g_o.cam.pitch = -0.32f;
    g_o.cam_dist  = 8.0f;
    g_o.cam_dist_cur = 8.0f;
    g_o.cam.dist  = 8.0f;

    ecs_world_clear(&g_o.ecs);
    for (u32 i = 0; i < SPARK_COUNT; i++) {
        EcsEntity e = ecs_spawn(&g_o.ecs);
        Vec3 h = spot_pos(&k_spots[i]);
        Transform t = transform_trs(h, quat_identity(), vec3(0.28f, 0.28f, 0.28f));
        ecs_set(&g_o.ecs, e, g_o.c_tr, &t);
        Spark sp = { h, 0u, (f32)i * 0.83f };
        ecs_set(&g_o.ecs, e, g_o.c_spark, &sp);
    }
}

static void say(const char* text) {
    u32 i = 0;
    while (text[i] && i + 1 < sizeof(g_o.msg)) { g_o.msg[i] = text[i]; i++; }
    g_o.msg[i] = 0;
    g_o.msg_time = 3.0f;
}

void scene_ocak_init(Arena* perm, Arena* scratch) {
    __builtin_memset(&g_o, 0, sizeof(g_o));
    g_o.cube   = mesh_cube(perm, 0.5f);
    g_o.sphere = mesh_sphere(perm, 1.0f, 20, 12);
    g_o.ground = mesh_plane(perm, 160.0f, 20, 40.0f);

    tex_init_mipmapped(&g_o.tex_ground, perm, 64, 64, TEX_FLAG_BILINEAR);
    tex_gen_checker(&g_o.tex_ground, 16, FB_RGBA(120, 150, 80, 255), FB_RGBA(98, 130, 66, 255));
    tex_build_mips(&g_o.tex_ground);
    tex_init_mipmapped(&g_o.tex_stone, perm, 32, 32, TEX_FLAG_BILINEAR);
    tex_gen_checker(&g_o.tex_stone, 8, FB_RGBA(170, 160, 150, 255), FB_RGBA(140, 132, 124, 255));
    tex_build_mips(&g_o.tex_stone);
    tex_init(&g_o.tex_white, perm, 4, 4, 0);
    tex_gen_checker(&g_o.tex_white, 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 255, 255, 255));

    r3d_init(&g_o.r3d, perm, scratch, OCAK_SHADOW);
    build_level(perm);

    ecs_world_init(&g_o.ecs, perm, 256);
    transform_register(&g_o.ecs);
    g_o.c_tr    = transform_comp_id(&g_o.ecs);
    g_o.c_spark = ecs_register(&g_o.ecs, "Spark", sizeof(Spark), _Alignof(Spark), NULL);

    camera_init(&g_o.cam, CAM_ORBIT);
    g_o.music_on = TRUE;
    reset_game();
}

void scene_ocak_leave(void) {
    stop_music();
}

/* ---------- Oyun mantığı ---------- */

static Capsule player_capsule(Vec3 feet) {
    return capsule_make(vec3(feet.x, feet.y + PLAYER_R, feet.z), vec3(feet.x, feet.y + PLAYER_H - PLAYER_R, feet.z), PLAYER_R);
}

static f32 approach(f32 v, f32 target, f32 step) {
    if (v < target) return m_min(v + step, target);
    return m_max(v - step, target);
}

static void player_step(Vec3 wish) {
    f32 acc = g_o.grounded ? ACC_GROUND : ACC_AIR;
    g_o.vel.x = approach(g_o.vel.x, wish.x * RUN_SPEED, acc * STEP_DT);
    g_o.vel.z = approach(g_o.vel.z, wish.z * RUN_SPEED, acc * STEP_DT);
    g_o.vel.y += GRAVITY * STEP_DT;

    g_o.coyote   = g_o.grounded ? COYOTE_TIME : m_max(g_o.coyote - STEP_DT, 0.0f);
    g_o.jump_buf = m_max(g_o.jump_buf - STEP_DT, 0.0f);
    if (g_o.jump_buf > 0.0f && g_o.coyote > 0.0f) {
        g_o.vel.y    = JUMP_SPEED;
        g_o.jump_buf = 0.0f;
        g_o.coyote   = 0.0f;
        sfx(&k_sfx_jump);
    }

    Capsule cap = player_capsule(g_o.feet);
    SlideResult res = phys_move_and_slide(&g_o.world, &cap, &g_o.vel, STEP_DT, PHYS_DEFAULT_SLOPE_COS);
    g_o.feet = vec3(cap.a.x, cap.a.y - PLAYER_R, cap.a.z);
    g_o.grounded = res.on_ground;
    if (g_o.grounded && g_o.vel.y < 0.0f) g_o.vel.y = 0.0f;
    if (res.hit_ceiling && g_o.vel.y > 0.0f) g_o.vel.y = 0.0f;
    if (g_o.feet.y < -20.0f) { g_o.feet = vec3(0.0f, 0.0f, 5.0f); g_o.vel = vec3(0.0f, 0.0f, 0.0f); }
}

static void update_sparks(f32 dt) {
    EcsQuery q = ecs_query(&g_o.ecs, (1u << g_o.c_tr) | (1u << g_o.c_spark));
    u32 carry_slot = 0;
    while (ecs_query_next(&q)) {
        Transform* t = (Transform*)ecs_query_get(&q, g_o.c_tr);
        Spark* sp = (Spark*)ecs_query_get(&q, g_o.c_spark);
        sp->phase += dt;
        if (sp->state == 0u) {
            t->pos = vec3(sp->home.x, sp->home.y + 0.18f * m_sin(sp->phase * 2.4f), sp->home.z);
            Vec3 c = vec3(g_o.feet.x, g_o.feet.y + 0.9f, g_o.feet.z);
            if (g_o.state == OCAK_PLAY && g_o.carried < MAX_CARRY && vec3_len(vec3_sub(t->pos, c)) < PICK_R) {
                sp->state = 1u;
                g_o.carried++;
                sfx(&k_sfx_pick);
            }
        } else if (sp->state == 1u) {
            /* Oyuncunun etrafında dönen kıvılcımlar */
            f32 a = sp->phase * 3.0f + (f32)carry_slot * (M_TAU / 3.0f);
            t->pos = vec3(g_o.feet.x + m_cos(a) * 0.8f, g_o.feet.y + 1.3f + 0.15f * m_sin(sp->phase * 5.0f),
                          g_o.feet.z + m_sin(a) * 0.8f);
            carry_slot++;
        } else {
            /* Ocakta: ateşin üstünde yavaş dönen halka */
            f32 a = sp->phase * 0.9f + sp->home.x;
            t->pos = vec3(m_cos(a) * 1.1f, 1.6f + 0.25f * m_sin(sp->phase * 1.7f), m_sin(a) * 1.1f);
        }
    }
}

/* Taşınan kıvılcımları ocağa bırak (deposit) veya yerlerine geri gönder (lost) */
static void release_carried(b32 deposit) {
    EcsQuery q = ecs_query(&g_o.ecs, 1u << g_o.c_spark);
    while (ecs_query_next(&q)) {
        Spark* sp = (Spark*)ecs_query_get(&q, g_o.c_spark);
        if (sp->state == 1u) sp->state = deposit ? 2u : 0u;
    }
    if (deposit) g_o.lit += g_o.carried;
    g_o.carried = 0;
}

static void game_logic(f32 dt) {
    if (g_o.state != OCAK_PLAY) return;
    g_o.play_time += dt;
    g_o.tod += dt * (DAY_END - DAY_START) / DAY_SECONDS;

    f32 hx = g_o.feet.x, hz = g_o.feet.z;
    b32 at_hearth = (hx * hx + hz * hz) < HEARTH_R * HEARTH_R && g_o.feet.y < 1.0f;
    if (at_hearth) {
        g_o.torch = 1.0f;
        if (g_o.carried > 0) {
            u32 n = g_o.carried;
            release_carried(TRUE);
            sfx(&k_sfx_deposit);
            sfx(&k_sfx_fire);
            char buf[64];
            fmt_format(buf, sizeof(buf), "+%u kivilcim! Ocak %u/%u", n, g_o.lit, SPARK_COUNT);
            say(buf);
        }
    } else {
        g_o.torch -= dt / TORCH_SECONDS;
        if (g_o.torch <= 0.0f) {
            sfx(&k_sfx_out);
            release_carried(FALSE);
            g_o.feet = vec3(0.0f, 0.0f, 5.0f);
            g_o.vel  = vec3(0.0f, 0.0f, 0.0f);
            g_o.torch = 1.0f;
            say("Mesalen sondu! Tasidigin kivilcimlar yerine dondu.");
        }
    }

    if (g_o.lit >= SPARK_COUNT) {
        g_o.state = OCAK_WIN;
        sfx(&k_sfx_win);
        char buf[96];
        fmt_format(buf, sizeof(buf), "ATESI YAKTIN! %.1f saniye  (R: yeniden)", (f64)g_o.play_time);
        say(buf);
        g_o.msg_time = 1e9f;
    } else if (g_o.tod >= DAY_END) {
        g_o.state = OCAK_LOSE;
        sfx(&k_sfx_out);
        say("Gece coktu, ocak sogudu...  (R: yeniden)");
        g_o.msg_time = 1e9f;
    }
}

static void update_camera(const InputState* in, b32 look_enabled, f32 dt) {
    Camera* c = &g_o.cam;
    if (look_enabled && (in->mouse_buttons & 1u)) {
        c->yaw   -= (f32)in->mouse_dx * 0.006f;
        c->pitch -= (f32)in->mouse_dy * 0.006f;
        c->pitch  = m_clamp(c->pitch, -1.25f, 0.35f);
    }
    if (in->wheel != 0) g_o.cam_dist = m_clamp(g_o.cam_dist + (f32)in->wheel * 0.01f, 3.5f, 16.0f);
    c->target = vec3(g_o.feet.x, g_o.feet.y + 1.3f, g_o.feet.z);
    c->mode   = CAM_ORBIT;

    /* Kamera çarpışması: hedeften göze ışın; arada sütun/taş varsa kamera öne çekilir (anında),
     * engel kalkınca yumuşakça geri açılır */
    Vec3 back = vec3_scale(camera_forward(c), -1.0f);
    f32 want = g_o.cam_dist;
    RayHit hit;
    if (phys_world_raycast(&g_o.world, ray_make(c->target, back), want + 0.4f, &hit)) {
        want = m_max(hit.t - 0.4f, 0.8f);
    }
    if (want < g_o.cam_dist_cur) g_o.cam_dist_cur = want;
    else g_o.cam_dist_cur += (want - g_o.cam_dist_cur) * (1.0f - m_exp2(-8.66f * dt));
    c->dist = g_o.cam_dist_cur;
}

static b32 key_hit(const InputState* in, u32 key, u8* prev) {
    u8 down = in->keys[key];
    b32 hit = down && !*prev;
    *prev = down;
    return hit;
}

/* ---------- Çizim ---------- */

static void draw_world(R3dPass* p, void* user) {
    (void)user;
    f32 t = g_o.anim_time;
    Mat4 m;

    /* Oyuncu: gövde, baş, meşale sapı */
    f32 fy = g_o.facing;
    m = mat4_trs(vec3(g_o.feet.x, g_o.feet.y + 0.75f, g_o.feet.z), fy, 0.0f, vec3(0.42f, 0.75f, 0.42f));
    r3d_draw(p, g_o.sphere, &m, &g_o.tex_white, vec4(0.25f, 0.45f, 0.85f, 1.0f), 0.0f, 0.7f, G3D_CULL_BACK);
    m = mat4_trs(vec3(g_o.feet.x, g_o.feet.y + 1.55f, g_o.feet.z), fy, 0.0f, vec3(0.28f, 0.28f, 0.28f));
    r3d_draw(p, g_o.sphere, &m, &g_o.tex_white, vec4(0.95f, 0.80f, 0.65f, 1.0f), 0.0f, 0.6f, G3D_CULL_BACK);
    Vec3 hand = vec3(g_o.feet.x + m_cos(fy) * 0.45f, g_o.feet.y + 1.1f, g_o.feet.z - m_sin(fy) * 0.45f);
    m = mat4_trs(hand, 0.0f, 0.0f, vec3(0.06f, 0.35f, 0.06f));
    r3d_draw(p, g_o.cube, &m, &g_o.tex_white, vec4(0.45f, 0.30f, 0.18f, 1.0f), 0.0f, 0.9f, G3D_CULL_BACK);
    f32 flick = 0.85f + 0.15f * m_sin(t * 23.0f) * m_sin(t * 7.0f);
    f32 fs = (0.08f + 0.14f * g_o.torch) * flick;
    m = mat4_trs(vec3(hand.x, hand.y + 0.28f, hand.z), 0.0f, 0.0f, vec3(fs, fs * 1.4f, fs));
    r3d_draw_emissive(p, g_o.sphere, &m, vec4(1.0f, 0.62f + 0.3f * g_o.torch, 0.2f, 1.0f));
    if (p->gpu_active && g_o.torch > 0.02f) {
        gpu_scene_add_point_light(vec3(hand.x, hand.y + 0.35f, hand.z),
                                  11.0f * (0.3f + 0.7f * g_o.torch),
                                  vec3(3.0f * g_o.torch * flick, 1.5f * g_o.torch * flick, 0.45f * g_o.torch * flick),
                                  0.14f);
    }

    /* Kıvılcımlar */
    EcsQuery q = ecs_query(&g_o.ecs, (1u << g_o.c_tr) | (1u << g_o.c_spark));
    while (ecs_query_next(&q)) {
        const Transform* tr = (const Transform*)ecs_query_get(&q, g_o.c_tr);
        const Spark* sp = (const Spark*)ecs_query_get(&q, g_o.c_spark);
        f32 s = 0.22f + 0.05f * m_sin(sp->phase * 6.0f);
        m = mat4_trs(tr->pos, sp->phase, 0.0f, vec3(s, s, s));
        r3d_draw_emissive(p, g_o.sphere, &m, vec4(1.0f, 0.85f, 0.35f, 1.0f));
        if (p->gpu_active) {
            gpu_scene_add_point_light(tr->pos, 4.5f, vec3(1.8f, 1.3f, 0.4f), 0.35f);
        }
    }

    /* Ocak: taşlar + yanan ateş (yakılan kıvılcım sayısıyla büyür) */
    for (u32 i = 0; i < 10; i++) {
        f32 a = (f32)i * (M_TAU / 10.0f);
        m = mat4_trs(vec3(m_cos(a) * 2.2f, 0.3f, m_sin(a) * 2.2f), a, 0.0f, vec3(0.7f, 0.6f, 0.7f));
        r3d_draw(p, g_o.cube, &m, &g_o.tex_stone, vec4(1, 1, 1, 1), 0.0f, 0.9f, G3D_CULL_BACK);
    }
    f32 fire = (f32)g_o.lit / (f32)SPARK_COUNT;
    if (p->gpu_active) {
        gpu_scene_add_point_light(vec3(0.0f, 1.2f, 0.0f),
                                  8.0f + 16.0f * fire,
                                  vec3(0.8f + 3.5f * fire, 0.35f + 1.6f * fire, 0.1f + 0.4f * fire),
                                  0.08f);
    }
    for (u32 k = 0; k < 5; k++) {
        f32 ph = t * (5.0f + (f32)k) + (f32)k * 1.7f;
        f32 h  = 0.15f + 1.4f * fire * (0.6f + 0.4f * m_sin(ph));
        f32 r  = (0.15f + 0.55f * fire) * (1.0f - (f32)k * 0.13f);
        Vec3 c = vec3(0.25f * m_sin(ph * 0.7f + (f32)k), 0.2f + h * 0.5f + (f32)k * 0.15f * fire, 0.25f * m_cos(ph * 0.6f + (f32)k));
        m = mat4_trs(c, 0.0f, 0.0f, vec3(r, r * 1.5f, r));
        Vec4 col = (k < 2) ? vec4(1.0f, 0.45f, 0.12f, 1.0f) : vec4(1.0f, 0.8f, 0.3f, 1.0f);
        if (g_o.lit > 0 || k == 0) r3d_draw_emissive(p, g_o.sphere, &m, col);
    }

    /* Sütun adacıklar */
    for (u32 i = 0; i < PILLAR_COUNT; i++) {
        const Pillar* pl = &k_pillars[i];
        m = mat4_trs(vec3(pl->x, pl->top * 0.5f, pl->z), 0.0f, 0.0f, vec3(pl->half * 2.0f, pl->top, pl->half * 2.0f));
        f32 shade = 0.75f + 0.25f * (f32)(i % 3u) / 2.0f;
        r3d_draw(p, g_o.cube, &m, &g_o.tex_stone, vec4(shade, shade * 0.95f, shade * 0.88f, 1.0f), 0.0f, 0.85f, G3D_CULL_BACK);
        /* Çimenli tepe */
        m = mat4_trs(vec3(pl->x, pl->top + 0.04f, pl->z), 0.0f, 0.0f, vec3(pl->half * 2.05f, 0.08f, pl->half * 2.05f));
        r3d_draw(p, g_o.cube, &m, &g_o.tex_white, vec4(0.45f, 0.70f, 0.30f, 1.0f), 0.0f, 0.9f, G3D_CULL_BACK);
    }

    m = mat4_identity();
    r3d_draw_receiver(p, g_o.ground, &m, &g_o.tex_ground, vec4(1, 1, 1, 1), 0.0f, 0.95f);
}

void scene_ocak_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                      const InputState* in, f32 dt, b32 use_jit, b32 look_enabled) {
    if (dt > 0.1f) dt = 0.1f;
    g_o.anim_time += dt;
    if (key_hit(in, KEY_R, &g_o.prev_r)) reset_game();
    if (key_hit(in, KEY_N, &g_o.prev_n)) {
        g_o.music_on = !g_o.music_on;
        if (!g_o.music_on) stop_music();
    }
    if (g_o.music_on && !g_o.music_playing) start_music();
    if (key_hit(in, KEY_SPACE, &g_o.prev_space)) g_o.jump_buf = JUMP_BUFFER;

    update_camera(in, look_enabled, dt);

    /* Kameraya göre hareket yönü */
    Vec3 wish = vec3(0.0f, 0.0f, 0.0f);
    if (g_o.state == OCAK_PLAY) {
        Vec3 fwd = camera_forward(&g_o.cam);
        Vec3 f = vec3_norm(vec3(fwd.x, 0.0f, fwd.z));
        Vec3 rt = vec3(-f.z, 0.0f, f.x);
        if (in->keys[KEY_W]) wish = vec3_add(wish, f);
        if (in->keys[KEY_S]) wish = vec3_sub(wish, f);
        if (in->keys[KEY_D]) wish = vec3_add(wish, rt);
        if (in->keys[KEY_A]) wish = vec3_sub(wish, rt);
        f32 l = vec3_len(wish);
        if (l > 0.0f) {
            wish = vec3_scale(wish, 1.0f / l);
            g_o.facing = m_atan2(-wish.z, wish.x);
        }
    } else {
        g_o.jump_buf = 0.0f;
    }

    g_o.step_acc += dt;
    while (g_o.step_acc >= STEP_DT) {
        player_step(wish);
        g_o.step_acc -= STEP_DT;
    }
    update_sparks(dt);
    game_logic(dt);
    if (g_o.msg_time > 0.0f) g_o.msg_time -= dt;
    transform_update(&g_o.ecs);

    R3dFrame f;
    __builtin_memset(&f, 0, sizeof(f));
    f.cam           = &g_o.cam;
    f.dl            = r3d_daylight(g_o.tod);
    f.pbr           = TRUE;
    f.mipmap        = TRUE;
    f.use_jit       = use_jit;
    f.shadow_center = vec3(g_o.feet.x, 2.0f, g_o.feet.z);
    f.shadow_radius = 26.0f;
    f.fog_density   = 1.0f / 90.0f;
    f.time_sec      = g_o.anim_time;
    r3d_render(&g_o.r3d, r, fb, frame, sm, &f, draw_world, NULL, &g_o.stats_main, &g_o.stats_shadow);
}

void scene_ocak_draw_hud(UiContext* ui, const Framebuffer* fb) {
    char line[160];
    /* Üst bilgi şeridi */
    ui_draw_rect(ui, 0, 22, (i32)fb->w, 14, FB_RGBA(15, 18, 26, 170));
    f32 left = (DAY_END - g_o.tod) / (DAY_END - DAY_START) * DAY_SECONDS;
    if (left < 0.0f) left = 0.0f;
    if (engine_gpu_available()) {
        f32 gpu_ms = (f32)engine_gpu_frame_time_us() * 0.001f;
        fmt_format(line, sizeof(line), "OCAK [GPU %.2fms] %u/%u  tasinan %u/%u  batis %u:%02u",
                   (f64)gpu_ms, g_o.lit, SPARK_COUNT, g_o.carried, MAX_CARRY, (u32)left / 60u, (u32)left % 60u);
    } else {
        fmt_format(line, sizeof(line), "OCAK  %u/%u   tasinan %u/%u   gunes batana %u:%02u",
                   g_o.lit, SPARK_COUNT, g_o.carried, MAX_CARRY, (u32)left / 60u, (u32)left % 60u);
    }
    ui_draw_text(ui, 6, 25, line, FB_RGBA(255, 226, 150, 255), 1);

    /* Meşale çubuğu */
    i32 bx = (i32)fb->w - 128, bw = 120;
    ui_draw_rect(ui, bx - 2, 24, bw + 4, 10, FB_RGBA(40, 30, 20, 220));
    u32 tc = g_o.torch > 0.3f ? FB_RGBA(255, 170, 60, 255) : FB_RGBA(255, 70, 40, 255);
    ui_draw_rect(ui, bx, 26, (i32)((f32)bw * m_clamp(g_o.torch, 0.0f, 1.0f)), 6, tc);
    ui_draw_text(ui, bx - 56, 25, "mesale", FB_RGBA(255, 210, 160, 255), 1);

    if (g_o.msg[0] && g_o.msg_time > 0.0f) {
        i32 w = font_measure_width(g_o.msg, 2);
        i32 x = ((i32)fb->w - w) / 2, y = (i32)fb->h / 2 - 60;
        ui_draw_rect(ui, x - 8, y - 6, w + 16, 28, FB_RGBA(10, 10, 16, 190));
        ui_draw_text(ui, x, y, g_o.msg, FB_RGBA(255, 236, 170, 255), 2);
    }
    ui_draw_text(ui, 6, (i32)fb->h - 12, "WASD kos  Bosluk zipla  surukle: kamera  teker: yakinlik  R yeniden  N muzik",
                 FB_RGBA(200, 210, 230, 255), 1);
}

const f32* scene_ocak_debug(void) {
    static f32 d[10];
    d[0] = g_o.feet.x; d[1] = g_o.feet.y; d[2] = g_o.feet.z;
    d[3] = g_o.vel.y;  d[4] = (f32)g_o.carried; d[5] = (f32)g_o.lit;
    d[6] = g_o.torch;  d[7] = g_o.tod; d[8] = (f32)g_o.state; d[9] = g_o.cam.yaw;
    return d;
}

void scene_ocak_debug_set_yaw(f32 yaw) {
    g_o.cam.yaw = yaw;
}
