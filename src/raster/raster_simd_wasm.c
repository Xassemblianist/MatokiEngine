/* raster/raster_simd_wasm.c — WebAssembly 128-bit SIMD rasterizer ve blend çekirdekleri.
 * 2x2 quad'ların 4 lane'ini tek i32x4 / f32x4 vektöründe işler:
 *  - Kenar fonksiyonları: i32x4 artımlı adım + v128_or + (~e >> 31) i32x4.bitmask
 *  - Varying ve early-z kurulumu: düzlem denklemini 4 lane f32x4 olarak doğrudan SoA'ya yazar
 *  - Blend (NONE, ALPHA, ADD): i32x4.trunc_sat_f32x4 + i16x8.narrow + u8x16.narrow ile paketler.
 */
#include "raster/raster.h"
#include "core/math.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>

/* 4-bit quad maskesinden (0..15) 4x32-bit lane seçim maskesi tablosu */
static const _Alignas(16) u32 k_quad_mask_lut[16][4] = {
    { 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u },
    { 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0x00000000u },
    { 0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0x00000000u },
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x00000000u },
    { 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u },
    { 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u },
    { 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u },
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u },
    { 0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu },
    { 0xFFFFFFFFu, 0x00000000u, 0x00000000u, 0xFFFFFFFFu },
    { 0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu },
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu },
    { 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu },
    { 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu },
    { 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu },
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }
};

/* Skaler m_clamp(x, 0.0f, 1.0f) ile NaN ve -0.0 dahil bit-bit aynı f32x4 clamp */
static inline __attribute__((always_inline)) v128_t simd_clamp01(v128_t v, v128_t zero, v128_t one) {
    v128_t gt0 = wasm_f32x4_gt(v, zero);
    v128_t c0  = wasm_v128_and(v, gt0);
    v128_t gt1 = wasm_f32x4_gt(c0, one);
    return wasm_v128_bitselect(one, c0, gt1);
}

/* 4 kanal f32x4 [0..1] -> 4 lane paketli RGBA8 (u32x4):
 * trunc_sat_f32x4 (x4) + i16x8.narrow (x2) + i16x8.shl + v128.or + i16x8.shuffle */
static inline __attribute__((always_inline)) v128_t simd_pack_rgba8(v128_t r, v128_t g, v128_t b, v128_t a,
                                                                    v128_t zero, v128_t one, v128_t s255, v128_t half) {
    r = wasm_f32x4_add(wasm_f32x4_mul(simd_clamp01(r, zero, one), s255), half);
    g = wasm_f32x4_add(wasm_f32x4_mul(simd_clamp01(g, zero, one), s255), half);
    b = wasm_f32x4_add(wasm_f32x4_mul(simd_clamp01(b, zero, one), s255), half);
    a = wasm_f32x4_add(wasm_f32x4_mul(simd_clamp01(a, zero, one), s255), half);

    v128_t ri = wasm_i32x4_trunc_sat_f32x4(r);
    v128_t gi = wasm_i32x4_trunc_sat_f32x4(g);
    v128_t bi = wasm_i32x4_trunc_sat_f32x4(b);
    v128_t ai = wasm_i32x4_trunc_sat_f32x4(a);

    v128_t rb16 = wasm_i16x8_narrow_i32x4(ri, bi);
    v128_t ga16 = wasm_i16x8_narrow_i32x4(gi, ai);
    v128_t rgba_split = wasm_v128_or(rb16, wasm_i16x8_shl(ga16, 8));
    return wasm_i16x8_shuffle(rgba_split, rgba_split, 0, 4, 1, 5, 2, 6, 3, 7);
}

/* [0, 1] aralığında garanti edilen f32x4 kanallarını (ör. BLEND_ALPHA / BLEND_ADD sonucu) ekstra clamp olmadan paketler */
static inline __attribute__((always_inline)) v128_t simd_pack_rgba8_unclamped(v128_t r, v128_t g, v128_t b, v128_t a,
                                                                              v128_t s255, v128_t half) {
    v128_t ri = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_add(wasm_f32x4_mul(r, s255), half));
    v128_t gi = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_add(wasm_f32x4_mul(g, s255), half));
    v128_t bi = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_add(wasm_f32x4_mul(b, s255), half));
    v128_t ai = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_add(wasm_f32x4_mul(a, s255), half));

    v128_t rb16 = wasm_i16x8_narrow_i32x4(ri, bi);
    v128_t ga16 = wasm_i16x8_narrow_i32x4(gi, ai);
    v128_t rgba_split = wasm_v128_or(rb16, wasm_i16x8_shl(ga16, 8));
    return wasm_i16x8_shuffle(rgba_split, rgba_split, 0, 4, 1, 5, 2, 6, 3, 7);
}

