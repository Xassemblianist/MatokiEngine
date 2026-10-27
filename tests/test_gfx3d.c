/* tests/test_gfx3d.c — 3D boru hattı testleri (host raster ile).
 * Guard band sınırında taşmasız kapsama, yakın düzlem kırpmasında varying doğruluğu,
 * kırpılan ızgarada çatlak/çift boyama olmaması, perspektif-doğru UV (ışın izleme referansı),
 * arka yüz ayıklama + derinlik tutarlılığı ve nesne frustum culling'i doğrular.
 */
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "raster/framebuffer.h"
#include "raster/raster.h"
#include "gfx3d/mesh.h"
#include "gfx3d/clip.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/shadow.h"
#include "gfx3d/vertex.h"

static u8 g_mem[MB(48)];

typedef struct { Arena perm, frame; Framebuffer fb; Rasterizer r; } Env;

static void env_init(Env* e, u32 w, u32 h) {
    arena_init(&e->perm, g_mem, MB(8));
    arena_init(&e->frame, g_mem + MB(8), MB(40));
    fb_init(&e->fb, &e->perm, w, h);
    fb_init_depth(&e->fb, &e->perm);
}

static void env_frame(Env* e) {
    arena_reset(&e->frame);
    fb_clear(&e->fb, 0);
    fb_clear_depth(&e->fb, 0.0f);
    raster_begin(&e->r, &e->fb, &e->frame);
}

/* R kanalını 1/255 artıran shader (BLEND_ADD ile piksel başına boyama sayısı) */
static void shade_count(ShadeBatch* b) {
    u32 n = b->count * 4u;
    for (u32 i = 0; i < n; i++) {
        b->out_rgba[i] = 1.0f / 255.0f;
        b->out_rgba[n + i] = 0.0f;
        b->out_rgba[2u * n + i] = 0.0f;
        b->out_rgba[3u * n + i] = 1.0f;
    }
}

/* İlk 3 varying'i renk olarak yazar (normal → renk) */
static void shade_var_rgb(ShadeBatch* b) {
    u32 n = b->count * 4u;
    for (u32 i = 0; i < n; i++) {
        b->out_rgba[i]          = b->varyings[i] * 0.5f + 0.5f;
        b->out_rgba[n + i]      = b->varyings[n + i] * 0.5f + 0.5f;
        b->out_rgba[2u * n + i] = b->varyings[2u * n + i] * 0.5f + 0.5f;
        b->out_rgba[3u * n + i] = 1.0f;
    }
}

static G3dMaterial mat_basic(ShadeFn fn, BlendMode blend, G3dCull cull, b32 depth) {
    G3dMaterial m;
    __builtin_memset(&m, 0, sizeof(m));
    m.shade_fn = fn;
    m.blend = blend;
    m.cull = cull;
    m.depth_test = depth;
    m.depth_write = depth;
    return m;
}

TEST(gfx3d_guard_band_no_overflow) {
    CHECK(clip_guard_margin_px(640, 360) == 768);
    CHECK(clip_guard_margin_px(1280, 720) == 320);

    /* Guard band'in köşelerine uzanan iki üçgen: her piksel tam bir kez boyanmalı */
    Env e;
    env_init(&e, 640, 360);
    env_frame(&e);
    raster_set_shader(&e.r, shade_count, NULL, NULL, 0, BLEND_ADD);
    f32 g = (f32)clip_guard_margin_px(640, 360);
    RasterVert a = { -g, -g, 0, 1, {0} }, b = { 640 + g, -g, 0, 1, {0} };
    RasterVert c = { 640 + g, 360 + g, 0, 1, {0} }, d = { -g, 360 + g, 0, 1, {0} };
    raster_triangle(&e.r, a, b, c);
    raster_triangle(&e.r, a, c, d);
    raster_flush(&e.r);
    u32 bad = 0;
    for (u32 i = 0; i < 640u * 360u; i++) bad += ((e.fb.px[i] & 0xFFu) != 1u);
    CHECK(bad == 0);
}

