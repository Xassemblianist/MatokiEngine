/* phys/raycast.c — Küre, AABB, kapsül, düzlem, üçgen (Möller–Trumbore) ve BVH-Mesh ışın kesişimi.
 * Şeklin içinden başlayan ışınlarda yüzeyden çıkış/ilk pozitif sınır kesişimi (t >= 0) döner.
 * BVH gezintisi önden arkaya (near-first) sıralıdır ve kaba kuvvet ile bit-bit aynı sonucu üretir.
 */
#include "phys/raycast.h"

static void rayhit_clear(RayHit* out) {
    if (!out) return;
    out->hit = FALSE;
    out->t = 0.0f;
    out->point = vec3(0.0f, 0.0f, 0.0f);
    out->normal = vec3(0.0f, 0.0f, 0.0f);
    out->tri_index = U32_MAX;
}

b32 raycast_sphere(Ray r, Sphere s, f32 t_max, RayHit* out) {
    if (s.radius < 0.0f || t_max < 0.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    Vec3 oc = vec3_sub(r.origin, s.center);
    f32 a = vec3_dot(r.dir, r.dir);
    if (a <= 1e-12f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 half_b = vec3_dot(oc, r.dir);
    f32 c = vec3_dot(oc, oc) - s.radius * s.radius;
    f32 disc = half_b * half_b - a * c;
    if (disc < 0.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 sq = m_sqrt(m_max(0.0f, disc));
    f32 t = (-half_b - sq) / a;
    if (t < 0.0f) {
        t = (-half_b + sq) / a;
    }
    if (t < 0.0f || t > t_max) {
        rayhit_clear(out);
        return FALSE;
    }
    Vec3 pt = ray_at(r, t);
    Vec3 n = (s.radius > 1e-8f)
        ? vec3_norm(vec3_sub(pt, s.center))
        : vec3_scale(vec3_norm(r.dir), -1.0f);
    if (out) {
        out->hit = TRUE;
        out->t = t;
        out->point = pt;
        out->normal = n;
        out->tri_index = U32_MAX;
    }
    return TRUE;
}

b32 raycast_aabb(Ray r, Aabb b, f32 t_max, RayHit* out) {
    if (t_max < 0.0f || b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z) {
        rayhit_clear(out);
        return FALSE;
    }

    f32 orig[3] = { r.origin.x, r.origin.y, r.origin.z };
    f32 dir[3]  = { r.dir.x,    r.dir.y,    r.dir.z };
    f32 bmin[3] = { b.min.x,    b.min.y,    b.min.z };
    f32 bmax[3] = { b.max.x,    b.max.y,    b.max.z };

    f32 t_enter = -1e30f;
    f32 t_exit  =  1e30f;
    Vec3 n_enter = vec3(0.0f, 1.0f, 0.0f);
    Vec3 n_exit  = vec3(0.0f, 1.0f, 0.0f);

    for (u32 i = 0; i < 3; i++) {
        if (m_abs(dir[i]) < 1e-12f) {
            if (orig[i] < bmin[i] || orig[i] > bmax[i]) {
                rayhit_clear(out);
                return FALSE;
            }
        } else {
            f32 inv_d = 1.0f / dir[i];
            f32 t1 = (bmin[i] - orig[i]) * inv_d;
            f32 t2 = (bmax[i] - orig[i]) * inv_d;
            Vec3 n1 = vec3(i == 0 ? -1.0f : 0.0f, i == 1 ? -1.0f : 0.0f, i == 2 ? -1.0f : 0.0f);
            Vec3 n2 = vec3(i == 0 ?  1.0f : 0.0f, i == 1 ?  1.0f : 0.0f, i == 2 ?  1.0f : 0.0f);
            if (t1 > t2) {
                f32 tmp = t1; t1 = t2; t2 = tmp;
                Vec3 tn = n1; n1 = n2; n2 = tn;
            }
            if (t1 > t_enter) {
                t_enter = t1;
                n_enter = n1;
            }
            if (t2 < t_exit) {
                t_exit = t2;
                n_exit = n2;
            }
            if (t_enter > t_exit) {
                rayhit_clear(out);
                return FALSE;
            }
        }
    }

    if (t_exit < 0.0f) {
        rayhit_clear(out);
        return FALSE;
    }

    f32 t = (t_enter >= 0.0f) ? t_enter : t_exit;
    Vec3 n = (t_enter >= 0.0f) ? n_enter : n_exit;
    if (t < 0.0f || t > t_max) {
        rayhit_clear(out);
        return FALSE;
    }

    if (out) {
        out->hit = TRUE;
        out->t = t;
        out->point = ray_at(r, t);
        out->normal = n;
        out->tri_index = U32_MAX;
    }
    return TRUE;
}

b32 raycast_capsule(Ray r, Capsule c, f32 t_max, RayHit* out) {
    if (c.radius < 0.0f || t_max < 0.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    Vec3 ba = vec3_sub(c.b, c.a);
    f32 len2 = vec3_dot(ba, ba);
    if (len2 <= 1e-12f) {
        return raycast_sphere(r, sphere_make(c.a, c.radius), t_max, out);
    }

    f32 seg_len = m_sqrt(len2);
    Vec3 u = vec3_scale(ba, 1.0f / seg_len);
    Vec3 oa = vec3_sub(r.origin, c.a);

    f32 d_dot_u  = vec3_dot(r.dir, u);
    f32 oa_dot_u = vec3_dot(oa, u);
    Vec3 d_perp  = vec3_sub(r.dir, vec3_scale(u, d_dot_u));
    Vec3 oa_perp = vec3_sub(oa,    vec3_scale(u, oa_dot_u));

    f32 best_t = t_max + 1.0f;
    Vec3 best_n = vec3(0.0f, 1.0f, 0.0f);
    b32 found = FALSE;

    /* 1) Silindir gövdesi (0 <= s <= seg_len) */
    f32 a_cyl = vec3_dot(d_perp, d_perp);
    f32 b_cyl = vec3_dot(oa_perp, d_perp);
    f32 c_cyl = vec3_dot(oa_perp, oa_perp) - c.radius * c.radius;
    if (a_cyl > 1e-12f) {
        f32 disc = b_cyl * b_cyl - a_cyl * c_cyl;
        if (disc >= 0.0f) {
            f32 sq = m_sqrt(m_max(0.0f, disc));
            f32 roots[2] = { (-b_cyl - sq) / a_cyl, (-b_cyl + sq) / a_cyl };
            for (u32 k = 0; k < 2; k++) {
                f32 t = roots[k];
                if (t < 0.0f || t > t_max || t >= best_t) continue;
                f32 s = oa_dot_u + t * d_dot_u;
                if (s >= 0.0f && s <= seg_len) {
                    Vec3 pt = ray_at(r, t);
                    Vec3 axis_pt = vec3_add(c.a, vec3_scale(u, s));
                    best_t = t;
                    best_n = (c.radius > 1e-8f) ? vec3_norm(vec3_sub(pt, axis_pt)) : vec3(0.0f, 1.0f, 0.0f);
                    found = TRUE;
                }
            }
        }
    }

    /* 2) Uç yarım-küre kapakları (A için s <= 0, B için s >= seg_len) */
    Vec3 caps[2] = { c.a, c.b };
    f32 a_sph = vec3_dot(r.dir, r.dir);
    if (a_sph > 1e-12f) {
        for (u32 cap_i = 0; cap_i < 2; cap_i++) {
            Vec3 oc = vec3_sub(r.origin, caps[cap_i]);
            f32 hb = vec3_dot(oc, r.dir);
            f32 cs = vec3_dot(oc, oc) - c.radius * c.radius;
            f32 disc = hb * hb - a_sph * cs;
            if (disc < 0.0f) continue;
            f32 sq = m_sqrt(m_max(0.0f, disc));
            f32 roots[2] = { (-hb - sq) / a_sph, (-hb + sq) / a_sph };
            for (u32 k = 0; k < 2; k++) {
                f32 t = roots[k];
                if (t < 0.0f || t > t_max || t >= best_t) continue;
                f32 s = oa_dot_u + t * d_dot_u;
                b32 on_cap = (cap_i == 0u) ? (s <= 1e-5f) : (s >= seg_len - 1e-5f);
                if (on_cap) {
                    Vec3 pt = ray_at(r, t);
                    best_t = t;
                    best_n = (c.radius > 1e-8f) ? vec3_norm(vec3_sub(pt, caps[cap_i])) : u;
                    found = TRUE;
                }
            }
        }
    }

    if (!found) {
        rayhit_clear(out);
        return FALSE;
    }

    if (out) {
        out->hit = TRUE;
        out->t = best_t;
        out->point = ray_at(r, best_t);
        out->normal = best_n;
        out->tri_index = U32_MAX;
    }
    return TRUE;
}

b32 raycast_plane(Ray r, Plane p, f32 t_max, RayHit* out) {
    if (t_max < 0.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 n_len = vec3_len(p.normal);
    if (n_len <= 1e-8f) {
        rayhit_clear(out);
        return FALSE;
    }
    Vec3 n = vec3_scale(p.normal, 1.0f / n_len);
    f32 d = p.d / n_len;

    f32 denom = vec3_dot(n, r.dir);
    if (m_abs(denom) < 1e-8f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 t = (d - vec3_dot(n, r.origin)) / denom;
    if (t < 0.0f || t > t_max) {
        rayhit_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->t = t;
        out->point = ray_at(r, t);
        out->normal = n;
        out->tri_index = U32_MAX;
    }
    return TRUE;
}

b32 raycast_triangle(Ray r, Vec3 v0, Vec3 v1, Vec3 v2, f32 t_max, RayHit* out) {
    /* Möller–Trumbore ışın-üçgen kesişimi (iki yüzlü) */
    Vec3 e1 = vec3_sub(v1, v0);
    Vec3 e2 = vec3_sub(v2, v0);
    Vec3 pvec = vec3_cross(r.dir, e2);
    f32 det = vec3_dot(e1, pvec);
    if (m_abs(det) < 1e-8f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 inv_det = 1.0f / det;
    Vec3 tvec = vec3_sub(r.origin, v0);
    f32 u = vec3_dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    Vec3 qvec = vec3_cross(tvec, e1);
    f32 v = vec3_dot(r.dir, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        rayhit_clear(out);
        return FALSE;
    }
    f32 t = vec3_dot(e2, qvec) * inv_det;
    if (t < 0.0f || t > t_max) {
        rayhit_clear(out);
        return FALSE;
    }
    if (out) {
        out->hit = TRUE;
        out->t = t;
        out->point = ray_at(r, t);
        out->normal = vec3_norm(vec3_cross(e1, e2));
        out->tri_index = U32_MAX;
    }
    return TRUE;
}

b32 raycast_mesh_brute(Ray r, const Mesh* mesh, f32 t_max, RayHit* out) {
    rayhit_clear(out);
    if (!mesh || mesh->index_count < 3u || t_max < 0.0f) return FALSE;

    u32 tri_count = mesh->index_count / 3u;
    RayHit best;
    rayhit_clear(&best);
    best.t = t_max;

    for (u32 t = 0; t < tri_count; t++) {
        u32 i0 = mesh->indices[t * 3u + 0u];
        u32 i1 = mesh->indices[t * 3u + 1u];
        u32 i2 = mesh->indices[t * 3u + 2u];
        Vec3 v0 = vec3(mesh->px[i0], mesh->py[i0], mesh->pz[i0]);
        Vec3 v1 = vec3(mesh->px[i1], mesh->py[i1], mesh->pz[i1]);
        Vec3 v2 = vec3(mesh->px[i2], mesh->py[i2], mesh->pz[i2]);
        RayHit h;
        if (raycast_triangle(r, v0, v1, v2, best.t, &h)) {
            if (!best.hit || h.t < best.t || (h.t == best.t && t < best.tri_index)) {
                best = h;
                best.tri_index = t;
            }
        }
    }

    if (best.hit && out) {
        *out = best;
    }
    return best.hit;
}

static inline b32 bvh_slab_test(Aabb b, Vec3 orig, f32 inv_x, f32 inv_y, f32 inv_z,
                                f32 max_t, f32* out_tmin) {
    f32 tx1 = (b.min.x - orig.x) * inv_x;
    f32 tx2 = (b.max.x - orig.x) * inv_x;
    f32 tmin = m_min(tx1, tx2);
    f32 tmax = m_max(tx1, tx2);

    f32 ty1 = (b.min.y - orig.y) * inv_y;
    f32 ty2 = (b.max.y - orig.y) * inv_y;
    tmin = m_max(tmin, m_min(ty1, ty2));
    tmax = m_min(tmax, m_max(ty1, ty2));

    f32 tz1 = (b.min.z - orig.z) * inv_z;
    f32 tz2 = (b.max.z - orig.z) * inv_z;
    tmin = m_max(tmin, m_min(tz1, tz2));
    tmax = m_min(tmax, m_max(tz1, tz2));

    if (tmax < 0.0f || tmin > tmax || tmin > max_t + 1e-5f) return FALSE;
    *out_tmin = tmin;
    return TRUE;
}

typedef struct {
    u32 node_idx;
    f32 tmin;
} BvhStackEntry;

b32 raycast_mesh(Ray r, const Mesh* mesh, const Bvh* bvh, f32 t_max, RayHit* out) {
    if (!bvh) {
        return raycast_mesh_brute(r, mesh, t_max, out);
    }
    rayhit_clear(out);
    if (bvh->node_count == 0u || t_max < 0.0f) return FALSE;

    f32 inv_x = (m_abs(r.dir.x) > 1e-20f) ? (1.0f / r.dir.x) : ((r.dir.x >= 0.0f) ? 1e20f : -1e20f);
    f32 inv_y = (m_abs(r.dir.y) > 1e-20f) ? (1.0f / r.dir.y) : ((r.dir.y >= 0.0f) ? 1e20f : -1e20f);
    f32 inv_z = (m_abs(r.dir.z) > 1e-20f) ? (1.0f / r.dir.z) : ((r.dir.z >= 0.0f) ? 1e20f : -1e20f);

    RayHit best;
    rayhit_clear(&best);
    best.t = t_max;

    f32 root_tmin = 0.0f;
    if (!bvh_slab_test(bvh->nodes[0].bounds, r.origin, inv_x, inv_y, inv_z, best.t, &root_tmin)) {
        return FALSE;
    }

    BvhStackEntry stack[64];
    u32 sp = 0u;
    stack[sp].node_idx = 0u;
    stack[sp].tmin = root_tmin;
    sp++;

    while (sp > 0u) {
        sp--;
        u32 idx = stack[sp].node_idx;
        if (stack[sp].tmin > best.t + 1e-5f) continue;

        const BvhNode* node = &bvh->nodes[idx];
        if (node->count > 0u) {
            const BvhTri* tris = bvh->tris + node->left_first;
            for (u32 i = 0; i < node->count; i++) {
                RayHit h;
                if (raycast_triangle(r, tris[i].v0, tris[i].v1, tris[i].v2, best.t, &h)) {
                    u32 tidx = tris[i].tri_index;
                    if (!best.hit || h.t < best.t || (h.t == best.t && tidx < best.tri_index)) {
                        best = h;
                        best.tri_index = tidx;
                    }
                }
            }
        } else {
            u32 left_idx  = node->left_first;
            u32 right_idx = left_idx + 1u;
            f32 tmin_l = 0.0f, tmin_r = 0.0f;
            b32 hit_l = bvh_slab_test(bvh->nodes[left_idx].bounds,  r.origin, inv_x, inv_y, inv_z, best.t, &tmin_l);
            b32 hit_r = bvh_slab_test(bvh->nodes[right_idx].bounds, r.origin, inv_x, inv_y, inv_z, best.t, &tmin_r);

            if (hit_l && hit_r) {
                /* Daha yakın çocuğu yığının üstüne koy (önce gezilsin) */
                if (tmin_l <= tmin_r) {
                    if (sp + 2u <= 64u) {
                        stack[sp].node_idx = right_idx; stack[sp].tmin = tmin_r; sp++;
                        stack[sp].node_idx = left_idx;  stack[sp].tmin = tmin_l; sp++;
                    }
                } else {
                    if (sp + 2u <= 64u) {
                        stack[sp].node_idx = left_idx;  stack[sp].tmin = tmin_l; sp++;
                        stack[sp].node_idx = right_idx; stack[sp].tmin = tmin_r; sp++;
                    }
                }
            } else if (hit_l) {
                if (sp < 64u) { stack[sp].node_idx = left_idx; stack[sp].tmin = tmin_l; sp++; }
            } else if (hit_r) {
                if (sp < 64u) { stack[sp].node_idx = right_idx; stack[sp].tmin = tmin_r; sp++; }
            }
        }
    }

    if (best.hit && out) {
        *out = best;
    }
    return best.hit;
}

b32 raycast_shapes(Ray r, const PhysShape* s, f32 t_max, RayHit* out) {
    if (!s) {
        rayhit_clear(out);
        return FALSE;
    }
    switch (s->kind) {
    case SHAPE_SPHERE:  return raycast_sphere(r, s->as.sphere, t_max, out);
    case SHAPE_AABB:    return raycast_aabb(r, s->as.aabb, t_max, out);
    case SHAPE_CAPSULE: return raycast_capsule(r, s->as.capsule, t_max, out);
    case SHAPE_PLANE:   return raycast_plane(r, s->as.plane, t_max, out);
    default:
        rayhit_clear(out);
        return FALSE;
    }
}
