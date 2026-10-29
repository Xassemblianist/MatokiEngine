/* app/scene_gelgit.c — "Gelgit" (OYUN1) sahnesi implementasyonu.
 * 60 Hz deterministik Sim çekirdeği, 3 kademeli Tessendorf FFT okyanus, XPBD kumaş yelkenler,
 * 4 takımada + liman iskelesi + 2 döner ışıklı deniz feneri + şamandıralar + hayalet tekne,
 * prosedürel deniz/fırtına ses sentezi, 2D pruva spreyi/yağmur, rüzgâr gülü HUD'u,
 * görev/tersane menüsü (M) ve canlı fizik ayar kaydırıcıları (F2).
 */
#include "app/scene_gelgit.h"
#include "game/gelgit.h"
#include "gfx3d/ocean.h"
#include "gfx3d/sail.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/camera.h"
#include "gfx3d/mesh.h"
#include "gpu/rhi.h"
#include "gpu/gpu_ocean.h"
#include "audio/audio.h"
#include "audio/synth.h"
#include "raster/texture.h"
#include "ui/font.h"
#include "core/embed.h"
#include "core/math.h"
#include "core/log.h"
#include "core/profiler.h"
#include "plat/platform.h"

/* Tuş kodları */
#define KEY_SPACE 32
#define KEY_LEFT  37
#define KEY_UP    38
#define KEY_RIGHT 39
#define KEY_DOWN  40
#define KEY_1     49
#define KEY_8     56
#define KEY_C     67
#define KEY_F     70
#define KEY_G     71
#define KEY_H     72
#define KEY_M     77
#define KEY_N     78
#define KEY_P     80
#define KEY_R     82
#define KEY_U     85
#define KEY_V     86
#define KEY_F2    113

#define GELGIT_REPLAY_MAX_FRAMES 7200u   /* 120 saniye @ 60 Hz */
#define GELGIT_MAX_SPRAY         96u

/* ---------- Shader Uniform Blokları (CPU yedek yolu için, 16B hizalı) ---------- */

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 sea_deep;
    _Alignas(16) Vec4 sea_shallow;
    _Alignas(16) Vec4 eye;
    _Alignas(16) Vec4 scales;
    _Alignas(16) Vec4 params;
} GelgitOceanUniforms;

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 params;
} GelgitSkyUniforms;

typedef struct {
    _Alignas(16) Vec4 light_dir;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
} GelgitLitUniforms;

typedef struct {
    _Alignas(16) Vec4 color;
    _Alignas(16) Vec4 fog;
} GelgitUnlitUniforms;

/* 2B Pruva Spreyi Parçacığı (Görsel sınıf, D-112) */
typedef struct {
    Vec3 pos;
    Vec3 vel;
    f32  life;
    f32  max_life;
    f32  size;
} SprayParticle;

typedef struct {
    Sim            sim;
    GelgitSimCtx   sim_ctx;
    SimReplay      cur_replay;
    SimReplay      best_daily_replay;
    b32            has_best_replay;

    Ocean          ocean;
    Camera         cam;
    SailCloth      main_sail;
    SailCloth      jib_sail;
    Mesh*          sky_mesh;
    Mesh*          hull_mesh;
    Mesh*          unit_cube;
    Texture        tex_hull;
    Texture        tex_deck;
    Texture        tex_sail;

    f32            time_scale;
    b32            paused;
    u32            wireframe;
    b32            show_hud;
    b32            show_menu;      /* [M] Görev & Tersane menüsü */
    b32            show_tuning;    /* [F2] Canlı fizik ayar kaydırıcıları */
    b32            follow_cam;     /* TRUE: tekneyi takip eden kamera, FALSE: serbest orbit */

    /* Ses zamanlayıcıları */
    f32            sfx_wind_cd;
    f32            sfx_water_cd;
    f32            sfx_flap_cd;
    f32            sfx_creak_cd;
    u32            prev_stage;
    u32            prev_status;
    u32            prev_thunder_tick;

    /* Pruva spreyi parçacık havuzu */
    SprayParticle  spray[GELGIT_MAX_SPRAY];
    u32            spray_head;
    u32            spray_rng;

    /* Ölçümler */
    f32            sim_ms;
    f32            fft_ms;
    f32            mesh_ms;
    f32            shade_ms;
    u32            tris_drawn;
    u64            last_state_hash;
    f32            debug_buf[16];

    f32            last_ocean_wind_spd;
    f32            last_ocean_wind_x;
    f32            last_ocean_wind_z;
    b32            gpu_spectrum_dirty;
    b32            gpu_first_update;

    u8             prev_keys[256];
} GelgitScene;

static GelgitScene g_gg;

static b32 gg_key_pressed(const InputState* in, u32 key) {
    if (key >= 256u) return FALSE;
    return in->keys[key] != 0u && g_gg.prev_keys[key] == 0u;
}

static GelgitState* gg_state(void) {
    return (GelgitState*)sim_state(&g_gg.sim);
}

static const GelgitState* gg_state_const(void) {
    return (const GelgitState*)sim_state_const(&g_gg.sim);
}

/* ---------- Prosedürel Dokular ve Gövde Ağı ---------- */

static void gg_build_wood_tex(Texture* out, Arena* perm, u32 r_base, u32 g_base, u32 b_base, b32 plank_lines) {
    u32 w = 64u, h = 64u;
    tex_init_mipmapped(out, perm, w, h, TEX_FLAG_BILINEAR);
    u32* px = out->px;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 hsh = (x * 1973u + y * 9277u + 1013u) * 2654435761u;
            i32 grain = (i32)((hsh >> 24) & 15u) - 8;
            f32 wave = m_sin((f32)x * 0.22f + m_sin((f32)y * 0.35f) * 1.4f);
            i32 dv = grain + (i32)(wave * 10.0f);
            if (plank_lines && ((y & 7u) == 0u || ((x + (y >> 3) * 19u) & 31u) == 0u)) {
                dv -= 28;
            }
            i32 r = CLAMP((i32)r_base + dv, 10, 255);
            i32 g = CLAMP((i32)g_base + (dv * 3) / 4, 10, 255);
            i32 b = CLAMP((i32)b_base + dv / 2, 10, 255);
            px[y * w + x] = FB_RGBA((u32)r, (u32)g, (u32)b, 255u);
        }
    }
    tex_build_mips(out);
}

static void gg_build_sail_tex(Texture* out, Arena* perm) {
    u32 w = 64u, h = 64u;
    tex_init_mipmapped(out, perm, w, h, TEX_FLAG_BILINEAR);
    u32* px = out->px;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 hsh = (x * 3313u + y * 6971u + 79u) * 2654435761u;
            i32 weave = (i32)((hsh >> 25) & 7u) - 3;
            if ((y & 15u) == 0u) weave -= 14;
            i32 r = CLAMP(244 + weave, 180, 255);
            i32 g = CLAMP(236 + weave, 175, 255);
            i32 b = CLAMP(218 + weave, 160, 255);
            px[y * w + x] = FB_RGBA((u32)r, (u32)g, (u32)b, 255u);
        }
    }
    tex_build_mips(out);
}

