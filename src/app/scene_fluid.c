/* app/scene_fluid.c — "Akışkan" (P1_F) sahnesi implementasyonu.
 * GPU üzerinde 262 144 (256k) parçacıklı Position Based Fluids (Macklin 2013) +
 * Ekran-Uzayı Akışkan Görüntüleme (SSFR) ve katı cisim / eğilebilir kova etkileşimi.
 * WebGPU yoksa 1024 parçacıklı CPU PBF yedek yoluna düşer.
 */
#include "app/scene_fluid.h"
#include "gfx3d/camera.h"
#include "gpu/rhi.h"
#include "gpu/gpu_fluid.h"
#include "core/math.h"
#include "core/log.h"
#include "plat/platform.h"

/* Tuş kodları */
#define KEY_SPACE 32
#define KEY_LEFT  37
#define KEY_UP    38
#define KEY_RIGHT 39
#define KEY_DOWN  40
#define KEY_B     66
#define KEY_C     67
#define KEY_F     70
#define KEY_G     71
#define KEY_H     72
#define KEY_N     78
#define KEY_O     79
#define KEY_R     82
#define KEY_U     85

/* CPU yedek yolu için küçük parçacık kümesi */
#define CPU_FLUID_N 1024u

typedef struct {
    Camera              cam;
    GpuFluidDesc        desc;
    GpuFluidSceneParams scene;

    f64    sim_time;
    f32    time_scale;
    b32    paused;
    b32    auto_slosh;
    b32    reset_pending;
    b32    show_hud;

    f32    tilt_x;
    f32    tilt_z;
    f32    sun_elev;

    /* CPU yedek parçacıkları */
    Vec4   cpu_pos[CPU_FLUID_N];
    Vec4   cpu_vel[CPU_FLUID_N];

    /* Ölçümler */
    f32    gpu_ms;
    f32    cpu_ms;
    f32    debug_buf[12];
    u8     prev_keys[256];
} FluidScene;

static FluidScene g_fl;

static b32 fl_key_pressed(const InputState* in, u32 key) {
    if (key >= 256u) return FALSE;
    return in->keys[key] != 0u && g_fl.prev_keys[key] == 0u;
}

static void init_cpu_particles(FluidScene* sc) {
    /* 16 x 8 x 8 = 1024 parçacık dam-break bloğu */
    u32 idx = 0u;
    for (u32 z = 0u; z < 8u; z++) {
        for (u32 y = 0u; y < 8u; y++) {
            for (u32 x = 0u; x < 16u; x++) {
                f32 jx = (f32)((idx * 1973u) & 15u) * 0.0005f;
                f32 jy = (f32)((idx * 9277u) & 15u) * 0.0005f;
                f32 jz = (f32)((idx * 3571u) & 15u) * 0.0005f;
                f32 px = sc->scene.box_min.x + 0.08f + (f32)x * 0.075f + jx;
                f32 py = sc->scene.box_min.y + 0.08f + (f32)y * 0.145f + jy;
                f32 pz = sc->scene.box_min.z + 0.12f + (f32)z * 0.195f + jz;
                sc->cpu_pos[idx] = vec4(px, py, pz, 0.0f);
                sc->cpu_vel[idx] = vec4(0.35f, 0.0f, 0.0f, 0.0f);
                idx++;
            }
        }
    }
}

static void reset_fluid_scene(FluidScene* sc) {
    camera_init(&sc->cam, CAM_ORBIT);
    sc->cam.target = vec3(0.0f, 0.52f, 0.0f);
    sc->cam.yaw    = 0.52f;
    sc->cam.pitch  = -0.48f;
    sc->cam.dist   = 3.25f;
    sc->cam.fov_y  = 52.0f * M_DEG2RAD;
    sc->cam.near_z = 0.10f;
    sc->cam.pos    = camera_eye(&sc->cam);

    sc->desc  = gpu_fluid_default_desc();
    sc->scene = gpu_fluid_default_scene();

    sc->sim_time      = 0.0;
    sc->time_scale    = 1.0f;
    sc->paused        = FALSE;
    sc->auto_slosh    = TRUE;
    sc->reset_pending = TRUE;
    sc->show_hud      = TRUE;

    sc->tilt_x   = 0.0f;
    sc->tilt_z   = 0.0f;
    sc->sun_elev = 0.62f;

    init_cpu_particles(sc);
}

