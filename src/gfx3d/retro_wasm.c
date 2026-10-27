/* gfx3d/retro_wasm.c — Retro görüntü modunun WebAssembly 128-bit SIMD satır çekirdekleri.
 * 4 piksel/iterasyon (i32x4) işler; tüm aritmetik tamsayı tabanlı olup skaler retro.c ile
 * bit-bit aynı sonucu üretir (test_retro.c içinde 1000 rastgele kare ile doğrulanır).
 */
#include "gfx3d/retro.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>

static const u8 k_bayer4x4_simd[4][4] = {
    {  0u,  8u,  2u, 10u },
    { 12u,  4u, 14u,  6u },
    {  3u, 11u,  1u,  9u },
    { 15u,  7u, 13u,  5u }
};

void retro_quantize_row(const u32* src, u32* dst, u32 w, u32 sy, u32 bpc, b32 dither,
                        const RetroPalette* palette) {
    const u8* brow = k_bayer4x4_simd[sy & 3u];
    const v128_t vff    = wasm_i32x4_const_splat(0xFF);
    const v128_t v31    = wasm_i32x4_const_splat(31);
    const v128_t v32897 = wasm_i32x4_const_splat(32897);
    const v128_t valpha = wasm_i32x4_const_splat((i32)0xFF000000u);
    const v128_t vb16   = wasm_i32x4_make((i32)((u32)brow[0] << 4),
                                          (i32)((u32)brow[1] << 4),
                                          (i32)((u32)brow[2] << 4),
                                          (i32)((u32)brow[3] << 4));

    if (palette == NULL || palette->count == 0u) {
        if (bpc == 5u && dither) {
            u32 x = 0u;
            for (; x + 4u <= w; x += 4u) {
                v128_t px = wasm_v128_load(src + x);
                v128_t r  = wasm_v128_and(px, vff);
                v128_t g  = wasm_v128_and(wasm_u32x4_shr(px, 8), vff);
                v128_t b  = wasm_v128_and(wasm_u32x4_shr(px, 16), vff);

                v128_t r5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(r, v31), vb16), v32897), 23);
                v128_t g5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(g, v31), vb16), v32897), 23);
                v128_t b5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(b, v31), vb16), v32897), 23);

                v128_t ro = wasm_v128_or(wasm_i32x4_shl(r5, 3), wasm_u32x4_shr(r5, 2));
                v128_t go = wasm_v128_or(wasm_i32x4_shl(g5, 3), wasm_u32x4_shr(g5, 2));
                v128_t bo = wasm_v128_or(wasm_i32x4_shl(b5, 3), wasm_u32x4_shr(b5, 2));

                v128_t out = wasm_v128_or(wasm_v128_or(ro, wasm_i32x4_shl(go, 8)),
                                          wasm_v128_or(wasm_i32x4_shl(bo, 16), valpha));
                wasm_v128_store(dst + x, out);
            }
            for (; x < w; x++) {
                u32 px = src[x];
                u32 r  = px & 0xFFu;
                u32 g  = (px >> 8) & 0xFFu;
                u32 b  = (px >> 16) & 0xFFu;
                u32 b16 = (u32)brow[x & 3u] << 4;
                u32 r5 = ((r * 31u + b16) * 32897u) >> 23;
                u32 g5 = ((g * 31u + b16) * 32897u) >> 23;
                u32 b5 = ((b * 31u + b16) * 32897u) >> 23;
                u32 ro = (r5 << 3) | (r5 >> 2);
                u32 go = (g5 << 3) | (g5 >> 2);
                u32 bo = (b5 << 3) | (b5 >> 2);
                dst[x] = ro | (go << 8) | (bo << 16) | 0xFF000000u;
            }
            return;
        }
        if (bpc == 5u && !dither) {
            u32 x = 0u;
            for (; x + 4u <= w; x += 4u) {
                v128_t px = wasm_v128_load(src + x);
                v128_t r5 = wasm_u32x4_shr(wasm_v128_and(px, vff), 3);
                v128_t g5 = wasm_u32x4_shr(wasm_v128_and(wasm_u32x4_shr(px, 8), vff), 3);
                v128_t b5 = wasm_u32x4_shr(wasm_v128_and(wasm_u32x4_shr(px, 16), vff), 3);

                v128_t ro = wasm_v128_or(wasm_i32x4_shl(r5, 3), wasm_u32x4_shr(r5, 2));
                v128_t go = wasm_v128_or(wasm_i32x4_shl(g5, 3), wasm_u32x4_shr(g5, 2));
                v128_t bo = wasm_v128_or(wasm_i32x4_shl(b5, 3), wasm_u32x4_shr(b5, 2));

                v128_t out = wasm_v128_or(wasm_v128_or(ro, wasm_i32x4_shl(go, 8)),
                                          wasm_v128_or(wasm_i32x4_shl(bo, 16), valpha));
                wasm_v128_store(dst + x, out);
            }
            for (; x < w; x++) {
                u32 px = src[x];
                u32 r5 = (px & 0xFFu) >> 3;
                u32 g5 = ((px >> 8) & 0xFFu) >> 3;
                u32 b5 = ((px >> 16) & 0xFFu) >> 3;
                u32 ro = (r5 << 3) | (r5 >> 2);
                u32 go = (g5 << 3) | (g5 >> 2);
                u32 bo = (b5 << 3) | (b5 >> 2);
                dst[x] = ro | (go << 8) | (bo << 16) | 0xFF000000u;
            }
            return;
        }
        if (bpc >= 8u) {
            u32 x = 0u;
            for (; x + 4u <= w; x += 4u) {
                v128_t px = wasm_v128_load(src + x);
                wasm_v128_store(dst + x, wasm_v128_or(px, valpha));
            }
            for (; x < w; x++) {
                dst[x] = src[x] | 0xFF000000u;
            }
            return;
        }
    } else if (bpc >= 5u) {
        /* Palet + 5-bit (veya üstü) hızlı SIMD indeks hesabı + tablo araması */
        u32 x = 0u;
        u32 pcount = palette->count;
        for (; x + 4u <= w; x += 4u) {
            v128_t px = wasm_v128_load(src + x);
            v128_t r  = wasm_v128_and(px, vff);
            v128_t g  = wasm_v128_and(wasm_u32x4_shr(px, 8), vff);
            v128_t b  = wasm_v128_and(wasm_u32x4_shr(px, 16), vff);
            v128_t r5, g5, b5;
            if (dither) {
                r5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(r, v31), vb16), v32897), 23);
                g5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(g, v31), vb16), v32897), 23);
                b5 = wasm_u32x4_shr(wasm_i32x4_mul(wasm_i32x4_add(wasm_i32x4_mul(b, v31), vb16), v32897), 23);
            } else {
                r5 = wasm_u32x4_shr(r, 3);
                g5 = wasm_u32x4_shr(g, 3);
                b5 = wasm_u32x4_shr(b, 3);
            }
            v128_t idx = wasm_v128_or(wasm_v128_or(wasm_i32x4_shl(r5, 10), wasm_i32x4_shl(g5, 5)), b5);
            u32 i0 = (u32)wasm_i32x4_extract_lane(idx, 0) & 0x7FFFu;
            u32 i1 = (u32)wasm_i32x4_extract_lane(idx, 1) & 0x7FFFu;
            u32 i2 = (u32)wasm_i32x4_extract_lane(idx, 2) & 0x7FFFu;
            u32 i3 = (u32)wasm_i32x4_extract_lane(idx, 3) & 0x7FFFu;
            u8 p0 = palette->lut[i0]; if (p0 >= pcount) p0 = 0u;
            u8 p1 = palette->lut[i1]; if (p1 >= pcount) p1 = 0u;
            u8 p2 = palette->lut[i2]; if (p2 >= pcount) p2 = 0u;
            u8 p3 = palette->lut[i3]; if (p3 >= pcount) p3 = 0u;
            dst[x + 0u] = palette->colors[p0] | 0xFF000000u;
            dst[x + 1u] = palette->colors[p1] | 0xFF000000u;
            dst[x + 2u] = palette->colors[p2] | 0xFF000000u;
            dst[x + 3u] = palette->colors[p3] | 0xFF000000u;
        }
        for (; x < w; x++) {
            dst[x] = retro_quantize_pixel(src[x], x, sy, bpc, dither, palette);
        }
        return;
    }

    for (u32 x = 0u; x < w; x++) {
        dst[x] = retro_quantize_pixel(src[x], x, sy, bpc, dither, palette);
    }
}

