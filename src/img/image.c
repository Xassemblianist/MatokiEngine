/* img/image.c — Görüntü işleme algoritmaları implementasyonu.
 * Ayrılabilir kutu/Gauss bulanıklık, bilinear/kutu boyutlandırma, mipmap zinciri,
 * Sobel teğet uzayı normal haritası, renk düzeltme ve alfa ön-çarpma.
 * Hiçbir libc çağrısı içermez; tüm bellek Arena'dan ayrılır ve geri bırakılır.
 */
#include "img/image.h"
#include "core/math.h"
#include "core/log.h"

static u32 u32_log2(u32 v) {
    u32 r = 0;
    while (v > 1u) {
        v >>= 1u;
        r++;
    }
    return r;
}

void img_tex_init(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags) {
    ASSERT(IS_POW2(w) && IS_POW2(h));
    tex->px     = (u32*)arena_push(arena, (usize)w * (usize)h * sizeof(u32), 16);
    tex->w      = w;
    tex->h      = h;
    tex->w_mask = w - 1u;
    tex->h_mask = h - 1u;
    tex->log2_w = u32_log2(w);
    tex->flags  = flags;
    tex->levels = 1u;
}

static inline u8 clamp_u8(f32 v) {
    i32 iv = (i32)(v + 0.5f);
    if (iv < 0) return 0;
    if (iv > 255) return 255;
    return (u8)iv;
}

void img_blur_box(u32* px, u32 w, u32 h, i32 radius, Arena* scratch) {
    if (radius <= 0 || w == 0 || h == 0) return;

    usize mark = arena_mark(scratch);
    u32* tmp = ARENA_PUSH_ARRAY(scratch, u32, (usize)w * (usize)h);

    i32 diam = 2 * radius + 1;
    f32 inv_d = 1.0f / (f32)diam;

    /* Yatay geçiş: px -> tmp */
    for (u32 y = 0; y < h; y++) {
        const u32* row_in = px + (usize)y * w;
        u32* row_out = tmp + (usize)y * w;
        for (u32 x = 0; x < w; x++) {
            u32 sr = 0, sg = 0, sb = 0, sa = 0;
            for (i32 k = -radius; k <= radius; k++) {
                i32 sx = CLAMP((i32)x + k, 0, (i32)w - 1);
                u32 c = row_in[sx];
                sr += (c >>  0) & 0xFFu;
                sg += (c >>  8) & 0xFFu;
                sb += (c >> 16) & 0xFFu;
                sa += (c >> 24) & 0xFFu;
            }
            row_out[x] = FB_RGBA(clamp_u8((f32)sr * inv_d),
                                 clamp_u8((f32)sg * inv_d),
                                 clamp_u8((f32)sb * inv_d),
                                 clamp_u8((f32)sa * inv_d));
        }
    }

    /* Dikey geçiş: tmp -> px */
    for (u32 y = 0; y < h; y++) {
        u32* row_out = px + (usize)y * w;
        for (u32 x = 0; x < w; x++) {
            u32 sr = 0, sg = 0, sb = 0, sa = 0;
            for (i32 k = -radius; k <= radius; k++) {
                i32 sy = CLAMP((i32)y + k, 0, (i32)h - 1);
                u32 c = tmp[(usize)sy * w + x];
                sr += (c >>  0) & 0xFFu;
                sg += (c >>  8) & 0xFFu;
                sb += (c >> 16) & 0xFFu;
                sa += (c >> 24) & 0xFFu;
            }
            row_out[x] = FB_RGBA(clamp_u8((f32)sr * inv_d),
                                 clamp_u8((f32)sg * inv_d),
                                 clamp_u8((f32)sb * inv_d),
                                 clamp_u8((f32)sa * inv_d));
        }
    }

    arena_pop_to(scratch, mark);
}