static void update_rigid_bodies(FluidScene* sc, f32 dt) {
    f32 t = (f32)sc->sim_time;
    if (sc->auto_slosh && !sc->paused && t > 1.2f) {
        f32 phase = t - 1.2f;
        sc->tilt_x = 0.22f * m_sin(phase * 1.65f);
        sc->tilt_z = 0.09f * m_cos(phase * 1.15f);
    }

    /* Kova eğimini kutu sınırlarına yansıt (kova sağa-sola/öne-arkaya kayarak çalkalar) */
    f32 shift_x = sc->tilt_x * 0.45f;
    f32 shift_z = sc->tilt_z * 0.35f;
    sc->scene.box_min = vec3(-1.35f + shift_x, 0.0f, -0.95f + shift_z);
    sc->scene.box_max = vec3( 1.35f + shift_x, 1.90f, 0.95f + shift_z);

    Vec3 prev_s0 = sc->scene.sphere0_pos;
    f32 sx = 0.52f * m_cos(t * 1.35f) + shift_x * 0.5f;
    f32 sz = 0.36f * m_sin(t * 2.10f) + shift_z * 0.5f;
    f32 sy = 0.32f + 0.06f * m_sin(t * 2.80f);
    sc->scene.sphere0_pos = vec3(sx, sy, sz);
    if (dt > 1e-5f && !sc->paused) {
        sc->scene.sphere0_vel = vec3((sx - prev_s0.x) / dt,
                                     (sy - prev_s0.y) / dt,
                                     (sz - prev_s0.z) / dt);
    } else {
        sc->scene.sphere0_vel = vec3(-0.52f * 1.35f * m_sin(t * 1.35f),
                                      0.06f * 2.80f * m_cos(t * 2.80f),
                                      0.36f * 2.10f * m_cos(t * 2.10f));
    }
}

