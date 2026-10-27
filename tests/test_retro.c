/* tests/test_retro.c — Retro görüntü modu (src/gfx3d/retro.h/.c ve retro_wasm.c) testleri.
 * Tamsayı büyütme + ortalama + kenar temizleme + kanarya sınır denetimi, 5:5:5 + Bayer 4x4 gri
 * rampa hatası (<= 1/2 LSB) ve dither kapalı tablo testi, 16/256 renk palet 32^3 LUT <-> kaba kuvvet
 * birebir eşleşmesi, köşe yakalama, düşmanca girdiler, wasm SIMD 1000 rastgele kare bit-bit
 * eşdeğerliği ve 320x180 -> 1280x720 present benchmark ölçümünü kapsar.
 */
#define _DEFAULT_SOURCE
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "plat/platform.h"
#include "raster/framebuffer.h"
#include "gfx3d/retro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static u8 g_retro_mem[MB(32)];

/* Kanarya korumalı Framebuffer: px tamponunun hemen öncesine ve sonrasına nöbetçi kelimeler koyar */
#define CANARY_WORDS 64u
#define CANARY_VAL   0xCAFEBABEu

typedef struct {
    Framebuffer fb;
    u32*        raw;
    u32         total_px;
} GuardedFb;

static void guarded_fb_init(GuardedFb* g, Arena* a, u32 w, u32 h) {
    g->total_px = w * h;
    g->raw = ARENA_PUSH_ARRAY(a, u32, g->total_px + 2u * CANARY_WORDS);
    for (u32 i = 0u; i < CANARY_WORDS; i++) {
        g->raw[i] = CANARY_VAL;
        g->raw[CANARY_WORDS + g->total_px + i] = CANARY_VAL;
    }
    g->fb.w     = w;
    g->fb.h     = h;
    g->fb.px    = g->raw + CANARY_WORDS;
    g->fb.depth = NULL;
}

static b32 guarded_fb_check(const GuardedFb* g) {
    for (u32 i = 0u; i < CANARY_WORDS; i++) {
        if (g->raw[i] != CANARY_VAL) return FALSE;
        if (g->raw[CANARY_WORDS + g->total_px + i] != CANARY_VAL) return FALSE;
    }
    return TRUE;
}

