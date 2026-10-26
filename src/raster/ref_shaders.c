/* raster/ref_shaders.c — C referans shader'ları ve doku örnekleme implementasyonu.
 * En yakın komşu (nearest) ve çift doğrusal (bilinear) filtrelemeyi wrap/clamp ile destekler.
 * ref_shade_sprite, MATOKI.md §2'deki sprite.mtk örneğiyle birebir aynı semantiğe sahiptir.
 */
#include "raster/ref_shaders.h"
#include "core/log.h"

static inline i32 trunc_sat_f32(f32 x) {
#if defined(__wasm__)
    return (i32)x;
#else
    if (x != x) return 0;
    if (x >= 2147483648.0f) return I32_MAX;
    if (x < -2147483648.0f) return I32_MIN;
    return (i32)x;
#endif
}

static inline void unpack_rgba8(u32 px, f32 out[4]) {
    const f32 inv255 = 1.0f / 255.0f;
    out[0] = (f32)(px & 0xFFu) * inv255;
    out[1] = (f32)((px >> 8) & 0xFFu) * inv255;
    out[2] = (f32)((px >> 16) & 0xFFu) * inv255;
    out[3] = (f32)((px >> 24) & 0xFFu) * inv255;
}

static inline i32 wrap_or_clamp(i32 coord, i32 size, i32 mask, b32 is_clamp) {
    if (is_clamp) {
        return CLAMP(coord, 0, size - 1);
    }
    return coord & mask;
}

void ref_sample_tex(const Texture* tex, f32 u, f32 v, f32 out_rgba[4]) {
    ASSERT(tex != NULL && tex->px != NULL);
    f32 wf = (f32)(i32)tex->w;
    f32 hf = (f32)(i32)tex->h;
    i32 wi = (i32)tex->w;
    i32 hi = (i32)tex->h;
    i32 w_mask = (i32)tex->w_mask;
    i32 h_mask = (i32)tex->h_mask;
    b32 is_clamp = (tex->flags & TEX_FLAG_CLAMP) != 0;
    b32 is_bilerp = (tex->flags & TEX_FLAG_BILINEAR) != 0;

    if (!is_bilerp) {
        i32 tx = trunc_sat_f32(m_floor(u * wf));
        i32 ty = trunc_sat_f32(m_floor(v * hf));
        tx = wrap_or_clamp(tx, wi, w_mask, is_clamp);
        ty = wrap_or_clamp(ty, hi, h_mask, is_clamp);
        u32 px = tex->px[((u32)ty << tex->log2_w) + (u32)tx];
        unpack_rgba8(px, out_rgba);
    } else {
        f32 fu = u * wf - 0.5f;
        f32 fv = v * hf - 0.5f;
        f32 x0f = m_floor(fu);
        f32 y0f = m_floor(fv);
        f32 ax = fu - x0f;
        f32 ay = fv - y0f;

        i32 x0 = trunc_sat_f32(x0f);
        i32 y0 = trunc_sat_f32(y0f);
        i32 x1 = x0 + 1;
        i32 y1 = y0 + 1;

        x0 = wrap_or_clamp(x0, wi, w_mask, is_clamp);
        x1 = wrap_or_clamp(x1, wi, w_mask, is_clamp);
        y0 = wrap_or_clamp(y0, hi, h_mask, is_clamp);
        y1 = wrap_or_clamp(y1, hi, h_mask, is_clamp);

        f32 t00[4], t10[4], t01[4], t11[4];
        unpack_rgba8(tex->px[((u32)y0 << tex->log2_w) + (u32)x0], t00);
        unpack_rgba8(tex->px[((u32)y0 << tex->log2_w) + (u32)x1], t10);
        unpack_rgba8(tex->px[((u32)y1 << tex->log2_w) + (u32)x0], t01);
        unpack_rgba8(tex->px[((u32)y1 << tex->log2_w) + (u32)x1], t11);

        for (u32 c = 0; c < 4; c++) {
            f32 top = m_lerp(t00[c], t10[c], ax);
            f32 bot = m_lerp(t01[c], t11[c], ax);
            out_rgba[c] = m_lerp(top, bot, ay);
        }
    }
}

static inline u32 ref_f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

/* wasm f32.max: NaN yayılır */
static inline f32 ref_wasm_fmax(f32 a, f32 b) {
    if (a != a || b != b) return a + b;
    return a > b ? a : b;
}

static inline i32 ref_clamp_level(const Texture* tex, i32 level) {
    i32 levels = (tex->levels > 1u) ? (i32)tex->levels : 1;
    if (level > levels - 1) level = levels - 1;
    if (level < 0) level = 0;
    return level;
}

i32 ref_tex_mip_level(const Texture* tex, const f32 u[4], const f32 v[4]) {
    f32 wf = (f32)(i32)tex->w;
    f32 hf = (f32)(i32)tex->h;
    f32 dudx = (u[1] - u[0]) * wf;
    f32 dvdx = (v[1] - v[0]) * hf;
    f32 dudy = (u[2] - u[0]) * wf;
    f32 dvdy = (v[2] - v[0]) * hf;
    f32 px2  = dudx * dudx + dvdx * dvdx;
    f32 py2  = dudy * dudy + dvdy * dvdy;
    f32 rho2 = ref_wasm_fmax(px2, py2);
    i32 level = 0;
    if (rho2 >= 1.0f) {
        i32 e = (i32)(ref_f32_bits(rho2) >> 23) - 127;
        level = e >> 1;
    }
    return ref_clamp_level(tex, level);
}

