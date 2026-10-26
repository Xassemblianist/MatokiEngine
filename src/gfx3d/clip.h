/* gfx3d/clip.h — Homojen klip uzayında üçgen sınıflandırma ve Sutherland–Hodgman kırpma.
 * Yakın düzlem (reverse-Z: z <= w) her zaman gerçek kırpılır; x/y için guard band kullanılır:
 * yalnızca 24.8 sabit nokta rasterizer'ın güvenli aralığını aşan üçgenler kırpılır.
 * Kenar kesişimi her zaman İÇERİDEKİ köşeden hesaplanır → ortak kenarlarda çatlak oluşmaz.
 */
#ifndef GFX3D_CLIP_H
#define GFX3D_CLIP_H

#include "core/types.h"
#include "raster/shade_abi.h"

#define CLIP_MAX_VERTS 9   /* 3 + kırpan düzlem sayısı (5) + 1 */

typedef struct {
    f32 x, y, z, w;
    f32 var[SHADE_MAX_VARYINGS];
} ClipVert;

/* Outcode bitleri */
#define CLIP_NEAR      0x01u   /* z > w (kameraya near'dan yakın veya arkasında) */
#define CLIP_LEFT      0x02u   /* x < -w */
#define CLIP_RIGHT     0x04u   /* x >  w */
#define CLIP_BOTTOM    0x08u   /* y < -w */
#define CLIP_TOP       0x10u   /* y >  w */
#define CLIP_G_LEFT    0x20u   /* x < -gx*w (guard band dışı) */
#define CLIP_G_RIGHT   0x40u
#define CLIP_G_BOTTOM  0x80u
#define CLIP_G_TOP     0x100u
#define CLIP_VIEW_MASK (CLIP_NEAR | CLIP_LEFT | CLIP_RIGHT | CLIP_BOTTOM | CLIP_TOP)
#define CLIP_MUST_MASK (CLIP_NEAR | CLIP_G_LEFT | CLIP_G_RIGHT | CLIP_G_BOTTOM | CLIP_G_TOP)

typedef struct {
    f32 gx, gy;   /* guard band çarpanları (>= 1): |x| <= gx*w, |y| <= gy*w */
} ClipGuard;

/* Viewport boyutundan güvenli guard band'i türetir (raster i32 kenar fonksiyonu taşma analizi,
 * bkz. DECISIONS.md D-080). */
ClipGuard clip_guard_for_viewport(u32 vp_w, u32 vp_h);
/* gx/gy için piksel cinsinden kenar payı (test ve belge için) */
u32       clip_guard_margin_px(u32 vp_w, u32 vp_h);

u32 clip_outcode(const ClipVert* v, ClipGuard g);

/* in[3] üçgenini `planes` (CLIP_MUST_MASK alt kümesi) düzlemlerine karşı kırpar.
 * out[CLIP_MAX_VERTS] konveks çokgen, dönüş: köşe sayısı (0 = tamamen dışarıda).
 * Sarım yönü korunur. nvar: kopyalanacak/interpole edilecek varying sayısı. */
u32 clip_triangle(const ClipVert in[3], u32 planes, ClipGuard g, u32 nvar,
                  ClipVert out[CLIP_MAX_VERTS]);

#endif