static Mesh* gg_build_hull_mesh(Arena* a) {
    static const f32 z_st[9]  = { -3.6f, -3.0f, -2.0f, -0.8f,  0.4f,  1.6f,  2.7f,  3.6f,  4.25f };
    static const f32 bw_st[9] = {  0.92f, 1.22f, 1.45f, 1.52f, 1.44f, 1.20f, 0.78f, 0.32f, 0.02f };
    static const f32 kl_st[9] = { -0.42f,-0.62f,-0.74f,-0.78f,-0.75f,-0.68f,-0.55f,-0.32f, 0.15f };
    static const f32 sh_st[9] = {  0.72f, 0.64f, 0.56f, 0.52f, 0.54f, 0.60f, 0.70f, 0.82f, 0.92f };

    const u32 st_n = 9u;
    const u32 ring_n = 7u;
    u32 v_count = st_n * ring_n + st_n * 2u + ring_n;
    u32 i_count = (st_n - 1u) * (ring_n - 1u) * 6u + (st_n - 1u) * 6u + 5u * 3u;
    Mesh* m = mesh_alloc(a, v_count, i_count, FALSE);

    for (u32 s = 0; s < st_n; s++) {
        f32 z  = z_st[s];
        f32 bw = bw_st[s];
        f32 kl = kl_st[s];
        f32 sh = sh_st[s];
        f32 ch_y1 = kl + (sh - kl) * 0.34f;
        f32 ch_y2 = kl + (sh - kl) * 0.72f;
        f32 xs[7] = { -bw, -bw * 0.94f, -bw * 0.55f, 0.0f, bw * 0.55f, bw * 0.94f, bw };
        f32 ys[7] = {  sh,  ch_y2,       ch_y1,      kl,   ch_y1,      ch_y2,      sh };
        for (u32 r = 0; r < ring_n; r++) {
            u32 vi = s * ring_n + r;
            m->px[vi] = xs[r];
            m->py[vi] = ys[r];
            m->pz[vi] = z;
            m->u[vi]  = (f32)s / (f32)(st_n - 1u) * 3.0f;
            m->v[vi]  = (f32)r / (f32)(ring_n - 1u) * 2.0f;
        }
    }

    u32* ix = m->indices;
    for (u32 s = 0; s < st_n - 1u; s++) {
        for (u32 r = 0; r < ring_n - 1u; r++) {
            u32 a0 = s * ring_n + r;
            u32 a1 = a0 + 1u;
            u32 b0 = a0 + ring_n;
            u32 b1 = b0 + 1u;
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }

    u32 deck_base = st_n * ring_n;
    for (u32 s = 0; s < st_n; s++) {
        f32 z  = z_st[s];
        f32 bw = bw_st[s] * 0.96f;
        f32 dy = sh_st[s] - 0.06f;
        u32 v0 = deck_base + s * 2u;
        u32 v1 = v0 + 1u;
        m->px[v0] = -bw; m->py[v0] = dy; m->pz[v0] = z;
        m->u[v0]  = 0.0f; m->v[v0] = (f32)s * 0.5f;
        m->px[v1] =  bw; m->py[v1] = dy; m->pz[v1] = z;
        m->u[v1]  = 1.0f; m->v[v1] = (f32)s * 0.5f;
    }
    for (u32 s = 0; s < st_n - 1u; s++) {
        u32 a0 = deck_base + s * 2u;
        u32 a1 = a0 + 1u;
        u32 b0 = a0 + 2u;
        u32 b1 = a0 + 3u;
        *ix++ = a0; *ix++ = a1; *ix++ = b1;
        *ix++ = a0; *ix++ = b1; *ix++ = b0;
    }

    u32 tr_base = deck_base + st_n * 2u;
    for (u32 r = 0; r < ring_n; r++) {
        u32 src = r;
        u32 dst = tr_base + r;
        m->px[dst] = m->px[src];
        m->py[dst] = m->py[src];
        m->pz[dst] = m->pz[src];
        m->u[dst]  = (f32)r / 6.0f;
        m->v[dst]  = 0.5f;
    }
    for (u32 r = 1; r < ring_n - 1u; r++) {
        *ix++ = tr_base;
        *ix++ = tr_base + r;
        *ix++ = tr_base + r + 1u;
    }

    mesh_compute_normals(m);
    mesh_compute_bounds(m);
    return m;
}

static Mat4 gg_mat4_rot_z(f32 rad) {
    f32 c = m_cos(rad), s = m_sin(rad);
    Mat4 m = mat4_identity();
    m.m[0] =  c; m.m[4] = -s;
    m.m[1] =  s; m.m[5] =  c;
    return m;
}

static Mat4 gg_boat_world_matrix(const GelgitBoat* b) {
    Mat4 ry = mat4_rotate_y(b->yaw);
    Mat4 rx = mat4_rotate_x(b->pitch);
    Mat4 rz = gg_mat4_rot_z(b->roll);
    Mat4 rot = mat4_mul(ry, mat4_mul(rx, rz));
    return mat4_mul(mat4_translate(b->pos), rot);
}

static Vec3 gg_mat4_xform_point(Mat4 m, Vec3 p) {
    Vec4 r = mat4_mul_vec4(m, vec4(p.x, p.y, p.z, 1.0f));
    return vec3(r.x, r.y, r.z);
}

/* ---------- Varsayılan Hayalet Tekne Kaydı (Günün Denizi için) ---------- */

static void gg_seed_default_ghost_replay(SimReplay* rep) {
    sim_replay_clear(rep);
    for (u32 f = 0; f < 3600u && f < rep->max_frames; f++) {
        SimInput in = sim_input_zero();
        /* Kapılara doğru dengeli dümen ve ıskota manevraları */
        if (f < 480u) {
            in.move_x = 4500;
            in.move_y = 6000;
        } else if (f < 1100u) {
            in.move_x = -8200;
            in.move_y = -4000;
            in.buttons = GELGIT_BTN_HIKE;
        } else if (f < 1850u) {
            in.move_x = -3500;
            in.move_y = 5000;
        } else {
            in.move_x = 5200;
            in.move_y = 2000;
            in.buttons = GELGIT_BTN_HIKE;
        }
        sim_replay_record(rep, f, &in, 1u);
    }
}

/* ---------- Okyanus Rüzgâr Senkronizasyonu ve Görev Başlatma ---------- */

static void gg_sync_ocean_to_state(const GelgitState* st, b32 force_rebuild) {
    f32 wspd = m_clamp(st->weather.cur_wind_speed, 2.0f, 24.0f);
    f32 wx   = st->weather.cur_wind_x;
    f32 wz   = st->weather.cur_wind_z;
    f32 chop = m_clamp(1.25f + st->weather.storm_intensity * 0.65f, 0.5f, 2.1f);

    f32 d_spd = m_abs(wspd - g_gg.last_ocean_wind_spd);
    f32 d_dir = m_abs(wx - g_gg.last_ocean_wind_x) + m_abs(wz - g_gg.last_ocean_wind_z);
    if (force_rebuild || d_spd > 0.65f || d_dir > 0.08f) {
        ocean_set_wind(&g_gg.ocean, wspd, wx, wz);
        g_gg.ocean.d.choppy = chop;
        g_gg.last_ocean_wind_spd = wspd;
        g_gg.last_ocean_wind_x   = wx;
        g_gg.last_ocean_wind_z   = wz;
        g_gg.gpu_spectrum_dirty  = TRUE;
        g_gg.gpu_first_update    = TRUE;
    } else {
        g_gg.ocean.d.choppy = chop;
    }
}

static void gg_start_mission_internal(u32 mission_id, u32 upgrades_mask, u32 completed_mask, f32 best_daily) {
    u32 seed = (mission_id == GELGIT_MODE_DAILY_SEA) ? 0x20261029u : (0x6E161700u + mission_id);
    sim_reset(&g_gg.sim);
    GelgitState* st = gg_state();
    gelgit_state_init(st, seed, mission_id, upgrades_mask, completed_mask);
    st->best_daily_time = best_daily;

    sim_replay_clear(&g_gg.cur_replay);
    g_gg.cur_replay.seed = seed;

    sail_cloth_reset(&g_gg.main_sail);
    sail_cloth_reset(&g_gg.jib_sail);

    gg_sync_ocean_to_state(st, TRUE);
    f64 sim_t = (f64)st->tick * (f64)GELGIT_FIXED_DT + 4.25;
    ocean_update(&g_gg.ocean, sim_t, 0.0f);

    /* Tekneyi başlangıç dalga yüksekliğine oturt */
    OceanSample os;
    ocean_sample(&g_gg.ocean, st->boat.pos.x, st->boat.pos.z, 0.0f, &os);
    st->boat.pos.y = os.disp.y - 0.14f;
    if (st->ghost_active) {
        st->ghost.pos.y = os.disp.y - 0.14f;
    }

    g_gg.prev_stage        = st->mission_stage;
    g_gg.prev_status       = st->mission_status;
    g_gg.prev_thunder_tick = st->weather.thunder_tick;

    if (g_gg.follow_cam) {
        g_gg.cam.yaw = st->boat.yaw;
    }
    g_gg.cam.target = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.55f, st->boat.pos.z);
    ocean_mesh_update(&g_gg.ocean, camera_eye(&g_gg.cam));
    g_gg.last_state_hash = gelgit_state_hash(st, sizeof(*st), NULL);
}

/* ---------- 3D Dünya Nesneleri (Adalar, Liman, Fenerler, Şamandıralar, Hayalet Tekne) ---------- */

static void gg_push_extra(GpuOceanExtraDraw* out, u32* io_count, u32 max_out,
                          Mat4 model, Vec4 color, b32 is_unlit, b32 use_hull_mesh) {
    if (*io_count >= max_out) return;
    GpuOceanExtraDraw* e = &out[(*io_count)++];
    e->model         = model;
    e->color         = color;
    e->is_unlit      = is_unlit;
    e->use_hull_mesh = use_hull_mesh;
}

static u32 gg_build_world_extras(const GelgitState* st, Mat4 boat_m,
                                 GpuOceanExtraDraw* out, u32 max_out) {
    u32 cnt = 0u;

    /* 1. 4 Takımada (bazalt kayalık taban + yeşil tepe sırtı) */
    for (u32 i = 0; i < GELGIT_MAX_ISLANDS; i++) {
        const GelgitIsland* isl = &st->islands[i];
        f32 r = isl->radius;
        f32 h = isl->height;
        f32 rot = (f32)i * 0.65f + 0.25f;

        Mat4 m_rock = mat4_trs(vec3(isl->x, h * 0.22f - 2.0f, isl->z), rot, 0.0f,
                               vec3(r * 1.42f, h * 0.78f, r * 1.32f));
        gg_push_extra(out, &cnt, max_out, m_rock,
                      vec4(0.30f, 0.28f, 0.26f, 1.0f), FALSE, FALSE);

        Mat4 m_top = mat4_trs(vec3(isl->x, h * 0.56f - 1.2f, isl->z), rot + 0.45f, 0.0f,
                              vec3(r * 0.92f, h * 0.48f, r * 0.88f));
        gg_push_extra(out, &cnt, max_out, m_top,
                      vec4(0.22f, 0.42f, 0.20f, 1.0f), FALSE, FALSE);
    }

    /* 2. Liman İskelesi (Ada 0 kıyısında: (-52, -68)) + Sıcak Liman Feneri */
    {
        Mat4 m_pier = mat4_trs(vec3(-56.5f, 0.65f, -73.0f), 0.38f, 0.0f,
                               vec3(4.2f, 0.45f, 16.0f));
        gg_push_extra(out, &cnt, max_out, m_pier,
                      vec4(0.62f, 0.44f, 0.28f, 1.0f), FALSE, FALSE);

        Mat4 m_post = mat4_trs(vec3(-51.5f, 1.85f, -67.2f), 0.0f, 0.0f,
                               vec3(0.35f, 2.6f, 0.35f));
        gg_push_extra(out, &cnt, max_out, m_post,
                      vec4(0.45f, 0.32f, 0.20f, 1.0f), FALSE, FALSE);

        Mat4 m_lamp = mat4_trs(vec3(-51.5f, 3.25f, -67.2f), 0.0f, 0.0f,
                               vec3(0.75f, 0.75f, 0.75f));
        gg_push_extra(out, &cnt, max_out, m_lamp,
                      vec4(1.0f, 0.82f, 0.36f, 1.0f), TRUE, FALSE);
    }

    /* 3. Güvertede veya iskelede kargo sandığı */
    if (st->boat.has_cargo) {
        Mat4 local_crate = mat4_mul(mat4_translate(vec3(0.0f, 0.72f, 0.90f)),
                                    mat4_scale(vec3(0.95f, 0.58f, 1.15f)));
        gg_push_extra(out, &cnt, max_out, mat4_mul(boat_m, local_crate),
                      vec4(0.82f, 0.56f, 0.24f, 1.0f), FALSE, FALSE);
    } else if (st->mission_id == 4u || st->mission_id == 5u) {
        Mat4 dock_crate = mat4_trs(vec3(-52.5f, 1.35f, -68.0f), 0.38f, 0.0f,
                                   vec3(1.25f, 0.85f, 1.45f));
        gg_push_extra(out, &cnt, max_out, dock_crate,
                      vec4(0.85f, 0.58f, 0.25f, 1.0f), FALSE, FALSE);
    }

    /* 4. 2 Deniz Feneri (beyaz taş kule + kırmızı fener odası + döner ışık huzmesi) */
    for (u32 i = 0; i < GELGIT_MAX_LIGHTHOUSES; i++) {
        const GelgitLighthouse* lh = &st->lighthouses[i];
        f32 by = lh->base_y;
        f32 th = lh->tower_h;

        Mat4 m_tower = mat4_trs(vec3(lh->x, by + th * 0.45f, lh->z), 0.0f, 0.0f,
                                vec3(3.4f, th, 3.4f));
        gg_push_extra(out, &cnt, max_out, m_tower,
                      vec4(0.94f, 0.92f, 0.88f, 1.0f), FALSE, FALSE);

        Mat4 m_top = mat4_trs(vec3(lh->x, by + th + 1.2f, lh->z), 0.0f, 0.0f,
                              vec3(3.8f, 2.5f, 3.8f));
        gg_push_extra(out, &cnt, max_out, m_top,
                      vec4(0.88f, 0.18f, 0.14f, 1.0f), FALSE, FALSE);

        /* Döner fener ışığı + huzmesi */
        f32 beam_y = by + th + 2.8f;
        f32 bx = lh->x + m_sin(lh->beam_phase) * 8.5f;
        f32 bz = lh->z + m_cos(lh->beam_phase) * 8.5f;
        Mat4 m_beam = mat4_trs(vec3(bx, beam_y, bz), lh->beam_phase, 0.0f,
                               vec3(1.65f, 1.45f, 18.0f));
        gg_push_extra(out, &cnt, max_out, m_beam,
                      vec4(1.0f, 0.94f, 0.58f, 1.0f), TRUE, FALSE);
    }

    /* 5. Görev Şamandıraları (dalga üzerinde yüzen şamandıra + ışıklı tepe sütunu) */
    for (u32 i = 0; i < st->waypoint_count && i < GELGIT_MAX_WAYPOINTS; i++) {
        const GelgitWaypoint* wp = &st->waypoints[i];
        OceanSample os;
        ocean_sample(&g_gg.ocean, wp->x, wp->z, 0.0f, &os);
        f32 wy = os.disp.y;
        b32 is_active = (i == st->mission_stage && st->mission_status == GELGIT_STATUS_PLAYING);
        b32 is_done   = (i < st->mission_stage || st->mission_status == GELGIT_STATUS_WON);

        Vec4 base_col = is_done   ? vec4(0.18f, 0.72f, 0.35f, 1.0f)
                      : is_active ? vec4(1.00f, 0.38f, 0.10f, 1.0f)
                                  : vec4(0.85f, 0.68f, 0.15f, 1.0f);
        Vec4 glow_col = is_done   ? vec4(0.25f, 0.95f, 0.48f, 1.0f)
                      : is_active ? vec4(1.00f, 0.88f, 0.22f, 1.0f)
                                  : vec4(0.35f, 0.80f, 0.95f, 1.0f);

        f32 float_scale = is_active ? 2.4f : 1.8f;
        Mat4 m_float = mat4_trs(vec3(wp->x, wy + 0.6f, wp->z), (f32)st->tick * 0.02f, 0.0f,
                                vec3(float_scale, 1.6f, float_scale));
        gg_push_extra(out, &cnt, max_out, m_float, base_col, FALSE, FALSE);

        f32 pillar_h = is_active ? 9.5f : 4.5f;
        Mat4 m_beacon = mat4_trs(vec3(wp->x, wy + 1.4f + pillar_h * 0.5f, wp->z), 0.0f, 0.0f,
                                 vec3(0.65f, pillar_h, 0.65f));
        gg_push_extra(out, &cnt, max_out, m_beacon, glow_col, TRUE, FALSE);
    }

    /* 6. "Günün Denizi" Hayalet Teknesi (gövde + yelken sütunu) */
    if (st->ghost_active) {
        Mat4 gm = gg_boat_world_matrix(&st->ghost);
        gg_push_extra(out, &cnt, max_out, gm,
                      vec4(0.28f, 0.88f, 1.0f, 1.0f), TRUE, TRUE);
        Mat4 g_sail = mat4_mul(gm, mat4_mul(mat4_translate(vec3(0.0f, 3.2f, -1.2f)),
                               mat4_mul(mat4_rotate_y(st->ghost.boom_angle),
                                        mat4_scale(vec3(0.18f, 4.4f, 2.6f)))));
        gg_push_extra(out, &cnt, max_out, g_sail,
                      vec4(0.52f, 0.95f, 1.0f, 1.0f), TRUE, FALSE);
    }

    return cnt;
}