TEST(retro_integer_scaling_and_bounds) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    /* 1) Tam bölünen boyut: 320x180 -> 1280x720 (s = 4) */
    {
        arena_reset(&a);
        GuardedFb src, dst;
        guarded_fb_init(&src, &a, 320, 180);
        guarded_fb_init(&dst, &a, 1280, 720);

        Rng rng;
        rng_seed(&rng, 0x1020304050607080ULL);
        for (u32 i = 0u; i < 320u * 180u; i++) {
            src.fb.px[i] = rng_u32(&rng) | 0xFF000000u;
        }
        fb_clear(&dst.fb, 0xDEADBEEFu);

        RetroParams p = retro_default_params();
        p.scale      = 4u;
        p.color_bits = 8u;
        p.dither     = FALSE;
        p.scanline   = 0.0f;
        p.crt_blur   = FALSE;
        retro_present(&src.fb, &dst.fb, &p);

        CHECK(guarded_fb_check(&src));
        CHECK(guarded_fb_check(&dst));

        u32 bad = 0u;
        for (u32 sy = 0u; sy < 180u; sy++) {
            for (u32 sx = 0u; sx < 320u; sx++) {
                u32 expected = src.fb.px[sy * 320u + sx];
                for (u32 dy = 0u; dy < 4u; dy++) {
                    for (u32 dx = 0u; dx < 4u; dx++) {
                        u32 got = dst.fb.px[(sy * 4u + dy) * 1280u + (sx * 4u + dx)];
                        if (got != expected) bad++;
                    }
                }
            }
        }
        CHECK(bad == 0u);
    }

    /* 2) Hedef kaynaktan büyük ve ölçeğe tam bölünmüyor: 17x13 -> 60x50 (s = 3)
     *    scaled_w = 51, scaled_h = 39 -> ox = (60-51)/2 = 4, oy = (50-39)/2 = 5 */
    {
        arena_reset(&a);
        GuardedFb src, dst;
        guarded_fb_init(&src, &a, 17, 13);
        guarded_fb_init(&dst, &a, 60, 50);

        for (u32 y = 0u; y < 13u; y++) {
            for (u32 x = 0u; x < 17u; x++) {
                src.fb.px[y * 17u + x] = FB_RGBA(x * 15u + 1u, y * 19u + 2u, (x + y) * 7u + 3u, 255u);
            }
        }
        fb_clear(&dst.fb, 0xDEADBEEFu);

        RetroParams p = retro_default_params();
        p.scale      = 3u;
        p.color_bits = 8u;
        p.dither     = FALSE;
        p.scanline   = 0.0f;
        retro_present(&src.fb, &dst.fb, &p);

        CHECK(guarded_fb_check(&dst));

        i32 ox = (60 - 17 * 3) / 2; /* 4 */
        i32 oy = (50 - 13 * 3) / 2; /* 5 */
        u32 bad_border = 0u, bad_block = 0u;
        for (i32 y = 0; y < 50; y++) {
            for (i32 x = 0; x < 60; x++) {
                u32 got = dst.fb.px[y * 60 + x];
                if (x >= ox && x < ox + 17 * 3 && y >= oy && y < oy + 13 * 3) {
                    u32 sx = (u32)((x - ox) / 3);
                    u32 sy = (u32)((y - oy) / 3);
                    if (got != src.fb.px[sy * 17u + sx]) bad_block++;
                } else {
                    if (got != 0xFF000000u) bad_border++;
                }
            }
        }
        CHECK(bad_border == 0u);
        CHECK(bad_block == 0u);
    }

    /* 3) Hedef kaynaktan küçük / ölçeklenmiş görüntü hedeften taşar: 40x30 -> 50x25, s = 2
     *    scaled_w = 80 > 50 (ox = -15), scaled_h = 60 > 25 (oy = -17) */
    {
        arena_reset(&a);
        GuardedFb src, dst;
        guarded_fb_init(&src, &a, 40, 30);
        guarded_fb_init(&dst, &a, 50, 25);

        for (u32 y = 0u; y < 30u; y++) {
            for (u32 x = 0u; x < 40u; x++) {
                src.fb.px[y * 40u + x] = FB_RGBA(x * 6u, y * 8u, 128u, 255u);
            }
        }
        fb_clear(&dst.fb, 0xDEADBEEFu);

        RetroParams p = retro_default_params();
        p.scale      = 2u;
        p.color_bits = 8u;
        p.dither     = FALSE;
        p.scanline   = 0.0f;
        retro_present(&src.fb, &dst.fb, &p);

        CHECK(guarded_fb_check(&src));
        CHECK(guarded_fb_check(&dst));

        i32 ox = (50 - 80) / 2; /* -15 */
        i32 oy = (25 - 60) / 2; /* -17 */
        u32 bad = 0u;
        for (i32 y = 0; y < 25; y++) {
            for (i32 x = 0; x < 50; x++) {
                u32 sx = (u32)((x - ox) / 2);
                u32 sy = (u32)((y - oy) / 2);
                u32 expected = src.fb.px[sy * 40u + sx];
                if (dst.fb.px[y * 50 + x] != expected) bad++;
            }
        }
        CHECK(bad == 0u);
    }

    /* 4) inner_w / inner_h alt dikdörtgen ve yerinde (src == dst) present */
    {
        arena_reset(&a);
        GuardedFb src, dst, inplace;
        guarded_fb_init(&src, &a, 64, 64);
        guarded_fb_init(&dst, &a, 64, 64);
        guarded_fb_init(&inplace, &a, 64, 64);

        for (u32 i = 0u; i < 64u * 64u; i++) {
            u32 col = FB_RGBA((i * 37u) & 255u, (i * 73u) & 255u, (i * 19u) & 255u, 255u);
            src.fb.px[i] = col;
            inplace.fb.px[i] = col;
        }

        RetroParams p = retro_default_params();
        p.inner_w    = 20u;
        p.inner_h    = 16u;
        p.scale      = 2u;
        p.color_bits = 5u;
        p.dither     = TRUE;
        p.scanline   = 0.25f;
        retro_present(&src.fb, &dst.fb, &p);

        CHECK(guarded_fb_check(&dst));
        /* Merkeze 40x32 oturmalı: ox = 12, oy = 16; dışarısı siyah olmalı */
        CHECK(dst.fb.px[0] == 0xFF000000u);
        CHECK(dst.fb.px[63 * 64 + 63] == 0xFF000000u);

        /* inner_w/inner_h + scale=2 yerinde (src == dst) ile ayrı dst karşılaştırması */
        retro_present(&inplace.fb, &inplace.fb, &p);
        CHECK(guarded_fb_check(&inplace));
        CHECK(memcmp(dst.fb.px, inplace.fb.px, 64u * 64u * sizeof(u32)) == 0);

        /* Tam boyut 1x yerinde (src == dst) ile ayrı dst karşılaştırması */
        for (u32 i = 0u; i < 64u * 64u; i++) {
            inplace.fb.px[i] = src.fb.px[i];
        }
        p.inner_w = 0u;
        p.inner_h = 0u;
        p.scale   = 1u;
        retro_present(&src.fb, &dst.fb, &p);
        retro_present(&inplace.fb, &inplace.fb, &p);
        CHECK(guarded_fb_check(&inplace));
        CHECK(memcmp(dst.fb.px, inplace.fb.px, 64u * 64u * sizeof(u32)) == 0);
    }
}

