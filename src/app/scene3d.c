/* app/scene3d.c — 3D demo sahnesinin kurulumu, çizimi ve HUD'u.
 * Tüm kalıcı varlıklar (mesh'ler, dokular, gölge haritası) perm arena'da; materyal uniform blokları ve
 * doku dizileri her kare frame arena'dan ayrılır (raster flush'a kadar geçerli kalmaları gerekir).
 * Kare: (1) güneşten gölge haritası (yalnız derinlik), (2) PBR ana geçiş (pbr3d.mtk) veya eski Lambert (lit3d.mtk).
 */
#include "app/scene3d.h"
#include "core/math.h"
#include "core/log.h"
#include "raster/texture.h"
#include "core/embed.h"
#include "asset/gltf.h"
#include "asset/obj.h"
#include "img/png.h"
#include "img/image.h"
#include "plat/platform.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/shadow.h"
#include "gfx3d/billboard.h"
#include "gfx3d/particles.h"
#include "audio/audio.h"
#include "audio/synth.h"
#include "app/r3d.h"
#include "core/profiler.h"
#include "ui/font.h"

#define KEY_C 67
#define KEY_G 71
#define KEY_M 77
#define KEY_L 76
#define KEY_N 78
#define KEY_K 75
#define KEY_O 79
#define KEY_P 80
#define GRID_X 40
#define GRID_Z 25
#define SHADOW_SIZE  1024u
#define DAY_SECONDS  90.0f
#define PARTICLE_CAP 4096u
#define FIRE_RATE    90.0f
#define SMOKE_RATE   55.0f

static Mesh*          g_cube;
static Mesh*          g_ground;
static Mesh*          g_sphere;
static Texture        g_tex[3];      /* 0: zemin damalı (mip zincirli), 1: küp, 2: beyaz */
static Texture        g_tex_ground_nomip;   /* aynı pikseller, MIPMAP bayrağı yok (C referans yolu için) */
static R3dRenderer    g_r3d;
static b32            g_mipmap_on;
static b32            g_pbr_on;
static b32            g_day_paused;
static f32            g_tod;                /* gün saati [0,1): 0.25 öğle, 0.75 gece yarısı */
static u8             g_prev_m, g_prev_p, g_prev_t;
static Camera         g_cam;
static G3dStats       g_last_stats;
static G3dStats       g_shadow_stats;
static ParticleSystem g_particles;
static BillboardBatch g_billboards;
static f32            g_fire_acc;
static f32            g_smoke_acc;
static f32            g_part_update_ms;
static f32            g_part_mesh_ms;
static u8             g_prev_burst;
static b32            g_fx_enabled;

/* Patlama ses efektleri: TPT SVF alçak geçiren filtre (kesim frekansı kaymalı) */
static const SynthSfx k_sfx_burst_boom = {
    WAVE_NOISE, 650.0f, 95.0f, 0.0f,
    0.003f, 0.18f, 0.35f, 0.08f, 0.42f, 0.40f,
    0.0f, 0.0f,
    2800.0f, 180.0f, 0.45f
};
static const SynthSfx k_sfx_burst_spark = {
    WAVE_SAW, 520.0f, 110.0f, 0.0f,
    0.002f, 0.12f, 0.25f, 0.04f, 0.25f, 0.22f,
    6.0f, 0.35f,
    3600.0f, 320.0f, 0.60f
};

/* Yüklenen model (sürükle-bırak) */
typedef struct {
    Mesh*          mesh;
    Mat4           world;
    const Texture* tex;
    Vec4           color;
    b32            double_sided;
    SkinnedMesh*   skinned;      /* skin'li çizimde bağlama pozu + ağırlıklar, yoksa NULL */
    i32            skin;         /* g_anim.skins indeksi */
} ModelDraw;
#define MAX_MODEL_DRAWS 512
static ModelDraw g_model[MAX_MODEL_DRAWS];
static u32       g_model_count;
static Mat4      g_model_fit;
static char      g_model_info[160];