static void step_cpu_fallback(FluidScene* sc, f32 dt) {
    if (dt <= 0.0f) return;
    f32 gx = 9.81f * m_sin(sc->tilt_x);
    f32 gy = -9.81f * m_cos(sc->tilt_x) * m_cos(sc->tilt_z);
    f32 gz = 9.81f * m_sin(sc->tilt_z);
    f32 r_wall = 0.03f;

    for (u32 i = 0u; i < CPU_FLUID_N; i++) {
        Vec4 p = sc->cpu_pos[i];
        Vec4 v = sc->cpu_vel[i];
        v.x = (v.x + gx * dt) * 0.995f;
        v.y = (v.y + gy * dt) * 0.995f;
        v.z = (v.z + gz * dt) * 0.995f;
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;

        /* Kova sınırları */
        if (p.x < sc->scene.box_min.x + r_wall) { p.x = sc->scene.box_min.x + r_wall; v.x = m_abs(v.x) * 0.35f; }
        if (p.x > sc->scene.box_max.x - r_wall) { p.x = sc->scene.box_max.x - r_wall; v.x = -m_abs(v.x) * 0.35f; }
        if (p.y < sc->scene.box_min.y + r_wall) { p.y = sc->scene.box_min.y + r_wall; v.y = m_abs(v.y) * 0.25f; }
        if (p.y > sc->scene.box_max.y - r_wall) { p.y = sc->scene.box_max.y - r_wall; v.y = -m_abs(v.y) * 0.25f; }
        if (p.z < sc->scene.box_min.z + r_wall) { p.z = sc->scene.box_min.z + r_wall; v.z = m_abs(v.z) * 0.35f; }
        if (p.z > sc->scene.box_max.z - r_wall) { p.z = sc->scene.box_max.z - r_wall; v.z = -m_abs(v.z) * 0.35f; }

        /* Küre 0 çarpışması */
        f32 dx0 = p.x - sc->scene.sphere0_pos.x;
        f32 dy0 = p.y - sc->scene.sphere0_pos.y;
        f32 dz0 = p.z - sc->scene.sphere0_pos.z;
        f32 d0  = m_sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0);
        f32 mr0 = sc->scene.sphere0_rad + r_wall;
        if (d0 < mr0 && d0 > 1e-5f) {
            f32 inv = 1.0f / d0;
            p.x = sc->scene.sphere0_pos.x + dx0 * inv * mr0;
            p.y = sc->scene.sphere0_pos.y + dy0 * inv * mr0;
            p.z = sc->scene.sphere0_pos.z + dz0 * inv * mr0;
            v.x += sc->scene.sphere0_vel.x * 0.35f;
            v.z += sc->scene.sphere0_vel.z * 0.35f;
        }

        /* Küre 1 çarpışması */
        f32 dx1 = p.x - sc->scene.sphere1_pos.x;
        f32 dy1 = p.y - sc->scene.sphere1_pos.y;
        f32 dz1 = p.z - sc->scene.sphere1_pos.z;
        f32 d1  = m_sqrt(dx1 * dx1 + dy1 * dy1 + dz1 * dz1);
        f32 mr1 = sc->scene.sphere1_rad + r_wall;
        if (d1 < mr1 && d1 > 1e-5f) {
            f32 inv = 1.0f / d1;
            p.x = sc->scene.sphere1_pos.x + dx1 * inv * mr1;
            p.y = sc->scene.sphere1_pos.y + dy1 * inv * mr1;
            p.z = sc->scene.sphere1_pos.z + dz1 * inv * mr1;
        }

        f32 spd = m_sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        p.w = m_clamp((spd - 0.8f) * 0.35f, 0.0f, 1.0f);
        sc->cpu_pos[i] = p;
        sc->cpu_vel[i] = v;
    }
}

