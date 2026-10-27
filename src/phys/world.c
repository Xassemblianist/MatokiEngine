/* phys/world.c — 3D fizik dünyası simülasyonu ve karakter kontrolcüsü (move_and_slide).
 * Yarı-örtük Euler entegrasyonu, yarıçap-oranlı alt adımlama (tünelleme önleme),
 * eğim sınırı (max_slope_cos) ve Coulomb sürtünme / esneklik (restitution) çözümünü uygular.
 */
#include "phys/world.h"

#define SOLVER_ITERS       4u
#define MAX_SUBSTEPS     256u
#define BVH_QUERY_CAP    256u

void phys_world_init(PhysWorld* w, Arena* a, u32 max_bodies, u32 max_statics, u32 max_meshes) {
    w->gravity      = vec3(0.0f, PHYS_DEFAULT_GRAVITY, 0.0f);
    w->fixed_dt     = PHYS_FIXED_DT;
    w->accum        = 0.0f;
    w->body_count   = 0u;
    w->body_cap     = max_bodies;
    w->bodies       = (max_bodies > 0u) ? ARENA_PUSH_ARRAY(a, PhysBody, max_bodies) : NULL;
    w->static_count = 0u;
    w->static_cap   = max_statics;
    w->statics      = (max_statics > 0u) ? ARENA_PUSH_ARRAY(a, PhysStaticShape, max_statics) : NULL;
    w->mesh_count   = 0u;
    w->mesh_cap     = max_meshes;
    w->meshes       = (max_meshes > 0u) ? ARENA_PUSH_ARRAY(a, PhysStaticMesh, max_meshes) : NULL;
}

u32 phys_world_add_sphere(PhysWorld* w, Vec3 center, f32 radius,
                          f32 mass, f32 restitution, f32 friction) {
    if (w->body_count >= w->body_cap) return U32_MAX;
    u32 idx = w->body_count++;
    PhysBody* b = &w->bodies[idx];
    b->kind          = BODY_SPHERE;
    b->pos           = center;
    b->vel           = vec3(0.0f, 0.0f, 0.0f);
    b->seg_offset    = vec3(0.0f, 0.0f, 0.0f);
    b->radius        = radius;
    b->inv_mass      = (mass > 1e-8f) ? (1.0f / mass) : 0.0f;
    b->restitution   = m_clamp(restitution, 0.0f, 1.0f);
    b->friction      = m_clamp(friction, 0.0f, 1.0f);
    b->max_slope_cos = PHYS_DEFAULT_SLOPE_COS;
    b->on_ground     = FALSE;
    b->ground_normal = vec3(0.0f, 1.0f, 0.0f);
    return idx;
}

u32 phys_world_add_capsule(PhysWorld* w, Vec3 a, Vec3 b_pt, f32 radius,
                           f32 mass, f32 restitution, f32 friction) {
    if (w->body_count >= w->body_cap) return U32_MAX;
    u32 idx = w->body_count++;
    PhysBody* b = &w->bodies[idx];
    b->kind          = BODY_CAPSULE;
    b->pos           = vec3_scale(vec3_add(a, b_pt), 0.5f);
    b->vel           = vec3(0.0f, 0.0f, 0.0f);
    b->seg_offset    = vec3_scale(vec3_sub(b_pt, a), 0.5f);
    b->radius        = radius;
    b->inv_mass      = (mass > 1e-8f) ? (1.0f / mass) : 0.0f;
    b->restitution   = m_clamp(restitution, 0.0f, 1.0f);
    b->friction      = m_clamp(friction, 0.0f, 1.0f);
    b->max_slope_cos = PHYS_DEFAULT_SLOPE_COS;
    b->on_ground     = FALSE;
    b->ground_normal = vec3(0.0f, 1.0f, 0.0f);
    return idx;
}

u32 phys_world_add_static(PhysWorld* w, PhysShape shape, f32 restitution, f32 friction) {
    if (w->static_count >= w->static_cap) return U32_MAX;
    u32 idx = w->static_count++;
    w->statics[idx].shape       = shape;
    w->statics[idx].restitution = m_clamp(restitution, 0.0f, 1.0f);
    w->statics[idx].friction    = m_clamp(friction, 0.0f, 1.0f);
    return idx;
}

u32 phys_world_add_mesh(PhysWorld* w, const Mesh* mesh, const Bvh* bvh,
                        f32 restitution, f32 friction) {
    if (w->mesh_count >= w->mesh_cap) return U32_MAX;
    u32 idx = w->mesh_count++;
    w->meshes[idx].mesh        = mesh;
    w->meshes[idx].bvh         = bvh;
    w->meshes[idx].restitution = m_clamp(restitution, 0.0f, 1.0f);
    w->meshes[idx].friction    = m_clamp(friction, 0.0f, 1.0f);
    return idx;
}

