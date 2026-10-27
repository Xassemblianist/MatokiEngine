/* phys/shapes.c — Sınır kutusu (AABB) hesapları ve 3D en yakın nokta algoritmaları.
 * Doğru parçası, AABB, OBB ve üçgen üzerinde en yakın nokta sorgularını içerir.
 * Dejenere (sıfır uzunluklu doğru parçası, paralel parçalar, çakışık köşeler) durumlarda
 * NaN üretmeyecek şekilde güvenli eşiklerle çalışır.
 */
#include "phys/shapes.h"

Aabb aabb_for_sphere(Sphere s) {
    Vec3 r = vec3(s.radius, s.radius, s.radius);
    return aabb_make(vec3_sub(s.center, r), vec3_add(s.center, r));
}

Aabb aabb_for_capsule(Capsule c) {
    Vec3 r = vec3(c.radius, c.radius, c.radius);
    Vec3 lo = vec3(m_min(c.a.x, c.b.x), m_min(c.a.y, c.b.y), m_min(c.a.z, c.b.z));
    Vec3 hi = vec3(m_max(c.a.x, c.b.x), m_max(c.a.y, c.b.y), m_max(c.a.z, c.b.z));
    return aabb_make(vec3_sub(lo, r), vec3_add(hi, r));
}

Aabb aabb_for_triangle(Vec3 v0, Vec3 v1, Vec3 v2) {
    Vec3 lo = vec3(m_min(v0.x, m_min(v1.x, v2.x)),
                   m_min(v0.y, m_min(v1.y, v2.y)),
                   m_min(v0.z, m_min(v1.z, v2.z)));
    Vec3 hi = vec3(m_max(v0.x, m_max(v1.x, v2.x)),
                   m_max(v0.y, m_max(v1.y, v2.y)),
                   m_max(v0.z, m_max(v1.z, v2.z)));
    return aabb_make(lo, hi);
}

Vec3 shape_closest_point_aabb(Aabb b, Vec3 p) {
    return vec3(
        m_clamp(p.x, b.min.x, b.max.x),
        m_clamp(p.y, b.min.y, b.max.y),
        m_clamp(p.z, b.min.z, b.max.z)
    );
}

Vec3 shape_closest_point_obb(Obb o, Vec3 p) {
    Vec3 d = vec3_sub(p, o.center);
    Vec3 ax = vec3(o.axes.m[0], o.axes.m[1], o.axes.m[2]);
    Vec3 ay = vec3(o.axes.m[3], o.axes.m[4], o.axes.m[5]);
    Vec3 az = vec3(o.axes.m[6], o.axes.m[7], o.axes.m[8]);
    f32 qx = m_clamp(vec3_dot(d, ax), -o.half_extents.x, o.half_extents.x);
    f32 qy = m_clamp(vec3_dot(d, ay), -o.half_extents.y, o.half_extents.y);
    f32 qz = m_clamp(vec3_dot(d, az), -o.half_extents.z, o.half_extents.z);
    Vec3 res = o.center;
    res = vec3_add(res, vec3_scale(ax, qx));
    res = vec3_add(res, vec3_scale(ay, qy));
    res = vec3_add(res, vec3_scale(az, qz));
    return res;
}

Vec3 shape_closest_point_segment(Vec3 a, Vec3 b, Vec3 p, f32* out_t) {
    Vec3 ab = vec3_sub(b, a);
    f32 len2 = vec3_dot(ab, ab);
    f32 t = 0.0f;
    if (len2 > 1e-12f) {
        t = vec3_dot(vec3_sub(p, a), ab) / len2;
        t = m_clamp(t, 0.0f, 1.0f);
    }
    if (out_t) *out_t = t;
    return vec3_add(a, vec3_scale(ab, t));
}

