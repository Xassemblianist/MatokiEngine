/* gfx3d/mesh.h — 3D üçgen ağları (SoA köşe akışları + u32 indeksler).
 * Konum, normal, UV ve opsiyonel RGBA8 köşe rengi ayrı dizilerde tutulur (vertex aşaması SIMD'e uygun).
 * Prosedürel üreteçler (küp, düzlem, küre), normal hesabı ve sınır (küre + AABB) hesabı içerir.
 * Ön yüzler sağ-el sisteminde dışarıdan bakınca saat yönünün TERSİ (CCW) sarılır.
 */
#ifndef GFX3D_MESH_H
#define GFX3D_MESH_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"

typedef struct {
    u32  vertex_count;
    u32  index_count;      /* 3'ün katı */
    f32* px; f32* py; f32* pz;
    f32* nx; f32* ny; f32* nz;
    f32* u;  f32* v;
    u32* color;            /* NULL olabilir; RGBA8 (FB_RGBA ile aynı paketleme) */
    u32* indices;
    Vec3 aabb_min, aabb_max;
    Vec3 center;           /* sınır küresi (yerel uzay) */
    f32  radius;
} Mesh;

Mesh* mesh_alloc(Arena* a, u32 vertex_count, u32 index_count, b32 with_color);
void  mesh_compute_bounds(Mesh* m);
void  mesh_compute_normals(Mesh* m);   /* alan ağırlıklı köşe normalleri */

/* Merkezde, kenar uzunluğu 2*half olan küp; yüz başına 4 köşe (keskin normaller), UV 0..1 */
Mesh* mesh_cube(Arena* a, f32 half);
/* XZ düzleminde (y=0, normal +Y), size×size, div×div hücre; UV = konum/size * uv_scale */
Mesh* mesh_plane(Arena* a, f32 size, u32 div, f32 uv_scale);
/* UV küresi: seg boylam × rings enlem bölmesi */
Mesh* mesh_sphere(Arena* a, f32 radius, u32 seg, u32 rings);

#endif