Sphere phys_body_sphere(const PhysBody* b) {
    return sphere_make(b->pos, b->radius);
}

Capsule phys_body_capsule(const PhysBody* b) {
    return capsule_make(vec3_sub(b->pos, b->seg_offset),
                        vec3_add(b->pos, b->seg_offset),
                        b->radius);
}

PhysShape phys_body_shape(const PhysBody* b) {
    if (b->kind == BODY_CAPSULE) {
        return shape_from_capsule(phys_body_capsule(b));
    }
    return shape_from_sphere(phys_body_sphere(b));
}

static Aabb shape_bounds(const PhysShape* s) {
    if (s->kind == SHAPE_CAPSULE) return aabb_for_capsule(s->as.capsule);
    if (s->kind == SHAPE_SPHERE)  return aabb_for_sphere(s->as.sphere);
    if (s->kind == SHAPE_AABB)    return s->as.aabb;
    return aabb_empty();
}

static b32 collide_shape_tri(const PhysShape* s, Vec3 v0, Vec3 v1, Vec3 v2, Contact* out) {
    if (s->kind == SHAPE_CAPSULE) {
        return collide_capsule_triangle(s->as.capsule, v0, v1, v2, out);
    }
    if (s->kind == SHAPE_SPHERE) {
        return collide_sphere_triangle(s->as.sphere, v0, v1, v2, out);
    }
    return FALSE;
}

static void resolve_body_contact(PhysBody* b, Contact c, f32 surf_rest, f32 surf_fric,
                                 f32 grav_step) {
    if (!c.hit) return;

    b32 walkable = (c.normal.y >= b->max_slope_cos && c.normal.y > 0.1f);
    if (walkable) {
        b->on_ground = TRUE;
        b->ground_normal = c.normal;
    }

    if (c.depth > 0.0f) {
        if (b->kind == BODY_CAPSULE && walkable) {
            /* Eğim sınırı altındaki zeminde dikey konum düzeltmesi: yerçekimi yatay kayma üretmez */
            b->pos.y += c.depth / c.normal.y;
        } else {
            b->pos = vec3_add(b->pos, vec3_scale(c.normal, c.depth));
        }
    }

    f32 vn = vec3_dot(b->vel, c.normal);
    if (vn < 0.0f) {
        f32 e = b->restitution * surf_rest;
        /* Düşük çarpma hızlarında mikro-zıplamayı kes (yerde ~0 derinlikle sakin duruş) */
        if (-vn <= 2.5f * grav_step + 0.05f) {
            e = 0.0f;
        }
        f32 jn = -(1.0f + e) * vn;
        b->vel = vec3_add(b->vel, vec3_scale(c.normal, jn));

        /* Teğetsel hız ve sürtünme */
        Vec3 vt = vec3_sub(b->vel, vec3_scale(c.normal, vec3_dot(b->vel, c.normal)));
        f32 vt_len = vec3_len(vt);
        f32 mu = 0.5f * (b->friction + surf_fric);

        if (b->kind == BODY_CAPSULE && walkable) {
            /* Eğim sınırı altındaki kapsül: yerçekimi kaynaklı kaymayı durdur */
            if (vt_len <= m_max(mu * jn, 2.0f * grav_step + 0.05f)) {
                b->vel = vec3_sub(b->vel, vt);
            } else if (vt_len > 1e-8f) {
                f32 drop = m_min(vt_len, mu * jn);
                b->vel = vec3_sub(b->vel, vec3_scale(vt, drop / vt_len));
            }
        } else if (vt_len > 1e-8f && mu > 0.0f) {
            /* Eğim sınırı üstündeki dik eğimde yalnızca dinamik sürtünme (eğim > sınır ise kayar) */
            f32 factor = walkable ? 1.0f : 0.0f;
            f32 drop = m_min(vt_len, factor * mu * jn);
            b->vel = vec3_sub(b->vel, vec3_scale(vt, drop / vt_len));
        }
    }
}