void retro_blur_row(const u32* src, u32* dst, u32 w) {
    if (w == 0u) return;
    if (w == 1u) {
        dst[0] = src[0] | 0xFF000000u;
        return;
    }

    /* Sol sınır pikseli (x = 0, sol komşu = 0) */
    {
        u32 pl = src[0];
        u32 pc = src[0];
        u32 pr = src[1];
        u32 r  = ((pl & 0xFFu) + 6u * (pc & 0xFFu) + (pr & 0xFFu) + 4u) >> 3;
        u32 g  = (((pl >> 8) & 0xFFu) + 6u * ((pc >> 8) & 0xFFu) + ((pr >> 8) & 0xFFu) + 4u) >> 3;
        u32 b  = (((pl >> 16) & 0xFFu) + 6u * ((pc >> 16) & 0xFFu) + ((pr >> 16) & 0xFFu) + 4u) >> 3;
        dst[0] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }

    const v128_t vff    = wasm_i32x4_const_splat(0xFF);
    const v128_t v6     = wasm_i32x4_const_splat(6);
    const v128_t v4     = wasm_i32x4_const_splat(4);
    const v128_t valpha = wasm_i32x4_const_splat((i32)0xFF000000u);

    u32 x = 1u;
    for (; x + 4u < w; x += 4u) {
        v128_t pl = wasm_v128_load(src + x - 1u);
        v128_t pc = wasm_v128_load(src + x);
        v128_t pr = wasm_v128_load(src + x + 1u);

        v128_t rl = wasm_v128_and(pl, vff);
        v128_t rc = wasm_v128_and(pc, vff);
        v128_t rr = wasm_v128_and(pr, vff);
        v128_t ro = wasm_u32x4_shr(wasm_i32x4_add(wasm_i32x4_add(rl, wasm_i32x4_mul(rc, v6)),
                                                  wasm_i32x4_add(rr, v4)), 3);

        v128_t gl = wasm_v128_and(wasm_u32x4_shr(pl, 8), vff);
        v128_t gc = wasm_v128_and(wasm_u32x4_shr(pc, 8), vff);
        v128_t gr = wasm_v128_and(wasm_u32x4_shr(pr, 8), vff);
        v128_t go = wasm_u32x4_shr(wasm_i32x4_add(wasm_i32x4_add(gl, wasm_i32x4_mul(gc, v6)),
                                                  wasm_i32x4_add(gr, v4)), 3);

        v128_t bl = wasm_v128_and(wasm_u32x4_shr(pl, 16), vff);
        v128_t bc = wasm_v128_and(wasm_u32x4_shr(pc, 16), vff);
        v128_t br = wasm_v128_and(wasm_u32x4_shr(pr, 16), vff);
        v128_t bo = wasm_u32x4_shr(wasm_i32x4_add(wasm_i32x4_add(bl, wasm_i32x4_mul(bc, v6)),
                                                  wasm_i32x4_add(br, v4)), 3);

        v128_t out = wasm_v128_or(wasm_v128_or(ro, wasm_i32x4_shl(go, 8)),
                                  wasm_v128_or(wasm_i32x4_shl(bo, 16), valpha));
        wasm_v128_store(dst + x, out);
    }

    for (; x < w; x++) {
        u32 pl = src[x - 1u];
        u32 pc = src[x];
        u32 pr = src[(x + 1u < w) ? (x + 1u) : (w - 1u)];
        u32 r  = ((pl & 0xFFu) + 6u * (pc & 0xFFu) + (pr & 0xFFu) + 4u) >> 3;
        u32 g  = (((pl >> 8) & 0xFFu) + 6u * ((pc >> 8) & 0xFFu) + ((pr >> 8) & 0xFFu) + 4u) >> 3;
        u32 b  = (((pl >> 16) & 0xFFu) + 6u * ((pc >> 16) & 0xFFu) + ((pr >> 16) & 0xFFu) + 4u) >> 3;
        dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
}

void retro_darken_row(const u32* src, u32* dst, u32 w, u32 keep) {
    const v128_t vff    = wasm_i32x4_const_splat(0xFF);
    const v128_t vkeep  = wasm_i32x4_splat((i32)keep);
    const v128_t valpha = wasm_i32x4_const_splat((i32)0xFF000000u);

    u32 x = 0u;
    for (; x + 4u <= w; x += 4u) {
        v128_t px = wasm_v128_load(src + x);
        v128_t r  = wasm_u32x4_shr(wasm_i32x4_mul(wasm_v128_and(px, vff), vkeep), 8);
        v128_t g  = wasm_u32x4_shr(wasm_i32x4_mul(wasm_v128_and(wasm_u32x4_shr(px, 8), vff), vkeep), 8);
        v128_t b  = wasm_u32x4_shr(wasm_i32x4_mul(wasm_v128_and(wasm_u32x4_shr(px, 16), vff), vkeep), 8);
        v128_t out = wasm_v128_or(wasm_v128_or(r, wasm_i32x4_shl(g, 8)),
                                  wasm_v128_or(wasm_i32x4_shl(b, 16), valpha));
        wasm_v128_store(dst + x, out);
    }
    for (; x < w; x++) {
        u32 px = src[x];
        u32 r  = ((px & 0xFFu) * keep) >> 8;
        u32 g  = (((px >> 8) & 0xFFu) * keep) >> 8;
        u32 b  = (((px >> 16) & 0xFFu) * keep) >> 8;
        dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
}

void retro_expand_row(const u32* src, u32* dst, u32 count, u32 scale) {
    if (scale == 1u) {
        __builtin_memcpy(dst, src, (usize)count * sizeof(u32));
        return;
    }
    if (scale == 2u) {
        u32 i = 0u;
        for (; i + 4u <= count; i += 4u) {
            v128_t v  = wasm_v128_load(src + i);
            v128_t v0 = wasm_i32x4_shuffle(v, v, 0, 0, 1, 1);
            v128_t v1 = wasm_i32x4_shuffle(v, v, 2, 2, 3, 3);
            wasm_v128_store(dst + i * 2u + 0u, v0);
            wasm_v128_store(dst + i * 2u + 4u, v1);
        }
        for (; i < count; i++) {
            u32 px = src[i];
            dst[i * 2u + 0u] = px;
            dst[i * 2u + 1u] = px;
        }
        return;
    }
    if (scale == 3u) {
        u32 i = 0u;
        for (; i + 4u <= count; i += 4u) {
            v128_t v  = wasm_v128_load(src + i);
            v128_t v0 = wasm_i32x4_shuffle(v, v, 0, 0, 0, 1);
            v128_t v1 = wasm_i32x4_shuffle(v, v, 1, 1, 2, 2);
            v128_t v2 = wasm_i32x4_shuffle(v, v, 2, 3, 3, 3);
            wasm_v128_store(dst + i * 3u + 0u, v0);
            wasm_v128_store(dst + i * 3u + 4u, v1);
            wasm_v128_store(dst + i * 3u + 8u, v2);
        }
        for (; i < count; i++) {
            u32 px = src[i];
            dst[i * 3u + 0u] = px;
            dst[i * 3u + 1u] = px;
            dst[i * 3u + 2u] = px;
        }
        return;
    }
    if (scale == 4u) {
        u32 i = 0u;
        for (; i + 4u <= count; i += 4u) {
            v128_t v  = wasm_v128_load(src + i);
            v128_t v0 = wasm_i32x4_shuffle(v, v, 0, 0, 0, 0);
            v128_t v1 = wasm_i32x4_shuffle(v, v, 1, 1, 1, 1);
            v128_t v2 = wasm_i32x4_shuffle(v, v, 2, 2, 2, 2);
            v128_t v3 = wasm_i32x4_shuffle(v, v, 3, 3, 3, 3);
            u32* d = dst + i * 4u;
            wasm_v128_store(d + 0u,  v0);
            wasm_v128_store(d + 4u,  v1);
            wasm_v128_store(d + 8u,  v2);
            wasm_v128_store(d + 12u, v3);
        }
        for (; i < count; i++) {
            v128_t v = wasm_i32x4_splat((i32)src[i]);
            wasm_v128_store(dst + i * 4u, v);
        }
        return;
    }
    for (u32 i = 0u; i < count; i++) {
        u32 px = src[i];
        u32* d = dst + (usize)i * (usize)scale;
        for (u32 k = 0u; k < scale; k++) {
            d[k] = px;
        }
    }
}

#endif /* defined(__wasm_simd128__) */