TEST(retro_555_bayer_and_nodither_table) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    /* 1) Dither kapalıyken 5:5:5 çıktısı bit-bit tablo ile birebir aynı olmalı */
    u8 expected_555[256];
    for (u32 c = 0u; c < 256u; c++) {
        u32 q5 = c >> 3;
        expected_555[c] = (u8)((q5 << 3) | (q5 >> 2));
    }

    Framebuffer src, dst;
    fb_init(&src, &a, 256, 4);
    fb_init(&dst, &a, 256, 4);
    for (u32 y = 0u; y < 4u; y++) {
        for (u32 x = 0u; x < 256u; x++) {
            u8 r = (u8)x;
            u8 g = (u8)(255u - x);
            u8 b = (u8)((x * 97u + y * 53u) & 0xFFu);
            src.px[y * 256u + x] = FB_RGBA(r, g, b, 123u);
        }
    }

    RetroParams p = retro_default_params();
    p.scale      = 1u;
    p.color_bits = 15u; /* 15-bit = 5:5:5 */
    p.dither     = FALSE;
    p.scanline   = 0.0f;
    p.crt_blur   = FALSE;
    retro_present(&src, &dst, &p);

    u32 mismatch = 0u;
    for (u32 y = 0u; y < 4u; y++) {
        for (u32 x = 0u; x < 256u; x++) {
            u32 in_px  = src.px[y * 256u + x];
            u32 out_px = dst.px[y * 256u + x];
            u32 exp_px = FB_RGBA(expected_555[in_px & 0xFFu],
                                 expected_555[(in_px >> 8) & 0xFFu],
                                 expected_555[(in_px >> 16) & 0xFFu],
                                 255u);
            if (out_px != exp_px) mismatch++;
        }
    }
    CHECK(mismatch == 0u);

    /* 2) 5:5:5 + Bayer 4x4: düz gri rampada (0..255) ortalama hata <= 1/2 LSB(5 bit) */
    Framebuffer ramp_src, ramp_dst;
    /* Her gri seviyesi g=0..255 için tam bir 4x4 Bayer bloğu (256*4 x 4) */
    fb_init(&ramp_src, &a, 256 * 4, 4);
    fb_init(&ramp_dst, &a, 256 * 4, 4);
    for (u32 g = 0u; g < 256u; g++) {
        u32 col = FB_RGBA(g, g, g, 255u);
        for (u32 y = 0u; y < 4u; y++) {
            for (u32 dx = 0u; dx < 4u; dx++) {
                ramp_src.px[y * (256u * 4u) + g * 4u + dx] = col;
            }
        }
    }

    p.color_bits = 5u;
    p.dither     = TRUE;
    retro_present(&ramp_src, &ramp_dst, &p);

    f64 total_block_err = 0.0;
    f64 total_pixel_err = 0.0;
    for (u32 g = 0u; g < 256u; g++) {
        u32 sum = 0u;
        for (u32 y = 0u; y < 4u; y++) {
            for (u32 dx = 0u; dx < 4u; dx++) {
                u32 out_px = ramp_dst.px[y * (256u * 4u) + g * 4u + dx];
                u32 r = out_px & 0xFFu;
                u32 gr = (out_px >> 8) & 0xFFu;
                u32 b = (out_px >> 16) & 0xFFu;
                /* Gri girdi için R, G, B kanalları birbirine eşit olmalı */
                CHECK(r == gr && gr == b);
                sum += r;
                f64 pe = (f64)r - (f64)g;
                if (pe < 0.0) pe = -pe;
                total_pixel_err += pe;
            }
        }
        f64 avg = (f64)sum / 16.0;
        f64 be  = avg - (f64)g;
        if (be < 0.0) be = -be;
        total_block_err += be;
    }

    f64 mean_block_err = total_block_err / 256.0;
    f64 mean_pixel_err = total_pixel_err / (256.0 * 16.0);
    f64 lsb_5bit       = 255.0 / 31.0;       /* ~8.2258 */
    f64 half_lsb_5bit  = 0.5 * lsb_5bit;     /* ~4.1129 */
    printf("  [5:5:5 + Bayer gri rampa] 4x4 blok ort. hata: %.4f (%.4f LSB) | piksel ort. hata: %.4f (%.4f LSB)\n",
           mean_block_err, mean_block_err / lsb_5bit, mean_pixel_err, mean_pixel_err / lsb_5bit);

    /* Hem 4x4 blok ortalaması hatası hem tekil piksel ortalama hatası <= 1/2 LSB(5 bit) */
    CHECK(mean_block_err <= half_lsb_5bit);
    CHECK(mean_pixel_err <= half_lsb_5bit);
    /* Sınır değerler: g=0 tam 0, g=255 tam 255 kalmalı */
    for (u32 y = 0u; y < 4u; y++) {
        for (u32 dx = 0u; dx < 4u; dx++) {
            CHECK((ramp_dst.px[y * 1024u + dx] & 0xFFu) == 0u);
            CHECK((ramp_dst.px[y * 1024u + 255u * 4u + dx] & 0xFFu) == 255u);
        }
    }
}

