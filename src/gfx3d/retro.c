/* gfx3d/retro.c — Eski konsol görüntü modu: 5:5:5 / N-bit nicemleme, Bayer 4x4 titreşim,
 * 32^3 palet arama tablosu, yatay CRT bulanıklığı, tarama çizgisi (scanline) karartma,
 * tamsayı büyütme (merkezleme + kenar temizleme) ve 3D köşe yakalama (vertex snapping).
 */
#include "gfx3d/retro.h"
#include "core/math.h"

/* Standart 4x4 sıralı Bayer titreşim matrisi (0..15) */
static const u8 k_bayer4x4[4][4] = {
    {  0u,  8u,  2u, 10u },
    { 12u,  4u, 14u,  6u },
    {  3u, 11u,  1u,  9u },
    { 15u,  7u, 13u,  5u }
};

/* color_bits parametresini kanal başına bit sayısına (1..8) dönüştürür */
static inline u32 retro_norm_bpc(u32 color_bits) {
    if (color_bits >= 1u && color_bits <= 8u) return color_bits;
    if (color_bits == 15u || color_bits == 16u) return 5u;
    if (color_bits == 12u) return 4u;
    if (color_bits == 9u)  return 3u;
    return 8u;
}

/* bpc-bit (0 .. (1<<bpc)-1) değerini bit tekrarıyla tam [0..255] aralığına genişletir */
static inline u32 expand_bits(u32 q, u32 bpc) {
    switch (bpc) {
    case 1u: return q ? 255u : 0u;
    case 2u: return (q << 6) | (q << 4) | (q << 2) | q;
    case 3u: return (q << 5) | (q << 2) | (q >> 1);
    case 4u: return (q << 4) | q;
    case 5u: return (q << 3) | (q >> 2);
    case 6u: return (q << 2) | (q >> 4);
    case 7u: return (q << 1) | (q >> 6);
    default: return q & 0xFFu;
    }
}

/* [0..255] kanalı bpc-bit (0..max_q) değere nicemler.
 * Dither açıkken: floor((c * max_q + bayer * 16) / 255) = ((c * max_q + bayer * 16) * 32897) >> 23 */
static inline u32 quantize_chan(u32 c, u32 bpc, b32 dither, u32 bayer) {
    if (bpc >= 8u) return c & 0xFFu;
    if (!dither) return (c & 0xFFu) >> (8u - bpc);
    u32 max_q = (1u << bpc) - 1u;
    u32 m = (c & 0xFFu) * max_q + (bayer << 4);
    u32 q = (m * 32897u) >> 23;
    return (q > max_q) ? max_q : q;
}

RetroParams retro_default_params(void) {
    RetroParams p;
    p.inner_w    = 0u;
    p.inner_h    = 0u;
    p.scale      = 0u;
    p.color_bits = 5u;
    p.dither     = TRUE;
    p.palette    = NULL;
    p.scanline   = 0.25f;
    p.crt_blur   = FALSE;
    return p;
}

u8 retro_palette_nearest_brute(const u32* colors, u32 count, u8 r, u8 g, u8 b) {
    if (colors == NULL || count == 0u) return 0u;
    if (count > RETRO_PALETTE_MAX) count = RETRO_PALETTE_MAX;

    u32 best_idx  = 0u;
    u32 best_dist = U32_MAX;
    for (u32 i = 0u; i < count; i++) {
        u32 col = colors[i];
        i32 pr  = (i32)(col & 0xFFu);
        i32 pg  = (i32)((col >> 8) & 0xFFu);
        i32 pb  = (i32)((col >> 16) & 0xFFu);
        i32 dr  = (i32)r - pr;
        i32 dg  = (i32)g - pg;
        i32 db  = (i32)b - pb;
        u32 d2  = (u32)(dr * dr + dg * dg + db * db);
        if (d2 < best_dist) {
            best_dist = d2;
            best_idx  = i;
            if (d2 == 0u) break;
        }
    }
    return (u8)best_idx;
}

