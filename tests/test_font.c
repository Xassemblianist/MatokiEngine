/* tests/test_font.c — TrueType ayrıştırıcı (ttf), işaretli alan rasterleyici (raster) ve
 * raf paketlemeli atlas (atlas) testleri + 50.000 mutasyon ASan fuzz + 16/32/64 px benchmark.
 */
#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "plat/platform.h"
#include "font/ttf.h"
#include "font/raster.h"
#include "font/atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 g_arena_mem[MB(32)];
static u8 g_ttf_buf[MB(2)];

static u32 read_bin_file(const char* path, u8* dst, u32 cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0u;
    u32 n = (u32)fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

TEST(ttf_parse_metrics_cmap_kern_composite) {
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, sizeof(g_ttf_buf));
    CHECK(n > 0u);
    if (n == 0u) return;

    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));
    CHECK(font.units_per_em == 1000u);
    CHECK(font.ascent == 800);
    CHECK(font.descent == -200);
    CHECK(font.line_gap == 100);
    CHECK(font.num_glyphs == 11u);
    CHECK(font.num_hmetrics == 10u);
    CHECK(font.index_to_loc_format == 0);
    CHECK(font.cmap4_off != 0u && font.cmap12_off != 0u);
    CHECK(font.kern0_pairs_off != 0u && font.kern0_pair_count == 5u);

    /* Cmap biçim 4 ve 12 eşlemeleri */
    CHECK(ttf_glyph_index(&font, ' ') == 1u);
    CHECK(ttf_glyph_index(&font, 'A') == 2u);
    CHECK(ttf_glyph_index(&font, 'B') == 3u);
    CHECK(ttf_glyph_index(&font, 'O') == 4u);
    CHECK(ttf_glyph_index(&font, 'C') == 5u);
    CHECK(ttf_glyph_index(&font, 'D') == 6u);
    CHECK(ttf_glyph_index(&font, 0x011Eu) == 7u); /* Ğ (cmap4 glyphIdArray) */
    CHECK(ttf_glyph_index(&font, 0x011Fu) == 8u); /* ğ (cmap4 glyphIdArray) */
    CHECK(ttf_glyph_index(&font, 0x0131u) == 9u); /* ı */
    CHECK(ttf_glyph_index(&font, 0x015Fu) == 9u); /* ş */
    CHECK(ttf_cmap4_lookup(&font, 0x011Eu) == 7u);
    CHECK(ttf_cmap4_lookup(&font, 0x011Fu) == 8u);
    CHECK(ttf_cmap12_lookup(&font, 0x011Eu) == 7u);
    CHECK(ttf_cmap4_lookup(&font, 0x1F600u) == 0u);
    CHECK(ttf_cmap12_lookup(&font, 0x1F600u) == 10u);
    CHECK(ttf_glyph_index(&font, 0x1F600u) == 10u);
    CHECK(ttf_glyph_index(&font, 'Z') == 0u);
    CHECK(ttf_glyph_index(&font, 0xFFFFu) == 0u);

    /* hmtx advance ve lsb (kuyruk lsb dahil) */
    u16 adv = 0u;
    i16 lsb = 0;
    CHECK(ttf_glyph_metrics(&font, 1u, &adv, &lsb) && adv == 300u && lsb == 0);
    CHECK(ttf_glyph_metrics(&font, 2u, &adv, &lsb) && adv == 1000u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 3u, &adv, &lsb) && adv == 800u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 6u, &adv, &lsb) && adv == 950u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 7u, &adv, &lsb) && adv == 850u && lsb == 80);
    CHECK(ttf_glyph_metrics(&font, 9u, &adv, &lsb) && adv == 650u && lsb == 50);
    CHECK(ttf_glyph_metrics(&font, 10u, &adv, &lsb) && adv == 650u && lsb == 45);
    CHECK(!ttf_glyph_metrics(&font, 11u, &adv, &lsb));

    /* Kern biçim 0 */
    CHECK(ttf_kern_advance(&font, 2u, 3u) == -80);
    CHECK(ttf_kern_advance(&font, 2u, 4u) == -50);
    CHECK(ttf_kern_advance(&font, 3u, 2u) == -30);
    CHECK(ttf_kern_advance(&font, 4u, 2u) == -40);
    CHECK(ttf_kern_advance(&font, 6u, 7u) == 25);
    CHECK(ttf_kern_advance(&font, 2u, 2u) == 0);

    /* Bileşik glif dönüşümleri */
    Arena a;
    arena_init(&a, g_arena_mem, MB(4));
    TtfGlyph g;

    /* Glif 1: boş */
    CHECK(ttf_load_glyph(&font, 1u, &a, &g));
    CHECK(g.contour_count == 0u && g.point_count == 0u && g.advance == 300u);

    /* Glif 6: ölçekli (0.5) kare + xy-ölçekli (0.5, 0.5) üçgen */
    CHECK(ttf_load_glyph(&font, 6u, &a, &g));
    CHECK(g.contour_count == 2u && g.point_count == 7u);
    CHECK(g.contour_ends[0] == 3u && g.contour_ends[1] == 6u);
    CHECK_NEAR(g.points[0].x, 100.0f, 1e-4);
    CHECK_NEAR(g.points[0].y, 100.0f, 1e-4);
    CHECK_NEAR(g.points[2].x, 400.0f, 1e-4);
    CHECK_NEAR(g.points[2].y, 400.0f, 1e-4);
    CHECK_NEAR(g.points[4].x, 450.0f, 1e-4);
    CHECK_NEAR(g.points[4].y, 100.0f, 1e-4);
    CHECK_NEAR(g.points[5].x, 650.0f, 1e-4);
    CHECK_NEAR(g.points[5].y, 500.0f, 1e-4);
    CHECK_NEAR(g.points[6].x, 850.0f, 1e-4);
    CHECK_NEAR(g.points[6].y, 100.0f, 1e-4);
    CHECK(g.xmin == 100 && g.ymin == 100 && g.xmax == 850 && g.ymax == 500);

    /* Glif 7: 2x2 matrisli (a=0.5, b=0, c=0.25, d=0.5, dx=80, dy=40) kare */
    CHECK(ttf_load_glyph(&font, 7u, &a, &g));
    CHECK(g.contour_count == 1u && g.point_count == 4u);
    CHECK_NEAR(g.points[0].x, 155.0f, 1e-3);
    CHECK_NEAR(g.points[0].y,  90.0f, 1e-3);
    CHECK_NEAR(g.points[1].x, 305.0f, 1e-3);
    CHECK_NEAR(g.points[1].y, 390.0f, 1e-3);
    CHECK_NEAR(g.points[2].x, 605.0f, 1e-3);
    CHECK_NEAR(g.points[2].y, 390.0f, 1e-3);
    CHECK_NEAR(g.points[3].x, 455.0f, 1e-3);
    CHECK_NEAR(g.points[3].y,  90.0f, 1e-3);
}

