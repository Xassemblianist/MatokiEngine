/* tests/test_raster.c — Yazılım rasterizer, doku örnekleme, blend, derinlik, füzyon ve wasm SIMD testleri.
 * Paylaşılan kenar (1000 rastgele üçgen çifti), top-left doldurma kuralı, piksel merkezinde
 * varying interpolasyonu (tol 1e-4), blend modları, early-z derinlik testi, füzyon ve
 * node üzerinden wasm SIMD <-> host skaler bit-bit eşdeğerliğini doğrular.
 */
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/fixed.h"
#include "core/math.h"
#include "core/rng.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "raster/ref_shaders.h"
#include "raster/raster.h"
#include "gfx2d/batch2d.h"
#include <stdlib.h>

static u8 g_test_mem[MB(16)];

/* Yardımcı: tek kanal (R) +1 artıran test shader'ı (paylaşılan kenar sayımı için) */
static void shade_inc_one(ShadeBatch* batch) {
    u32 total = batch->count * 4u;
    f32* out_r = batch->out_rgba + 0u * total;
    f32* out_g = batch->out_rgba + 1u * total;
    f32* out_b = batch->out_rgba + 2u * total;
    f32* out_a = batch->out_rgba + 3u * total;
    const f32 step = 1.0f / 255.0f;
    for (u32 i = 0; i < total; i++) {
        out_r[i] = step;
        out_g[i] = 0.0f;
        out_b[i] = 0.0f;
        out_a[i] = 1.0f;
    }
}

/* 24.8 sabit nokta top-left kenar testi (dışbükey dörtgen sınır kontrolü için) */
static b32 eval_edge_top_left(Fx8 xa, Fx8 ya, Fx8 xb, Fx8 yb, i32 px, i32 py) {
    i32 a = ya - yb;
    i32 b = xb - xa;
    b32 top_left = (a > 0) || (a == 0 && b > 0);
    i64 bias = top_left ? 0 : -1;
    i64 xp = ((i64)px << FX8_SHIFT) + FX8_HALF;
    i64 yp = ((i64)py << FX8_SHIFT) + FX8_HALF;
    i64 e = (i64)a * (xp - (i64)xa) + (i64)b * (yp - (i64)ya) + bias;
    return e >= 0;
}

