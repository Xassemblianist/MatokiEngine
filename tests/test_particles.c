/* tests/test_particles.c — 3D billboard toplayıcı ve parçacık sistemi testleri + 5000 parçacık benchmark'ı.
 * Kamera dikliği (normal · ileri ≈ -1), perspektifte ekran karesi (en/boy ≈ 1), kapasite taşması,
 * bozuk/düşmanca girdi (0/negatif/NaN/Inf), radix sıralama, determinizm ve analitik ömür testlerini içerir.
 */
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"
#include "plat/platform.h"
#include "raster/framebuffer.h"
#include "raster/raster.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/billboard.h"
#include "gfx3d/particles.h"

static u8 g_mem[MB(32)];

static inline f32 make_nan(void) {
    u32 u = 0x7FC00001u;
    f32 x;
    __builtin_memcpy(&x, &u, sizeof(x));
    return x;
}

static inline f32 make_inf(b32 neg) {
    u32 u = neg ? 0xFF800000u : 0x7F800000u;
    f32 x;
    __builtin_memcpy(&x, &u, sizeof(x));
    return x;
}

/* Köşe rengini (G3D_ATTR_COLOR: 4 varying) doğrudan çıktıya yazan test shader'ı */
static void shade_vcolor(ShadeBatch* b) {
    u32 n = b->count * 4u;
    for (u32 i = 0; i < n; i++) {
        b->out_rgba[i]          = b->varyings[i];
        b->out_rgba[n + i]      = b->varyings[n + i];
        b->out_rgba[2u * n + i] = b->varyings[2u * n + i];
        b->out_rgba[3u * n + i] = b->varyings[3u * n + i];
    }
}

/* UV (2) + COLOR (4) varying'lerini okuyan test shader'ı */
static void shade_uv_color(ShadeBatch* b) {
    u32 n = b->count * 4u;
    for (u32 i = 0; i < n; i++) {
        f32 u = b->varyings[i];
        f32 v = b->varyings[n + i];
        f32 r = b->varyings[2u * n + i];
        f32 g = b->varyings[3u * n + i];
        f32 bl = b->varyings[4u * n + i];
        f32 a = b->varyings[5u * n + i];
        b->out_rgba[i]          = r * 0.5f + u * 0.5f;
        b->out_rgba[n + i]      = g * 0.5f + v * 0.5f;
        b->out_rgba[2u * n + i] = bl;
        b->out_rgba[3u * n + i] = a;
    }
}