static f64 bitmap_area_sum(const u8* px, u32 w, u32 h) {
    u64 sum = 0u;
    for (u32 i = 0u; i < w * h; i++) sum += px[i];
    return (f64)sum / 255.0;
}

TEST(font_raster_square_subpixel_hole_area_triangle) {
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, sizeof(g_ttf_buf));
    CHECK(n > 0u);
    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));

    Arena a;
    arena_init(&a, g_arena_mem, MB(8));

    /* 1. Eksen hizalı kare (Glif 3: [100..700] x [100..700])
     * scale = 0.01 -> [1..7] x [-7..-1]. offset=(1, 9) ile 10x10 tamponda [2..8) x [2..8) */
    TtfGlyph sq;
    CHECK(ttf_load_glyph(&font, 3u, &a, &sq));
    u8 canvas[100];
    memset(canvas, 0xAA, sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&sq, 0.01f, 0.01f, 1.0f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (x >= 2u && x < 8u && y >= 2u && y < 8u) {
                CHECK(v == 255u);
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 36.0, 36.0 * 0.005);

    /* 2. Yarım piksel (+0.5) x kaydırmalı kare: [2.5 .. 8.5] x [2.0 .. 8.0] */
    memset(canvas, 0xAA, sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&sq, 0.01f, 0.01f, 1.5f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (y >= 2u && y < 8u) {
                if (x == 2u || x == 8u) {
                    CHECK(v >= 127u && v <= 128u);
                } else if (x >= 3u && x < 8u) {
                    CHECK(v == 255u);
                } else {
                    CHECK(v == 0u);
                }
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 36.0, 36.0 * 0.005);

    /* 3. Delikli 'O' konturu (Glif 4: dış [100..700]^2, iç delik [300..500]^2) */
    TtfGlyph hollow;
    CHECK(ttf_load_glyph(&font, 4u, &a, &hollow));
    memset(canvas, 0xAA, sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&hollow, 0.01f, 0.01f, 1.0f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (x >= 4u && x < 6u && y >= 4u && y < 6u) {
                CHECK(v == 0u); /* iç delik 0 */
            } else if (x >= 2u && x < 8u && y >= 2u && y < 8u) {
                CHECK(v == 255u);
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 32.0, 32.0 * 0.005);

    /* Delikli konturu küsuratlı ölçek ve alt piksel kaydırmayla da doğrula */
    u8 big[64 * 64];
    f32 sc = 0.043f;
    CHECK(font_rasterize_glyph(&hollow, sc, sc, 0.37f, 0.21f, big, 64u, 64u, 64u, &a));
    f64 expected_hollow = 320000.0 * (f64)sc * (f64)sc;
    CHECK_NEAR(bitmap_area_sum(big, 64u, 64u), expected_hollow, expected_hollow * 0.005);

    /* 4. İkinci derece Bezier eğrisi (Glif 5: analitik alan = 300000 * sc^2)
     * Hem doğrudan rasterleme hem font_flatten_glyph + font_rasterize_edges yolu doğrulanır */
    TtfGlyph bez;
    CHECK(ttf_load_glyph(&font, 5u, &a, &bez));
    memset(big, 0, sizeof(big));
    CHECK(font_rasterize_glyph(&bez, 0.05f, 0.05f, 0.25f, 0.0f, big, 64u, 64u, 64u, &a));
    f64 expected_bez = 300000.0 * 0.05 * 0.05; /* 750.0 px^2 */
    CHECK_NEAR(bitmap_area_sum(big, 64u, 64u), expected_bez, expected_bez * 0.005);

    FontEdge* edges = NULL;
    u32 edge_cnt = font_flatten_glyph(&bez, 0.05f, 0.05f, 2.0f, 40.0f, FONT_DEFAULT_FLATNESS, &a, &edges);
    CHECK(edge_cnt >= 16u && edges != NULL);
    u8 big2[64 * 64];
    memset(big2, 0, sizeof(big2));
    CHECK(font_rasterize_edges(edges, edge_cnt, big2, 64u, 64u, 64u, &a));
    CHECK_NEAR(bitmap_area_sum(big2, 64u, 64u), expected_bez, expected_bez * 0.005);

    /* 5. Üçgen (Glif 2: (100,0)->(500,800)->(900,0), analitik alan = 320000 * sc^2)
     * Kenar monotonluğu ve alan doğrulaması */
    TtfGlyph tri;
    CHECK(ttf_load_glyph(&font, 2u, &a, &tri));
    u8 tri_bmp[32 * 32];
    memset(tri_bmp, 0, sizeof(tri_bmp));
    f32 tsc = 0.025f; /* genişlik=20, yükseklik=20, tepe x=12.5, analitik alan = 200.0 */
    CHECK(font_rasterize_glyph_ex(&tri, tsc, tsc, 0.0f, 22.0f, tri_bmp, 32u, 32u, 32u, &a));
    f64 expected_tri = 320000.0 * (f64)tsc * (f64)tsc;
    CHECK_NEAR(bitmap_area_sum(tri_bmp, 32u, 32u), expected_tri, expected_tri * 0.005);

    /* Üçgenin her satırında soldan ortaya monoton artan, ortadan sağa monoton azalan;
     * ayrıca her sütunda yukarıdan aşağıya (y=2 tepe -> y=21 taban) monoton artan */
    for (u32 y = 2u; y < 22u; y++) {
        for (u32 x = 0u; x < 12u; x++) {
            CHECK(tri_bmp[y * 32u + x] <= tri_bmp[y * 32u + x + 1u]);
        }
        for (u32 x = 13u; x < 31u; x++) {
            CHECK(tri_bmp[y * 32u + x] >= tri_bmp[y * 32u + x + 1u]);
        }
    }
    for (u32 x = 0u; x < 32u; x++) {
        for (u32 y = 2u; y < 21u; y++) {
            CHECK(tri_bmp[y * 32u + x] <= tri_bmp[(y + 1u) * 32u + x]);
        }
    }
}

TEST(font_atlas_and_measure) {
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, sizeof(g_ttf_buf));
    CHECK(n > 0u);
    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(16));
    arena_init(&scratch_a, g_arena_mem + MB(16), MB(16));

    FontAtlas atlas;
    CHECK(font_atlas_build(&font, 20.0f, 256u, 256u, &out_a, &scratch_a, &atlas));
    CHECK(atlas.w == 256u && atlas.h == 256u);
    CHECK_NEAR(atlas.scale, 0.02f, 1e-6);

    /* Paketlenen glif kutularının birbirine çarpmadığını doğrula */
    for (u32 i = 0u; i < atlas.glyph_count; i++) {
        const FontGlyph* gi = &atlas.glyphs[i];
        if (gi->w == 0u || gi->h == 0u) continue;
        CHECK(gi->x + gi->w <= atlas.w && gi->y + gi->h <= atlas.h);
        CHECK(gi->u0 >= 0.0f && gi->u1 <= 1.0f && gi->u0 < gi->u1);
        CHECK(gi->v0 >= 0.0f && gi->v1 <= 1.0f && gi->v0 < gi->v1);
        for (u32 j = i + 1u; j < atlas.glyph_count; j++) {
            const FontGlyph* gj = &atlas.glyphs[j];
            if (gj->w == 0u || gj->h == 0u) continue;
            b32 overlap_x = (gi->x < gj->x + gj->w) && (gj->x < gi->x + gi->w);
            b32 overlap_y = (gi->y < gj->y + gj->h) && (gj->y < gi->y + gi->h);
            CHECK(!(overlap_x && overlap_y));
        }
    }

    /* Kerning ve UTF-8 metin ölçümü */
    f32 w = 0.0f, h = 0.0f;
    CHECK_NEAR(font_measure(&atlas, "A", &w, &h), 20.0f, 1e-4);
    CHECK_NEAR(w, 20.0f, 1e-4);
    CHECK_NEAR(h, 22.0f, 1e-4); /* (800 - (-200) + 100) * 0.02 = 22 */

    /* 'A' (1000) + 'B' (800) + kern(-80) = 1720 * 0.02 = 34.4 */
    CHECK_NEAR(font_measure(&atlas, "AB", NULL, NULL), 34.4f, 1e-4);
    /* 'D' (950) + 'Ğ' (850) + kern(+25) = 1825 * 0.02 = 36.5 */
    CHECK_NEAR(font_measure(&atlas, "D\xC4\x9E", NULL, NULL), 36.5f, 1e-4);
    /* "Ağış": 1000 + 750 + 650 + 650 = 3050 * 0.02 = 61.0 */
    CHECK_NEAR(font_measure(&atlas, "A\xC4\x9F\xC4\xB1\xC5\x9F", NULL, NULL), 61.0f, 1e-4);
}

