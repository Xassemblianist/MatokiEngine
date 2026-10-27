/* gfx3d/pipeline.h — 3D boru hattı: vertex aşaması (C), nesne frustum culling, homojen kırpma,
 * perspektif bölme, viewport ve arka yüz ayıklama; çıktı raster_triangle'a gider.
 * Arka uçtan bağımsız tasarım (DECISIONS Ö-5): mesh + model matrisi + materyal alır.
 * Sözleşme: RasterVert.var HAM varying'dir (w ile çarpılmaz), inv_w = 1/w_clip; raster
 * perspektif-doğru interpolasyonu kendisi yapar. Derinlik reverse-Z: DEPTH_GREATER, temizleme 0.
 */
#ifndef GFX3D_PIPELINE_H
#define GFX3D_PIPELINE_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/raster.h"
#include "gfx3d/mesh.h"
#include "gfx3d/clip.h"

/* Materyalin shader'ına sırayla verilen varying'ler (Matoki `in` bildirim sırasıyla aynı olmalı) */
typedef enum {
    G3D_ATTR_UV         = 0,   /* vec2 */
    G3D_ATTR_NORMAL     = 1,   /* vec3, dünya uzayı (normal matrisiyle) */
    G3D_ATTR_VIEW_DEPTH = 2,   /* f32, kameraya görüş ekseni boyunca uzaklık (= w_clip) */
    G3D_ATTR_COLOR      = 3,   /* vec4, köşe rengi (yoksa 1,1,1,1) */
    G3D_ATTR_WORLD_POS  = 4,   /* vec3 */
    G3D_ATTR_SHADOW_POS = 5    /* vec3, gölge haritası doku uzayı (u, v, derinlik); bkz. g3d_set_shadow */
} G3dAttr;

#define G3D_MAX_ATTRS 6

typedef enum {
    G3D_CULL_NONE  = 0,
    G3D_CULL_BACK  = 1,
    G3D_CULL_FRONT = 2
} G3dCull;

typedef struct {
    ShadeFn        shade_fn;
    b32            fused;
    BlendMode      blend;
    const void*    uniforms;     /* flush'a kadar geçerli kalmalı (frame arena önerilir) */
    const Texture* textures;
    u8             attrs[G3D_MAX_ATTRS];
    u32            attr_count;
    G3dCull        cull;
    b32            depth_test;
    b32            depth_write;
    b32            alpha_test;   /* shader discard yapabilir → geç derinlik yazımı (raster_set_depth_ex) */
} G3dMaterial;

typedef struct {
    u64 draws;           /* g3d_draw çağrısı */
    u64 draws_culled;    /* frustum dışında kalan nesne */
    u64 draws_inside;    /* tamamen içeride (köşe outcode'u atlandı) */
    u64 tris_in;         /* görünür nesnelerin üçgenleri */
    u64 tris_rejected;   /* tamamen görüş dışı (outcode) */
    u64 tris_clipped;    /* kırpıcıdan geçen */
    u64 tris_backface;   /* arka yüz / sıfır alan */
    u64 tris_out;        /* rasterizer'a gönderilen */
} G3dStats;

typedef struct {
    Rasterizer* r;
    Arena*      frame;
    Mat4        view, proj, view_proj;
    Vec3        eye;
    u32         vp_w, vp_h;
    ClipGuard   guard;
    Vec4        planes[5];   /* dünya uzayı: sol, sağ, alt, üst, yakın (n·p + d >= 0 içeride) */
    Mat4        shadow_mat;  /* dünya → gölge doku uzayı (G3D_ATTR_SHADOW_POS için), varsayılan birim */
    G3dStats    stats;
} G3dContext;

/* Ölçüm için vertex aşaması optimizasyon anahtarları (varsayılan: hepsi açık) */
#define G3D_OPT_SIMD_XFORM   1u   /* SoA SIMD konum dönüşümü */
#define G3D_OPT_INSIDE_SKIP  2u   /* tamamen içerideki nesnede outcode atla */
#define G3D_OPT_ALL          3u
void g3d_set_opt(u32 mask);

u32  g3d_attr_components(G3dAttr a);
void g3d_begin(G3dContext* c, Rasterizer* r, Arena* frame, Mat4 view, Mat4 proj, Vec3 eye);
void g3d_set_shadow(G3dContext* c, Mat4 world_to_shadow);
b32  g3d_sphere_visible(const G3dContext* c, Vec3 center, f32 radius);
/* Mesh'i çizer; nesne frustum dışındaysa FALSE döner. */
b32  g3d_draw(G3dContext* c, const Mesh* m, const Mat4* model, const G3dMaterial* mat);

/* Model matrisi yardımcıları */
Mat4 mat4_translate(Vec3 t);
Mat4 mat4_scale(Vec3 s);
Mat4 mat4_rotate_y(f32 rad);
Mat4 mat4_rotate_x(f32 rad);
Mat4 mat4_trs(Vec3 t, f32 rot_y, f32 rot_x, Vec3 s);

#endif
