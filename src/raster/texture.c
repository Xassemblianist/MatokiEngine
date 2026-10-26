/* raster/texture.c — Doku ilklendirme ve prosedürel doku üretimi implementasyonu.
 * Genişlik ve yüksekliğin 2'nin kuvveti olduğu ASSERT ile doğrulanır.
 * Sarma (wrap) maskeleri ve log2_w alanı hızlı örnekleme için önceden hesaplanır.
 */
#include "raster/texture.h"
#include "raster/framebuffer.h"
#include "core/log.h"
#include "core/rng.h"

static u32 u32_log2_pow2(u32 v) {
    u32 r = 0;
    while ((v >> r) > 1u) {
        r++;
    }
    return r;
}

void tex_init(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags) {
    ASSERT(tex != NULL);
    ASSERT(arena != NULL);
    ASSERT(IS_POW2(w) && IS_POW2(h));

    tex->px     = (u32*)arena_push(arena, (usize)w * (usize)h * sizeof(u32), 16);
    tex->w      = w;
    tex->h      = h;
    tex->w_mask = w - 1u;
    tex->h_mask = h - 1u;
    tex->log2_w = u32_log2_pow2(w);
    tex->flags  = flags;
    tex->levels = 1;
}

u32 tex_mip_offset(const Texture* tex, u32 level) {
    u32 wh4 = (tex->w * tex->h) << 2;
    return (wh4 - (wh4 >> (2u * level))) * 0xAAAAAAABu;   /* Σ_{i<level} wi*hi = (4wh - 4wh/4^level) / 3 */
}

void tex_init_mipmapped(Texture* tex, Arena* arena, u32 w, u32 h, u32 flags) {
    ASSERT(tex != NULL && arena != NULL);
    ASSERT(IS_POW2(w) && IS_POW2(h));
    u32 lw = u32_log2_pow2(w), lh = u32_log2_pow2(h);
    u32 levels = 1u + (lw < lh ? lw : lh);
    tex->w      = w;
    tex->h      = h;
    tex->w_mask = w - 1u;
    tex->h_mask = h - 1u;
    tex->log2_w = lw;
    tex->flags  = flags | TEX_FLAG_MIPMAP;
    tex->levels = levels;
    tex->px     = (u32*)arena_push(arena, (usize)tex_mip_offset(tex, levels) * sizeof(u32), 16);
}

void tex_build_mips(Texture* tex) {
    ASSERT(tex != NULL && tex->px != NULL);
    for (u32 k = 1; k < tex->levels; k++) {
        const u32* src = tex->px + tex_mip_offset(tex, k - 1u);
        u32*       dst = tex->px + tex_mip_offset(tex, k);
        u32 sw = tex->w >> (k - 1u);
        u32 dw = tex->w >> k, dh = tex->h >> k;
        for (u32 y = 0; y < dh; y++) {
            for (u32 x = 0; x < dw; x++) {
                const u32* s0 = src + (2u * y) * sw + 2u * x;
                const u32* s1 = s0 + sw;
                u32 out = 0;
                for (u32 c = 0; c < 32; c += 8) {
                    u32 sum = ((s0[0] >> c) & 0xFFu) + ((s0[1] >> c) & 0xFFu) +
                              ((s1[0] >> c) & 0xFFu) + ((s1[1] >> c) & 0xFFu) + 2u;
                    out |= (sum >> 2) << c;
                }
                dst[y * dw + x] = out;
            }
        }
    }
}

Texture tex_create(Arena* arena, u32 w, u32 h, u32 flags) {
    Texture tex;
    tex_init(&tex, arena, w, h, flags);
    return tex;
}

void tex_gen_checker(Texture* tex, u32 cell_size, u32 color_a, u32 color_b) {
    ASSERT(tex != NULL && tex->px != NULL);
    if (cell_size == 0) cell_size = 1;

    u32 w = tex->w;
    u32 h = tex->h;
    for (u32 y = 0; y < h; y++) {
        u32 cy = y / cell_size;
        u32* row = tex->px + y * w;
        for (u32 x = 0; x < w; x++) {
            u32 cx = x / cell_size;
            row[x] = ((cx ^ cy) & 1u) ? color_b : color_a;
        }
    }
}

static u8 lerp_u8(u8 a, u8 b, f32 t) {
    f32 v = (f32)a + ((f32)b - (f32)a) * t;
    i32 iv = (i32)(v + 0.5f);
    if (iv < 0) iv = 0;
    if (iv > 255) iv = 255;
    return (u8)iv;
}

static u32 bilerp_rgba(u32 c00, u32 c10, u32 c01, u32 c11, f32 tx, f32 ty) {
    u8 r0 = lerp_u8((u8)(c00 & 0xFFu), (u8)(c10 & 0xFFu), tx);
    u8 g0 = lerp_u8((u8)((c00 >> 8) & 0xFFu), (u8)((c10 >> 8) & 0xFFu), tx);
    u8 b0 = lerp_u8((u8)((c00 >> 16) & 0xFFu), (u8)((c10 >> 16) & 0xFFu), tx);
    u8 a0 = lerp_u8((u8)((c00 >> 24) & 0xFFu), (u8)((c10 >> 24) & 0xFFu), tx);

    u8 r1 = lerp_u8((u8)(c01 & 0xFFu), (u8)(c11 & 0xFFu), tx);
    u8 g1 = lerp_u8((u8)((c01 >> 8) & 0xFFu), (u8)((c11 >> 8) & 0xFFu), tx);
    u8 b1 = lerp_u8((u8)((c01 >> 16) & 0xFFu), (u8)((c11 >> 16) & 0xFFu), tx);
    u8 a1 = lerp_u8((u8)((c01 >> 24) & 0xFFu), (u8)((c11 >> 24) & 0xFFu), tx);

    u8 r = lerp_u8(r0, r1, ty);
    u8 g = lerp_u8(g0, g1, ty);
    u8 b = lerp_u8(b0, b1, ty);
    u8 a = lerp_u8(a0, a1, ty);
    return FB_RGBA(r, g, b, a);
}

void tex_gen_gradient(Texture* tex, u32 c_tl, u32 c_tr, u32 c_bl, u32 c_br) {
    ASSERT(tex != NULL && tex->px != NULL);
    u32 w = tex->w;
    u32 h = tex->h;
    f32 inv_w = (w > 1u) ? (1.0f / (f32)(w - 1u)) : 0.0f;
    f32 inv_h = (h > 1u) ? (1.0f / (f32)(h - 1u)) : 0.0f;

    for (u32 y = 0; y < h; y++) {
        f32 ty = (f32)y * inv_h;
        u32* row = tex->px + y * w;
        for (u32 x = 0; x < w; x++) {
            f32 tx = (f32)x * inv_w;
            row[x] = bilerp_rgba(c_tl, c_tr, c_bl, c_br, tx, ty);
        }
    }
}

void tex_gen_noise(Texture* tex, u64 seed) {
    ASSERT(tex != NULL && tex->px != NULL);
    Rng rng;
    rng_seed(&rng, seed);

    u32 w = tex->w;
    u32 h = tex->h;
    for (u32 y = 0; y < h; y++) {
        u32* row = tex->px + y * w;
        for (u32 x = 0; x < w; x++) {
            u32 rv = rng_u32(&rng);
            u8 r = (u8)(rv & 0xFFu);
            u8 g = (u8)((rv >> 8) & 0xFFu);
            u8 b = (u8)((rv >> 16) & 0xFFu);
            row[x] = FB_RGBA(r, g, b, 255);
        }
    }
}