TEST(font_dejavu_system_and_bench) {
    const char* sys_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    u32 n = read_bin_file(sys_path, g_ttf_buf, sizeof(g_ttf_buf));
    if (n == 0u) {
        printf("    [bilgi] DejaVuSans.ttf bulunamadi, sistem font testi atlandi.\n");
        return;
    }

    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));
    CHECK(font.num_glyphs > 1000u);

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(16));
    arena_init(&scratch_a, g_arena_mem + MB(16), MB(16));

    /* Cmap'teki tüm eşlenen glifleri topla ve 8..64 px aralığında rasterle */
    u8* seen = (u8*)calloc(font.num_glyphs, 1u);
    CHECK(seen != NULL);
    /* Cmap12 gruplarını (veya cmap4 aralığını) tarayarak tüm cmap gliflerini işaretle */
    if (font.cmap12_off != 0u) {
        const u8* sub = font.data + font.cmap12_off;
        u32 n_groups = ((u32)sub[12] << 24) | ((u32)sub[13] << 16) | ((u32)sub[14] << 8) | (u32)sub[15];
        for (u32 i = 0u; i < n_groups; i++) {
            const u8* g = sub + 16u + i * 12u;
            u32 sc = ((u32)g[0] << 24) | ((u32)g[1] << 16) | ((u32)g[2] << 8) | (u32)g[3];
            u32 ec = ((u32)g[4] << 24) | ((u32)g[5] << 16) | ((u32)g[6] << 8) | (u32)g[7];
            for (u32 cp = sc; cp <= ec; cp++) {
                u16 gid = ttf_glyph_index(&font, cp);
                if (gid < font.num_glyphs) seen[gid] = 1u;
            }
        }
    } else {
        for (u32 cp = 0u; cp <= 0xFFFFu; cp++) {
            u16 gid = ttf_glyph_index(&font, cp);
            if (gid < font.num_glyphs) seen[gid] = 1u;
        }
    }

    u8 bmp[256 * 256];
    u32 raster_count = 0u;
    for (u16 gid = 0u; gid < font.num_glyphs; gid++) {
        if (!seen[gid]) continue;
        /* 8..64 px aralığındaki tüm tamsayı boyutları glifler üzerinde döngüsel test et */
        f32 px_sz = (f32)(8u + (gid % 57u));
        f32 sc    = px_sz / (f32)font.units_per_em;
        usize mark = arena_mark(&scratch_a);
        TtfGlyph tg;
        CHECK(ttf_load_glyph(&font, gid, &scratch_a, &tg));
        i32 ix0, iy0, ix1, iy1;
        font_glyph_bbox(&tg, sc, sc, 0.0f, 0.0f, &ix0, &iy0, &ix1, &iy1);
        u32 gw = (ix1 > ix0) ? MIN((u32)(ix1 - ix0), 256u) : 0u;
        u32 gh = (iy1 > iy0) ? MIN((u32)(iy1 - iy0), 256u) : 0u;
        if (gw > 0u && gh > 0u) {
            CHECK(font_rasterize_glyph(&tg, sc, sc, 0.0f, 0.0f, bmp, gw, gh, gw, &scratch_a));
        }
        arena_pop_to(&scratch_a, mark);
        raster_count++;
    }
    free(seen);
    CHECK(raster_count > 1000u);

    /* "Ağış" ölçümü: 16, 32, 64 px'te pozitif ve ölçekle tutarlı */
    FontAtlas a16, a32;
    arena_reset(&out_a);
    CHECK(font_atlas_build(&font, 16.0f, 0u, 0u, &out_a, &scratch_a, &a16));
    CHECK(font_atlas_build(&font, 32.0f, 0u, 0u, &out_a, &scratch_a, &a32));

    f32 w16 = 0.0f, h16 = 0.0f, w32 = 0.0f, h32 = 0.0f;
    f32 r16 = font_measure(&a16, "A\xC4\x9F\xC4\xB1\xC5\x9F", &w16, &h16);
    f32 r32 = font_measure(&a32, "A\xC4\x9F\xC4\xB1\xC5\x9F", &w32, &h32);
    CHECK(r16 > 10.0f && w16 == r16 && h16 > 10.0f);
    CHECK(r32 > 20.0f && w32 == r32 && h32 > 20.0f);
    CHECK_NEAR(r32, r16 * 2.0f, 0.05f);
    CHECK_NEAR(h32, h16 * 2.0f, 0.05f);

    /* Benchmark: 16 / 32 / 64 px'te 95 ASCII glif (32..126) rasterleme hızı (15 koşum medyanı) */
    const f32 bench_sizes[3] = { 16.0f, 32.0f, 64.0f };
    const u32 bench_iters[3] = { 20u, 15u, 10u };
    u16 ascii_gids[95];
    for (u32 i = 0u; i < 95u; i++) {
        ascii_gids[i] = ttf_glyph_index(&font, 32u + i);
    }

    for (u32 s = 0u; s < 3u; s++) {
        f32 px_sz = bench_sizes[s];
        f32 sc    = px_sz / (f32)font.units_per_em;
        u32 iters = bench_iters[s];

        /* Isınma (5 tur) */
        for (u32 w_it = 0u; w_it < 5u; w_it++) {
            for (u32 i = 0u; i < 95u; i++) {
                usize mark = arena_mark(&scratch_a);
                TtfGlyph tg;
                ttf_load_glyph(&font, ascii_gids[i], &scratch_a, &tg);
                i32 ix0, iy0, ix1, iy1;
                font_glyph_bbox(&tg, sc, sc, 0.0f, 0.0f, &ix0, &iy0, &ix1, &iy1);
                u32 gw = (ix1 > ix0) ? (u32)(ix1 - ix0) : 0u;
                u32 gh = (iy1 > iy0) ? (u32)(iy1 - iy0) : 0u;
                if (gw > 0u && gh > 0u) {
                    font_rasterize_glyph(&tg, sc, sc, 0.0f, 0.0f, bmp, gw, gh, gw, &scratch_a);
                }
                arena_pop_to(&scratch_a, mark);
            }
        }

        f64 samples[15];
        for (u32 run = 0u; run < 15u; run++) {
            f64 t0 = plat_time_ms();
            for (u32 it = 0u; it < iters; it++) {
                for (u32 i = 0u; i < 95u; i++) {
                    usize mark = arena_mark(&scratch_a);
                    TtfGlyph tg;
                    ttf_load_glyph(&font, ascii_gids[i], &scratch_a, &tg);
                    i32 ix0, iy0, ix1, iy1;
                    font_glyph_bbox(&tg, sc, sc, 0.0f, 0.0f, &ix0, &iy0, &ix1, &iy1);
                    u32 gw = (ix1 > ix0) ? (u32)(ix1 - ix0) : 0u;
                    u32 gh = (iy1 > iy0) ? (u32)(iy1 - iy0) : 0u;
                    if (gw > 0u && gh > 0u) {
                        font_rasterize_glyph(&tg, sc, sc, 0.0f, 0.0f, bmp, gw, gh, gw, &scratch_a);
                    }
                    arena_pop_to(&scratch_a, mark);
                }
            }
            samples[run] = (plat_time_ms() - t0) / (f64)iters;
        }
        for (u32 a = 0u; a < 15u; a++) {
            for (u32 b = a + 1u; b < 15u; b++) {
                if (samples[b] < samples[a]) {
                    f64 tmp = samples[a]; samples[a] = samples[b]; samples[b] = tmp;
                }
            }
        }
        f64 med_ms = samples[7];
        f64 glyphs_per_sec = (med_ms > 0.0) ? (95.0 * 1000.0 / med_ms) : 0.0;
        printf("    [bench] DejaVuSans 95 ASCII @ %2.0f px: medyan %.3f ms / 95 glif (%.0f glif/s)\n",
               px_sz, med_ms, glyphs_per_sec);
    }
}

