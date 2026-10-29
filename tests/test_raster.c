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
#include "plat/platform.h"
#include "thread/thread.h"
#include "thread/job.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "raster/ref_shaders.h"
#include "raster/raster.h"
#include "gfx2d/batch2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 g_test_mem[MB(32)];

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

/* Genel quad/batch yolunu (emit_quad_scalar / emit_quad_general_simd) zorlayan,
 * renk tamponuna dokunmayan boş füzyonlu shader (depth-only hızlı yol referansı için). */
static void shade_noop_fused(ShadeBatch* batch) {
    UNUSED(batch);
}

TEST(raster_depth_only_fast_path_bit_exact) {
    Arena perm, frame_fast, frame_ref;
    arena_init(&perm,       g_test_mem,           MB(8));
    arena_init(&frame_fast, g_test_mem + MB(8),   MB(12));
    arena_init(&frame_ref,  g_test_mem + MB(20),  MB(12));

    const u32 W = 256u;
    const u32 H = 256u;
    Framebuffer fb_fast, fb_ref;
    fb_init(&fb_fast, &perm, W, H);
    fb_init_depth(&fb_fast, &perm);
    fb_init(&fb_ref, &perm, W, H);
    fb_init_depth(&fb_ref, &perm);

    /* Küçük (<16x16), orta (kısmi 8x8 blok) ve büyük (8x8 trivial accept + karo sınırı)
     * üçgenlerden oluşan 400 üçgenlik test kümesi */
    enum { N_TRIS = 400 };
    RasterVert* v0s = ARENA_PUSH_ARRAY(&perm, RasterVert, N_TRIS);
    RasterVert* v1s = ARENA_PUSH_ARRAY(&perm, RasterVert, N_TRIS);
    RasterVert* v2s = ARENA_PUSH_ARRAY(&perm, RasterVert, N_TRIS);

    Rng rng;
    rng_seed(&rng, 0xD3974011CAFE1234ULL);
    for (u32 i = 0; i < N_TRIS; i++) {
        f32 cx  = -8.0f + rng_f32(&rng) * (f32)(W + 16u);
        f32 cy  = -8.0f + rng_f32(&rng) * (f32)(H + 16u);
        f32 rad = (i % 5u == 0u) ? (32.0f + rng_f32(&rng) * 64.0f)
                : (i % 2u == 0u) ? (4.0f  + rng_f32(&rng) * 9.0f)
                                 : (10.0f + rng_f32(&rng) * 24.0f);
        f32 a0 = rng_f32(&rng) * M_TAU;
        f32 a1 = a0 + 0.6f + rng_f32(&rng) * 1.4f;
        f32 a2 = a1 + 0.6f + rng_f32(&rng) * 1.4f;
        f32 z0 = 0.05f + rng_f32(&rng) * 0.90f;
        f32 z1 = 0.05f + rng_f32(&rng) * 0.90f;
        f32 z2 = 0.05f + rng_f32(&rng) * 0.90f;

        v0s[i] = (RasterVert){ cx + m_cos(a0) * rad, cy + m_sin(a0) * rad, z0, 1.0f, {0} };
        v1s[i] = (RasterVert){ cx + m_cos(a1) * rad, cy + m_sin(a1) * rad, z1, 1.0f, {0} };
        v2s[i] = (RasterVert){ cx + m_cos(a2) * rad, cy + m_sin(a2) * rad, z2, 1.0f, {0} };
    }

    DepthFunc funcs[4] = { DEPTH_LESS, DEPTH_GREATER, DEPTH_LEQUAL, DEPTH_GEQUAL };
    f32 clear_zs[4]    = { 1.0f,       0.0f,          1.0f,         0.0f         };

    for (u32 m = 0; m < 5u; m++) {
        b32 do_test   = (m < 4u);
        DepthFunc fn  = (m < 4u) ? funcs[m] : DEPTH_ALWAYS;
        f32 clear_z   = (m < 4u) ? clear_zs[m] : 0.5f;

        fb_clear_depth(&fb_fast, clear_z);
        fb_clear_depth(&fb_ref,  clear_z);
        arena_reset(&frame_fast);
        arena_reset(&frame_ref);

        Rasterizer r_fast, r_ref;
        raster_begin(&r_fast, &fb_fast, &frame_fast);
        raster_set_depth(&r_fast, do_test, TRUE, fn);
        raster_set_shader(&r_fast, NULL, NULL, NULL, 0, BLEND_NONE);

        raster_begin(&r_ref, &fb_ref, &frame_ref);
        raster_set_depth(&r_ref, do_test, TRUE, fn);
        raster_set_shader_ex(&r_ref, shade_noop_fused, NULL, NULL, 0, BLEND_NONE, TRUE);

        for (u32 i = 0; i < N_TRIS; i++) {
            raster_triangle(&r_fast, v0s[i], v1s[i], v2s[i]);
            raster_triangle(&r_ref,  v0s[i], v1s[i], v2s[i]);
        }
        raster_flush(&r_fast);
        raster_flush(&r_ref);

        const u32* d_fast = (const u32*)fb_fast.depth;
        const u32* d_ref  = (const u32*)fb_ref.depth;
        u32 diffs = 0;
        for (u32 i = 0; i < W * H; i++) {
            if (d_fast[i] != d_ref[i]) {
                diffs++;
            }
        }
        CHECK(diffs == 0u);
    }

    /* depth_write = FALSE iken derinlik tamponu hiç değişmemeli */
    fb_clear_depth(&fb_fast, 0.75f);
    arena_reset(&frame_fast);
    Rasterizer r_nowrite;
    raster_begin(&r_nowrite, &fb_fast, &frame_fast);
    raster_set_depth(&r_nowrite, TRUE, FALSE, DEPTH_LESS);
    raster_set_shader(&r_nowrite, NULL, NULL, NULL, 0, BLEND_NONE);
    for (u32 i = 0; i < 32u; i++) {
        raster_triangle(&r_nowrite, v0s[i], v1s[i], v2s[i]);
    }
    raster_flush(&r_nowrite);
    for (u32 i = 0; i < W * H; i++) {
        CHECK(fb_fast.depth[i] == 0.75f);
    }
}

