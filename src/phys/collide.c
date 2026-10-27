/* phys/collide.c — 3D analitik çakışma ve temas (Contact) algoritmaları.
 * Tüm çiftlerde Contact.normal B'den A'ya birim vektördür; A ve B yer değiştirdiğinde
 * derinlik birebir aynı, normal ise tam ters (-normal) çıkar.
 * Sıfır uzunluklu vektör ve çakışık merkez durumlarında güvenli yedek eksen seçilir.
 */
#include "phys/collide.h"

static void contact_clear(Contact* out) {
    if (!out) return;
    out->hit = FALSE;
    out->normal = vec3(0.0f, 0.0f, 0.0f);
    out->depth = 0.0f;
    out->point = vec3(0.0f, 0.0f, 0.0f);
}

static i32 cmp_f32(f32 a, f32 b) {
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

static i32 cmp_vec3(Vec3 a, Vec3 b) {
    i32 c = cmp_f32(a.x, b.x);
    if (c != 0) return c;
    c = cmp_f32(a.y, b.y);
    if (c != 0) return c;
    return cmp_f32(a.z, b.z);
}

static i32 cmp_sphere(Sphere a, Sphere b) {
    i32 c = cmp_vec3(a.center, b.center);
    if (c != 0) return c;
    return cmp_f32(a.radius, b.radius);
}

static i32 cmp_aabb(Aabb a, Aabb b) {
    i32 c = cmp_vec3(a.min, b.min);
    if (c != 0) return c;
    return cmp_vec3(a.max, b.max);
}

static i32 cmp_capsule(Capsule a, Capsule b) {
    i32 c = cmp_vec3(a.a, b.a);
    if (c != 0) return c;
    c = cmp_vec3(a.b, b.b);
    if (c != 0) return c;
    return cmp_f32(a.radius, b.radius);
}

/* Verilen eksene dik herhangi bir birim vektör üretir (dejenere kapsül/çakışma durumu için) */
static Vec3 perp_unit(Vec3 axis) {
    Vec3 a = vec3_norm(axis);
    if (vec3_dot(a, a) < 0.5f) return vec3(0.0f, 1.0f, 0.0f);
    Vec3 ref = (m_abs(a.x) < 0.9f) ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f);
    return vec3_norm(vec3_cross(a, ref));
}

static b32 sphere_sphere_canon(Sphere a, Sphere b, Contact* out) {
    Vec3 d = vec3_sub(a.center, b.center);
    f32 dist2 = vec3_dot(d, d);
    f32 r_sum = a.radius + b.radius;
    if (r_sum < 0.0f || dist2 > r_sum * r_sum) {
        contact_clear(out);
        return FALSE;
    }
    f32 dist = m_sqrt(m_max(0.0f, dist2));
    Vec3 n = (dist > 1e-8f) ? vec3_scale(d, 1.0f / dist) : vec3(0.0f, 1.0f, 0.0f);
    f32 depth = m_max(0.0f, r_sum - dist);
    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = depth;
        out->point = vec3_add(b.center, vec3_scale(n, b.radius - 0.5f * depth));
    }
    return TRUE;
}

b32 collide_sphere_sphere(Sphere a, Sphere b, Contact* out) {
    if (cmp_sphere(a, b) > 0) {
        Contact c;
        if (!sphere_sphere_canon(b, a, &c)) {
            contact_clear(out);
            return FALSE;
        }
        if (out) {
            out->hit = TRUE;
            out->normal = vec3_scale(c.normal, -1.0f);
            out->depth = c.depth;
            out->point = c.point;
        }
        return TRUE;
    }
    return sphere_sphere_canon(a, b, out);
}

