/* app/scene3d.c — 3D demo sahnesinin kurulumu, çizimi ve HUD'u.
 * Tüm kalıcı varlıklar (mesh'ler, dokular) perm arena'da; materyal uniform blokları her kare
 * frame arena'dan ayrılır (raster flush'a kadar geçerli kalmaları gerekir).
 */
#include "app/scene3d.h"
#include "core/math.h"
#include "core/log.h"
#include "raster/texture.h"
#include "core/embed.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"

#define KEY_C 67
#define KEY_M 77
#define GRID_X 40
#define GRID_Z 25

static Mesh*    g_cube;
static Mesh*    g_ground;
static Mesh*    g_sphere;
static Texture  g_tex[3];      /* 0: zemin damalı (mip zincirli), 1: küp, 2: beyaz */
static Texture  g_tex_ground_nomip;   /* aynı pikseller, MIPMAP bayrağı yok (C referans yolu için) */
static ManagedShader g_lit_nomip;     /* lit3d.mtk'nin @mipmap'siz varyantı (JIT yolu için) */
static b32      g_mipmap_on;
static u8       g_prev_m;
static Camera   g_cam;
static G3dStats g_last_stats;
static u8       g_prev_c;

static const f32 k_sky[3] = { 0.55f, 0.70f, 0.90f };

/* lit3d.mtk kaynağından " @mipmap" niteliğini çıkarıp ayrı varyant derler */
static void compile_nomip_variant(Arena* scratch) {
    __builtin_memset(&g_lit_nomip, 0, sizeof(g_lit_nomip));
    for (u32 i = 0; i < g_embedded_shader_count; i++) {
        const EmbeddedFile* ef = &g_embedded_shaders[i];
        const char* n = ef->name;
        if (!(n[0] == 'l' && n[1] == 'i' && n[2] == 't' && n[3] == '3' && n[4] == 'd' && n[5] == '.')) continue;
        char* buf = ARENA_PUSH_ARRAY(scratch, char, ef->len + 1u);
        static const char k_attr[] = " @mipmap";
        u32 o = 0;
        for (u32 k = 0; k < ef->len; k++) {
            u32 m = 0;
            while (m < sizeof(k_attr) - 1u && k + m < ef->len && ef->src[k + m] == k_attr[m]) m++;
            if (m == sizeof(k_attr) - 1u) { k += m - 1u; continue; }
            buf[o++] = ef->src[k];
        }
        buf[o] = 0;
        MtkDiag diag;
        if (!shaders_compile_one(&g_lit_nomip, "lit3d_nomip", buf, o, ref_shade_lit3d, scratch, &diag)) {
            LOG("lit3d_nomip derlenemedi:\n%s", diag.text);
        }
        return;
    }
}

void scene3d_init(Arena* perm, Arena* scratch, Framebuffer* fb) {
    if (fb->depth == NULL) fb_init_depth(fb, perm);

    g_cube   = mesh_cube(perm, 0.5f);
    g_ground = mesh_plane(perm, 240.0f, 24, 60.0f);   /* 4 m'lik hücreler; düzlemi kesen üçgenler */
    g_sphere = mesh_sphere(perm, 1.0f, 32, 16);

    tex_init_mipmapped(&g_tex[0], perm, 64, 64, TEX_FLAG_BILINEAR);
    tex_gen_checker(&g_tex[0], 8, FB_RGBA(235, 235, 228, 255), FB_RGBA(70, 82, 96, 255));
    tex_build_mips(&g_tex[0]);
    g_tex_ground_nomip        = g_tex[0];
    g_tex_ground_nomip.flags &= ~TEX_FLAG_MIPMAP;
    g_tex_ground_nomip.levels = 1u;
    g_mipmap_on = TRUE;
    g_prev_m    = 0;
    compile_nomip_variant(scratch);
    tex_init(&g_tex[1], perm, 32, 32, TEX_FLAG_BILINEAR);
    tex_gen_gradient(&g_tex[1], FB_RGBA(255, 220, 90, 255), FB_RGBA(255, 120, 60, 255),
                     FB_RGBA(90, 200, 255, 255), FB_RGBA(150, 90, 255, 255));
    tex_init(&g_tex[2], perm, 4, 4, 0);
    tex_gen_checker(&g_tex[2], 4, FB_RGBA(255, 255, 255, 255), FB_RGBA(255, 255, 255, 255));

    camera_init(&g_cam, CAM_FLY);
    g_cam.pos   = vec3(0.0f, 3.0f, 14.0f);
    g_cam.pitch = -0.18f;
    g_prev_c    = 0;
}