TEST(raster_top_left_rule) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 128, 128);

    Rasterizer r;
    RefSolidUniforms u_solid = { .color = vec4(1.0f, 0.0f, 0.0f, 1.0f) };

    /* 1) Tam piksel sınırlarına oturan dikdörtgen: [13, 13+37] x [19, 19+29] -> 37*29 piksel */
    fb_clear(&fb, 0);
    arena_reset(&frame);
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, ref_shade_solid, &u_solid, NULL, 0, BLEND_NONE);

    i32 rx = 13, ry = 19, rw = 37, rh = 29;
    RasterVert v0 = { (f32)rx,        (f32)ry,        0.0f, 1.0f, {0} };
    RasterVert v1 = { (f32)(rx + rw), (f32)ry,        0.0f, 1.0f, {0} };
    RasterVert v2 = { (f32)(rx + rw), (f32)(ry + rh), 0.0f, 1.0f, {0} };
    RasterVert v3 = { (f32)rx,        (f32)(ry + rh), 0.0f, 1.0f, {0} };

    raster_triangle(&r, v0, v1, v2);
    raster_triangle(&r, v0, v2, v3);
    raster_flush(&r);

    u32 painted = 0;
    for (i32 y = 0; y < (i32)fb.h; y++) {
        for (i32 x = 0; x < (i32)fb.w; x++) {
            u32 px = fb.px[y * (i32)fb.w + x];
            b32 inside = (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
            if (px != 0) {
                painted++;
                CHECK(inside);
            } else {
                CHECK(!inside);
            }
        }
    }
    CHECK(painted == (u32)(rw * rh));

    /* 2) Doğrudan piksel merkezlerinden (x+0.5, y+0.5) geçen dikdörtgen:
     * Üst ve sol kenarlar dahil edilmeli, alt ve sağ kenarlar hariç tutulmalı -> tam rw*rh piksel */
    fb_clear(&fb, 0);
    arena_reset(&frame);
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, ref_shade_solid, &u_solid, NULL, 0, BLEND_NONE);

    RasterVert c0 = { (f32)rx + 0.5f,        (f32)ry + 0.5f,        0.0f, 1.0f, {0} };
    RasterVert c1 = { (f32)(rx + rw) + 0.5f, (f32)ry + 0.5f,        0.0f, 1.0f, {0} };
    RasterVert c2 = { (f32)(rx + rw) + 0.5f, (f32)(ry + rh) + 0.5f, 0.0f, 1.0f, {0} };
    RasterVert c3 = { (f32)rx + 0.5f,        (f32)(ry + rh) + 0.5f, 0.0f, 1.0f, {0} };

    raster_triangle(&r, c0, c1, c2);
    raster_triangle(&r, c0, c2, c3);
    raster_flush(&r);

    painted = 0;
    for (i32 y = 0; y < (i32)fb.h; y++) {
        for (i32 x = 0; x < (i32)fb.w; x++) {
            u32 px = fb.px[y * (i32)fb.w + x];
            b32 inside = (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
            if (px != 0) {
                painted++;
                CHECK(inside);
            } else {
                CHECK(!inside);
            }
        }
    }
    CHECK(painted == (u32)(rw * rh));

    /* 3) Arka yüz ayıklama (cull_back) testi */
    fb_clear(&fb, 0);
    arena_reset(&frame);
    raster_begin(&r, &fb, &frame);
    raster_set_cull(&r, TRUE);
    raster_set_shader(&r, ref_shade_solid, &u_solid, NULL, 0, BLEND_NONE);
    /* Ters sarımlı üçgen elenmeli */
    raster_triangle(&r, v0, v2, v1);
    raster_flush(&r);
    CHECK(r.stat_pixels == 0);
    /* Düz sarımlı üçgen çizilmeli */
    raster_triangle(&r, v0, v1, v2);
    raster_flush(&r);
    CHECK(r.stat_pixels > 0);
}