TEST(font_ttf_fuzz_50k) {
    u8 orig[2048];
    u32 n = read_bin_file("tests/assets/test_font.ttf", orig, sizeof(orig));
    CHECK(n > 0u && n <= sizeof(orig));

    /* ASan kırmızı bölge (redzone) denetimi için tam boyutlu yığın tamponu */
    u8* mut_buf = (u8*)malloc(n);
    CHECK(mut_buf != NULL);

    Arena a;
    arena_init(&a, g_arena_mem, MB(4));

    Rng rng;
    rng_seed(&rng, 0x9C0FFEE1u);
    u32 parsed_ok = 0u;
    u8 small_bmp[32 * 32];

    for (u32 it = 0u; it < 50000u; it++) {
        memcpy(mut_buf, orig, n);
        u32 mut_count = 1u + (rng_u32(&rng) & 3u);
        for (u32 m = 0u; m < mut_count; m++) {
            mut_buf[rng_u32(&rng) % n] = (u8)rng_u32(&rng);
        }

        u32 test_len = n;
        if ((it & 31u) == 0u) {
            test_len = rng_u32(&rng) % (n + 1u);
        }

        TtfFont f;
        if (ttf_parse(mut_buf, test_len, &f)) {
            parsed_ok++;
            u16 gid_a = ttf_glyph_index(&f, 'A');
            u16 gid_b = ttf_glyph_index(&f, 'B');
            (void)ttf_kern_advance(&f, gid_a, gid_b);
            (void)ttf_glyph_index(&f, 0x011Eu);
            (void)ttf_glyph_index(&f, 0x1F600u);

            u16 limit = MIN(f.num_glyphs, (u16)11u);
            for (u16 g = 0u; g < limit; g++) {
                usize mark = arena_mark(&a);
                TtfGlyph tg;
                if (ttf_load_glyph(&f, g, &a, &tg)) {
                    font_rasterize_glyph(&tg, 0.01f, 0.01f, 0.0f, 0.0f,
                                         small_bmp, 16u, 16u, 16u, &a);
                }
                arena_pop_to(&a, mark);
            }
        }
    }

    free(mut_buf);
    CHECK(parsed_ok > 0u);
}