/* Yüklenen modelin iskelet animasyonu: K sonraki klip, O durdur/sürdür */
typedef struct {
    Skeleton* skins;
    u32       skin_count;
    AnimClip* clips;
    u32       clip_count;
    u32       clip;
    f32       time;
    b32       paused;
    f32       ms;           /* son karede örnekleme + skinning süresi */
} ModelAnim;
static ModelAnim g_anim;
static u8        g_prev_k, g_prev_o;
static u8       g_prev_c;

static ParticleEmitter make_fire_emitter(void) {
    ParticleEmitter em = particles_default_emitter();
    em.shape       = EMIT_SHAPE_SPHERE;
    em.pos         = vec3(-3.2f, 0.25f, 1.5f);
    em.radius      = 0.35f;
    em.vel_min     = vec3(-0.45f, 1.4f, -0.45f);
    em.vel_max     = vec3(0.45f, 3.0f, 0.45f);
    em.speed_min   = 0.1f;
    em.speed_max   = 0.5f;
    em.life_min    = 0.45f;
    em.life_max    = 1.1f;
    em.gravity     = vec3(0.0f, 0.8f, 0.0f);
    em.drag        = 0.6f;
    em.size_start  = 0.55f;
    em.size_end    = 0.12f;
    em.color_start = vec4(1.0f, 0.85f, 0.25f, 0.90f);
    em.color_end   = vec4(0.95f, 0.18f, 0.02f, 0.0f);
    em.rot_min     = 0.0f;
    em.rot_max     = M_TAU;
    em.rot_vel_min = -2.0f;
    em.rot_vel_max = 2.0f;
    return em;
}

static ParticleEmitter make_smoke_emitter(void) {
    ParticleEmitter em = particles_default_emitter();
    em.shape       = EMIT_SHAPE_SPHERE;
    em.pos         = vec3(-3.2f, 1.15f, 1.5f);
    em.radius      = 0.45f;
    em.vel_min     = vec3(-0.6f, 1.2f, -0.6f);
    em.vel_max     = vec3(0.6f, 2.3f, 0.6f);
    em.speed_min   = 0.05f;
    em.speed_max   = 0.25f;
    em.life_min    = 1.4f;
    em.life_max    = 2.8f;
    em.gravity     = vec3(0.25f, 0.35f, -0.1f);
    em.drag        = 0.4f;
    em.size_start  = 0.45f;
    em.size_end    = 1.35f;
    em.color_start = vec4(0.28f, 0.26f, 0.25f, 0.52f);
    em.color_end   = vec4(0.55f, 0.58f, 0.62f, 0.0f);
    em.rot_min     = 0.0f;
    em.rot_max     = M_TAU;
    em.rot_vel_min = -1.0f;
    em.rot_vel_max = 1.0f;
    return em;
}

static void emit_continuous_fx(f32 dt) {
    if (!(dt > 0.0f)) return;
    if (dt > 0.1f) dt = 0.1f;
    g_fire_acc  += dt * FIRE_RATE;
    g_smoke_acc += dt * SMOKE_RATE;
    u32 n_fire  = (u32)g_fire_acc;
    u32 n_smoke = (u32)g_smoke_acc;
    if (n_fire > 0u) {
        g_fire_acc -= (f32)n_fire;
        ParticleEmitter ef = make_fire_emitter();
        particles_emit_burst(&g_particles, &ef, n_fire);
    }
    if (n_smoke > 0u) {
        g_smoke_acc -= (f32)n_smoke;
        ParticleEmitter es = make_smoke_emitter();
        particles_emit_burst(&g_particles, &es, n_smoke);
    }
}