void img_blur_gaussian(u32* px, u32 w, u32 h, i32 radius, f32 sigma, Arena* scratch) {
    if (radius <= 0 || sigma <= 0.0f || w == 0 || h == 0) return;
    if (radius > 64) radius = 64;

    usize mark = arena_mark(scratch);
    i32 ksize = 2 * radius + 1;
    f32* kernel = ARENA_PUSH_ARRAY(scratch, f32, ksize);
    u32* tmp = ARENA_PUSH_ARRAY(scratch, u32, (usize)w * (usize)h);

    /* exp(-x^2 / (2*sigma^2)) = exp2(-x^2 * log2(e) / (2*sigma^2)) */
    f32 inv_2s2_log2e = -1.44269504f / (2.0f * sigma * sigma);
    f32 ksum = 0.0f;
    for (i32 k = -radius; k <= radius; k++) {
        f32 fk = (f32)k;
        f32 wgt = m_exp2(fk * fk * inv_2s2_log2e);
        kernel[k + radius] = wgt;
        ksum += wgt;
    }
    f32 inv_sum = ksum > 0.0f ? (1.0f / ksum) : 1.0f;
    for (i32 i = 0; i < ksize; i++) {
        kernel[i] *= inv_sum;
    }

    /* Yatay geçiş */
    for (u32 y = 0; y < h; y++) {
        const u32* row_in = px + (usize)y * w;
        u32* row_out = tmp + (usize)y * w;
        for (u32 x = 0; x < w; x++) {
            f32 sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
            for (i32 k = -radius; k <= radius; k++) {
                i32 sx = CLAMP((i32)x + k, 0, (i32)w - 1);
                u32 c = row_in[sx];
                f32 kw = kernel[k + radius];
                sr += (f32)((c >>  0) & 0xFFu) * kw;
                sg += (f32)((c >>  8) & 0xFFu) * kw;
                sb += (f32)((c >> 16) & 0xFFu) * kw;
                sa += (f32)((c >> 24) & 0xFFu) * kw;
            }
            row_out[x] = FB_RGBA(clamp_u8(sr), clamp_u8(sg), clamp_u8(sb), clamp_u8(sa));
        }
    }

    /* Dikey geçiş */
    for (u32 y = 0; y < h; y++) {
        u32* row_out = px + (usize)y * w;
        for (u32 x = 0; x < w; x++) {
            f32 sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
            for (i32 k = -radius; k <= radius; k++) {
                i32 sy = CLAMP((i32)y + k, 0, (i32)h - 1);
                u32 c = tmp[(usize)sy * w + x];
                f32 kw = kernel[k + radius];
                sr += (f32)((c >>  0) & 0xFFu) * kw;
                sg += (f32)((c >>  8) & 0xFFu) * kw;
                sb += (f32)((c >> 16) & 0xFFu) * kw;
                sa += (f32)((c >> 24) & 0xFFu) * kw;
            }
            row_out[x] = FB_RGBA(clamp_u8(sr), clamp_u8(sg), clamp_u8(sb), clamp_u8(sa));
        }
    }

    arena_pop_to(scratch, mark);
}

void img_resize_bilinear(const u32* src, u32 src_w, u32 src_h,
                         u32* dst, u32 dst_w, u32 dst_h) {
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    f32 scale_x = (f32)src_w / (f32)dst_w;
    f32 scale_y = (f32)src_h / (f32)dst_h;

    for (u32 dy = 0; dy < dst_h; dy++) {
        f32 fy = ((f32)dy + 0.5f) * scale_y - 0.5f;
        f32 fy0 = m_floor(fy);
        i32 y0 = (i32)fy0;
        f32 ay = fy - fy0;
        i32 y1 = CLAMP(y0 + 1, 0, (i32)src_h - 1);
        y0     = CLAMP(y0,     0, (i32)src_h - 1);

        for (u32 dx = 0; dx < dst_w; dx++) {
            f32 fx = ((f32)dx + 0.5f) * scale_x - 0.5f;
            f32 fx0 = m_floor(fx);
            i32 x0 = (i32)fx0;
            f32 ax = fx - fx0;
            i32 x1 = CLAMP(x0 + 1, 0, (i32)src_w - 1);
            x0     = CLAMP(x0,     0, (i32)src_w - 1);

            u32 c00 = src[(usize)y0 * src_w + (usize)x0];
            u32 c10 = src[(usize)y0 * src_w + (usize)x1];
            u32 c01 = src[(usize)y1 * src_w + (usize)x0];
            u32 c11 = src[(usize)y1 * src_w + (usize)x1];

            u8 rgba[4];
            for (u32 ch = 0; ch < 4; ch++) {
                u32 shift = ch * 8u;
                f32 v00 = (f32)((c00 >> shift) & 0xFFu);
                f32 v10 = (f32)((c10 >> shift) & 0xFFu);
                f32 v01 = (f32)((c01 >> shift) & 0xFFu);
                f32 v11 = (f32)((c11 >> shift) & 0xFFu);
                f32 v = m_lerp(m_lerp(v00, v10, ax), m_lerp(v01, v11, ax), ay);
                rgba[ch] = clamp_u8(v);
            }
            dst[(usize)dy * dst_w + dx] = FB_RGBA(rgba[0], rgba[1], rgba[2], rgba[3]);
        }
    }
}