TEST(raster_parallel_binning_bit_exact) {
    job_system_init();

    Arena perm, frame_1t, frame_mt;
    arena_init(&perm,     g_test_mem,           MB(10));
    arena_init(&frame_1t, g_test_mem + MB(10),  MB(11));
    arena_init(&frame_mt, g_test_mem + MB(21),  MB(11));

    /* 640x360 (10x6 karo, cols=2 -> 12 şerit) üzerinde hem çoklu raster_bin_tris dilimleri
     * hem de anlık raster_triangle çağrılarıyla 1T ve 4T/8T paralel binlemeyi karşılaştır */
    const u32 W = 640u;
    const u32 H = 360u;
    Framebuffer fb_1t, fb_mt;
    fb_init(&fb_1t, &perm, W, H);
    fb_init_depth(&fb_1t, &perm);
    fb_init(&fb_mt, &perm, W, H);
    fb_init_depth(&fb_mt, &perm);

    enum { N_SPANS = 80, TRIS_PER_SPAN = 6, TOTAL = N_SPANS * TRIS_PER_SPAN };
    BinnedTri* span_tris = ARENA_PUSH_ARRAY(&perm, BinnedTri, TOTAL);
    f32*       span_vc   = ARENA_PUSH_ARRAY(&perm, f32, TOTAL * 4u * 3u);
    u32*       span_cnt  = ARENA_PUSH_ARRAY(&perm, u32, N_SPANS);

    RasterShaderState st_vcol = {
        .shade_fn      = REF_SHADE_VCOLOR_FN,
        .uniforms      = NULL,
        .textures      = NULL,
        .varying_count = 4u,
        .blend         = BLEND_NONE,
        .fused         = FALSE,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_func    = DEPTH_GREATER,
        .depth_late    = FALSE
    };

    Rng rng;
    rng_seed(&rng, 0xB144146012345678ULL);
    for (u32 s = 0; s < N_SPANS; s++) {
        f32 cx  = 16.0f + rng_f32(&rng) * (f32)(W - 32u);
        f32 cy  = 16.0f + rng_f32(&rng) * (f32)(H - 32u);
        f32 rad = (s < 8u) ? (90.0f + rng_f32(&rng) * 60.0f) : (8.0f + rng_f32(&rng) * 18.0f);
        u32 sc  = 0;
        for (u32 t = 0; t < TRIS_PER_SPAN; t++) {
            f32 a0 = (f32)t * (M_TAU / (f32)TRIS_PER_SPAN);
            f32 a1 = a0 + 0.85f;
            f32 a2 = a0 + 1.75f;
            f32 z  = 0.1f + 0.8f * rng_f32(&rng);
            RasterVert v0 = { cx + m_cos(a0) * rad * 0.2f, cy + m_sin(a0) * rad * 0.2f, z,         1.0f, {1.0f, 0.2f, 0.3f, 1.0f} };
            RasterVert v1 = { cx + m_cos(a1) * rad,        cy + m_sin(a1) * rad,        z + 0.04f, 1.0f, {0.2f, 1.0f, 0.4f, 1.0f} };
            RasterVert v2 = { cx + m_cos(a2) * rad,        cy + m_sin(a2) * rad,        z - 0.03f, 1.0f, {0.3f, 0.4f, 1.0f, 1.0f} };
            u32 idx = s * TRIS_PER_SPAN + sc;
            if (raster_setup_tri(&v0, &v1, &v2, &st_vcol, FALSE, (i32)W, (i32)H,
                                 &span_tris[idx], &span_vc[idx * 12u])) {
                sc++;
            }
        }
        span_cnt[s] = sc;
    }

    Rasterizer r_1t, r_mt;
    fb_clear(&fb_1t, FB_RGBA(10, 12, 18, 255));
    fb_clear_depth(&fb_1t, 0.0f);
    arena_reset(&frame_1t);

    engine_set_worker_count(0u);
    raster_begin(&r_1t, &fb_1t, &frame_1t);
    for (u32 s = 0; s < N_SPANS; s++) {
        raster_bin_tris(&r_1t, &span_tris[s * TRIS_PER_SPAN], span_cnt[s], TRIS_PER_SPAN);
    }
    raster_flush(&r_1t);

    u32 worker_counts[2] = { 3u, 7u };
    for (u32 wc = 0; wc < 2u; wc++) {
        fb_clear(&fb_mt, FB_RGBA(10, 12, 18, 255));
        fb_clear_depth(&fb_mt, 0.0f);
        arena_reset(&frame_mt);

        engine_set_worker_count(worker_counts[wc]);
        raster_begin(&r_mt, &fb_mt, &frame_mt);
        for (u32 s = 0; s < N_SPANS; s++) {
            raster_bin_tris(&r_mt, &span_tris[s * TRIS_PER_SPAN], span_cnt[s], TRIS_PER_SPAN);
        }
        raster_flush(&r_mt);

        CHECK(r_mt.stat_tris_in     == r_1t.stat_tris_in);
        CHECK(r_mt.stat_tris_binned == r_1t.stat_tris_binned);
        CHECK(r_mt.stat_quads       == r_1t.stat_quads);
        CHECK(r_mt.stat_pixels      == r_1t.stat_pixels);

        const u32* d1 = (const u32*)fb_1t.depth;
        const u32* dm = (const u32*)fb_mt.depth;
        u32 px_diffs = 0;
        u32 z_diffs  = 0;
        for (u32 i = 0; i < W * H; i++) {
            if (fb_1t.px[i] != fb_mt.px[i]) px_diffs++;
            if (d1[i] != dm[i]) z_diffs++;
        }
        CHECK(px_diffs == 0u);
        CHECK(z_diffs  == 0u);
    }
    engine_set_worker_count(0u);
}

