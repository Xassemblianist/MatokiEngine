/* phys2/bvh.h — dinamik AABB ağacı (Box2D v2.4 b2DynamicTree'nin 3B karşılığı): geniş faz, raycast, CCD.
 * Yapraklar "şişman" AABB (sıkı kutu ± margin) taşır; gövde şişman kutusundan çıkmadıkça ağaç değişmez.
 * Ekleme: yüzey alanı maliyetiyle kardeş seçimi; yukarı yürürken AVL tarzı döndürmelerle denge.
 * Düğümler sabit kapasiteli havuzda (arena), boş liste ile; işlem sırası aynıysa ağaç aynı → belirleyici.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_BVH_H
#define PHYS2_BVH_H
#include "core/arena.h"
#include "core/math.h"

#define PHYS2_TREE_NULL 0xFFFFFFFFu

typedef struct Phys2TreeNode {
    Vec3 mn, mx;      /* şişman AABB (iç düğüm: çocukların birleşimi) */
    u32  parent;      /* boş listede: sonraki boş düğüm */
    u32  child1, child2;
    i32  height;      /* yaprak 0, boş −1 */
    u32  body;        /* yaprak: gövde indeksi */
} Phys2TreeNode;

typedef struct Phys2DynTree {
    Phys2TreeNode* nodes;
    u32       cap, count;   /* count: kullanılan düğüm */
    u32       root, free_list;
    f32       margin;
    u32       leaves;
} Phys2DynTree;

void phys2_tree_init(Phys2DynTree* t, Arena* a, u32 cap, f32 margin);
void phys2_tree_clear(Phys2DynTree* t);
u32  phys2_tree_insert(Phys2DynTree* t, Vec3 mn, Vec3 mx, u32 body);   /* dönüş: vekil (yaprak düğüm) */
void phys2_tree_remove(Phys2DynTree* t, u32 proxy);
/* sıkı kutu şişman kutunun dışına taştıysa yeniden ekler (dönüş TRUE) */
b32  phys2_tree_move(Phys2DynTree* t, u32 proxy, Vec3 mn, Vec3 mx);
/* şişman kutusu [mn, mx] ile kesişen yaprakların gövdeleri (sırasız); dönüş = bulunan (max'a kesilir) */
u32  phys2_tree_query(const Phys2DynTree* t, Vec3 mn, Vec3 mx, u32* out, u32 max);
/* ışın: her aday yaprakta fn çağrılır; fn tmax'ı küçültebilir (budama) */
typedef void (*Phys2TreeRayFn)(void* ctx, u32 body, f32* tmax);
void phys2_tree_raycast(const Phys2DynTree* t, Vec3 o, Vec3 d, f32 tmax, Phys2TreeRayFn fn, void* ctx);

/* test / istatistik */
i32  phys2_tree_height(const Phys2DynTree* t);
f32  phys2_tree_area_ratio(const Phys2DynTree* t);   /* iç düğüm alanları toplamı / kök alanı */
b32  phys2_tree_validate(const Phys2DynTree* t);     /* ebeveyn bağları, yükseklikler, kapsama, yaprak sayısı */

#endif