TEST(raster_shared_edges_1000_pairs) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 128, 128);

    Rasterizer r;
    Rng rng;
    rng_seed(&rng, 0xCAFEBABE12345678ULL);

    /* A) 1000 rastgele dikdörtgen / quad ızgarası içinde ortak kenarlı üçgen çiftleri:
     * Kapsayan dikdörtgen içindeki hiçbir piksel 0 kez veya >=2 kez boyanmaz (her piksel tam 1 kez). */
    for (u32 iter = 0; iter < 1000; iter++) {
        fb_clear(&fb, 0);
        arena_reset(&frame);

        i32 x0 = rng_range(&rng, 4, 60);
        i32 y0 = rng_range(&rng, 4, 60);
        i32 x1 = rng_range(&rng, x0 + 4, 120);
        i32 y1 = rng_range(&rng, y0 + 4, 120);

        f32 off = (iter & 1u) ? 0.5f : 0.0f;
        f32 fx0 = (f32)x0 + off;
        f32 fy0 = (f32)y0 + off;
        f32 fx1 = (f32)x1 + off;
        f32 fy1 = (f32)y1 + off;

        RasterVert va = { fx0, fy0, 0.0f, 1.0f, {0} };
        RasterVert vb = { fx1, fy0, 0.0f, 1.0f, {0} };
        RasterVert vc = { fx1, fy1, 0.0f, 1.0f, {0} };
        RasterVert vd = { fx0, fy1, 0.0f, 1.0f, {0} };

        raster_begin(&r, &fb, &frame);
        raster_set_shader(&r, shade_inc_one, NULL, NULL, 0, BLEND_ADD);

        if (iter % 3 == 0) {
            raster_triangle(&r, va, vb, vc);
            raster_triangle(&r, va, vc, vd);
        } else if (iter % 3 == 1) {
            raster_triangle(&r, va, vb, vd);
            raster_triangle(&r, vc, vd, vb);
        } else {
            f32 px = fx0 + 1.0f + rng_f32(&rng) * (fx1 - fx0 - 2.0f);
            f32 py = fy0 + 1.0f + rng_f32(&rng) * (fy1 - fy0 - 2.0f);
            RasterVert vp = { px, py, 0.0f, 1.0f, {0} };
            raster_triangle(&r, va, vb, vp);
            raster_triangle(&r, vb, vc, vp);
            raster_triangle(&r, vc, vd, vp);
            raster_triangle(&r, vd, va, vp);
        }

        raster_flush(&r);

        for (i32 y = y0; y < y1; y++) {
            u32* row = fb.px + y * (i32)fb.w;
            for (i32 x = x0; x < x1; x++) {
                u32 hits = row[x] & 0xFFu;
                CHECK(hits == 1u);
            }
        }
    }

    /* B) 1000 rastgele eğik/alt-piksel dışbükey dörtgen (iki ortak kenarlı üçgen ABC + ACD):
     * Dışbükey dörtgenin içindeki her piksel tam 1 kez, dışındaki her piksel tam 0 kez boyanır. */
    for (u32 iter = 0; iter < 1000; iter++) {
        fb_clear(&fb, 0);
        arena_reset(&frame);

        /* 4 çeyrekte rastgele nokta seçerek garantili dışbükey dörtgen oluştur */
        f32 ax = 8.0f  + rng_f32(&rng) * 44.0f;
        f32 ay = 8.0f  + rng_f32(&rng) * 44.0f;
        f32 bx = 68.0f + rng_f32(&rng) * 50.0f;
        f32 by = 8.0f  + rng_f32(&rng) * 44.0f;
        f32 cx = 68.0f + rng_f32(&rng) * 50.0f;
        f32 cy = 68.0f + rng_f32(&rng) * 50.0f;
        f32 dx = 8.0f  + rng_f32(&rng) * 44.0f;
        f32 dy = 68.0f + rng_f32(&rng) * 50.0f;

        Fx8 xa = fx8_from_f32(ax), ya = fx8_from_f32(ay);
        Fx8 xb = fx8_from_f32(bx), yb = fx8_from_f32(by);
        Fx8 xc = fx8_from_f32(cx), yc = fx8_from_f32(cy);
        Fx8 xd = fx8_from_f32(dx), yd = fx8_from_f32(dy);

        /* Dörtgenin kesin dışbükey (tüm iç açılar < 180 derece) olduğunu doğrula */
        i64 c_abc = (i64)(xb - xa) * (i64)(yc - yb) - (i64)(yb - ya) * (i64)(xc - xb);
        i64 c_bcd = (i64)(xc - xb) * (i64)(yd - yc) - (i64)(yc - yb) * (i64)(xd - xc);
        i64 c_cda = (i64)(xd - xc) * (i64)(ya - yd) - (i64)(yd - yc) * (i64)(xa - xd);
        i64 c_dab = (i64)(xa - xd) * (i64)(yb - ya) - (i64)(ya - yd) * (i64)(xb - xa);
        if (c_abc <= 0 || c_bcd <= 0 || c_cda <= 0 || c_dab <= 0) {
            iter--;
            continue;
        }

        RasterVert va = { ax, ay, 0.0f, 1.0f, {0} };
        RasterVert vb = { bx, by, 0.0f, 1.0f, {0} };
        RasterVert vc = { cx, cy, 0.0f, 1.0f, {0} };
        RasterVert vd = { dx, dy, 0.0f, 1.0f, {0} };

        raster_begin(&r, &fb, &frame);
        raster_set_shader(&r, shade_inc_one, NULL, NULL, 0, BLEND_ADD);
        raster_triangle(&r, va, vb, vc);
        raster_triangle(&r, va, vc, vd);
        raster_flush(&r);

        for (i32 y = 0; y < (i32)fb.h; y++) {
            u32* row = fb.px + y * (i32)fb.w;
            for (i32 x = 0; x < (i32)fb.w; x++) {
                u32 hits = row[x] & 0xFFu;
                b32 in_quad = eval_edge_top_left(xa, ya, xb, yb, x, y) &&
                              eval_edge_top_left(xb, yb, xc, yc, x, y) &&
                              eval_edge_top_left(xc, yc, xd, yd, x, y) &&
                              eval_edge_top_left(xd, yd, xa, ya, x, y);
                CHECK(hits == (in_quad ? 1u : 0u));
            }
        }
    }
}