/* Her kamera yönünde billboard normal · ileri ≈ -1 ve perspektifte ekranda kare (en/boy ≈ 1) */
TEST(billboard_orientation_and_screen_square) {
    Arena perm, frame;
    arena_init(&perm, g_mem, MB(16));
    arena_init(&frame, g_mem + MB(16), MB(16));

    Framebuffer fb;
    Rasterizer r;
    fb_init(&fb, &perm, 640, 360);
    fb_init_depth(&fb, &perm);

    BillboardBatch bb;
    CHECK(billboard_init(&bb, &perm, 128));

    Rng rng;
    rng_seed(&rng, 42);

    /* 50 farklı kamera konumu, bakış açısı (yaw/pitch) ve dörtgen dönüşü (rot) */
    for (u32 it = 0; it < 50; it++) {
        Camera cam;
        camera_init(&cam, (it & 1u) ? CAM_ORBIT : CAM_FLY);
        cam.pos    = vec3(rng_f32(&rng) * 20.0f - 10.0f,
                          rng_f32(&rng) * 10.0f - 5.0f,
                          rng_f32(&rng) * 20.0f - 10.0f);
        cam.target = vec3(rng_f32(&rng) * 6.0f - 3.0f,
                          rng_f32(&rng) * 6.0f - 3.0f,
                          rng_f32(&rng) * 6.0f - 3.0f);
        cam.dist   = 4.0f + rng_f32(&rng) * 10.0f;
        cam.yaw    = (rng_f32(&rng) * 2.0f - 1.0f) * M_PI;
        cam.pitch  = (rng_f32(&rng) * 2.0f - 1.0f) * 1.2f;

        Mat4 view = camera_view(&cam);
        Vec3 fwd  = camera_forward(&cam);
        Vec3 eye  = camera_eye(&cam);

        billboard_begin(&bb, view);
        CHECK_NEAR(vec3_dot(bb.normal, fwd), -1.0f, 1e-5f);
        CHECK_NEAR(vec3_len(bb.cam_right), 1.0f, 1e-5f);
        CHECK_NEAR(vec3_len(bb.cam_up), 1.0f, 1e-5f);
        CHECK_NEAR(vec3_dot(bb.cam_right, bb.cam_up), 0.0f, 1e-5f);
        CHECK_NEAR(vec3_dot(bb.cam_right, fwd), 0.0f, 1e-5f);
        CHECK_NEAR(vec3_dot(bb.cam_up, fwd), 0.0f, 1e-5f);
        CHECK_NEAR(bb.eye.x, eye.x, 1e-4f);
        CHECK_NEAR(bb.eye.y, eye.y, 1e-4f);
        CHECK_NEAR(bb.eye.z, eye.z, 1e-4f);

        f32 dist = 6.0f;
        f32 size = 1.5f;
        f32 rot  = (it == 0u) ? 0.0f : (rng_f32(&rng) * M_TAU - M_PI);
        Vec3 center = vec3_add(eye, vec3_scale(fwd, dist));
        CHECK(billboard_push(&bb, center, size, rot, FB_RGBA(255, 255, 255, 255)));

        Mesh* m = billboard_end(&bb);
        CHECK(m != NULL);
        CHECK(m->vertex_count == 4u);
        CHECK(m->index_count == 6u);

        /* Köşe normalleri ve geometrik üçgen normalleri kameraya tam dik olmalı */
        for (u32 v = 0; v < 4u; v++) {
            Vec3 vn = vec3(m->nx[v], m->ny[v], m->nz[v]);
            CHECK_NEAR(vec3_dot(vn, fwd), -1.0f, 1e-5f);
        }
        Vec3 p0 = vec3(m->px[0], m->py[0], m->pz[0]);
        Vec3 p1 = vec3(m->px[1], m->py[1], m->pz[1]);
        Vec3 p2 = vec3(m->px[2], m->py[2], m->pz[2]);
        Vec3 p3 = vec3(m->px[3], m->py[3], m->pz[3]);

        Vec3 ex = vec3_sub(p1, p0);
        Vec3 ey = vec3_sub(p3, p0);
        CHECK_NEAR(vec3_len(ex), size, 1e-4f);
        CHECK_NEAR(vec3_len(ey), size, 1e-4f);
        CHECK_NEAR(vec3_dot(ex, ey), 0.0f, 1e-4f);
        CHECK_NEAR(vec3_dot(ex, fwd), 0.0f, 1e-4f);
        CHECK_NEAR(vec3_dot(ey, fwd), 0.0f, 1e-4f);

        Vec3 gn0 = vec3_norm(vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0)));
        Vec3 gn1 = vec3_norm(vec3_cross(vec3_sub(p2, p0), vec3_sub(p3, p0)));
        CHECK_NEAR(vec3_dot(gn0, fwd), -1.0f, 1e-5f);
        CHECK_NEAR(vec3_dot(gn1, fwd), -1.0f, 1e-5f);

        /* Perspektif izdüşümde ekran uzayı kenar uzunlukları eşit (en/boy ≈ 1) olmalı */
        Mat4 proj = camera_proj(&cam, 640.0f / 360.0f);
        Mat4 vp   = mat4_mul(proj, view);
        Vec2 sc[4];
        for (u32 v = 0; v < 4u; v++) {
            Vec4 c = mat4_mul_vec4(vp, vec4(m->px[v], m->py[v], m->pz[v], 1.0f));
            f32 iw = 1.0f / c.w;
            sc[v].x = (c.x * iw * 0.5f + 0.5f) * 640.0f;
            sc[v].y = (0.5f - c.y * iw * 0.5f) * 360.0f;
        }
        f32 sw = vec2_len(vec2_sub(sc[1], sc[0]));
        f32 sh = vec2_len(vec2_sub(sc[3], sc[0]));
        CHECK(sw > 20.0f && sh > 20.0f);
        CHECK_NEAR(sw / sh, 1.0f, 1e-4f);
        CHECK_NEAR(vec2_dot(vec2_sub(sc[1], sc[0]), vec2_sub(sc[3], sc[0])), 0.0f, 1e-2f);
    }

    /* Gerçek rasterizer ile perspektifte rot=0 dörtgenin ekranda kare piksel kutusu kapladığını doğrula */
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos   = vec3(2.0f, -1.0f, 5.0f);
    cam.yaw   = 0.42f;
    cam.pitch = -0.18f;
    Mat4 view = camera_view(&cam);
    Mat4 proj = camera_proj(&cam, 640.0f / 360.0f);
    Vec3 eye  = camera_eye(&cam);
    Vec3 fwd  = camera_forward(&cam);

    arena_reset(&frame);
    fb_clear(&fb, 0);
    fb_clear_depth(&fb, 0.0f);
    raster_begin(&r, &fb, &frame);

    G3dContext ctx;
    g3d_begin(&ctx, &r, &frame, view, proj, eye);

    billboard_begin(&bb, view);
    billboard_push(&bb, vec3_add(eye, vec3_scale(fwd, 5.0f)), 2.0f, 0.0f, FB_RGBA(255, 128, 64, 255));

    G3dMaterial mat;
    __builtin_memset(&mat, 0, sizeof(mat));
    mat.shade_fn    = shade_vcolor;
    mat.blend       = BLEND_NONE;
    mat.cull        = G3D_CULL_BACK;
    mat.depth_test  = TRUE;
    mat.depth_write = TRUE;
    mat.attrs[0]    = G3D_ATTR_COLOR;
    mat.attr_count  = 1;

    CHECK(billboard_draw(&bb, &ctx, &mat));
    raster_flush(&r);
    CHECK(ctx.stats.draws == 1u);
    CHECK(ctx.stats.tris_out == 2u);
    CHECK(ctx.stats.tris_backface == 0u);

    u32 min_x = 640, max_x = 0, min_y = 360, max_y = 0, painted = 0;
    for (u32 y = 0; y < 360; y++) {
        for (u32 x = 0; x < 640; x++) {
            u32 px = fb.px[y * 640 + x];
            if (px != 0u) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                painted++;
                CHECK(px == FB_RGBA(255, 128, 64, 255));
            }
        }
    }
    CHECK(painted > 1000u);
    f32 pw = (f32)(max_x - min_x + 1u);
    f32 ph = (f32)(max_y - min_y + 1u);
    CHECK_NEAR(pw / ph, 1.0f, 0.025f);
}