TEST(retro_palette_lut_16_and_256) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    Rng rng;
    rng_seed(&rng, 0xA5A5F0F012345678ULL);

    u32 pal16_cols[16];
    for (u32 i = 0u; i < 16u; i++) {
        pal16_cols[i] = rng_u32(&rng) | 0xFF000000u;
    }

    u32 pal256_cols[256];
    for (u32 i = 0u; i < 256u; i++) {
        pal256_cols[i] = rng_u32(&rng) | 0xFF000000u;
    }

    RetroPalette* pal16  = ARENA_PUSH_STRUCT(&a, RetroPalette);
    RetroPalette* pal256 = ARENA_PUSH_STRUCT(&a, RetroPalette);
    retro_build_palette_lut(pal16, pal16_cols, 16u);
    retro_build_palette_lut(pal256, pal256_cols, 256u);

    CHECK(pal16->count == 16u);
    CHECK(pal256->count == 256u);

    u32 diff16  = 0u;
    u32 diff256 = 0u;
    for (u32 r5 = 0u; r5 < 32u; r5++) {
        u8 r8 = (u8)((r5 << 3) | (r5 >> 2));
        for (u32 g5 = 0u; g5 < 32u; g5++) {
            u8 g8 = (u8)((g5 << 3) | (g5 >> 2));
            for (u32 b5 = 0u; b5 < 32u; b5++) {
                u8 b8 = (u8)((b5 << 3) | (b5 >> 2));
                u32 idx = retro_lut_index(r5, g5, b5);

                u8 brute16 = retro_palette_nearest_brute(pal16_cols, 16u, r8, g8, b8);
                if (pal16->lut[idx] != brute16) diff16++;

                u8 brute256 = retro_palette_nearest_brute(pal256_cols, 256u, r8, g8, b8);
                if (pal256->lut[idx] != brute256) diff256++;
            }
        }
    }
    CHECK(diff16 == 0u);
    CHECK(diff256 == 0u);
}

TEST(retro_scanline_blur_and_snap) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    /* 1) Scanline karartma: çift satırlar tam parlaklık, tek satırlar (c * keep) >> 8 */
    Framebuffer src, dst;
    fb_init(&src, &a, 8, 4);
    fb_init(&dst, &a, 16, 8);
    fb_clear(&src, FB_RGBA(200, 120, 80, 255));

    RetroParams p = retro_default_params();
    p.scale      = 2u;
    p.color_bits = 8u;
    p.dither     = FALSE;
    p.scanline   = 0.5f; /* keep = 128 */
    p.crt_blur   = FALSE;
    retro_present(&src, &dst, &p);

    for (u32 y = 0u; y < 8u; y++) {
        u32 px = dst.px[y * 16u + 4u];
        if ((y & 1u) == 0u) {
            CHECK(px == FB_RGBA(200, 120, 80, 255));
        } else {
            CHECK(px == FB_RGBA(100, 60, 40, 255));
        }
    }

    /* 2) Yatay CRT bulanıklığı ([1, 6, 1]/8): tek darbe pikseli komşulara sızmalı */
    fb_clear(&src, FB_RGBA(0, 0, 0, 255));
    src.px[4] = FB_RGBA(240, 160, 80, 255);
    p.scale    = 1u;
    p.scanline = 0.0f;
    p.crt_blur = TRUE;
    Framebuffer blur_dst;
    fb_init(&blur_dst, &a, 8, 4);
    retro_present(&src, &blur_dst, &p);

    CHECK(blur_dst.px[2] == FB_RGBA(0, 0, 0, 255));
    CHECK(blur_dst.px[3] == FB_RGBA(30, 20, 10, 255));   /* (240 + 4) >> 3 = 30 */
    CHECK(blur_dst.px[4] == FB_RGBA(180, 120, 60, 255)); /* (6*240 + 4) >> 3 = 180 */
    CHECK(blur_dst.px[5] == FB_RGBA(30, 20, 10, 255));

    /* 3) Köşe yakalama (retro_snap_xy) */
    f32 x = 12.37f, y = -7.62f;
    retro_snap_xy(&x, &y, 1u);
    CHECK_NEAR(x, 12.0f, 1e-6);
    CHECK_NEAR(y, -8.0f, 1e-6);

    x = 12.37f; y = -7.62f;
    retro_snap_xy(&x, &y, 2u);
    CHECK_NEAR(x, 12.5f, 1e-6);
    CHECK_NEAR(y, -7.5f, 1e-6);

    x = 12.37f; y = -7.62f;
    retro_snap_xy(&x, &y, 4u);
    CHECK_NEAR(x, 12.25f, 1e-6);
    CHECK_NEAR(y, -7.5f, 1e-6);
}