static void trigger_spark_burst(void) {
    ParticleEmitter sp = particles_default_emitter();
    sp.shape       = EMIT_SHAPE_SPHERE;
    sp.pos         = vec3(-3.2f, 1.2f, 1.5f);
    sp.radius      = 0.25f;
    sp.vel_min     = vec3(-2.5f, 2.0f, -2.5f);
    sp.vel_max     = vec3(2.5f, 7.5f, 2.5f);
    sp.speed_min   = 3.0f;
    sp.speed_max   = 8.5f;
    sp.life_min    = 0.6f;
    sp.life_max    = 1.6f;
    sp.gravity     = vec3(0.0f, -9.8f, 0.0f);
    sp.drag        = 0.8f;
    sp.size_start  = 0.28f;
    sp.size_end    = 0.04f;
    sp.color_start = vec4(1.0f, 0.96f, 0.55f, 1.0f);
    sp.color_end   = vec4(1.0f, 0.30f, 0.05f, 0.0f);
    sp.rot_min     = 0.0f;
    sp.rot_max     = M_TAU;
    sp.rot_vel_min = -4.0f;
    sp.rot_vel_max = 4.0f;
    particles_emit_burst(&g_particles, &sp, 240u);

    ParticleEmitter sm = make_smoke_emitter();
    sm.pos         = vec3(-3.2f, 1.2f, 1.5f);
    sm.speed_min   = 1.0f;
    sm.speed_max   = 3.2f;
    sm.size_start  = 0.55f;
    sm.size_end    = 1.60f;
    sm.color_start = vec4(0.85f, 0.50f, 0.20f, 0.65f);
    sm.color_end   = vec4(0.35f, 0.35f, 0.38f, 0.0f);
    particles_emit_burst(&g_particles, &sm, 40u);

    Synth* s = audio_synth();
    synth_set_reverb(s, 0.22f, 0.65f, 0.40f);
    synth_play(s, &k_sfx_burst_boom);
    synth_play(s, &k_sfx_burst_spark);
}

void scene3d_init(Arena* perm, Arena* scratch, Framebuffer* fb) {
    if (fb->depth == NULL) fb_init_depth(fb, perm);

    g_cube   = mesh_cube(perm, 0.5f);
    g_ground = mesh_plane(perm, 240.0f, 24, 60.0f);   /* 4 m'lik hücreler; düzlemi kesen üçgenler */
    g_sphere = mesh_sphere(perm, 1.0f, 32, 16);

    tex_init_mipmapped(&g_tex[0], perm, 64, 64, TEX_FLAG_BILINEAR);
    tex_gen_checker(&g_tex[0], 8, FB_RGBA(235, 235, 228, 255), FB_RGBA(70, 82, 96, 255));
    tex_build_mips(&g_tex[0]);
    g_tex_ground_nomip        = g_tex[0];
    g_tex_ground_nomip.flags &= ~TEX_FLAG_MIPMAP;
    g_tex_ground_nomip.levels = 1u;
    g_mipmap_on = TRUE;
    g_pbr_on    = TRUE;
    g_day_paused = FALSE;
    g_tod       = 0.13f;     /* sabah */
    g_prev_m = g_prev_p = g_prev_t = g_prev_burst = 0;
    r3d_init(&g_r3d, perm, scratch, SHADOW_SIZE);
    tex_init(&g_tex[1], perm, 32, 32, TEX_FLAG_BILINEAR);
    tex_gen_gradient(&g_tex[1], FB_RGBA(255, 220, 90, 255), FB_RGBA(255, 120, 60, 255),
                     FB_RGBA(90, 200, 255, 255), FB_RGBA(150, 90, 255, 255));
    tex_init(&g_tex[2], perm, 4, 4, 0);
    tex_gen_checker(&g_tex[2], 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 255, 255, 255));

    camera_init(&g_cam, CAM_FLY);
    g_cam.pos   = vec3(0.0f, 3.0f, 14.0f);
    g_cam.pitch = -0.18f;
    g_prev_c    = 0;

    particles_init(&g_particles, perm, PARTICLE_CAP, 0x5CE3D001ULL);
    billboard_init(&g_billboards, perm, PARTICLE_CAP);
    g_fire_acc       = 0.0f;
    g_smoke_acc      = 0.0f;
    g_part_update_ms = 0.0f;
    g_part_mesh_ms   = 0.0f;
    g_fx_enabled     = TRUE;
    /* İlk karede ateş ve duman bulutu hazır görünsün (1.5 s ön ısıtma) */
    for (u32 i = 0; i < 30u; i++) {
        emit_continuous_fx(0.05f);
        particles_update(&g_particles, 0.05f);
    }
    particles_build_billboards(&g_particles, &g_billboards, camera_view(&g_cam), TRUE);
}

