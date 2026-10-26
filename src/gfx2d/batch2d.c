/* gfx2d/batch2d.c — 2D sprite batcher implementasyonu.
 * Kamera Mat3 dönüşümü, döndürme, ölçekleme ve UV kırpma işlemlerini köşelerde hesaplar.
 * Doku değiştiğinde mevcut karo binlerini flush eder, aynı dokuda üçgenleri biriktirir.
 */
#include "gfx2d/batch2d.h"
#include "raster/ref_shaders.h"
#include "core/log.h"

static b32 mat3_is_id(Mat3 m) {
    return m.m[0] == 1.0f && m.m[1] == 0.0f && m.m[2] == 0.0f &&
           m.m[3] == 0.0f && m.m[4] == 1.0f && m.m[5] == 0.0f &&
           m.m[6] == 0.0f && m.m[7] == 0.0f && m.m[8] == 1.0f;
}

void b2d_begin(Batch2D* b, Rasterizer* r, Mat3 camera) {
    ASSERT(b != NULL && r != NULL);
    b->raster          = r;
    b->camera          = camera;
    b->cam_is_identity = mat3_is_id(camera);
    b->shade_fn        = REF_SHADE_SPRITE_FN;
    b->blend           = BLEND_NONE;
    b->fused           = FALSE;
    b->cur_tex         = NULL;
    b->cur_tint        = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    b->state_dirty     = TRUE;
}

void b2d_set_blend(Batch2D* b, BlendMode blend) {
    if (b->blend != blend) {
        b->blend       = blend;
        b->state_dirty = TRUE;
    }
}

void b2d_set_shader(Batch2D* b, ShadeFn shade_fn) {
    b2d_set_shader_ex(b, shade_fn, FALSE);
}

void b2d_set_shader_ex(Batch2D* b, ShadeFn shade_fn, b32 fused) {
    if (b->shade_fn != shade_fn || b->fused != fused) {
        b->shade_fn    = shade_fn;
        b->fused       = fused;
        b->state_dirty = TRUE;
    }
}

void b2d_sprite(Batch2D* b, const Texture* tex, Rect src_rect,
                Vec2 dst_pos, Vec2 size, f32 rot, Vec4 tint) {
    ASSERT(b != NULL && tex != NULL);

    if (b->cur_tex != tex) {
        if (b->cur_tex != NULL) {
            raster_flush(b->raster);
        }
        b->cur_tex     = tex;
        b->state_dirty = TRUE;
    }

    if (b->cur_tint.x != tint.x || b->cur_tint.y != tint.y ||
        b->cur_tint.z != tint.z || b->cur_tint.w != tint.w) {
        b->cur_tint    = tint;
        b->state_dirty = TRUE;
    }

    if (b->state_dirty) {
        Vec4* u_tint = (Vec4*)arena_push(b->raster->frame, sizeof(Vec4), 16);
        *u_tint = b->cur_tint;
        raster_set_shader_ex(b->raster, b->shade_fn, u_tint, b->cur_tex, 2, b->blend, b->fused);
        b->state_dirty = FALSE;
    }

    f32 hx = size.x * 0.5f;
    f32 hy = size.y * 0.5f;
    f32 c = 1.0f;
    f32 s = 0.0f;
    if (rot != 0.0f) {
        c = m_cos(rot);
        s = m_sin(rot);
    }

    f32 rx_x = hx * c;
    f32 rx_y = hx * s;
    f32 ry_x = -hy * s;
    f32 ry_y = hy * c;

    Vec2 p0 = vec2(dst_pos.x - rx_x - ry_x, dst_pos.y - rx_y - ry_y);
    Vec2 p1 = vec2(dst_pos.x + rx_x - ry_x, dst_pos.y + rx_y - ry_y);
    Vec2 p2 = vec2(dst_pos.x + rx_x + ry_x, dst_pos.y + rx_y + ry_y);
    Vec2 p3 = vec2(dst_pos.x - rx_x + ry_x, dst_pos.y - rx_y + ry_y);

    if (!b->cam_is_identity) {
        p0 = mat3_mul_point(b->camera, p0);
        p1 = mat3_mul_point(b->camera, p1);
        p2 = mat3_mul_point(b->camera, p2);
        p3 = mat3_mul_point(b->camera, p3);
    }

    f32 u0, v0, u1, v1;
    if (src_rect.w > 1.0f || src_rect.h > 1.0f) {
        f32 inv_w = 1.0f / (f32)tex->w;
        f32 inv_h = 1.0f / (f32)tex->h;
        u0 = src_rect.x * inv_w;
        v0 = src_rect.y * inv_h;
        u1 = (src_rect.x + src_rect.w) * inv_w;
        v1 = (src_rect.y + src_rect.h) * inv_h;
    } else {
        u0 = src_rect.x;
        v0 = src_rect.y;
        u1 = src_rect.x + src_rect.w;
        v1 = src_rect.y + src_rect.h;
    }

    RasterVert rv0 = { p0.x, p0.y, 0.0f, 1.0f, { u0, v0 } };
    RasterVert rv1 = { p1.x, p1.y, 0.0f, 1.0f, { u1, v0 } };
    RasterVert rv2 = { p2.x, p2.y, 0.0f, 1.0f, { u1, v1 } };
    RasterVert rv3 = { p3.x, p3.y, 0.0f, 1.0f, { u0, v1 } };

    raster_triangle(b->raster, rv0, rv1, rv2);
    raster_triangle(b->raster, rv0, rv2, rv3);
}

void b2d_end(Batch2D* b) {
    ASSERT(b != NULL);
    raster_flush(b->raster);
    b->cur_tex = NULL;
}