/* Köşe rengi (G3D_ATTR_COLOR) ve doku atlası UV koordinatlarının doğrulanması */
TEST(billboard_vertex_color_and_atlas) {
    Arena a;
    arena_init(&a, g_mem, MB(4));

    BillboardBatch bb;
    CHECK(billboard_init(&bb, &a, 16));
    billboard_begin(&bb, mat4_identity());

    u32 c0 = FB_RGBA(255, 10, 20, 255);
    u32 c1 = FB_RGBA(30, 240, 40, 128);
    CHECK(billboard_push_atlas(&bb, vec3(-1.0f, 0.0f, -4.0f), 1.0f, 0.0f, c0, 4, 4, 0));
    CHECK(billboard_push_atlas(&bb, vec3( 1.0f, 0.0f, -4.0f), 1.0f, 0.0f, c1, 4, 4, 6));

    Mesh* m = billboard_end(&bb);
    CHECK(m != NULL);
    CHECK(m->vertex_count == 8u);
    CHECK(m->index_count == 12u);

    /* Kare 0: col=0, row=0 -> u in [0, 0.25], v in [0, 0.25] */
    CHECK_NEAR(m->u[0], 0.0f,  1e-6f); CHECK_NEAR(m->v[0], 0.25f, 1e-6f);
    CHECK_NEAR(m->u[1], 0.25f, 1e-6f); CHECK_NEAR(m->v[1], 0.25f, 1e-6f);
    CHECK_NEAR(m->u[2], 0.25f, 1e-6f); CHECK_NEAR(m->v[2], 0.0f,  1e-6f);
    CHECK_NEAR(m->u[3], 0.0f,  1e-6f); CHECK_NEAR(m->v[3], 0.0f,  1e-6f);
    CHECK(m->color[0] == c0 && m->color[3] == c0);

    /* Kare 6 (4x4): col=2, row=1 -> u in [0.5, 0.75], v in [0.25, 0.5] */
    CHECK_NEAR(m->u[4], 0.5f,  1e-6f); CHECK_NEAR(m->v[4], 0.5f,  1e-6f);
    CHECK_NEAR(m->u[5], 0.75f, 1e-6f); CHECK_NEAR(m->v[5], 0.5f,  1e-6f);
    CHECK_NEAR(m->u[6], 0.75f, 1e-6f); CHECK_NEAR(m->v[6], 0.25f, 1e-6f);
    CHECK_NEAR(m->u[7], 0.5f,  1e-6f); CHECK_NEAR(m->v[7], 0.25f, 1e-6f);
    CHECK(m->color[4] == c1 && m->color[7] == c1);
}