void img_resize_box(const u32* src, u32 src_w, u32 src_h,
                    u32* dst, u32 dst_w, u32 dst_h) {
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    f32 scale_x = (f32)src_w / (f32)dst_w;
    f32 scale_y = (f32)src_h / (f32)dst_h;

    for (u32 dy = 0; dy < dst_h; dy++) {
        i32 sy0 = (i32)m_floor((f32)dy * scale_y);
        i32 sy1 = (i32)m_ceil((f32)(dy + 1u) * scale_y);
        sy0 = CLAMP(sy0, 0, (i32)src_h - 1);
        sy1 = CLAMP(sy1, sy0 + 1, (i32)src_h);

        for (u32 dx = 0; dx < dst_w; dx++) {
            i32 sx0 = (i32)m_floor((f32)dx * scale_x);
            i32 sx1 = (i32)m_ceil((f32)(dx + 1u) * scale_x);
            sx0 = CLAMP(sx0, 0, (i32)src_w - 1);
            sx1 = CLAMP(sx1, sx0 + 1, (i32)src_w);

            u32 sr = 0, sg = 0, sb = 0, sa = 0;
            u32 cnt = 0;
            for (i32 sy = sy0; sy < sy1; sy++) {
                for (i32 sx = sx0; sx < sx1; sx++) {
                    u32 c = src[(usize)sy * src_w + (usize)sx];
                    sr += (c >>  0) & 0xFFu;
                    sg += (c >>  8) & 0xFFu;
                    sb += (c >> 16) & 0xFFu;
                    sa += (c >> 24) & 0xFFu;
                    cnt++;
                }
            }
            if (cnt == 0) cnt = 1;
            dst[(usize)dy * dst_w + dx] = FB_RGBA(
                (u8)((sr + cnt / 2u) / cnt),
                (u8)((sg + cnt / 2u) / cnt),
                (u8)((sb + cnt / 2u) / cnt),
                (u8)((sa + cnt / 2u) / cnt)
            );
        }
    }
}

u32 img_mipmap_build(const Texture* base, Texture* out_mips, u32 max_levels, Arena* arena) {
    if (!base || !out_mips || max_levels == 0) return 0;

    out_mips[0] = *base;
    u32 levels = 1;
    u32 cw = base->w;
    u32 ch = base->h;

    while ((cw > 1u || ch > 1u) && levels < max_levels) {
        u32 nw = cw > 1u ? (cw >> 1u) : 1u;
        u32 nh = ch > 1u ? (ch >> 1u) : 1u;
        Texture* prev = &out_mips[levels - 1u];
        Texture* cur  = &out_mips[levels];

        img_tex_init(cur, arena, nw, nh, base->flags);

        for (u32 y = 0; y < nh; y++) {
            u32 sy0 = (y * 2u) & prev->h_mask;
            u32 sy1 = (y * 2u + 1u) & prev->h_mask;
            for (u32 x = 0; x < nw; x++) {
                u32 sx0 = (x * 2u) & prev->w_mask;
                u32 sx1 = (x * 2u + 1u) & prev->w_mask;

                u32 c00 = prev->px[sy0 * prev->w + sx0];
                u32 c10 = prev->px[sy0 * prev->w + sx1];
                u32 c01 = prev->px[sy1 * prev->w + sx0];
                u32 c11 = prev->px[sy1 * prev->w + sx1];

                u32 r = (((c00 >>  0) & 0xFFu) + ((c10 >>  0) & 0xFFu) +
                         ((c01 >>  0) & 0xFFu) + ((c11 >>  0) & 0xFFu) + 2u) >> 2u;
                u32 g = (((c00 >>  8) & 0xFFu) + ((c10 >>  8) & 0xFFu) +
                         ((c01 >>  8) & 0xFFu) + ((c11 >>  8) & 0xFFu) + 2u) >> 2u;
                u32 b = (((c00 >> 16) & 0xFFu) + ((c10 >> 16) & 0xFFu) +
                         ((c01 >> 16) & 0xFFu) + ((c11 >> 16) & 0xFFu) + 2u) >> 2u;
                u32 a = (((c00 >> 24) & 0xFFu) + ((c10 >> 24) & 0xFFu) +
                         ((c01 >> 24) & 0xFFu) + ((c11 >> 24) & 0xFFu) + 2u) >> 2u;

                cur->px[y * nw + x] = FB_RGBA(r, g, b, a);
            }
        }

        cw = nw;
        ch = nh;
        levels++;
    }
    return levels;
}

static inline f32 px_luma(u32 c) {
    f32 r = (f32)((c >>  0) & 0xFFu);
    f32 g = (f32)((c >>  8) & 0xFFu);
    f32 b = (f32)((c >> 16) & 0xFFu);
    return (0.299f * r + 0.587f * g + 0.114f * b) * (1.0f / 255.0f);
}

