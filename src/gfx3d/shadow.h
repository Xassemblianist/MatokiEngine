/* gfx3d/shadow.h — Yönlü ışık (güneş) için gölge haritası: ışık uzayında ortografik, reverse-Z,
 * yalnız-derinlik geçişi (raster shade_fn = NULL), ardından derinliğin 24 bitlik RGB'ye kodlanıp
 * shader'ın örnekleyebileceği bir Texture'a yazılması (px = gölge framebuffer'ının renk tamponu).
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
    Framebuffer fb;          /* depth: gölge derinliği; px: kodlanmış doku pikselleri */
    Texture     tex;         /* px = fb.px, CLAMP + en yakın komşu */
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
/* flush + derinliği dokuya kodlar */
void shadow_end(ShadowMap* s, Rasterizer* r);

/* Kodlama (testler ve C referans shader'ı için): d ∈ [0,1] → RGB 24 bit, a = 255 */
u32 shadow_encode_depth(f32 d);
f32 shadow_decode_texel(u32 px);

#endif