/* Varying interpolasyon testi için özel kontrol shader'ı */
typedef struct {
    f32 ax, ay, ac;
    f32 bx, by, bc;
} VarTestFunc;

static const VarTestFunc* g_var_func = NULL;

static void shade_check_varying(ShadeBatch* batch) {
    u32 count = batch->count;
    u32 stride = count * 4u;
    const f32* v0 = batch->varyings + 0u * stride;
    const f32* v1 = batch->varyings + 1u * stride;
    f32* out_r = batch->out_rgba + 0u * stride;
    f32* out_g = batch->out_rgba + 1u * stride;
    f32* out_b = batch->out_rgba + 2u * stride;
    f32* out_a = batch->out_rgba + 3u * stride;

    for (u32 q = 0; q < count; q++) {
        u32 mask = batch->quads[q].mask;
        u32 qx = batch->quads[q].x;
        u32 qy = batch->quads[q].y;
        for (u32 lane = 0; lane < 4; lane++) {
            u32 idx = q * 4u + lane;
            if (mask & (1u << lane)) {
                f32 px = (f32)(qx + (lane & 1u)) + 0.5f;
                f32 py = (f32)(qy + (lane >> 1u)) + 0.5f;
                f32 exp0 = g_var_func->ax * px + g_var_func->ay * py + g_var_func->ac;
                f32 exp1 = g_var_func->bx * px + g_var_func->by * py + g_var_func->bc;
                CHECK_NEAR(v0[idx], exp0, 1e-4);
                CHECK_NEAR(v1[idx], exp1, 1e-4);
            }
            out_r[idx] = 1.0f;
            out_g[idx] = 1.0f;
            out_b[idx] = 1.0f;
            out_a[idx] = 1.0f;
        }
    }
}

TEST(raster_varying_interpolation) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 128, 128);

    VarTestFunc fn = {
        .ax = 0.0125f, .ay = -0.0075f, .ac = 0.25f,
        .bx = -0.02f,  .by = 0.03125f, .bc = 1.5f
    };
    g_var_func = &fn;

    RasterVert v0 = { 10.5f, 12.5f, 0.0f, 1.0f, {0} };
    RasterVert v1 = { 98.5f, 24.5f, 0.0f, 1.0f, {0} };
    RasterVert v2 = { 34.5f, 106.5f, 0.0f, 1.0f, {0} };

    RasterVert* verts[3] = { &v0, &v1, &v2 };
    for (u32 i = 0; i < 3; i++) {
        f32 x = verts[i]->x;
        f32 y = verts[i]->y;
        verts[i]->var[0] = fn.ax * x + fn.ay * y + fn.ac;
        verts[i]->var[1] = fn.bx * x + fn.by * y + fn.bc;
    }

    fb_clear(&fb, 0);
    Rasterizer r;
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, shade_check_varying, NULL, NULL, 2, BLEND_NONE);
    raster_triangle(&r, v0, v1, v2);
    raster_flush(&r);
    CHECK(r.stat_pixels > 1000);

    /* Perspektif-doğru (inv_w != 1) yolunu da doğrula */
    v0.inv_w = 0.5f;
    v1.inv_w = 0.5f;
    v2.inv_w = 0.5f;
    arena_reset(&frame);
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, shade_check_varying, NULL, NULL, 2, BLEND_NONE);
    raster_triangle(&r, v0, v1, v2);
    raster_flush(&r);
}