TEST(gfx3d_near_clip_varyings) {
    Rng rng;
    rng_seed(&rng, 7);
    ClipGuard g = clip_guard_for_viewport(640, 360);
    u32 polys = 0;
    f32 max_err = 0.0f;
    for (u32 it = 0; it < 10000; it++) {
        ClipVert in[3];
        for (u32 k = 0; k < 3; k++) {
            in[k].w = 0.05f + rng_f32(&rng) * 2.0f * (rng_f32(&rng) < 0.5f ? -1.0f : 1.0f);
            in[k].x = (rng_f32(&rng) * 2.0f - 1.0f) * 3.0f;
            in[k].y = (rng_f32(&rng) * 2.0f - 1.0f) * 3.0f;
            in[k].z = 0.05f;   /* reverse-Z: z_clip = near sabit */
            /* Klip uzayında doğrusal fonksiyonlar: kırpılmış köşelerde aynen sağlanmalı */
            in[k].var[0] = 2.0f * in[k].x - in[k].w + 0.5f;
            in[k].var[1] = in[k].y + 3.0f * in[k].w;
        }
        u32 must = 0;
        for (u32 k = 0; k < 3; k++) must |= clip_outcode(&in[k], g) & CLIP_NEAR;
        ClipVert out[CLIP_MAX_VERTS];
        u32 n = clip_triangle(in, must, g, 2, out);
        CHECK(n == 0 || (n >= 3 && n <= CLIP_MAX_VERTS));
        if (n) polys++;
        for (u32 k = 0; k < n; k++) {
            CHECK(out[k].z <= out[k].w + 1e-5f);
            f32 e0 = m_abs(out[k].var[0] - (2.0f * out[k].x - out[k].w + 0.5f));
            f32 e1 = m_abs(out[k].var[1] - (out[k].y + 3.0f * out[k].w));
            max_err = m_max(max_err, m_max(e0, e1));
        }
    }
    CHECK(polys > 1000);
    CHECK(max_err < 1e-5f);
}

/* Kameranın altından ufka uzanan, yakın düzlemi ve guard band'i kesen ızgara zemini:
 * hiçbir piksel iki kez boyanmamalı, her sütun ufkun altında boşluksuz olmalı. */
TEST(gfx3d_clipped_grid_no_cracks) {
    Env e;
    env_init(&e, 640, 360);
    Arena* a = &e.perm;
    Mesh* plane = mesh_plane(a, 400.0f, 40, 1.0f);
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.3f, 1.5f, 0.2f);
    cam.yaw = 0.37f;
    cam.pitch = -0.25f;

    for (u32 pass = 0; pass < 2; pass++) {
        env_frame(&e);
        G3dContext ctx;
        g3d_begin(&ctx, &e.r, &e.frame, camera_view(&cam), camera_proj(&cam, 640.0f / 360.0f), camera_eye(&cam));
        G3dMaterial m = mat_basic(shade_count, BLEND_ADD, pass ? G3D_CULL_BACK : G3D_CULL_NONE, FALSE);
        Mat4 model = mat4_identity();
        CHECK(g3d_draw(&ctx, plane, &model, &m));
        raster_flush(&e.r);
        CHECK(ctx.stats.tris_clipped > 0);

        u32 doubles = 0, holes = 0, painted = 0;
        for (u32 x = 0; x < 640; x++) {
            b32 seen = FALSE;
            for (u32 y = 0; y < 360; y++) {
                u32 v = e.fb.px[y * 640 + x] & 0xFFu;
                if (v > 1) doubles++;
                if (v) { seen = TRUE; painted++; }
                else if (seen) holes++;   /* boyalı bölgenin altında boşluk */
            }
        }
        CHECK(doubles == 0);
        CHECK(holes == 0);
        CHECK(painted > 640u * 100u);
    }
}

/* Perspektif-doğru UV: piksel merkezinden zemine ışın at, analitik UV ile karşılaştır */
typedef struct { Vec3 eye; Mat4 inv_vp; f32 size, uv_scale; f32 max_err; u32 checked; } UvCheck;
static UvCheck g_uv;