TEST(retro_hostile_inputs) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    GuardedFb src, dst;
    guarded_fb_init(&src, &a, 16, 16);
    guarded_fb_init(&dst, &a, 32, 32);
    fb_clear(&src.fb, FB_RGBA(90, 140, 210, 255));

    RetroParams p = retro_default_params();

    /* NULL işaretçiler */
    retro_present(NULL, NULL, NULL);
    retro_present(&src.fb, NULL, &p);
    retro_present(NULL, &dst.fb, &p);
    CHECK(dst.fb.px[0] == 0xFF000000u);

    /* Aşırı ölçek, aşırı inner_w/h, bozuk color_bits ve NaN/aşırı scanline */
    p.inner_w    = U32_MAX;
    p.inner_h    = U32_MAX;
    p.scale      = U32_MAX;
    p.color_bits = U32_MAX;
    p.scanline   = 0.0f / 0.0f; /* NaN */
    p.crt_blur   = TRUE;
    retro_present(&src.fb, &dst.fb, &p);
    CHECK(guarded_fb_check(&src));
    CHECK(guarded_fb_check(&dst));

    p.scale    = 1000000u;
    p.scanline = -999.0f;
    retro_present(&src.fb, &dst.fb, &p);
    CHECK(guarded_fb_check(&dst));

    p.scale    = 2u;
    p.scanline = 999.0f;
    retro_present(&src.fb, &dst.fb, &p);
    CHECK(guarded_fb_check(&dst));

    /* Aşırı boyutlu sahte Framebuffer (taşma koruması) */
    Framebuffer huge_fb = { src.fb.px, NULL, U32_MAX, U32_MAX };
    retro_present(&huge_fb, &dst.fb, &p);
    retro_present(&src.fb, &huge_fb, &p);
    CHECK(guarded_fb_check(&dst));

    /* RETRO_ROW_BUF_CAP (2048) üstü genişlikte yedek yol testi */
    GuardedFb wide_src, wide_dst;
    guarded_fb_init(&wide_src, &a, 2200, 2);
    guarded_fb_init(&wide_dst, &a, 2200, 2);
    fb_clear(&wide_src.fb, FB_RGBA(123, 231, 45, 255));
    p = retro_default_params();
    p.scale    = 1u;
    p.crt_blur = TRUE;
    retro_present(&wide_src.fb, &wide_dst.fb, &p);
    CHECK(guarded_fb_check(&wide_src));
    CHECK(guarded_fb_check(&wide_dst));

    /* Palet düşmanca girdileri */
    RetroPalette* pal = ARENA_PUSH_STRUCT(&a, RetroPalette);
    u32 col = 0xFF112233u;
    retro_build_palette_lut(NULL, &col, 1u);
    retro_build_palette_lut(pal, NULL, 500u);
    CHECK(pal->count == 0u);
    retro_build_palette_lut(pal, &col, 0u);
    CHECK(pal->count == 0u);
    CHECK(retro_palette_nearest_brute(NULL, 10u, 0, 0, 0) == 0u);

    /* snap_xy düşmanca girdileri */
    retro_snap_xy(NULL, NULL, 0u);
    f32 nan_v = 0.0f / 0.0f;
    f32 huge_v = 1e30f;
    retro_snap_xy(&nan_v, &huge_v, U32_MAX);
    CHECK(nan_v == 0.0f);
    CHECK(huge_v == 1e30f);
}

/* Wasm SIMD (retro_wasm.c) ile Host skaler (retro.c) arasında 1000 rastgele kareyi
 * bit-bit karşılaştırır; ayrıca wasm SIMD üzerinde 320x180 -> 1280x720 süresini ölçer. */