TEST(raster_blend_modes) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 16, 16);

    Rasterizer r;
    RasterVert v0 = { 0.0f, 0.0f, 0.0f, 1.0f, {0} };
    RasterVert v1 = { 8.0f, 0.0f, 0.0f, 1.0f, {0} };
    RasterVert v2 = { 8.0f, 8.0f, 0.0f, 1.0f, {0} };
    RasterVert v3 = { 0.0f, 8.0f, 0.0f, 1.0f, {0} };

    /* 1) BLEND_NONE: doğrudan üzerine yazar */
    fb_clear(&fb, FB_RGBA(100, 100, 100, 255));
    arena_reset(&frame);
    RefSolidUniforms u1 = { .color = vec4(0.5f, 0.25f, 1.0f, 0.75f) };
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, ref_shade_solid, &u1, NULL, 0, BLEND_NONE);
    raster_triangle(&r, v0, v1, v2);
    raster_triangle(&r, v0, v2, v3);
    raster_flush(&r);

    u32 p_none = fb.px[2 * 16 + 2];
    CHECK((p_none & 0xFFu) == (u32)(0.5f * 255.0f + 0.5f));
    CHECK(((p_none >> 8) & 0xFFu) == (u32)(0.25f * 255.0f + 0.5f));
    CHECK(((p_none >> 16) & 0xFFu) == 255u);
    CHECK(((p_none >> 24) & 0xFFu) == (u32)(0.75f * 255.0f + 0.5f));

    /* 2) BLEND_ALPHA (src-over düz alpha):
     * dst = (100, 200, 50, 255), src = (200/255, 50/255, 150/255, 0.5)
     * out_r = 200*0.5 + 100*0.5 = 150
     * out_g = 50*0.5  + 200*0.5 = 125
     * out_b = 150*0.5 + 50*0.5  = 100
     * out_a = 0.5 + 1.0*0.5     = 1.0 (255) */
    fb_clear(&fb, FB_RGBA(100, 200, 50, 255));
    arena_reset(&frame);
    RefSolidUniforms u2 = { .color = vec4(200.0f / 255.0f, 50.0f / 255.0f, 150.0f / 255.0f, 0.5f) };
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, ref_shade_solid, &u2, NULL, 0, BLEND_ALPHA);
    raster_triangle(&r, v0, v1, v2);
    raster_triangle(&r, v0, v2, v3);
    raster_flush(&r);

    u32 p_alpha = fb.px[2 * 16 + 2];
    CHECK((p_alpha & 0xFFu) == 150u);
    CHECK(((p_alpha >> 8) & 0xFFu) == 125u);
    CHECK(((p_alpha >> 16) & 0xFFu) == 100u);
    CHECK(((p_alpha >> 24) & 0xFFu) == 255u);

    /* 3) BLEND_ADD: toplama ve 255'te doyma (clamp) */
    fb_clear(&fb, FB_RGBA(200, 100, 50, 200));
    arena_reset(&frame);
    RefSolidUniforms u3 = { .color = vec4(100.0f / 255.0f, 80.0f / 255.0f, 25.0f / 255.0f, 100.0f / 255.0f) };
    raster_begin(&r, &fb, &frame);
    raster_set_shader(&r, ref_shade_solid, &u3, NULL, 0, BLEND_ADD);
    raster_triangle(&r, v0, v1, v2);
    raster_triangle(&r, v0, v2, v3);
    raster_flush(&r);

    u32 p_add = fb.px[2 * 16 + 2];
    CHECK((p_add & 0xFFu) == 255u);
    CHECK(((p_add >> 8) & 0xFFu) == 180u);
    CHECK(((p_add >> 16) & 0xFFu) == 75u);
    CHECK(((p_add >> 24) & 0xFFu) == 255u);
}

