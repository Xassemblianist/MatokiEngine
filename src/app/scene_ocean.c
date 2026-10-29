/* app/scene_ocean.c — "Okyanus" (M17_A) sahnesi implementasyonu.
 * 3 kademeli Tessendorf FFT okyanus (L = 500/60/8 m, N = 128/128/64, JONSWAP γ=3.3),
 * Jacobian köpük + mipmap dokuları, ocean.mtk + sky.mtk gölgelendiricileri,
 * 8 noktalı kaldırma kuvvetiyle dalgalarda yalpalayan ahşap yelkenli tekne ve HUD.
 */
#include "app/scene_ocean.h"
#include "gfx3d/ocean.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/camera.h"
#include "gfx3d/mesh.h"
#include "raster/texture.h"
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
#define KEY_C     67
#define KEY_F     70
#define KEY_G     71
#define KEY_H     72
#define KEY_I     73
#define KEY_K     75
#define KEY_L     76
#define KEY_M     77
#define KEY_O     79
#define KEY_R     82
#define KEY_U     85
#define KEY_V     86

/* ---------- Shader Uniform Blokları (std140 / 16B hizalı) ---------- */

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 sea_deep;
    _Alignas(16) Vec4 sea_shallow;
    _Alignas(16) Vec4 eye;
    _Alignas(16) Vec4 scales;      /* x=L0/L1, y=L0/L2, z=slope_scale, w=fog_density */
    _Alignas(16) Vec4 params;      /* x=sss, y=foam, z=glare, w=warmth */
} OceanUniforms;

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 params;      /* x=time, y=cloud, z=sun_intensity, w=warmth */
} SkyUniforms;

typedef struct {
    _Alignas(16) Vec4 light_dir;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
} Lit3dUniforms;

typedef struct {
    _Alignas(16) Vec4 color;
    _Alignas(16) Vec4 fog;
} Unlit3dUniforms;

/* ---------- Yüzen Tekne Fiziği (8 noktalı kaldırma kuvveti + yay-sönüm) ---------- */

typedef struct {
    Vec3 pos;          /* dünya konumu (m) */
    Vec3 vel;          /* doğrusal hız (m/s) */
    f32  pitch;        /* baş-kıç açısı (rad, X ekseni etrafında) */
    f32  roll;         /* yalpa açısı (rad, Z ekseni etrafında) */
    f32  yaw;          /* pruva yönü (rad, Y ekseni etrafında) */
    f32  vel_pitch;    /* açısal hız X (rad/s) */
    f32  vel_roll;     /* açısal hız Z (rad/s) */
    f32  vel_yaw;      /* açısal hız Y (rad/s) */
} BoatState;

/* ---------- Sahne Durumu ---------- */

typedef struct {
    Ocean     ocean;
    Camera    cam;
    BoatState boat;
    Mesh*     sky_mesh;
    Mesh*     hull_mesh;
    Mesh*     main_sail_mesh;
    Mesh*     jib_sail_mesh;
    Mesh*     unit_cube;
    Texture   tex_hull;
    Texture   tex_deck;
    Texture   tex_sail;

    f64       sim_time;
    f32       time_scale;
    b32       paused;
    f32       sun_elev;      /* radyan: 0.08 (altın gün batımı) .. 0.85 (öğle) */
    f32       sun_azim;      /* radyan */
    u32       wireframe;     /* 0: kapalı, 1: yüzey+tel kafes, 2: yalnız tel kafes */
    b32       show_hud;

    /* Ölçümler */
    f32       fft_ms;
    f32       mesh_ms;
    f32       shade_ms;
    u32       tris_drawn;
    f32       debug_buf[12];

    /* Önceki kare tuş durumları */
    u8        prev_keys[256];
} OceanScene;

static OceanScene g_sc;

static b32 sc_key_pressed(const InputState* in, u32 key) {
    if (key >= 256u) return FALSE;
    return in->keys[key] != 0u && g_sc.prev_keys[key] == 0u;
}

/* ---------- Prosedürel Dokular (Ahşap & Yelken Bezi) ---------- */

static void build_wood_tex(Texture* out, Arena* perm, u32 r_base, u32 g_base, u32 b_base, b32 plank_lines) {
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

static void build_sail_tex(Texture* out, Arena* perm) {
    u32 w = 64u, h = 64u;
    tex_init_mipmapped(out, perm, w, h, TEX_FLAG_BILINEAR);
    u32* px = out->px;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 hsh = (x * 3313u + y * 6971u + 79u) * 2654435761u;
            i32 weave = (i32)((hsh >> 25) & 7u) - 3;
            if ((y & 15u) == 0u) weave -= 14;   /* dikiş şeritleri */
            i32 r = CLAMP(244 + weave, 180, 255);
            i32 g = CLAMP(236 + weave, 175, 255);
            i32 b = CLAMP(218 + weave, 160, 255);
            px[y * w + x] = FB_RGBA((u32)r, (u32)g, (u32)b, 255u);
        }
    }
    tex_build_mips(out);
}

/* ---------- Prosedürel Tekne Gövdesi ve Şişkin Yelken Ağları ---------- */

/* V-tabanlı klasik ahşap tekne gövdesi + güverte:
 * 9 boyuna istasyon (kıç z=-3.6 .. pruva z=+4.2), her istasyonda 7 enine kesit köşesi:
 * 0: iskele küpeşte, 1: iskele üst çene, 2: iskele alt çene, 3: omurga,
 * 4: sancak alt çene, 5: sancak üst çene, 6: sancak küpeşte */