static inline __attribute__((always_inline)) v128_t simd_depth_cmp(DepthFunc fn, v128_t z, v128_t ref) {
    switch (fn) {
        case DEPTH_NEVER:    return wasm_i32x4_const_splat(0);
        case DEPTH_LESS:     return wasm_f32x4_lt(z, ref);
        case DEPTH_EQUAL:    return wasm_f32x4_eq(z, ref);
        case DEPTH_LEQUAL:   return wasm_f32x4_le(z, ref);
        case DEPTH_GREATER:  return wasm_f32x4_gt(z, ref);
        case DEPTH_NOTEQUAL: return wasm_f32x4_ne(z, ref);
        case DEPTH_GEQUAL:   return wasm_f32x4_ge(z, ref);
        case DEPTH_ALWAYS:   return wasm_i32x4_const_splat(-1);
    }
    return wasm_i32x4_const_splat(-1);
}

u64 raster_kern_blend_batch(Rasterizer* r, const RasterShaderState* state, u32 count) {
    u32 total_lanes = count * 4u;
    const f32* out_r = r->out_rgba + 0u * total_lanes;
    const f32* out_g = r->out_rgba + 1u * total_lanes;
    const f32* out_b = r->out_rgba + 2u * total_lanes;
    const f32* out_a = r->out_rgba + 3u * total_lanes;

    u32* fb_px = r->fb->px;
    u32  fb_w  = r->fb->w;
    u64  pixels_written = 0;

    const v128_t zero = wasm_f32x4_const_splat(0.0f);
    const v128_t one  = wasm_f32x4_const_splat(1.0f);
    const v128_t s255 = wasm_f32x4_const_splat(255.0f);
    const v128_t half = wasm_f32x4_const_splat(0.5f);

    if (state->blend == BLEND_NONE) {
        for (u32 q = 0; q < count; q++) {
            u32 mask = r->quads[q].mask & 0xFu;
            if (mask == 0) continue;
            u32 qx = r->quads[q].x;
            u32 qy = r->quads[q].y;
            u32 base = q * 4u;
            u32* row0 = fb_px + qy * fb_w + qx;
            u32* row1 = row0 + fb_w;

            v128_t vr = wasm_v128_load(out_r + base);
            v128_t vg = wasm_v128_load(out_g + base);
            v128_t vb = wasm_v128_load(out_b + base);
            v128_t va = wasm_v128_load(out_a + base);
            v128_t packed = simd_pack_rgba8(vr, vg, vb, va, zero, one, s255, half);

            if (mask == 0xFu) {
                wasm_v128_store64_lane(row0, packed, 0);
                wasm_v128_store64_lane(row1, packed, 1);
                pixels_written += 4u;
            } else {
                v128_t dst = wasm_v128_load64_zero(row0);
                dst = wasm_v128_load64_lane(row1, dst, 1);
                v128_t vmask = wasm_v128_load(k_quad_mask_lut[mask]);
                v128_t res = wasm_v128_bitselect(packed, dst, vmask);
                wasm_v128_store64_lane(row0, res, 0);
                wasm_v128_store64_lane(row1, res, 1);
                pixels_written += (u64)__builtin_popcount(mask);
            }
        }
    } else if (state->blend == BLEND_ALPHA) {
        RasterWatBlendAlphaFn wat_blend = raster_get_wat_blend_fn();
        if (wat_blend != NULL) {
            _Alignas(16) u32 src_buf[SHADE_MAX_QUADS * 4];
            _Alignas(16) u32 dst_buf[SHADE_MAX_QUADS * 4];
            for (u32 q = 0; q < count; q++) {
                u32 mask = r->quads[q].mask & 0xFu;
                if (mask == 0) continue;
                u32 qx = r->quads[q].x;
                u32 qy = r->quads[q].y;
                u32 base = q * 4u;
                u32* row0 = fb_px + qy * fb_w + qx;
                u32* row1 = row0 + fb_w;

                v128_t dst = wasm_v128_load64_zero(row0);
                dst = wasm_v128_load64_lane(row1, dst, 1);
                wasm_v128_store(dst_buf + base, dst);

                v128_t vr = wasm_v128_load(out_r + base);
                v128_t vg = wasm_v128_load(out_g + base);
                v128_t vb = wasm_v128_load(out_b + base);
                v128_t va = wasm_v128_load(out_a + base);
                v128_t packed_src = simd_pack_rgba8(vr, vg, vb, va, zero, one, s255, half);
                wasm_v128_store(src_buf + base, packed_src);
            }
            wat_blend(dst_buf, src_buf, count * 4u);
            for (u32 q = 0; q < count; q++) {
                u32 mask = r->quads[q].mask & 0xFu;
                if (mask == 0) continue;
                u32 qx = r->quads[q].x;
                u32 qy = r->quads[q].y;
                u32 base = q * 4u;
                u32* row0 = fb_px + qy * fb_w + qx;
                u32* row1 = row0 + fb_w;
                v128_t packed = wasm_v128_load(dst_buf + base);
                if (mask == 0xFu) {
                    wasm_v128_store64_lane(row0, packed, 0);
                    wasm_v128_store64_lane(row1, packed, 1);
                    pixels_written += 4u;
                } else {
                    v128_t orig = wasm_v128_load64_zero(row0);
                    orig = wasm_v128_load64_lane(row1, orig, 1);
                    v128_t vmask = wasm_v128_load(k_quad_mask_lut[mask]);
                    v128_t res = wasm_v128_bitselect(packed, orig, vmask);
                    wasm_v128_store64_lane(row0, res, 0);
                    wasm_v128_store64_lane(row1, res, 1);
                    pixels_written += (u64)__builtin_popcount(mask);
                }
            }
        } else {
            const v128_t inv255 = wasm_f32x4_const_splat(1.0f / 255.0f);
            const v128_t m_ff   = wasm_i32x4_const_splat(0xFF);

            for (u32 q = 0; q < count; q++) {
                u32 mask = r->quads[q].mask & 0xFu;
                if (mask == 0) continue;
                u32 qx = r->quads[q].x;
                u32 qy = r->quads[q].y;
                u32 base = q * 4u;
                u32* row0 = fb_px + qy * fb_w + qx;
                u32* row1 = row0 + fb_w;

                v128_t dst = wasm_v128_load64_zero(row0);
                dst = wasm_v128_load64_lane(row1, dst, 1);

                v128_t sr = simd_clamp01(wasm_v128_load(out_r + base), zero, one);
                v128_t sg = simd_clamp01(wasm_v128_load(out_g + base), zero, one);
                v128_t sb = simd_clamp01(wasm_v128_load(out_b + base), zero, one);
                v128_t sa = simd_clamp01(wasm_v128_load(out_a + base), zero, one);
                v128_t inv_sa = wasm_f32x4_sub(one, sa);

                v128_t dr = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(dst, m_ff)), inv255);
                v128_t dg = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(dst, 8), m_ff)), inv255);
                v128_t db = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(dst, 16), m_ff)), inv255);
                v128_t da = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_u32x4_shr(dst, 24)), inv255);

                v128_t rr = wasm_f32x4_add(wasm_f32x4_mul(sr, sa), wasm_f32x4_mul(dr, inv_sa));
                v128_t rg = wasm_f32x4_add(wasm_f32x4_mul(sg, sa), wasm_f32x4_mul(dg, inv_sa));
                v128_t rb = wasm_f32x4_add(wasm_f32x4_mul(sb, sa), wasm_f32x4_mul(db, inv_sa));
                v128_t ra = wasm_f32x4_add(sa, wasm_f32x4_mul(da, inv_sa));

                v128_t packed = simd_pack_rgba8_unclamped(rr, rg, rb, ra, s255, half);
                if (mask == 0xFu) {
                    wasm_v128_store64_lane(row0, packed, 0);
                    wasm_v128_store64_lane(row1, packed, 1);
                    pixels_written += 4u;
                } else {
                    v128_t vmask = wasm_v128_load(k_quad_mask_lut[mask]);
                    v128_t res = wasm_v128_bitselect(packed, dst, vmask);
                    wasm_v128_store64_lane(row0, res, 0);
                    wasm_v128_store64_lane(row1, res, 1);
                    pixels_written += (u64)__builtin_popcount(mask);
                }
            }
        }
    } else { /* BLEND_ADD */
        const v128_t inv255 = wasm_f32x4_const_splat(1.0f / 255.0f);
        const v128_t m_ff   = wasm_i32x4_const_splat(0xFF);

        for (u32 q = 0; q < count; q++) {
            u32 mask = r->quads[q].mask & 0xFu;
            if (mask == 0) continue;
            u32 qx = r->quads[q].x;
            u32 qy = r->quads[q].y;
            u32 base = q * 4u;
            u32* row0 = fb_px + qy * fb_w + qx;
            u32* row1 = row0 + fb_w;

            v128_t dst = wasm_v128_load64_zero(row0);
            dst = wasm_v128_load64_lane(row1, dst, 1);

            v128_t sr = simd_clamp01(wasm_v128_load(out_r + base), zero, one);
            v128_t sg = simd_clamp01(wasm_v128_load(out_g + base), zero, one);
            v128_t sb = simd_clamp01(wasm_v128_load(out_b + base), zero, one);
            v128_t sa = simd_clamp01(wasm_v128_load(out_a + base), zero, one);

            v128_t dr = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(dst, m_ff)), inv255);
            v128_t dg = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(dst, 8), m_ff)), inv255);
            v128_t db = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(dst, 16), m_ff)), inv255);
            v128_t da = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_u32x4_shr(dst, 24)), inv255);

            v128_t rr = wasm_f32x4_min(wasm_f32x4_add(sr, dr), one);
            v128_t rg = wasm_f32x4_min(wasm_f32x4_add(sg, dg), one);
            v128_t rb = wasm_f32x4_min(wasm_f32x4_add(sb, db), one);
            v128_t ra = wasm_f32x4_min(wasm_f32x4_add(sa, da), one);

            v128_t packed = simd_pack_rgba8_unclamped(rr, rg, rb, ra, s255, half);
            if (mask == 0xFu) {
                wasm_v128_store64_lane(row0, packed, 0);
                wasm_v128_store64_lane(row1, packed, 1);
                pixels_written += 4u;
            } else {
                v128_t vmask = wasm_v128_load(k_quad_mask_lut[mask]);
                v128_t res = wasm_v128_bitselect(packed, dst, vmask);
                wasm_v128_store64_lane(row0, res, 0);
                wasm_v128_store64_lane(row1, res, 1);
                pixels_written += (u64)__builtin_popcount(mask);
            }
        }
    }

    return pixels_written;
}

