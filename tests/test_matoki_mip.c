/* tests/test_matoki_mip.c — Mip zinciri yerleşimi, seviye seçimi ve Matoki @mipmap örneklemesinin
 * C referansıyla (ref_tex_mip_level + ref_sample_tex_level) bit-bit eşitliği.
 * Matoki tarafı ir_eval ile çalıştırılır; ir_eval ↔ wasm codegen eşitliği test_matoki_back'te
 * (DERIV ve mip'li sample içeren 500 rastgele programla) ayrıca doğrulanır.
 */
#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "matoki/matoki.h"
#include "matoki/codegen.h"
#include "raster/shade_abi.h"
#include "raster/texture.h"
#include "raster/ref_shaders.h"
#include <string.h>

static u8 g_mem[MB(24)];

TEST(mip_chain_layout) {
    Arena a;
    arena_init(&a, g_mem, MB(4));
    static const u32 sizes[][2] = { {1, 1}, {4, 4}, {64, 64}, {256, 16}, {16, 256}, {128, 32} };
    for (u32 i = 0; i < ARRAY_LEN(sizes); i++) {
        Texture t;
        tex_init_mipmapped(&t, &a, sizes[i][0], sizes[i][1], 0);
        u32 lw = 0, lh = 0;
        while ((sizes[i][0] >> lw) > 1u) lw++;
        while ((sizes[i][1] >> lh) > 1u) lh++;
        CHECK(t.levels == 1u + MIN(lw, lh));
        CHECK(t.flags & TEX_FLAG_MIPMAP);
        u32 sum = 0;
        for (u32 k = 0; k <= t.levels; k++) {
            CHECK(tex_mip_offset(&t, k) == sum);
            if (k < t.levels) sum += (t.w >> k) * (t.h >> k);
        }
        /* Görünüm alanları */
        for (u32 k = 0; k < t.levels; k++) {
            Texture v = ref_tex_level_view(&t, (i32)k);
            CHECK(v.w == (t.w >> k) && v.h == (t.h >> k));
            CHECK(v.px == t.px + tex_mip_offset(&t, k));
            CHECK((1u << v.log2_w) == v.w);
        }
    }

    /* Kutu filtresi: düz renk zincir boyunca korunur */
    Texture t;
    tex_init_mipmapped(&t, &a, 32, 8, 0);
    for (u32 i = 0; i < 32u * 8u; i++) t.px[i] = 0x80402010u;
    tex_build_mips(&t);
    for (u32 k = 1; k < t.levels; k++) {
        CHECK(t.px[tex_mip_offset(&t, k)] == 0x80402010u);
    }
}

TEST(mip_level_selection) {
    Arena a;
    arena_init(&a, g_mem, MB(4));
    Texture t;
    tex_init_mipmapped(&t, &a, 256, 256, 0);
    /* Ekranda bir piksel s texel kapsıyorsa seviye floor(log2 s) */
    static const f32 scales[] = { 0.25f, 1.0f, 1.5f, 2.0f, 3.9f, 4.0f, 8.0f, 100.0f, 255.0f, 4096.0f };
    static const i32 expect[] = { 0,     0,    0,    1,    1,    2,    3,    6,      7,      8 };
    for (u32 i = 0; i < ARRAY_LEN(scales); i++) {
        f32 d = scales[i] / 256.0f;
        f32 u[4] = { 0.3f, 0.3f + d, 0.3f, 0.3f + d };
        f32 v[4] = { 0.6f, 0.6f, 0.6f + d * 0.5f, 0.6f + d * 0.5f };
        CHECK(ref_tex_mip_level(&t, u, v) == expect[i]);
    }
    /* NaN ve eşit lane'ler → seviye 0 */
    f32 nan = __builtin_nanf("");
    f32 un[4] = { nan, nan, nan, nan }, vz[4] = { 0, 0, 0, 0 };
    CHECK(ref_tex_mip_level(&t, un, vz) == 0);
    CHECK(ref_tex_mip_level(&t, vz, vz) == 0);
}

static b32 f32_same(f32 a, f32 b) {
    u32 x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y || (a != a && b != b);
}