static Mesh* build_hull_mesh(Arena* a) {
    static const f32 z_st[9]  = { -3.6f, -3.0f, -2.0f, -0.8f,  0.4f,  1.6f,  2.7f,  3.6f,  4.25f };
    static const f32 bw_st[9] = {  0.92f, 1.22f, 1.45f, 1.52f, 1.44f, 1.20f, 0.78f, 0.32f, 0.02f };
    static const f32 kl_st[9] = { -0.42f,-0.62f,-0.74f,-0.78f,-0.75f,-0.68f,-0.55f,-0.32f, 0.15f };
    static const f32 sh_st[9] = {  0.72f, 0.64f, 0.56f, 0.52f, 0.54f, 0.60f, 0.70f, 0.82f, 0.92f };

    const u32 st_n = 9u;
    const u32 ring_n = 7u;
    /* Gövde dış kabuğu (9*7 = 63 köşe) + güverte (9*2 = 18 köşe) + kıç aynalık (7 köşe) = 88 köşe */
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

    /* Güverte yüzeyi */
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

    /* Kıç aynalık (transom, s = 0) */
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

/* Rüzgârla şişkin kavisli yelken ağı (8×8 ızgara) */
static Mesh* build_sail_mesh(Arena* a, f32 z_bot0, f32 z_bot1, f32 z_top0, f32 z_top1,
                             f32 y_bot, f32 y_top, f32 belly_x) {
    const u32 div = 8u;
    const u32 n   = div + 1u;
    Mesh* m = mesh_alloc(a, n * n, div * div * 6u, FALSE);
    for (u32 j = 0; j < n; j++) {
        f32 vt = (f32)j / (f32)div;
        f32 y  = y_bot + (y_top - y_bot) * vt;
        f32 z0 = z_bot0 + (z_top0 - z_bot0) * vt;
        f32 z1 = z_bot1 + (z_top1 - z_bot1) * vt;
        f32 vy_arch = 4.0f * vt * (1.0f - vt);
        for (u32 i = 0; i < n; i++) {
            f32 ut = (f32)i / (f32)div;
            f32 z  = z0 + (z1 - z0) * ut;
            f32 ux_arch = 4.0f * ut * (1.0f - ut);
            f32 x  = belly_x * (0.65f * ux_arch + 0.35f * vy_arch) * m_sqrt(ut);
            u32 vi = j * n + i;
            m->px[vi] = x;
            m->py[vi] = y;
            m->pz[vi] = z;
            m->u[vi]  = ut;
            m->v[vi]  = 1.0f - vt;
        }
    }
    u32* ix = m->indices;
    for (u32 j = 0; j < div; j++) {
        for (u32 i = 0; i < div; i++) {
            u32 a0 = j * n + i, a1 = a0 + 1u, b0 = a0 + n, b1 = b0 + 1u;
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }
    mesh_compute_normals(m);
    mesh_compute_bounds(m);
    return m;
}

/* ---------- Tekne Fiziği ve Dönüşüm Matrisi ---------- */

static Mat4 mat4_rot_z(f32 rad) {
    f32 c = m_cos(rad), s = m_sin(rad);
    Mat4 m = mat4_identity();
    m.m[0] =  c; m.m[4] = -s;
    m.m[1] =  s; m.m[5] =  c;
    return m;
}

static Vec3 mat4_xform_point(Mat4 m, Vec3 p) {
    Vec4 r = mat4_mul_vec4(m, vec4(p.x, p.y, p.z, 1.0f));
    return vec3(r.x, r.y, r.z);
}

static Mat4 boat_world_matrix(const BoatState* b) {
    Mat4 ry = mat4_rotate_y(b->yaw);
    Mat4 rx = mat4_rotate_x(b->pitch);
    Mat4 rz = mat4_rot_z(b->roll);
    Mat4 rot = mat4_mul(ry, mat4_mul(rx, rz));
    return mat4_mul(mat4_translate(b->pos), rot);
}

static void reset_boat_to_water(OceanScene* sc) {
    __builtin_memset(&sc->boat, 0, sizeof(sc->boat));
    sc->boat.yaw = 0.25f;
    OceanSample s_c, s_f, s_b, s_l, s_r;
    ocean_sample(&sc->ocean,  0.0f,  0.0f, 0.0f, &s_c);
    ocean_sample(&sc->ocean,  0.0f,  2.8f, 0.0f, &s_f);
    ocean_sample(&sc->ocean,  0.0f, -2.8f, 0.0f, &s_b);
    ocean_sample(&sc->ocean, -1.2f,  0.0f, 0.0f, &s_l);
    ocean_sample(&sc->ocean,  1.2f,  0.0f, 0.0f, &s_r);
    sc->boat.pos   = vec3(s_c.disp.x * 0.35f, s_c.disp.y - 0.16f, s_c.disp.z * 0.35f);
    sc->boat.pitch = m_clamp(-(s_f.disp.y - s_b.disp.y) / 6.5f, -0.25f, 0.25f);
    sc->boat.roll  = m_clamp((s_r.disp.y - s_l.disp.y) / 3.2f, -0.25f, 0.25f);
}

/* 8 gövde prob noktasında okyanus yüksekliğini örnekleyerek kaldırma kuvveti,
 * baş-kıç (pitch) ve yalpa (roll) momenti + viskoz su sönümü uygular. */
static void step_boat_physics(OceanScene* sc, f32 dt) {
    if (dt <= 0.0f) return;
    f32 rem = m_min(dt, 0.08f);
    const f32 max_sub = 1.0f / 120.0f;

    /* Gövde üzerindeki 8 su hattı prob noktası (yerel uzay, y=+0.02m su hattı) */
    static const Vec3 probes[8] = {
        {  0.00f,  0.04f,  3.35f },   /* pruva */
        {  0.00f,  0.02f, -3.15f },   /* kıç */
        { -1.05f,  0.02f,  1.65f },   /* iskele baş omuzluk */
        {  1.05f,  0.02f,  1.65f },   /* sancak baş omuzluk */
        { -1.32f, -0.02f, -0.15f },   /* iskele vasat */
        {  1.32f, -0.02f, -0.15f },   /* sancak vasat */
        { -1.08f,  0.02f, -2.05f },   /* iskele kıç omuzluk */
        {  1.08f,  0.02f, -2.05f }    /* sancak kıç omuzluk */
    };

    BoatState* b = &sc->boat;
    while (rem > 1e-5f) {
        f32 h = m_min(rem, max_sub);
        rem -= h;

        Mat4 m_boat = boat_world_matrix(b);
        f32 fy_total   = -OCEAN_G * 1.0f;   /* birim kütle yerçekimi */
        f32 tau_pitch  = 0.0f;
        f32 tau_roll   = 0.0f;
        f32 avg_disp_x = 0.0f;
        f32 avg_disp_z = 0.0f;

        for (u32 i = 0; i < 8u; i++) {
            Vec3 lp = probes[i];
            Vec3 wp = mat4_xform_point(m_boat, lp);
            OceanSample os;
            ocean_sample(&sc->ocean, wp.x, wp.z, 0.0f, &os);

            avg_disp_x += os.disp.x;
            avg_disp_z += os.disp.z;

            /* Prob noktasının düşey hızı: v_y - ω_pitch * z + ω_roll * x */
            f32 v_probe_y = b->vel.y - b->vel_pitch * lp.z + b->vel_roll * lp.x;
            f32 depth = os.disp.y - wp.y;
            if (depth > -0.25f) {
                /* Yumuşak geçişli batma derinliği */
                f32 sub = m_clamp(depth + 0.25f, 0.0f, 1.40f);
                f32 f_buoy = sub * 5.20f - v_probe_y * 1.25f;
                if (f_buoy < -1.5f) f_buoy = -1.5f;
                fy_total  += f_buoy;
                /* Pitch (+X dönüşü: +Z'yi aşağı, -Z'yi yukarı iter -> tau_pitch -= lp.z * f_buoy) */
                tau_pitch -= lp.z * f_buoy * 0.38f;
                /* Roll (+Z dönüşü: +X'i yukarı, -X'i aşağı iter -> tau_roll += lp.x * f_buoy) */
                tau_roll  += lp.x * f_buoy * 0.72f;
            }
        }
        avg_disp_x *= 0.125f;
        avg_disp_z *= 0.125f;

        /* Doğrultucu omurga momenti (metacentric righting) + açısal su sönümü */
        tau_pitch += -b->pitch * 8.5f - b->vel_pitch * 4.8f;
        tau_roll  += -b->roll  * 11.0f - b->vel_roll  * 5.2f;

        b->vel.y     += fy_total * h;
        b->vel.y     *= (1.0f - 1.6f * h);
        b->pos.y     += b->vel.y * h;

        b->vel_pitch += tau_pitch * h;
        b->vel_roll  += tau_roll * h;
        b->pitch     += b->vel_pitch * h;
        b->roll      += b->vel_roll * h;
        b->pitch      = m_clamp(b->pitch, -0.35f, 0.35f);
        b->roll       = m_clamp(b->roll,  -0.38f, 0.38f);

        /* Yatay dalga yörünge sürüklemesi + yumuşak merkezleme bağı */
        f32 target_x = avg_disp_x * 0.45f;
        f32 target_z = avg_disp_z * 0.45f;
        b->vel.x += ((target_x - b->pos.x) * 3.2f - b->vel.x * 2.4f) * h;
        b->vel.z += ((target_z - b->pos.z) * 3.2f - b->vel.z * 2.4f) * h;
        b->pos.x += b->vel.x * h;
        b->pos.z += b->vel.z * h;

        /* Hafif pruva salınımı */
        f32 target_yaw = 0.25f + m_sin((f32)sc->sim_time * 0.45f) * 0.06f - b->roll * 0.12f;
        b->vel_yaw += ((target_yaw - b->yaw) * 4.0f - b->vel_yaw * 3.0f) * h;
        b->yaw     += b->vel_yaw * h;
    }
}

/* ---------- Tel Kafes (Wireframe) Çizimi ---------- */

static void draw_line_2d_clipped(Framebuffer* fb, i32 x0, i32 y0, i32 x1, i32 y1, u32 color) {
    i32 w = (i32)fb->w, h = (i32)fb->h;
    if ((x0 < 0 && x1 < 0) || (x0 >= w && x1 >= w) || (y0 < 18 && y1 < 18) || (y0 >= h && y1 >= h)) {
        return;
    }
    i32 dx = x1 - x0; if (dx < 0) dx = -dx;
    i32 dy = y1 - y0; if (dy < 0) dy = -dy;
    i32 sx = (x0 < x1) ? 1 : -1;
    i32 sy = (y0 < y1) ? 1 : -1;
    i32 err = dx - dy;
    u32 steps = 0;
    while (steps++ < 1024u) {
        if (x0 >= 0 && x0 < w && y0 >= 18 && y0 < h) {
            fb->px[(u32)y0 * fb->w + (u32)x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void draw_ocean_wireframe(const Ocean* o, Framebuffer* fb, Mat4 mvp) {
    const Mesh* m = o->mesh;
    if (!m) return;
    u32 div = o->grid_div;
    u32 n   = div + 1u;
    f32 hw  = (f32)fb->w * 0.5f;
    f32 hh  = (f32)fb->h * 0.5f;
    u32 col_main = FB_RGBA(70, 235, 255, 255);
    u32 col_sub  = FB_RGBA(35, 145, 190, 255);

    /* Her 2 hücrede bir ızgara çizgisi çiz (performans + görsel netlik) */
    for (u32 j = 0; j < div; j += 2u) {
        for (u32 i = 0; i < div; i += 2u) {
            u32 i00 = j * n + i;
            u32 i10 = j * n + MIN(i + 2u, div);
            u32 i01 = MIN(j + 2u, div) * n + i;

            Vec4 c00 = mat4_mul_vec4(mvp, vec4(m->px[i00], m->py[i00], m->pz[i00], 1.0f));
            if (c00.w <= 0.3f) continue;
            f32 inv00 = 1.0f / c00.w;
            i32 x00 = (i32)((c00.x * inv00 + 1.0f) * hw);
            i32 y00 = (i32)((1.0f - c00.y * inv00) * hh);
            u32 col = (c00.w < 45.0f) ? col_main : col_sub;

            Vec4 c10 = mat4_mul_vec4(mvp, vec4(m->px[i10], m->py[i10], m->pz[i10], 1.0f));
            if (c10.w > 0.3f) {
                f32 inv10 = 1.0f / c10.w;
                i32 x10 = (i32)((c10.x * inv10 + 1.0f) * hw);
                i32 y10 = (i32)((1.0f - c10.y * inv10) * hh);
                draw_line_2d_clipped(fb, x00, y00, x10, y10, col);
            }

            Vec4 c01 = mat4_mul_vec4(mvp, vec4(m->px[i01], m->py[i01], m->pz[i01], 1.0f));
            if (c01.w > 0.3f) {
                f32 inv01 = 1.0f / c01.w;
                i32 x01 = (i32)((c01.x * inv01 + 1.0f) * hw);
                i32 y01 = (i32)((1.0f - c01.y * inv01) * hh);
                draw_line_2d_clipped(fb, x00, y00, x01, y01, col);
            }
        }
    }
}

/* ---------- Tekne Parçası Çizim Yardımcıları ---------- */

static void draw_boat_part(G3dContext* ctx, Arena* frame, const G3dMaterial* base_mat,
                           const Mesh* mesh, Mat4 boat_m, Vec3 pos, Vec3 scale, f32 rot_x,
                           const Texture* tex, Vec4 color, Vec3 light_dir, Vec3 fog_srgb, f32 fog_dens,
                           G3dCull cull) {
    Mat4 local = mat4_mul(mat4_translate(pos), mat4_mul(mat4_rotate_x(rot_x), mat4_scale(scale)));
    Mat4 model = mat4_mul(boat_m, local);
    Lit3dUniforms* u = ARENA_PUSH_STRUCT(frame, Lit3dUniforms);
    u->light_dir = vec4(light_dir.x, light_dir.y, light_dir.z, 0.0f);
    u->base      = color;
    u->fog       = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
    G3dMaterial m = *base_mat;
    m.uniforms = u;
    m.textures = tex;
    m.cull     = cull;
    g3d_draw(ctx, mesh, &model, &m);
}

static void draw_boat_emissive(G3dContext* ctx, Arena* frame, const G3dMaterial* em_mat,
                               const Mesh* mesh, Mat4 boat_m, Vec3 pos, Vec3 scale,
                               Vec4 color, Vec3 fog_srgb, f32 fog_dens) {
    Mat4 local = mat4_mul(mat4_translate(pos), mat4_scale(scale));
    Mat4 model = mat4_mul(boat_m, local);
    Unlit3dUniforms* u = ARENA_PUSH_STRUCT(frame, Unlit3dUniforms);
    u->color = color;
    u->fog   = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
    G3dMaterial m = *em_mat;
    m.uniforms = u;
    g3d_draw(ctx, mesh, &model, &m);
}

/* ---------- Sahne Kurulumu & Sıfırlama ---------- */

static void reset_scene_params(OceanScene* sc) {
    OceanDesc d = ocean_default_desc();
    sc->ocean.d.wind_speed = d.wind_speed;
    sc->ocean.d.choppy     = d.choppy;
    ocean_rebuild_spectrum(&sc->ocean);

    sc->sim_time   = 4.25;
    sc->time_scale = 1.0f;
    sc->paused     = FALSE;
    sc->sun_elev   = 0.15f;   /* altın ikindi güneşi: parlak güneş diski + yakamoz + turkuaz SSS */
    sc->sun_azim   = 0.42f;
    sc->wireframe  = 0u;

    camera_init(&sc->cam, CAM_ORBIT);
    sc->cam.yaw    = 0.52f;
    sc->cam.pitch  = -0.17f;
    sc->cam.dist   = 17.5f;
    sc->cam.near_z = 0.25f;

    ocean_update(&sc->ocean, sc->sim_time, 0.0f);
    reset_boat_to_water(sc);
    sc->cam.target = vec3(sc->boat.pos.x, sc->boat.pos.y * 0.35f + 1.35f, sc->boat.pos.z);
    ocean_mesh_update(&sc->ocean, camera_eye(&sc->cam));
}

void scene_ocean_init(Arena* perm, Arena* scratch) {
    (void)scratch;
    __builtin_memset(&g_sc, 0, sizeof(g_sc));
    g_sc.show_hud = TRUE;

    OceanDesc desc = ocean_default_desc();
    ocean_init(&g_sc.ocean, perm, &desc);
    /* 96×96 kamera merkezli geometrik ızgara (9409 köşe, 18432 üçgen, 320m yarıçap) */
    ocean_mesh_init(&g_sc.ocean, perm, 96u, 2.5f, 320.0f);

    /* Gök kubbesi (kamera merkezli, yarıçap 540m < far 650m) ve tekne ağları */
    g_sc.sky_mesh       = mesh_sphere(perm, 540.0f, 32u, 16u);
    g_sc.hull_mesh      = build_hull_mesh(perm);
    g_sc.main_sail_mesh = build_sail_mesh(perm, -3.10f, -0.35f, -2.55f, -0.35f, 1.35f, 5.65f, 0.52f);
    g_sc.jib_sail_mesh  = build_sail_mesh(perm,  0.10f,  4.55f,  0.00f,  0.20f, 1.15f, 4.95f, 0.42f);
    g_sc.unit_cube      = mesh_cube(perm, 0.5f);

    build_wood_tex(&g_sc.tex_hull, perm, 158u, 78u, 42u, TRUE);
    build_wood_tex(&g_sc.tex_deck, perm, 205u, 152u, 96u, TRUE);
    build_sail_tex(&g_sc.tex_sail, perm);

    reset_scene_params(&g_sc);
}

/* ---------- Kare Güncelleme ve Çizim ---------- */

void scene_ocean_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                       const InputState* in, f32 dt, b32 use_jit, b32 look_enabled) {
    /* Klavye kontrolleri */
    if (sc_key_pressed(in, KEY_R)) {
        reset_scene_params(&g_sc);
    }
    if (sc_key_pressed(in, KEY_C)) {
        if (g_sc.cam.mode == CAM_ORBIT) {
            g_sc.cam.pos  = camera_eye(&g_sc.cam);
            g_sc.cam.mode = CAM_FLY;
        } else {
            g_sc.cam.mode = CAM_ORBIT;
        }
    }
    if (sc_key_pressed(in, KEY_M) || sc_key_pressed(in, KEY_V)) {
        g_sc.wireframe = (g_sc.wireframe + 1u) % 3u;
    }
    if (sc_key_pressed(in, KEY_SPACE)) {
        g_sc.paused = !g_sc.paused;
    }

    /* Rüzgâr hızı +/- (U / I veya Orbit modunda Aşağı / Yukarı ok) */
    b32 orbit_arrows = (g_sc.cam.mode == CAM_ORBIT);
    f32 wind_delta = 0.0f;
    if (in->keys[KEY_I] || (orbit_arrows && in->keys[KEY_UP]))   wind_delta += 4.5f * dt;
    if (in->keys[KEY_U] || (orbit_arrows && in->keys[KEY_DOWN])) wind_delta -= 4.5f * dt;
    if (wind_delta != 0.0f) {
        f32 nw = m_clamp(g_sc.ocean.d.wind_speed + wind_delta, 2.0f, 24.0f);
        if (m_abs(nw - g_sc.ocean.d.wind_speed) > 1e-4f) {
            g_sc.ocean.d.wind_speed = nw;
            ocean_rebuild_spectrum(&g_sc.ocean);
        }
    }

    /* Choppiness λ +/- (H / K veya Orbit modunda Sol / Sağ ok) */
    f32 chop_delta = 0.0f;
    if (in->keys[KEY_K] || (orbit_arrows && in->keys[KEY_RIGHT])) chop_delta += 0.65f * dt;
    if (in->keys[KEY_H] || (orbit_arrows && in->keys[KEY_LEFT]))  chop_delta -= 0.65f * dt;
    if (chop_delta != 0.0f) {
        g_sc.ocean.d.choppy = m_clamp(g_sc.ocean.d.choppy + chop_delta, 0.0f, 2.2f);
    }

    /* Zaman ölçeği +/- (L / O) */
    if (in->keys[KEY_O]) g_sc.time_scale = m_clamp(g_sc.time_scale + 0.75f * dt, 0.05f, 3.0f);
    if (in->keys[KEY_L]) g_sc.time_scale = m_clamp(g_sc.time_scale - 0.75f * dt, 0.05f, 3.0f);

    /* Güneş yüksekliği +/- (G / F) */
    if (in->keys[KEY_G]) g_sc.sun_elev = m_clamp(g_sc.sun_elev + 0.35f * dt, 0.06f, 0.95f);
    if (in->keys[KEY_F]) g_sc.sun_elev = m_clamp(g_sc.sun_elev - 0.35f * dt, 0.06f, 0.95f);

    __builtin_memcpy(g_sc.prev_keys, in->keys, sizeof(g_sc.prev_keys));

    /* Simülasyon adımı */
    f32 eff_dt = (g_sc.paused || dt <= 0.0f) ? 0.0f : (dt * g_sc.time_scale);
    g_sc.sim_time += (f64)eff_dt;

    /* (1) FFT kademeleri + doku/mipmap güncellemesi */
    PROF_BEGIN("ocean_fft");
    f64 t_fft0 = plat_time_ms();
    if (eff_dt > 0.0f || wind_delta != 0.0f || chop_delta != 0.0f || !g_sc.ocean.has_updated) {
        ocean_update(&g_sc.ocean, g_sc.sim_time, eff_dt);
    }
    f64 t_fft1 = plat_time_ms();
    g_sc.fft_ms = (f32)(t_fft1 - t_fft0);
    PROF_END();

    /* (2) Tekne fiziği ve kamera güncellemesi */
    if (eff_dt > 0.0f) {
        step_boat_physics(&g_sc, eff_dt);
    }
    if (g_sc.cam.mode == CAM_ORBIT) {
        Vec3 desired = vec3(g_sc.boat.pos.x, g_sc.boat.pos.y * 0.35f + 1.35f, g_sc.boat.pos.z);
        if (dt > 0.0f) {
            f32 a = m_min(dt * 6.0f, 1.0f);
            g_sc.cam.target = vec3_add(g_sc.cam.target, vec3_scale(vec3_sub(desired, g_sc.cam.target), a));
        } else {
            g_sc.cam.target = desired;
        }
    }
    camera_update(&g_sc.cam, in, dt, look_enabled);
    Vec3 eye = camera_eye(&g_sc.cam);

    /* (3) Kamera merkezli okyanus ızgara ağı güncellemesi */
    PROF_BEGIN("ocean_mesh");
    f64 t_mesh0 = plat_time_ms();
    ocean_mesh_update(&g_sc.ocean, eye);
    f64 t_mesh1 = plat_time_ms();
    g_sc.mesh_ms = (f32)(t_mesh1 - t_mesh0);
    PROF_END();

    /* Güneş ve atmosfer renkleri (kamera bakış kongrüansı: -sin(azim), sin(elev), -cos(azim)) */
    f32 ce = m_cos(g_sc.sun_elev), se = m_sin(g_sc.sun_elev);
    Vec3 sun_dir = vec3_norm(vec3(-ce * m_sin(g_sc.sun_azim), se, -ce * m_cos(g_sc.sun_azim)));
    f32 warm = m_clamp(1.0f - (g_sc.sun_elev - 0.08f) * 2.2f, 0.0f, 1.0f);

    Vec3 sun_noon   = vec3(3.2f, 3.0f, 2.65f);
    Vec3 sun_golden = vec3(4.1f, 2.25f, 1.05f);
    Vec3 sun_col    = vec3_add(sun_noon, vec3_scale(vec3_sub(sun_golden, sun_noon), warm));

    Vec3 zen_noon   = vec3(0.035f, 0.18f, 0.58f);
    Vec3 zen_golden = vec3(0.028f, 0.13f, 0.44f);
    Vec3 sky_zenith = vec3_add(zen_noon, vec3_scale(vec3_sub(zen_golden, zen_noon), warm));

    Vec3 hor_noon   = vec3(0.36f, 0.58f, 0.82f);
    Vec3 hor_golden = vec3(0.68f, 0.46f, 0.34f);
    Vec3 sky_horiz  = vec3_add(hor_noon, vec3_scale(vec3_sub(hor_golden, hor_noon), warm));

    Vec3 sea_deep    = vec3(0.005f, 0.042f, 0.115f);
    Vec3 sea_shallow = vec3(0.025f, 0.42f, 0.38f);
    Vec3 fog_srgb    = vec3(m_sqrt(m_clamp(sky_horiz.x, 0.0f, 1.0f)),
                            m_sqrt(m_clamp(sky_horiz.y, 0.0f, 1.0f)),
                            m_sqrt(m_clamp(sky_horiz.z, 0.0f, 1.0f)));
    f32  fog_dens    = 1.0f / 340.0f;

    /* (4) 3D Sahne Çizimi */
    fb_clear(fb, FB_RGBA(12, 24, 42, 255));
    fb_clear_depth(fb, 0.0f);

    raster_begin(r, fb, frame);
    G3dContext ctx;
    f32 aspect = (f32)fb->w / (f32)fb->h;
    Mat4 view = camera_view(&g_sc.cam);
    Mat4 proj = camera_proj(&g_sc.cam, aspect);
    g3d_begin(&ctx, r, frame, view, proj, eye);
    g3d_set_deferred(&ctx, 128u);

    /* (4a) Yüzen tekne parçaları (lit3d.mtk + unlit3d.mtk) */
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

    Mat4 boat_m = boat_world_matrix(&g_sc.boat);
    /* Gövde ve güverte */
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.hull_mesh, boat_m,
                   vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                   &g_sc.tex_hull, vec4(1.0f, 0.94f, 0.88f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);
    /* Kamara gövdesi ve çatısı */
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.unit_cube, boat_m,
                   vec3(0.0f, 0.78f, -1.35f), vec3(1.38f, 0.62f, 1.85f), 0.0f,
                   &g_sc.tex_deck, vec4(0.95f, 0.90f, 0.82f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.unit_cube, boat_m,
                   vec3(0.0f, 1.13f, -1.35f), vec3(1.52f, 0.12f, 2.02f), 0.0f,
                   &g_sc.tex_hull, vec4(0.75f, 0.45f, 0.28f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    /* Sıcak ışıklı kamara fener pencereleri */
    draw_boat_emissive(&ctx, frame, &mat_em, g_sc.unit_cube, boat_m,
                       vec3(0.0f, 0.84f, -1.35f), vec3(1.42f, 0.22f, 0.85f),
                       vec4(1.0f, 0.78f, 0.35f, 1.0f), fog_srgb, fog_dens);
    /* Ana direk, bumba, civadra (bowsprit) */
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.unit_cube, boat_m,
                   vec3(0.0f, 3.15f, -0.25f), vec3(0.14f, 5.60f, 0.14f), 0.0f,
                   &g_sc.tex_deck, vec4(0.88f, 0.74f, 0.55f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.unit_cube, boat_m,
                   vec3(0.0f, 1.32f, -1.72f), vec3(0.10f, 0.10f, 2.95f), 0.0f,
                   &g_sc.tex_deck, vec4(0.85f, 0.70f, 0.52f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.unit_cube, boat_m,
                   vec3(0.0f, 1.02f, 4.15f), vec3(0.10f, 0.10f, 1.75f), -0.18f,
                   &g_sc.tex_deck, vec4(0.85f, 0.70f, 0.52f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_BACK);
    /* Direk tepesi kırmızı flama */
    draw_boat_emissive(&ctx, frame, &mat_em, g_sc.unit_cube, boat_m,
                       vec3(0.12f, 5.88f, -0.55f), vec3(0.04f, 0.16f, 0.65f),
                       vec4(0.92f, 0.22f, 0.16f, 1.0f), fog_srgb, fog_dens);
    /* Şişkin ana yelken ve flok yelkeni */
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.main_sail_mesh, boat_m,
                   vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                   &g_sc.tex_sail, vec4(1.0f, 0.97f, 0.92f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);
    draw_boat_part(&ctx, frame, &mat_lit, g_sc.jib_sail_mesh, boat_m,
                   vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                   &g_sc.tex_sail, vec4(0.98f, 0.94f, 0.88f, 1.0f), sun_dir, fog_srgb, fog_dens, G3D_CULL_NONE);

    /* (4b) Okyanus yüzeyi (ocean.mtk) — yalnız tel kafes modunda değilse */
    if (g_sc.wireframe != 2u) {
        const ManagedShader* sh_ocean = shaders_find(sm, "ocean.mtk");
        b32 ocean_fused = FALSE;
        ShadeFn fn_ocean = shaders_get_fn(sh_ocean, use_jit, BLEND_NONE, &ocean_fused);

        OceanUniforms* ou = ARENA_PUSH_STRUCT(frame, OceanUniforms);
        ou->sun_dir     = vec4(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
        ou->sun_col     = vec4(sun_col.x, sun_col.y, sun_col.z, 0.0f);
        ou->sky_zenith  = vec4(sky_zenith.x, sky_zenith.y, sky_zenith.z, 0.0f);
        ou->sky_horizon = vec4(sky_horiz.x, sky_horiz.y, sky_horiz.z, 0.0f);
        ou->sea_deep    = vec4(sea_deep.x, sea_deep.y, sea_deep.z, 0.0f);
        ou->sea_shallow = vec4(sea_shallow.x, sea_shallow.y, sea_shallow.z, 0.0f);
        ou->eye         = vec4(eye.x, eye.y, eye.z, 0.0f);
        f32 l0 = g_sc.ocean.cascades[0].size;
        f32 l1 = g_sc.ocean.cascades[1].size;
        f32 l2 = g_sc.ocean.cascades[2].size;
        ou->scales = vec4(l0 / l1, l0 / l2, OCEAN_TEX_SLOPE_MAX * 2.0f, fog_dens);
        ou->params = vec4(1.25f, 1.15f, 1.10f, 0.35f + 0.65f * warm);

        Texture* otex = ARENA_PUSH_ARRAY(frame, Texture, 3u);
        otex[0] = g_sc.ocean.cascades[0].tex;
        otex[1] = g_sc.ocean.cascades[1].tex;
        otex[2] = g_sc.ocean.cascades[2].tex;

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
        g3d_draw(&ctx, g_sc.ocean.mesh, &id, &mat_ocean);
    }

    /* (4c) Gök kubbesi (sky.mtk) — en son çizilir, early-Z ile yalnız açık gökyüzü pikselleri gölgelenir */
    {
        const ManagedShader* sh_sky = shaders_find(sm, "sky.mtk");
        b32 sky_fused = FALSE;
        ShadeFn fn_sky = shaders_get_fn(sh_sky, use_jit, BLEND_NONE, &sky_fused);

        SkyUniforms* su = ARENA_PUSH_STRUCT(frame, SkyUniforms);
        su->sun_dir     = vec4(sun_dir.x, sun_dir.y, sun_dir.z, 0.0f);
        su->sun_col     = vec4(sun_col.x, sun_col.y, sun_col.z, 0.0f);
        su->sky_zenith  = vec4(sky_zenith.x, sky_zenith.y, sky_zenith.z, 0.0f);
        su->sky_horizon = vec4(sky_horiz.x, sky_horiz.y, sky_horiz.z, 0.0f);
        su->params      = vec4((f32)g_sc.sim_time, 0.42f, 1.0f, 0.35f + 0.65f * warm);

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
        g3d_draw(&ctx, g_sc.sky_mesh, &sky_m, &mat_sky);
    }

    g3d_flush(&ctx);
    f64 t_sh0 = plat_time_ms();
    raster_flush(r);
    f64 t_sh1 = plat_time_ms();
    g_sc.shade_ms   = (f32)(t_sh1 - t_sh0);
    g_sc.tris_drawn = (u32)ctx.stats.tris_out;

    /* Profiler'da "shade" bölgesi varsa onu da oku */
    for (u32 z = 0; z < prof_zone_count(); z++) {
        const ProfZone* pz = prof_zone_get(z);
        if (pz && pz->name[0] == 's' && pz->name[1] == 'h' && pz->name[2] == 'a' &&
            pz->name[3] == 'd' && pz->name[4] == 'e' && pz->name[5] == '\0') {
            g_sc.shade_ms = (f32)pz->last_ms;
            break;
        }
    }

    /* Tel kafes modu açıksa ızgara çizgilerini çiz */
    if (g_sc.wireframe > 0u) {
        draw_ocean_wireframe(&g_sc.ocean, fb, mat4_mul(proj, view));
    }

    /* Debug tamponunu doldur */
    g_sc.debug_buf[0]  = g_sc.fft_ms;
    g_sc.debug_buf[1]  = g_sc.mesh_ms;
    g_sc.debug_buf[2]  = g_sc.shade_ms;
    g_sc.debug_buf[3]  = g_sc.ocean.hs;
    g_sc.debug_buf[4]  = g_sc.ocean.theory_hs;
    g_sc.debug_buf[5]  = g_sc.ocean.d.wind_speed;
    g_sc.debug_buf[6]  = g_sc.ocean.d.choppy;
    g_sc.debug_buf[7]  = g_sc.paused ? 0.0f : g_sc.time_scale;
    g_sc.debug_buf[8]  = g_sc.boat.pos.y;
    g_sc.debug_buf[9]  = g_sc.boat.pitch;
    g_sc.debug_buf[10] = g_sc.boat.roll;
    g_sc.debug_buf[11] = (f32)g_sc.tris_drawn;
}

/* ---------- HUD Çizimi ---------- */

void scene_ocean_draw_hud(UiContext* ui, const Framebuffer* fb) {
    if (!g_sc.show_hud) return;

    char line[160];
    i32 y = (i32)fb->h - 24;
    ui_draw_rect(ui, 0, y - 14, (i32)fb->w, 38, FB_RGBA(10, 16, 28, 195));

    fmt_format(line, sizeof(line),
               "FFT %.2fms | Mesh %.2fms | Shade %.2fms | Hs=%.2fm (teo %.2fm) | Ucgen %u",
               (f64)g_sc.fft_ms, (f64)g_sc.mesh_ms, (f64)g_sc.shade_ms,
               (f64)g_sc.ocean.hs, (f64)g_sc.ocean.theory_hs, g_sc.tris_drawn);
    ui_draw_text(ui, 5, y - 11, line, FB_RGBA(110, 230, 255, 255), 1);

    const char* wf_str = (g_sc.wireframe == 0u) ? "Kap" : (g_sc.wireframe == 1u) ? "Karma" : "Tel";
    fmt_format(line, sizeof(line),
               "[U/I]Ruzgar %.1f [H/K]Chop %.2f [L/O/Spc]Hiz %.1fx%s [F/G]Gunes [M]Tel:%s [C]%s",
               (f64)g_sc.ocean.d.wind_speed, (f64)g_sc.ocean.d.choppy,
               (f64)g_sc.time_scale, g_sc.paused ? "*" : "",
               wf_str, (g_sc.cam.mode == CAM_ORBIT) ? "Orb" : "Ucus");
    ui_draw_text(ui, 5, y + 1, line, FB_RGBA(225, 235, 248, 255), 1);
}

void scene_ocean_set_hud(b32 show) {
    g_sc.show_hud = show ? TRUE : FALSE;
}

const f32* scene_ocean_debug(void) {
    return g_sc.debug_buf;
}

void scene_ocean_debug_cmd(u32 cmd, f32 a, f32 b, f32 c) {
    if (cmd == 0u) {
        reset_scene_params(&g_sc);
    } else if (cmd == 1u) {
        /* Sabit zaman, rüzgâr ve choppiness (deterministik görsel/JIT testi için) */
        if (b > 0.0f) {
            g_sc.ocean.d.wind_speed = m_clamp(b, 2.0f, 24.0f);
            ocean_rebuild_spectrum(&g_sc.ocean);
        }
        if (c >= 0.0f) {
            g_sc.ocean.d.choppy = m_clamp(c, 0.0f, 2.2f);
        }
        g_sc.sim_time = (f64)a;
        g_sc.paused   = TRUE;
        g_sc.ocean.has_updated = FALSE;
        ocean_update(&g_sc.ocean, g_sc.sim_time, 0.0f);
        reset_boat_to_water(&g_sc);
        g_sc.cam.target = vec3(g_sc.boat.pos.x, g_sc.boat.pos.y * 0.45f + 1.15f, g_sc.boat.pos.z);
        ocean_mesh_update(&g_sc.ocean, camera_eye(&g_sc.cam));
    } else if (cmd == 2u) {
        g_sc.cam.yaw   = a;
        g_sc.cam.pitch = b;
        g_sc.cam.dist  = c;
        ocean_mesh_update(&g_sc.ocean, camera_eye(&g_sc.cam));
    } else if (cmd == 3u) {
        g_sc.sun_elev   = m_clamp(a, 0.06f, 0.95f);
        g_sc.time_scale = m_clamp(b, 0.0f, 3.0f);
        g_sc.wireframe  = ((u32)c) % 3u;
    }
}