/* Sıcak yol (2 varying = UV): her quad adımı yalnızca 2 f32x4.add + 2 v128.store */
static inline __attribute__((always_inline)) void emit_quad_uv_fast(
    Rasterizer* r, const RasterShaderState* state,
    v128_t vu, v128_t vv, u16 qx, u16 qy, u32 mask) {
    u32 q = r->batch_count++;
    r->quads[q].mask = mask;
    r->quads[q].x    = qx;
    r->quads[q].y    = qy;

    wasm_v128_store(r->varyings + (0u * SHADE_MAX_QUADS + q) * 4u, vu);
    wasm_v128_store(r->varyings + (1u * SHADE_MAX_QUADS + q) * 4u, vv);

    if (r->batch_count == SHADE_MAX_QUADS) {
        raster_flush_full_batch(r, state);
    }
}

/* Genel yol: early-z derinlik testi ve/veya N varying / 3D perspektif-doğru (inv_w) interpolasyonu */
static void emit_quad_general_simd(Rasterizer* r, const BinnedTri* tri,
                                   const RasterShaderState* state, u16 qx, u16 qy, u32 mask) {
    f32 dx = ((f32)qx + 0.5f) - tri->x0;
    f32 dy = ((f32)qy + 0.5f) - tri->y0;

    v128_t vz = wasm_f32x4_const_splat(0.0f);
    b32 has_depth = (r->fb->depth != NULL) && (state->depth_test || state->depth_write);
    if (has_depth) {
        f32 zb = tri->z0 + dx * tri->dz_dx + dy * tri->dz_dy;
        f32 gx = tri->dz_dx;
        f32 gy = tri->dz_dy;
        vz = wasm_f32x4_add(wasm_f32x4_splat(zb), wasm_f32x4_make(0.0f, gx, gy, gx + gy));

        if (state->depth_test) {
            const f32* d0 = r->fb->depth + (u32)qy * r->fb->w + (u32)qx;
            const f32* d1 = d0 + r->fb->w;
            v128_t ref = wasm_v128_load64_zero(d0);
            ref = wasm_v128_load64_lane(d1, ref, 1);
            v128_t cmp = simd_depth_cmp(state->depth_func, vz, ref);
            u32 zmask = (u32)wasm_i32x4_bitmask(cmp);
            mask &= zmask;
            if (mask == 0) return;
        }
    }

    u32 q = r->batch_count++;
    r->quads[q].mask = mask;
    r->quads[q].x    = qx;
    r->quads[q].y    = qy;

    if (has_depth && state->depth_write) {
        wasm_v128_store(r->quad_z + q * 4u, vz);
    }

    u32 vcount = state->varying_count;
    if (vcount > 0) {
        const f32* vc = tri->var_coeffs;
        if (tri->is_affine) {
            for (u32 v = 0; v < vcount; v++) {
                f32 v0 = vc[v * 3u + 0u];
                f32 gx = vc[v * 3u + 1u];
                f32 gy = vc[v * 3u + 2u];
                f32 b0 = v0 + dx * gx + dy * gy;
                v128_t vv = wasm_f32x4_add(wasm_f32x4_splat(b0),
                                           wasm_f32x4_make(0.0f, gx, gy, gx + gy));
                wasm_v128_store(r->varyings + (v * SHADE_MAX_QUADS + q) * 4u, vv);
            }
        } else {
            f32 l1_0 = dx * tri->dl1_dx + dy * tri->dl1_dy;
            f32 l2_0 = dx * tri->dl2_dx + dy * tri->dl2_dy;
            const v128_t ox = wasm_f32x4_const(0.0f, 1.0f, 0.0f, 1.0f);
            const v128_t oy = wasm_f32x4_const(0.0f, 0.0f, 1.0f, 1.0f);

            v128_t vl1 = wasm_f32x4_add(wasm_f32x4_splat(l1_0),
                         wasm_f32x4_add(wasm_f32x4_mul(ox, wasm_f32x4_splat(tri->dl1_dx)),
                                        wasm_f32x4_mul(oy, wasm_f32x4_splat(tri->dl1_dy))));
            v128_t vl2 = wasm_f32x4_add(wasm_f32x4_splat(l2_0),
                         wasm_f32x4_add(wasm_f32x4_mul(ox, wasm_f32x4_splat(tri->dl2_dx)),
                                        wasm_f32x4_mul(oy, wasm_f32x4_splat(tri->dl2_dy))));
            v128_t vl0 = wasm_f32x4_sub(wasm_f32x4_sub(wasm_f32x4_const_splat(1.0f), vl1), vl2);

            v128_t viw = wasm_f32x4_add(
                         wasm_f32x4_add(wasm_f32x4_mul(vl0, wasm_f32x4_splat(tri->inv_w0)),
                                        wasm_f32x4_mul(vl1, wasm_f32x4_splat(tri->inv_w1))),
                         wasm_f32x4_mul(vl2, wasm_f32x4_splat(tri->inv_w2)));
            v128_t v_one = wasm_f32x4_const_splat(1.0f);
            v128_t valid = wasm_f32x4_gt(wasm_f32x4_abs(viw), wasm_f32x4_const_splat(1e-20f));
            v128_t vw    = wasm_v128_bitselect(wasm_f32x4_div(v_one, viw), v_one, valid);

            for (u32 v = 0; v < vcount; v++) {
                v128_t vu0  = wasm_f32x4_splat(vc[v * 3u + 0u]);
                v128_t vdu1 = wasm_f32x4_splat(vc[v * 3u + 1u]);
                v128_t vdu2 = wasm_f32x4_splat(vc[v * 3u + 2u]);
                v128_t val  = wasm_f32x4_mul(
                              wasm_f32x4_add(wasm_f32x4_add(vu0, wasm_f32x4_mul(vl1, vdu1)),
                                             wasm_f32x4_mul(vl2, vdu2)),
                              vw);
                wasm_v128_store(r->varyings + (v * SHADE_MAX_QUADS + q) * 4u, val);
            }
        }
    }

    if (r->batch_count == SHADE_MAX_QUADS) {
        raster_flush_full_batch(r, state);
    }
}

