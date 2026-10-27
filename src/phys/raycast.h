/* phys/raycast.h — 3D ışın kesişim (raycast) sorguları.
 * Küre, AABB, kapsül, düzlem, üçgen (Möller–Trumbore) ve üçgen ağı (Mesh + BVH)
 * için en yakın isabet parametresi (t >= 0), temas noktası, yüzey normali ve üçgen indeksini döner.
 */
#ifndef PHYS_RAYCAST_H
#define PHYS_RAYCAST_H

#include "core/types.h"
#include "core/math.h"
#include "gfx3d/mesh.h"
#include "phys/shapes.h"
#include "phys/bvh.h"

typedef struct {
    b32  hit;
    f32  t;          /* ışın parametresi (0 <= t <= t_max) */
    Vec3 point;      /* origin + dir * t */
    Vec3 normal;     /* yüzey birim normali */
    u32  tri_index;  /* mesh isabetinde üçgen indeksi, diğer şekillerde U32_MAX */
} RayHit;

b32 raycast_sphere(Ray r, Sphere s, f32 t_max, RayHit* out);
b32 raycast_aabb(Ray r, Aabb b, f32 t_max, RayHit* out);
b32 raycast_capsule(Ray r, Capsule c, f32 t_max, RayHit* out);
b32 raycast_plane(Ray r, Plane p, f32 t_max, RayHit* out);
b32 raycast_triangle(Ray r, Vec3 v0, Vec3 v1, Vec3 v2, f32 t_max, RayHit* out);

/* Üçgen ağı üzerinde ışın izleme: bvh != NULL ise BVH hızlandırmalı, NULL ise kaba kuvvet çalışır. */
b32 raycast_mesh(Ray r, const Mesh* mesh, const Bvh* bvh, f32 t_max, RayHit* out);
b32 raycast_mesh_brute(Ray r, const Mesh* mesh, f32 t_max, RayHit* out);
b32 raycast_shapes(Ray r, const PhysShape* s, f32 t_max, RayHit* out);

#endif
