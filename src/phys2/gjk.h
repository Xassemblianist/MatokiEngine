/* phys2/gjk.h — destek fonksiyonlu dışbükey çekirdekler, GJK mesafesi ve EPA girişimi.
 * Her şekil "çekirdek + yarıçap": küre = nokta + r, kapsül = doğru parçası + r, kutu / hull /
 * mesh üçgeni = çokyüzlü + 0. GJK çekirdekler arası en yakın noktaları verir; çekirdekler kesişiyorsa
 * EPA en küçük ayırma normalini ve derinliği verir. Normal kuralı: A'dan B'ye.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_GJK_H
#define PHYS2_GJK_H
#include "phys2/pmath.h"
#include "phys2/shapes.h"

enum { PHYS2_CORE_POINT, PHYS2_CORE_SEGMENT, PHYS2_CORE_POLY };

typedef struct Phys2Core {
    u32  kind;
    f32  radius;
    Vec3 p0, p1;               /* nokta / doğru parçası uçları (dünya) */
    const Phys2ConvexHull* hull;    /* çokyüzlü: yerel köşeler ve yüzler */
    Vec3 scale;                /* yerel köşe ölçeği (kutu: yarı boyutlar; hull: 1) */
    Vec3 pos;
    Mat3 rot;
    /* üçgen için yerinde depolanan çokyüzlü */
    Vec3       tri_v[3];
    Phys2HullFace   tri_f[2];
    u32        tri_i[6];
    Phys2ConvexHull tri_h;
} Phys2Core;

void phys2_core_triangle(Phys2Core* c, Vec3 a, Vec3 b, Vec3 cc);
Vec3 phys2_core_support(const Phys2Core* c, Vec3 dir);
/* Çokyüzlü köşesi / yüz normali dünya uzayında */
static inline Vec3 phys2_core_vertex(const Phys2Core* c, u32 i) {
    Vec3 v = c->hull->verts[i];
    return vec3_add(c->pos, phys2_mat3_mul_v3(&c->rot, vec3(v.x * c->scale.x, v.y * c->scale.y, v.z * c->scale.z)));
}
static inline Vec3 phys2_core_face_normal(const Phys2Core* c, u32 f) {
    /* eksen ölçeklemesi (kutu) yüz normallerini değiştirmez çünkü yüzler eksen hizalı; hull'da ölçek 1 */
    return phys2_mat3_mul_v3(&c->rot, c->hull->faces[f].n);
}

typedef struct Phys2GjkResult {
    b32  overlap;
    f32  dist;
    Vec3 pa, pb;          /* çekirdekler üzerinde en yakın noktalar */
    u32  iters;
    u32  exit;            /* 0 yakınsadı, 1 tekrar eden köşe, 2 tur sınırı, 3 içeride, 4 ilerleme durdu */
    /* EPA için son simplex */
    u32  n;
    Vec3 w[4], a[4], b[4];
} Phys2GjkResult;

void phys2_gjk_distance(const Phys2Core* A, const Phys2Core* B, Phys2GjkResult* r);

typedef struct Phys2EpaResult {
    b32  ok;
    Vec3 normal;          /* A → B */
    f32  depth;           /* çekirdek girişimi */
    Vec3 pa, pb;
} Phys2EpaResult;

void phys2_epa_penetration(const Phys2Core* A, const Phys2Core* B, const Phys2GjkResult* g, Phys2EpaResult* r);

#endif
