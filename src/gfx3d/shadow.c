/* gfx3d/shadow.c — Gölge haritası kurulumu ve ışık izdüşümü oturtma.
 * Derinlik reverse-Z (1 = ışığa en yakın), temizleme 0 = "hiçbir şey yok" → alıcı her zaman aydınlık.
 * Gölge dokusu (tex.px) doğrudan fb.depth'i (f32*) gösterir; RGB24 kodlama/çözme yapılmaz (D-107).
 */
#include "gfx3d/shadow.h"
#include "gfx3d/camera.h"

void shadow_init(ShadowMap* s, Arena* perm, u32 size) {
    __builtin_memset(s, 0, sizeof(*s));
    s->size = size;
    fb_init(&s->fb, perm, size, size);
    fb_init_depth(&s->fb, perm);
    s->tex.px     = (u32*)s->fb.depth;
    s->tex.w      = size;
    s->tex.h      = size;
    s->tex.w_mask = size - 1u;
    s->tex.h_mask = size - 1u;
    s->tex.log2_w = (u32)__builtin_ctz(size);
    s->tex.flags  = TEX_FLAG_CLAMP | TEX_FLAG_F32;
    s->tex.levels = 1u;
    s->view = s->proj = s->world_to_tex = mat4_identity();
}

void shadow_fit(ShadowMap* s, Vec3 sun_dir, Vec3 center, f32 radius, f32 caster_extra) {
    Vec3 up = (m_abs(sun_dir.y) > 0.99f) ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
    /* Yalnız dönme içeren ışık görüşü; öteleme izdüşüme konur → texel hizalama dünya sabit ızgarada */
    Mat4 rot = mat4_look_at(vec3(0.0f, 0.0f, 0.0f), vec3_scale(sun_dir, -1.0f), up);
    Vec4 c = mat4_mul_vec4(rot, vec4(center.x, center.y, center.z, 1.0f));
    f32 texel = (2.0f * radius) / (f32)s->size;
    f32 cx = m_floor(c.x / texel) * texel;
    f32 cy = m_floor(c.y / texel) * texel;
    f32 n = -c.z - radius - caster_extra;
    f32 f = -c.z + radius;
    s->view = rot;
    s->proj = mat4_ortho_rz(cx - radius, cx + radius, cy - radius, cy + radius, n, f);
    s->texel_world = texel;
    Mat4 bias = mat4_identity();
    bias.m[0] = 0.5f;  bias.m[12] = 0.5f;
    bias.m[5] = -0.5f; bias.m[13] = 0.5f;
    s->world_to_tex = mat4_mul(bias, mat4_mul(s->proj, s->view));
}

void shadow_begin(ShadowMap* s, G3dContext* ctx, Rasterizer* r, Arena* frame) {
    fb_clear_depth(&s->fb, 0.0f);
    raster_begin(r, &s->fb, frame);
    /* eye: ortografik olduğundan yalnız istatistik/uzaklık için; ışık yönünde uzak bir nokta */
    g3d_begin(ctx, r, frame, s->view, s->proj, vec3(0.0f, 0.0f, 0.0f));
}

G3dMaterial shadow_caster_material(void) {
    G3dMaterial m;
    __builtin_memset(&m, 0, sizeof(m));
    m.shade_fn    = NULL;
    m.blend       = BLEND_NONE;
    m.attr_count  = 0;
    m.cull        = G3D_CULL_FRONT;
    m.depth_test  = TRUE;
    m.depth_write = TRUE;
    return m;
}

void shadow_end(ShadowMap* s, Rasterizer* r) {
    (void)s;
    raster_flush(r);
}