TEST(raster_wasm_simd_bit_exact) {
    /* Wasm SIMD yolunu Node.js altında çalıştırıp host skaler referansıyla bit-bit karşılaştırır */
    int rc = system("node tests/test_raster_wasm.mjs");
    CHECK(rc == 0);
}

TEST(raster_m14a_wasm_simd_and_bench) {
    /* Wasm SIMD altında depth-only hızlı yolu (shade_fn==NULL) genel SIMD yoluyla (shade_noop_fused)
     * ve paralel binlemeyi (4T) seri binlemeyle (1T) 512x512 6000 üçgenlik gölge geçişinde
     * bit-bit karşılaştırır ve medyan süreleri ölçer. Geçici dosyalar yalnızca build/wasm/ altındadır. */
    const char* c_path    = "build/wasm/m14a_simd_test.c";
    const char* wasm_path = "build/wasm/m14a_simd_test.wasm";

    FILE* fp = fopen(c_path, "wb");
    CHECK(fp != NULL);
    if (!fp) return;

    const char* src =
        "#include \"core/types.h\"\n"
        "#include \"core/arena.h\"\n"
        "#include \"core/math.h\"\n"
        "#include \"core/rng.h\"\n"
        "#include \"plat/platform.h\"\n"
        "#include \"thread/thread.h\"\n"
        "#include \"thread/job.h\"\n"
        "#include \"raster/framebuffer.h\"\n"
        "#include \"raster/raster.h\"\n"
        "#define SW 512\n"
        "#define SH 512\n"
        "#define NITEMS 1000\n"
        "#define TPI 6\n"
        "#define NTRIS (NITEMS * TPI)\n"
        "static u8 g_perm[MB(8)];\n"
        "static u8 g_frm1[MB(16)];\n"
        "static u8 g_frm2[MB(16)];\n"
        "static Framebuffer g_fb_fast, g_fb_ref;\n"
        "static Rasterizer g_r1, g_r2;\n"
        "static BinnedTri g_tris_fast[NTRIS];\n"
        "static BinnedTri g_tris_ref[NTRIS];\n"
        "static u32 g_cnts[NITEMS];\n"
        "static RasterShaderState g_st_fast, g_st_ref;\n"
        "static void noop_fused(ShadeBatch* b) { UNUSED(b); }\n"
        "static f64 g_med_bin = 0.0, g_med_fast = 0.0, g_med_ref = 0.0;\n"
        "static f64 sort_med(f64* a, u32 n) {\n"
        "    for (u32 i = 1; i < n; i++) { f64 v = a[i]; i32 j = (i32)i - 1;\n"
        "        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }\n"
        "    return a[n / 2u];\n"
        "}\n"
        "EXPORT(m14a_run) u32 m14a_run(void) {\n"
        "    job_system_init();\n"
        "    Arena perm, f1, f2;\n"
        "    arena_init(&perm, g_perm, sizeof(g_perm));\n"
        "    arena_init(&f1, g_frm1, sizeof(g_frm1));\n"
        "    arena_init(&f2, g_frm2, sizeof(g_frm2));\n"
        "    fb_init(&g_fb_fast, &perm, SW, SH); fb_init_depth(&g_fb_fast, &perm);\n"
        "    fb_init(&g_fb_ref,  &perm, SW, SH); fb_init_depth(&g_fb_ref,  &perm);\n"
        "    g_st_fast = (RasterShaderState){ .shade_fn = NULL, .depth_test = TRUE, .depth_write = TRUE, .depth_func = DEPTH_LESS };\n"
        "    g_st_ref  = (RasterShaderState){ .shade_fn = noop_fused, .fused = TRUE, .depth_test = TRUE, .depth_write = TRUE, .depth_func = DEPTH_LESS };\n"
        "    Rng rng; rng_seed(&rng, 0xA5A5F0F012345678ULL);\n"
        "    for (u32 it = 0; it < NITEMS; it++) {\n"
        "        u32 gx = it % 32u, gy = it / 32u;\n"
        "        f32 cx = 16.0f + (f32)gx * 15.0f + rng_f32(&rng) * 4.0f;\n"
        "        f32 cy = 16.0f + (f32)gy * 15.0f + rng_f32(&rng) * 4.0f;\n"
        "        f32 rad = (it < 12u) ? 95.0f : (8.0f + rng_f32(&rng) * 14.0f);\n"
        "        u32 sc = 0;\n"
        "        for (u32 t = 0; t < TPI; t++) {\n"
        "            f32 a0 = (f32)t * (M_TAU / (f32)TPI), a1 = a0 + 0.9f, a2 = a0 + 1.8f;\n"
        "            f32 zb = 0.15f + 0.7f * ((f32)((it * 7u + t) % 100u) / 100.0f);\n"
        "            RasterVert v0 = { cx + m_cos(a0) * rad * 0.2f, cy + m_sin(a0) * rad * 0.2f, zb, 1.0f, {0} };\n"
        "            RasterVert v1 = { cx + m_cos(a1) * rad, cy + m_sin(a1) * rad, zb + 0.03f, 1.0f, {0} };\n"
        "            RasterVert v2 = { cx + m_cos(a2) * rad, cy + m_sin(a2) * rad, zb - 0.02f, 1.0f, {0} };\n"
        "            if (raster_setup_tri(&v0, &v1, &v2, &g_st_fast, FALSE, SW, SH, &g_tris_fast[it * TPI + sc], NULL)) {\n"
        "                g_tris_ref[it * TPI + sc] = g_tris_fast[it * TPI + sc];\n"
        "                g_tris_ref[it * TPI + sc].state = &g_st_ref;\n"
        "                sc++;\n"
        "            }\n"
        "        }\n"
        "        g_cnts[it] = sc;\n"
        "    }\n"
        "    /* 1) 4T paralel binleme vs 1T seri binleme + fast vs ref bit-bit doğrulama */\n"
        "    fb_clear_depth(&g_fb_fast, 1.0f); fb_clear_depth(&g_fb_ref, 1.0f);\n"
        "    arena_reset(&f1); arena_reset(&f2);\n"
        "    engine_set_worker_count(3u);\n"
        "    raster_begin(&g_r1, &g_fb_fast, &f1);\n"
        "    for (u32 it = 0; it < NITEMS; it++) raster_bin_tris(&g_r1, &g_tris_fast[it * TPI], g_cnts[it], TPI);\n"
        "    raster_flush(&g_r1);\n"
        "    engine_set_worker_count(0u);\n"
        "    raster_begin(&g_r2, &g_fb_ref, &f2);\n"
        "    for (u32 it = 0; it < NITEMS; it++) raster_bin_tris(&g_r2, &g_tris_ref[it * TPI], g_cnts[it], TPI);\n"
        "    raster_flush(&g_r2);\n"
        "    if (g_r1.stat_tris_binned != g_r2.stat_tris_binned) return 1000000u;\n"
        "    const u32* d1 = (const u32*)g_fb_fast.depth;\n"
        "    const u32* d2 = (const u32*)g_fb_ref.depth;\n"
        "    u32 diffs = 0;\n"
        "    for (u32 i = 0; i < SW * SH; i++) if (d1[i] != d2[i]) diffs++;\n"
        "    if (diffs != 0u) return diffs;\n"
        "    /* 2) Medyan süre ölçümü (15 tekrar, 3 ısınma) */\n"
        "    f64 t_bin[15], t_fast[15], t_ref[15];\n"
        "    for (u32 k = 0; k < 18u; k++) {\n"
        "        arena_reset(&f1); fb_clear_depth(&g_fb_fast, 1.0f);\n"
        "        raster_begin(&g_r1, &g_fb_fast, &f1);\n"
        "        f64 t0 = plat_time_ms();\n"
        "        for (u32 it = 0; it < NITEMS; it++) raster_bin_tris(&g_r1, &g_tris_fast[it * TPI], g_cnts[it], TPI);\n"
        "        raster_flush_bins(&g_r1);\n"
        "        f64 t1 = plat_time_ms();\n"
        "        raster_flush(&g_r1);\n"
        "        f64 t2 = plat_time_ms();\n"
        "        arena_reset(&f2); fb_clear_depth(&g_fb_ref, 1.0f);\n"
        "        raster_begin(&g_r2, &g_fb_ref, &f2);\n"
        "        for (u32 it = 0; it < NITEMS; it++) raster_bin_tris(&g_r2, &g_tris_ref[it * TPI], g_cnts[it], TPI);\n"
        "        raster_flush_bins(&g_r2);\n"
        "        f64 t3 = plat_time_ms();\n"
        "        raster_flush(&g_r2);\n"
        "        f64 t4 = plat_time_ms();\n"
        "        if (k >= 3u) {\n"
        "            t_bin[k - 3u]  = t1 - t0;\n"
        "            t_fast[k - 3u] = t2 - t1;\n"
        "            t_ref[k - 3u]  = t4 - t3;\n"
        "        }\n"
        "    }\n"
        "    g_med_bin  = sort_med(t_bin, 15u);\n"
        "    g_med_fast = sort_med(t_fast, 15u);\n"
        "    g_med_ref  = sort_med(t_ref, 15u);\n"
        "    return 0u;\n"
        "}\n"
        "EXPORT(m14a_bin_ms)  f64 m14a_bin_ms(void)  { return g_med_bin; }\n"
        "EXPORT(m14a_fast_ms) f64 m14a_fast_ms(void) { return g_med_fast; }\n"
        "EXPORT(m14a_ref_ms)  f64 m14a_ref_ms(void)  { return g_med_ref; }\n";

    fwrite(src, 1, strlen(src), fp);
    fclose(fp);

    int rc_compile = system(
        "clang --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib "
        "-msimd128 -matomics -mbulk-memory -mnontrapping-fptoint -msign-ext -DPLAT_SHARED_MEMORY=1 -Isrc "
        "-Wl,--no-entry -Wl,--export-dynamic -Wl,--import-memory -Wl,--shared-memory "
        "-Wl,--max-memory=268435456 -Wl,--stack-first -z stack-size=1048576 "
        "build/wasm/m14a_simd_test.c "
        "build/wasm/src/core/arena.o build/wasm/src/core/math.o build/wasm/src/core/rng.o "
        "build/wasm/src/core/log.o build/wasm/src/core/mem.o build/wasm/src/core/profiler.o "
        "build/wasm/src/thread/thread.o build/wasm/src/thread/job.o "
        "build/wasm/src/raster/framebuffer.o build/wasm/src/raster/ref_shaders.o "
        "build/wasm/src/raster/raster.o build/wasm/src/raster/raster_simd_wasm.o "
        "-o build/wasm/m14a_simd_test.wasm"
    );
    remove(c_path);
    CHECK(rc_compile == 0);
    if (rc_compile != 0) return;

    int rc_node = system(
        "node -e '"
        "const fs = require(\"fs\");"
        "const { performance } = require(\"perf_hooks\");"
        "const bytes = fs.readFileSync(\"build/wasm/m14a_simd_test.wasm\");"
        "const mod = new WebAssembly.Module(bytes);"
        "const memory = new WebAssembly.Memory({ initial: 1024, maximum: 4096, shared: true });"
        "const inst = new WebAssembly.Instance(mod, {"
        "  env: { memory, plat_log: ()=>{}, plat_panic: ()=>{ throw new Error(\"panic\"); }, plat_time_ms: ()=>performance.now(), plat_jit_load: ()=>-1 }"
        "});"
        "const diffs = inst.exports.m14a_run() >>> 0;"
        "if (diffs !== 0) { console.error(\"m14a wasm diffs:\", diffs); process.exit(1); }"
        "const b = inst.exports.m14a_bin_ms();"
        "const f = inst.exports.m14a_fast_ms();"
        "const r = inst.exports.m14a_ref_ms();"
        "console.log(`  [bench M14_A wasm SIMD] 512x512 6k ucgen golge: bin=${b.toFixed(3)} ms | scan(genel)=${r.toFixed(3)} ms -> scan(depth-only)=${f.toFixed(3)} ms (${(r/f).toFixed(2)}x, diff=0)`);"
        "'"
    );
    remove(wasm_path);
    CHECK(rc_node == 0);
}