static void draw_line_2d(Framebuffer* fb, i32 x0, i32 y0, i32 x1, i32 y1, u32 col) {
    i32 dx = x1 - x0; if (dx < 0) dx = -dx;
    i32 dy = y1 - y0; if (dy < 0) dy = -dy;
    i32 sx = (x0 < x1) ? 1 : -1;
    i32 sy = (y0 < y1) ? 1 : -1;
    i32 err = dx - dy;
    for (u32 step = 0u; step < 2048u; step++) {
        if (x0 >= 0 && x0 < (i32)fb->w && y0 >= 0 && y0 < (i32)fb->h) {
            fb->px[(u32)y0 * fb->w + (u32)x0] = col;
        }
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static b32 project_pt(Mat4 vp, Vec3 p, u32 w, u32 h, i32* ox, i32* oy, f32* oz) {
    Vec4 c = mat4_mul_vec4(vp, vec4(p.x, p.y, p.z, 1.0f));
    if (c.w <= 0.05f) return FALSE;
    f32 inv_w = 1.0f / c.w;
    f32 ndc_x = c.x * inv_w;
    f32 ndc_y = c.y * inv_w;
    if (ndc_x < -1.5f || ndc_x > 1.5f || ndc_y < -1.5f || ndc_y > 1.5f) return FALSE;
    *ox = (i32)((ndc_x * 0.5f + 0.5f) * (f32)w);
    *oy = (i32)((0.5f - ndc_y * 0.5f) * (f32)h);
    *oz = c.w;
    return TRUE;
}

static void render_cpu_fallback(FluidScene* sc, Framebuffer* fb, Mat4 vp) {
    /* Gökyüzü ve zemin gradyanı */
    for (u32 y = 0u; y < fb->h; y++) {
        f32 t = (f32)y / (f32)(fb->h > 0u ? fb->h : 1u);
        u32 r, g, b;
        if (t < 0.45f) {
            f32 k = t / 0.45f;
            r = (u32)(18.0f + k * 65.0f);
            g = (u32)(38.0f + k * 90.0f);
            b = (u32)(78.0f + k * 110.0f);
        } else {
            f32 k = (t - 0.45f) / 0.55f;
            r = (u32)(28.0f - k * 10.0f);
            g = (u32)(52.0f - k * 18.0f);
            b = (u32)(72.0f - k * 22.0f);
        }
        u32 col = FB_RGBA(r, g, b, 255u);
        u32* row = &fb->px[y * fb->w];
        for (u32 x = 0u; x < fb->w; x++) row[x] = col;
    }

    /* Kova tel kafesi */
    Vec3 bmin = sc->scene.box_min;
    Vec3 bmax = sc->scene.box_max;
    Vec3 corners[8] = {
        vec3(bmin.x, bmin.y, bmin.z),
        vec3(bmax.x, bmin.y, bmin.z),
        vec3(bmax.x, bmin.y, bmax.z),
        vec3(bmin.x, bmin.y, bmax.z),
        vec3(bmin.x, bmax.y, bmin.z),
        vec3(bmax.x, bmax.y, bmin.z),
        vec3(bmax.x, bmax.y, bmax.z),
        vec3(bmin.x, bmax.y, bmax.z),
    };
    static const u8 edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for (u32 e = 0u; e < 12u; e++) {
        i32 x0, y0, x1, y1;
        f32 z0, z1;
        if (project_pt(vp, corners[edges[e][0]], fb->w, fb->h, &x0, &y0, &z0) &&
            project_pt(vp, corners[edges[e][1]], fb->w, fb->h, &x1, &y1, &z1)) {
            draw_line_2d(fb, x0, y0, x1, y1, FB_RGBA(140u, 195u, 235u, 255u));
        }
    }

    /* Katı küreleri çiz */
    Vec3 sp_pos[2] = { sc->scene.sphere0_pos, sc->scene.sphere1_pos };
    f32  sp_rad[2] = { sc->scene.sphere0_rad, sc->scene.sphere1_rad };
    u32  scols[2]  = { FB_RGBA(235u, 85u, 45u, 255u), FB_RGBA(45u, 195u, 185u, 255u) };
    for (u32 s = 0u; s < 2u; s++) {
        i32 cx, cy;
        f32 cz;
        if (project_pt(vp, sp_pos[s], fb->w, fb->h, &cx, &cy, &cz)) {
            i32 rad = (i32)((sp_rad[s] * (f32)fb->h) / cz);
            if (rad < 3) rad = 3;
            if (rad > 80) rad = 80;
            for (i32 dy = -rad; dy <= rad; dy++) {
                i32 py = cy + dy;
                if (py < 0 || py >= (i32)fb->h) continue;
                for (i32 dx = -rad; dx <= rad; dx++) {
                    if (dx * dx + dy * dy > rad * rad) continue;
                    i32 px = cx + dx;
                    if (px < 0 || px >= (i32)fb->w) continue;
                    fb->px[(u32)py * fb->w + (u32)px] = scols[s];
                }
            }
        }
    }

    /* Su parçacıklarını çiz */
    for (u32 i = 0u; i < CPU_FLUID_N; i++) {
        Vec4 p = sc->cpu_pos[i];
        i32 px, py;
        f32 pz;
        if (!project_pt(vp, vec3(p.x, p.y, p.z), fb->w, fb->h, &px, &py, &pz)) continue;
        i32 rad = (i32)((0.045f * (f32)fb->h) / pz);
        if (rad < 2) rad = 2;
        if (rad > 12) rad = 12;
        u32 foam = (u32)(m_clamp(p.w, 0.0f, 1.0f) * 140.0f);
        u32 col  = FB_RGBA(25u + foam, 145u + foam / 2u, 225u + foam / 5u, 255u);
        for (i32 dy = -rad; dy <= rad; dy++) {
            i32 yy = py + dy;
            if (yy < 0 || yy >= (i32)fb->h) continue;
            for (i32 dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dy * dy > rad * rad) continue;
                i32 xx = px + dx;
                if (xx < 0 || xx >= (i32)fb->w) continue;
                fb->px[(u32)yy * fb->w + (u32)xx] = col;
            }
        }
    }
}

void scene_fluid_init(Arena* perm, Arena* scratch) {
    (void)perm;
    (void)scratch;
    __builtin_memset(&g_fl, 0, sizeof(g_fl));
    reset_fluid_scene(&g_fl);
}

void scene_fluid_frame(Rasterizer* r, Framebuffer* fb, Arena* frame,
                       const ShaderManager* sm, const InputState* in,
                       f32 dt, b32 use_jit, b32 look_enabled) {
    (void)r;
    (void)sm;
    (void)use_jit;

    /* (1) Girdi ve kısayol tuşları */
    if (fl_key_pressed(in, KEY_SPACE)) {
        g_fl.paused = !g_fl.paused;
    }
    if (fl_key_pressed(in, KEY_B)) {
        g_fl.auto_slosh = !g_fl.auto_slosh;
        if (!g_fl.auto_slosh) {
            g_fl.tilt_x = 0.0f;
            g_fl.tilt_z = 0.0f;
        }
    }
    if (fl_key_pressed(in, KEY_C)) {
        g_fl.cam.mode = (g_fl.cam.mode == CAM_ORBIT) ? CAM_FLY : CAM_ORBIT;
    }

    if (in->keys[KEY_LEFT])  { g_fl.auto_slosh = FALSE; g_fl.tilt_x = m_clamp(g_fl.tilt_x - 0.65f * dt, -0.48f, 0.48f); }
    if (in->keys[KEY_RIGHT]) { g_fl.auto_slosh = FALSE; g_fl.tilt_x = m_clamp(g_fl.tilt_x + 0.65f * dt, -0.48f, 0.48f); }
    if (in->keys[KEY_UP])    { g_fl.auto_slosh = FALSE; g_fl.tilt_z = m_clamp(g_fl.tilt_z + 0.65f * dt, -0.48f, 0.48f); }
    if (in->keys[KEY_DOWN])  { g_fl.auto_slosh = FALSE; g_fl.tilt_z = m_clamp(g_fl.tilt_z - 0.65f * dt, -0.48f, 0.48f); }

    if (in->keys[KEY_U]) g_fl.desc.vorticity_eps = m_clamp(g_fl.desc.vorticity_eps + 0.20f * dt, 0.0f, 1.20f);
    if (in->keys[KEY_O]) g_fl.desc.vorticity_eps = m_clamp(g_fl.desc.vorticity_eps - 0.20f * dt, 0.0f, 1.20f);
    if (in->keys[KEY_H]) g_fl.desc.xsph_c        = m_clamp(g_fl.desc.xsph_c + 0.035f * dt, 0.0f, 0.18f);
    if (in->keys[KEY_N]) g_fl.desc.xsph_c        = m_clamp(g_fl.desc.xsph_c - 0.035f * dt, 0.0f, 0.18f);
    if (in->keys[KEY_F]) g_fl.sun_elev           = m_clamp(g_fl.sun_elev - 0.45f * dt, 0.08f, 0.92f);
    if (in->keys[KEY_G]) g_fl.sun_elev           = m_clamp(g_fl.sun_elev + 0.45f * dt, 0.08f, 0.92f);

    camera_update(&g_fl.cam, in, dt, look_enabled);

    for (u32 k = 0u; k < 256u; k++) {
        g_fl.prev_keys[k] = in->keys[k];
    }

    /* (2) Zaman ve katı cisim güncellemesi */
    f32 eff_dt = g_fl.paused ? 0.0f : (dt * g_fl.time_scale);
    if (eff_dt > 0.016f) eff_dt = 0.016f;
    g_fl.sim_time += (f64)eff_dt;
    update_rigid_bodies(&g_fl, eff_dt);

    Vec3 eye = camera_eye(&g_fl.cam);

    /* (3) GPU Akışkan Yolu (262 144 parçacık + SSFR) */
    if (engine_gpu_available()) {
        u32 vp_w = engine_gpu_viewport_width();
        u32 vp_h = engine_gpu_viewport_height();
        if (vp_w == 0u) vp_w = 1920u;
        if (vp_h == 0u) vp_h = 1080u;
        f32 aspect = (f32)vp_w / (f32)vp_h;
        Mat4 view = camera_view(&g_fl.cam);
        Mat4 proj = camera_proj(&g_fl.cam, aspect);

        GpuFluidFrameParams fp;
        __builtin_memset(&fp, 0, sizeof(fp));
        fp.view            = view;
        fp.proj            = proj;
        fp.cam_pos         = eye;
        fp.time_sec        = (f32)g_fl.sim_time;
        fp.dt              = eff_dt > 0.0f ? 0.0083f : 0.0f;
        fp.reset_particles = g_fl.reset_pending;
        fp.desc            = g_fl.desc;
        fp.scene           = g_fl.scene;

        f64 t0 = plat_time_ms();
        if (gpu_fluid_render(frame, &fp)) {
            f64 t1 = plat_time_ms();
            g_fl.reset_pending = FALSE;
            g_fl.cpu_ms        = (f32)(t1 - t0);
            g_fl.gpu_ms        = (f32)engine_gpu_frame_time_us() * 0.001f;

            /* 2B HUD katmanı için arka planı saydam temizle (alttaki WebGPU tuvali görünsün) */
            fb_clear(fb, 0u);

            g_fl.debug_buf[0]  = g_fl.gpu_ms;
            g_fl.debug_buf[1]  = g_fl.cpu_ms;
            g_fl.debug_buf[2]  = (f32)GPU_FLUID_PARTICLES;
            g_fl.debug_buf[3]  = (f32)g_fl.desc.solver_iterations;
            g_fl.debug_buf[4]  = g_fl.desc.xsph_c;
            g_fl.debug_buf[5]  = g_fl.desc.vorticity_eps;
            g_fl.debug_buf[6]  = g_fl.tilt_x;
            g_fl.debug_buf[7]  = g_fl.tilt_z;
            g_fl.debug_buf[8]  = g_fl.scene.sphere0_pos.x;
            g_fl.debug_buf[9]  = g_fl.scene.sphere0_pos.z;
            g_fl.debug_buf[10] = g_fl.sun_elev;
            g_fl.debug_buf[11] = (f32)g_fl.sim_time;
            return;
        }
    }

    /* (4) CPU Yedek Yolu (WebGPU devre dışıysa) */
    f64 t0 = plat_time_ms();
    if (!g_fl.paused) {
        step_cpu_fallback(&g_fl, eff_dt > 0.0f ? eff_dt : 0.016f);
    }
    f32 aspect = (f32)fb->w / (f32)(fb->h > 0u ? fb->h : 1u);
    Mat4 view = camera_view(&g_fl.cam);
    Mat4 proj = camera_proj(&g_fl.cam, aspect);
    render_cpu_fallback(&g_fl, fb, mat4_mul(proj, view));
    f64 t1 = plat_time_ms();
    g_fl.cpu_ms = (f32)(t1 - t0);
    g_fl.gpu_ms = 0.0f;

    g_fl.debug_buf[0]  = g_fl.gpu_ms;
    g_fl.debug_buf[1]  = g_fl.cpu_ms;
    g_fl.debug_buf[2]  = (f32)CPU_FLUID_N;
    g_fl.debug_buf[3]  = 1.0f;
    g_fl.debug_buf[4]  = g_fl.desc.xsph_c;
    g_fl.debug_buf[5]  = g_fl.desc.vorticity_eps;
    g_fl.debug_buf[6]  = g_fl.tilt_x;
    g_fl.debug_buf[7]  = g_fl.tilt_z;
    g_fl.debug_buf[8]  = g_fl.scene.sphere0_pos.x;
    g_fl.debug_buf[9]  = g_fl.scene.sphere0_pos.z;
    g_fl.debug_buf[10] = g_fl.sun_elev;
    g_fl.debug_buf[11] = (f32)g_fl.sim_time;
}

void scene_fluid_draw_hud(UiContext* ui, const Framebuffer* fb) {
    if (!g_fl.show_hud) return;

    char line[160];
    i32 y = (i32)fb->h - 24;
    ui_draw_rect(ui, 0, y - 14, (i32)fb->w, 38, FB_RGBA(10, 16, 28, 195));

    if (engine_gpu_available()) {
        u32 vp_w = engine_gpu_viewport_width();
        u32 vp_h = engine_gpu_viewport_height();
        f64 gpu_ms = (f64)engine_gpu_frame_time_us() * 0.001;
        const char* ts_tag = engine_gpu_has_timestamp() ? "ts" : "sub";
        fmt_format(line, sizeof(line),
                   "GPU PBF 256k (%u) | %ux%u | GPU %.2fms (%s) | Egim(%.2f,%.2f)%s",
                   GPU_FLUID_PARTICLES, vp_w, vp_h, gpu_ms, ts_tag,
                   (f64)g_fl.tilt_x, (f64)g_fl.tilt_z,
                   g_fl.auto_slosh ? " [Oto]" : "");
    } else {
        fmt_format(line, sizeof(line),
                   "CPU PBF Yedek (%u) | %ux%u | CPU %.2fms | Egim (%.2f,%.2f)",
                   CPU_FLUID_N, fb->w, fb->h, (f64)g_fl.cpu_ms,
                   (f64)g_fl.tilt_x, (f64)g_fl.tilt_z);
    }
    ui_draw_text(ui, 5, y - 11, line, FB_RGBA(110, 230, 255, 255), 1);

    fmt_format(line, sizeof(line),
               "[Oklar/B]Kova [U/O]Vort %.2f [H/N]XSPH %.3f [F/G]Gunes [Spc]%s [C]%s",
               (f64)g_fl.desc.vorticity_eps, (f64)g_fl.desc.xsph_c,
               g_fl.paused ? "Dur" : "Akar",
               (g_fl.cam.mode == CAM_ORBIT) ? "Orb" : "Ucus");
    ui_draw_text(ui, 5, y + 1, line, FB_RGBA(225, 235, 248, 255), 1);
}

void scene_fluid_set_hud(b32 show) {
    g_fl.show_hud = show ? TRUE : FALSE;
}

const f32* scene_fluid_debug(void) {
    return g_fl.debug_buf;
}

void scene_fluid_debug_cmd(u32 cmd, f32 a, f32 b, f32 c) {
    if (cmd == 0u) {
        reset_fluid_scene(&g_fl);
    } else if (cmd == 1u) {
        g_fl.sim_time   = (f64)a;
        g_fl.tilt_x     = m_clamp(b, -0.48f, 0.48f);
        g_fl.tilt_z     = m_clamp(c, -0.48f, 0.48f);
        g_fl.auto_slosh = FALSE;
        update_rigid_bodies(&g_fl, 0.0f);
    } else if (cmd == 2u) {
        g_fl.cam.yaw   = a;
        g_fl.cam.pitch = b;
        g_fl.cam.dist  = c;
    } else if (cmd == 3u) {
        g_fl.desc.vorticity_eps = m_clamp(a, 0.0f, 1.20f);
        g_fl.desc.xsph_c        = m_clamp(b, 0.0f, 0.18f);
        g_fl.sun_elev           = m_clamp(c, 0.08f, 0.92f);
    }
}
