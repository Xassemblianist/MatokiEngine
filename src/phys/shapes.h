/* phys/shapes.h — 3D çarpışma ve fizik için temel geometrik şekil tipleri.
 * Küre (Sphere), eksen hizalı kutu (Aabb), kapsül (Capsule), ışın (Ray),
 * düzlem (Plane), yönlü kutu (Obb) ve genel etiketli PhysShape birliğini tanımlar.
 * Ayrıca nokta-şekil, doğru parçası-doğru parçası ve üçgen en yakın nokta yardımcılarını sunar.
 */
#ifndef PHYS_SHAPES_H
#define PHYS_SHAPES_H

#include "core/types.h"
#include "core/math.h"

typedef struct {
    Vec3 center;
    f32  radius;
} Sphere;

typedef struct {
    Vec3 min;
    Vec3 max;
} Aabb;

typedef struct {
    Vec3 a;
    Vec3 b;
    f32  radius;
} Capsule;

typedef struct {
    Vec3 origin;
    Vec3 dir;      /* birim yön vektörü */
} Ray;

/* Düzlem denklemi: dot(normal, x) = d (normal birim vektör) */
typedef struct {
    Vec3 normal;
    f32  d;
} Plane;

typedef struct {
    Vec3 center;
    Vec3 half_extents;
    Mat3 axes;     /* sütunlar: yerel X, Y, Z birim eksenleri */
} Obb;

typedef enum {
    SHAPE_SPHERE  = 0,
    SHAPE_AABB    = 1,
    SHAPE_CAPSULE = 2,
    SHAPE_PLANE   = 3,
    SHAPE_OBB     = 4
} ShapeKind;

typedef struct {
    ShapeKind kind;
    union {
        Sphere  sphere;
        Aabb    aabb;
        Capsule capsule;
        Plane   plane;
        Obb     obb;
    } as;
} PhysShape;

/* --- Kurucular --- */

static inline Sphere sphere_make(Vec3 center, f32 radius) {
    Sphere s = { center, radius };
    return s;
}

static inline Aabb aabb_make(Vec3 min_pt, Vec3 max_pt) {
    Aabb b = { min_pt, max_pt };
    return b;
}

static inline Aabb aabb_from_center_half(Vec3 center, Vec3 half) {
    Aabb b = { vec3_sub(center, half), vec3_add(center, half) };
    return b;
}

static inline Aabb aabb_empty(void) {
    Aabb b = { vec3(1e30f, 1e30f, 1e30f), vec3(-1e30f, -1e30f, -1e30f) };
    return b;
}

static inline Capsule capsule_make(Vec3 a, Vec3 b, f32 radius) {
    Capsule c = { a, b, radius };
    return c;
}

static inline Ray ray_make(Vec3 origin, Vec3 dir) {
    Ray r = { origin, vec3_norm(dir) };
    return r;
}

static inline Vec3 ray_at(Ray r, f32 t) {
    return vec3_add(r.origin, vec3_scale(r.dir, t));
}

static inline Plane plane_make(Vec3 normal, f32 d) {
    f32 l = vec3_len(normal);
    if (l > 1e-8f) {
        f32 inv = 1.0f / l;
        Plane p = { vec3_scale(normal, inv), d * inv };
        return p;
    }
    Plane p = { vec3(0.0f, 1.0f, 0.0f), d };
    return p;
}

static inline Plane plane_from_point_normal(Vec3 point, Vec3 normal) {
    Vec3 n = vec3_norm(normal);
    if (vec3_dot(n, n) < 0.5f) n = vec3(0.0f, 1.0f, 0.0f);
    Plane p = { n, vec3_dot(n, point) };
    return p;
}

static inline f32 plane_signed_dist(Plane p, Vec3 pt) {
    return vec3_dot(p.normal, pt) - p.d;
}

static inline Obb obb_make(Vec3 center, Vec3 half_extents, Mat3 axes) {
    Obb o = { center, half_extents, axes };
    return o;
}

static inline PhysShape shape_from_sphere(Sphere s) {
    PhysShape sh;
    sh.kind = SHAPE_SPHERE;
    sh.as.sphere = s;
    return sh;
}

static inline PhysShape shape_from_aabb(Aabb b) {
    PhysShape sh;
    sh.kind = SHAPE_AABB;
    sh.as.aabb = b;
    return sh;
}

static inline PhysShape shape_from_capsule(Capsule c) {
    PhysShape sh;
    sh.kind = SHAPE_CAPSULE;
    sh.as.capsule = c;
    return sh;
}

static inline PhysShape shape_from_plane(Plane p) {
    PhysShape sh;
    sh.kind = SHAPE_PLANE;
    sh.as.plane = p;
    return sh;
}

/* --- AABB yardımcıları --- */

static inline Vec3 aabb_center(Aabb b) {
    return vec3_scale(vec3_add(b.min, b.max), 0.5f);
}

static inline Vec3 aabb_half_extents(Aabb b) {
    return vec3_scale(vec3_sub(b.max, b.min), 0.5f);
}

static inline f32 aabb_surface_area(Aabb b) {
    Vec3 d = vec3_sub(b.max, b.min);
    if (d.x < 0.0f || d.y < 0.0f || d.z < 0.0f) return 0.0f;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

static inline Aabb aabb_merge(Aabb a, Aabb b) {
    return aabb_make(
        vec3(m_min(a.min.x, b.min.x), m_min(a.min.y, b.min.y), m_min(a.min.z, b.min.z)),
        vec3(m_max(a.max.x, b.max.x), m_max(a.max.y, b.max.y), m_max(a.max.z, b.max.z))
    );
}

static inline Aabb aabb_expand_point(Aabb b, Vec3 p) {
    return aabb_make(
        vec3(m_min(b.min.x, p.x), m_min(b.min.y, p.y), m_min(b.min.z, p.z)),
        vec3(m_max(b.max.x, p.x), m_max(b.max.y, p.y), m_max(b.max.z, p.z))
    );
}

static inline Aabb aabb_expand_margin(Aabb b, f32 m) {
    Vec3 v = vec3(m, m, m);
    return aabb_make(vec3_sub(b.min, v), vec3_add(b.max, v));
}

static inline b32 aabb_overlaps(Aabb a, Aabb b) {
    return (a.min.x <= b.max.x && a.max.x >= b.min.x) &&
           (a.min.y <= b.max.y && a.max.y >= b.min.y) &&
           (a.min.z <= b.max.z && a.max.z >= b.min.z);
}

Aabb aabb_for_sphere(Sphere s);
Aabb aabb_for_capsule(Capsule c);
Aabb aabb_for_triangle(Vec3 v0, Vec3 v1, Vec3 v2);

/* --- En yakın nokta yardımcıları --- */

Vec3 shape_closest_point_aabb(Aabb b, Vec3 p);
Vec3 shape_closest_point_obb(Obb o, Vec3 p);
Vec3 shape_closest_point_segment(Vec3 a, Vec3 b, Vec3 p, f32* out_t);
void shape_closest_points_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2,
                                   f32* out_s, f32* out_t,
                                   Vec3* out_c1, Vec3* out_c2);
Vec3 shape_closest_point_triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 p);
void shape_closest_points_segment_triangle(Vec3 p, Vec3 q,
                                           Vec3 a, Vec3 b, Vec3 c,
                                           Vec3* out_seg, Vec3* out_tri);

#endif