/* Kapasite taşması sayacı ve sıfır/negatif/NaN/Inf boyut/konum filtreleme */
TEST(billboard_capacity_and_invalid_inputs) {
    Arena a;
    arena_init(&a, g_mem, MB(2));

    BillboardBatch bb;
    /* Geçersiz kapasiteler ve küçük arena PANIC yapmadan FALSE dönmeli */
    CHECK(!billboard_init(NULL, &a, 16));
    CHECK(!billboard_init(&bb, NULL, 16));
    CHECK(!billboard_init(&bb, &a, 0));
    CHECK(!billboard_init(&bb, &a, BILLBOARD_MAX_CAPACITY + 1u));
    CHECK(!billboard_init(&bb, &a, U32_MAX));

    Arena tiny;
    arena_init(&tiny, g_mem, 256);
    CHECK(!billboard_init(&bb, &tiny, 32));

    /* Kapasite = 8 olan geçerli bir toplayıcı */
    CHECK(billboard_init(&bb, &a, 8));
    billboard_begin(&bb, mat4_identity());

    f32 nan_v = make_nan();
    f32 inf_p = make_inf(FALSE);
    f32 inf_n = make_inf(TRUE);
    u32 col   = FB_RGBA(255, 255, 255, 255);

    /* Sıfır, negatif, NaN, ±Inf boyutlar atlanmalı */
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),  0.0f,  0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5), -1.0f,  0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),  nan_v, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),  inf_p, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),  inf_n, 0.0f, col));
    CHECK(!billboard_push_ex(&bb, vec3(0, 0, -5), vec2(1.0f, 0.0f), 0.0f, col, billboard_uv_full()));
    CHECK(!billboard_push_ex(&bb, vec3(0, 0, -5), vec2(0.0f, 1.0f), 0.0f, col, billboard_uv_full()));
    CHECK(!billboard_push_ex(&bb, vec3(0, 0, -5), vec2(1.0f, -0.5f), 0.0f, col, billboard_uv_full()));

    /* NaN, ±Inf ve aşırı büyük konumlar / açılar / UV'ler atlanmalı */
    CHECK(!billboard_push(&bb, vec3(nan_v, 0, -5), 1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, nan_v, -5), 1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, nan_v),  1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(inf_p, 0, -5), 1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, inf_n, -5), 1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(1e20f, 0, -5), 1.0f, 0.0f, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),     1.0f, nan_v, col));
    CHECK(!billboard_push(&bb, vec3(0, 0, -5),     1.0f, inf_p, col));
    CHECK(!billboard_push_ex(&bb, vec3(0, 0, -5), vec2(1, 1), 0.0f, col,
                             billboard_uv_rect(nan_v, 0, 1, 1)));

    CHECK(bb.count == 0u);
    CHECK(bb.skipped_count == 17u);
    CHECK(bb.overflow_count == 0u);

    /* (0,0,0) konumu ve büyük sonlu dönüş açıları geçerlidir */
    for (u32 i = 0; i < 20u; i++) {
        b32 ok = billboard_push(&bb, vec3(0.0f, 0.0f, -(f32)i), 1.0f, (f32)i * 1.0e7f, col);
        if (i < 8u) CHECK(ok);
        else        CHECK(!ok);
    }
    CHECK(bb.count == 8u);
    CHECK(bb.overflow_count == 12u);
    CHECK(bb.skipped_count == 17u);

    Mesh* m = billboard_end(&bb);
    CHECK(m != NULL && m->vertex_count == 32u && m->index_count == 48u);
}