static Vec4 hue_color(f32 h) {
    f32 r = m_clamp(m_abs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    f32 g = m_clamp(2.0f - m_abs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    f32 b = m_clamp(2.0f - m_abs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    return vec4(0.35f + 0.65f * r, 0.35f + 0.65f * g, 0.35f + 0.65f * b, 1.0f);
}

/* Sahnedeki tüm nesneler (gölge ve ana geçiş aynı listeyi kullanır). Zemin gölge düşürmez. */
static void draw_scene(R3dPass* p, void* user) {
    f32 time_sec = *(const f32*)user;
    /* Önden arkaya kaba sıra early-Z'ye yardım eder: önce yakın nesneler, zemin en son */
    Mat4 m = mat4_trs(vec3(0.0f, 1.6f, 0.0f), time_sec * 0.7f, time_sec * 0.45f, vec3(2.2f, 2.2f, 2.2f));
    r3d_draw(p, g_cube, &m, &g_tex[1], vec4(1, 1, 1, 1), 0.0f, 0.45f, G3D_CULL_BACK);

    m = mat4_trs(vec3(4.0f, 1.2f + 0.4f * m_sin(time_sec * 1.3f), -1.0f), 0.0f, 0.0f, vec3(1.2f, 1.2f, 1.2f));
    r3d_draw(p, g_sphere, &m, &g_tex[2], vec4(0.95f, 0.75f, 0.35f, 1.0f), 1.0f, 0.25f, G3D_CULL_BACK);

    /* 1000 küplük ızgara: sütunlar boyunca metallik (5 adım), satırlar boyunca pürüzlülük (5 adım) */
    for (u32 j = 0; j < GRID_Z; j++) {
        for (u32 i = 0; i < GRID_X; i++) {
            f32 x = ((f32)i - (f32)(GRID_X - 1) * 0.5f) * 3.0f;
            f32 z = -10.0f - (f32)j * 3.0f;
            f32 ph = (f32)(i * 7u + j * 13u) * 0.37f;
            f32 h = 0.6f + 0.5f * m_sin(time_sec * 0.9f + ph);
            m = mat4_trs(vec3(x, h, z), time_sec * 0.5f + ph, 0.0f, vec3(1.0f, 1.0f, 1.0f));
            f32 metal = (f32)(i % 5u) * 0.25f;
            f32 rough = 0.1f + (f32)(j % 5u) * 0.2f;
            r3d_draw(p, g_cube, &m, &g_tex[2], hue_color((f32)((i * 37u + j * 11u) % 64u) / 64.0f),
                     metal, rough, G3D_CULL_BACK);
        }
    }

    /* Sürükle-bırak ile yüklenen model */
    for (u32 i = 0; i < g_model_count; i++) {
        const ModelDraw* d = &g_model[i];
        m = mat4_mul(g_model_fit, d->world);
        r3d_draw(p, d->mesh, &m, d->tex, d->color, 0.0f, 0.6f, d->double_sided ? G3D_CULL_NONE : G3D_CULL_BACK);
    }

    m = mat4_identity();
    r3d_draw_receiver(p, g_ground, &m, g_mipmap_on ? &g_tex[0] : &g_tex_ground_nomip, vec4(1, 1, 1, 1), 0.0f, 0.85f);

    /* Alfa harmanlı parçacıklar en son çizilir; billboard mesh'i g3d_flush'a kadar sabit kalır */
    if (g_fx_enabled) {
        r3d_draw_particles(p, g_billboards.mesh);
    }
}

/* Animasyonlu çizimleri bu karenin pozuyla günceller: klip yalnız 0. iskelete uygulanır (kanallar glTF
 * yükleyicide ilk eşleşen iskeletin eklemlerine bağlanır), diğer iskeletler bağlama pozunda kalır. */
static void animate_model(Arena* frame, f32 dt) {
    if (g_anim.skin_count == 0u) return;
    f64 t0 = plat_time_ms();
    if (!g_anim.paused) g_anim.time += dt;
    usize mark = arena_mark(frame);
    for (u32 s = 0; s < g_anim.skin_count; s++) {
        const Skeleton* sk = &g_anim.skins[s];
        u32 jc = sk->joint_count;
        if (jc == 0u) continue;
        JointPose* poses = ARENA_PUSH_ARRAY(frame, JointPose, jc);
        Mat4* mats = ARENA_PUSH_ARRAY(frame, Mat4, jc);
        b32 ok = FALSE;
        if (s == 0u && g_anim.clip_count > 0u) ok = clip_sample(&g_anim.clips[g_anim.clip], sk, g_anim.time, TRUE, poses);
        if (!skeleton_compute_matrices(sk, ok ? poses : NULL, NULL, mats)) continue;
        for (u32 i = 0; i < g_model_count; i++) {
            ModelDraw* d = &g_model[i];
            if (d->skinned && d->skin == (i32)s) skin_mesh(d->skinned, mats, jc, d->mesh);
        }
        arena_pop_to(frame, mark);
    }
    g_anim.ms = (f32)(plat_time_ms() - t0);
}

static b32 key_pressed(const InputState* in, u32 key, u8* prev) {
    u8 down = in->keys[key];
    b32 hit = down && !*prev;
    *prev = down;
    return hit;
}

void scene3d_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                   const InputState* in, f32 dt, f32 time_sec, b32 use_jit, b32 look_enabled) {
    if (key_pressed(in, KEY_C, &g_prev_c)) {
        if (g_cam.mode == CAM_FLY) {
            g_cam.mode   = CAM_ORBIT;
            g_cam.target = vec3_add(g_cam.pos, vec3_scale(camera_forward(&g_cam), g_cam.dist));
        } else {
            g_cam.pos  = camera_eye(&g_cam);
            g_cam.mode = CAM_FLY;
        }
    }
    if (key_pressed(in, KEY_M, &g_prev_m)) {
        g_mipmap_on = !g_mipmap_on;
        LOG("Mipmap: %s", g_mipmap_on ? "acik" : "kapali");
    }
    if (key_pressed(in, KEY_L, &g_prev_p)) {
        g_pbr_on = !g_pbr_on;
        LOG("Aydinlatma: %s", g_pbr_on ? "PBR + golge" : "Lambert");
    }
    if (key_pressed(in, KEY_N, &g_prev_t)) g_day_paused = !g_day_paused;
    f32 speed = in->keys[KEY_G] ? 20.0f : 1.0f;
    if (!g_day_paused || in->keys[KEY_G]) {
        g_tod += dt * speed / DAY_SECONDS;
        g_tod -= m_floor(g_tod);
    }
    camera_update(&g_cam, in, dt, look_enabled);
    if (key_pressed(in, KEY_K, &g_prev_k) && g_anim.clip_count > 0u) {
        g_anim.clip = (g_anim.clip + 1u) % g_anim.clip_count;
        g_anim.time = 0.0f;
        LOG("Animasyon: %u/%u '%s'", g_anim.clip + 1u, g_anim.clip_count, g_anim.clips[g_anim.clip].name);
    }
    if (key_pressed(in, KEY_O, &g_prev_o)) g_anim.paused = !g_anim.paused;
    animate_model(frame, dt);

    if (key_pressed(in, KEY_P, &g_prev_burst)) {
        trigger_spark_burst();
    }
    if (g_fx_enabled) {
        PROF_BEGIN("particles");
        f64 t0 = plat_time_ms();
        emit_continuous_fx(dt);
        particles_update(&g_particles, dt);
        f64 t1 = plat_time_ms();
        particles_build_billboards(&g_particles, &g_billboards, camera_view(&g_cam), TRUE);
        f64 t2 = plat_time_ms();
        g_part_update_ms = (f32)(t1 - t0);
        g_part_mesh_ms   = (f32)(t2 - t1);
        PROF_END();
    } else {
        g_part_update_ms = 0.0f;
        g_part_mesh_ms   = 0.0f;
    }

    /* Gölge: kameranın önündeki 36 birimlik küre */
    Vec3 eye = camera_eye(&g_cam);
    Vec3 fwd = camera_forward(&g_cam);
    Vec3 c = vec3_add(eye, vec3_scale(vec3(fwd.x, 0.0f, fwd.z), 22.0f));
    c.y = 1.0f;

    R3dFrame f;
    f.cam           = &g_cam;
    f.dl            = r3d_daylight(g_tod);
    f.pbr           = g_pbr_on;
    f.mipmap        = g_mipmap_on;
    f.use_jit       = use_jit;
    f.shadow_center = c;
    f.shadow_radius = 36.0f;
    f.fog_density   = 1.0f / 160.0f;
    r3d_render(&g_r3d, r, fb, frame, sm, &f, draw_scene, &time_sec, &g_last_stats, &g_shadow_stats);
}

void scene3d_bench_set(b32 pbr, f32 tod) {
    g_pbr_on = (pbr & 1) != 0;
    if (pbr & 2) trigger_spark_burst();
    if (tod <= -10.0f) {
        g_fx_enabled = FALSE;
    } else {
        g_fx_enabled = TRUE;
        if (tod >= 0.0f) { g_tod = tod; g_day_paused = TRUE; }
    }
}

void scene3d_draw_hud(UiContext* ui, const Framebuffer* fb) {
    char line[160];
    const G3dStats* s = &g_last_stats;
    i32 y = (i32)fb->h - 22;
    ui_draw_rect(ui, 0, y - 14, (i32)fb->w, 36, FB_RGBA(15, 18, 26, 190));
    fmt_format(line, sizeof(line),
               "[P] Patlama | Parcacik %u/%u | sim+mesh %.3f ms (sim %.3f + mesh %.3f)",
               g_particles.count, g_particles.capacity,
               (f64)(g_part_update_ms + g_part_mesh_ms),
               (f64)g_part_update_ms, (f64)g_part_mesh_ms);
    ui_draw_text(ui, 4, y - 11, line, FB_RGBA(255, 205, 130, 255), 1);
    fmt_format(line, sizeof(line),
               "Nesne %llu (culled %llu) | Ucgen %llu -> ciz %llu, kirp %llu, arka %llu, red %llu | golge %llu",
               (unsigned long long)s->draws, (unsigned long long)s->draws_culled,
               (unsigned long long)s->tris_in, (unsigned long long)s->tris_out,
               (unsigned long long)s->tris_clipped, (unsigned long long)s->tris_backface,
               (unsigned long long)s->tris_rejected,
               (unsigned long long)(g_pbr_on ? g_shadow_stats.tris_out : 0));
    ui_draw_text(ui, 4, y, line, FB_RGBA(225, 235, 248, 255), 1);
    u32 hh = (u32)(g_tod * 24.0f + 6.0f) % 24u;   /* tod 0 = 06:00 gün doğumu */
    f32 hf = g_tod * 24.0f;
    u32 mm = (u32)((hf - m_floor(hf)) * 60.0f);
    fmt_format(line, sizeof(line), "[C] %s [M] mip %s [L] %s [N] %s [G] hizli %02u:%02u | (%.1f, %.1f, %.1f)",
               g_cam.mode == CAM_FLY ? "Ucus" : "Orbit", g_mipmap_on ? "acik" : "kapali",
               g_pbr_on ? "PBR+golge" : "Lambert", g_day_paused ? "durdu" : "gun", hh, mm,
               (f64)camera_eye(&g_cam).x, (f64)camera_eye(&g_cam).y, (f64)camera_eye(&g_cam).z);
    ui_draw_text(ui, 4, y + 11, line, FB_RGBA(150, 200, 255, 255), 1);
    if (g_anim.clip_count > 0u) {
        fmt_format(line, sizeof(line), "[K] klip %u/%u %s  [O] %s  t=%.2f s  poz+skin %.3f ms",
                   g_anim.clip + 1u, g_anim.clip_count, g_anim.clips[g_anim.clip].name,
                   g_anim.paused ? "durdu" : "oynuyor", (f64)g_anim.time, (f64)g_anim.ms);
        ui_draw_rect(ui, 0, y - 26, (i32)fb->w, 12, FB_RGBA(15, 18, 26, 190));
        ui_draw_text(ui, 4, y - 24, line, FB_RGBA(180, 255, 180, 255), 1);
    }
    const char* hint = g_model_info[0] ? g_model_info : ".glb / .gltf / .obj dosyasini pencereye surukle-birak";
    i32 hw = font_measure_width(hint, 1);
    ui_draw_rect(ui, (i32)fb->w - hw - 10, 22, hw + 8, 12, FB_RGBA(15, 18, 26, 190));
    ui_draw_text(ui, (i32)fb->w - hw - 6, 24, hint, FB_RGBA(255, 236, 140, 255), 1);
}

/* ---------- Model yükleme ---------- */

static b32 name_has_ext(const char* n, const char* ext) {
    u32 ln = 0, le = 0;
    while (n[ln]) ln++;
    while (ext[le]) le++;
    if (ln < le) return FALSE;
    for (u32 i = 0; i < le; i++) {
        char a = n[ln - le + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}

/* "dizin/ad.png" ile "ad.png" eşleşsin: yalnız son yol bileşeni karşılaştırılır */
static const char* base_name(const char* s) {
    const char* b = s;
    for (const char* p = s; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static b32 same_name(const char* a, const char* b) {
    a = base_name(a);
    b = base_name(b);
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

typedef struct { const DroppedFile* files; u32 count; } FileSet;

static const u8* resolve_file(void* user, const char* uri, u32* out_len) {
    const FileSet* fs = (const FileSet*)user;
    for (u32 i = 0; i < fs->count; i++) {
        if (same_name(fs->files[i].name, uri)) { *out_len = fs->files[i].len; return fs->files[i].data; }
    }
    return NULL;
}

static b32 png_to_texture(const u8* data, u32 len, Arena* out, Arena* scratch, Texture* t) {
    u32 w = 0, h = 0;
    u32* px = png_decode(data, len, &w, &h, scratch);
    if (!px || !w || !h) return FALSE;
    u32 nw = 1, nh = 1;
    while (nw < w && nw < 1024) nw <<= 1;
    while (nh < h && nh < 1024) nh <<= 1;
    tex_init_mipmapped(t, out, nw, nh, TEX_FLAG_BILINEAR);
    if (nw == w && nh == h) { for (u32 i = 0; i < w * h; i++) t->px[i] = px[i]; }
    else img_resize_bilinear(px, w, h, t->px, nw, nh);
    tex_build_mips(t);
    return TRUE;
}

/* Modeli zemine oturt, en büyük boyutu 4 birime ölçekle, (-6, 0, 2) civarına yerleştir */
static void fit_model(Vec3 lo, Vec3 hi) {
    Vec3 ext = vec3_sub(hi, lo);
    f32 big = m_max(ext.x, m_max(ext.y, ext.z));
    f32 s = (big > 0.0f) ? 4.0f / big : 1.0f;
    Vec3 c = vec3((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
    g_model_fit = mat4_mul(mat4_translate(vec3(-6.0f, 0.0f, 2.0f)),
                           mat4_mul(mat4_scale(vec3(s, s, s)), mat4_translate(vec3_scale(c, -1.0f))));
}

b32 scene3d_load_files(const DroppedFile* files, u32 count, Arena* out, Arena* scratch) {
    g_model_count = 0;
    __builtin_memset(&g_anim, 0, sizeof(g_anim));
    g_model_info[0] = 0;
    FileSet fs = { files, count };
    f64 t0 = plat_time_ms();
    for (u32 i = 0; i < count; i++) {
        const DroppedFile* f = &files[i];
        if (name_has_ext(f->name, ".glb") || name_has_ext(f->name, ".gltf")) {
            GltfModel gm;
            if (!gltf_load(f->data, f->len, out, scratch, resolve_file, &fs, &gm)) {
                fmt_format(g_model_info, sizeof(g_model_info), "%s: HATA %s", base_name(f->name), gm.err);
                LOG("glTF yuklenemedi: %s", gm.err);
                return FALSE;
            }
            for (u32 k = 0; k < gm.draw_count && g_model_count < MAX_MODEL_DRAWS; k++) {
                const GltfDraw* gd = &gm.draws[k];
                ModelDraw* d = &g_model[g_model_count++];
                d->mesh = gd->mesh;
                d->world = gd->world;
                d->tex = &g_tex[2];
                d->color = vec4(1, 1, 1, 1);
                d->double_sided = FALSE;
                d->skinned = NULL;
                d->skin = -1;
                if (gd->skinned && gd->skin >= 0) {
                    /* Kendi çıktı mesh'i: her kare bağlama pozundan yeniden kaplanır */
                    d->skinned = gd->skinned;
                    d->skin = gd->skin;
                    d->mesh = skin_clone_mesh(out, gd->skinned->bind_mesh);
                }
                if (gd->material >= 0) {
                    const GltfMaterial* mt = &gm.materials[gd->material];
                    d->color = mt->base_color;
                    d->double_sided = mt->double_sided;
                    if (mt->base_texture >= 0) d->tex = &gm.textures[mt->base_texture];
                }
            }
            fit_model(gm.bounds_min, gm.bounds_max);
            g_anim.skins = gm.skins;
            g_anim.skin_count = gm.skin_count;
            g_anim.clips = gm.animations;
            g_anim.clip_count = gm.anim_count;
            fmt_format(g_model_info, sizeof(g_model_info), "%s: %u cizim, %u ucgen, %u iskelet, %u klip, %u uyari, %.1f ms",
                       base_name(f->name), gm.draw_count, gm.triangles, gm.skin_count, gm.anim_count, gm.warnings,
                       plat_time_ms() - t0);
            if (gm.warnings) LOG("glTF uyarilari: %s", gm.warn);
            LOG("glTF sureleri: JSON %.2f ms, doku (PNG+mip) %.2f ms, mesh %.2f ms", gm.t_json_ms, gm.t_tex_ms, gm.t_mesh_ms);
            LOG("%s", g_model_info);
            return TRUE;
        }
        if (name_has_ext(f->name, ".obj")) {
            ObjInfo oi;
            Mesh* mesh = obj_load((const char*)f->data, f->len, out, scratch, &oi);
            if (!mesh) {
                fmt_format(g_model_info, sizeof(g_model_info), "%s: HATA satir %u: %s", base_name(f->name), oi.err_line, oi.err);
                return FALSE;
            }
            ModelDraw* d = &g_model[g_model_count++];
            d->skinned = NULL;
            d->skin = -1;
            d->mesh = mesh;
            d->world = mat4_identity();
            d->tex = &g_tex[2];
            d->color = vec4(1, 1, 1, 1);
            d->double_sided = FALSE;
            u32 mlen = 0;
            const u8* mtl = oi.mtllib[0] ? resolve_file(&fs, oi.mtllib, &mlen) : NULL;
            if (mtl) {
                MtlInfo mi;
                if (mtl_load_first((const char*)mtl, mlen, &mi)) {
                    d->color = vec4(mi.kd[0], mi.kd[1], mi.kd[2], 1.0f);
                    u32 plen = 0;
                    const u8* png = mi.map_kd[0] ? resolve_file(&fs, mi.map_kd, &plen) : NULL;
                    Texture* t = ARENA_PUSH_STRUCT(out, Texture);
                    if (png && png_to_texture(png, plen, out, scratch, t)) d->tex = t;
                }
            }
            fit_model(mesh->aabb_min, mesh->aabb_max);
            fmt_format(g_model_info, sizeof(g_model_info), "%s: %u kose, %u ucgen%s, %.1f ms", base_name(f->name),
                       mesh->vertex_count, oi.triangles, oi.generated_normals ? " (normal uretildi)" : "",
                       plat_time_ms() - t0);
            LOG("%s", g_model_info);
            return TRUE;
        }
    }
    fmt_format(g_model_info, sizeof(g_model_info), "Model bulunamadi (.glb/.gltf/.obj bekleniyor)");
    return FALSE;
}