void retro_build_palette_lut(RetroPalette* pal, const u32* colors, u32 count) {
    if (pal == NULL) return;
    if (colors == NULL || count == 0u) {
        __builtin_memset(pal, 0, sizeof(*pal));
        return;
    }
    if (count > RETRO_PALETTE_MAX) count = RETRO_PALETTE_MAX;

    pal->count = count;
    for (u32 i = 0u; i < count; i++) {
        pal->colors[i] = colors[i] | 0xFF000000u;
    }
    for (u32 i = count; i < RETRO_PALETTE_MAX; i++) {
        pal->colors[i] = 0xFF000000u;
    }

    for (u32 r5 = 0u; r5 < RETRO_LUT_DIM; r5++) {
        u8 r8 = (u8)((r5 << 3) | (r5 >> 2));
        for (u32 g5 = 0u; g5 < RETRO_LUT_DIM; g5++) {
            u8 g8 = (u8)((g5 << 3) | (g5 >> 2));
            for (u32 b5 = 0u; b5 < RETRO_LUT_DIM; b5++) {
                u8 b8 = (u8)((b5 << 3) | (b5 >> 2));
                u32 idx = retro_lut_index(r5, g5, b5);
                pal->lut[idx] = retro_palette_nearest_brute(pal->colors, count, r8, g8, b8);
            }
        }
    }
}

static inline u32 quantize_pixel_bpc(u32 rgba, u32 x, u32 y, u32 bpc, b32 dither,
                                     const RetroPalette* palette) {
    u32 r = rgba & 0xFFu;
    u32 g = (rgba >> 8) & 0xFFu;
    u32 b = (rgba >> 16) & 0xFFu;
    u32 bayer = (u32)k_bayer4x4[y & 3u][x & 3u];

    if (palette != NULL && palette->count > 0u) {
        u32 r5, g5, b5;
        if (bpc < 5u) {
            u32 rq = expand_bits(quantize_chan(r, bpc, dither, bayer), bpc);
            u32 gq = expand_bits(quantize_chan(g, bpc, dither, bayer), bpc);
            u32 bq = expand_bits(quantize_chan(b, bpc, dither, bayer), bpc);
            r5 = rq >> 3;
            g5 = gq >> 3;
            b5 = bq >> 3;
        } else {
            r5 = quantize_chan(r, 5u, dither, bayer);
            g5 = quantize_chan(g, 5u, dither, bayer);
            b5 = quantize_chan(b, 5u, dither, bayer);
        }
        u32 idx = retro_lut_index(r5, g5, b5);
        u8  pi  = palette->lut[idx];
        if (pi >= palette->count) pi = 0u;
        return palette->colors[pi] | 0xFF000000u;
    }

    if (bpc >= 8u) {
        return rgba | 0xFF000000u;
    }

    u32 rq = quantize_chan(r, bpc, dither, bayer);
    u32 gq = quantize_chan(g, bpc, dither, bayer);
    u32 bq = quantize_chan(b, bpc, dither, bayer);
    u32 ro = expand_bits(rq, bpc);
    u32 go = expand_bits(gq, bpc);
    u32 bo = expand_bits(bq, bpc);
    return ro | (go << 8) | (bo << 16) | 0xFF000000u;
}

u32 retro_quantize_pixel(u32 rgba, u32 x, u32 y, u32 color_bits, b32 dither,
                         const RetroPalette* palette) {
    u32 bpc = retro_norm_bpc(color_bits);
    return quantize_pixel_bpc(rgba, x, y, bpc, dither, palette);
}

static inline f32 snap_coord(f32 v, f32 s, f32 inv_s) {
    /* NaN veya aşırı büyük değerlerde taşmasız güvenli dönüş */
    if (!(v == v)) return 0.0f;
    f32 scaled = v * s;
    if (!(scaled == scaled) || m_abs(scaled) >= 8388608.0f) return v;
    return m_floor(scaled + 0.5f) * inv_s;
}

