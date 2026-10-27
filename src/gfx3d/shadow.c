/* gfx3d/shadow.c — Gölge haritası kurulumu, ışık izdüşümü oturtma ve derinlik kodlama.
 * Derinlik reverse-Z (1 = ışığa en yakın), temizleme 0 = "hiçbir şey yok" → alıcı her zaman aydınlık.
 * Kodlama: q = round(d * (2^24-1)), r = q >> 16, g = (q >> 8) & 255, b = q & 255.
 */
#include "gfx3d/shadow.h"
#include "gfx3d/camera.h"
#include "core/profiler.h"
#include "thread/job.h"

void shadow_init(ShadowMap* s, Arena* perm, u32 size) {
    __builtin_memset(s, 0, sizeof(*s));
    s->size = size;
    fb_init(&s->fb, perm, size, size);
    fb_init_depth(&s->fb, perm);
    s->tex.px     = s->fb.px;
    s->tex.w      = size;
    s->tex.h      = size;
    s->tex.w_mask = size - 1u;
    s->tex.h_mask = size - 1u;
    s->tex.log2_w = (u32)__builtin_ctz(size);
    s->tex.flags  = TEX_FLAG_CLAMP;
    s->tex.levels = 1u;
    s->view = s->proj = s->world_to_tex = mat4_identity();
    fb_clear(&s->fb, 0xFF000000u);
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

u32 shadow_encode_depth(f32 d) {
    if (!(d > 0.0f)) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    u32 q = (u32)(d * 16777215.0f + 0.5f);   /* f32: 1.0 → 2^24 olabilir */
    if (q > 0xFFFFFFu) q = 0xFFFFFFu;
    return ((q >> 16) & 0xFFu) | (((q >> 8) & 0xFFu) << 8) | ((q & 0xFFu) << 16) | 0xFF000000u;
}

f32 shadow_decode_texel(u32 px) {
    u32 q = ((px & 0xFFu) << 16) | (((px >> 8) & 0xFFu) << 8) | ((px >> 16) & 0xFFu);
    return (f32)q * (1.0f / 16777215.0f);
}

typedef struct {
    const f32* d;
    u32*       px;
} EncodeCtx;

#define SHADOW_ENCODE_CHUNK 16384u

static void encode_job(u32 start, u32 end, void* user) {
    const EncodeCtx* e = (const EncodeCtx*)user;
    const f32* d = e->d + (usize)start * SHADOW_ENCODE_CHUNK;
    u32* px = e->px + (usize)start * SHADOW_ENCODE_CHUNK;
    u32 n = (end - start) * SHADOW_ENCODE_CHUNK;
    /* Dalsız gövde: clang -msimd128 bunu f32x4/i32x4'e vektörleştirir (shadow_encode_depth ile aynı sonuç) */
    for (u32 i = 0; i < n; i++) {
        f32 z = d[i];
        z = (z > 0.0f) ? z : 0.0f;
        z = (z < 1.0f) ? z : 1.0f;
        u32 q = (u32)(z * 16777215.0f + 0.5f);
        q = (q > 0xFFFFFFu) ? 0xFFFFFFu : q;
        px[i] = (q >> 16) | (q & 0xFF00u) | ((q & 0xFFu) << 16) | 0xFF000000u;
    }
}

void shadow_end(ShadowMap* s, Rasterizer* r) {
    raster_flush(r);
    PROF_BEGIN("shadow_encode");
    u32 n = s->size * s->size;
    EncodeCtx e = { s->fb.depth, s->fb.px };
    /* size bir 2 kuvveti (>= 128) → n parça boyunun katı; değilse kalan tek parça olarak yapılır */
    u32 chunks = n / SHADOW_ENCODE_CHUNK;
    if (chunks > 0) job_parallel_for(chunks, 1, encode_job, &e);
    u32 done = chunks * SHADOW_ENCODE_CHUNK;
    if (done < n) {
        EncodeCtx tail = { s->fb.depth + done, s->fb.px + done };
        for (u32 i = 0; i < n - done; i++) tail.px[i] = shadow_encode_depth(tail.d[i]);
    }
    PROF_END();
}
