/* app/shaders.c — Matoki shader yöneticisi ve C referans yedekleri implementasyonu.
 * Açılışta g_embedded_shaders dizisini derler, derleme süresini ve boyutları ölçer,
 * D hattının blend füzyonu (fuse_blend) desteğini otomatik algılayarak varyant seçer.
 */
#include "app/shaders.h"
#include "core/embed.h"
#include "core/math.h"
#include "core/log.h"
#include "plat/platform.h"
#include "raster/ref_shaders.h"
#include "jit/jit.h"

static b32 str_eq_name(const char* a, const char* b) {
    if (!a || !b) return FALSE;
    while (*a && *b) {
        if (*a != *b) break;
        a++;
        b++;
    }
    if (*a == '\0' && *b == '\0') return TRUE;
    /* "sprite" ile "sprite.mtk" eşleşmesini destekle */
    if (*a == '\0' && b[0] == '.' && b[1] == 'm' && b[2] == 't' && b[3] == 'k' && b[4] == '\0') return TRUE;
    if (*b == '\0' && a[0] == '.' && a[1] == 'm' && a[2] == 't' && a[3] == 'k' && a[4] == '\0') return TRUE;
    return FALSE;
}

static void copy_name(char dst[32], const char* src) {
    u32 i = 0;
    if (src) {
        while (src[i] && i < 31) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ---------- C Referans Shader'ları (plasma ve branchy) ---------- */

static inline f32 plasma_wave(f32 px, f32 py, f32 t) {
    f32 v1 = m_sin(px * 10.0f + t);
    f32 v2 = m_cos(py * 10.0f - t * 0.5f);
    f32 v3 = m_sin((px + py) * 7.0f + t * 0.7f);
    f32 cx = px - 0.5f;
    f32 cy = py - 0.5f;
    f32 d  = m_sqrt(cx * cx + cy * cy);
    f32 v4 = m_cos(d * 14.0f - t * 1.3f);
    return (v1 + v2 + v3 + v4) * 0.25f;
}

void ref_shade_plasma(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;

    f32 time = 0.0f;
    if (batch->uniforms != NULL) {
        time = *(const f32*)batch->uniforms;
    }

    u32 total_lanes = count * 4u;
    const f32* var_u = batch->varyings + 0u * total_lanes;
    const f32* var_v = batch->varyings + 1u * total_lanes;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    const f32 pi = 3.14159265f;
    for (u32 i = 0; i < total_lanes; i++) {
        f32 w = plasma_wave(var_u[i], var_v[i], time);
        f32 w_pi = w * pi;
        out_r[i] = m_sin(w_pi) * 0.5f + 0.5f;
        out_g[i] = m_cos(w_pi + 1.0f) * 0.5f + 0.5f;
        out_b[i] = m_sin(w_pi + 2.0f) * 0.5f + 0.5f;
        out_a[i] = 1.0f;
    }
}

void ref_shade_branchy(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;

    f32 threshold = 1.4f;
    if (batch->uniforms != NULL) {
        threshold = *(const f32*)batch->uniforms;
    }

    u32 total_lanes = count * 4u;
    const f32* var_u = batch->varyings + 0u * total_lanes;
    const f32* var_v = batch->varyings + 1u * total_lanes;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    for (u32 q = 0; q < count; q++) {
        u32 mask = batch->quads[q].mask;
        u32 base = q * 4u;
        for (u32 lane = 0; lane < 4; lane++) {
            u32 idx = base + lane;
            f32 ux = var_u[idx];
            f32 uy = var_v[idx];
            f32 r = 0.1f, g = 0.2f, b = 0.3f, a = 1.0f;

            if (ux > 0.5f) {
                if (uy > 0.5f) {
                    if (ux + uy > threshold) {
                        mask &= ~(1u << lane);
                    }
                    r = 1.0f; g = 0.0f; b = 0.0f; a = 1.0f;
                } else {
                    g = ux;
                }
            } else {
                if (uy > 0.5f) {
                    b = uy;
                } else {
                    r = ux; g = uy; b = 0.5f; a = 1.0f;
                }
            }

            out_r[idx] = r;
            out_g[idx] = g;
            out_b[idx] = b;
            out_a[idx] = a;
        }
        batch->quads[q].mask = mask;
    }
}

/* lit3d.mtk C referansı. Uniform yerleşimi: light_dir@0 (vec3), base@16, fog@32 (vec4).
 * Varying: uv(0,1) nrm(2,3,4) depth(5). Transandantal yok; normalize = x / sqrt(dot).
 * Doku çalışma anı bayrağında TEX_FLAG_MIPMAP varsa seviye quad türevlerinden seçilir. */
void ref_shade_lit3d(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    const Texture* tex = &batch->textures[0];
    i32 level = 0;
    for (u32 i = 0; i < n; i++) {
        if ((i & 3u) == 0u && (tex->flags & TEX_FLAG_MIPMAP)) {
            level = ref_tex_mip_level(tex, &v[i], &v[n + i]);
        }
        f32 nx = v[2u * n + i], ny = v[3u * n + i], nz = v[4u * n + i];
        f32 inv = 1.0f / m_sqrt(nx * nx + ny * ny + nz * nz);
        f32 d = m_max((nx * u[0] + ny * u[1] + nz * u[2]) * inv, 0.0f);
        f32 c[4];
        ref_sample_tex_level(tex, v[i], v[n + i], level, c);
        f32 l = 0.3f + 0.7f * d;
        f32 f = m_clamp(v[5u * n + i] * u[11], 0.0f, 1.0f);
        for (u32 k = 0; k < 3; k++) {
            f32 lit = c[k] * u[4 + k] * l;
            o[k * n + i] = lit + (u[8 + k] - lit) * f;
        }
        o[3u * n + i] = c[3] * u[7];
    }
}

/* pbr3d.mtk C referansı (shader ile aynı formüller). Uniform yerleşimi: sun_dir@0 sun_col@16 sky_col@32
 * gnd_col@48 eye@64 base@80 mat@96 fog@112; dokular: 0 albedo, 1 gölge (texture2d_f32). Varying: uv nrm depth wpos spos. */
static f32 pbr_sample_depth(const Texture* sht, f32 u, f32 v) {
    i32 wi = (i32)sht->w;
    i32 hi = (i32)sht->h;
    i32 tx = (i32)m_floor(u * (f32)wi);
    i32 ty = (i32)m_floor(v * (f32)hi);
    if (sht->flags & TEX_FLAG_CLAMP) {
        tx = CLAMP(tx, 0, wi - 1);
        ty = CLAMP(ty, 0, hi - 1);
    } else {
        tx &= (i32)sht->w_mask;
        ty &= (i32)sht->h_mask;
    }
    const f32* depth_px = (const f32*)sht->px;
    return depth_px[((u32)ty << sht->log2_w) + (u32)tx];
}

void ref_shade_pbr3d(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    const Texture* tex = &batch->textures[0];
    const Texture* sht = &batch->textures[1];
    const f32 k_pi = 3.14159265f;
    i32 level = 0;
    for (u32 i = 0; i < n; i++) {
        if ((i & 3u) == 0u && (tex->flags & TEX_FLAG_MIPMAP)) {
            level = ref_tex_mip_level(tex, &v[i], &v[n + i]);
        }
        f32 tc[4];
        ref_sample_tex_level(tex, v[i], v[n + i], level, tc);
        f32 alb[3], f0[3];
        for (u32 k = 0; k < 3; k++) { tc[k] *= u[20 + k]; alb[k] = tc[k] * tc[k]; }
        tc[3] *= u[23];
        f32 metal = u[24], rough = m_max(u[25], 0.04f);

        f32 nx = v[2u * n + i], ny = v[3u * n + i], nz = v[4u * n + i];
        f32 il = 1.0f / m_sqrt(nx * nx + ny * ny + nz * nz);
        nx *= il; ny *= il; nz *= il;
        f32 vx = u[16] - v[6u * n + i], vy = u[17] - v[7u * n + i], vz = u[18] - v[8u * n + i];
        il = 1.0f / m_sqrt(vx * vx + vy * vy + vz * vz);
        vx *= il; vy *= il; vz *= il;
        f32 hx = vx + u[0], hy = vy + u[1], hz = vz + u[2];
        il = 1.0f / m_sqrt(hx * hx + hy * hy + hz * hz);
        hx *= il; hy *= il; hz *= il;
        f32 ndl = m_max(nx * u[0] + ny * u[1] + nz * u[2], 0.0f);
        f32 ndv = m_max(nx * vx + ny * vy + nz * vz, 0.0001f);
        f32 ndh = m_max(nx * hx + ny * hy + nz * hz, 0.0f);
        f32 vdh = m_max(vx * hx + vy * hy + vz * hz, 0.0f);

        f32 sz = u[27], dt = 1.0f / sz;
        f32 stx = v[9u * n + i] * sz - 0.5f, sty = v[10u * n + i] * sz - 0.5f;
        f32 flx = m_floor(stx), fly = m_floor(sty);
        f32 frx = stx - flx, fry = sty - fly;
        f32 p0x = (flx + 0.5f) * dt, p0y = (fly + 0.5f) * dt;
        f32 z = v[11u * n + i] + u[26];
        f32 s4[4];
        for (u32 k = 0; k < 4; k++) {
            f32 d = pbr_sample_depth(sht, p0x + (f32)(k & 1u) * dt, p0y + (f32)(k >> 1) * dt);
            s4[k] = (z >= d) ? 1.0f : 0.0f;
        }
        f32 a0 = s4[0] + (s4[1] - s4[0]) * frx, a1 = s4[2] + (s4[3] - s4[2]) * frx;
        f32 sh = a0 + (a1 - a0) * fry;

        f32 a = rough * rough, a2 = a * a;
        f32 dd = ndh * ndh * (a2 - 1.0f) + 1.0f;
        f32 dist = a2 / (k_pi * dd * dd);
        f32 kk = (rough + 1.0f) * (rough + 1.0f) * 0.125f;
        f32 vis = 0.25f / ((ndl * (1.0f - kk) + kk) * (ndv * (1.0f - kk) + kk));
        f32 x = 1.0f - vdh, x2 = x * x, fw = x2 * x2 * x;
        f32 hemi_t = ny * 0.5f + 0.5f;
        f32 fogf = m_clamp(v[5u * n + i] * u[31], 0.0f, 1.0f);
        for (u32 k = 0; k < 3; k++) {
            f0[k] = 0.04f + (alb[k] - 0.04f) * metal;
            f32 fres = f0[k] + (1.0f - f0[k]) * fw;
            f32 kd = (1.0f - fres) * (1.0f - metal);
            f32 direct = (kd * alb[k] * (1.0f / k_pi) + fres * (dist * vis)) * u[4 + k] * (ndl * sh);
            f32 hemi = u[12 + k] + (u[8 + k] - u[12 + k]) * hemi_t;
            f32 amb = hemi * (alb[k] * (1.0f - metal) + f0[k] * ((1.0f - rough) * 0.5f + 0.25f));
            f32 c = direct + amb;
            c = c + (u[28 + k] - c) * fogf;
            f32 tm = (c * (c * 2.51f + 0.03f)) / (c * (c * 2.43f + 0.59f) + 0.14f);
            o[k * n + i] = m_sqrt(m_clamp(tm, 0.0f, 1.0f));
        }
        o[3u * n + i] = tc[3];
    }
}

/* unlit3d.mtk C referansı. Uniform: color@0, fog@16. Varying: nrm(3) depth(1). */
void ref_shade_unlit3d(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    for (u32 i = 0; i < n; i++) {
        f32 nx = v[i], ny = v[n + i], nz = v[2u * n + i];
        f32 ny_n = ny / m_sqrt(nx * nx + ny * ny + nz * nz);
        f32 rim = 1.0f - m_abs(ny_n) * 0.35f;
        f32 f = m_clamp(v[3u * n + i] * u[7] * 0.6f, 0.0f, 1.0f);
        for (u32 k = 0; k < 3; k++) {
            f32 c = m_min(u[k] * rim, 1.0f);
            o[k * n + i] = c + (u[4 + k] - c) * f;
        }
        o[3u * n + i] = u[3];
    }
}

/* dungeon3d.mtk C referansı. Uniform: amb@0 base@16 fog@32, ışık i: konum@48+32i, renk@64+32i;
 * doku 0 albedo (en yakın + mipmap). Varying: uv nrm depth wpos. */
void ref_shade_dungeon3d(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    const Texture* tex = &batch->textures[0];
    i32 level = 0;
    for (u32 i = 0; i < n; i++) {
        if ((i & 3u) == 0u && (tex->flags & TEX_FLAG_MIPMAP)) {
            level = ref_tex_mip_level(tex, &v[i], &v[n + i]);
        }
        f32 tc[4];
        ref_sample_tex_level(tex, v[i], v[n + i], level, tc);
        f32 nx = v[2u * n + i], ny = v[3u * n + i], nz = v[4u * n + i];
        f32 il = 1.0f / m_sqrt(nx * nx + ny * ny + nz * nz);
        nx *= il; ny *= il; nz *= il;
        f32 px = v[6u * n + i], py = v[7u * n + i], pz = v[8u * n + i];
        f32 li[3] = { u[0], u[1], u[2] };
        for (u32 l = 0; l < 4u; l++) {
            const f32* lp = &u[12u + l * 8u];
            const f32* lc = lp + 4;
            f32 dx = lp[0] - px, dy = lp[1] - py, dz = lp[2] - pz;
            f32 d2 = dx * dx + dy * dy + dz * dz;
            f32 w = m_clamp(1.0f - d2 * lp[3], 0.0f, 1.0f);
            f32 ndl = m_clamp((nx * dx + ny * dy + nz * dz) * (1.0f / m_sqrt(d2 + 0.0001f)) * 0.75f + 0.25f, 0.0f, 1.0f);
            f32 a = w * w * ndl / (1.0f + d2 * 0.25f);
            for (u32 k = 0; k < 3; k++) li[k] += lc[k] * a;
        }
        f32 f = m_clamp(v[5u * n + i] * u[11], 0.0f, 1.0f);
        for (u32 k = 0; k < 3; k++) {
            f32 t = tc[k] * u[4 + k];
            f32 c = t * t * li[k];
            c = c + (u[8 + k] - c) * f;
            o[k * n + i] = m_sqrt(m_clamp(c, 0.0f, 1.0f));
        }
        o[3u * n + i] = tc[3] * u[7];
    }
}

/* particle3d.mtk C referansı. Uniform: fog@0 (vec4). Varying: uv(2) v_color(4) depth(1). */
static void ref_shade_particle3d(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    for (u32 i = 0; i < n; i++) {
        f32 dx = v[0u * n + i] * 2.0f - 1.0f;
        f32 dy = v[1u * n + i] * 2.0f - 1.0f;
        f32 t = m_clamp(1.0f - (dx * dx + dy * dy), 0.0f, 1.0f);
        f32 w = t * t;
        f32 f = m_clamp(v[6u * n + i] * u[3] * 0.6f, 0.0f, 1.0f);
        for (u32 k = 0; k < 3; k++) {
            f32 c = v[(2u + k) * n + i];
            o[k * n + i] = c + (u[k] - c) * f;
        }
        o[3u * n + i] = v[5u * n + i] * w;
    }
}

static inline f32 mtk_smoothstep(f32 e0, f32 e1, f32 x) {
    f32 raw = (x - e0) / (e1 - e0);
    f32 t   = m_clamp(raw, 0.0f, 1.0f);
    return (t * t) * (3.0f - (2.0f * t));
}

static inline void mtk_sample_bilerp_level(const Texture* tex, f32 u, f32 v, i32 level, f32 out_rgba[4]) {
    if ((tex->flags & TEX_FLAG_BILINEAR) == 0u) {
        ref_sample_tex_level(tex, u, v, level, out_rgba);
        return;
    }
    Texture t = ref_tex_level_view(tex, level);
    f32 wf   = (f32)(i32)t.w;
    f32 hf   = (f32)(i32)t.h;
    i32 w_m1 = (i32)t.w - 1;
    i32 h_m1 = (i32)t.h - 1;
    f32 fu   = u * wf - 0.5f;
    f32 fv   = v * hf - 0.5f;
    f32 x0_f = m_floor(fu);
    f32 y0_f = m_floor(fv);
    f32 ax   = fu - x0_f;
    f32 ay   = fv - y0_f;
    i32 x0_i = (i32)x0_f;
    i32 y0_i = (i32)y0_f;
    i32 x0   = x0_i & w_m1;
    i32 y0   = y0_i & h_m1;
    i32 x1   = (x0_i + 1) & w_m1;
    i32 y1   = (y0_i + 1) & h_m1;
    u32 p00  = t.px[(u32)(y0 * (i32)t.w + x0)];
    u32 p10  = t.px[(u32)(y0 * (i32)t.w + x1)];
    u32 p01  = t.px[(u32)(y1 * (i32)t.w + x0)];
    u32 p11  = t.px[(u32)(y1 * (i32)t.w + x1)];
    const f32 inv255 = 1.0f / 255.0f;
    for (u32 c = 0; c < 4u; c++) {
        u32 sh  = c * 8u;
        f32 c00 = (f32)(i32)((p00 >> sh) & 0xFFu);
        f32 c10 = (f32)(i32)((p10 >> sh) & 0xFFu);
        f32 c01 = (f32)(i32)((p01 >> sh) & 0xFFu);
        f32 c11 = (f32)(i32)((p11 >> sh) & 0xFFu);
        f32 top = c00 + (c10 - c00) * ax;
        f32 bot = c01 + (c11 - c01) * ax;
        f32 lrp = top + (bot - top) * ay;
        out_rgba[c] = lrp * inv255;
    }
}

/* sky.mtk C referansı. Uniform: sun_dir@0 sun_col@16 sky_zenith@32 sky_horizon@48 params@64.
 * Varying: nrm(3). */
void ref_shade_sky(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    const f32* sun_dir     = &u[0];
    const f32* sun_col     = &u[4];
    const f32* sky_zenith  = &u[8];
    const f32* sky_horizon = &u[12];
    const f32* params      = &u[16];

    for (u32 i = 0; i < n; i++) {
        f32 dx = v[0u * n + i], dy = v[1u * n + i], dz = v[2u * n + i];
        f32 il = 1.0f / m_sqrt(dx * dx + dy * dy + dz * dz);
        dx *= il; dy *= il; dz *= il;

        f32 y   = m_clamp(dy, 0.0f, 1.0f);
        f32 iy  = 1.0f - m_clamp(y * 1.85f, 0.0f, 1.0f);
        f32 iy2 = iy * iy;
        f32 hz  = iy2 * iy;

        f32 mu   = m_max(dx * sun_dir[0] + dy * sun_dir[1] + dz * sun_dir[2], 0.0f);
        f32 mu2  = mu * mu;
        f32 mu4  = mu2 * mu2;
        f32 mu8  = mu4 * mu4;
        f32 mu16 = mu8 * mu8;
        f32 mu64 = mu16 * mu16;

        f32 warm_col[3] = { 1.0f * params[3], 0.48f * params[3], 0.18f * params[3] };
        f32 disk_k = mtk_smoothstep(0.9978f, 0.9994f, mu) * (14.0f * params[2]);

        f32 py     = m_max(dy, 0.08f);
        f32 inv_py = 1.8f / py;
        f32 uvc_x  = dx * inv_py + params[0] * 0.015f;
        f32 uvc_y  = dz * inv_py + params[0] * 0.008f;

        f32 a1_x = uvc_x * 0.35f + 0.17f;
        f32 a1_y = uvc_y * 0.35f + 0.43f;
        f32 p1_x = m_abs((a1_x - m_floor(a1_x)) * 2.0f - 1.0f);
        f32 p1_y = m_abs((a1_y - m_floor(a1_y)) * 2.0f - 1.0f);

        f32 ruv_x = uvc_x * 0.62f - uvc_y * 0.48f;
        f32 ruv_y = uvc_x * 0.48f + uvc_y * 0.62f;
        f32 a2_x  = ruv_x + p1_y * 0.35f;
        f32 a2_y  = ruv_y + p1_x * 0.35f;
        f32 p2_x  = m_abs((a2_x - m_floor(a2_x)) * 2.0f - 1.0f);
        f32 p2_y  = m_abs((a2_y - m_floor(a2_y)) * 2.0f - 1.0f);

        f32 n_val      = (p1_x * p1_y + p2_x * 0.6f + p2_y * 0.4f) * 0.5f;
        f32 c_vis      = mtk_smoothstep(0.42f, 0.80f, n_val) * mtk_smoothstep(0.04f, 0.26f, y);
        f32 cloud_mask = c_vis * (1.0f - y * 0.55f) * params[1];
        f32 cl_t       = 0.35f + 0.65f * mu4;
        f32 below      = mtk_smoothstep(-0.06f, 0.0f, dy);
        f32 white_c[3] = { 0.95f, 0.92f, 0.88f };

        for (u32 k = 0; k < 3u; k++) {
            f32 base_sky  = sky_zenith[k] + (sky_horizon[k] - sky_zenith[k]) * hz;
            f32 mie_glow  = (warm_col[k] * (mu4 * hz * 0.55f) + sun_col[k] * (mu16 * 0.18f + mu64 * 0.95f)) * params[2];
            f32 sun_disk  = sun_col[k] * disk_k;
            f32 cl_a      = sky_horizon[k] * 1.10f;
            f32 cl_b      = sun_col[k] * 0.55f + white_c[k];
            f32 cloud_lit = cl_a + (cl_b - cl_a) * cl_t;
            f32 under_col = sky_horizon[k] * 0.55f;
            f32 bm        = base_sky + mie_glow;
            f32 sky_above = (bm + (cloud_lit - bm) * cloud_mask) + sun_disk;
            f32 c         = under_col + (sky_above - under_col) * below;
            f32 tm        = (c * (c * 2.51f + 0.03f)) / (c * (c * 2.43f + 0.59f) + 0.14f);
            o[k * n + i]  = m_sqrt(m_clamp(tm, 0.0f, 1.0f));
        }
        o[3u * n + i] = 1.0f;
    }
}

/* ocean.mtk C referansı. Uniform: sun_dir@0 sun_col@16 sky_zenith@32 sky_horizon@48
 * sea_deep@64 sea_shallow@80 eye@96 scales@112 params@128; dokular: map0, map1, map2.
 * Varying: uv(0,1) nrm(2,3,4) depth(5) wpos(6,7,8). */
void ref_shade_ocean(ShadeBatch* batch) {
    u32 n = batch->count * 4u;
    const f32* u = (const f32*)batch->uniforms;
    const f32* v = batch->varyings;
    f32* o = batch->out_rgba;
    const Texture* map0 = &batch->textures[0];
    const Texture* map1 = &batch->textures[1];
    const Texture* map2 = &batch->textures[2];

    const f32* sun_dir     = &u[0];
    const f32* sun_col     = &u[4];
    const f32* sky_zenith  = &u[8];
    const f32* sky_horizon = &u[12];
    const f32* sea_deep    = &u[16];
    const f32* sea_shallow = &u[20];
    const f32* eye         = &u[24];
    const f32* scales      = &u[28];
    const f32* params      = &u[32];

    i32 lvl0 = 0, lvl1 = 0, lvl2 = 0;
    for (u32 i = 0; i < n; i++) {
        if ((i & 3u) == 0u) {
            f32 u0_q[4] = { v[i], v[i + 1u], v[i + 2u], v[i + 3u] };
            f32 v0_q[4] = { v[n + i], v[n + i + 1u], v[n + i + 2u], v[n + i + 3u] };
            f32 u1_q[4] = { u0_q[0] * scales[0], u0_q[1] * scales[0], u0_q[2] * scales[0], u0_q[3] * scales[0] };
            f32 v1_q[4] = { v0_q[0] * scales[0], v0_q[1] * scales[0], v0_q[2] * scales[0], v0_q[3] * scales[0] };
            f32 u2_q[4] = { u0_q[0] * scales[1], u0_q[1] * scales[1], u0_q[2] * scales[1], u0_q[3] * scales[1] };
            f32 v2_q[4] = { v0_q[0] * scales[1], v0_q[1] * scales[1], v0_q[2] * scales[1], v0_q[3] * scales[1] };
            lvl0 = (map0->flags & TEX_FLAG_MIPMAP) ? ref_tex_mip_level(map0, u0_q, v0_q) : 0;
            lvl1 = (map1->flags & TEX_FLAG_MIPMAP) ? ref_tex_mip_level(map1, u1_q, v1_q) : 0;
            lvl2 = (map2->flags & TEX_FLAG_MIPMAP) ? ref_tex_mip_level(map2, u2_q, v2_q) : 0;
        }

        f32 uv0_x = v[i], uv0_y = v[n + i];
        f32 uv1_x = uv0_x * scales[0], uv1_y = uv0_y * scales[0];
        f32 uv2_x = uv0_x * scales[1], uv2_y = uv0_y * scales[1];

        f32 t0[4], t1[4], t2[4];
        mtk_sample_bilerp_level(map0, uv0_x, uv0_y, lvl0, t0);
        mtk_sample_bilerp_level(map1, uv1_x, uv1_y, lvl1, t1);
        mtk_sample_bilerp_level(map2, uv2_x, uv2_y, lvl2, t2);

        f32 depth = v[5u * n + i];
        f32 w1    = m_clamp(1.0f - depth * 0.0035f, 0.25f, 1.0f);
        f32 w2    = m_clamp(1.0f - depth * 0.012f, 0.10f, 1.0f);

        const f32 zc = 0.50196078f;
        f32 d0_x = t0[0] - zc, d0_y = t0[1] - zc;
        f32 d1_x = (t1[0] - zc) * w1, d1_y = (t1[1] - zc) * w1;
        f32 d2_x = (t2[0] - zc) * w2, d2_y = (t2[1] - zc) * w2;
        f32 sl_x = (d0_x + d1_x + d2_x) * scales[2];
        f32 sl_y = (d0_y + d1_y + d2_y) * scales[2];

        f32 vnx = v[2u * n + i], vny = v[3u * n + i], vnz = v[4u * n + i];
        f32 il  = 1.0f / m_sqrt(vnx * vnx + vny * vny + vnz * vnz);
        vnx *= il; vny *= il; vnz *= il;

        f32 nx = vnx * 0.25f - sl_x;
        f32 ny = 1.0f;
        f32 nz = vnz * 0.25f - sl_y;
        il = 1.0f / m_sqrt(nx * nx + ny * ny + nz * nz);
        nx *= il; ny *= il; nz *= il;

        f32 wx = v[6u * n + i], wy = v[7u * n + i], wz = v[8u * n + i];
        f32 vx = eye[0] - wx, vy = eye[1] - wy, vz = eye[2] - wz;
        il = 1.0f / m_sqrt(vx * vx + vy * vy + vz * vz);
        vx *= il; vy *= il; vz *= il;

        f32 dot_nv  = nx * vx + ny * vy + nz * vz;
        f32 ndv     = m_max(dot_nv, 0.001f);
        f32 fx      = m_clamp(1.0f - ndv, 0.0f, 1.0f);
        f32 fx2     = fx * fx;
        f32 fx5     = fx2 * fx2 * fx;
        f32 fresnel = 0.02f + 0.98f * fx5;

        f32 two_nv = 2.0f * dot_nv;
        f32 rrx    = nx * two_nv - vx;
        f32 rry    = m_max(ny * two_nv - vy, 0.01f);
        f32 rrz    = nz * two_nv - vz;
        il = 1.0f / m_sqrt(rrx * rrx + rry * rry + rrz * rrz);
        f32 rdx = rrx * il, rdy = rry * il, rdz = rrz * il;

        f32 ry   = m_clamp(rdy, 0.0f, 1.0f);
        f32 iry  = 1.0f - m_clamp(ry * 1.85f, 0.0f, 1.0f);
        f32 r_hz = iry * iry * iry;
        f32 r_mu = m_max(rdx * sun_dir[0] + rdy * sun_dir[1] + rdz * sun_dir[2], 0.0f);
        f32 r_mu2 = r_mu * r_mu;
        f32 r_mu4 = r_mu2 * r_mu2;
        f32 r_mu8 = r_mu4 * r_mu4;
        f32 r_mu16 = r_mu8 * r_mu8;
        f32 warm_col[3] = { 1.0f * params[3], 0.48f * params[3], 0.18f * params[3] };

        f32 hx = vx + sun_dir[0], hy = vy + sun_dir[1], hz_v = vz + sun_dir[2];
        il = 1.0f / m_sqrt(hx * hx + hy * hy + hz_v * hz_v);
        hx *= il; hy *= il; hz_v *= il;

        f32 ndh   = m_max(nx * hx + ny * hy + nz * hz_v, 0.0f);
        f32 ndl   = m_max(nx * sun_dir[0] + ny * sun_dir[1] + nz * sun_dir[2], 0.0f);
        f32 nh2   = ndh * ndh;
        f32 nh4   = nh2 * nh2;
        f32 nh8   = nh4 * nh4;
        f32 nh16  = nh8 * nh8;
        f32 nh64  = nh16 * nh16;
        f32 nh256 = nh64 * nh64;
        f32 glare_f = (nh16 * 0.10f + nh64 * 0.55f + nh256 * 3.4f) * (0.25f + 0.75f * fresnel) * params[2];
        f32 glare_k = glare_f * (0.2f + 0.8f * mtk_smoothstep(-0.05f, 0.25f, ndl));

        f32 up_f     = m_clamp(1.0f - ndv * 0.65f, 0.0f, 1.0f);
        f32 sun_lift = 0.35f + 0.65f * m_max(sun_dir[1], 0.15f);
        f32 wb_t     = up_f * 0.40f;

        f32 local_h  = m_max((t0[2] - 0.52f) * 0.45f + (t1[2] - 0.48f) * 1.15f + (t2[2] - 0.48f) * 0.65f, 0.0f);
        f32 dist_sss = m_clamp(1.0f - depth * 0.0085f, 0.0f, 1.0f);
        f32 crest_h  = m_clamp((local_h * 0.85f + (1.0f - ny) * 1.25f) * dist_sss, 0.0f, 1.4f);
        f32 bdx      = sun_dir[0] + nx * 0.45f;
        f32 bdy      = sun_dir[1] + ny * 0.45f;
        f32 bdz      = sun_dir[2] + nz * 0.45f;
        il = 1.0f / m_sqrt(bdx * bdx + bdy * bdy + bdz * bdz);
        bdx *= il; bdy *= il; bdz *= il;

        f32 back_lit   = m_max(vx * (0.0f - bdx) + vy * (0.0f - bdy) + vz * (0.0f - bdz), 0.0f);
        f32 bl2        = back_lit * back_lit;
        f32 front_sss  = m_max(ndl * 0.5f + 0.5f, 0.0f) * (1.0f - ndv) * 0.45f;
        f32 sss_amount = crest_h * (bl2 * 1.45f + front_sss) * params[0];
        f32 sss_tint[3] = { 0.12f, 0.42f, 0.38f };

        f32 raw_foam    = t0[3] * 0.85f + t1[3] * 0.80f + t2[3] * (0.60f * w1);
        f32 foam_detail = 0.60f + 0.40f * (t1[2] * 0.5f + t2[2] * 0.5f);
        f32 foam_mask   = m_clamp(mtk_smoothstep(0.08f, 0.58f, raw_foam * foam_detail) * params[1], 0.0f, 0.94f);
        f32 foam_sun_k  = m_max(sun_dir[1], 0.15f) * 0.26f + ndl * 0.18f;

        f32 fd    = m_clamp(depth * scales[3], 0.0f, 1.0f);
        f32 fog_f = fd * (2.0f - fd);
        f32 v_sun = m_max((0.0f - vx) * sun_dir[0] + (0.0f - vy) * sun_dir[1] + (0.0f - vz) * sun_dir[2], 0.0f);
        f32 vs2   = v_sun * v_sun * 0.35f;

        for (u32 k = 0; k < 3u; k++) {
            f32 refl_sky = (sky_zenith[k] + (sky_horizon[k] - sky_zenith[k]) * r_hz)
                         + warm_col[k] * (r_mu4 * r_hz * 0.45f)
                         + sun_col[k] * (r_mu16 * 0.28f);
            f32 sun_glare  = sun_col[k] * glare_k;
            f32 sh_b       = sea_shallow[k] * 0.38f;
            f32 water_body = (sea_deep[k] + (sh_b - sea_deep[k]) * wb_t) * sun_lift;
            f32 sss_col    = (sea_shallow[k] * (sun_col[k] * 0.35f + sss_tint[k])) * sss_amount;
            f32 refracted  = water_body + sss_col;
            f32 surf_col   = (refracted + (refl_sky - refracted) * fresnel) + sun_glare;
            f32 foam_lit   = sky_horizon[k] * 0.55f + sun_col[k] * foam_sun_k;
            f32 with_foam  = surf_col + (foam_lit - surf_col) * foam_mask;
            f32 fog_col    = sky_horizon[k] + warm_col[k] * vs2;
            f32 c          = with_foam + (fog_col - with_foam) * fog_f;
            f32 tm         = (c * (c * 2.51f + 0.03f)) / (c * (c * 2.43f + 0.59f) + 0.14f);
            o[k * n + i]   = m_sqrt(m_clamp(tm, 0.0f, 1.0f));
        }
        o[3u * n + i] = 1.0f;
    }
}

static ShadeFn default_ref_for_name(const char* name) {
    if (str_eq_name(name, "sprite.mtk"))     return REF_SHADE_SPRITE_FN;
    if (str_eq_name(name, "solid.mtk"))      return REF_SHADE_SOLID_FN;
    if (str_eq_name(name, "vcolor.mtk"))     return REF_SHADE_VCOLOR_FN;
    if (str_eq_name(name, "plasma.mtk"))     return ref_shade_plasma;
    if (str_eq_name(name, "branchy.mtk"))    return ref_shade_branchy;
    if (str_eq_name(name, "lit3d.mtk"))      return ref_shade_lit3d;
    if (str_eq_name(name, "pbr3d.mtk"))      return ref_shade_pbr3d;
    if (str_eq_name(name, "unlit3d.mtk"))    return ref_shade_unlit3d;
    if (str_eq_name(name, "dungeon3d.mtk"))  return ref_shade_dungeon3d;
    if (str_eq_name(name, "particle3d.mtk")) return ref_shade_particle3d;
    if (str_eq_name(name, "ocean.mtk"))      return ref_shade_ocean;
    if (str_eq_name(name, "sky.mtk"))        return ref_shade_sky;
    return REF_SHADE_SOLID_FN;
}

b32 shaders_compile_one(ManagedShader* out, const char* name,
                        const char* src, u32 len,
                        ShadeFn ref_fallback, Arena* scratch,
                        MtkDiag* diag) {
    ASSERT(out != NULL && scratch != NULL);
    __builtin_memset(out, 0, sizeof(*out));
    copy_name(out->name, name ? name : "<shader>");
    out->ref_fn = ref_fallback ? ref_fallback : default_ref_for_name(name);

    usize mark = arena_mark(scratch);
    MtkDiag local_diag;
    if (!diag) diag = &local_diag;
    mtk_diag_init(diag);

    f64 t0 = plat_time_ms();

    IrFunc ir;
    __builtin_memset(&ir, 0, sizeof(ir));
    if (!mtk_compile_ir(src, len, out->name, scratch, &ir, &out->layout, diag)) {
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    ir_optimize(&ir);

    /* Aktif IR komut sayısını hesapla (NOP hariç) */
    u32 live_insts = 0;
    for (u32 i = 0; i < ir.count; i++) {
        if (ir.insts[i].op != IR_NOP) {
            live_insts++;
        }
    }
    out->ir_inst_count = live_insts;

    u32 wasm_cap = 262144;
    u8* wasm_buf = ARENA_PUSH_ARRAY(scratch, u8, wasm_cap);

    MtkCodegenOpts opts;
    opts.own_memory    = FALSE;
#if defined(PLAT_SHARED_MEMORY) && PLAT_SHARED_MEMORY
    opts.shared_memory = TRUE;
#else
    opts.shared_memory = FALSE;
#endif
    opts.fuse_blend    = MTK_FUSE_NONE;

    char err[256];
    err[0] = '\0';
    u32 wasm_len = mtk_codegen(&ir, &opts, scratch, wasm_buf, wasm_cap, err, sizeof(err));
    if (wasm_len == 0) {
        mtk_diag_error(diag, out->name, src, len, 1, 1, "codegen hatasi: %s", err);
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    out->wasm_bytes = wasm_len;
    ShadeFn fn_none = jit_load_shade(wasm_buf, wasm_len);
    f64 t1 = plat_time_ms();
    out->compile_ms = t1 - t0;

    if (fn_none == NULL) {
        mtk_diag_error(diag, out->name, src, len, 1, 1, "JIT yukleme basarisiz (plat_jit_load)");
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    out->jit_fn[0] = fn_none;
    out->jit_fn[1] = fn_none;
    out->jit_fn[2] = fn_none;
    out->jit_fn[3] = fn_none;
    out->ok        = TRUE;

    /* D hattının blend füzyonu (MTK_FUSE_REPLACE/ALPHA/ADD) desteğini yokla:
     * Üretilen wasm ikilisi MTK_FUSE_NONE ikilisiyle birebir aynıysa füzyon henüz
     * codegen'de yok demektir (has_fused[fm] = FALSE kalır). Farklıysa JIT'e yükle. */
    u8* fuse_buf = ARENA_PUSH_ARRAY(scratch, u8, wasm_cap);
    for (u32 fm = 1; fm <= 3; fm++) {
        opts.fuse_blend = fm;
        err[0] = '\0';
        u32 flen = mtk_codegen(&ir, &opts, scratch, fuse_buf, wasm_cap, err, sizeof(err));
        if (flen > 0 && (flen != wasm_len || __builtin_memcmp(fuse_buf, wasm_buf, wasm_len) != 0)) {
            ShadeFn ffn = jit_load_shade(fuse_buf, flen);
            if (ffn != NULL) {
                out->jit_fn[fm]    = ffn;
                out->has_fused[fm] = TRUE;
            }
        }
    }

    arena_pop_to(scratch, mark);
    return TRUE;
}

void shaders_init(ShaderManager* sm, Arena* scratch) {
    ASSERT(sm != NULL && scratch != NULL);
    __builtin_memset(sm, 0, sizeof(*sm));

    u32 count = g_embedded_shader_count;
    if (count > APP_MAX_SHADERS) count = APP_MAX_SHADERS;

    for (u32 i = 0; i < count; i++) {
        const EmbeddedFile* ef = &g_embedded_shaders[i];
        ManagedShader* sh = &sm->items[sm->count++];
        MtkDiag diag;
        b32 ok = shaders_compile_one(sh, ef->name, ef->src, ef->len,
                                     default_ref_for_name(ef->name), scratch, &diag);
        if (ok) {
            LOG("JIT shader [%s]: %.3f ms, %u byte wasm, %u IR komut, %u uniformB, %u varying",
                sh->name, sh->compile_ms, sh->wasm_bytes, sh->ir_inst_count,
                sh->layout.uniform_bytes, sh->layout.varying_count);
        } else {
            LOG("JIT shader HATA [%s]: C referansina dusuldu\n%s",
                ef->name, diag.text);
        }
    }
}

const ManagedShader* shaders_find(const ShaderManager* sm, const char* name) {
    if (!sm || !name) return NULL;
    for (u32 i = 0; i < sm->count; i++) {
        if (str_eq_name(sm->items[i].name, name)) {
            return &sm->items[i];
        }
    }
    return NULL;
}

ShadeFn shaders_get_fn(const ManagedShader* sh, b32 use_jit,
                       BlendMode blend, b32* out_fused) {
    if (out_fused) *out_fused = FALSE;
    if (!sh) return REF_SHADE_SOLID_FN;
    if (!use_jit || !sh->ok || sh->jit_fn[0] == NULL) {
        return sh->ref_fn;
    }
    u32 idx = 0;
    if (blend == BLEND_NONE)       idx = 1;
    else if (blend == BLEND_ALPHA) idx = 2;
    else if (blend == BLEND_ADD)   idx = 3;

    if (sh->has_fused[idx] && sh->jit_fn[idx] != NULL) {
        if (out_fused) *out_fused = TRUE;
        return sh->jit_fn[idx];
    }
    return sh->jit_fn[0];
}
