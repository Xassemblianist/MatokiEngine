/* phys/bvh.h — 3D üçgen ağları (Mesh) için statik Sınır Hacim Hiyerarşisi (BVH).
 * Yüzey Alanı Sezgiseli (8 bölmeli SAH) ve ortanca yedek bölmesi ile Arena üzerinde kurulur.
 * Işın izleme (raycast_mesh) ve AABB çakışma sorgularını hızlandırır.
 */
#ifndef PHYS_BVH_H
#define PHYS_BVH_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/mesh.h"
#include "phys/shapes.h"

#define BVH_LEAF_MAX 4u

typedef struct {
    Vec3 v0, v1, v2;
    u32  tri_index;   /* Mesh içindeki orijinal üçgen indeksi (0 .. tri_count-1) */
} BvhTri;

typedef struct {
    Aabb bounds;
    u32  left_first;  /* count == 0 ise sol çocuk indeksi (sağ = left_first + 1);
                         count > 0 ise tris[] dizisindeki ilk üçgen indeksi */
    u32  count;       /* yaprak ise üçgen sayısı (> 0), iç düğüm ise 0 */
} BvhNode;

typedef struct {
    const Mesh* mesh;
    BvhNode*    nodes;
    u32         node_count;
    u32         node_cap;
    BvhTri*     tris;
    u32         tri_count;
} Bvh;

/* Verilen Mesh için statik BVH ağacını Arena üzerinde kurar. */
Bvh* bvh_build(Arena* a, const Mesh* mesh);

/* Verilen AABB ile kesişen yapraklardaki üçgen indekslerini (tris[] içindeki indeksler)
 * out_leaf_tris tamponuna yazar; yazılan üçgen sayısını döner. */
u32  bvh_query_aabb(const Bvh* bvh, Aabb query, u32* out_tri_slots, u32 max_out);

#endif