/* Radix sıralama: arkadan öne (azalan uzaklık), eşit uzaklıkta kararlılık ve alfa harmanlama */
TEST(billboard_radix_sort_back_to_front) {
    Arena perm, frame;
    arena_init(&perm, g_mem, MB(16));
    arena_init(&frame, g_mem + MB(16), MB(16));

    BillboardBatch bb;
    CHECK(billboard_init(&bb, &perm, 1024));

    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos   = vec3(3.0f, 2.0f, 8.0f);
    cam.yaw   = -0.25f;
    cam.pitch = 0.1f;
    Mat4 view = camera_view(&cam);
    Vec3 eye  = camera_eye(&cam);

    Rng rng;
    rng_seed(&rng, 99);
    billboard_begin(&bb, view);

    for (u32 i = 0; i < 600u; i++) {
        Vec3 p = vec3((rng_f32(&rng) - 0.5f) * 100.0f,
                      (rng_f32(&rng) - 0.5f) * 100.0f,
                      (rng_f32(&rng) - 0.5f) * 100.0f);
        CHECK(billboard_push(&bb, p, 0.5f, 0.0f, i));
    }
    /* Aynı konumda iki dörtgen (kararlılık testi) */
    CHECK(billboard_push(&bb, vec3(0.0f, 0.0f, -10.0f), 0.5f, 0.0f, 0xAAAA0001u));
    CHECK(billboard_push(&bb, vec3(0.0f, 0.0f, -10.0f), 0.5f, 0.0f, 0xAAAA0002u));

    billboard_sort(&bb);
    Mesh* m = billboard_end(&bb);
    CHECK(m->vertex_count == bb.count * 4u);

    f32 prev_d2 = 1.0e38f;
    u32 tie_seen = 0u;
    for (u32 i = 0; i < bb.count; i++) {
        u32 idx = bb.order[i];
        Vec3 d = vec3_sub(bb.items[idx].pos, eye);
        f32 d2 = vec3_dot(d, d);
        CHECK(d2 <= prev_d2);
        prev_d2 = d2;
        if (m->color[i * 4u] == 0xAAAA0001u) {
            CHECK(tie_seen == 0u);
            tie_seen = 1u;
        } else if (m->color[i * 4u] == 0xAAAA0002u) {
            CHECK(tie_seen == 1u);
            tie_seen = 2u;
        }
    }
    CHECK(tie_seen == 2u);

    /* Alfa harmanlı çizimde karışık sırada eklenen 3 dörtgenin arkadan öne doğru birleşmesi */
    Framebuffer fb;
    Rasterizer r;
    fb_init(&fb, &perm, 128, 128);
    fb_init_depth(&fb, &perm);
    fb_clear(&fb, 0);
    fb_clear_depth(&fb, 0.0f);
    raster_begin(&r, &fb, &frame);

    Camera cam0;
    camera_init(&cam0, CAM_FLY);
    cam0.pos = vec3(0.0f, 0.0f, 0.0f);
    cam0.yaw = 0.0f;
    cam0.pitch = 0.0f;
    Mat4 v0 = camera_view(&cam0);
    Mat4 p0 = camera_proj(&cam0, 1.0f);

    G3dContext ctx;
    g3d_begin(&ctx, &r, &frame, v0, p0, cam0.pos);

    /* Sırasız ekle: önce yakın (z=-2, yarı saydam mavi), sonra uzak (z=-6, tam opak kırmızı) */
    billboard_begin(&bb, v0);
    billboard_push(&bb, vec3(0.0f, 0.0f, -2.0f), 2.0f, 0.0f, FB_RGBA(0, 0, 255, 128));
    billboard_push(&bb, vec3(0.0f, 0.0f, -6.0f), 6.0f, 0.0f, FB_RGBA(255, 0, 0, 255));
    billboard_sort(&bb);

    G3dMaterial mat;
    __builtin_memset(&mat, 0, sizeof(mat));
    mat.shade_fn    = shade_vcolor;
    mat.blend       = BLEND_ALPHA;
    mat.cull        = G3D_CULL_BACK;
    mat.depth_test  = TRUE;
    mat.depth_write = FALSE;
    mat.attrs[0]    = G3D_ATTR_COLOR;
    mat.attr_count  = 1;

    CHECK(billboard_draw(&bb, &ctx, &mat));
    raster_flush(&r);

    /* Merkez piksel: arkada kırmızı (255,0,0), üstünde %50 mavi -> R ~ 127, B ~ 128 */
    u32 center_px = fb.px[64 * 128 + 64];
    u32 cr = center_px & 0xFFu;
    u32 cg = (center_px >> 8) & 0xFFu;
    u32 cb = (center_px >> 16) & 0xFFu;
    CHECK(cr >= 125u && cr <= 130u);
    CHECK(cg == 0u);
    CHECK(cb >= 125u && cb <= 130u);
}