TEST(retro_wasm_simd_1000_frames_bit_exact) {
    char tmp_dir[] = "/tmp/matoki_retro_wasm_XXXXXX";
    CHECK(mkdtemp(tmp_dir) != NULL);

    char c_path[256], wasm_path[256], host_path[256], ref_bin[256], mjs_path[256], cmd[1024];
    snprintf(c_path, sizeof(c_path), "%s/retro_harness.c", tmp_dir);
    snprintf(wasm_path, sizeof(wasm_path), "%s/retro_harness.wasm", tmp_dir);
    snprintf(host_path, sizeof(host_path), "%s/retro_harness_host", tmp_dir);
    snprintf(ref_bin, sizeof(ref_bin), "%s/ref_hashes.bin", tmp_dir);
    snprintf(mjs_path, sizeof(mjs_path), "%s/run_wasm.mjs", tmp_dir);

    const char* harness_src =
        "#include \"core/types.h\"\n"
        "#include \"core/arena.h\"\n"
        "#include \"core/math.h\"\n"
        "#include \"core/rng.h\"\n"
        "#include \"plat/platform.h\"\n"
        "#include \"raster/framebuffer.h\"\n"
        "#include \"gfx3d/retro.h\"\n"
        "\n"
        "static u8 g_mem[MB(24)];\n"
        "static u64 g_hashes[1000];\n"
        "static u32 g_sample_fb[128 * 128];\n"
        "\n"
        "static u64 fnv1a_u32(const u32* data, u32 count) {\n"
        "    u64 h = 14695981039346656037ULL;\n"
        "    const u8* bytes = (const u8*)data;\n"
        "    usize len = (usize)count * sizeof(u32);\n"
        "    for (usize i = 0; i < len; i++) {\n"
        "        h ^= (u64)bytes[i];\n"
        "        h *= 1099511628211ULL;\n"
        "    }\n"
        "    return h;\n"
        "}\n"
        "\n"
        "static void run_1000_frames(void) {\n"
        "    Arena perm, frame;\n"
        "    arena_init(&perm, g_mem, MB(4));\n"
        "    arena_init(&frame, g_mem + MB(4), MB(20));\n"
        "\n"
        "    Rng rng;\n"
        "    rng_seed(&rng, 0xCAFEBEEF13572468ULL);\n"
        "\n"
        "    u32 pal16_c[16], pal256_c[256];\n"
        "    for (u32 i = 0; i < 16; i++) pal16_c[i] = rng_u32(&rng) | 0xFF000000u;\n"
        "    for (u32 i = 0; i < 256; i++) pal256_c[i] = rng_u32(&rng) | 0xFF000000u;\n"
        "    RetroPalette* pal16 = ARENA_PUSH_STRUCT(&perm, RetroPalette);\n"
        "    RetroPalette* pal256 = ARENA_PUSH_STRUCT(&perm, RetroPalette);\n"
        "    retro_build_palette_lut(pal16, pal16_c, 16);\n"
        "    retro_build_palette_lut(pal256, pal256_c, 256);\n"
        "\n"
        "    const u32 bit_choices[8] = { 1, 2, 3, 4, 5, 6, 8, 15 };\n"
        "\n"
        "    for (u32 it = 0; it < 1000; it++) {\n"
        "        arena_reset(&frame);\n"
        "        u32 sw = (u32)rng_range(&rng, 5, 68);\n"
        "        u32 sh = (u32)rng_range(&rng, 4, 52);\n"
        "        u32 dw = (u32)rng_range(&rng, 16, 140);\n"
        "        u32 dh = (u32)rng_range(&rng, 12, 110);\n"
        "\n"
        "        Framebuffer src, dst;\n"
        "        fb_init(&src, &frame, sw, sh);\n"
        "        fb_init(&dst, &frame, dw, dh);\n"
        "        for (u32 i = 0; i < sw * sh; i++) src.px[i] = rng_u32(&rng);\n"
        "        fb_clear(&dst, 0x11223344u);\n"
        "\n"
        "        RetroParams p = retro_default_params();\n"
        "        p.scale = (u32)rng_range(&rng, 0, 6);\n"
        "        p.color_bits = bit_choices[rng_u32(&rng) & 7u];\n"
        "        p.dither = (rng_u32(&rng) & 1u) ? TRUE : FALSE;\n"
        "        u32 pm = rng_u32(&rng) % 3u;\n"
        "        p.palette = (pm == 0u) ? NULL : (pm == 1u ? pal16 : pal256);\n"
        "        p.scanline = (f32)(rng_u32(&rng) % 5u) * 0.2f;\n"
        "        p.crt_blur = (rng_u32(&rng) & 1u) ? TRUE : FALSE;\n"
        "\n"
        "        retro_present(&src, &dst, &p);\n"
        "        g_hashes[it] = fnv1a_u32(dst.px, dw * dh);\n"
        "        if (it == 999) {\n"
        "            Framebuffer s2, d2;\n"
        "            fb_init(&s2, &frame, 32, 32);\n"
        "            fb_init(&d2, &frame, 128, 128);\n"
        "            for (u32 i = 0; i < 32 * 32; i++) s2.px[i] = rng_u32(&rng);\n"
        "            p.scale = 4; p.color_bits = 5; p.dither = TRUE; p.palette = NULL; p.scanline = 0.25f; p.crt_blur = TRUE;\n"
        "            retro_present(&s2, &d2, &p);\n"
        "            __builtin_memcpy(g_sample_fb, d2.px, sizeof(g_sample_fb));\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "#if defined(__wasm32__)\n"
        "EXPORT(run_test) u64* run_test(void) { run_1000_frames(); return g_hashes; }\n"
        "EXPORT(get_sample) u32* get_sample(void) { return g_sample_fb; }\n"
        "static f64 run_bench(u32 iters, u32 mode) {\n"
        "    Arena a;\n"
        "    arena_init(&a, g_mem, MB(24));\n"
        "    Framebuffer src, dst;\n"
        "    fb_init(&src, &a, 320, 180);\n"
        "    fb_init(&dst, &a, 1280, 720);\n"
        "    Rng rng; rng_seed(&rng, 42);\n"
        "    for (u32 i = 0; i < 320 * 180; i++) src.px[i] = rng_u32(&rng);\n"
        "    RetroPalette* pal = ARENA_PUSH_STRUCT(&a, RetroPalette);\n"
        "    u32 cols[256];\n"
        "    for (u32 i = 0; i < 256; i++) cols[i] = rng_u32(&rng) | 0xFF000000u;\n"
        "    retro_build_palette_lut(pal, cols, 256);\n"
        "    RetroParams p = retro_default_params();\n"
        "    p.scale = 4; p.color_bits = 5; p.dither = TRUE;\n"
        "    p.scanline = (mode >= 1) ? 0.25f : 0.0f;\n"
        "    p.crt_blur = (mode >= 2) ? TRUE : FALSE;\n"
        "    p.palette  = (mode == 3) ? pal : NULL;\n"
        "    for (u32 w = 0; w < 10; w++) retro_present(&src, &dst, &p);\n"
        "    f64 t0 = plat_time_ms();\n"
        "    for (u32 i = 0; i < iters; i++) retro_present(&src, &dst, &p);\n"
        "    return (plat_time_ms() - t0) / (f64)iters;\n"
        "}\n"
        "EXPORT(bench_wasm) f64 bench_wasm(u32 iters, u32 mode) { return run_bench(iters, mode); }\n"
        "#else\n"
        "#include <stdio.h>\n"
        "static f64 run_bench(u32 iters, u32 mode) {\n"
        "    Arena a;\n"
        "    arena_init(&a, g_mem, MB(24));\n"
        "    Framebuffer src, dst;\n"
        "    fb_init(&src, &a, 320, 180);\n"
        "    fb_init(&dst, &a, 1280, 720);\n"
        "    Rng rng; rng_seed(&rng, 42);\n"
        "    for (u32 i = 0; i < 320 * 180; i++) src.px[i] = rng_u32(&rng);\n"
        "    RetroPalette* pal = ARENA_PUSH_STRUCT(&a, RetroPalette);\n"
        "    u32 cols[256];\n"
        "    for (u32 i = 0; i < 256; i++) cols[i] = rng_u32(&rng) | 0xFF000000u;\n"
        "    retro_build_palette_lut(pal, cols, 256);\n"
        "    RetroParams p = retro_default_params();\n"
        "    p.scale = 4; p.color_bits = 5; p.dither = TRUE;\n"
        "    p.scanline = (mode >= 1) ? 0.25f : 0.0f;\n"
        "    p.crt_blur = (mode >= 2) ? TRUE : FALSE;\n"
        "    p.palette  = (mode == 3) ? pal : NULL;\n"
        "    for (u32 w = 0; w < 10; w++) retro_present(&src, &dst, &p);\n"
        "    f64 t0 = plat_time_ms();\n"
        "    for (u32 i = 0; i < iters; i++) retro_present(&src, &dst, &p);\n"
        "    return (plat_time_ms() - t0) / (f64)iters;\n"
        "}\n"
        "int main(int argc, char** argv) {\n"
        "    if (argc < 2) return 1;\n"
        "    run_1000_frames();\n"
        "    FILE* f = fopen(argv[1], \"wb\");\n"
        "    if (!f) return 1;\n"
        "    fwrite(g_hashes, sizeof(u64), 1000, f);\n"
        "    fwrite(g_sample_fb, sizeof(u32), 128 * 128, f);\n"
        "    fclose(f);\n"
        "    printf(\"  [bench M10_B host -O2] 320x180->1280x720: 555+bayer=%.3f ms | +scanline=%.3f ms | +blur=%.3f ms | pal256+scan+blur=%.3f ms\\n\",\n"
        "           run_bench(100, 0), run_bench(100, 1), run_bench(100, 2), run_bench(100, 3));\n"
        "    return 0;\n"
        "}\n"
        "#endif\n";

    FILE* fc = fopen(c_path, "wb");
    CHECK(fc != NULL);
    fwrite(harness_src, 1, strlen(harness_src), fc);
    fclose(fc);

    const char* mjs_src =
        "import fs from 'node:fs';\n"
        "const [wasmPath, refPath] = process.argv.slice(2);\n"
        "const refBuf = fs.readFileSync(refPath);\n"
        "const wasmBytes = fs.readFileSync(wasmPath);\n"
        "const mod = new WebAssembly.Module(wasmBytes);\n"
        "const inst = new WebAssembly.Instance(mod, {\n"
        "  env: {\n"
        "    plat_log: () => {},\n"
        "    plat_panic: () => { throw new Error('panic'); },\n"
        "    plat_time_ms: () => performance.now(),\n"
        "    plat_jit_load: () => -1,\n"
        "    plat_download: () => {}\n"
        "  }\n"
        "});\n"
        "const hashPtr = inst.exports.run_test();\n"
        "const samplePtr = inst.exports.get_sample();\n"
        "const wasmHashes = new BigUint64Array(inst.exports.memory.buffer, hashPtr, 1000);\n"
        "const wasmSample = new Uint32Array(inst.exports.memory.buffer, samplePtr, 128 * 128);\n"
        "const refView = new DataView(refBuf.buffer, refBuf.byteOffset, refBuf.byteLength);\n"
        "for (let i = 0; i < 1000; i++) {\n"
        "  const rh = refView.getBigUint64(i * 8, true);\n"
        "  if (rh !== wasmHashes[i]) {\n"
        "    console.error(`Frame ${i} hash mismatch: host=${rh.toString(16)} wasm=${wasmHashes[i].toString(16)}`);\n"
        "    process.exit(1);\n"
        "  }\n"
        "}\n"
        "for (let i = 0; i < 128 * 128; i++) {\n"
        "  const rp = refView.getUint32(8000 + i * 4, true);\n"
        "  if (rp !== wasmSample[i]) {\n"
        "    console.error(`Sample px ${i} mismatch`);\n"
        "    process.exit(1);\n"
        "  }\n"
        "}\n"
        "const m0 = inst.exports.bench_wasm(100, 0);\n"
        "const m1 = inst.exports.bench_wasm(100, 1);\n"
        "const m2 = inst.exports.bench_wasm(100, 2);\n"
        "const m3 = inst.exports.bench_wasm(100, 3);\n"
        "console.log(`  [bench M10_B wasm SIMD] 320x180->1280x720: 555+bayer=${m0.toFixed(3)} ms | +scanline=${m1.toFixed(3)} ms | +blur=${m2.toFixed(3)} ms | pal256+scan+blur=${m3.toFixed(3)} ms`);\n";

    FILE* fm = fopen(mjs_path, "wb");
    CHECK(fm != NULL);
    fwrite(mjs_src, 1, strlen(mjs_src), fm);
    fclose(fm);

    snprintf(cmd, sizeof(cmd),
             "clang -std=c11 -O2 -Isrc %s src/core/arena.c src/core/math.c src/core/rng.c "
             "src/core/log.c src/raster/framebuffer.c src/gfx3d/retro.c tools/host/plat_host.c -o %s && "
             "%s %s",
             c_path, host_path, host_path, ref_bin);
    CHECK(system(cmd) == 0);

    snprintf(cmd, sizeof(cmd),
             "clang --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib "
             "-msimd128 -mbulk-memory -mnontrapping-fptoint -msign-ext -Isrc "
             "-Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=67108864 "
             "-Wl,--stack-first -z stack-size=1048576 "
             "%s src/core/mem.c src/core/arena.c src/core/math.c src/core/rng.c src/core/log.c "
             "src/raster/framebuffer.c src/gfx3d/retro.c src/gfx3d/retro_wasm.c -o %s && "
             "node %s %s %s",
             c_path, wasm_path, mjs_path, wasm_path, ref_bin);
    CHECK(system(cmd) == 0);

    remove(c_path);
    remove(wasm_path);
    remove(host_path);
    remove(ref_bin);
    remove(mjs_path);
    rmdir(tmp_dir);
}