void shape_closest_points_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2,
                                   f32* out_s, f32* out_t,
                                   Vec3* out_c1, Vec3* out_c2) {
    Vec3 d1 = vec3_sub(q1, p1);
    Vec3 d2 = vec3_sub(q2, p2);
    Vec3 r  = vec3_sub(p1, p2);
    f32 a = vec3_dot(d1, d1);
    f32 e = vec3_dot(d2, d2);
    f32 f = vec3_dot(d2, r);
    f32 s = 0.0f, t = 0.0f;

    if (a <= 1e-12f && e <= 1e-12f) {
        s = 0.0f;
        t = 0.0f;
    } else if (a <= 1e-12f) {
        s = 0.0f;
        t = m_clamp(f / e, 0.0f, 1.0f);
    } else {
        f32 c = vec3_dot(d1, r);
        if (e <= 1e-12f) {
            t = 0.0f;
            s = m_clamp(-c / a, 0.0f, 1.0f);
        } else {
            f32 b = vec3_dot(d1, d2);
            f32 denom = a * e - b * b;
            /* Paralel veya neredeyse paralel doğru parçaları: örtüşen aralığın ortasını seç */
            if (denom > 1e-10f * a * e) {
                s = m_clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            } else {
                f32 s0 = vec3_dot(vec3_sub(p2, p1), d1) / a;
                f32 s1 = vec3_dot(vec3_sub(q2, p1), d1) / a;
                f32 lo = m_max(0.0f, m_min(s0, s1));
                f32 hi = m_min(1.0f, m_max(s0, s1));
                s = (lo <= hi) ? 0.5f * (lo + hi) : m_clamp(0.5f * (s0 + s1), 0.0f, 1.0f);
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = m_clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = m_clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    if (out_s) *out_s = s;
    if (out_t) *out_t = t;
    if (out_c1) *out_c1 = vec3_add(p1, vec3_scale(d1, s));
    if (out_c2) *out_c2 = vec3_add(p2, vec3_scale(d2, t));
}

Vec3 shape_closest_point_triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 p) {
    /* Ericson, Real-Time Collision Detection §5.1.5 — Voronoi bölgeleri */
    Vec3 ab = vec3_sub(b, a);
    Vec3 ac = vec3_sub(c, a);
    Vec3 ap = vec3_sub(p, a);
    f32 d1 = vec3_dot(ab, ap);
    f32 d2 = vec3_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    Vec3 bp = vec3_sub(p, b);
    f32 d3 = vec3_dot(ab, bp);
    f32 d4 = vec3_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        f32 denom = d1 - d3;
        f32 v = (denom > 1e-12f) ? (d1 / denom) : 0.0f;
        return vec3_add(a, vec3_scale(ab, v));
    }

    Vec3 cp = vec3_sub(p, c);
    f32 d5 = vec3_dot(ab, cp);
    f32 d6 = vec3_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        f32 denom = d2 - d6;
        f32 w = (denom > 1e-12f) ? (d2 / denom) : 0.0f;
        return vec3_add(a, vec3_scale(ac, w));
    }

    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        f32 denom = (d4 - d3) + (d5 - d6);
        f32 w = (denom > 1e-12f) ? ((d4 - d3) / denom) : 0.0f;
        return vec3_add(b, vec3_scale(vec3_sub(c, b), w));
    }

    f32 sum = va + vb + vc;
    if (sum <= 1e-12f) {
        return shape_closest_point_segment(a, b, p, NULL);
    }
    f32 denom = 1.0f / sum;
    f32 v = vb * denom;
    f32 w = vc * denom;
    return vec3_add(a, vec3_add(vec3_scale(ab, v), vec3_scale(ac, w)));
}

void shape_closest_points_segment_triangle(Vec3 p, Vec3 q,
                                           Vec3 a, Vec3 b, Vec3 c,
                                           Vec3* out_seg, Vec3* out_tri) {
    Vec3 best_s = p;
    Vec3 best_t = shape_closest_point_triangle(a, b, c, p);
    f32 best_d2 = vec3_dot(vec3_sub(best_s, best_t), vec3_sub(best_s, best_t));

    /* Uç nokta Q -> üçgen */
    {
        Vec3 ct = shape_closest_point_triangle(a, b, c, q);
        Vec3 diff = vec3_sub(q, ct);
        f32 d2 = vec3_dot(diff, diff);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_s = q;
            best_t = ct;
        }
    }

    /* Doğru parçası PQ -> 3 üçgen kenarı */
    Vec3 edges[3][2] = { { a, b }, { b, c }, { c, a } };
    for (u32 i = 0; i < 3; i++) {
        Vec3 cs, ct;
        shape_closest_points_segments(p, q, edges[i][0], edges[i][1], NULL, NULL, &cs, &ct);
        Vec3 diff = vec3_sub(cs, ct);
        f32 d2 = vec3_dot(diff, diff);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_s = cs;
            best_t = ct;
        }
    }

    /* Doğru parçası üçgen düzlemini üçgenin içinden deliyor mu? (kesişim -> mesafe 0) */
    {
        Vec3 e1 = vec3_sub(b, a);
        Vec3 e2 = vec3_sub(c, a);
        Vec3 dir = vec3_sub(q, p);
        Vec3 pvec = vec3_cross(dir, e2);
        f32 det = vec3_dot(e1, pvec);
        if (m_abs(det) > 1e-10f) {
            f32 inv_det = 1.0f / det;
            Vec3 tvec = vec3_sub(p, a);
            f32 u = vec3_dot(tvec, pvec) * inv_det;
            if (u >= 0.0f && u <= 1.0f) {
                Vec3 qvec = vec3_cross(tvec, e1);
                f32 v = vec3_dot(dir, qvec) * inv_det;
                if (v >= 0.0f && u + v <= 1.0f) {
                    f32 t = vec3_dot(e2, qvec) * inv_det;
                    if (t >= 0.0f && t <= 1.0f) {
                        Vec3 hit = vec3_add(p, vec3_scale(dir, t));
                        best_s = hit;
                        best_t = hit;
                    }
                }
            }
        }
    }

    if (out_seg) *out_seg = best_s;
    if (out_tri) *out_tri = best_t;
}