b32 collide_sphere_aabb(Sphere a, Aabb b, Contact* out) {
    if (a.radius < 0.0f || b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z) {
        contact_clear(out);
        return FALSE;
    }
    Vec3 closest = shape_closest_point_aabb(b, a.center);
    Vec3 d = vec3_sub(a.center, closest);
    f32 dist2 = vec3_dot(d, d);
    if (dist2 > a.radius * a.radius) {
        contact_clear(out);
        return FALSE;
    }

    if (dist2 > 1e-12f) {
        f32 dist = m_sqrt(dist2);
        Vec3 n = vec3_scale(d, 1.0f / dist);
        f32 depth = m_max(0.0f, a.radius - dist);
        if (out) {
            out->hit = TRUE;
            out->normal = n;
            out->depth = depth;
            out->point = closest;
        }
        return TRUE;
    }

    /* Küre merkezi AABB'nin içinde veya tam yüzeyinde: en yakın yüzeyi bul */
    f32 dx0 = a.center.x - b.min.x;
    f32 dx1 = b.max.x - a.center.x;
    f32 dy0 = a.center.y - b.min.y;
    f32 dy1 = b.max.y - a.center.y;
    f32 dz0 = a.center.z - b.min.z;
    f32 dz1 = b.max.z - a.center.z;

    f32 min_d = dx0;
    Vec3 n = vec3(-1.0f, 0.0f, 0.0f);
    Vec3 pt = vec3(b.min.x, a.center.y, a.center.z);

    if (dx1 < min_d) { min_d = dx1; n = vec3( 1.0f, 0.0f, 0.0f); pt = vec3(b.max.x, a.center.y, a.center.z); }
    if (dy0 < min_d) { min_d = dy0; n = vec3(0.0f, -1.0f, 0.0f); pt = vec3(a.center.x, b.min.y, a.center.z); }
    if (dy1 < min_d) { min_d = dy1; n = vec3(0.0f,  1.0f, 0.0f); pt = vec3(a.center.x, b.max.y, a.center.z); }
    if (dz0 < min_d) { min_d = dz0; n = vec3(0.0f, 0.0f, -1.0f); pt = vec3(a.center.x, a.center.y, b.min.z); }
    if (dz1 < min_d) { min_d = dz1; n = vec3(0.0f, 0.0f,  1.0f); pt = vec3(a.center.x, a.center.y, b.max.z); }

    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = m_max(0.0f, a.radius + min_d);
        out->point = pt;
    }
    return TRUE;
}