static void solve_body_vs_world(const PhysWorld* w, PhysBody* b, f32 grav_step) {
    for (u32 iter = 0; iter < SOLVER_ITERS; iter++) {
        b32 any_deep = FALSE;

        /* 1) Statik analitik şekiller */
        for (u32 s = 0; s < w->static_count; s++) {
            PhysShape bs = phys_body_shape(b);
            Contact c;
            if (collide_shapes(&bs, &w->statics[s].shape, &c)) {
                if (c.depth > 1e-5f) any_deep = TRUE;
                resolve_body_contact(b, c, w->statics[s].restitution, w->statics[s].friction, grav_step);
            }
        }

        /* 2) Statik üçgen ağları (BVH varsa yaprak sorgusu, yoksa AABB filtreli döngü) */
        for (u32 m = 0; m < w->mesh_count; m++) {
            const Mesh* mesh = w->meshes[m].mesh;
            const Bvh*  bvh  = w->meshes[m].bvh;
            if (!mesh) continue;
            PhysShape bs = phys_body_shape(b);
            Aabb qbox = aabb_expand_margin(shape_bounds(&bs), 1e-3f);

            if (bvh && bvh->node_count > 0u) {
                u32 slots[BVH_QUERY_CAP];
                u32 cnt = bvh_query_aabb(bvh, qbox, slots, BVH_QUERY_CAP);
                for (u32 k = 0; k < cnt; k++) {
                    const BvhTri* bt = &bvh->tris[slots[k]];
                    Contact c;
                    if (collide_shape_tri(&bs, bt->v0, bt->v1, bt->v2, &c)) {
                        if (c.depth > 1e-5f) any_deep = TRUE;
                        resolve_body_contact(b, c, w->meshes[m].restitution, w->meshes[m].friction, grav_step);
                        bs = phys_body_shape(b);
                    }
                }
            } else {
                u32 tri_count = mesh->index_count / 3u;
                for (u32 t = 0; t < tri_count; t++) {
                    u32 i0 = mesh->indices[t * 3u + 0u];
                    u32 i1 = mesh->indices[t * 3u + 1u];
                    u32 i2 = mesh->indices[t * 3u + 2u];
                    Vec3 v0 = vec3(mesh->px[i0], mesh->py[i0], mesh->pz[i0]);
                    Vec3 v1 = vec3(mesh->px[i1], mesh->py[i1], mesh->pz[i1]);
                    Vec3 v2 = vec3(mesh->px[i2], mesh->py[i2], mesh->pz[i2]);
                    if (!aabb_overlaps(qbox, aabb_for_triangle(v0, v1, v2))) continue;
                    Contact c;
                    if (collide_shape_tri(&bs, v0, v1, v2, &c)) {
                        if (c.depth > 1e-5f) any_deep = TRUE;
                        resolve_body_contact(b, c, w->meshes[m].restitution, w->meshes[m].friction, grav_step);
                        bs = phys_body_shape(b);
                    }
                }
            }
        }

        if (!any_deep) break;
    }
}

void phys_world_step(PhysWorld* w) {
    f32 dt = w->fixed_dt;
    f32 grav_step = vec3_len(w->gravity) * dt;

    for (u32 i = 0; i < w->body_count; i++) {
        PhysBody* b = &w->bodies[i];
        if (b->inv_mass <= 0.0f) continue;

        /* Yarı-örtük Euler: önce hız, sonra (alt adımlı) konum */
        b->vel = vec3_add(b->vel, vec3_scale(w->gravity, dt));
        b->on_ground = FALSE;

        f32 disp = vec3_len(b->vel) * dt;
        f32 max_step = m_max(0.02f, b->radius * 0.4f);
        u32 steps = (disp > max_step) ? (u32)m_ceil(disp / max_step) : 1u;
        if (steps > MAX_SUBSTEPS) steps = MAX_SUBSTEPS;
        f32 sub_dt = dt / (f32)steps;

        for (u32 s = 0; s < steps; s++) {
            b->pos = vec3_add(b->pos, vec3_scale(b->vel, sub_dt));
            solve_body_vs_world(w, b, grav_step);
        }
    }

    /* Dinamik - dinamik gövde çiftleri */
    for (u32 i = 0; i < w->body_count; i++) {
        for (u32 j = i + 1u; j < w->body_count; j++) {
            PhysBody* a = &w->bodies[i];
            PhysBody* b = &w->bodies[j];
            f32 inv_sum = a->inv_mass + b->inv_mass;
            if (inv_sum <= 0.0f) continue;

            PhysShape sa = phys_body_shape(a);
            PhysShape sb = phys_body_shape(b);
            Contact c;
            if (!collide_shapes(&sa, &sb, &c)) continue;

            if (c.depth > 0.0f) {
                f32 wa = a->inv_mass / inv_sum;
                f32 wb = b->inv_mass / inv_sum;
                a->pos = vec3_add(a->pos, vec3_scale(c.normal,  c.depth * wa));
                b->pos = vec3_sub(b->pos, vec3_scale(c.normal,  c.depth * wb));
            }

            Vec3 rv = vec3_sub(a->vel, b->vel);
            f32 vn = vec3_dot(rv, c.normal);
            if (vn < 0.0f) {
                f32 e = m_min(a->restitution, b->restitution);
                if (-vn <= 2.5f * grav_step + 0.05f) e = 0.0f;
                f32 j_imp = -(1.0f + e) * vn / inv_sum;
                Vec3 imp = vec3_scale(c.normal, j_imp);
                a->vel = vec3_add(a->vel, vec3_scale(imp, a->inv_mass));
                b->vel = vec3_sub(b->vel, vec3_scale(imp, b->inv_mass));
            }
        }
    }
}

