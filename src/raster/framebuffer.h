/* raster/framebuffer.h — Yazılım hedef tamponu (RGBA8 renk pikselleri).
 * JS tarafında ImageData (Uint8ClampedArray) olarak doğrudan okunur.
 * Little-endian düzende 0xAABBGGRR olarak paketlenir (byte sırası: R, G, B, A).
 * Opsiyonel f32* depth tamponu ile 3D erken derinlik (early-z) testini destekler.
 */
#ifndef RASTER_FRAMEBUFFER_H
#define RASTER_FRAMEBUFFER_H

#include "core/types.h"
#include "core/arena.h"

typedef struct {
    u32* px;
    f32* depth;
    u32  w;
    u32  h;
} Framebuffer;

#define FB_RGBA(r, g, b, a) \
    ((u32)((u8)(r)) | ((u32)((u8)(g)) << 8) | ((u32)((u8)(b)) << 16) | ((u32)((u8)(a)) << 24))

void fb_init(Framebuffer* fb, Arena* arena, u32 w, u32 h);
void fb_init_depth(Framebuffer* fb, Arena* arena);
void fb_clear(Framebuffer* fb, u32 rgba);
void fb_clear_depth(Framebuffer* fb, f32 z);
void fb_rect(Framebuffer* fb, i32 x, i32 y, i32 w, i32 h, u32 rgba);

#endif