static f64 median_f64(f64* arr, u32 n) {
    for (u32 i = 1u; i < n; i++) {
        f64 k = arr[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && arr[j] > k) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = k;
    }
    return arr[n / 2u];
}

TEST(retro_bench_320x180_to_1280x720) {
    Arena a;
    arena_init(&a, g_retro_mem, sizeof(g_retro_mem));

    Framebuffer src, dst;
    fb_init(&src, &a, 320, 180);
    fb_init(&dst, &a, 1280, 720);

    Rng rng;
    rng_seed(&rng, 0x9876543210ABCDEFULL);
    for (u32 i = 0u; i < 320u * 180u; i++) {
        src.px[i] = rng_u32(&rng) | 0xFF000000u;
    }

    RetroPalette* pal = ARENA_PUSH_STRUCT(&a, RetroPalette);
    u32 cols[256];
    for (u32 i = 0u; i < 256u; i++) cols[i] = rng_u32(&rng) | 0xFF000000u;
    retro_build_palette_lut(pal, cols, 256u);

    RetroParams p = retro_default_params();
    p.scale      = 4u;
    p.color_bits = 5u;
    p.dither     = TRUE;
    p.scanline   = 0.25f;
    p.crt_blur   = FALSE;

    /* Isınma */
    for (u32 i = 0u; i < 5u; i++) retro_present(&src, &dst, &p);

    f64 samples_def[31], samples_blur[31], samples_pal[31];
    for (u32 s = 0u; s < 31u; s++) {
        p.palette  = NULL;
        p.crt_blur = FALSE;
        f64 t0 = plat_time_ms();
        retro_present(&src, &dst, &p);
        samples_def[s] = plat_time_ms() - t0;

        p.crt_blur = TRUE;
        t0 = plat_time_ms();
        retro_present(&src, &dst, &p);
        samples_blur[s] = plat_time_ms() - t0;

        p.palette = pal;
        t0 = plat_time_ms();
        retro_present(&src, &dst, &p);
        samples_pal[s] = plat_time_ms() - t0;
    }

    f64 med_def  = median_f64(samples_def, 31u);
    f64 med_blur = median_f64(samples_blur, 31u);
    f64 med_pal  = median_f64(samples_pal, 31u);
    printf("  [bench M10_B host skaler] 320x180->1280x720: 555+bayer+scan=%.3f ms | +blur=%.3f ms | +pal256=%.3f ms\n",
           med_def, med_blur, med_pal);
    CHECK(med_def >= 0.0 && med_blur >= 0.0 && med_pal >= 0.0);
}