u32 phys_world_update(PhysWorld* w, f32 dt) {
    if (dt <= 0.0f) return 0u;
    w->accum += dt;
    u32 steps = 0u;
    while (w->accum >= w->fixed_dt && steps < 64u) {
        phys_world_step(w);
        w->accum -= w->fixed_dt;
        steps++;
    }
    return steps;
}

static void apply_slide_contact(PhysShape* sh, Vec3* vel, Contact c,
                                f32 max_slope_cos, SlideResult* res) {
    if (!c.hit) return;
    res->contact_count++;

    b32 walkable = (c.normal.y >= max_slope_cos && c.normal.y > 0.1f);
    Vec3 push = vec3(0.0f, 0.0f, 0.0f);

    if (walkable) {
        res->on_ground = TRUE;
        res->ground_normal = c.normal;
        if (c.depth > 0.0f) {
            /* Eğim sınırı altındaki yüzeyde dikey çıkarma -> aşağı kayma olmaz */
            push = vec3(0.0f, c.depth / c.normal.y, 0.0f);
        }
        /* Düşey hız bileşenini sıfırla, yatay yürüme hızı varsa yüzeye teğet koru */
        f32 vn = vec3_dot(*vel, c.normal);
        if (vn < 0.0f) {
            if (m_abs(vel->x) < 1e-4f && m_abs(vel->z) < 1e-4f) {
                vel->y = 0.0f;
            } else {
                *vel = vec3_sub(*vel, vec3_scale(c.normal, vn));
            }
        }
    } else {
        if (c.normal.y < -0.7f) {
            res->hit_ceiling = TRUE;
        } else {
            res->hit_wall = TRUE;
        }
        if (c.depth > 0.0f) {
            push = vec3_scale(c.normal, c.depth);
        }
        f32 vn = vec3_dot(*vel, c.normal);
        if (vn < 0.0f) {
            *vel = vec3_sub(*vel, vec3_scale(c.normal, vn));
        }
    }

    if (sh->kind == SHAPE_CAPSULE) {
        sh->as.capsule.a = vec3_add(sh->as.capsule.a, push);
        sh->as.capsule.b = vec3_add(sh->as.capsule.b, push);
    } else if (sh->kind == SHAPE_SPHERE) {
        sh->as.sphere.center = vec3_add(sh->as.sphere.center, push);
    }
}