static void shade_check_uv(ShadeBatch* b) {
    u32 n = b->count * 4u;
    for (u32 q = 0; q < b->count; q++) {
        for (u32 l = 0; l < 4; l++) {
            if (!(b->quads[q].mask & (1u << l))) continue;
            f32 px = (f32)b->quads[q].x + (f32)(l & 1u) + 0.5f;
            f32 py = (f32)b->quads[q].y + (f32)(l >> 1) + 0.5f;
            f32 nx = px / 640.0f * 2.0f - 1.0f;
            f32 ny = 1.0f - py / 360.0f * 2.0f;
            Vec4 far = mat4_mul_vec4(g_uv.inv_vp, vec4(nx, ny, 0.5f, 1.0f));
            Vec3 p = vec3(far.x / far.w, far.y / far.w, far.z / far.w);
            Vec3 d = vec3_sub(p, g_uv.eye);
            f32 t = -g_uv.eye.y / d.y;
            Vec3 hit = vec3_add(g_uv.eye, vec3_scale(d, t));
            f32 u = (hit.x + 0.5f * g_uv.size) / g_uv.size * g_uv.uv_scale;
            f32 v = (hit.z + 0.5f * g_uv.size) / g_uv.size * g_uv.uv_scale;
            f32 eu = m_abs(b->varyings[q * 4 + l] - u);
            f32 ev = m_abs(b->varyings[n + q * 4 + l] - v);
            g_uv.max_err = m_max(g_uv.max_err, m_max(eu, ev));
            g_uv.checked++;
        }
    }
    shade_count(b);
}

TEST(gfx3d_perspective_correct_uv) {
    Env e;
    env_init(&e, 640, 360);
    env_frame(&e);
    Mesh* plane = mesh_plane(&e.perm, 8.0f, 1, 1.0f);   /* 2 üçgen: afin olsaydı hata büyük olurdu */
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.0f, 1.0f, 4.5f);
    cam.pitch = -0.35f;
    Mat4 view = camera_view(&cam), proj = camera_proj(&cam, 640.0f / 360.0f);
    G3dContext ctx;
    g3d_begin(&ctx, &e.r, &e.frame, view, proj, camera_eye(&cam));
    g_uv.eye = camera_eye(&cam);
    g_uv.inv_vp = mat4_inverse(ctx.view_proj);
    g_uv.size = 8.0f;
    g_uv.uv_scale = 1.0f;
    g_uv.max_err = 0.0f;
    g_uv.checked = 0;
    G3dMaterial m = mat_basic(shade_check_uv, BLEND_ADD, G3D_CULL_BACK, FALSE);
    m.attrs[0] = G3D_ATTR_UV;
    m.attr_count = 1;
    Mat4 model = mat4_identity();
    g3d_draw(&ctx, plane, &model, &m);
    raster_flush(&e.r);
    CHECK(g_uv.checked > 10000);
    CHECK(g_uv.max_err < 1e-4f);
}

static u32 render_cube(Env* e, const Mesh* cube, G3dCull cull, b32 depth, G3dStats* st) {
    env_frame(e);
    Camera cam;
    camera_init(&cam, CAM_ORBIT);
    cam.yaw = 0.6f;
    cam.pitch = 0.45f;
    cam.dist = 4.0f;
    cam.target = vec3(0, 0, 0);
    G3dContext ctx;
    g3d_begin(&ctx, &e->r, &e->frame, camera_view(&cam), camera_proj(&cam, 640.0f / 360.0f), camera_eye(&cam));
    G3dMaterial m = mat_basic(shade_var_rgb, BLEND_NONE, cull, depth);
    m.attrs[0] = G3D_ATTR_NORMAL;
    m.attr_count = 1;
    Mat4 model = mat4_rotate_y(0.3f);
    g3d_draw(&ctx, cube, &model, &m);
    raster_flush(&e->r);
    *st = ctx.stats;
    u32 painted = 0;
    for (u32 i = 0; i < 640u * 360u; i++) painted += (e->fb.px[i] != 0);
    return painted;
}

TEST(gfx3d_backface_and_depth) {
    Env e;
    env_init(&e, 640, 360);
    G3dStats sb, sf, sn;
    Mesh* cube = mesh_cube(&e.perm, 1.0f);
    render_cube(&e, cube, G3D_CULL_BACK, TRUE, &sb);
    u32* img_back = ARENA_PUSH_ARRAY(&e.perm, u32, 640 * 360);
    __builtin_memcpy(img_back, e.fb.px, 640 * 360 * 4);
    render_cube(&e, cube, G3D_CULL_FRONT, TRUE, &sf);
    CHECK(sb.tris_out == 6);          /* genel bakışta 3 yüz görünür */
    CHECK(sb.tris_out + sf.tris_out == 12);
    /* Ayıklamasız + derinlik testli çizim, arka yüz ayıklamalı çizimle aynı görüntüyü vermeli */
    u32 painted = render_cube(&e, cube, G3D_CULL_NONE, TRUE, &sn);
    CHECK(sn.tris_out == 12);
    u32 diff = 0;
    for (u32 i = 0; i < 640u * 360u; i++) diff += (img_back[i] != e.fb.px[i]);
    CHECK(painted > 5000);
    CHECK(diff == 0);
}