/* ---------- Prosedürel Deniz & Fırtına Ses Sentezi ---------- */

static void gg_update_audio(const GelgitState* st, f32 dt) {
    Synth* syn = audio_synth();
    if (!syn || dt <= 0.0f) return;

    /* Görev aşaması / tamamlanma çanı */
    if (st->mission_stage != g_gg.prev_stage ||
        (st->mission_status == GELGIT_STATUS_WON && g_gg.prev_status != GELGIT_STATUS_WON)) {
        b32 won = (st->mission_status == GELGIT_STATUS_WON);
        SynthSfx bell;
        synth_sfx_buoy_bell(won, &bell);
        synth_play(syn, &bell);
        g_gg.prev_stage  = st->mission_stage;
        g_gg.prev_status = st->mission_status;
    }

    /* Şimşek sonrası gök gürültüsü */
    if (st->weather.thunder_tick != g_gg.prev_thunder_tick && st->weather.thunder_tick > 0u) {
        SynthSfx th;
        synth_sfx_thunder(0.65f + 0.35f * st->weather.storm_intensity, &th);
        synth_play(syn, &th);
        g_gg.prev_thunder_tick = st->weather.thunder_tick;
    }

    /* Sürekli rüzgâr uğultusu */
    g_gg.sfx_wind_cd -= dt;
    if (g_gg.sfx_wind_cd <= 0.0f) {
        SynthSfx ws;
        synth_sfx_sea_wind(st->boat.aw_speed, st->boat.aw_angle, &ws);
        synth_play(syn, &ws);
        g_gg.sfx_wind_cd = 0.42f;
    }

    /* Gövde su hışırtısı ve dalga sörfü köpürmesi */
    g_gg.sfx_water_cd -= dt;
    if (g_gg.sfx_water_cd <= 0.0f && st->boat.forward_speed > 0.8f) {
        SynthSfx hw;
        synth_sfx_hull_water(st->boat.forward_speed, st->boat.surf_intensity > 0.25f, &hw);
        synth_play(syn, &hw);
        g_gg.sfx_water_cd = 0.34f;
    }

    /* Ölü bölgede yelken yapraklama (luffing) çırpınması */
    g_gg.sfx_flap_cd -= dt;
    if (g_gg.sfx_flap_cd <= 0.0f && st->boat.luffing > 0.35f && !st->boat.capsized) {
        SynthSfx fl;
        synth_sfx_sail_flap(st->boat.luffing, &fl);
        synth_play(syn, &fl);
        g_gg.sfx_flap_cd = 0.22f;
    }

    /* Sert yatmada (heel > 24°) ahşap/halat gıcırtısı */
    g_gg.sfx_creak_cd -= dt;
    f32 heel_norm = m_clamp((m_abs(st->boat.roll) * M_RAD2DEG - 22.0f) / 30.0f, 0.0f, 1.0f);
    if (g_gg.sfx_creak_cd <= 0.0f && heel_norm > 0.15f) {
        SynthSfx cr;
        synth_sfx_rope_creak(heel_norm, &cr);
        synth_play(syn, &cr);
        g_gg.sfx_creak_cd = 0.48f;
    }
}

/* ---------- 2D Pruva Spreyi Parçacıkları ---------- */