static SlideResult move_and_slide_shape(const PhysWorld* w, PhysShape* sh, f32 radius,
                                        Vec3* vel, f32 dt, f32 max_slope_cos) {
    SlideResult res;
    res.on_ground     = FALSE;
    res.hit_wall      = FALSE;
    res.hit_ceiling   = FALSE;
    res.ground_normal = vec3(0.0f, 1.0f, 0.0f);
    res.contact_count = 0u;

    if (!w || !sh || !vel || dt <= 0.0f) return res;

    f32 total_dist = vec3_len(*vel) * dt;
    f32 max_step = m_max(0.02f, radius * 0.4f);
    u32 steps = (total_dist > max_step) ? (u32)m_ceil(total_dist / max_step) : 1u;
    if (steps > MAX_SUBSTEPS) steps = MAX_SUBSTEPS;
    f32 sub_dt = dt / (f32)steps;

    for (u32 step = 0; step < steps; step++) {
        Vec3 d = vec3_scale(*vel, sub_dt);
        if (sh->kind == SHAPE_CAPSULE) {
            sh->as.capsule.a = vec3_add(sh->as.capsule.a, d);
            sh->as.capsule.b = vec3_add(sh->as.capsule.b, d);
        } else if (sh->kind == SHAPE_SPHERE) {
            sh->as.sphere.center = vec3_add(sh->as.sphere.center, d);
        }

        for (u32 iter = 0; iter < SOLVER_ITERS; iter++) {
            b32 any_deep = FALSE;

            for (u32 s = 0; s < w->static_count; s++) {
                Contact c;
                if (collide_shapes(sh, &w->statics[s].shape, &c)) {
                    if (c.depth > 1e-5f) any_deep = TRUE;
                    apply_slide_contact(sh, vel, c, max_slope_cos, &res);
                }
            }

            for (u32 m = 0; m < w->mesh_count; m++) {
                const Mesh* mesh = w->meshes[m].mesh;
                const Bvh*  bvh  = w->meshes[m].bvh;
                if (!mesh) continue;
                Aabb qbox = aabb_expand_margin(shape_bounds(sh), 1e-3f);

                if (bvh && bvh->node_count > 0u) {
                    u32 slots[BVH_QUERY_CAP];
                    u32 cnt = bvh_query_aabb(bvh, qbox, slots, BVH_QUERY_CAP);
                    for (u32 k = 0; k < cnt; k++) {
                        const BvhTri* bt = &bvh->tris[slots[k]];
                        Contact c;
                        if (collide_shape_tri(sh, bt->v0, bt->v1, bt->v2, &c)) {
                            if (c.depth > 1e-5f) any_deep = TRUE;
                            apply_slide_contact(sh, vel, c, max_slope_cos, &res);
                        }
                    }
                } else {
                    u32 tri_count = mesh->index_count / 3u;
                    for (u32 t = 0; t < tri_count; t++) {
                        u32 i0 = mesh->indices[t * 3u + 0u];
                        u32 i1 = mesh->indices[t * 3u + 1u];
                        u32 i2 = mesh->indices[t * 3u + 2u];
                        Vec3 v0 = vec3(mesh->px[i0], mesh->py[i0], mesh->pz[i0]);
                        Vec3 v1 = vec3(mesh->px[i1], mesh->py[i1], mesh->pz[i1]);
                        Vec3 v2 = vec3(mesh->px[i2], mesh->py[i2], mesh->pz[i2]);
                        if (!aabb_overlaps(qbox, aabb_for_triangle(v0, v1, v2))) continue;
                        Contact c;
                        if (collide_shape_tri(sh, v0, v1, v2, &c)) {
                            if (c.depth > 1e-5f) any_deep = TRUE;
                            apply_slide_contact(sh, vel, c, max_slope_cos, &res);
                        }
                    }
                }
            }

            if (!any_deep) break;
        }
    }

    return res;
}

SlideResult phys_move_and_slide(const PhysWorld* w, Capsule* cap, Vec3* vel,
                                f32 dt, f32 max_slope_cos) {
    PhysShape sh = shape_from_capsule(*cap);
    SlideResult res = move_and_slide_shape(w, &sh, cap->radius, vel, dt, max_slope_cos);
    *cap = sh.as.capsule;
    return res;
}

SlideResult phys_move_and_slide_sphere(const PhysWorld* w, Sphere* s, Vec3* vel,
                                       f32 dt, f32 max_slope_cos) {
    PhysShape sh = shape_from_sphere(*s);
    SlideResult res = move_and_slide_shape(w, &sh, s->radius, vel, dt, max_slope_cos);
    *s = sh.as.sphere;
    return res;
}

b32 phys_world_raycast(const PhysWorld* w, Ray r, f32 t_max, RayHit* out) {
    RayHit best;
    best.hit = FALSE;
    best.t = t_max;
    best.point = vec3(0.0f, 0.0f, 0.0f);
    best.normal = vec3(0.0f, 0.0f, 0.0f);
    best.tri_index = U32_MAX;

    for (u32 i = 0; i < w->static_count; i++) {
        RayHit h;
        if (raycast_shapes(r, &w->statics[i].shape, best.t, &h)) {
            if (!best.hit || h.t < best.t) best = h;
        }
    }

    for (u32 i = 0; i < w->mesh_count; i++) {
        RayHit h;
        if (raycast_mesh(r, w->meshes[i].mesh, w->meshes[i].bvh, best.t, &h)) {
            if (!best.hit || h.t < best.t) best = h;
        }
    }

    for (u32 i = 0; i < w->body_count; i++) {
        PhysShape bs = phys_body_shape(&w->bodies[i]);
        RayHit h;
        if (raycast_shapes(r, &bs, best.t, &h)) {
            if (!best.hit || h.t < best.t) best = h;
        }
    }

    if (out) *out = best;
    return best.hit;
}