void raster_kern_tri_scan(Rasterizer* r, const BinnedTri* tri,
                          const RasterShaderState* state, i32 tile_x0, i32 tile_y0,
                          i32 tile_x1, i32 tile_y1) {
    i32 min_x = MAX((i32)tri->min_x, tile_x0);
    i32 max_x = MIN((i32)tri->max_x, tile_x1);
    i32 min_y = MAX((i32)tri->min_y, tile_y0);
    i32 max_y = MIN((i32)tri->max_y, tile_y1);
    if (min_x > max_x || min_y > max_y) return;

    i32 a0 = tri->a0, b0 = tri->b0, c0 = tri->c0;
    i32 a1 = tri->a1, b1 = tri->b1, c1 = tri->c1;
    i32 a2 = tri->a2, b2 = tri->b2, c2 = tri->c2;

    i32 qx_min = min_x & ~1;
    i32 qy_min = min_y & ~1;

    /* 2x2 quad içinde 4 lane ofseti: lane0=(0,0), lane1=(a,0), lane2=(0,b), lane3=(a,b) */
    const v128_t step_x0 = wasm_i32x4_splat(2 * a0);
    const v128_t step_x1 = wasm_i32x4_splat(2 * a1);
    const v128_t step_x2 = wasm_i32x4_splat(2 * a2);

    const v128_t lane_off0 = wasm_i32x4_make(0, a0, b0, a0 + b0);
    const v128_t lane_off1 = wasm_i32x4_make(0, a1, b1, a1 + b1);
    const v128_t lane_off2 = wasm_i32x4_make(0, a2, b2, a2 + b2);

    b32 has_depth = (r->fb->depth != NULL) && (state->depth_test || state->depth_write);
    u32 vcount = state->varying_count;
    b32 fast_uv = (!has_depth) && tri->is_affine && (vcount == 2u);

    /* Hızlı 2D UV yolu: küçük üçgenlerde (<16x16) kenarları i32x4 ile, UV'yi f32x4 ile kur */
    if (max_x - qx_min < 16 && max_y - qy_min < 16) {
        i32 srow_e0 = a0 * qx_min + b0 * qy_min + c0;
        i32 srow_e1 = a1 * qx_min + b1 * qy_min + c1;
        i32 srow_e2 = a2 * qx_min + b2 * qy_min + c2;

        if (fast_uv) {
            const f32* vc = tri->var_coeffs;
            f32 x0 = tri->x0, y0 = tri->y0;
            f32 u0 = vc[0], ugx = vc[1], ugy = vc[2];
            f32 v0 = vc[3], vgx = vc[4], vgy = vc[5];
            v128_t u_lane = wasm_f32x4_make(0.0f, ugx, ugy, ugx + ugy);
            v128_t v_lane = wasm_f32x4_make(0.0f, vgx, vgy, vgx + vgy);

            for (i32 qy = qy_min; qy <= max_y; qy += 2) {
                v128_t qe0 = wasm_i32x4_add(wasm_i32x4_splat(srow_e0), lane_off0);
                v128_t qe1 = wasm_i32x4_add(wasm_i32x4_splat(srow_e1), lane_off1);
                v128_t qe2 = wasm_i32x4_add(wasm_i32x4_splat(srow_e2), lane_off2);
                u32 y_clip = (qy + 1 > tile_y1) ? ~0xCu : 0xFu;
                f32 dy = ((f32)qy + 0.5f) - y0;

                for (i32 qx = qx_min; qx <= max_x; qx += 2) {
                    v128_t any_neg = wasm_v128_or(wasm_v128_or(qe0, qe1), qe2);
                    u32 mask = (u32)wasm_i32x4_bitmask(wasm_v128_not(any_neg)) & y_clip;
                    if (qx + 1 > tile_x1) mask &= ~0xAu;

                    if (mask != 0) {
                        f32 dx = ((f32)qx + 0.5f) - x0;
                        v128_t vu = wasm_f32x4_add(wasm_f32x4_splat(u0 + dx * ugx + dy * ugy), u_lane);
                        v128_t vv = wasm_f32x4_add(wasm_f32x4_splat(v0 + dx * vgx + dy * vgy), v_lane);
                        emit_quad_uv_fast(r, state, vu, vv, (u16)qx, (u16)qy, mask);
                    }

                    qe0 = wasm_i32x4_add(qe0, step_x0);
                    qe1 = wasm_i32x4_add(qe1, step_x1);
                    qe2 = wasm_i32x4_add(qe2, step_x2);
                }
                srow_e0 += 2 * b0;
                srow_e1 += 2 * b1;
                srow_e2 += 2 * b2;
            }
        } else {
            for (i32 qy = qy_min; qy <= max_y; qy += 2) {
                v128_t qe0 = wasm_i32x4_add(wasm_i32x4_splat(srow_e0), lane_off0);
                v128_t qe1 = wasm_i32x4_add(wasm_i32x4_splat(srow_e1), lane_off1);
                v128_t qe2 = wasm_i32x4_add(wasm_i32x4_splat(srow_e2), lane_off2);
                u32 y_clip = (qy + 1 > tile_y1) ? ~0xCu : 0xFu;

                for (i32 qx = qx_min; qx <= max_x; qx += 2) {
                    v128_t any_neg = wasm_v128_or(wasm_v128_or(qe0, qe1), qe2);
                    u32 mask = (u32)wasm_i32x4_bitmask(wasm_v128_not(any_neg)) & y_clip;
                    if (qx + 1 > tile_x1) mask &= ~0xAu;

                    if (mask != 0) {
                        emit_quad_general_simd(r, tri, state, (u16)qx, (u16)qy, mask);
                    }

                    qe0 = wasm_i32x4_add(qe0, step_x0);
                    qe1 = wasm_i32x4_add(qe1, step_x1);
                    qe2 = wasm_i32x4_add(qe2, step_x2);
                }
                srow_e0 += 2 * b0;
                srow_e1 += 2 * b1;
                srow_e2 += 2 * b2;
            }
        }
    } else {
        i32 max_off0 = (a0 > 0 ? 7 * a0 : 0) + (b0 > 0 ? 7 * b0 : 0);
        i32 max_off1 = (a1 > 0 ? 7 * a1 : 0) + (b1 > 0 ? 7 * b1 : 0);
        i32 max_off2 = (a2 > 0 ? 7 * a2 : 0) + (b2 > 0 ? 7 * b2 : 0);
        i32 min_off0 = (a0 < 0 ? 7 * a0 : 0) + (b0 < 0 ? 7 * b0 : 0);
        i32 min_off1 = (a1 < 0 ? 7 * a1 : 0) + (b1 < 0 ? 7 * b1 : 0);
        i32 min_off2 = (a2 < 0 ? 7 * a2 : 0) + (b2 < 0 ? 7 * b2 : 0);

        i32 bx0 = min_x & ~7;
        i32 by0 = min_y & ~7;

        i32 row_e0 = a0 * bx0 + b0 * by0 + c0;
        i32 row_e1 = a1 * bx0 + b1 * by0 + c1;
        i32 row_e2 = a2 * bx0 + b2 * by0 + c2;

        const f32* vc = tri->var_coeffs;
        f32 x0 = tri->x0, y0 = tri->y0;
        v128_t u_lane = wasm_f32x4_const_splat(0.0f);
        v128_t v_lane = wasm_f32x4_const_splat(0.0f);
        if (fast_uv) {
            u_lane = wasm_f32x4_make(0.0f, vc[1], vc[2], vc[1] + vc[2]);
            v_lane = wasm_f32x4_make(0.0f, vc[4], vc[5], vc[4] + vc[5]);
        }

        for (i32 by = by0; by <= max_y; by += 8) {
            i32 blk_e0 = row_e0;
            i32 blk_e1 = row_e1;
            i32 blk_e2 = row_e2;

            for (i32 bx = bx0; bx <= max_x; bx += 8) {
                /* 8x8 Trivial Reject */
                if (((blk_e0 + max_off0) | (blk_e1 + max_off1) | (blk_e2 + max_off2)) >= 0) {
                    /* 8x8 Trivial Accept */
                    if (bx + 7 <= tile_x1 && by + 7 <= tile_y1 &&
                        ((blk_e0 + min_off0) | (blk_e1 + min_off1) | (blk_e2 + min_off2)) >= 0) {
                        if (fast_uv) {
                            for (i32 qy = by; qy < by + 8; qy += 2) {
                                f32 dy = ((f32)qy + 0.5f) - y0;
                                for (i32 qx = bx; qx < bx + 8; qx += 2) {
                                    f32 dx = ((f32)qx + 0.5f) - x0;
                                    v128_t vu = wasm_f32x4_add(wasm_f32x4_splat(vc[0] + dx * vc[1] + dy * vc[2]), u_lane);
                                    v128_t vv = wasm_f32x4_add(wasm_f32x4_splat(vc[3] + dx * vc[4] + dy * vc[5]), v_lane);
                                    emit_quad_uv_fast(r, state, vu, vv, (u16)qx, (u16)qy, 0xFu);
                                }
                            }
                        } else {
                            for (i32 qy = by; qy < by + 8; qy += 2) {
                                for (i32 qx = bx; qx < bx + 8; qx += 2) {
                                    emit_quad_general_simd(r, tri, state, (u16)qx, (u16)qy, 0xFu);
                                }
                            }
                        }
                    } else {
                        RasterWatEdgeBlockFn wat_edge = raster_get_wat_edge_fn();
                        if (wat_edge != NULL && bx >= qx_min && by >= qy_min &&
                            bx + 6 <= max_x && by + 6 <= max_y) {
                            i32 coeffs[9] = { a0, b0, c0, a1, b1, c1, a2, b2, c2 };
                            _Alignas(4) u8 bmasks[16];
                            wat_edge(coeffs, bx, by, bmasks);
                            u32 idx = 0;
                            for (i32 qy = by; qy < by + 8; qy += 2) {
                                u32 y_clip = (qy + 1 > tile_y1) ? ~0xCu : 0xFu;
                                f32 dy = ((f32)qy + 0.5f) - y0;
                                for (i32 qx = bx; qx < bx + 8; qx += 2) {
                                    u32 mask = ((u32)bmasks[idx++]) & y_clip;
                                    if (qx + 1 > tile_x1) mask &= ~0xAu;
                                    if (mask != 0) {
                                        if (fast_uv) {
                                            f32 dx = ((f32)qx + 0.5f) - x0;
                                            v128_t vu = wasm_f32x4_add(wasm_f32x4_splat(vc[0] + dx * vc[1] + dy * vc[2]), u_lane);
                                            v128_t vv = wasm_f32x4_add(wasm_f32x4_splat(vc[3] + dx * vc[4] + dy * vc[5]), v_lane);
                                            emit_quad_uv_fast(r, state, vu, vv, (u16)qx, (u16)qy, mask);
                                        } else {
                                            emit_quad_general_simd(r, tri, state, (u16)qx, (u16)qy, mask);
                                        }
                                    }
                                }
                            }
                        } else {
                            /* Kısmi 8x8 blok -> SIMD 2x2 quad taraması */
                            i32 qx0 = MAX(bx, qx_min);
                            i32 qy0 = MAX(by, qy_min);
                            i32 qx1 = MIN(bx + 6, max_x);
                            i32 qy1 = MIN(by + 6, max_y);

                            i32 s_e0 = blk_e0 + a0 * (qx0 - bx) + b0 * (qy0 - by);
                            i32 s_e1 = blk_e1 + a1 * (qx0 - bx) + b1 * (qy0 - by);
                            i32 s_e2 = blk_e2 + a2 * (qx0 - bx) + b2 * (qy0 - by);

                            for (i32 qy = qy0; qy <= qy1; qy += 2) {
                                v128_t qe0 = wasm_i32x4_add(wasm_i32x4_splat(s_e0), lane_off0);
                                v128_t qe1 = wasm_i32x4_add(wasm_i32x4_splat(s_e1), lane_off1);
                                v128_t qe2 = wasm_i32x4_add(wasm_i32x4_splat(s_e2), lane_off2);
                                u32 y_clip = (qy + 1 > tile_y1) ? ~0xCu : 0xFu;

                                for (i32 qx = qx0; qx <= qx1; qx += 2) {
                                    v128_t any_neg = wasm_v128_or(wasm_v128_or(qe0, qe1), qe2);
                                    u32 mask = (u32)wasm_i32x4_bitmask(wasm_v128_not(any_neg)) & y_clip;
                                    if (qx + 1 > tile_x1) mask &= ~0xAu;

                                    if (mask != 0) {
                                        if (fast_uv) {
                                            f32 dx = ((f32)qx + 0.5f) - x0;
                                            f32 dy = ((f32)qy + 0.5f) - y0;
                                            v128_t vu = wasm_f32x4_add(wasm_f32x4_splat(vc[0] + dx * vc[1] + dy * vc[2]), u_lane);
                                            v128_t vv = wasm_f32x4_add(wasm_f32x4_splat(vc[3] + dx * vc[4] + dy * vc[5]), v_lane);
                                            emit_quad_uv_fast(r, state, vu, vv, (u16)qx, (u16)qy, mask);
                                        } else {
                                            emit_quad_general_simd(r, tri, state, (u16)qx, (u16)qy, mask);
                                        }
                                    }

                                    qe0 = wasm_i32x4_add(qe0, step_x0);
                                    qe1 = wasm_i32x4_add(qe1, step_x1);
                                    qe2 = wasm_i32x4_add(qe2, step_x2);
                                }
                                s_e0 += 2 * b0;
                                s_e1 += 2 * b1;
                                s_e2 += 2 * b2;
                            }
                        }
                    }
                }
                blk_e0 += 8 * a0;
                blk_e1 += 8 * a1;
                blk_e2 += 8 * a2;
            }
            row_e0 += 8 * b0;
            row_e1 += 8 * b1;
            row_e2 += 8 * b2;
        }
    }
}

#endif /* defined(__wasm_simd128__) */