TEST(raster_texture_and_batch2d) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 64, 64);

    Texture tex = tex_create(&perm, 4, 4, TEX_FLAG_BILINEAR | TEX_FLAG_CLAMP);
    tex_gen_checker(&tex, 2, FB_RGBA(255, 0, 0, 255), FB_RGBA(0, 255, 0, 255));

    /* Bilinear örnekleme merkezinde (u=0.5, v=0.5) 4 texel'in ortalamasını vermeli */
    f32 rgba[4];
    ref_sample_tex(&tex, 0.5f, 0.5f, rgba);
    CHECK_NEAR(rgba[0], 0.5f, 1e-4);
    CHECK_NEAR(rgba[1], 0.5f, 1e-4);
    CHECK_NEAR(rgba[2], 0.0f, 1e-4);
    CHECK_NEAR(rgba[3], 1.0f, 1e-4);

    /* Batch2D ile sprite çizimi ve alpha<0.01 discard kontrolü */
    Texture stex = tex_create(&perm, 8, 8, 0);
    tex_gen_checker(&stex, 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 0, 0, 0));

    fb_clear(&fb, 0);
    Rasterizer r;
    Batch2D b;
    raster_begin(&r, &fb, &frame);
    b2d_begin(&b, &r, mat3_identity());
    b2d_sprite(&b, &stex, rect(0.0f, 0.0f, 1.0f, 1.0f),
               vec2(16.0f, 16.0f), vec2(8.0f, 8.0f), 0.0f, vec4(1.0f, 1.0f, 1.0f, 1.0f));
    b2d_end(&b);

    /* 8x8 sprite'ın yarısı alpha=0 olduğundan discard edilmeli (32 piksel boyanmalı) */
    CHECK(r.stat_pixels == 32u);
}

/* Füzyonlu test shader'ı: doğrudan batch->fb_px'e yazar */
static void shade_fused_solid(ShadeBatch* batch) {
    CHECK(batch->fb_px != NULL);
    CHECK(batch->fb_stride > 0);
    u32 col = FB_RGBA(42, 128, 250, 255);
    for (u32 q = 0; q < batch->count; q++) {
        u32 mask = batch->quads[q].mask;
        u32 qx = batch->quads[q].x;
        u32 qy = batch->quads[q].y;
        u32* r0 = batch->fb_px + qy * batch->fb_stride + qx;
        u32* r1 = r0 + batch->fb_stride;
        if (mask & 1u) r0[0] = col;
        if (mask & 2u) r0[1] = col;
        if (mask & 4u) r1[0] = col;
        if (mask & 8u) r1[1] = col;
    }
}