/* Parçacık sistemi: aynı tohumla iki koşu bit-bit aynı, boyut/renk eğrisi ve fizik doğrulaması */
TEST(particles_determinism_and_curves) {
    Arena a1, a2;
    arena_init(&a1, g_mem, MB(16));
    arena_init(&a2, g_mem + MB(16), MB(16));

    ParticleSystem ps1, ps2;
    BillboardBatch bb1, bb2;
    CHECK(particles_init(&ps1, &a1, 2048, 0xCAFEBABE12345678ULL));
    CHECK(particles_init(&ps2, &a2, 2048, 0xCAFEBABE12345678ULL));
    CHECK(billboard_init(&bb1, &a1, 2048));
    CHECK(billboard_init(&bb2, &a2, 2048));

    ParticleEmitter em_pt = particles_default_emitter();
    em_pt.shape       = EMIT_SHAPE_POINT;
    em_pt.pos         = vec3(1.0f, 2.0f, -5.0f);
    em_pt.vel_min     = vec3(-3.0f, 2.0f, -3.0f);
    em_pt.vel_max     = vec3( 3.0f, 8.0f,  3.0f);
    em_pt.life_min    = 0.3f;
    em_pt.life_max    = 1.8f;
    em_pt.gravity     = vec3(0.0f, -9.8f, 0.0f);
    em_pt.drag        = 0.8f;
    em_pt.size_start  = 0.4f;
    em_pt.size_end    = 0.05f;
    em_pt.color_start = vec4(1.0f, 0.8f, 0.2f, 1.0f);
    em_pt.color_end   = vec4(0.5f, 0.1f, 0.0f, 0.0f);
    em_pt.rot_min     = -M_PI;
    em_pt.rot_max     =  M_PI;
    em_pt.rot_vel_min = -4.0f;
    em_pt.rot_vel_max =  4.0f;

    ParticleEmitter em_sp = particles_default_emitter();
    em_sp.shape         = EMIT_SHAPE_SPHERE;
    em_sp.pos           = vec3(-2.0f, 0.5f, -8.0f);
    em_sp.radius        = 1.5f;
    em_sp.vel_min       = vec3(0.0f, 0.5f, 0.0f);
    em_sp.vel_max       = vec3(0.0f, 1.5f, 0.0f);
    em_sp.speed_min     = 1.0f;
    em_sp.speed_max     = 5.0f;
    em_sp.life_min      = 0.4f;
    em_sp.life_max      = 1.4f;
    em_sp.atlas_cols    = 4;
    em_sp.atlas_rows    = 4;
    em_sp.animate_atlas = TRUE;

    CHECK(particles_emit_burst(&ps1, &em_pt, 500) == 500u);
    CHECK(particles_emit_burst(&ps2, &em_pt, 500) == 500u);
    CHECK(particles_emit_burst(&ps1, &em_sp, 500) == 500u);
    CHECK(particles_emit_burst(&ps2, &em_sp, 500) == 500u);

    for (u32 step = 0; step < 60u; step++) {
        f32 dt = 1.0f / 60.0f;
        if (step == 20u || step == 40u) {
            particles_emit_burst(&ps1, &em_pt, 150);
            particles_emit_burst(&ps2, &em_pt, 150);
        }
        u32 e1 = particles_update(&ps1, dt);
        u32 e2 = particles_update(&ps2, dt);
        CHECK(e1 == e2);
        CHECK(ps1.count == ps2.count);
    }

    CHECK(ps1.count > 100u && ps1.count < 1300u);
    CHECK(ps1.expired_total == ps2.expired_total);
    CHECK(__builtin_memcmp(ps1.items, ps2.items, sizeof(Particle) * ps1.count) == 0);

    Camera cam;
    camera_init(&cam, CAM_FLY);
    Mat4 view = camera_view(&cam);
    Mesh* m1 = particles_build_billboards(&ps1, &bb1, view, TRUE);
    Mesh* m2 = particles_build_billboards(&ps2, &bb2, view, TRUE);
    CHECK(m1->vertex_count == m2->vertex_count);
    CHECK(m1->index_count == m2->index_count);
    usize vbytes = sizeof(f32) * m1->vertex_count;
    CHECK(__builtin_memcmp(m1->px, m2->px, vbytes) == 0);
    CHECK(__builtin_memcmp(m1->py, m2->py, vbytes) == 0);
    CHECK(__builtin_memcmp(m1->pz, m2->pz, vbytes) == 0);
    CHECK(__builtin_memcmp(m1->u,  m2->u,  vbytes) == 0);
    CHECK(__builtin_memcmp(m1->v,  m2->v,  vbytes) == 0);
    CHECK(__builtin_memcmp(m1->color, m2->color, sizeof(u32) * m1->vertex_count) == 0);

    /* Boyut ve renk eğrisi analitik doğrulaması */
    particles_reset(&ps1, 1);
    ParticleEmitter ec = particles_default_emitter();
    ec.pos         = vec3(0.0f, 0.0f, 0.0f);
    ec.vel_min     = vec3(2.0f, 0.0f, 0.0f);
    ec.vel_max     = vec3(2.0f, 0.0f, 0.0f);
    ec.gravity     = vec3(0.0f, 0.0f, 0.0f);
    ec.drag        = 0.0f;
    ec.life_min    = 2.0f;
    ec.life_max    = 2.0f;
    ec.size_start  = 1.0f;
    ec.size_end    = 3.0f;
    ec.color_start = vec4(1.0f, 0.0f, 0.0f, 1.0f);
    ec.color_end   = vec4(0.0f, 1.0f, 0.0f, 0.0f);
    CHECK(particles_emit_burst(&ps1, &ec, 1) == 1u);

    particles_update(&ps1, 1.0f); /* t = 0.5 */
    CHECK(ps1.count == 1u);
    CHECK_NEAR(ps1.items[0].pos.x, 2.0f, 1e-5f);
    CHECK_NEAR(ps1.items[0].size,  2.0f, 1e-5f);
    u32 col = ps1.items[0].color;
    CHECK((col & 0xFFu) >= 126u && (col & 0xFFu) <= 129u);
    CHECK(((col >> 8) & 0xFFu) >= 126u && ((col >> 8) & 0xFFu) <= 129u);
    CHECK(((col >> 16) & 0xFFu) == 0u);
    CHECK(((col >> 24) & 0xFFu) >= 126u && ((col >> 24) & 0xFFu) <= 129u);
}

