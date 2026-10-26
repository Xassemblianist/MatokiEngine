/* raster/framebuffer.c — Framebuffer ilklendirme, temizleme ve dikdörtgen çizimi.
 * Tüm çizim işlemleri ekran sınırlarına kırpılır (clipping).
 * Bellek Arena üzerinden 16-byte hizalı olarak ayrılır; wasm'da SIMD/bulk-memory ile temizlenir.
 */
#include "raster/framebuffer.h"
#include "core/log.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

void fb_init(Framebuffer* fb, Arena* arena, u32 w, u32 h) {
    fb->w     = w;
    fb->h     = h;
    fb->px    = (u32*)arena_push(arena, (usize)w * (usize)h * sizeof(u32), 16);
    fb->depth = NULL;
}

void fb_init_depth(Framebuffer* fb, Arena* arena) {
    ASSERT(fb != NULL && arena != NULL);
    fb->depth = (f32*)arena_push(arena, (usize)fb->w * (usize)fb->h * sizeof(f32), 16);
}

void fb_clear(Framebuffer* fb, u32 rgba) {
    u32 count = fb->w * fb->h;
    u32* px = fb->px;

    /* Tekrarlayan byte (ör. 0x00000000 veya 0xFFFFFFFF) için doğrudan memory.fill / memset */
    u8 b0 = (u8)(rgba & 0xFFu);
    if (rgba == ((u32)b0 * 0x01010101u)) {
        __builtin_memset(px, (int)b0, (usize)count * sizeof(u32));
        return;
    }

#if defined(__wasm_simd128__)
    v128_t v = wasm_i32x4_splat((i32)rgba);
    u32 i = 0;
    u32 n16 = count & ~15u;
    for (; i < n16; i += 16) {
        wasm_v128_store(px + i + 0,  v);
        wasm_v128_store(px + i + 4,  v);
        wasm_v128_store(px + i + 8,  v);
        wasm_v128_store(px + i + 12, v);
    }
    u32 n4 = count & ~3u;
    for (; i < n4; i += 4) {
        wasm_v128_store(px + i, v);
    }
    for (; i < count; i++) {
        px[i] = rgba;
    }
#else
    for (u32 i = 0; i < count; i++) {
        px[i] = rgba;
    }
#endif
}

void fb_clear_depth(Framebuffer* fb, f32 z) {
    if (fb == NULL || fb->depth == NULL) return;
    u32 count = fb->w * fb->h;
    f32* d = fb->depth;

    if (z == 0.0f) {
        __builtin_memset(d, 0, (usize)count * sizeof(f32));
        return;
    }

#if defined(__wasm_simd128__)
    v128_t v = wasm_f32x4_splat(z);
    u32 i = 0;
    u32 n16 = count & ~15u;
    for (; i < n16; i += 16) {
        wasm_v128_store(d + i + 0,  v);
        wasm_v128_store(d + i + 4,  v);
        wasm_v128_store(d + i + 8,  v);
        wasm_v128_store(d + i + 12, v);
    }
    u32 n4 = count & ~3u;
    for (; i < n4; i += 4) {
        wasm_v128_store(d + i, v);
    }
    for (; i < count; i++) {
        d[i] = z;
    }
#else
    for (u32 i = 0; i < count; i++) {
        d[i] = z;
    }
#endif
}

void fb_rect(Framebuffer* fb, i32 x, i32 y, i32 w, i32 h, u32 rgba) {
    if (w <= 0 || h <= 0) return;
    i32 x0 = MAX(x, 0);
    i32 y0 = MAX(y, 0);
    i32 x1 = MIN(x + w, (i32)fb->w);
    i32 y1 = MIN(y + h, (i32)fb->h);
    if (x0 >= x1 || y0 >= y1) return;

    u32 stride = fb->w;
    for (i32 row = y0; row < y1; row++) {
        u32* dst = fb->px + (u32)row * stride;
        for (i32 col = x0; col < x1; col++) {
            dst[col] = rgba;
        }
    }
}