static inline i32 coord_mode(i32 v, i32 dim, b32 wrap) {
    if (wrap) {
        i32 r = v % dim;
        return r < 0 ? r + dim : r;
    }
    return CLAMP(v, 0, dim - 1);
}

void img_normal_from_height(const u32* src_rgba, u32* dst_rgba,
                            u32 w, u32 h, f32 strength, b32 wrap) {
    if (w == 0 || h == 0) return;

    i32 iw = (i32)w;
    i32 ih = (i32)h;

    for (i32 y = 0; y < ih; y++) {
        i32 ym1 = coord_mode(y - 1, ih, wrap);
        i32 yp1 = coord_mode(y + 1, ih, wrap);
        for (i32 x = 0; x < iw; x++) {
            i32 xm1 = coord_mode(x - 1, iw, wrap);
            i32 xp1 = coord_mode(x + 1, iw, wrap);

            f32 tl = px_luma(src_rgba[(usize)ym1 * w + (usize)xm1]);
            f32 tc = px_luma(src_rgba[(usize)ym1 * w + (usize)x]);
            f32 tr = px_luma(src_rgba[(usize)ym1 * w + (usize)xp1]);
            f32 ml = px_luma(src_rgba[(usize)y   * w + (usize)xm1]);
            f32 mr = px_luma(src_rgba[(usize)y   * w + (usize)xp1]);
            f32 bl = px_luma(src_rgba[(usize)yp1 * w + (usize)xm1]);
            f32 bc = px_luma(src_rgba[(usize)yp1 * w + (usize)x]);
            f32 br = px_luma(src_rgba[(usize)yp1 * w + (usize)xp1]);

            f32 gx = (tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl);
            f32 gy = (bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr);

            Vec3 n = vec3_norm(vec3(-gx * strength, -gy * strength, 1.0f));
            u8 nr = clamp_u8((n.x * 0.5f + 0.5f) * 255.0f);
            u8 ng = clamp_u8((n.y * 0.5f + 0.5f) * 255.0f);
            u8 nb = clamp_u8((n.z * 0.5f + 0.5f) * 255.0f);
            dst_rgba[(usize)y * w + (usize)x] = FB_RGBA(nr, ng, nb, 255u);
        }
    }
}

void img_color_grade(u32* px, u32 w, u32 h,
                     f32 brightness, f32 contrast, f32 saturation, f32 gamma) {
    u32 count = w * h;
    f32 inv_gamma = (gamma > 0.001f) ? (1.0f / gamma) : 1.0f;
    b32 use_gamma = (m_abs(gamma - 1.0f) > 0.001f);

    for (u32 i = 0; i < count; i++) {
        u32 c = px[i];
        f32 r = (f32)((c >>  0) & 0xFFu) * (1.0f / 255.0f);
        f32 g = (f32)((c >>  8) & 0xFFu) * (1.0f / 255.0f);
        f32 b = (f32)((c >> 16) & 0xFFu) * (1.0f / 255.0f);
        u8  a = (u8)((c >> 24) & 0xFFu);

        /* Parlaklık ve kontrast */
        r = (r - 0.5f) * contrast + 0.5f + brightness;
        g = (g - 0.5f) * contrast + 0.5f + brightness;
        b = (b - 0.5f) * contrast + 0.5f + brightness;

        /* Doygunluk */
        f32 luma = 0.299f * r + 0.587f * g + 0.114f * b;
        r = m_lerp(luma, r, saturation);
        g = m_lerp(luma, g, saturation);
        b = m_lerp(luma, b, saturation);

        r = m_clamp(r, 0.0f, 1.0f);
        g = m_clamp(g, 0.0f, 1.0f);
        b = m_clamp(b, 0.0f, 1.0f);

        if (use_gamma) {
            r = r > 0.0f ? m_pow(r, inv_gamma) : 0.0f;
            g = g > 0.0f ? m_pow(g, inv_gamma) : 0.0f;
            b = b > 0.0f ? m_pow(b, inv_gamma) : 0.0f;
        }

        px[i] = FB_RGBA(clamp_u8(r * 255.0f),
                        clamp_u8(g * 255.0f),
                        clamp_u8(b * 255.0f),
                        a);
    }
}

void img_premultiply_alpha(u32* px, u32 w, u32 h) {
    u32 count = w * h;
    for (u32 i = 0; i < count; i++) {
        u32 c = px[i];
        u32 r = (c >>  0) & 0xFFu;
        u32 g = (c >>  8) & 0xFFu;
        u32 b = (c >> 16) & 0xFFu;
        u32 a = (c >> 24) & 0xFFu;

        r = (r * a + 127u) / 255u;
        g = (g * a + 127u) / 255u;
        b = (b * a + 127u) / 255u;
        px[i] = FB_RGBA(r, g, b, a);
    }
}