/* Ömrü dolan parçacık sayısı analitik beklenenle tutarlı; havuz asla taşmaz */
TEST(particles_expiration_and_pool_bounds) {
    Arena a;
    arena_init(&a, g_mem, MB(8));

    ParticleSystem ps;
    CHECK(particles_init(&ps, &a, 500, 777));

    ParticleEmitter e1 = particles_default_emitter();
    e1.life_min = e1.life_max = 0.5f;
    ParticleEmitter e2 = particles_default_emitter();
    e2.life_min = e2.life_max = 1.0f;
    ParticleEmitter e3 = particles_default_emitter();
    e3.life_min = e3.life_max = 1.5f;

    CHECK(particles_emit_burst(&ps, &e1, 100) == 100u);
    CHECK(particles_emit_burst(&ps, &e2, 150) == 150u);
    CHECK(particles_emit_burst(&ps, &e3, 200) == 200u);
    CHECK(ps.count == 450u);

    /* 4 adım × 0.1s = 0.4s: henüz ölen yok */
    for (u32 s = 0; s < 4u; s++) {
        CHECK(particles_update(&ps, 0.1f) == 0u);
    }
    CHECK(ps.count == 450u);
    CHECK(ps.expired_total == 0u);

    /* +0.11s -> t = 0.51s: 1. grup (100 parçacık) tam olarak sona erer */
    CHECK(particles_update(&ps, 0.11f) == 100u);
    CHECK(ps.count == 350u);
    CHECK(ps.expired_total == 100u);

    /* +0.50s -> t = 1.01s: 2. grup (150 parçacık) sona erer */
    CHECK(particles_update(&ps, 0.50f) == 150u);
    CHECK(ps.count == 200u);
    CHECK(ps.expired_total == 250u);

    /* +0.50s -> t = 1.51s: 3. grup (200 parçacık) sona erer */
    CHECK(particles_update(&ps, 0.50f) == 200u);
    CHECK(ps.count == 0u);
    CHECK(ps.expired_total == 450u);

    /* Sürekli [1.0, 3.0] düzgün ömür dağılımında t = 2.0s'de analitik %50 (2000 -> ~1000) sönüm */
    ParticleSystem ps_dist;
    CHECK(particles_init(&ps_dist, &a, 2000, 12345));
    ParticleEmitter ed = particles_default_emitter();
    ed.life_min = 1.0f;
    ed.life_max = 3.0f;
    CHECK(particles_emit_burst(&ps_dist, &ed, 2000) == 2000u);
    for (u32 s = 0; s < 100u; s++) {
        particles_update(&ps_dist, 0.02f);
    }
    CHECK(ps_dist.count + (u32)ps_dist.expired_total == 2000u);
    CHECK(ps_dist.expired_total >= 940u && ps_dist.expired_total <= 1060u);

    /* Havuz taşma koruması ve bozuk girdi denetimi (kanarya korumalı) */
    u32* canary_lo = ARENA_PUSH_ARRAY(&a, u32, 4);
    for (u32 k = 0; k < 4u; k++) canary_lo[k] = 0xDEADBEEFu;

    ParticleSystem ps_small;
    CHECK(particles_init(&ps_small, &a, 64, 999));

    u32* canary_hi = ARENA_PUSH_ARRAY(&a, u32, 4);
    for (u32 k = 0; k < 4u; k++) canary_hi[k] = 0xCAFEBABEu;

    CHECK(particles_emit_burst(&ps_small, &e1, 40) == 40u);
    CHECK(particles_emit_burst(&ps_small, &e1, 100) == 24u);
    CHECK(ps_small.count == 64u);
    CHECK(ps_small.overflow_count == 76u);
    CHECK(particles_emit_burst(&ps_small, &e1, 5000) == 0u);
    CHECK(ps_small.count == 64u);
    CHECK(ps_small.overflow_count == 5076u);

    /* Geçersiz dt ve bozuk yayıcı çağrıları */
    f32 nan_v = make_nan();
    f32 inf_v = make_inf(FALSE);
    CHECK(particles_update(&ps_small, 0.0f) == 0u);
    CHECK(particles_update(&ps_small, -1.0f) == 0u);
    CHECK(particles_update(&ps_small, nan_v) == 0u);
    CHECK(particles_update(&ps_small, inf_v) == 0u);
    CHECK(ps_small.count == 64u);

    ParticleEmitter bad = particles_default_emitter();
    bad.pos.x = nan_v;
    CHECK(particles_emit_burst(&ps_small, &bad, 10) == 0u);
    bad = particles_default_emitter();
    bad.life_min = -1.0f; bad.life_max = 0.0f;
    CHECK(particles_emit_burst(&ps_small, &bad, 10) == 0u);
    bad = particles_default_emitter();
    bad.size_start = 0.0f; bad.size_end = 0.0f;
    CHECK(particles_emit_burst(&ps_small, &bad, 10) == 0u);
    bad = particles_default_emitter();
    bad.drag = -0.5f;
    CHECK(particles_emit_burst(&ps_small, &bad, 10) == 0u);
    CHECK(ps_small.skipped_count == 40u);

    for (u32 k = 0; k < 4u; k++) {
        CHECK(canary_lo[k] == 0xDEADBEEFu);
        CHECK(canary_hi[k] == 0xCAFEBABEu);
    }
}