TEST(gfx3d_frustum_and_depth_range) {
    Env e;
    env_init(&e, 640, 360);
    env_frame(&e);
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0, 0, 0);
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    G3dContext ctx;
    g3d_begin(&ctx, &e.r, &e.frame, camera_view(&cam), camera_proj(&cam, 16.0f / 9.0f), cam.pos);
    CHECK(g3d_sphere_visible(&ctx, vec3(0, 0, -10), 1.0f));
    CHECK(!g3d_sphere_visible(&ctx, vec3(0, 0, 10), 1.0f));      /* arkada */
    CHECK(!g3d_sphere_visible(&ctx, vec3(-100, 0, -10), 1.0f));  /* solda dışarıda */
    CHECK(g3d_sphere_visible(&ctx, vec3(0, 0, 2), 2.5f));        /* kamerayı içeriyor */
    CHECK(g3d_sphere_visible(&ctx, vec3(0, 0, -1e6f), 1.0f));    /* sonsuz uzak düzlem */

    /* reverse-Z: near'da z/w = 1, 2*near'da 0.5 */
    Vec4 p0 = mat4_mul_vec4(ctx.view_proj, vec4(0, 0, -cam.near_z, 1));
    Vec4 p1 = mat4_mul_vec4(ctx.view_proj, vec4(0, 0, -2.0f * cam.near_z, 1));
    CHECK_NEAR(p0.z / p0.w, 1.0f, 1e-6);
    CHECK_NEAR(p1.z / p1.w, 0.5f, 1e-6);
}

/* Satranç deseninde lane discard eden kırmızı shader (alfa testi taklidi) */
static void shade_discard_checker(ShadeBatch* b) {
    shade_count(b);
    for (u32 q = 0; q < b->count; q++) {
        u32 keep = 0;
        for (u32 l = 0; l < 4; l++) {
            u32 px = b->quads[q].x + (l & 1u), py = b->quads[q].y + (l >> 1);
            if (((px >> 2) + (py >> 2)) & 1u) keep |= 1u << l;
        }
        b->quads[q].mask &= keep;
    }
}

static void quad_at(Rasterizer* r, f32 z) {
    RasterVert a = { 100, 100, z, 1, {0} }, b = { 300, 100, z, 1, {0} };
    RasterVert c = { 300, 250, z, 1, {0} }, d = { 100, 250, z, 1, {0} };
    raster_triangle(r, a, b, c);
    raster_triangle(r, a, c, d);
}

/* Önde, delikli (discard) yüzey önce çizilir; arkadaki yüzey deliklerden görünmeli.
 * Early-Z yazımında delikler de derinlik yazar → arka yüzey kaybolur; geç yazımda görünür. */
TEST(gfx3d_alpha_test_late_depth) {
    Env e;
    env_init(&e, 640, 360);
    for (u32 late = 0; late < 2; late++) {
        env_frame(&e);
        raster_set_depth_ex(&e.r, TRUE, TRUE, DEPTH_GREATER, late);
        raster_set_shader(&e.r, shade_discard_checker, NULL, NULL, 0, BLEND_ADD);
        quad_at(&e.r, 0.9f);
        raster_set_depth_ex(&e.r, TRUE, TRUE, DEPTH_GREATER, FALSE);
        raster_set_shader(&e.r, shade_count, NULL, NULL, 0, BLEND_ADD);
        quad_at(&e.r, 0.1f);
        raster_flush(&e.r);
        u32 painted = 0, doubles = 0;
        for (u32 y = 100; y < 250; y++) {
            for (u32 x = 100; x < 300; x++) {
                u32 v = e.fb.px[y * 640 + x] & 0xFFu;
                painted += (v != 0);
                doubles += (v > 1);
            }
        }
        CHECK(doubles == 0);
        if (late) CHECK(painted == 200u * 150u);   /* delikler arka yüzeyle dolu */
        else      CHECK(painted < 200u * 150u * 6u / 10u);
    }
}