TEST(raster_resolution_resize_50x_no_leak) {
    static u8 fb_buf[MB(32)];
    static u8 frame_buf[MB(16)];
    Arena fb_arena;
    Arena frame_arena;
    arena_init(&fb_arena, fb_buf, sizeof(fb_buf));
    arena_init(&frame_arena, frame_buf, sizeof(frame_buf));

    Framebuffer fb;
    Rasterizer r;

    const u32 test_resolutions[4][2] = {
        { 640,  360 },
        { 960,  540 },
        { 1280, 720 },
        { 1920, 1080 }
    };

    /* İlk 640x360 durumundaki bellek kullanımını al */
    arena_reset(&fb_arena);
    fb_init(&fb, &fb_arena, 640, 360);
    fb_init_depth(&fb, &fb_arena);
    usize initial_fb_used = fb_arena.used;
    CHECK(initial_fb_used == (usize)(640 * 360 * 4 + 640 * 360 * 4));

    /* 50 kez çözünürlük değiştir ve her seferinde rasterizer ile kare çiz */
    for (u32 iter = 0; iter < 50; iter++) {
        u32 mode = iter % 4;
        u32 w = test_resolutions[mode][0];
        u32 h = test_resolutions[mode][1];

        arena_reset(&fb_arena);
        fb_init(&fb, &fb_arena, w, h);
        fb_init_depth(&fb, &fb_arena);

        CHECK(fb.w == w);
        CHECK(fb.h == h);
        CHECK(fb.px != NULL);
        CHECK(fb.depth != NULL);
        CHECK(fb_arena.used == (usize)w * (usize)h * 8);

        arena_reset(&frame_arena);
        fb_clear(&fb, FB_RGBA(20, 30, 40, 255));
        fb_clear_depth(&fb, 1.0f);

        raster_begin(&r, &fb, &frame_arena);
        RasterVert v0 = { 10.0f, 10.0f, 0.5f, 1.0f, {0} };
        RasterVert v1 = { (f32)w - 10.0f, 20.0f, 0.5f, 1.0f, {0} };
        RasterVert v2 = { (f32)w * 0.5f, (f32)h - 10.0f, 0.5f, 1.0f, {0} };
        raster_triangle(&r, v0, v1, v2);
        raster_flush(&r);

        CHECK(r.tiles_x == (w + RASTER_TILE_SIZE - 1u) / RASTER_TILE_SIZE);
        CHECK(r.tiles_y == (h + RASTER_TILE_SIZE - 1u) / RASTER_TILE_SIZE);
    }

    /* 50 döngü sonra 640x360'a dön ve ilk kullanımla karşılaştır */
    arena_reset(&fb_arena);
    fb_init(&fb, &fb_arena, 640, 360);
    fb_init_depth(&fb, &fb_arena);
    usize final_fb_used = fb_arena.used;

    CHECK(final_fb_used == initial_fb_used);
}