b32 collide_aabb_sphere(Aabb a, Sphere b, Contact* out) {
    Contact c;
    if (!collide_sphere_aabb(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

b32 collide_sphere_capsule(Sphere a, Capsule b, Contact* out) {
    Vec3 closest = shape_closest_point_segment(b.a, b.b, a.center, NULL);
    Vec3 d = vec3_sub(a.center, closest);
    f32 dist2 = vec3_dot(d, d);
    f32 r_sum = a.radius + b.radius;
    if (r_sum < 0.0f || dist2 > r_sum * r_sum) {
        contact_clear(out);
        return FALSE;
    }
    f32 dist = m_sqrt(m_max(0.0f, dist2));
    Vec3 n = (dist > 1e-8f) ? vec3_scale(d, 1.0f / dist) : perp_unit(vec3_sub(b.b, b.a));
    f32 depth = m_max(0.0f, r_sum - dist);
    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = depth;
        out->point = vec3_add(closest, vec3_scale(n, b.radius - 0.5f * depth));
    }
    return TRUE;
}

b32 collide_capsule_sphere(Capsule a, Sphere b, Contact* out) {
    Contact c;
    if (!collide_sphere_capsule(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

static b32 capsule_capsule_canon(Capsule a, Capsule b, Contact* out) {
    Vec3 ca, cb;
    shape_closest_points_segments(a.a, a.b, b.a, b.b, NULL, NULL, &ca, &cb);
    Vec3 d = vec3_sub(ca, cb);
    f32 dist2 = vec3_dot(d, d);
    f32 r_sum = a.radius + b.radius;
    if (r_sum < 0.0f || dist2 > r_sum * r_sum) {
        contact_clear(out);
        return FALSE;
    }
    f32 dist = m_sqrt(m_max(0.0f, dist2));
    Vec3 n = (dist > 1e-8f) ? vec3_scale(d, 1.0f / dist) : perp_unit(vec3_sub(b.b, b.a));
    f32 depth = m_max(0.0f, r_sum - dist);
    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = depth;
        out->point = vec3_add(cb, vec3_scale(n, b.radius - 0.5f * depth));
    }
    return TRUE;
}

b32 collide_capsule_capsule(Capsule a, Capsule b, Contact* out) {
    if (cmp_capsule(a, b) > 0) {
        Contact c;
        if (!capsule_capsule_canon(b, a, &c)) {
            contact_clear(out);
            return FALSE;
        }
        if (out) {
            out->hit = TRUE;
            out->normal = vec3_scale(c.normal, -1.0f);
            out->depth = c.depth;
            out->point = c.point;
        }
        return TRUE;
    }
    return capsule_capsule_canon(a, b, out);
}

static b32 aabb_aabb_canon(Aabb a, Aabb b, Contact* out) {
    f32 ox = m_min(a.max.x, b.max.x) - m_max(a.min.x, b.min.x);
    f32 oy = m_min(a.max.y, b.max.y) - m_max(a.min.y, b.min.y);
    f32 oz = m_min(a.max.z, b.max.z) - m_max(a.min.z, b.min.z);
    if (ox < 0.0f || oy < 0.0f || oz < 0.0f) {
        contact_clear(out);
        return FALSE;
    }

    Vec3 ca = aabb_center(a);
    Vec3 cb = aabb_center(b);
    Vec3 d  = vec3_sub(ca, cb);

    f32 depth = ox;
    Vec3 n = vec3((d.x >= 0.0f) ? 1.0f : -1.0f, 0.0f, 0.0f);

    if (oy < depth) {
        depth = oy;
        n = vec3(0.0f, (d.y >= 0.0f) ? 1.0f : -1.0f, 0.0f);
    }
    if (oz < depth) {
        depth = oz;
        n = vec3(0.0f, 0.0f, (d.z >= 0.0f) ? 1.0f : -1.0f);
    }

    Vec3 omin = vec3(m_max(a.min.x, b.min.x), m_max(a.min.y, b.min.y), m_max(a.min.z, b.min.z));
    Vec3 omax = vec3(m_min(a.max.x, b.max.x), m_min(a.max.y, b.max.y), m_min(a.max.z, b.max.z));

    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = depth;
        out->point = vec3_scale(vec3_add(omin, omax), 0.5f);
    }
    return TRUE;
}

b32 collide_aabb_aabb(Aabb a, Aabb b, Contact* out) {
    if (cmp_aabb(a, b) > 0) {
        Contact c;
        if (!aabb_aabb_canon(b, a, &c)) {
            contact_clear(out);
            return FALSE;
        }
        if (out) {
            out->hit = TRUE;
            out->normal = vec3_scale(c.normal, -1.0f);
            out->depth = c.depth;
            out->point = c.point;
        }
        return TRUE;
    }
    return aabb_aabb_canon(a, b, out);
}

/* Kapsül doğru parçası üzerinde AABB'ye en yakın noktayı bulur (uçlar + düzlem kesişimleri + alt örnekleme) */
static Vec3 closest_seg_point_to_aabb(Vec3 p0, Vec3 p1, Aabb b) {
    Vec3 ab = vec3_sub(p1, p0);
    f32 candidates[16];
    u32 count = 0;
    candidates[count++] = 0.0f;
    candidates[count++] = 1.0f;
    candidates[count++] = 0.5f;

    if (m_abs(ab.x) > 1e-8f) {
        candidates[count++] = m_clamp((b.min.x - p0.x) / ab.x, 0.0f, 1.0f);
        candidates[count++] = m_clamp((b.max.x - p0.x) / ab.x, 0.0f, 1.0f);
    }
    if (m_abs(ab.y) > 1e-8f) {
        candidates[count++] = m_clamp((b.min.y - p0.y) / ab.y, 0.0f, 1.0f);
        candidates[count++] = m_clamp((b.max.y - p0.y) / ab.y, 0.0f, 1.0f);
    }
    if (m_abs(ab.z) > 1e-8f) {
        candidates[count++] = m_clamp((b.min.z - p0.z) / ab.z, 0.0f, 1.0f);
        candidates[count++] = m_clamp((b.max.z - p0.z) / ab.z, 0.0f, 1.0f);
    }

    /* AABB merkezinin doğru parçası üzerindeki izdüşümü */
    f32 t_center = 0.0f;
    shape_closest_point_segment(p0, p1, aabb_center(b), &t_center);
    candidates[count++] = t_center;

    f32 best_t = 0.0f;
    f32 best_d2 = 1e30f;
    for (u32 i = 0; i < count; i++) {
        f32 t = candidates[i];
        Vec3 pt = vec3_add(p0, vec3_scale(ab, t));
        Vec3 cp = shape_closest_point_aabb(b, pt);
        Vec3 diff = vec3_sub(pt, cp);
        f32 d2 = vec3_dot(diff, diff);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_t = t;
        }
    }

    /* En iyi t etrafında yerel 3-nokta iyileştirmesi */
    f32 step = 0.1f;
    for (u32 iter = 0; iter < 6; iter++) {
        f32 t_lo = m_clamp(best_t - step, 0.0f, 1.0f);
        f32 t_hi = m_clamp(best_t + step, 0.0f, 1.0f);
        f32 ts[2] = { t_lo, t_hi };
        for (u32 k = 0; k < 2; k++) {
            Vec3 pt = vec3_add(p0, vec3_scale(ab, ts[k]));
            Vec3 cp = shape_closest_point_aabb(b, pt);
            Vec3 diff = vec3_sub(pt, cp);
            f32 d2 = vec3_dot(diff, diff);
            if (d2 < best_d2) {
                best_d2 = d2;
                best_t = ts[k];
            }
        }
        step *= 0.5f;
    }

    return vec3_add(p0, vec3_scale(ab, best_t));
}

b32 collide_capsule_aabb(Capsule a, Aabb b, Contact* out) {
    Vec3 seg_pt = closest_seg_point_to_aabb(a.a, a.b, b);
    return collide_sphere_aabb(sphere_make(seg_pt, a.radius), b, out);
}

b32 collide_aabb_capsule(Aabb a, Capsule b, Contact* out) {
    Contact c;
    if (!collide_capsule_aabb(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

static Plane normalize_plane(Plane p) {
    f32 len = vec3_len(p.normal);
    if (len > 1e-8f) {
        f32 inv = 1.0f / len;
        Plane r = { vec3_scale(p.normal, inv), p.d * inv };
        return r;
    }
    Plane r = { vec3(0.0f, 1.0f, 0.0f), p.d };
    return r;
}

b32 collide_sphere_plane(Sphere a, Plane b, Contact* out) {
    if (a.radius < 0.0f) {
        contact_clear(out);
        return FALSE;
    }
    Plane p = normalize_plane(b);
    f32 sd = plane_signed_dist(p, a.center);
    if (sd > a.radius) {
        contact_clear(out);
        return FALSE;
    }
    f32 depth = m_max(0.0f, a.radius - sd);
    if (out) {
        out->hit = TRUE;
        out->normal = p.normal;
        out->depth = depth;
        out->point = vec3_sub(a.center, vec3_scale(p.normal, sd));
    }
    return TRUE;
}

b32 collide_plane_sphere(Plane a, Sphere b, Contact* out) {
    Contact c;
    if (!collide_sphere_plane(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

b32 collide_capsule_plane(Capsule a, Plane b, Contact* out) {
    if (a.radius < 0.0f) {
        contact_clear(out);
        return FALSE;
    }
    Plane p = normalize_plane(b);
    f32 sda = plane_signed_dist(p, a.a);
    f32 sdb = plane_signed_dist(p, a.b);
    f32 sd = m_min(sda, sdb);
    if (sd > a.radius) {
        contact_clear(out);
        return FALSE;
    }
    f32 depth = m_max(0.0f, a.radius - sd);
    Vec3 deep_pt;
    if (m_abs(sda - sdb) < 1e-6f) {
        deep_pt = vec3_scale(vec3_add(a.a, a.b), 0.5f);
    } else {
        deep_pt = (sda < sdb) ? a.a : a.b;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = p.normal;
        out->depth = depth;
        out->point = vec3_sub(deep_pt, vec3_scale(p.normal, sd));
    }
    return TRUE;
}

b32 collide_plane_capsule(Plane a, Capsule b, Contact* out) {
    Contact c;
    if (!collide_capsule_plane(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

b32 collide_aabb_plane(Aabb a, Plane b, Contact* out) {
    if (a.min.x > a.max.x || a.min.y > a.max.y || a.min.z > a.max.z) {
        contact_clear(out);
        return FALSE;
    }
    Plane p = normalize_plane(b);
    Vec3 c = aabb_center(a);
    Vec3 e = aabb_half_extents(a);
    f32 r = e.x * m_abs(p.normal.x) + e.y * m_abs(p.normal.y) + e.z * m_abs(p.normal.z);
    f32 sd = plane_signed_dist(p, c);
    if (sd > r) {
        contact_clear(out);
        return FALSE;
    }
    f32 depth = m_max(0.0f, r - sd);
    Vec3 sup = vec3(
        (p.normal.x >= 0.0f) ? a.min.x : a.max.x,
        (p.normal.y >= 0.0f) ? a.min.y : a.max.y,
        (p.normal.z >= 0.0f) ? a.min.z : a.max.z
    );
    f32 sup_sd = plane_signed_dist(p, sup);
    if (out) {
        out->hit = TRUE;
        out->normal = p.normal;
        out->depth = depth;
        out->point = vec3_sub(sup, vec3_scale(p.normal, sup_sd));
    }
    return TRUE;
}

b32 collide_plane_aabb(Plane a, Aabb b, Contact* out) {
    Contact c;
    if (!collide_aabb_plane(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

b32 collide_sphere_obb(Sphere a, Obb b, Contact* out) {
    if (a.radius < 0.0f) {
        contact_clear(out);
        return FALSE;
    }
    Vec3 closest = shape_closest_point_obb(b, a.center);
    Vec3 d = vec3_sub(a.center, closest);
    f32 dist2 = vec3_dot(d, d);
    if (dist2 > a.radius * a.radius) {
        contact_clear(out);
        return FALSE;
    }
    if (dist2 > 1e-12f) {
        f32 dist = m_sqrt(dist2);
        if (out) {
            out->hit = TRUE;
            out->normal = vec3_scale(d, 1.0f / dist);
            out->depth = m_max(0.0f, a.radius - dist);
            out->point = closest;
        }
        return TRUE;
    }
    /* Küre merkezi OBB'nin içinde: yerel uzayda en yakın yüzeyi bul */
    Vec3 rel = vec3_sub(a.center, b.center);
    Vec3 axes[3] = {
        vec3(b.axes.m[0], b.axes.m[1], b.axes.m[2]),
        vec3(b.axes.m[3], b.axes.m[4], b.axes.m[5]),
        vec3(b.axes.m[6], b.axes.m[7], b.axes.m[8])
    };
    f32 he[3] = { b.half_extents.x, b.half_extents.y, b.half_extents.z };
    f32 min_d = 1e30f;
    Vec3 best_n = vec3(0.0f, 1.0f, 0.0f);
    for (u32 i = 0; i < 3; i++) {
        f32 proj = vec3_dot(rel, axes[i]);
        f32 d_pos = he[i] - proj;
        f32 d_neg = he[i] + proj;
        if (d_pos < min_d) { min_d = d_pos; best_n = axes[i]; }
        if (d_neg < min_d) { min_d = d_neg; best_n = vec3_scale(axes[i], -1.0f); }
    }
    if (out) {
        out->hit = TRUE;
        out->normal = best_n;
        out->depth = m_max(0.0f, a.radius + min_d);
        out->point = vec3_add(a.center, vec3_scale(best_n, min_d));
    }
    return TRUE;
}

b32 collide_obb_sphere(Obb a, Sphere b, Contact* out) {
    Contact c;
    if (!collide_sphere_obb(b, a, &c)) {
        contact_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->normal = vec3_scale(c.normal, -1.0f);
        out->depth = c.depth;
        out->point = c.point;
    }
    return TRUE;
}

b32 collide_sphere_triangle(Sphere a, Vec3 v0, Vec3 v1, Vec3 v2, Contact* out) {
    if (a.radius < 0.0f) {
        contact_clear(out);
        return FALSE;
    }
    Vec3 closest = shape_closest_point_triangle(v0, v1, v2, a.center);
    Vec3 d = vec3_sub(a.center, closest);
    f32 dist2 = vec3_dot(d, d);
    if (dist2 > a.radius * a.radius) {
        contact_clear(out);
        return FALSE;
    }
    f32 dist = m_sqrt(m_max(0.0f, dist2));
    Vec3 n;
    if (dist > 1e-8f) {
        n = vec3_scale(d, 1.0f / dist);
    } else {
        n = vec3_norm(vec3_cross(vec3_sub(v1, v0), vec3_sub(v2, v0)));
        if (vec3_dot(n, n) < 0.5f) n = vec3(0.0f, 1.0f, 0.0f);
    }
    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = m_max(0.0f, a.radius - dist);
        out->point = closest;
    }
    return TRUE;
}

b32 collide_capsule_triangle(Capsule a, Vec3 v0, Vec3 v1, Vec3 v2, Contact* out) {
    if (a.radius < 0.0f) {
        contact_clear(out);
        return FALSE;
    }
    Vec3 cs, ct;
    shape_closest_points_segment_triangle(a.a, a.b, v0, v1, v2, &cs, &ct);
    Vec3 d = vec3_sub(cs, ct);
    f32 dist2 = vec3_dot(d, d);
    if (dist2 > a.radius * a.radius) {
        contact_clear(out);
        return FALSE;
    }
    f32 dist = m_sqrt(m_max(0.0f, dist2));
    Vec3 n;
    if (dist > 1e-8f) {
        n = vec3_scale(d, 1.0f / dist);
    } else {
        n = vec3_norm(vec3_cross(vec3_sub(v1, v0), vec3_sub(v2, v0)));
        if (vec3_dot(n, n) < 0.5f) n = vec3(0.0f, 1.0f, 0.0f);
    }
    if (out) {
        out->hit = TRUE;
        out->normal = n;
        out->depth = m_max(0.0f, a.radius - dist);
        out->point = ct;
    }
    return TRUE;
}

b32 collide_shapes(const PhysShape* a, const PhysShape* b, Contact* out) {
    if (!a || !b) {
        contact_clear(out);
        return FALSE;
    }
    switch (a->kind) {
    case SHAPE_SPHERE:
        switch (b->kind) {
        case SHAPE_SPHERE:  return collide_sphere_sphere(a->as.sphere, b->as.sphere, out);
        case SHAPE_AABB:    return collide_sphere_aabb(a->as.sphere, b->as.aabb, out);
        case SHAPE_CAPSULE: return collide_sphere_capsule(a->as.sphere, b->as.capsule, out);
        case SHAPE_PLANE:   return collide_sphere_plane(a->as.sphere, b->as.plane, out);
        case SHAPE_OBB:     return collide_sphere_obb(a->as.sphere, b->as.obb, out);
        }
        break;
    case SHAPE_AABB:
        switch (b->kind) {
        case SHAPE_SPHERE:  return collide_aabb_sphere(a->as.aabb, b->as.sphere, out);
        case SHAPE_AABB:    return collide_aabb_aabb(a->as.aabb, b->as.aabb, out);
        case SHAPE_CAPSULE: return collide_aabb_capsule(a->as.aabb, b->as.capsule, out);
        case SHAPE_PLANE:   return collide_aabb_plane(a->as.aabb, b->as.plane, out);
        default: break;
        }
        break;
    case SHAPE_CAPSULE:
        switch (b->kind) {
        case SHAPE_SPHERE:  return collide_capsule_sphere(a->as.capsule, b->as.sphere, out);
        case SHAPE_AABB:    return collide_capsule_aabb(a->as.capsule, b->as.aabb, out);
        case SHAPE_CAPSULE: return collide_capsule_capsule(a->as.capsule, b->as.capsule, out);
        case SHAPE_PLANE:   return collide_capsule_plane(a->as.capsule, b->as.plane, out);
        default: break;
        }
        break;
    case SHAPE_PLANE:
        switch (b->kind) {
        case SHAPE_SPHERE:  return collide_plane_sphere(a->as.plane, b->as.sphere, out);
        case SHAPE_AABB:    return collide_plane_aabb(a->as.plane, b->as.aabb, out);
        case SHAPE_CAPSULE: return collide_plane_capsule(a->as.plane, b->as.capsule, out);
        default: break;
        }
        break;
    case SHAPE_OBB:
        if (b->kind == SHAPE_SPHERE) {
            return collide_obb_sphere(a->as.obb, b->as.sphere, out);
        }
        break;
    }
    contact_clear(out);
    return FALSE;
}
