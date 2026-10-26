/* gfx2d/batch2d.h — 2D sprite yığınlayıcı (batcher) API'si.
 * Her sprite'ı 2 üçgene (4 köşe) dönüştürerek yazılım rasterizer'ına besler.
 * Doku değişiminde otomatik olarak raster_flush() çağırır; tek çizim yolu üçgendir.
 */
#ifndef GFX2D_BATCH2D_H
#define GFX2D_BATCH2D_H

#include "core/types.h"
#include "core/math.h"
#include "raster/raster.h"

typedef struct {
    f32 x, y, w, h;
} Rect;

static inline Rect rect(f32 x, f32 y, f32 w, f32 h) {
    Rect r = { x, y, w, h };
    return r;
}

typedef struct {
    Rasterizer*    raster;
    Mat3           camera;
    b32            cam_is_identity;
    ShadeFn        shade_fn;
    BlendMode      blend;
    b32            fused;
    const Texture* cur_tex;
    Vec4           cur_tint;
    b32            state_dirty;
} Batch2D;

void b2d_begin(Batch2D* b, Rasterizer* r, Mat3 camera);
void b2d_set_blend(Batch2D* b, BlendMode blend);
void b2d_set_shader(Batch2D* b, ShadeFn shade_fn);
void b2d_set_shader_ex(Batch2D* b, ShadeFn shade_fn, b32 fused);
void b2d_sprite(Batch2D* b, const Texture* tex, Rect src_rect,
                Vec2 dst_pos, Vec2 size, f32 rot, Vec4 tint);
void b2d_end(Batch2D* b);

#endif