/* Matoki @mipmap sample() ve sample_level() ↔ C referansı, 4 örnekleyici kombinasyonu × 25.000 quad */
TEST(mip_matoki_vs_ref_bit_exact) {
    static const char* srcs[4] = {
        "uniform t: texture2d @mipmap;\nin uv: vec2;\nfn frag() -> rgba { return sample(t, uv); }\n",
        "uniform t: texture2d @mipmap @bilinear;\nin uv: vec2;\nfn frag() -> rgba { return sample(t, uv); }\n",
        "uniform t: texture2d @mipmap @clamp;\nin uv: vec2;\nfn frag() -> rgba { return sample(t, uv); }\n",
        "uniform t: texture2d @mipmap @bilinear @clamp;\nin uv: vec2;\nfn frag() -> rgba { return sample(t, uv); }\n",
    };
    static const u32 flags_of[4] = {
        TEX_FLAG_MIPMAP, TEX_FLAG_MIPMAP | TEX_FLAG_BILINEAR,
        TEX_FLAG_MIPMAP | TEX_FLAG_CLAMP, TEX_FLAG_MIPMAP | TEX_FLAG_BILINEAR | TEX_FLAG_CLAMP,
    };
    Arena a;
    arena_init(&a, g_mem, MB(24));
    Rng rng;
    rng_seed(&rng, 99);

    Texture tex;
    tex_init_mipmapped(&tex, &a, 64, 32, 0);
    for (u32 i = 0; i < tex_mip_offset(&tex, tex.levels); i++) tex.px[i] = rng_u32(&rng);

    u32 mismatches = 0, total = 0, level_hist[8] = {0};
    for (u32 s = 0; s < 4; s++) {
        IrFunc ir;
        MtkLayout layout;
        MtkDiag diag;
        mtk_diag_init(&diag);
        CHECK(mtk_compile_ir(srcs[s], (u32)strlen(srcs[s]), "mip.mtk", &a, &ir, &layout, &diag));
        ir_optimize(&ir);
        Texture t = tex;
        t.flags = flags_of[s];

        enum { Q = 256 };
        QuadHeader quads[Q];
        f32 var[2 * Q * 4], out[4 * Q * 4];
        for (u32 rep = 0; rep < 25000 / Q + 1; rep++) {
            for (u32 q = 0; q < Q; q++) {
                quads[q].mask = 0xFu;
                quads[q].x = (u16)(2u * q);
                quads[q].y = 0;
                /* Rastgele merkez + 1/256..64 texel/piksel ölçek, eğik türevler */
                f32 cu = rng_f32(&rng) * 4.0f - 2.0f, cv = rng_f32(&rng) * 4.0f - 2.0f;
                f32 sc = m_exp2(rng_f32(&rng) * 16.0f - 7.0f) / 64.0f;
                f32 ax = (rng_f32(&rng) - 0.5f) * sc, ay = (rng_f32(&rng) - 0.5f) * sc;
                f32 bx = (rng_f32(&rng) - 0.5f) * sc, by = (rng_f32(&rng) - 0.5f) * sc;
                for (u32 l = 0; l < 4; l++) {
                    f32 ox = (f32)(l & 1u), oy = (f32)(l >> 1);
                    var[(0 * Q + q) * 4 + l] = cu + ox * ax + oy * bx;
                    var[(1 * Q + q) * 4 + l] = cv + ox * ay + oy * by;
                }
            }
            ShadeBatch b;
            memset(&b, 0, sizeof(b));
            b.count = Q;
            b.varying_count = 2;
            b.quads = quads;
            b.varyings = var;
            b.out_rgba = out;
            b.textures = &t;
            ir_eval(&ir, &b);
            for (u32 q = 0; q < Q; q++) {
                f32 u4[4], v4[4];
                for (u32 l = 0; l < 4; l++) { u4[l] = var[q * 4 + l]; v4[l] = var[(Q + q) * 4 + l]; }
                i32 lvl = ref_tex_mip_level(&t, u4, v4);
                level_hist[lvl & 7]++;
                for (u32 l = 0; l < 4; l++) {
                    f32 ref[4];
                    ref_sample_tex_level(&t, u4[l], v4[l], lvl, ref);
                    for (u32 c = 0; c < 4; c++) {
                        mismatches += !f32_same(ref[c], out[(c * Q + q) * 4 + l]);
                    }
                    total++;
                }
            }
        }
    }
    CHECK(total >= 100000);
    CHECK(mismatches == 0);
    /* Testin tüm seviyeleri gerçekten kullandığından emin ol (64x32 → 6 seviye) */
    for (u32 k = 0; k < tex.levels; k++) CHECK(level_hist[k] > 100);
}
