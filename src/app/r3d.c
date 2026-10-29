/* app/r3d.c — Gün ışığı modeli, materyal uniform blokları ve iki geçişli (gölge + ana) kare çizimi.
 * Uniform blokları ve doku dizileri frame arena'dan; raster flush'a kadar geçerli kalırlar.
 */
#include "app/r3d.h"
#include "core/log.h"
#include "core/embed.h"
#include "core/profiler.h"

/* ---------- Gün ışığı ---------- */

static Vec3 vec3_mix(Vec3 a, Vec3 b, f32 t) {
    return vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static f32 smooth01(f32 e0, f32 e1, f32 x) {
    f32 t = m_clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

DayLight r3d_daylight(f32 tod) {
    DayLight d;
    f32 ang = tod * M_TAU;
    Vec3 sun = vec3_norm(vec3(m_cos(ang) * 0.85f, m_sin(ang), 0.38f));
    f32 elev = sun.y;
    f32 day  = smooth01(-0.12f, 0.18f, elev);           /* 0 gece, 1 gündüz */
    f32 warm = 1.0f - smooth01(0.02f, 0.35f, elev);     /* ufka yakın güneş turuncu */
    Vec3 noon_sky = vec3(0.55f, 0.70f, 0.90f), dusk_sky = vec3(0.88f, 0.55f, 0.40f), night_sky = vec3(0.04f, 0.05f, 0.10f);
    Vec3 sky = vec3_mix(noon_sky, dusk_sky, warm * day);
    sky = vec3_mix(night_sky, sky, day);
    d.sky_srgb = sky;
    d.day      = day;
    if (elev > -0.02f) {
        Vec3 c = vec3_mix(vec3(1.0f, 0.95f, 0.88f), vec3(1.0f, 0.50f, 0.22f), warm);
        d.light_dir = vec3_norm(vec3(sun.x, m_max(elev, 0.02f), sun.z));
        d.light_col = vec3_scale(c, 3.2f * smooth01(-0.02f, 0.08f, elev));
    } else {
        /* Ay: güneşin karşısı, soluk mavi */
        d.light_dir = vec3_norm(vec3(-sun.x, m_max(-elev, 0.05f), sun.z));
        d.light_col = vec3_scale(vec3(0.35f, 0.45f, 0.75f), 0.35f * smooth01(0.0f, 0.15f, -elev));
    }
    d.sky_lin = vec3_scale(vec3(sky.x * sky.x, sky.y * sky.y, sky.z * sky.z), 0.45f);
    d.gnd_lin = vec3_scale(vec3(0.20f, 0.17f, 0.13f), 0.15f + 0.85f * day);
    return d;
}

/* ---------- Uniform blokları ---------- */

/* lit3d.mtk (light_dir@0 base@16 fog@32) */
typedef struct {
    _Alignas(16) Vec4 light_dir;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
} Lit3dUniforms;

/* pbr3d.mtk (sun_dir@0 sun_col@16 sky_col@32 gnd_col@48 eye@64 base@80 mat@96 fog@112) */
typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_col;
    _Alignas(16) Vec4 gnd_col;
    _Alignas(16) Vec4 eye;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 mat;
    _Alignas(16) Vec4 fog;
} Pbr3dUniforms;

/* unlit3d.mtk (color@0 fog@16) */
typedef struct {
    _Alignas(16) Vec4 color;
    _Alignas(16) Vec4 fog;
} Unlit3dUniforms;

/* particle3d.mtk (fog@0) */
typedef struct {
    _Alignas(16) Vec4 fog;
} Particle3dUniforms;

/* `prefix` ile başlayan gömülü shader kaynağından " @mipmap" niteliğini çıkarıp ayrı varyant derler */
static void compile_nomip_variant(ManagedShader* out, const char* prefix, const char* name, ShadeFn ref,
                                  Arena* scratch) {
    __builtin_memset(out, 0, sizeof(*out));
    u32 pl = 0;
    while (prefix[pl]) pl++;
    for (u32 i = 0; i < g_embedded_shader_count; i++) {
        const EmbeddedFile* ef = &g_embedded_shaders[i];
        u32 k = 0;
        while (k < pl && ef->name[k] == prefix[k]) k++;
        if (k < pl) continue;
        char* buf = ARENA_PUSH_ARRAY(scratch, char, ef->len + 1u);
        static const char k_attr[] = " @mipmap";
        u32 o = 0;
        for (u32 j = 0; j < ef->len; j++) {
            u32 m = 0;
            while (m < sizeof(k_attr) - 1u && j + m < ef->len && ef->src[j + m] == k_attr[m]) m++;
            if (m == sizeof(k_attr) - 1u) { j += m - 1u; continue; }
            buf[o++] = ef->src[j];
        }
        buf[o] = 0;
        MtkDiag diag;
        if (!shaders_compile_one(out, name, buf, o, ref, scratch, &diag)) {
            LOG("%s derlenemedi:\n%s", name, diag.text);
        }
        return;
    }
}

void r3d_init(R3dRenderer* rr, Arena* perm, Arena* scratch, u32 shadow_size) {
    __builtin_memset(rr, 0, sizeof(*rr));
    rr->shadow_size = shadow_size;
    compile_nomip_variant(&rr->lit_nomip, "lit3d.", "lit3d_nomip", ref_shade_lit3d, scratch);
    compile_nomip_variant(&rr->pbr_nomip, "pbr3d.", "pbr3d_nomip", ref_shade_pbr3d, scratch);
    shadow_init(&rr->shadow, perm, shadow_size);
}

/* ---------- Nesne çizimi ---------- */

static void draw_lit(R3dPass* p, const Mesh* mesh, const Mat4* model, const Texture* tex, Vec4 color,
                     f32 metal, f32 rough, G3dCull cull) {
    G3dMaterial m = p->mat;
    m.cull = cull;
    if (p->pbr) {
        Pbr3dUniforms* u = ARENA_PUSH_STRUCT(p->frame, Pbr3dUniforms);
        u->sun_dir = vec4(p->dl.light_dir.x, p->dl.light_dir.y, p->dl.light_dir.z, 0.0f);
        u->sun_col = vec4(p->dl.light_col.x, p->dl.light_col.y, p->dl.light_col.z, 0.0f);
        u->sky_col = vec4(p->dl.sky_lin.x, p->dl.sky_lin.y, p->dl.sky_lin.z, 0.0f);
        u->gnd_col = vec4(p->dl.gnd_lin.x, p->dl.gnd_lin.y, p->dl.gnd_lin.z, 0.0f);
        u->eye     = vec4(p->eye.x, p->eye.y, p->eye.z, 0.0f);
        u->base    = color;
        u->mat     = vec4(metal, rough, p->shadow_bias, (f32)p->shadow_tex->w);
        Vec3 f = p->dl.sky_lin;
        u->fog     = vec4(f.x, f.y, f.z, p->fog_density);
        Texture* t = ARENA_PUSH_ARRAY(p->frame, Texture, 2);
        t[0] = *tex;
        t[1] = *p->shadow_tex;
        m.uniforms = u;
        m.textures = t;
    } else {
        Lit3dUniforms* u = ARENA_PUSH_STRUCT(p->frame, Lit3dUniforms);
        u->light_dir = vec4(p->dl.light_dir.x, p->dl.light_dir.y, p->dl.light_dir.z, 0.0f);
        u->base      = color;
        u->fog       = vec4(p->dl.sky_srgb.x, p->dl.sky_srgb.y, p->dl.sky_srgb.z, p->fog_density);
        m.uniforms = u;
        m.textures = tex;
    }
    g3d_draw(p->ctx, mesh, model, &m);
}

void r3d_draw(R3dPass* p, const Mesh* mesh, const Mat4* model, const Texture* tex, Vec4 color,
              f32 metal, f32 rough, G3dCull cull) {
    if (p->shadow_pass) {
        G3dMaterial m = p->mat;
        if (cull == G3D_CULL_NONE) m.cull = G3D_CULL_NONE;
        g3d_draw(p->ctx, mesh, model, &m);
        return;
    }
    draw_lit(p, mesh, model, tex, color, metal, rough, cull);
}

void r3d_draw_receiver(R3dPass* p, const Mesh* mesh, const Mat4* model, const Texture* tex, Vec4 color,
                       f32 metal, f32 rough) {
    if (p->shadow_pass) return;
    draw_lit(p, mesh, model, tex, color, metal, rough, G3D_CULL_BACK);
}

void r3d_draw_emissive(R3dPass* p, const Mesh* mesh, const Mat4* model, Vec4 color) {
    if (p->shadow_pass || p->emissive.shade_fn == NULL) return;
    G3dMaterial m = p->emissive;
    Unlit3dUniforms* u = ARENA_PUSH_STRUCT(p->frame, Unlit3dUniforms);
    u->color = color;
    u->fog   = vec4(p->dl.sky_srgb.x, p->dl.sky_srgb.y, p->dl.sky_srgb.z, p->fog_density);
    m.uniforms = u;
    g3d_draw(p->ctx, mesh, model, &m);
}

void r3d_draw_particles(R3dPass* p, const Mesh* mesh) {
    if (p->shadow_pass || p->particles.shade_fn == NULL) return;
    if (!mesh || mesh->vertex_count == 0u || mesh->index_count == 0u) return;
    G3dMaterial m = p->particles;
    Particle3dUniforms* u = ARENA_PUSH_STRUCT(p->frame, Particle3dUniforms);
    u->fog = vec4(p->dl.sky_srgb.x, p->dl.sky_srgb.y, p->dl.sky_srgb.z, p->fog_density);
    m.uniforms = u;
    Mat4 model = mat4_identity();
    g3d_draw(p->ctx, mesh, &model, &m);
}

/* Ertelenmiş + paralel 3D ön uç (D-105); ölçüm için kapatılabilir */
#define R3D_MAX_DRAWS 1024u
static b32 g_r3d_parallel = TRUE;

void r3d_set_parallel(b32 on) {
    g_r3d_parallel = on ? TRUE : FALSE;
}

/* ---------- Kare ---------- */

void r3d_render(R3dRenderer* rr, Rasterizer* r, Framebuffer* fb, Arena* frame, const ShaderManager* sm,
                const R3dFrame* f, R3dSceneFn scene, void* user, G3dStats* out_main, G3dStats* out_shadow) {
    Vec3 eye = camera_eye(f->cam);
    R3dPass pass;
    __builtin_memset(&pass, 0, sizeof(pass));
    pass.frame       = frame;
    pass.dl          = f->dl;
    pass.eye         = eye;
    pass.pbr         = f->pbr;
    pass.shadow_tex  = &rr->shadow.tex;
    pass.fog_density = f->fog_density;

    /* (1) Gölge haritası */
    G3dContext sctx;
    if (f->pbr) {
        PROF_BEGIN("r3d_shadow");
        shadow_fit(&rr->shadow, f->dl.light_dir, f->shadow_center, f->shadow_radius, f->shadow_radius * 1.1f);
        shadow_begin(&rr->shadow, &sctx, r, frame);
        pass.ctx         = &sctx;
        pass.shadow_pass = TRUE;
        pass.mat         = shadow_caster_material();
        if (g_r3d_parallel) g3d_set_deferred(&sctx, R3D_MAX_DRAWS);
        scene(&pass, user);
        g3d_flush(&sctx);
        shadow_end(&rr->shadow, r);
        if (out_shadow) *out_shadow = sctx.stats;
        /* sapma: ~1.5 texel'lik dünya mesafesi, derinlik birimine çevrilmiş (proj z ölçeği) */
        pass.shadow_bias = 1.5f * rr->shadow.texel_world * rr->shadow.proj.m[10];
        PROF_END();
    }

    PROF_BEGIN("r3d_clear");
    Vec3 sk = f->dl.sky_srgb;
    fb_clear(fb, FB_RGBA((u32)(sk.x * 255.0f), (u32)(sk.y * 255.0f), (u32)(sk.z * 255.0f), 255));
    fb_clear_depth(fb, 0.0f);
    PROF_END();

    const ManagedShader* sh;
    if (f->pbr) sh = (f->mipmap || !rr->pbr_nomip.ok) ? shaders_find(sm, "pbr3d.mtk") : &rr->pbr_nomip;
    else        sh = (f->mipmap || !rr->lit_nomip.ok) ? shaders_find(sm, "lit3d.mtk") : &rr->lit_nomip;
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, f->use_jit, BLEND_NONE, &fused);

    raster_begin(r, fb, frame);
    G3dContext ctx;
    f32 aspect = (f32)fb->w / (f32)fb->h;
    g3d_begin(&ctx, r, frame, camera_view(f->cam), camera_proj(f->cam, aspect), eye);
    if (f->pbr) g3d_set_shadow(&ctx, rr->shadow.world_to_tex);
    if (g_r3d_parallel) g3d_set_deferred(&ctx, R3D_MAX_DRAWS);

    G3dMaterial mat;
    __builtin_memset(&mat, 0, sizeof(mat));
    mat.shade_fn    = fn;
    mat.fused       = fused;
    mat.blend       = BLEND_NONE;
    mat.attrs[0]    = G3D_ATTR_UV;
    mat.attrs[1]    = G3D_ATTR_NORMAL;
    mat.attrs[2]    = G3D_ATTR_VIEW_DEPTH;
    mat.attr_count  = 3;
    if (f->pbr) {
        mat.attrs[3]   = G3D_ATTR_WORLD_POS;
        mat.attrs[4]   = G3D_ATTR_SHADOW_POS;
        mat.attr_count = 5;
    }
    mat.cull        = G3D_CULL_BACK;
    mat.depth_test  = TRUE;
    mat.depth_write = TRUE;

    G3dMaterial em;
    __builtin_memset(&em, 0, sizeof(em));
    const ManagedShader* esh = shaders_find(sm, "unlit3d.mtk");
    if (esh) {
        b32 efused = FALSE;
        em.shade_fn    = shaders_get_fn(esh, f->use_jit, BLEND_NONE, &efused);
        em.fused       = efused;
        em.blend       = BLEND_NONE;
        em.attrs[0]    = G3D_ATTR_NORMAL;
        em.attrs[1]    = G3D_ATTR_VIEW_DEPTH;
        em.attr_count  = 2;
        em.cull        = G3D_CULL_BACK;
        em.depth_test  = TRUE;
        em.depth_write = TRUE;
    }

    G3dMaterial pm;
    __builtin_memset(&pm, 0, sizeof(pm));
    const ManagedShader* psh = shaders_find(sm, "particle3d.mtk");
    if (psh) {
        b32 pfused = FALSE;
        pm.shade_fn    = shaders_get_fn(psh, f->use_jit, BLEND_ALPHA, &pfused);
        pm.fused       = pfused;
        pm.blend       = BLEND_ALPHA;
        pm.attrs[0]    = G3D_ATTR_UV;
        pm.attrs[1]    = G3D_ATTR_COLOR;
        pm.attrs[2]    = G3D_ATTR_VIEW_DEPTH;
        pm.attr_count  = 3;
        pm.cull        = G3D_CULL_BACK;
        pm.depth_test  = TRUE;
        pm.depth_write = FALSE;
    }

    pass.ctx         = &ctx;
    pass.shadow_pass = FALSE;
    pass.mat         = mat;
    pass.emissive    = em;
    pass.particles   = pm;
    scene(&pass, user);
    g3d_flush(&ctx);

    PROF_BEGIN("r3d_flush");
    raster_flush(r);
    PROF_END();
    if (out_main) *out_main = ctx.stats;
}
