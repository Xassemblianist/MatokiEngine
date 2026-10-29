/* gfx3d/shadow.h — Yönlü ışık (güneş) için gölge haritası: ışık uzayında ortografik, reverse-Z,
 * yalnız-derinlik geçişi (raster shade_fn = NULL). Gölge dokusu (tex.px) kodlama yapılmadan
 * doğrudan fb.depth tamponuna (TEX_FLAG_F32 | TEX_FLAG_CLAMP) işaret eder; Matoki shader'ı
 * sample_depth() ile f32 derinliği doğrudan okur (DECISIONS D-107).
 * Kırpma kutusu kameraya göre bir küreye oturtulur ve texel ızgarasına hizalanır (kamera hareketinde titreme yok).
 */
#ifndef GFX3D_SHADOW_H
#define GFX3D_SHADOW_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/raster.h"
#include "raster/framebuffer.h"
#include "gfx3d/pipeline.h"

typedef struct {
    Framebuffer fb;          /* depth: gölge derinliği (f32) */
    Texture     tex;         /* px = (u32*)fb.depth, TEX_FLAG_CLAMP | TEX_FLAG_F32 */
    u32         size;
    Mat4        view, proj;  /* ışık görüşü ve ortografik izdüşüm */
    Mat4        world_to_tex;/* dünya → (u, v, derinlik); g3d_set_shadow'a verilir */
    f32         texel_world; /* bir texel'in dünya birimi cinsinden boyu (sapma ölçeği için) */
} ShadowMap;

void shadow_init(ShadowMap* s, Arena* perm, u32 size);
/* sun_dir: ışığa doğru birim vektör; center/radius: gölge alacak bölgeyi saran küre;
 * caster_extra: kürenin dışında, ışık tarafında kalan gölge düşürücüler için ek derinlik */
void shadow_fit(ShadowMap* s, Vec3 sun_dir, Vec3 center, f32 radius, f32 caster_extra);
/* raster'ı gölge haritasına bağlar ve g3d bağlamını ışık görüşüyle başlatır */
void shadow_begin(ShadowMap* s, G3dContext* ctx, Rasterizer* r, Arena* frame);
/* Gölge düşüren materyal: yalnız derinlik, ön yüzleri ayıklar (arka yüzler yazılır → daha az "acne") */
G3dMaterial shadow_caster_material(void);
/* raster_flush ile gölge derinlik geçişini tamamlar (kodlama yok, fb.depth doğrudan okunur) */
void shadow_end(ShadowMap* s, Rasterizer* r);

#endif