/* lit3d.mtk uniform bloğu (MtkLayout ile doğrulanır: light_dir@0 base@16 fog@32) */
typedef struct {
    _Alignas(16) Vec4 light_dir;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
} Lit3dUniforms;

static const Lit3dUniforms* make_uniforms(Arena* frame, Vec3 light, Vec4 base) {
    Lit3dUniforms* u = ARENA_PUSH_STRUCT(frame, Lit3dUniforms);
    u->light_dir = vec4(light.x, light.y, light.z, 0.0f);
    u->base      = base;
    u->fog       = vec4(k_sky[0], k_sky[1], k_sky[2], 1.0f / 160.0f);
    return u;
}

static Vec4 hue_color(f32 h) {
    f32 r = m_clamp(m_abs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    f32 g = m_clamp(2.0f - m_abs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    f32 b = m_clamp(2.0f - m_abs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    return vec4(0.35f + 0.65f * r, 0.35f + 0.65f * g, 0.35f + 0.65f * b, 1.0f);
}

void scene3d_frame(Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                   const InputState* in, f32 dt, f32 time_sec, b32 use_jit, b32 look_enabled) {
    u8 c_down = in->keys[KEY_C];
    if (c_down && !g_prev_c) {
        if (g_cam.mode == CAM_FLY) {
            g_cam.mode   = CAM_ORBIT;
            g_cam.target = vec3_add(g_cam.pos, vec3_scale(camera_forward(&g_cam), g_cam.dist));
        } else {
            g_cam.pos  = camera_eye(&g_cam);
            g_cam.mode = CAM_FLY;
        }
    }
    g_prev_c = c_down;
    u8 m_down = in->keys[KEY_M];
    if (m_down && !g_prev_m) {
        g_mipmap_on = !g_mipmap_on;
        LOG("Mipmap: %s", g_mipmap_on ? "acik" : "kapali");
    }
    g_prev_m = m_down;
    camera_update(&g_cam, in, dt, look_enabled);

    fb_clear(fb, FB_RGBA((u32)(k_sky[0] * 255.0f), (u32)(k_sky[1] * 255.0f), (u32)(k_sky[2] * 255.0f), 255));
    fb_clear_depth(fb, 0.0f);

    const ManagedShader* sh = (g_mipmap_on || !g_lit_nomip.ok) ? shaders_find(sm, "lit3d.mtk") : &g_lit_nomip;
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, use_jit, BLEND_NONE, &fused);

    raster_begin(r, fb, frame);
    G3dContext ctx;
    f32 aspect = (f32)fb->w / (f32)fb->h;
    g3d_begin(&ctx, r, frame, camera_view(&g_cam), camera_proj(&g_cam, aspect), camera_eye(&g_cam));

    Vec3 light = vec3_norm(vec3(0.45f, 0.8f, 0.35f));
    G3dMaterial mat;
    __builtin_memset(&mat, 0, sizeof(mat));
    mat.shade_fn    = fn;
    mat.fused       = fused;
    mat.blend       = BLEND_NONE;
    mat.attrs[0]    = G3D_ATTR_UV;
    mat.attrs[1]    = G3D_ATTR_NORMAL;
    mat.attrs[2]    = G3D_ATTR_VIEW_DEPTH;
    mat.attr_count  = 3;
    mat.cull        = G3D_CULL_BACK;
    mat.depth_test  = TRUE;
    mat.depth_write = TRUE;

    /* Önden arkaya kaba sıra early-Z'ye yardım eder: önce yakın nesneler, zemin en son */
    mat.textures = &g_tex[1];
    mat.uniforms = make_uniforms(frame, light, vec4(1, 1, 1, 1));
    Mat4 m = mat4_trs(vec3(0.0f, 1.6f, 0.0f), time_sec * 0.7f, time_sec * 0.45f, vec3(2.2f, 2.2f, 2.2f));
    g3d_draw(&ctx, g_cube, &m, &mat);

    mat.textures = &g_tex[2];
    mat.uniforms = make_uniforms(frame, light, vec4(0.95f, 0.45f, 0.35f, 1.0f));
    m = mat4_trs(vec3(4.0f, 1.2f + 0.4f * m_sin(time_sec * 1.3f), -1.0f), 0.0f, 0.0f, vec3(1.2f, 1.2f, 1.2f));
    g3d_draw(&ctx, g_sphere, &m, &mat);

    /* 1000 küplük ızgara: her küp kendi rengi ve dönüşüyle (nesne başına culling) */
    for (u32 j = 0; j < GRID_Z; j++) {
        for (u32 i = 0; i < GRID_X; i++) {
            f32 x = ((f32)i - (f32)(GRID_X - 1) * 0.5f) * 3.0f;
            f32 z = -10.0f - (f32)j * 3.0f;
            f32 ph = (f32)(i * 7u + j * 13u) * 0.37f;
            f32 h = 0.6f + 0.5f * m_sin(time_sec * 0.9f + ph);
            mat.uniforms = make_uniforms(frame, light, hue_color((f32)((i * 37u + j * 11u) % 64u) / 64.0f));
            m = mat4_trs(vec3(x, h, z), time_sec * 0.5f + ph, 0.0f, vec3(1.0f, 1.0f, 1.0f));
            g3d_draw(&ctx, g_cube, &m, &mat);
        }
    }

    mat.textures = g_mipmap_on ? &g_tex[0] : &g_tex_ground_nomip;
    mat.uniforms = make_uniforms(frame, light, vec4(1, 1, 1, 1));
    m = mat4_identity();
    g3d_draw(&ctx, g_ground, &m, &mat);

    raster_flush(r);
    g_last_stats = ctx.stats;
}

void scene3d_draw_hud(UiContext* ui, const Framebuffer* fb) {
    char line[160];
    const G3dStats* s = &g_last_stats;
    fmt_format(line, sizeof(line),
               "Nesne %llu (culled %llu) | Ucgen %llu -> ciz %llu, kirp %llu, arka %llu, red %llu",
               (unsigned long long)s->draws, (unsigned long long)s->draws_culled,
               (unsigned long long)s->tris_in, (unsigned long long)s->tris_out,
               (unsigned long long)s->tris_clipped, (unsigned long long)s->tris_backface,
               (unsigned long long)s->tris_rejected);
    i32 y = (i32)fb->h - 22;
    ui_draw_rect(ui, 0, y - 2, (i32)fb->w, 24, FB_RGBA(15, 18, 26, 190));
    ui_draw_text(ui, 4, y, line, FB_RGBA(225, 235, 248, 255), 1);
    fmt_format(line, sizeof(line), "[C] %s [M] mip %s | WASD/QE + surukle, Shift hizli | (%.1f, %.1f, %.1f)",
               g_cam.mode == CAM_FLY ? "Ucus" : "Orbit", g_mipmap_on ? "acik" : "kapali",
               (f64)camera_eye(&g_cam).x, (f64)camera_eye(&g_cam).y, (f64)camera_eye(&g_cam).z);
    ui_draw_text(ui, 4, y + 11, line, FB_RGBA(150, 200, 255, 255), 1);
}