Texture ref_tex_level_view(const Texture* tex, i32 level) {
    level = ref_clamp_level(tex, level);
    u32 wh4  = (tex->w * tex->h) << 2;
    u32 base = (wh4 - (wh4 >> (2u * (u32)level))) * 0xAAAAAAABu;   /* / 3 (tam bölünür) */
    Texture t = *tex;
    t.px     = tex->px + base;
    t.w      = tex->w >> level;
    t.h      = tex->h >> level;
    t.w_mask = t.w - 1u;
    t.h_mask = t.h - 1u;
    t.log2_w = tex->log2_w - (u32)level;
    t.levels = 1u;
    return t;
}

void ref_sample_tex_level(const Texture* tex, f32 u, f32 v, i32 level, f32 out_rgba[4]) {
    Texture t = ref_tex_level_view(tex, level);
    ref_sample_tex(&t, u, v, out_rgba);
}

void ref_shade_solid(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;

    Vec4 col = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (batch->uniforms != NULL) {
        const Vec4* u = (const Vec4*)batch->uniforms;
        col = *u;
    }

    u32 total_lanes = count * 4u;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    for (u32 i = 0; i < total_lanes; i++) {
        out_r[i] = col.x;
        out_g[i] = col.y;
        out_b[i] = col.z;
        out_a[i] = col.w;
    }
}

void ref_shade_vcolor(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;
    ASSERT(batch->varying_count >= 4);

    u32 total_lanes = count * 4u;
    for (u32 c = 0; c < 4; c++) {
        const f32* src = batch->varyings + c * total_lanes;
        f32* dst       = batch->out_rgba + c * total_lanes;
        __builtin_memcpy(dst, src, (usize)total_lanes * sizeof(f32));
    }
}

void ref_shade_sprite(ShadeBatch* batch) {
    u32 count = batch->count;
    if (count == 0) return;
    ASSERT(batch->varying_count >= 2);
    ASSERT(batch->textures != NULL);

    const Texture* tex = &batch->textures[0];
    Vec4 tint = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (batch->uniforms != NULL) {
        tint = *(const Vec4*)batch->uniforms;
    }

    u32 total_lanes = count * 4u;
    const f32* var_u = batch->varyings + 0u * total_lanes;
    const f32* var_v = batch->varyings + 1u * total_lanes;
    f32* out_r = batch->out_rgba + 0u * total_lanes;
    f32* out_g = batch->out_rgba + 1u * total_lanes;
    f32* out_b = batch->out_rgba + 2u * total_lanes;
    f32* out_a = batch->out_rgba + 3u * total_lanes;

    /* En yaygın durum: nearest + wrap/clamp için bayrak kontrolü döngü dışına alınır */
    if ((tex->flags & TEX_FLAG_BILINEAR) == 0) {
        const u32* px = tex->px;
        f32 wf = (f32)(i32)tex->w;
        f32 hf = (f32)(i32)tex->h;
        i32 wi = (i32)tex->w;
        i32 hi = (i32)tex->h;
        i32 w_mask = (i32)tex->w_mask;
        i32 h_mask = (i32)tex->h_mask;
        u32 log2_w = tex->log2_w;
        b32 is_clamp = (tex->flags & TEX_FLAG_CLAMP) != 0;
        const f32 inv255 = 1.0f / 255.0f;

        for (u32 q = 0; q < count; q++) {
            u32 mask = batch->quads[q].mask;
            u32 base = q * 4u;
            for (u32 lane = 0; lane < 4; lane++) {
                u32 idx = base + lane;
                i32 tx = trunc_sat_f32(m_floor(var_u[idx] * wf));
                i32 ty = trunc_sat_f32(m_floor(var_v[idx] * hf));
                if (is_clamp) {
                    tx = CLAMP(tx, 0, wi - 1);
                    ty = CLAMP(ty, 0, hi - 1);
                } else {
                    tx &= w_mask;
                    ty &= h_mask;
                }
                u32 texel = px[((u32)ty << log2_w) + (u32)tx];
                f32 cr = (f32)(texel & 0xFFu) * inv255;
                f32 cg = (f32)((texel >> 8) & 0xFFu) * inv255;
                f32 cb = (f32)((texel >> 16) & 0xFFu) * inv255;
                f32 ca = (f32)((texel >> 24) & 0xFFu) * inv255;

                if (ca < 0.01f) {
                    mask &= ~(1u << lane);
                }
                out_r[idx] = cr * tint.x;
                out_g[idx] = cg * tint.y;
                out_b[idx] = cb * tint.z;
                out_a[idx] = ca * tint.w;
            }
            batch->quads[q].mask = mask;
        }
    } else {
        for (u32 q = 0; q < count; q++) {
            u32 mask = batch->quads[q].mask;
            u32 base = q * 4u;
            for (u32 lane = 0; lane < 4; lane++) {
                u32 idx = base + lane;
                f32 rgba[4];
                ref_sample_tex(tex, var_u[idx], var_v[idx], rgba);
                if (rgba[3] < 0.01f) {
                    mask &= ~(1u << lane);
                }
                out_r[idx] = rgba[0] * tint.x;
                out_g[idx] = rgba[1] * tint.y;
                out_b[idx] = rgba[2] * tint.z;
                out_a[idx] = rgba[3] * tint.w;
            }
            batch->quads[q].mask = mask;
        }
    }
}