static void sort_f64(f64* a, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f64 v = a[i];
        u32 j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/* Benchmark: 5000 parçacık update + billboard mesh üretimi (sırasız ve radix sıralamalı) */
TEST(particles_benchmark_5000) {
    Arena a;
    arena_init(&a, g_mem, MB(16));

    ParticleSystem ps;
    BillboardBatch bb;
    CHECK(particles_init(&ps, &a, 5000, 2026));
    CHECK(billboard_init(&bb, &a, 5000));

    ParticleEmitter em = particles_default_emitter();
    em.shape         = EMIT_SHAPE_SPHERE;
    em.pos           = vec3(0.0f, 2.0f, -15.0f);
    em.radius        = 4.0f;
    em.vel_min       = vec3(-2.0f, 1.0f, -2.0f);
    em.vel_max       = vec3( 2.0f, 6.0f,  2.0f);
    em.speed_min     = 1.0f;
    em.speed_max     = 4.0f;
    em.life_min      = 100.0f;
    em.life_max      = 200.0f;
    em.gravity       = vec3(0.0f, -9.8f, 0.0f);
    em.drag          = 0.4f;
    em.size_start    = 0.3f;
    em.size_end      = 0.05f;
    em.color_start   = vec4(1.0f, 0.9f, 0.3f, 1.0f);
    em.color_end     = vec4(0.8f, 0.1f, 0.0f, 0.0f);

    CHECK(particles_emit_burst(&ps, &em, 5000) == 5000u);
    CHECK(ps.count == 5000u);

    Camera cam;
    camera_init(&cam, CAM_FLY);
    Mat4 view = camera_view(&cam);

    /* Isınma */
    for (u32 w = 0; w < 10u; w++) {
        particles_update(&ps, 1.0f / 120.0f);
        Mesh* m = particles_build_billboards(&ps, &bb, view, TRUE);
        CHECK(m->vertex_count == 20000u);
    }

    enum { N_RUNS = 51 };
    f64 t_update[N_RUNS];
    f64 t_mesh_nosort[N_RUNS];
    f64 t_mesh_sorted[N_RUNS];
    f64 t_total_nosort[N_RUNS];
    f64 t_total_sorted[N_RUNS];

    for (u32 r = 0; r < N_RUNS; r++) {
        f64 t0 = plat_time_ms();
        particles_update(&ps, 1.0f / 120.0f);
        f64 t1 = plat_time_ms();
        Mesh* m0 = particles_build_billboards(&ps, &bb, view, FALSE);
        f64 t2 = plat_time_ms();
        Mesh* m1 = particles_build_billboards(&ps, &bb, view, TRUE);
        f64 t3 = plat_time_ms();

        CHECK(m0->vertex_count == 20000u && m1->vertex_count == 20000u);
        t_update[r]       = t1 - t0;
        t_mesh_nosort[r]  = t2 - t1;
        t_mesh_sorted[r]  = t3 - t2;
        t_total_nosort[r] = (t1 - t0) + (t2 - t1);
        t_total_sorted[r] = (t1 - t0) + (t3 - t2);
    }

    sort_f64(t_update, N_RUNS);
    sort_f64(t_mesh_nosort, N_RUNS);
    sort_f64(t_mesh_sorted, N_RUNS);
    sort_f64(t_total_nosort, N_RUNS);
    sort_f64(t_total_sorted, N_RUNS);

    u32 mid = N_RUNS / 2u;
    LOG("  [bench M12_C] 5000 parcacik (host ASan/O1): update=%.3f ms | mesh(sirasiz)=%.3f ms (toplam %.3f ms) | mesh(radix sirali)=%.3f ms (toplam %.3f ms)",
        t_update[mid], t_mesh_nosort[mid], t_total_nosort[mid],
        t_mesh_sorted[mid], t_total_sorted[mid]);

    /* En az bir kez UV + COLOR varying'li çizim yolunu da doğrula */
    Framebuffer fb;
    Rasterizer rast;
    Arena frame;
    arena_init(&frame, g_mem + MB(16), MB(16));
    fb_init(&fb, &a, 128, 128);
    fb_init_depth(&fb, &a);
    fb_clear(&fb, 0);
    fb_clear_depth(&fb, 0.0f);
    raster_begin(&rast, &fb, &frame);
    G3dContext ctx;
    g3d_begin(&ctx, &rast, &frame, view, camera_proj(&cam, 1.0f), camera_eye(&cam));
    G3dMaterial mat;
    __builtin_memset(&mat, 0, sizeof(mat));
    mat.shade_fn    = shade_uv_color;
    mat.blend       = BLEND_ALPHA;
    mat.cull        = G3D_CULL_BACK;
    mat.depth_test  = TRUE;
    mat.depth_write = FALSE;
    mat.attrs[0]    = G3D_ATTR_UV;
    mat.attrs[1]    = G3D_ATTR_COLOR;
    mat.attr_count  = 2;
    CHECK(billboard_draw(&bb, &ctx, &mat));
    raster_flush(&rast);
    CHECK(ctx.stats.draws == 1u);
    CHECK(ctx.stats.tris_out > 0u);
}