void retro_snap_xy(f32* x, f32* y, u32 subpixel) {
    u32 sub = (subpixel == 0u) ? 1u : (subpixel > RETRO_MAX_SUBPIXEL ? RETRO_MAX_SUBPIXEL : subpixel);
    f32 s = (f32)sub;
    f32 inv_s = 1.0f / s;
    if (x != NULL) *x = snap_coord(*x, s, inv_s);
    if (y != NULL) *y = snap_coord(*y, s, inv_s);
}

#if !defined(__wasm_simd128__)

void retro_quantize_row(const u32* src, u32* dst, u32 w, u32 sy, u32 bpc, b32 dither,
                        const RetroPalette* palette) {
    const u8* brow = k_bayer4x4[sy & 3u];
    if (palette == NULL || palette->count == 0u) {
        if (bpc == 5u && dither) {
            for (u32 x = 0u; x < w; x++) {
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
            for (u32 x = 0u; x < w; x++) {
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
            for (u32 x = 0u; x < w; x++) {
                dst[x] = src[x] | 0xFF000000u;
            }
            return;
        }
    }

    for (u32 x = 0u; x < w; x++) {
        dst[x] = quantize_pixel_bpc(src[x], x, sy, bpc, dither, palette);
    }
}

void retro_blur_row(const u32* src, u32* dst, u32 w) {
    if (w == 0u) return;
    if (w == 1u) {
        dst[0] = src[0] | 0xFF000000u;
        return;
    }
    for (u32 x = 0u; x < w; x++) {
        u32 pl = src[(x > 0u) ? (x - 1u) : 0u];
        u32 pc = src[x];
        u32 pr = src[(x + 1u < w) ? (x + 1u) : (w - 1u)];
        u32 r  = ((pl & 0xFFu) + 6u * (pc & 0xFFu) + (pr & 0xFFu) + 4u) >> 3;
        u32 g  = (((pl >> 8) & 0xFFu) + 6u * ((pc >> 8) & 0xFFu) + ((pr >> 8) & 0xFFu) + 4u) >> 3;
        u32 b  = (((pl >> 16) & 0xFFu) + 6u * ((pc >> 16) & 0xFFu) + ((pr >> 16) & 0xFFu) + 4u) >> 3;
        dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
}

void retro_darken_row(const u32* src, u32* dst, u32 w, u32 keep) {
    for (u32 x = 0u; x < w; x++) {
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
        for (u32 i = 0u; i < count; i++) {
            u32 px = src[i];
            dst[i * 2u + 0u] = px;
            dst[i * 2u + 1u] = px;
        }
        return;
    }
    if (scale == 3u) {
        for (u32 i = 0u; i < count; i++) {
            u32 px = src[i];
            dst[i * 3u + 0u] = px;
            dst[i * 3u + 1u] = px;
            dst[i * 3u + 2u] = px;
        }
        return;
    }
    if (scale == 4u) {
        for (u32 i = 0u; i < count; i++) {
            u32 px = src[i];
            u32* d = dst + i * 4u;
            d[0] = px;
            d[1] = px;
            d[2] = px;
            d[3] = px;
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

#endif /* !defined(__wasm_simd128__) */

/* Bir satırın görünür kaynak aralığını [sx_start, sx_end) hedef satıra s çarpanıyla yazar */
static void write_scaled_span(const u32* row_px, u32 proc_x0, u32* dst_row, i32 dst_w,
                              i64 ox, u32 sx_start, u32 sx_end, u32 s) {
    if (ox >= 0 && ox + (i64)sx_end * (i64)s <= (i64)dst_w && sx_start == 0u && proc_x0 == 0u) {
        retro_expand_row(row_px, dst_row + ox, sx_end, s);
        return;
    }

    u32 sx = sx_start;
    /* Sol kenarda kısmen kırpılan ilk piksel */
    if (sx < sx_end) {
        i64 x0 = ox + (i64)sx * (i64)s;
        i64 x1 = x0 + (i64)s;
        if (x0 < 0 || x1 > (i64)dst_w) {
            i32 cx0 = (i32)MAX((i64)0, x0);
            i32 cx1 = (i32)MIN((i64)dst_w, x1);
            u32 px  = row_px[sx - proc_x0];
            for (i32 x = cx0; x < cx1; x++) dst_row[x] = px;
            sx++;
        }
    }

    /* Tamamen hedef sınırları içinde kalan orta pikseller */
    u32 full_start = sx;
    while (sx < sx_end) {
        i64 x1 = ox + (i64)(sx + 1u) * (i64)s;
        if (x1 > (i64)dst_w) break;
        sx++;
    }
    if (sx > full_start) {
        i32 dst_x = (i32)(ox + (i64)full_start * (i64)s);
        retro_expand_row(row_px + (full_start - proc_x0), dst_row + dst_x, sx - full_start, s);
    }

    /* Sağ kenarda kısmen kırpılan son piksel */
    for (; sx < sx_end; sx++) {
        i64 x0 = ox + (i64)sx * (i64)s;
        i64 x1 = x0 + (i64)s;
        i32 cx0 = (i32)MAX((i64)0, x0);
        i32 cx1 = (i32)MIN((i64)dst_w, x1);
        u32 px  = row_px[sx - proc_x0];
        for (i32 x = cx0; x < cx1; x++) dst_row[x] = px;
    }
}

#define RETRO_ROW_BUF_CAP 2048u

void retro_present(const Framebuffer* src, Framebuffer* dst, const RetroParams* params) {
    if (dst == NULL || dst->px == NULL || dst->w == 0u || dst->h == 0u) return;
    if (dst->w > RETRO_MAX_DIM || dst->h > RETRO_MAX_DIM) return;

    if (src == NULL || src->px == NULL || src->w == 0u || src->h == 0u ||
        src->w > RETRO_MAX_DIM || src->h > RETRO_MAX_DIM) {
        fb_clear(dst, 0xFF000000u);
        return;
    }

    RetroParams def = retro_default_params();
    const RetroParams* p = (params != NULL) ? params : &def;

    u32 src_w = (p->inner_w > 0u && p->inner_w < src->w) ? p->inner_w : src->w;
    u32 src_h = (p->inner_h > 0u && p->inner_h < src->h) ? p->inner_h : src->h;

    u32 s = p->scale;
    if (s == 0u) {
        u32 sx_fit = dst->w / src_w;
        u32 sy_fit = dst->h / src_h;
        s = MIN(sx_fit, sy_fit);
        if (s == 0u) s = 1u;
    }

    i64 scaled_w = (i64)src_w * (i64)s;
    i64 scaled_h = (i64)src_h * (i64)s;
    i64 ox = ((i64)dst->w - scaled_w) / 2;
    i64 oy = ((i64)dst->h - scaled_h) / 2;

    if (ox + scaled_w <= 0 || oy + scaled_h <= 0 || ox >= (i64)dst->w || oy >= (i64)dst->h) {
        fb_clear(dst, 0xFF000000u);
        return;
    }

    i32 vis_x0 = (i32)MAX((i64)0, ox);
    i32 vis_x1 = (i32)MIN((i64)dst->w, ox + scaled_w);
    i32 vis_y0 = (i32)MAX((i64)0, oy);
    i32 vis_y1 = (i32)MIN((i64)dst->h, oy + scaled_h);

    u32 sx_start = (ox < 0) ? (u32)((-ox) / (i64)s) : 0u;
    u32 sx_end   = (ox + scaled_w > (i64)dst->w)
                   ? (u32)(((i64)dst->w - ox + (i64)s - 1) / (i64)s)
                   : src_w;
    if (sx_end > src_w) sx_end = src_w;

    u32 sy_start = (oy < 0) ? (u32)((-oy) / (i64)s) : 0u;
    u32 sy_end   = (oy + scaled_h > (i64)dst->h)
                   ? (u32)(((i64)dst->h - oy + (i64)s - 1) / (i64)s)
                   : src_h;
    if (sy_end > src_h) sy_end = src_h;
    if (sx_start >= sx_end || sy_start >= sy_end) {
        fb_clear(dst, 0xFF000000u);
        return;
    }

    u32 bpc = retro_norm_bpc(p->color_bits);
    f32 sl  = p->scanline;
    if (!(sl > 0.0f)) sl = 0.0f;
    if (sl > 1.0f)    sl = 1.0f;
    u32 keep = (u32)((1.0f - sl) * 256.0f + 0.5f);
    if (keep > 256u) keep = 256u;

    /* Bayer 4x4 hizalamasını (x & 3 == 0) ve yatay bulanıklık komşularını koru */
    u32 proc_x0 = (sx_start > 0u ? (sx_start - 1u) : 0u) & ~3u;
    u32 proc_x1 = MIN(src_w, sx_end + 1u);
    u32 proc_w  = proc_x1 - proc_x0;

    _Alignas(16) u32 buf_a[RETRO_ROW_BUF_CAP];
    _Alignas(16) u32 buf_b[RETRO_ROW_BUF_CAP];
    _Alignas(16) u32 buf_dark[RETRO_ROW_BUF_CAP];

    /* Çok geniş kaynak görüntülerde (>2048 px) taşmasız piksel-piksel yedek yol */
    if (proc_w > RETRO_ROW_BUF_CAP) {
        for (u32 sy = sy_start; sy < sy_end; sy++) {
            i32 dy0 = (i32)MAX((i64)0, oy + (i64)sy * (i64)s);
            i32 dy1 = (i32)MIN((i64)dst->h, oy + (i64)(sy + 1u) * (i64)s);
            const u32* srow = src->px + (usize)sy * (usize)src->w;
            for (u32 sx = sx_start; sx < sx_end; sx++) {
                u32 c = quantize_pixel_bpc(srow[sx], sx, sy, bpc, p->dither, p->palette);
                if (p->crt_blur && src_w > 1u) {
                    u32 cl = quantize_pixel_bpc(srow[sx > 0u ? sx - 1u : 0u],
                                                sx > 0u ? sx - 1u : 0u, sy, bpc, p->dither, p->palette);
                    u32 cr = quantize_pixel_bpc(srow[sx + 1u < src_w ? sx + 1u : src_w - 1u],
                                                sx + 1u < src_w ? sx + 1u : src_w - 1u, sy, bpc, p->dither, p->palette);
                    u32 r = ((cl & 0xFFu) + 6u * (c & 0xFFu) + (cr & 0xFFu) + 4u) >> 3;
                    u32 g = (((cl >> 8) & 0xFFu) + 6u * ((c >> 8) & 0xFFu) + ((cr >> 8) & 0xFFu) + 4u) >> 3;
                    u32 b = (((cl >> 16) & 0xFFu) + 6u * ((c >> 16) & 0xFFu) + ((cr >> 16) & 0xFFu) + 4u) >> 3;
                    c = r | (g << 8) | (b << 16) | 0xFF000000u;
                }
                u32 c_dark = c;
                if (keep < 256u) {
                    u32 r = ((c & 0xFFu) * keep) >> 8;
                    u32 g = (((c >> 8) & 0xFFu) * keep) >> 8;
                    u32 b = (((c >> 16) & 0xFFu) * keep) >> 8;
                    c_dark = r | (g << 8) | (b << 16) | 0xFF000000u;
                }
                i32 cx0 = (i32)MAX((i64)0, ox + (i64)sx * (i64)s);
                i32 cx1 = (i32)MIN((i64)dst->w, ox + (i64)(sx + 1u) * (i64)s);
                for (i32 dy = dy0; dy < dy1; dy++) {
                    u32 odd = (u32)((i64)dy - oy) & 1u;
                    u32 out = odd ? c_dark : c;
                    u32* drow = dst->px + (usize)dy * (usize)dst->w;
                    for (i32 dx = cx0; dx < cx1; dx++) drow[dx] = out;
                }
            }
        }
    } else {
        b32 reverse_y = (src->px == dst->px && oy >= 0);
        usize span_bytes = (usize)(vis_x1 - vis_x0) * sizeof(u32);

        for (u32 step = 0u; step < (sy_end - sy_start); step++) {
            u32 sy = reverse_y ? (sy_end - 1u - step) : (sy_start + step);
            const u32* srow = src->px + (usize)sy * (usize)src->w + proc_x0;

            retro_quantize_row(srow, buf_a, proc_w, sy, bpc, p->dither, p->palette);
            const u32* row_px = buf_a;
            if (p->crt_blur) {
                retro_blur_row(buf_a, buf_b, proc_w);
                row_px = buf_b;
            }

            i32 dy0 = (i32)MAX((i64)0, oy + (i64)sy * (i64)s);
            i32 dy1 = (i32)MIN((i64)dst->h, oy + (i64)(sy + 1u) * (i64)s);
            if (dy0 >= dy1) continue;

            if (keep == 256u) {
                u32* first_row = dst->px + (usize)dy0 * (usize)dst->w;
                write_scaled_span(row_px, proc_x0, first_row, (i32)dst->w, ox, sx_start, sx_end, s);
                for (i32 dy = dy0 + 1; dy < dy1; dy++) {
                    u32* drow = dst->px + (usize)dy * (usize)dst->w;
                    __builtin_memcpy(drow + vis_x0, first_row + vis_x0, span_bytes);
                }
            } else {
                u32* even_ref = NULL;
                u32* odd_ref  = NULL;
                b32  dark_ready = FALSE;
                for (i32 dy = dy0; dy < dy1; dy++) {
                    u32 odd = (u32)((i64)dy - oy) & 1u;
                    u32* drow = dst->px + (usize)dy * (usize)dst->w;
                    if (!odd) {
                        if (even_ref == NULL) {
                            write_scaled_span(row_px, proc_x0, drow, (i32)dst->w, ox, sx_start, sx_end, s);
                            even_ref = drow;
                        } else {
                            __builtin_memcpy(drow + vis_x0, even_ref + vis_x0, span_bytes);
                        }
                    } else {
                        if (odd_ref == NULL) {
                            if (!dark_ready) {
                                retro_darken_row(row_px + (sx_start - proc_x0),
                                                 buf_dark + (sx_start - proc_x0),
                                                 sx_end - sx_start, keep);
                                dark_ready = TRUE;
                            }
                            write_scaled_span(buf_dark, proc_x0, drow, (i32)dst->w, ox, sx_start, sx_end, s);
                            odd_ref = drow;
                        } else {
                            __builtin_memcpy(drow + vis_x0, odd_ref + vis_x0, span_bytes);
                        }
                    }
                }
            }
        }
    }

    /* Kenar boşluklarını (varsa) temizle (src == dst yerinde kullanımda kaynak okunduktan sonra) */
    if (vis_y0 > 0) {
        fb_rect(dst, 0, 0, (i32)dst->w, vis_y0, 0xFF000000u);
    }
    if (vis_y1 < (i32)dst->h) {
        fb_rect(dst, 0, vis_y1, (i32)dst->w, (i32)dst->h - vis_y1, 0xFF000000u);
    }
    if (vis_x0 > 0) {
        fb_rect(dst, 0, vis_y0, vis_x0, vis_y1 - vis_y0, 0xFF000000u);
    }
    if (vis_x1 < (i32)dst->w) {
        fb_rect(dst, vis_x1, vis_y0, (i32)dst->w - vis_x1, vis_y1 - vis_y0, 0xFF000000u);
    }
}