static f32 gg_rand_f32(void) {
    u32 x = g_gg.spray_rng;
    if (x == 0u) x = 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_gg.spray_rng = x;
    return (f32)(x & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

static void gg_update_spray(const GelgitState* st, Mat4 boat_m, f32 dt) {
    if (dt <= 0.0f) return;

    f32 intensity = m_clamp(st->boat.bow_impact * 1.2f + st->boat.surf_intensity * 0.8f +
                            m_max(0.0f, (st->boat.forward_speed - 3.5f) * 0.15f), 0.0f, 1.5f);
    if (!st->boat.capsized && intensity > 0.08f) {
        u32 spawn_n = (u32)(intensity * 4.0f) + 1u;
        if (spawn_n > 6u) spawn_n = 6u;
        Vec3 bow_w = gg_mat4_xform_point(boat_m, vec3(0.0f, 0.35f, 3.65f));
        f32 sy = m_sin(st->boat.yaw), cy = m_cos(st->boat.yaw);

        for (u32 k = 0; k < spawn_n; k++) {
            SprayParticle* p = &g_gg.spray[g_gg.spray_head];
            g_gg.spray_head = (g_gg.spray_head + 1u) % GELGIT_MAX_SPRAY;
            f32 side = (gg_rand_f32() > 0.5f) ? 1.0f : -1.0f;
            f32 spread = (gg_rand_f32() * 1.4f + 0.6f) * side;
            f32 up_v = 2.2f + gg_rand_f32() * 3.2f * intensity;
            p->pos = vec3(bow_w.x + spread * cy * 0.5f,
                          bow_w.y + 0.15f,
                          bow_w.z - spread * sy * 0.5f);
            p->vel = vec3(st->boat.vel.x * 0.35f + cy * spread * 2.4f + st->boat.tw_x * 0.18f,
                          up_v,
                          st->boat.vel.z * 0.35f - sy * spread * 2.4f + st->boat.tw_z * 0.18f);
            p->max_life = 0.45f + gg_rand_f32() * 0.35f;
            p->life     = p->max_life;
            p->size     = 2.0f + gg_rand_f32() * 2.0f;
        }
    }

    for (u32 i = 0; i < GELGIT_MAX_SPRAY; i++) {
        SprayParticle* p = &g_gg.spray[i];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        p->vel.y -= 9.81f * dt;
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->pos.z += p->vel.z * dt;
    }
}

static void gg_draw_spray_and_rain(UiContext* ui, const Framebuffer* fb,
                                   const GelgitState* st, Mat4 view_proj) {
    f32 hw = (f32)fb->w * 0.5f;
    f32 hh = (f32)fb->h * 0.5f;

    /* 3D pruva spreyi parçacıklarını ekrana izdüşür */
    for (u32 i = 0; i < GELGIT_MAX_SPRAY; i++) {
        const SprayParticle* p = &g_gg.spray[i];
        if (p->life <= 0.0f) continue;
        Vec4 clip = mat4_mul_vec4(view_proj, vec4(p->pos.x, p->pos.y, p->pos.z, 1.0f));
        if (clip.w <= 0.3f) continue;
        f32 inv_w = 1.0f / clip.w;
        i32 sx = (i32)((clip.x * inv_w + 1.0f) * hw);
        i32 sy = (i32)((1.0f - clip.y * inv_w) * hh);
        if (sx < 2 || sx >= (i32)fb->w - 2 || sy < 20 || sy >= (i32)fb->h - 2) continue;

        f32 t = m_clamp(p->life / p->max_life, 0.0f, 1.0f);
        u32 alpha = (u32)(t * 215.0f);
        i32 sz = CLAMP((i32)(p->size * 14.0f * inv_w), 1, 5);
        ui_draw_rect(ui, sx - sz / 2, sy - sz / 2, sz, sz, FB_RGBA(235, 248, 255, alpha));
    }

    /* Fırtınada eğik yağmur çizgileri */
    if (st->weather.storm_intensity > 0.30f) {
        u32 drops = (u32)((st->weather.storm_intensity - 0.30f) * 90.0f);
        u32 alpha = (u32)m_clamp((st->weather.storm_intensity - 0.30f) * 165.0f, 20.0f, 130.0f);
        u32 col   = FB_RGBA(185, 210, 235, alpha);
        u32 t_seed = st->tick * 1664525u + 1013904223u;
        i32 slant_x = (i32)(st->weather.cur_wind_x * 6.0f);
        for (u32 i = 0; i < drops; i++) {
            t_seed = t_seed * 2654435761u + i * 9277u;
            i32 rx = (i32)(t_seed % fb->w);
            i32 ry = 22 + (i32)((t_seed >> 11) % (fb->h - 28u));
            ui_draw_line(ui, rx, ry, rx + slant_x, ry + 11, col);
        }
    }
}

/* ---------- CPU Yedek Yolu Çizim Yardımcıları ---------- */

static void gg_draw_boat_part(G3dContext* ctx, Arena* frame, const G3dMaterial* base_mat,
                              const Mesh* mesh, Mat4 boat_m, Vec3 pos, Vec3 scale, f32 rot_x,
                              const Texture* tex, Vec4 color, Vec3 light_dir, Vec3 fog_srgb, f32 fog_dens,
                              G3dCull cull) {
    Mat4 local = mat4_mul(mat4_translate(pos), mat4_mul(mat4_rotate_x(rot_x), mat4_scale(scale)));
    Mat4 model = mat4_mul(boat_m, local);
    GelgitLitUniforms* u = ARENA_PUSH_STRUCT(frame, GelgitLitUniforms);
    u->light_dir = vec4(light_dir.x, light_dir.y, light_dir.z, 0.0f);
    u->base      = color;
    u->fog       = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
    G3dMaterial m = *base_mat;
    m.uniforms = u;
    m.textures = tex;
    m.cull     = cull;
    g3d_draw(ctx, mesh, &model, &m);
}

static void gg_draw_boat_emissive(G3dContext* ctx, Arena* frame, const G3dMaterial* em_mat,
                                  const Mesh* mesh, Mat4 boat_m, Vec3 pos, Vec3 scale,
                                  Vec4 color, Vec3 fog_srgb, f32 fog_dens) {
    Mat4 local = mat4_mul(mat4_translate(pos), mat4_scale(scale));
    Mat4 model = mat4_mul(boat_m, local);
    GelgitUnlitUniforms* u = ARENA_PUSH_STRUCT(frame, GelgitUnlitUniforms);
    u->color = color;
    u->fog   = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
    G3dMaterial m = *em_mat;
    m.uniforms = u;
    g3d_draw(ctx, mesh, &model, &m);
}

static const EmbeddedFile* gg_find_embedded_mtk(const char* name) {
    for (u32 i = 0; i < g_embedded_shader_count; i++) {
        const char* a = g_embedded_shaders[i].name;
        const char* b = name;
        u32 k = 0;
        while (a[k] && b[k] && a[k] == b[k]) k++;
        if (a[k] == '\0' && b[k] == '\0') return &g_embedded_shaders[i];
    }
    return NULL;
}

/* ---------- Sahne İlklendirme & Bırakma ---------- */

void scene_gelgit_init(Arena* perm, Arena* scratch) {
    (void)scratch;
    __builtin_memset(&g_gg, 0, sizeof(g_gg));
    g_gg.show_hud    = TRUE;
    g_gg.follow_cam  = TRUE;
    g_gg.time_scale  = 1.0f;
    g_gg.spray_rng   = 0x6E161701u;

    OceanDesc desc = ocean_default_desc();
    ocean_init(&g_gg.ocean, perm, &desc);
    ocean_mesh_init(&g_gg.ocean, perm, 96u, 2.5f, 320.0f);

    g_gg.sky_mesh  = mesh_sphere(perm, 540.0f, 32u, 16u);
    g_gg.hull_mesh = gg_build_hull_mesh(perm);
    g_gg.unit_cube = mesh_cube(perm, 0.5f);

    /* XPBD ana yelken ve flok yelkeni */
    sail_cloth_init(&g_gg.main_sail, perm, -3.10f, -0.35f, -2.55f, -0.35f, 1.35f, 5.65f, 0.54f);
    sail_cloth_init(&g_gg.jib_sail,  perm,  0.10f,  4.55f,  0.00f,  0.20f, 1.15f, 4.95f, 0.44f);

    gg_build_wood_tex(&g_gg.tex_hull, perm, 158u, 78u, 42u, TRUE);
    gg_build_wood_tex(&g_gg.tex_deck, perm, 205u, 152u, 96u, TRUE);
    gg_build_sail_tex(&g_gg.tex_sail, perm);

    /* Deterministik 60 Hz Sim ve Replay tamponları */
    g_gg.sim_ctx.ocean                  = &g_gg.ocean;
    g_gg.sim_ctx.update_ocean_each_tick = FALSE;

    SimConfig cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = (u32)sizeof(GelgitState);
    cfg.player_count = 2u;   /* oyuncu 0: yerel tekne, oyuncu 1: Günün Denizi hayalet tekne */
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = GELGIT_FIXED_DT;
    cfg.step_fn      = gelgit_sim_step;
    cfg.hash_fn      = gelgit_state_hash;
    cfg.user         = &g_gg.sim_ctx;
    sim_init(&g_gg.sim, perm, &cfg);

    sim_replay_init(&g_gg.cur_replay, perm, 1u, GELGIT_REPLAY_MAX_FRAMES, 0x6E161700u, GELGIT_FIXED_DT);
    sim_replay_init(&g_gg.best_daily_replay, perm, 1u, GELGIT_REPLAY_MAX_FRAMES, 0x20261029u, GELGIT_FIXED_DT);
    gg_seed_default_ghost_replay(&g_gg.best_daily_replay);
    g_gg.has_best_replay = TRUE;

    camera_init(&g_gg.cam, CAM_ORBIT);
    g_gg.cam.yaw    = 0.0f;
    g_gg.cam.pitch  = -0.20f;
    g_gg.cam.dist   = 18.5f;
    g_gg.cam.near_z = 0.25f;

    /* Prosedürel deniz ambiyansı için hafif Schroeder yankısı */
    Synth* syn = audio_synth();
    if (syn) {
        synth_set_reverb(syn, 0.22f, 0.72f, 0.38f);
    }

    gg_start_mission_internal(0u, 0u, 0u, 0.0f);
}

void scene_gelgit_leave(void) {
    Synth* syn = audio_synth();
    if (syn) {
        synth_stop_all(syn);
    }
}

/* ---------- Kare Güncellemesi ve Çizim ---------- */

static void gg_fill_debug_buf(const GelgitState* st) {
    g_gg.debug_buf[0]  = g_gg.sim_ms;
    g_gg.debug_buf[1]  = g_gg.fft_ms;
    g_gg.debug_buf[2]  = g_gg.shade_ms;
    g_gg.debug_buf[3]  = g_gg.ocean.hs;
    g_gg.debug_buf[4]  = st->weather.cur_wind_speed;
    g_gg.debug_buf[5]  = st->boat.forward_speed;
    g_gg.debug_buf[6]  = st->boat.surf_intensity;
    g_gg.debug_buf[7]  = st->boat.roll * M_RAD2DEG;
    g_gg.debug_buf[8]  = st->boat.tw_angle_deg;
    g_gg.debug_buf[9]  = st->boat.vmg_upwind;
    g_gg.debug_buf[10] = (f32)st->mission_id;
    g_gg.debug_buf[11] = (f32)st->mission_stage;
    g_gg.debug_buf[12] = st->boat.capsized ? 1.0f : 0.0f;
    g_gg.debug_buf[13] = st->boat.cargo_health;
    g_gg.debug_buf[14] = (f32)g_gg.tris_drawn;
    g_gg.debug_buf[15] = (f32)(u32)(g_gg.last_state_hash & 0xFFFFu);
}

void scene_gelgit_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                        const InputState* in, f32 dt, b32 use_jit, b32 look_enabled) {
    GelgitState* st = gg_state();

    /* Sahne kısayolları */
    if (gg_key_pressed(in, KEY_R)) {
        gg_start_mission_internal(st->mission_id, st->upgrades_mask, st->completed_mask, st->best_daily_time);
        st = gg_state();
    }
    if (gg_key_pressed(in, KEY_N)) {
        u32 next_m = (st->mission_id + 1u) % GELGIT_TOTAL_MODES;
        gg_start_mission_internal(next_m, st->upgrades_mask, st->completed_mask, st->best_daily_time);
        st = gg_state();
    }
    if (gg_key_pressed(in, KEY_M)) {
        g_gg.show_menu = !g_gg.show_menu;
    }
    if (gg_key_pressed(in, KEY_F2) || gg_key_pressed(in, KEY_G)) {
        g_gg.show_tuning = !g_gg.show_tuning;
    }
    if (gg_key_pressed(in, KEY_H)) {
        g_gg.show_hud = !g_gg.show_hud;
    }
    if (gg_key_pressed(in, KEY_V)) {
        g_gg.wireframe = (g_gg.wireframe + 1u) % 3u;
    }
    if (gg_key_pressed(in, KEY_P)) {
        g_gg.paused = !g_gg.paused;
    }
    if (gg_key_pressed(in, KEY_C)) {
        g_gg.follow_cam = !g_gg.follow_cam;
    }
    if (gg_key_pressed(in, KEY_U)) {
        /* Hızlı yükseltme geçişi (klavye kısayolu) */
        st->upgrades_mask = (st->upgrades_mask + 1u) & 7u;
    }
    for (u32 m = 0; m < GELGIT_TOTAL_MODES; m++) {
        if (gg_key_pressed(in, KEY_1 + m)) {
            gg_start_mission_internal(m, st->upgrades_mask, st->completed_mask, st->best_daily_time);
            st = gg_state();
            break;
        }
    }

    __builtin_memcpy(g_gg.prev_keys, in->keys, sizeof(g_gg.prev_keys));

    f32 eff_dt = (g_gg.paused || dt <= 0.0f) ? 0.0f : (dt * g_gg.time_scale);

    /* (1) FFT Okyanus kademeleri ve rüzgâr senkronizasyonu */
    PROF_BEGIN("gelgit_fft");
    f64 t_fft0 = plat_time_ms();
    gg_sync_ocean_to_state(st, FALSE);
    f64 ocean_time = (f64)st->tick * (f64)GELGIT_FIXED_DT + 4.25;
    if (eff_dt > 0.0f || !g_gg.ocean.has_updated) {
        ocean_update(&g_gg.ocean, ocean_time, eff_dt);
    }
    f64 t_fft1 = plat_time_ms();
    g_gg.fft_ms = (f32)(t_fft1 - t_fft0);
    PROF_END();

    /* (2) 60 Hz Deterministik Sim Adımı (Sim + SimReplay) */
    PROF_BEGIN("gelgit_sim");
    f64 t_sim0 = plat_time_ms();
    if (eff_dt > 0.0f) {
        g_gg.sim.accum += m_min(eff_dt, 0.1f);
        SimInput player_in = gelgit_build_input(in, fb->w, fb->h);
        u32 steps = 0u;
        while (g_gg.sim.accum >= g_gg.sim.fixed_dt && steps < 8u) {
            g_gg.sim.accum -= g_gg.sim.fixed_dt;
            u32 f_idx = g_gg.sim.current_frame;
            sim_submit_input(&g_gg.sim, 0u, f_idx, player_in);

            /* Günün Denizi modunda hayalet teknenin kayıtlı girdisini gönder */
            SimInput ghost_in = sim_input_zero();
            if (st->ghost_active && g_gg.has_best_replay && f_idx < g_gg.best_daily_replay.frame_count) {
                const SimInput* rec = sim_replay_get(&g_gg.best_daily_replay, f_idx);
                if (rec) ghost_in = rec[0];
            }
            sim_submit_input(&g_gg.sim, 1u, f_idx, ghost_in);

            if (f_idx < g_gg.cur_replay.max_frames) {
                sim_replay_record(&g_gg.cur_replay, f_idx, &player_in, 1u);
            }

            sim_step(&g_gg.sim);
            st = gg_state();
            steps++;

            /* Günün Denizi tamamlandığında yeni rekor kırıldıysa hayalet kaydını güncelle */
            if (st->mission_id == GELGIT_MODE_DAILY_SEA &&
                st->mission_status == GELGIT_STATUS_WON &&
                g_gg.prev_status != GELGIT_STATUS_WON &&
                g_gg.cur_replay.frame_count > 10u) {
                u32 copy_n = g_gg.cur_replay.frame_count;
                if (copy_n > g_gg.best_daily_replay.max_frames) {
                    copy_n = g_gg.best_daily_replay.max_frames;
                }
                __builtin_memcpy(g_gg.best_daily_replay.inputs, g_gg.cur_replay.inputs,
                                 copy_n * sizeof(SimInput));
                g_gg.best_daily_replay.frame_count = copy_n;
                g_gg.has_best_replay = TRUE;
            }
        }
        g_gg.last_state_hash = gelgit_state_hash(st, sizeof(*st), NULL);
    }
    f64 t_sim1 = plat_time_ms();
    g_gg.sim_ms = (f32)(t_sim1 - t_sim0);
    PROF_END();

    /* (3) Görsel XPBD Yelken Kumaşı, Pruva Spreyi ve Prosedürel Sesler */
    Vec3 local_aw = vec3(st->boat.aw_x, 0.0f, st->boat.aw_z);
    sail_cloth_step(&g_gg.main_sail, local_aw, st->boat.boom_angle, st->boat.luffing, eff_dt);
    sail_cloth_step(&g_gg.jib_sail,  local_aw, st->boat.boom_angle * 0.82f, st->boat.luffing, eff_dt);

    Mat4 boat_m = gg_boat_world_matrix(&st->boat);
    gg_update_spray(st, boat_m, eff_dt);
    gg_update_audio(st, eff_dt);

    /* (4) Kamera Takibi */
    b32 ui_busy = g_gg.show_menu || g_gg.show_tuning;
    if (g_gg.cam.mode == CAM_ORBIT) {
        Vec3 desired = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.65f, st->boat.pos.z);
        if (dt > 0.0f) {
            f32 a = m_min(dt * 7.5f, 1.0f);
            g_gg.cam.target = vec3_add(g_gg.cam.target, vec3_scale(vec3_sub(desired, g_gg.cam.target), a));
            if (g_gg.follow_cam && !(look_enabled && !ui_busy && (in->mouse_buttons & 1u))) {
                /* Kamera pruva yönünün arkasından yumuşakça takip eder */
                f32 target_yaw = st->boat.yaw + M_PI;
                f32 dyaw = target_yaw - g_gg.cam.yaw;
                while (dyaw >  M_PI) dyaw -= M_TAU;
                while (dyaw < -M_PI) dyaw += M_TAU;
                g_gg.cam.yaw += dyaw * m_min(dt * 2.8f, 1.0f);
            }
        } else {
            g_gg.cam.target = desired;
        }
    }
    camera_update(&g_gg.cam, in, dt, look_enabled && !ui_busy);
    Vec3 eye = camera_eye(&g_gg.cam);

    b32 use_gpu = engine_gpu_available();
    if (!use_gpu || g_gg.wireframe > 0u) {
        f64 tm0 = plat_time_ms();
        ocean_mesh_update(&g_gg.ocean, eye);
        g_gg.mesh_ms = (f32)(plat_time_ms() - tm0);
    } else {
        g_gg.mesh_ms = 0.0f;
    }

    /* (5) Atmosfer, Güneş, Fırtına Cephesi ve Şimşek Aydınlatması */
    f32 storm = m_clamp(st->weather.storm_intensity, 0.0f, 1.0f);
    f32 flash = m_clamp(st->weather.lightning_flash, 0.0f, 1.0f);
    f32 ce = m_cos(st->weather.sun_elev), se = m_sin(st->weather.sun_elev);
    Vec3 sun_dir = vec3_norm(vec3(-ce * m_sin(st->weather.sun_azim), se, -ce * m_cos(st->weather.sun_azim)));
    f32 warm = m_clamp(1.0f - (st->weather.sun_elev - 0.08f) * 2.2f, 0.0f, 1.0f) * (1.0f - storm * 0.78f);

    Vec3 sun_golden = vec3(4.15f, 2.25f, 1.05f);
    Vec3 sun_storm  = vec3(0.75f, 0.85f, 1.05f);
    Vec3 sun_col    = vec3_add(sun_golden, vec3_scale(vec3_sub(sun_storm, sun_golden), storm));

    Vec3 zen_clear  = vec3(0.030f, 0.14f, 0.46f);
    Vec3 zen_storm  = vec3(0.018f, 0.026f, 0.045f);
    Vec3 sky_zenith = vec3_add(zen_clear, vec3_scale(vec3_sub(zen_storm, zen_clear), storm));

    Vec3 hor_clear  = vec3(0.68f, 0.45f, 0.32f);
    Vec3 hor_storm  = vec3(0.09f, 0.12f, 0.16f);
    Vec3 sky_horiz  = vec3_add(hor_clear, vec3_scale(vec3_sub(hor_storm, hor_clear), storm));

    if (flash > 0.02f) {
        Vec3 flash_col = vec3(1.6f * flash, 1.8f * flash, 2.4f * flash);
        sky_zenith = vec3_add(sky_zenith, vec3_scale(flash_col, 0.35f));
        sky_horiz  = vec3_add(sky_horiz,  vec3_scale(flash_col, 0.55f));
        sun_col    = vec3_add(sun_col,    vec3_scale(flash_col, 1.20f));
    }

    Vec3 sea_deep    = vec3(0.005f, 0.038f * (1.0f - 0.4f * storm), 0.105f * (1.0f - 0.35f * storm));
    Vec3 sea_shallow = vec3(0.022f, 0.38f * (1.0f - 0.45f * storm), 0.35f * (1.0f - 0.35f * storm));
    Vec3 fog_srgb    = vec3(m_sqrt(m_clamp(sky_horiz.x, 0.0f, 1.0f)),
                            m_sqrt(m_clamp(sky_horiz.y, 0.0f, 1.0f)),
                            m_sqrt(m_clamp(sky_horiz.z, 0.0f, 1.0f)));
    f32  fog_dens    = (1.0f / 360.0f) * (1.0f + storm * 1.45f);

    /* Dünya nesneleri (adalar, iskele, fenerler, şamandıralar, hayalet tekne) */
    GpuOceanExtraDraw extras[GPU_OCEAN_MAX_EXTRAS];
    u32 extra_count = gg_build_world_extras(st, boat_m, extras, GPU_OCEAN_MAX_EXTRAS);

    /* (6A) WebGPU 1080p MSAA 4x Çizim Yolu */
    if (use_gpu) {
        u32 vp_w = engine_gpu_viewport_width();
        u32 vp_h = engine_gpu_viewport_height();
        f32 aspect = (vp_h > 0u) ? ((f32)vp_w / (f32)vp_h) : ((f32)fb->w / (f32)fb->h);
        Mat4 view = camera_view(&g_gg.cam);
        Mat4 proj = camera_proj(&g_gg.cam, aspect);

        const EmbeddedFile* ef_ocean = gg_find_embedded_mtk("ocean.mtk");
        const EmbeddedFile* ef_sky   = gg_find_embedded_mtk("sky.mtk");
        const EmbeddedFile* ef_lit   = gg_find_embedded_mtk("lit3d.mtk");
        const EmbeddedFile* ef_unlit = gg_find_embedded_mtk("unlit3d.mtk");

        GpuOceanFrameParams gp;
        __builtin_memset(&gp, 0, sizeof(gp));
        gp.ocean          = &g_gg.ocean;
        gp.sim_time       = ocean_time;
        gp.eff_dt         = eff_dt;
        gp.spectrum_dirty = g_gg.gpu_spectrum_dirty;
        gp.first_update   = g_gg.gpu_first_update;
        gp.view           = view;
        gp.proj           = proj;
        gp.eye            = eye;
        gp.boat_model     = boat_m;
        gp.sun_dir        = sun_dir;
        gp.sun_col        = sun_col;
        gp.sky_zenith     = sky_zenith;
        gp.sky_horiz      = sky_horiz;
        gp.sea_deep       = sea_deep;
        gp.sea_shallow    = sea_shallow;
        gp.fog_srgb       = fog_srgb;
        gp.fog_dens       = fog_dens;
        gp.warm           = warm;
        gp.wireframe      = g_gg.wireframe;
        gp.sky_mesh       = g_gg.sky_mesh;
        gp.hull_mesh      = g_gg.hull_mesh;
        gp.main_sail_mesh = g_gg.main_sail.mesh;
        gp.jib_sail_mesh  = g_gg.jib_sail.mesh;
        gp.unit_cube      = g_gg.unit_cube;
        gp.tex_hull       = &g_gg.tex_hull;
        gp.tex_deck       = &g_gg.tex_deck;
        gp.tex_sail       = &g_gg.tex_sail;
        gp.dynamic_sails  = TRUE;
        gp.boom_angle     = g_gg.main_sail.boom_angle;
        gp.cloud_cover    = 0.38f + storm * 0.56f;
        gp.extra_count    = extra_count;
        for (u32 i = 0; i < extra_count; i++) {
            gp.extras[i] = extras[i];
        }
        if (ef_ocean) { gp.ocean_mtk_src = ef_ocean->src; gp.ocean_mtk_len = ef_ocean->len; }
        if (ef_sky)   { gp.sky_mtk_src   = ef_sky->src;   gp.sky_mtk_len   = ef_sky->len;   }
        if (ef_lit)   { gp.lit_mtk_src   = ef_lit->src;   gp.lit_mtk_len   = ef_lit->len;   }
        if (ef_unlit) { gp.unlit_mtk_src = ef_unlit->src; gp.unlit_mtk_len = ef_unlit->len; }

        if (gpu_ocean_render(frame, &gp, &g_gg.tris_drawn)) {
            g_gg.gpu_spectrum_dirty = FALSE;
            g_gg.gpu_first_update   = FALSE;
            g_gg.shade_ms = (f32)engine_gpu_frame_time_us() * 0.001f;
            fb_clear(fb, 0u);
            gg_fill_debug_buf(st);
            return;
        }
    }

    /* (6B) CPU 720p/360p Yedek Çizim Yolu */
    fb_clear(fb, FB_RGBA(12, 24, 42, 255));
    fb_clear_depth(fb, 0.0f);

    raster_begin(r, fb, frame);
    G3dContext ctx;
    f32 aspect = (f32)fb->w / (f32)fb->h;
    Mat4 view = camera_view(&g_gg.cam);
    Mat4 proj = camera_proj(&g_gg.cam, aspect);
    g3d_begin(&ctx, r, frame, view, proj, eye);
    g3d_set_deferred(&ctx, 128u);

    const ManagedShader* sh_lit   = shaders_find(sm, "lit3d.mtk");
    const ManagedShader* sh_unlit = shaders_find(sm, "unlit3d.mtk");
    b32 lit_fused = FALSE, unlit_fused = FALSE;
    G3dMaterial mat_lit;
    __builtin_memset(&mat_lit, 0, sizeof(mat_lit));
    mat_lit.shade_fn    = shaders_get_fn(sh_lit, use_jit, BLEND_NONE, &lit_fused);
    mat_lit.fused       = lit_fused;
    mat_lit.blend       = BLEND_NONE;
    mat_lit.attrs[0]    = G3D_ATTR_UV;
    mat_lit.attrs[1]    = G3D_ATTR_NORMAL;
    mat_lit.attrs[2]    = G3D_ATTR_VIEW_DEPTH;
    mat_lit.attr_count  = 3u;
    mat_lit.cull        = G3D_CULL_BACK;
    mat_lit.depth_test  = TRUE;
    mat_lit.depth_write = TRUE;

    G3dMaterial mat_em;
    __builtin_memset(&mat_em, 0, sizeof(mat_em));
    mat_em.shade_fn    = shaders_get_fn(sh_unlit, use_jit, BLEND_NONE, &unlit_fused);
    mat_em.fused       = unlit_fused;
    mat_em.blend       = BLEND_NONE;
    mat_em.attrs[0]    = G3D_ATTR_NORMAL;
    mat_em.attrs[1]    = G3D_ATTR_VIEW_DEPTH;
    mat_em.attr_count  = 2u;
    mat_em.cull        = G3D_CULL_BACK;
    mat_em.depth_test  = TRUE;
    mat_em.depth_write = TRUE;

    /* Tekne gövdesi ve XPBD yelkenleri */
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.hull_mesh, boat_m,
                      vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                      &g_gg.tex_hull, vec4(1.0f, 0.94f, 0.88f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.unit_cube, boat_m,
                      vec3(0.0f, 0.78f, -1.35f), vec3(1.38f, 0.62f, 1.85f), 0.0f,
                      &g_gg.tex_deck, vec4(0.95f, 0.90f, 0.82f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.unit_cube, boat_m,
                      vec3(0.0f, 1.13f, -1.35f), vec3(1.52f, 0.12f, 2.02f), 0.0f,
                      &g_gg.tex_hull, vec4(0.75f, 0.45f, 0.28f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    gg_draw_boat_emissive(&ctx, frame, &mat_em, g_gg.unit_cube, boat_m,
                         vec3(0.0f, 0.84f, -1.35f), vec3(1.42f, 0.22f, 0.85f),
                         vec4(1.0f, 0.78f, 0.35f, 1.0f), fog_srgb, fog_dens);
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.unit_cube, boat_m,
                      vec3(0.0f, 3.15f, -0.25f), vec3(0.14f, 5.60f, 0.14f), 0.0f,
                      &g_gg.tex_deck, vec4(0.88f, 0.74f, 0.55f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    {
        Mat4 boom_pivot = mat4_mul(boat_m, mat4_mul(mat4_translate(vec3(0.0f, 0.0f, -0.25f)),
                                   mat4_mul(mat4_rotate_y(g_gg.main_sail.boom_angle), mat4_translate(vec3(0.0f, 0.0f, 0.25f)))));
        gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.unit_cube, boom_pivot,
                          vec3(0.0f, 1.32f, -1.72f), vec3(0.10f, 0.10f, 2.95f), 0.0f,
                          &g_gg.tex_deck, vec4(0.85f, 0.70f, 0.52f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    }
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.unit_cube, boat_m,
                      vec3(0.0f, 1.02f, 4.15f), vec3(0.10f, 0.10f, 1.75f), -0.18f,
                      &g_gg.tex_deck, vec4(0.85f, 0.70f, 0.52f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    gg_draw_boat_emissive(&ctx, frame, &mat_em, g_gg.unit_cube, boat_m,
                         vec3(0.12f, 5.88f, -0.55f), vec3(0.04f, 0.16f, 0.65f),
                         vec4(0.92f, 0.22f, 0.16f, 1.0f), fog_srgb, fog_dens);
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.main_sail.mesh, boat_m,
                      vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                      &g_gg.tex_sail, vec4(1.0f, 0.97f, 0.92f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);
    gg_draw_boat_part(&ctx, frame, &mat_lit, g_gg.jib_sail.mesh, boat_m,
                      vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                      &g_gg.tex_sail, vec4(0.98f, 0.94f, 0.88f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);

    /* Ek 3D dünya nesneleri (adalar, iskele, deniz fenerleri, şamandıralar, hayalet tekne) */
    for (u32 i = 0; i < extra_count; i++) {
        const GpuOceanExtraDraw* ed = &extras[i];
        const Mesh* mesh = ed->use_hull_mesh ? g_gg.hull_mesh : g_gg.unit_cube;
        if (ed->is_unlit) {
            GelgitUnlitUniforms* u = ARENA_PUSH_STRUCT(frame, GelgitUnlitUniforms);
            u->color = ed->color;
            u->fog   = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
            G3dMaterial m = mat_em;
            m.uniforms = u;
            g3d_draw(&ctx, mesh, &ed->model, &m);
        } else {
            GelgitLitUniforms* u = ARENA_PUSH_STRUCT(frame, GelgitLitUniforms);
            u->light_dir = vec4(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
            u->base      = ed->color;
            u->fog       = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
            G3dMaterial m = mat_lit;
            m.uniforms = u;
            m.textures = &g_gg.tex_deck;
            g3d_draw(&ctx, mesh, &ed->model, &m);
        }
    }

    /* Okyanus yüzeyi */
    if (g_gg.wireframe != 2u) {
        const ManagedShader* sh_ocean = shaders_find(sm, "ocean.mtk");
        b32 ocean_fused = FALSE;
        ShadeFn fn_ocean = shaders_get_fn(sh_ocean, use_jit, BLEND_NONE, &ocean_fused);

        GelgitOceanUniforms* ou = ARENA_PUSH_STRUCT(frame, GelgitOceanUniforms);
        ou->sun_dir     = vec4(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
        ou->sun_col     = vec4(sun_col.x, sun_col.y, sun_col.z, 0.0f);
        ou->sky_zenith  = vec4(sky_zenith.x, sky_zenith.y, sky_zenith.z, 0.0f);
        ou->sky_horizon = vec4(sky_horiz.x, sky_horiz.y, sky_horiz.z, 0.0f);
        ou->sea_deep    = vec4(sea_deep.x, sea_deep.y, sea_deep.z, 0.0f);
        ou->sea_shallow = vec4(sea_shallow.x, sea_shallow.y, sea_shallow.z, 0.0f);
        ou->eye         = vec4(eye.x, eye.y, eye.z, 0.0f);
        f32 l0 = g_gg.ocean.cascades[0].size;
        f32 l1 = g_gg.ocean.cascades[1].size;
        f32 l2 = g_gg.ocean.cascades[2].size;
        ou->scales = vec4(l0 / l1, l0 / l2, OCEAN_TEX_SLOPE_MAX * 2.0f, fog_dens);
        ou->params = vec4(1.25f, 1.15f, 1.10f, 0.35f + 0.65f * warm);

        Texture* otex = ARENA_PUSH_ARRAY(frame, Texture, 3u);
        otex[0] = g_gg.ocean.cascades[0].tex;
        otex[1] = g_gg.ocean.cascades[1].tex;
        otex[2] = g_gg.ocean.cascades[2].tex;

        G3dMaterial mat_ocean;
        __builtin_memset(&mat_ocean, 0, sizeof(mat_ocean));
        mat_ocean.shade_fn    = fn_ocean;
        mat_ocean.fused       = ocean_fused;
        mat_ocean.uniforms    = ou;
        mat_ocean.textures    = otex;
        mat_ocean.blend       = BLEND_NONE;
        mat_ocean.attrs[0]    = G3D_ATTR_UV;
        mat_ocean.attrs[1]    = G3D_ATTR_NORMAL;
        mat_ocean.attrs[2]    = G3D_ATTR_VIEW_DEPTH;
        mat_ocean.attrs[3]    = G3D_ATTR_WORLD_POS;
        mat_ocean.attr_count  = 4u;
        mat_ocean.cull        = G3D_CULL_BACK;
        mat_ocean.depth_test  = TRUE;
        mat_ocean.depth_write = TRUE;

        Mat4 id = mat4_identity();
        g3d_draw(&ctx, g_gg.ocean.mesh, &id, &mat_ocean);
    }

    /* Gök kubbesi */
    {
        const ManagedShader* sh_sky = shaders_find(sm, "sky.mtk");
        b32 sky_fused = FALSE;
        ShadeFn fn_sky = shaders_get_fn(sh_sky, use_jit, BLEND_NONE, &sky_fused);

        GelgitSkyUniforms* su = ARENA_PUSH_STRUCT(frame, GelgitSkyUniforms);
        su->sun_dir     = vec4(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
        su->sun_col     = vec4(sun_col.x, sun_col.y, sun_col.z, 0.0f);
        su->sky_zenith  = vec4(sky_zenith.x, sky_zenith.y, sky_zenith.z, 0.0f);
        su->sky_horizon = vec4(sky_horiz.x, sky_horiz.y, sky_horiz.z, 0.0f);
        su->params      = vec4((f32)ocean_time, 0.38f + storm * 0.56f, 1.0f, 0.35f + 0.65f * warm);

        G3dMaterial mat_sky;
        __builtin_memset(&mat_sky, 0, sizeof(mat_sky));
        mat_sky.shade_fn    = fn_sky;
        mat_sky.fused       = sky_fused;
        mat_sky.uniforms    = su;
        mat_sky.textures    = NULL;
        mat_sky.blend       = BLEND_NONE;
        mat_sky.attrs[0]    = G3D_ATTR_NORMAL;
        mat_sky.attr_count  = 1u;
        mat_sky.cull        = G3D_CULL_FRONT;
        mat_sky.depth_test  = TRUE;
        mat_sky.depth_write = FALSE;

        Mat4 sky_m = mat4_translate(eye);
        g3d_draw(&ctx, g_gg.sky_mesh, &sky_m, &mat_sky);
    }

    g3d_flush(&ctx);
    f64 t_sh0 = plat_time_ms();
    raster_flush(r);
    g_gg.shade_ms   = (f32)(plat_time_ms() - t_sh0);
    g_gg.tris_drawn = (u32)ctx.stats.tris_out;

    gg_fill_debug_buf(st);
}

/* ---------- HUD, Rüzgâr Gülü, Görev Menüsü (M) ve Fizik Ayar Paneli (F2) ---------- */

static void gg_draw_wind_rose(UiContext* ui, const GelgitState* st, i32 cx, i32 cy, i32 rad) {
    /* Arka plan kutusu */
    ui_draw_rect(ui, cx - rad - 10, cy - rad - 18, (rad + 10) * 2, (rad + 18) * 2, FB_RGBA(10, 18, 30, 205));
    ui_draw_outline(ui, cx - rad - 10, cy - rad - 18, (rad + 10) * 2, (rad + 18) * 2, FB_RGBA(64, 98, 138, 255));
    ui_draw_text(ui, cx - rad - 4, cy - rad - 14, "RUZGAR GULU", FB_RGBA(175, 215, 248, 255), 1);

    /* Çember (16 segmanlı çokgen) */
    for (u32 i = 0; i < 16u; i++) {
        f32 a0 = ((f32)i / 16.0f) * M_TAU;
        f32 a1 = ((f32)(i + 1u) / 16.0f) * M_TAU;
        i32 x0 = cx + (i32)(m_sin(a0) * (f32)rad);
        i32 y0 = cy - (i32)(m_cos(a0) * (f32)rad);
        i32 x1 = cx + (i32)(m_sin(a1) * (f32)rad);
        i32 y1 = cy - (i32)(m_cos(a1) * (f32)rad);
        ui_draw_line(ui, x0, y0, x1, y1, FB_RGBA(80, 110, 145, 255));
    }

    /* Tekne gövde silüeti (yukarı = pruva) */
    ui_draw_line(ui, cx, cy - 12, cx - 6, cy + 8, FB_RGBA(225, 232, 245, 255));
    ui_draw_line(ui, cx, cy - 12, cx + 6, cy + 8, FB_RGBA(225, 232, 245, 255));
    ui_draw_line(ui, cx - 6, cy + 8, cx + 6, cy + 8, FB_RGBA(225, 232, 245, 255));

    /* Bumba / yelken açısı çizgisi (beyaz/krem) */
    f32 ba = st->boat.boom_angle;
    i32 bx = cx + (i32)(m_sin(ba) * (f32)(rad - 8));
    i32 by = cy + (i32)(m_cos(ba) * (f32)(rad - 8));
    u32 sail_col = (st->boat.luffing > 0.35f) ? FB_RGBA(255, 95, 75, 255) : FB_RGBA(255, 245, 210, 255);
    ui_draw_line(ui, cx, cy - 4, bx, by, sail_col);

    /* Görünür rüzgâr (AW) geliş yönü oku — altın sarısı */
    f32 aw_from = st->boat.aw_angle + M_PI;
    i32 aw_x = cx + (i32)(m_sin(aw_from) * (f32)(rad - 2));
    i32 aw_y = cy - (i32)(m_cos(aw_from) * (f32)(rad - 2));
    ui_draw_line(ui, cx, cy, aw_x, aw_y, FB_RGBA(255, 210, 65, 255));
    ui_draw_rect(ui, aw_x - 2, aw_y - 2, 5, 5, FB_RGBA(255, 210, 65, 255));

    /* Gerçek rüzgâr (TW) geliş yönü + ±38° ölü bölge sınırları — mavi / kırmızı */
    f32 tw_world_from = m_atan2(-st->boat.tw_x, -st->boat.tw_z);
    f32 tw_rel = tw_world_from - st->boat.yaw;
    i32 tw_x = cx + (i32)(m_sin(tw_rel) * (f32)rad);
    i32 tw_y = cy - (i32)(m_cos(tw_rel) * (f32)rad);
    ui_draw_line(ui, cx, cy, tw_x, tw_y, FB_RGBA(75, 195, 255, 255));
    ui_draw_rect(ui, tw_x - 2, tw_y - 2, 5, 5, FB_RGBA(75, 195, 255, 255));

    /* Ölü bölge (±38°) kırmızı çatal çizgileri */
    f32 dz_rad = 38.0f * M_DEG2RAD;
    for (i32 s = -1; s <= 1; s += 2) {
        f32 a = tw_rel + (f32)s * dz_rad;
        i32 dx = cx + (i32)(m_sin(a) * (f32)(rad - 4));
        i32 dy = cy - (i32)(m_cos(a) * (f32)(rad - 4));
        ui_draw_line(ui, cx, cy, dx, dy, FB_RGBA(235, 70, 70, 190));
    }

    char buf[64];
    fmt_format(buf, sizeof(buf), "TW:%.1f AW:%.1fm/s",
               (f64)st->boat.tw_speed, (f64)st->boat.aw_speed);
    ui_draw_text(ui, cx - rad - 6, cy + rad + 4, buf, FB_RGBA(215, 232, 248, 255), 1);
}

static void gg_draw_bar(UiContext* ui, i32 x, i32 y, i32 w, i32 h, f32 frac, u32 fill_col, const char* text) {
    frac = m_clamp(frac, 0.0f, 1.0f);
    ui_draw_rect(ui, x, y, w, h, FB_RGBA(18, 28, 42, 220));
    i32 fw = (i32)((f32)(w - 2) * frac);
    if (fw > 0) {
        ui_draw_rect(ui, x + 1, y + 1, fw, h - 2, fill_col);
    }
    ui_draw_outline(ui, x, y, w, h, FB_RGBA(72, 102, 138, 255));
    if (text) {
        ui_draw_text(ui, x + 4, y + 2, text, FB_RGBA(240, 246, 255, 255), 1);
    }
}

void scene_gelgit_draw_hud(UiContext* ui, const Framebuffer* fb) {
    const GelgitState* st_c = gg_state_const();
    GelgitState* st = gg_state();

    /* 2B Pruva spreyi ve yağmur çizgileri (3B sahnenin üstüne, HUD panellerinin altına) */
    f32 aspect = (f32)fb->w / (f32)fb->h;
    Mat4 vp = mat4_mul(camera_proj(&g_gg.cam, aspect), camera_view(&g_gg.cam));
    gg_draw_spray_and_rain(ui, fb, st_c, vp);

    if (!g_gg.show_hud) return;

    char line[160];

    /* 1. Sol Üst: Rüzgâr Gülü */
    gg_draw_wind_rose(ui, st_c, 52, 74, 34);

    /* 2. Üst Orta: Görev & Pusula Hedef Şeridi */
    {
        i32 bw = MIN((i32)fb->w - 130, 380);
        i32 bx = MAX(104, ((i32)fb->w - bw) / 2);
        i32 by = 23;
        ui_draw_rect(ui, bx, by, bw, 36, FB_RGBA(10, 18, 30, 210));
        ui_draw_outline(ui, bx, by, bw, 36, FB_RGBA(70, 108, 148, 255));

        const char* title = gelgit_mission_title(st_c->mission_id);
        const char* obj   = gelgit_mission_objective(st_c);

        f32 wp_dist = 0.0f;
        f32 wp_rel_deg = 0.0f;
        if (st_c->mission_stage < st_c->waypoint_count) {
            const GelgitWaypoint* wp = &st_c->waypoints[st_c->mission_stage];
            f32 dx = wp->x - st_c->boat.pos.x;
            f32 dz = wp->z - st_c->boat.pos.z;
            wp_dist = m_sqrt(dx * dx + dz * dz);
            f32 brg = m_atan2(dx, dz) - st_c->boat.yaw;
            while (brg >  M_PI) brg -= M_TAU;
            while (brg < -M_PI) brg += M_TAU;
            wp_rel_deg = brg * M_RAD2DEG;
        }

        if (st_c->waypoint_count > 0u && st_c->mission_status == GELGIT_STATUS_PLAYING) {
            fmt_format(line, sizeof(line), "%s | Hedef:%um (%+.0f deg) | %.1fs",
                       title, (u32)wp_dist, (f64)wp_rel_deg, (f64)st_c->mission_timer);
        } else {
            fmt_format(line, sizeof(line), "%s | %.1fs | Altin:%u",
                       title, (f64)st_c->mission_timer, st_c->gold);
        }
        ui_draw_text(ui, bx + 6, by + 5, line, FB_RGBA(255, 220, 110, 255), 1);
        ui_draw_text(ui, bx + 6, by + 19, obj, FB_RGBA(215, 234, 250, 255), 1);
    }

    /* 3. Sol Alt: Hız, Sörf, Yatma (Heel), Iskota ve Kargo Göstergeleri */
    {
        i32 px = 6;
        i32 py = (i32)fb->h - 102;
        i32 pw = 188;
        ui_draw_rect(ui, px, py, pw, 62, FB_RGBA(10, 18, 30, 205));
        ui_draw_outline(ui, px, py, pw, 62, FB_RGBA(64, 98, 138, 255));

        f32 kts = st_c->boat.forward_speed * 1.94384f;
        fmt_format(line, sizeof(line), "HIZ: %.1f kt (%.1f m/s)%s",
                   (f64)kts, (f64)st_c->boat.forward_speed,
                   (st_c->boat.surf_intensity > 0.20f) ? " SORF!" : "");
        u32 spd_col = (st_c->boat.surf_intensity > 0.20f)
                    ? FB_RGBA(45, 215, 145, 255)
                    : FB_RGBA(55, 145, 225, 255);
        gg_draw_bar(ui, px + 4, py + 4, pw - 8, 12, st_c->boat.forward_speed / 11.0f, spd_col, line);

        f32 heel_deg = m_abs(st_c->boat.roll) * M_RAD2DEG;
        fmt_format(line, sizeof(line), "YATMA: %.0f/%.0f deg %s",
                   (f64)heel_deg, (f64)st_c->tuning.capsize_deg,
                   (st_c->boat.hike_amount > 0.2f) ? "[TRAPEZ]" : "");
        u32 heel_col = (heel_deg > 42.0f) ? FB_RGBA(235, 65, 55, 255)
                     : (heel_deg > 26.0f) ? FB_RGBA(235, 165, 45, 255)
                                          : FB_RGBA(70, 175, 120, 255);
        gg_draw_bar(ui, px + 4, py + 19, pw - 8, 12, heel_deg / st_c->tuning.capsize_deg, heel_col, line);

        f32 sheet_deg = st_c->boat.sheet_angle * M_RAD2DEG;
        fmt_format(line, sizeof(line), "ISKOTA: %.0f deg | AOA:%.0f CL:%.2f",
                   (f64)sheet_deg, (f64)st_c->boat.aoa_deg, (f64)st_c->boat.cl);
        gg_draw_bar(ui, px + 4, py + 34, pw - 8, 12, (sheet_deg - 12.0f) / 68.0f,
                    FB_RGBA(115, 135, 195, 255), line);

        if (st_c->boat.has_cargo || st_c->mission_id == 4u || st_c->mission_id == 5u) {
            fmt_format(line, sizeof(line), "KARGO SAGLAM: %.0f%% %s",
                       (f64)st_c->boat.cargo_health,
                       st_c->boat.has_cargo ? "(Guvertede)" : "(Iskelede)");
            gg_draw_bar(ui, px + 4, py + 48, pw - 8, 11, st_c->boat.cargo_health * 0.01f,
                        FB_RGBA(215, 145, 45, 255), line);
        } else {
            fmt_format(line, sizeof(line), "Kontra:%s Tramola:%u Hs:%.1fm",
                       (st_c->boat.tack_side > 0) ? "Sancak" : (st_c->boat.tack_side < 0) ? "Iskele" : "Notr",
                       st_c->boat.tack_count, (f64)g_gg.ocean.hs);
            ui_draw_text(ui, px + 6, py + 50, line, FB_RGBA(190, 215, 238, 255), 1);
        }
    }

    /* 4. Alabora Kurtarma veya Görev Sonucu Uyarısı */
    if (st_c->boat.capsized) {
        i32 aw = 310, ah = 40;
        i32 ax = ((i32)fb->w - aw) / 2;
        i32 ay = (i32)fb->h / 2 - 28;
        ui_draw_rect(ui, ax, ay, aw, ah, FB_RGBA(45, 12, 16, 235));
        ui_draw_outline(ui, ax, ay, aw, ah, FB_RGBA(255, 85, 75, 255));
        fmt_format(line, sizeof(line), "ALABORA! [Shift]/[Space] Basili Tutun: %.1f / 3.0s",
                   (f64)st_c->boat.capsize_timer);
        gg_draw_bar(ui, ax + 8, ay + 12, aw - 16, 16, st_c->boat.capsize_timer / 3.0f,
                    FB_RGBA(240, 85, 55, 255), line);
    } else if (st_c->mission_status == GELGIT_STATUS_WON) {
        i32 aw = 340, ah = 32;
        i32 ax = ((i32)fb->w - aw) / 2;
        i32 ay = 64;
        ui_draw_rect(ui, ax, ay, aw, ah, FB_RGBA(14, 48, 32, 235));
        ui_draw_outline(ui, ax, ay, aw, ah, FB_RGBA(80, 235, 145, 255));
        fmt_format(line, sizeof(line), "GOREV TAMAMLANDI! (%.1fs) | [N] Sonraki | [R] Tekrar | [M] Menu",
                   (f64)st_c->mission_timer);
        ui_draw_text(ui, ax + 8, ay + 11, line, FB_RGBA(175, 255, 205, 255), 1);
    }

    /* 5. [M] Görev & Tersane Yükseltme Menüsü */
    if (g_gg.show_menu) {
        i32 mw = 290, mh = 248;
        i32 mx = 16, my = 62;
        ui_panel_begin(ui, "GELGIT GOREVLER & TERSANE (M)", mx, my, mw, mh);
        for (u32 m = 0; m < GELGIT_TOTAL_MODES; m++) {
            char mbtn[96];
            b32 done = (st->completed_mask & (1u << m)) != 0u;
            fmt_format(mbtn, sizeof(mbtn), "%s%s %s",
                       (st->mission_id == m) ? "> " : "  ",
                       gelgit_mission_title(m),
                       done ? "[OK]" : "");
            if (ui_button(ui, mbtn)) {
                gg_start_mission_internal(m, st->upgrades_mask, st->completed_mask, st->best_daily_time);
                st = gg_state();
            }
        }
        ui_separator(ui);
        b32 u_sail = (st->upgrades_mask & GELGIT_UPG_SAIL_CUT) != 0u;
        b32 u_keel = (st->upgrades_mask & GELGIT_UPG_DEEP_KEEL) != 0u;
        b32 u_hull = (st->upgrades_mask & GELGIT_UPG_COPPER_HULL) != 0u;
        if (ui_checkbox(ui, "Yukseltme 1: Yelken Kesimi (+15% CL)", &u_sail)) {
            if (u_sail) st->upgrades_mask |= GELGIT_UPG_SAIL_CUT;
            else        st->upgrades_mask &= ~GELGIT_UPG_SAIL_CUT;
        }
        if (ui_checkbox(ui, "Yukseltme 2: Derin Salma (+22% GM)", &u_keel)) {
            if (u_keel) st->upgrades_mask |= GELGIT_UPG_DEEP_KEEL;
            else        st->upgrades_mask &= ~GELGIT_UPG_DEEP_KEEL;
        }
        if (ui_checkbox(ui, "Yukseltme 3: Bakir Govde (-15% Drag)", &u_hull)) {
            if (u_hull) st->upgrades_mask |= GELGIT_UPG_COPPER_HULL;
            else        st->upgrades_mask &= ~GELGIT_UPG_COPPER_HULL;
        }
        ui_panel_end(ui);
    }

    /* 6. [F2] Canlı Fizik Ayar Kaydırıcıları (ui_slider_f32) */
    if (g_gg.show_tuning) {
        i32 tw = 276, th = 192;
        i32 tx = (i32)fb->w - tw - 8;
        i32 ty = 62;
        ui_panel_begin(ui, "CANLI YELKEN FIZIGI AYARLARI (F2)", tx, ty, tw, th);
        ui_slider_f32(ui, "Yelken Alani (m2)", &st->tuning.sail_area,   10.0f, 38.0f);
        ui_slider_f32(ui, "Tekne Kutlesi(kg)", &st->tuning.boat_mass,  380.0f, 1100.0f);
        ui_slider_f32(ui, "Salma GM (m)",      &st->tuning.keel_gm,      0.65f, 2.20f);
        ui_slider_f32(ui, "Sorf Carpani",      &st->tuning.surf_gain,    0.50f, 3.00f);
        ui_slider_f32(ui, "Alabora Esigi(d)",  &st->tuning.capsize_deg, 42.0f, 75.0f);
        ui_slider_f32(ui, "Dumen Kazanci",     &st->tuning.rudder_gain,  0.50f, 1.80f);
        if (ui_button(ui, "Varsayilan Fizik Ayarlarina Sifirla")) {
            st->tuning = gelgit_default_tuning();
        }
        ui_panel_end(ui);
    }

    /* 7. Alt Çubuk: Performans & Kontrol İpuçları */
    i32 by = (i32)fb->h - 36;
    ui_draw_rect(ui, 0, by, (i32)fb->w, 36, FB_RGBA(10, 16, 28, 205));
    if (engine_gpu_available()) {
        u32 vp_w = engine_gpu_viewport_width();
        u32 vp_h = engine_gpu_viewport_height();
        f64 gpu_ms = (f64)engine_gpu_frame_time_us() * 0.001;
        fmt_format(line, sizeof(line),
                   "GELGIT GPU %ux%u | GPU:%.2fms Sim:%.2fms | Hs:%.2fm Ruzgar:%.1fm/s | Ucgen:%u",
                   vp_w, vp_h, gpu_ms, (f64)g_gg.sim_ms,
                   (f64)g_gg.ocean.hs, (f64)st_c->weather.cur_wind_speed, g_gg.tris_drawn);
    } else {
        fmt_format(line, sizeof(line),
                   "GELGIT CPU %ux%u | Sim:%.2fms FFT:%.2fms Shade:%.2fms | Hs:%.2fm Ruzgar:%.1fm/s",
                   fb->w, fb->h, (f64)g_gg.sim_ms, (f64)g_gg.fft_ms, (f64)g_gg.shade_ms,
                   (f64)g_gg.ocean.hs, (f64)st_c->weather.cur_wind_speed);
    }
    ui_draw_text(ui, 6, by + 4, line, FB_RGBA(110, 230, 255, 255), 1);
    ui_draw_text(ui, 6, by + 18,
                 "[A/D]Dumen [W/S]Iskota [Shift]Trapez/Kurtar [Space]Kargo [1-8]Gorev [M]Tersane [F2]Fizik [R]Sifirla",
                 FB_RGBA(220, 232, 248, 255), 1);
}

void scene_gelgit_set_hud(b32 show) {
    g_gg.show_hud = show ? TRUE : FALSE;
}

const f32* scene_gelgit_debug(void) {
    return g_gg.debug_buf;
}

/* ---------- Test ve Ekran Görüntüsü Komutları (cmd = 10..14) ---------- */

void scene_gelgit_debug_cmd(u32 cmd, f32 a, f32 b, f32 c) {
    GelgitState* st = gg_state();
    if (cmd == 0u) {
        gg_start_mission_internal(st->mission_id, st->upgrades_mask, st->completed_mask, st->best_daily_time);
    } else if (cmd == 1u) {
        u32 mid = ((u32)a) % GELGIT_TOTAL_MODES;
        u32 upg = (u32)b;
        gg_start_mission_internal(mid, upg, st->completed_mask, st->best_daily_time);
    } else if (cmd == 2u) {
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = a;
        g_gg.cam.pitch  = b;
        g_gg.cam.dist   = c;
    } else if (cmd == 3u) {
        g_gg.time_scale = m_clamp(a, 0.0f, 3.0f);
        g_gg.paused     = (b > 0.5f) ? TRUE : FALSE;
        g_gg.wireframe  = ((u32)c) % 3u;
    } else if (cmd == 10u) {
        /* 10: gelgit_sunset.png — Altın gün batımında yelkenli, şişkin XPBD yelkenler ve takımadalar */
        gg_start_mission_internal(1u, GELGIT_UPG_SAIL_CUT, 1u, 0.0f);
        st = gg_state();
        SimInput in = sim_input_zero();
        in.move_y = 6000;
        for (u32 i = 0; i < 180u; i++) {
            sim_submit_input(&g_gg.sim, 0u, g_gg.sim.current_frame, in);
            sim_submit_input(&g_gg.sim, 1u, g_gg.sim.current_frame, sim_input_zero());
            sim_step(&g_gg.sim);
        }
        st = gg_state();
        st->weather.sun_elev = 0.13f;
        st->weather.sun_azim = 0.42f;
        st->weather.storm_intensity = 0.0f;
        sail_cloth_step(&g_gg.main_sail, vec3(st->boat.aw_x, 0.0f, st->boat.aw_z), st->boat.boom_angle, 0.0f, 0.05f);
        sail_cloth_step(&g_gg.jib_sail,  vec3(st->boat.aw_x, 0.0f, st->boat.aw_z), st->boat.boom_angle * 0.82f, 0.0f, 0.05f);
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = 0.54f;
        g_gg.cam.pitch  = -0.16f;
        g_gg.cam.dist   = 17.5f;
        g_gg.cam.target = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.55f, st->boat.pos.z);
        g_gg.paused     = TRUE;
    } else if (cmd == 11u) {
        /* 11: gelgit_storm.png — Fırtınada (Hs > 6m) dev dalgalar, köpük, yağmur ve şimşek */
        gg_start_mission_internal(5u, GELGIT_UPG_SAIL_CUT | GELGIT_UPG_DEEP_KEEL, 0x1Fu, 0.0f);
        st = gg_state();
        st->weather.storm_intensity = 0.94f;
        st->weather.cur_wind_speed  = 21.5f;
        st->weather.lightning_flash = 0.32f;
        gg_sync_ocean_to_state(st, TRUE);
        ocean_update(&g_gg.ocean, 18.5, 0.03f);
        SimInput in = sim_input_zero();
        in.buttons = GELGIT_BTN_HIKE;
        for (u32 i = 0; i < 90u; i++) {
            sim_submit_input(&g_gg.sim, 0u, g_gg.sim.current_frame, in);
            sim_submit_input(&g_gg.sim, 1u, g_gg.sim.current_frame, sim_input_zero());
            sim_step(&g_gg.sim);
        }
        st = gg_state();
        st->weather.lightning_flash = 0.28f;
        st->boat.roll = 0.54f;
        st->boat.bow_impact = 0.85f;
        gg_update_spray(st, gg_boat_world_matrix(&st->boat), 0.03f);
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = -0.68f;
        g_gg.cam.pitch  = -0.19f;
        g_gg.cam.dist   = 19.0f;
        g_gg.cam.target = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.65f, st->boat.pos.z);
        g_gg.paused     = TRUE;
    } else if (cmd == 12u) {
        /* 12: gelgit_surf.png — Dalga sırtında sörf (SURFI! aktif, pruva spreyi) */
        gg_start_mission_internal(3u, 7u, 7u, 0.0f);
        st = gg_state();
        SimInput in = sim_input_zero();
        for (u32 i = 0; i < 210u; i++) {
            sim_submit_input(&g_gg.sim, 0u, g_gg.sim.current_frame, in);
            sim_submit_input(&g_gg.sim, 1u, g_gg.sim.current_frame, sim_input_zero());
            sim_step(&g_gg.sim);
        }
        st = gg_state();
        st->boat.forward_speed  = 9.39f;
        st->boat.surf_intensity = 0.92f;
        st->boat.pitch          = -0.14f;
        st->boat.bow_impact     = 0.90f;
        gg_update_spray(st, gg_boat_world_matrix(&st->boat), 0.03f);
        gg_update_spray(st, gg_boat_world_matrix(&st->boat), 0.03f);
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = 2.45f;
        g_gg.cam.pitch  = -0.15f;
        g_gg.cam.dist   = 16.0f;
        g_gg.cam.target = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.45f, st->boat.pos.z);
        g_gg.paused     = TRUE;
    } else if (cmd == 13u) {
        /* 13: gelgit_capsize.png — Alabora durumu ve kurtarma çubuğu */
        gg_start_mission_internal(2u, 0u, 3u, 0.0f);
        st = gg_state();
        st->boat.capsized      = TRUE;
        st->boat.roll          = 1.68f;   /* ~96 derece yan yatmış */
        st->boat.capsize_timer = 1.45f;
        sail_cloth_step(&g_gg.main_sail, vec3(st->boat.aw_x, 0.0f, st->boat.aw_z), st->boat.boom_angle, 0.8f, 0.05f);
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = 0.85f;
        g_gg.cam.pitch  = -0.22f;
        g_gg.cam.dist   = 16.5f;
        g_gg.cam.target = vec3(st->boat.pos.x, st->boat.pos.y * 0.35f + 1.25f, st->boat.pos.z);
        g_gg.paused     = TRUE;
    } else if (cmd == 14u) {
        /* 14: gelgit_harbor.png — Liman iskelesi, takımada, deniz feneri ve kargo sandığı */
        gg_start_mission_internal(4u, GELGIT_UPG_SAIL_CUT | GELGIT_UPG_COPPER_HULL, 0x0Fu, 0.0f);
        st = gg_state();
        st->boat.pos.x = -46.0f;
        st->boat.pos.z = -62.0f;
        st->boat.yaw   = 0.48f;
        OceanSample os;
        ocean_sample(&g_gg.ocean, st->boat.pos.x, st->boat.pos.z, 0.0f, &os);
        st->boat.pos.y = os.disp.y - 0.14f;
        sail_cloth_step(&g_gg.main_sail, vec3(5.0f, 0.0f, 1.0f), 0.55f, 0.0f, 0.05f);
        sail_cloth_step(&g_gg.jib_sail,  vec3(5.0f, 0.0f, 1.0f), 0.45f, 0.0f, 0.05f);
        g_gg.follow_cam = FALSE;
        g_gg.cam.yaw    = 0.35f;
        g_gg.cam.pitch  = -0.18f;
        g_gg.cam.dist   = 22.0f;
        g_gg.cam.target = vec3(-50.0f, 1.85f, -66.0f);
        g_gg.paused     = TRUE;
    }
}
