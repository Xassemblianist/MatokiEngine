/* phys/collide.h — 3D analitik çakışma ve temas (Contact) hesapları.
 * Küre, AABB, kapsül, düzlem, OBB ve üçgen çiftleri için temas normali,
 * nüfuz derinliği (depth >= 0) ve temas noktası üretir.
 * Konvansiyon: Contact.normal her zaman B'den A'ya doğrudur (A'yı +normal*depth iter).
 */
#ifndef PHYS_COLLIDE_H
#define PHYS_COLLIDE_H

#include "core/types.h"
#include "core/math.h"
#include "phys/shapes.h"

typedef struct {
    b32  hit;
    Vec3 normal;   /* B'den A'ya birim vektör (A'yı B'den dışarı +normal*depth iter) */
    f32  depth;    /* nüfuz derinliği (>= 0; tam değme anında 0) */
    Vec3 point;    /* dünya uzayında temas noktası */
} Contact;

/* --- Temel şekil çiftleri --- */

b32 collide_sphere_sphere(Sphere a, Sphere b, Contact* out);
b32 collide_sphere_aabb(Sphere a, Aabb b, Contact* out);
b32 collide_aabb_sphere(Aabb a, Sphere b, Contact* out);
b32 collide_sphere_capsule(Sphere a, Capsule b, Contact* out);
b32 collide_capsule_sphere(Capsule a, Sphere b, Contact* out);
b32 collide_capsule_capsule(Capsule a, Capsule b, Contact* out);
b32 collide_aabb_aabb(Aabb a, Aabb b, Contact* out);
b32 collide_capsule_aabb(Capsule a, Aabb b, Contact* out);
b32 collide_aabb_capsule(Aabb a, Capsule b, Contact* out);

/* --- Düzlem çiftleri --- */

b32 collide_sphere_plane(Sphere a, Plane b, Contact* out);
b32 collide_plane_sphere(Plane a, Sphere b, Contact* out);
b32 collide_capsule_plane(Capsule a, Plane b, Contact* out);
b32 collide_plane_capsule(Plane a, Capsule b, Contact* out);
b32 collide_aabb_plane(Aabb a, Plane b, Contact* out);
b32 collide_plane_aabb(Plane a, Aabb b, Contact* out);

/* --- OBB ve üçgen çiftleri --- */

b32 collide_sphere_obb(Sphere a, Obb b, Contact* out);
b32 collide_obb_sphere(Obb a, Sphere b, Contact* out);
b32 collide_sphere_triangle(Sphere a, Vec3 v0, Vec3 v1, Vec3 v2, Contact* out);
b32 collide_capsule_triangle(Capsule a, Vec3 v0, Vec3 v1, Vec3 v2, Contact* out);

/* --- Genel etiketli şekil çifti --- */

b32 collide_shapes(const PhysShape* a, const PhysShape* b, Contact* out);

#endif
