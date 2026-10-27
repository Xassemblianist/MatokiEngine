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
 * gnd_col@48 eye@64 base@80 mat@96 fog@112; dokular: 0 albedo, 1 gölge. Varying: uv nrm depth wpos spos. */
static f32 pbr_sdepth(const f32 t[4]) {
    return t[0] * 0.99609381f + t[1] * 0.0038910f + t[2] * 0.0000152f;
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
        f32 t[4], s4[4];
        for (u32 k = 0; k < 4; k++) {
            ref_sample_tex_level(sht, p0x + (f32)(k & 1u) * dt, p0y + (f32)(k >> 1) * dt, 0, t);
            s4[k] = (z >= pbr_sdepth(t)) ? 1.0f : 0.0f;
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

static ShadeFn default_ref_for_name(const char* name) {
    if (str_eq_name(name, "sprite.mtk"))  return REF_SHADE_SPRITE_FN;
    if (str_eq_name(name, "solid.mtk"))   return REF_SHADE_SOLID_FN;
    if (str_eq_name(name, "vcolor.mtk"))  return REF_SHADE_VCOLOR_FN;
    if (str_eq_name(name, "plasma.mtk"))  return ref_shade_plasma;
    if (str_eq_name(name, "branchy.mtk")) return ref_shade_branchy;
    if (str_eq_name(name, "lit3d.mtk"))   return ref_shade_lit3d;
    if (str_eq_name(name, "pbr3d.mtk"))   return ref_shade_pbr3d;
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

    u32 wasm_cap = 65536;
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