TEST(raster_depth_and_fusion) {
    Arena perm, frame;
    arena_init(&perm, g_test_mem, MB(4));
    arena_init(&frame, g_test_mem + MB(4), MB(12));

    Framebuffer fb;
    fb_init(&fb, &perm, 64, 64);
    fb_init_depth(&fb, &perm);

    /* 1) Kesişen iki üçgen (early-z DEPTH_LESS testi):
     * Kırmızı üçgen: z soldan (0.2) sağa (0.8) artıyor -> z_red(x) = 0.2 + 0.6 * ((x - 8)/48)
     * Mavi üçgen:    z soldan (0.8) sağa (0.2) azalıyor -> z_blue(x) = 0.8 - 0.6 * ((x - 8)/48)
     * Kesişim x = 32 civarında: sol yarıda (x < 32) kırmızı önde, sağ yarıda (x > 32) mavi önde. */
    fb_clear(&fb, 0);
    fb_clear_depth(&fb, 1.0f);
    arena_reset(&frame);

    Rasterizer r;
    raster_begin(&r, &fb, &frame);
    raster_set_depth(&r, TRUE, TRUE, DEPTH_LESS);

    RefSolidUniforms u_red  = { .color = vec4(1.0f, 0.0f, 0.0f, 1.0f) };
    RefSolidUniforms u_blue = { .color = vec4(0.0f, 0.0f, 1.0f, 1.0f) };

    /* Önce kırmızı dörtgeni (2 üçgen) çiz */
    raster_set_shader(&r, ref_shade_solid, &u_red, NULL, 0, BLEND_NONE);
    RasterVert r0 = {  8.0f, 16.0f, 0.2f, 1.0f, {0} };
    RasterVert r1 = { 56.0f, 16.0f, 0.8f, 1.0f, {0} };
    RasterVert r2 = { 56.0f, 48.0f, 0.8f, 1.0f, {0} };
    RasterVert r3 = {  8.0f, 48.0f, 0.2f, 1.0f, {0} };
    raster_triangle(&r, r0, r1, r2);
    raster_triangle(&r, r0, r2, r3);

    /* Sonra mavi dörtgeni (2 üçgen) ters eğimli z ile çiz */
    raster_set_shader(&r, ref_shade_solid, &u_blue, NULL, 0, BLEND_NONE);
    RasterVert b0 = {  8.0f, 16.0f, 0.8f, 1.0f, {0} };
    RasterVert b1 = { 56.0f, 16.0f, 0.2f, 1.0f, {0} };
    RasterVert b2 = { 56.0f, 48.0f, 0.2f, 1.0f, {0} };
    RasterVert b3 = {  8.0f, 48.0f, 0.8f, 1.0f, {0} };
    raster_triangle(&r, b0, b1, b2);
    raster_triangle(&r, b0, b2, b3);
    raster_flush(&r);

    u32 col_red  = FB_RGBA(255, 0, 0, 255);
    u32 col_blue = FB_RGBA(0, 0, 255, 255);
    for (i32 y = 16; y < 48; y++) {
        for (i32 x = 8; x < 56; x++) {
            u32 px = fb.px[y * 64 + x];
            f32 z  = fb.depth[y * 64 + x];
            if (x < 31) {
                CHECK(px == col_red);
                CHECK(z < 0.5f);
            } else if (x > 32) {
                CHECK(px == col_blue);
                CHECK(z < 0.5f);
            }
        }
    }

    /* Early-Z kanıtı: arkada kalan üçgen shade edilmeden elenmeli (stat_pixels yarıya inmeli) */
    CHECK(r.stat_pixels == (u64)(48 * 32 + 24 * 32));

    /* 2) Füzyon (fused = TRUE) testi: rasterizer blend yapmaz, shader fb_px'e doğrudan yazar */
    fb_clear(&fb, 0);
    arena_reset(&frame);
    raster_begin(&r, &fb, &frame);
    raster_set_shader_ex(&r, shade_fused_solid, NULL, NULL, 0, BLEND_NONE, TRUE);
    RasterVert fv0 = { 10.0f, 10.0f, 0.0f, 1.0f, {0} };
    RasterVert fv1 = { 26.0f, 10.0f, 0.0f, 1.0f, {0} };
    RasterVert fv2 = { 26.0f, 26.0f, 0.0f, 1.0f, {0} };
    RasterVert fv3 = { 10.0f, 26.0f, 0.0f, 1.0f, {0} };
    raster_triangle(&r, fv0, fv1, fv2);
    raster_triangle(&r, fv0, fv2, fv3);
    raster_flush(&r);

    CHECK(r.stat_pixels == 256u);
    CHECK(fb.px[15 * 64 + 15] == FB_RGBA(42, 128, 250, 255));
    CHECK(fb.px[5 * 64 + 5] == 0u);
}

TEST(raster_wasm_simd_bit_exact) {
    /* Wasm SIMD yolunu Node.js altında çalıştırıp host skaler referansıyla bit-bit karşılaştırır */
    int rc = system("node tests/test_raster_wasm.mjs");
    CHECK(rc == 0);
}