/* SoA konum dönüşümü (host: skaler g3d_transform_positions) mat4_mul_vec4 ile bit-bit aynı.
 * wasm SIMD sürümünün skalerle aynılığı tarayıcıda tam sahne karşılaştırmasıyla doğrulandı (M8_C). */
TEST(gfx3d_transform_positions_exact) {
    Rng rng;
    rng_seed(&rng, 5);
    Mat4 m = mat4_mul(mat4_perspective_rz(1.1f, 1.7f, 0.05f),
                      mat4_trs(vec3(1.0f, -2.0f, -7.0f), 0.7f, -0.3f, vec3(1.5f, 0.8f, 2.0f)));
    f32 px[37], py[37], pz[37], ox[37], oy[37], oz[37], ow[37];
    for (u32 i = 0; i < 37; i++) {
        px[i] = rng_f32(&rng) * 20.0f - 10.0f;
        py[i] = rng_f32(&rng) * 20.0f - 10.0f;
        pz[i] = rng_f32(&rng) * 20.0f - 10.0f;
    }
    g3d_transform_positions(&m, px, py, pz, 37, ox, oy, oz, ow);
    u32 bad = 0;
    for (u32 i = 0; i < 37; i++) {
        Vec4 q = mat4_mul_vec4(m, vec4(px[i], py[i], pz[i], 1.0f));
        bad += (q.x != ox[i]) + (q.y != oy[i]) + (q.z != oz[i]) + (q.w != ow[i]);
    }
    CHECK(bad == 0);
}

/* Gölge haritası: güneş neredeyse tepede, (0,2,0)'da 2 birimlik küp. Küpün altındaki zemin noktası
 * gölgede, uzaktaki nokta aydınlık; kodlama gidiş-dönüşü 24 bit hassasiyetinde. */
static f32 shadow_lookup(const ShadowMap* s, Vec3 p, f32* recv_z) {
    Vec4 t = mat4_mul_vec4(s->world_to_tex, vec4(p.x, p.y, p.z, 1.0f));
    *recv_z = t.z;
    i32 x = (i32)(t.x * (f32)s->size), y = (i32)(t.y * (f32)s->size);
    if (x < 0 || y < 0 || x >= (i32)s->size || y >= (i32)s->size) return 0.0f;
    return shadow_decode_texel(s->tex.px[(u32)y * s->size + (u32)x]);
}

TEST(gfx3d_shadow_map) {
    Env e;
    env_init(&e, 64, 64);
    for (u32 i = 0; i <= 1000; i++) {
        f32 d = (f32)i / 1000.0f;
        CHECK_NEAR(shadow_decode_texel(shadow_encode_depth(d)), d, 2.0 / 16777215.0);
    }
    ShadowMap sm;
    shadow_init(&sm, &e.perm, 256);
    Mesh* cube = mesh_cube(&e.perm, 1.0f);
    Vec3 sun = vec3_norm(vec3(0.2f, 1.0f, 0.1f));
    shadow_fit(&sm, sun, vec3(0.0f, 0.0f, 0.0f), 10.0f, 10.0f);
    arena_reset(&e.frame);
    G3dContext ctx;
    shadow_begin(&sm, &ctx, &e.r, &e.frame);
    G3dMaterial m = shadow_caster_material();
    Mat4 model = mat4_translate(vec3(0.0f, 2.0f, 0.0f));
    CHECK(g3d_draw(&ctx, cube, &model, &m));
    shadow_end(&sm, &e.r);
    CHECK(ctx.stats.tris_out > 0);

    f32 zr, zs;
    /* Küpün tam altı: kayıtlı derinlik (küpün ışığa uzak alt yüzü, ön yüzler ayıklandığı için) alıcıdan büyük */
    zs = shadow_lookup(&sm, vec3(0.0f, 0.0f, 0.0f), &zr);
    CHECK(zs > zr + 0.01f);
    /* Uzak nokta: hiçbir şey yok → 0, alıcı aydınlık */
    zs = shadow_lookup(&sm, vec3(7.0f, 0.0f, 7.0f), &zr);
    CHECK(zs == 0.0f && zr > 0.0f);
    /* Küpün üst yüzü gölgede değil: kendi derinliği kayıtlıdan büyük/eşit (sapmasız da) */
    zs = shadow_lookup(&sm, vec3(0.1f, 3.0f, 0.1f), &zr);
    CHECK(zr >= zs - 1e-4f);
}
