/* tests/test_phys.c — 3D çarpışma, ışın izleme, BVH ve fizik dünyası (M9_B) testleri.
 * Her çakışma çifti için analitik (temas yok / değiyor / iç içe) testler, simetri ve
 * 100k rastgele çift NaN fuzz testi, ışın/BVH-kaba kuvvet 10k birebir doğrulaması,
 * fizik dünyası (düşen küre, eğimde kapsül, 50 m/s karakter tünelleme) ve 100k üçgen
 * × 100k ışın BVH benchmark'ını içerir.
 */
#include <stdio.h>
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "plat/platform.h"
#include "gfx3d/mesh.h"
#include "phys/shapes.h"
#include "phys/collide.h"
#include "phys/bvh.h"
#include "phys/raycast.h"
#include "phys/world.h"

static u8 g_phys_mem[MB(32)];

static b32 is_nan_f32(f32 x) {
    return x != x;
}

static b32 is_valid_vec3(Vec3 v) {
    return !is_nan_f32(v.x) && !is_nan_f32(v.y) && !is_nan_f32(v.z);
}

static void check_vec3_near(Vec3 a, Vec3 b, f32 eps) {
    CHECK_NEAR(a.x, b.x, eps);
    CHECK_NEAR(a.y, b.y, eps);
    CHECK_NEAR(a.z, b.z, eps);
}

/* ============================================================================
 * 1. Analitik çakışma testleri (temas yok / değiyor / iç içe)
 * ============================================================================ */

TEST(phys_collide_sphere_sphere_analytic) {
    Contact c;
    Sphere a = sphere_make(vec3(0.0f, 0.0f, 0.0f), 1.0f);

    /* 1) Temas yok */
    Sphere b_miss = sphere_make(vec3(2.5f, 0.0f, 0.0f), 1.0f);
    CHECK(!collide_sphere_sphere(a, b_miss, &c));
    CHECK(!c.hit);

    /* 2) Değiyor (mesafe = 2.0, yarıçap toplamı = 2.0 -> derinlik 0) */
    Sphere b_touch = sphere_make(vec3(2.0f, 0.0f, 0.0f), 1.0f);
    CHECK(collide_sphere_sphere(a, b_touch, &c));
    CHECK(c.hit);
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(-1.0f, 0.0f, 0.0f), 1e-6f);

    /* 3) İç içe (mesafe = 1.5, derinlik = 0.5, normal B'den A'ya: (-1, 0, 0)) */
    Sphere b_pen = sphere_make(vec3(1.5f, 0.0f, 0.0f), 1.0f);
    CHECK(collide_sphere_sphere(a, b_pen, &c));
    CHECK(c.hit);
    CHECK_NEAR(c.depth, 0.5f, 1e-6f);
    check_vec3_near(c.normal, vec3(-1.0f, 0.0f, 0.0f), 1e-6f);
}

TEST(phys_collide_sphere_aabb_analytic) {
    Contact c;
    Aabb box = aabb_make(vec3(-1.0f, -1.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f));

    /* 1) Temas yok */
    Sphere s_miss = sphere_make(vec3(0.0f, 2.5f, 0.0f), 1.0f);
    CHECK(!collide_sphere_aabb(s_miss, box, &c));

    /* 2) Değiyor (y = 2.0, r = 1.0 -> üst yüze teğet) */
    Sphere s_touch = sphere_make(vec3(0.0f, 2.0f, 0.0f), 1.0f);
    CHECK(collide_sphere_aabb(s_touch, box, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    /* 3) İç içe (y = 1.4, r = 1.0 -> derinlik 0.6, normal +Y) */
    Sphere s_pen = sphere_make(vec3(0.0f, 1.4f, 0.0f), 1.0f);
    CHECK(collide_sphere_aabb(s_pen, box, &c));
    CHECK_NEAR(c.depth, 0.6f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    /* 4) Küre merkezi kutunun içinde (y = 0.8, en yakın yüz +Y, min_d = 0.2 -> derinlik 1.2) */
    Sphere s_in = sphere_make(vec3(0.0f, 0.8f, 0.0f), 1.0f);
    CHECK(collide_sphere_aabb(s_in, box, &c));
    CHECK_NEAR(c.depth, 1.2f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);
}

TEST(phys_collide_sphere_capsule_analytic) {
    Contact c;
    Capsule cap = capsule_make(vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), 0.5f);

    /* 1) Temas yok */
    Sphere s_miss = sphere_make(vec3(2.0f, 0.0f, 0.0f), 0.5f);
    CHECK(!collide_sphere_capsule(s_miss, cap, &c));

    /* 2) Değiyor (x = 1.0, r_sum = 1.0 -> derinlik 0) */
    Sphere s_touch = sphere_make(vec3(1.0f, 0.0f, 0.0f), 0.5f);
    CHECK(collide_sphere_capsule(s_touch, cap, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(1.0f, 0.0f, 0.0f), 1e-6f);

    /* 3) İç içe (x = 0.7, r_sum = 1.0 -> derinlik 0.3, normal +X) */
    Sphere s_pen = sphere_make(vec3(0.7f, 0.0f, 0.0f), 0.5f);
    CHECK(collide_sphere_capsule(s_pen, cap, &c));
    CHECK_NEAR(c.depth, 0.3f, 1e-6f);
    check_vec3_near(c.normal, vec3(1.0f, 0.0f, 0.0f), 1e-6f);
}

TEST(phys_collide_capsule_capsule_analytic) {
    Contact c;
    Capsule a = capsule_make(vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), 0.5f);

    /* 1) Temas yok */
    Capsule b_miss = capsule_make(vec3(0.0f, -1.0f, 1.5f), vec3(0.0f, 1.0f, 1.5f), 0.5f);
    CHECK(!collide_capsule_capsule(a, b_miss, &c));

    /* 2) Değiyor (z = 1.0, r_sum = 1.0 -> derinlik 0) */
    Capsule b_touch = capsule_make(vec3(0.0f, -1.0f, 1.0f), vec3(0.0f, 1.0f, 1.0f), 0.5f);
    CHECK(collide_capsule_capsule(a, b_touch, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 0.0f, -1.0f), 1e-6f);

    /* 3) İç içe (dik çapraz kapsüller: mesafe 0.6, r_sum = 1.0 -> derinlik 0.4) */
    Capsule b_pen = capsule_make(vec3(-1.0f, 0.0f, 0.6f), vec3(1.0f, 0.0f, 0.6f), 0.5f);
    CHECK(collide_capsule_capsule(a, b_pen, &c));
    CHECK_NEAR(c.depth, 0.4f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 0.0f, -1.0f), 1e-6f);
}

TEST(phys_collide_aabb_aabb_analytic) {
    Contact c;
    Aabb a = aabb_make(vec3(-1.0f, -1.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f));

    /* 1) Temas yok */
    Aabb b_miss = aabb_make(vec3(1.5f, -1.0f, -1.0f), vec3(3.5f, 1.0f, 1.0f));
    CHECK(!collide_aabb_aabb(a, b_miss, &c));

    /* 2) Değiyor (a.max.x == b.min.x == 1.0 -> ox = 0) */
    Aabb b_touch = aabb_make(vec3(1.0f, -1.0f, -1.0f), vec3(3.0f, 1.0f, 1.0f));
    CHECK(collide_aabb_aabb(a, b_touch, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(-1.0f, 0.0f, 0.0f), 1e-6f);

    /* 3) İç içe (b.min.x = 0.6 -> ox = 0.4, normal B'den A'ya: (-1, 0, 0)) */
    Aabb b_pen = aabb_make(vec3(0.6f, -1.0f, -1.0f), vec3(2.6f, 1.0f, 1.0f));
    CHECK(collide_aabb_aabb(a, b_pen, &c));
    CHECK_NEAR(c.depth, 0.4f, 1e-6f);
    check_vec3_near(c.normal, vec3(-1.0f, 0.0f, 0.0f), 1e-6f);
}

TEST(phys_collide_plane_pairs_analytic) {
    Contact c;
    Plane ground = plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f);

    /* Küre - Düzlem: temas yok / değiyor / iç içe */
    CHECK(!collide_sphere_plane(sphere_make(vec3(0.0f, 1.5f, 0.0f), 1.0f), ground, &c));

    CHECK(collide_sphere_plane(sphere_make(vec3(0.0f, 1.0f, 0.0f), 1.0f), ground, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    CHECK(collide_sphere_plane(sphere_make(vec3(0.0f, 0.7f, 0.0f), 1.0f), ground, &c));
    CHECK_NEAR(c.depth, 0.3f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    /* Kapsül - Düzlem: temas yok / değiyor / iç içe */
    CHECK(!collide_capsule_plane(capsule_make(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 2.0f, 0.0f), 0.5f), ground, &c));

    CHECK(collide_capsule_plane(capsule_make(vec3(0.0f, 0.5f, 0.0f), vec3(0.0f, 1.5f, 0.0f), 0.5f), ground, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    CHECK(collide_capsule_plane(capsule_make(vec3(0.0f, 0.2f, 0.0f), vec3(0.0f, 1.2f, 0.0f), 0.5f), ground, &c));
    CHECK_NEAR(c.depth, 0.3f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    /* AABB - Düzlem: temas yok / değiyor / iç içe */
    CHECK(!collide_aabb_plane(aabb_make(vec3(-1.0f, 0.2f, -1.0f), vec3(1.0f, 1.2f, 1.0f)), ground, &c));

    CHECK(collide_aabb_plane(aabb_make(vec3(-1.0f, 0.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f)), ground, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    CHECK(collide_aabb_plane(aabb_make(vec3(-1.0f, -0.25f, -1.0f), vec3(1.0f, 0.75f, 1.0f)), ground, &c));
    CHECK_NEAR(c.depth, 0.25f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 1.0f, 0.0f), 1e-6f);

    /* Kapsül - AABB: temas yok / değiyor / iç içe */
    Aabb box = aabb_make(vec3(-1.0f, -1.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f));
    CHECK(!collide_capsule_aabb(capsule_make(vec3(2.0f, -1.0f, 0.0f), vec3(2.0f, 1.0f, 0.0f), 0.5f), box, &c));
    CHECK(collide_capsule_aabb(capsule_make(vec3(1.5f, -1.0f, 0.0f), vec3(1.5f, 1.0f, 0.0f), 0.5f), box, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(1.0f, 0.0f, 0.0f), 1e-6f);
    CHECK(collide_capsule_aabb(capsule_make(vec3(1.2f, -1.0f, 0.0f), vec3(1.2f, 1.0f, 0.0f), 0.5f), box, &c));
    CHECK_NEAR(c.depth, 0.3f, 1e-6f);
    check_vec3_near(c.normal, vec3(1.0f, 0.0f, 0.0f), 1e-6f);

    /* Küre - OBB: temas yok / değiyor / iç içe */
    Obb obb = obb_make(vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), mat3_identity());
    CHECK(!collide_sphere_obb(sphere_make(vec3(0.0f, 0.0f, 2.5f), 1.0f), obb, &c));
    CHECK(collide_sphere_obb(sphere_make(vec3(0.0f, 0.0f, 2.0f), 1.0f), obb, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 0.0f, 1.0f), 1e-6f);
    CHECK(collide_sphere_obb(sphere_make(vec3(0.0f, 0.0f, 1.6f), 1.0f), obb, &c));
    CHECK_NEAR(c.depth, 0.4f, 1e-6f);
    check_vec3_near(c.normal, vec3(0.0f, 0.0f, 1.0f), 1e-6f);
}

/* ============================================================================
 * 2. Simetri ve 100.000 rastgele çift NaN fuzz testi
 * ============================================================================ */

static Vec3 rand_vec3(Rng* rng, f32 scale) {
    return vec3(
        (rng_f32(rng) * 2.0f - 1.0f) * scale,
        (rng_f32(rng) * 2.0f - 1.0f) * scale,
        (rng_f32(rng) * 2.0f - 1.0f) * scale
    );
}

static PhysShape rand_shape(Rng* rng) {
    u32 kind = rng_u32(rng) % 4u;
    if (kind == 0u) {
        return shape_from_sphere(sphere_make(rand_vec3(rng, 2.0f), 0.05f + rng_f32(rng) * 1.5f));
    } else if (kind == 1u) {
        Vec3 c = rand_vec3(rng, 2.0f);
        Vec3 h = vec3(0.05f + rng_f32(rng) * 1.2f,
                      0.05f + rng_f32(rng) * 1.2f,
                      0.05f + rng_f32(rng) * 1.2f);
        return shape_from_aabb(aabb_from_center_half(c, h));
    } else if (kind == 2u) {
        return shape_from_capsule(capsule_make(rand_vec3(rng, 2.0f),
                                               rand_vec3(rng, 2.0f),
                                               0.05f + rng_f32(rng) * 1.2f));
    } else {
        return shape_from_plane(plane_make(rand_vec3(rng, 1.0f), (rng_f32(rng) * 2.0f - 1.0f) * 1.5f));
    }
}

TEST(phys_collide_symmetry_and_100k_fuzz) {
    Rng rng;
    rng_seed(&rng, 0xC0111DEu);

    u32 hit_count = 0u;
    u32 bad_nan = 0u;
    u32 bad_sym = 0u;

    for (u32 i = 0; i < 100000u; i++) {
        PhysShape a = rand_shape(&rng);
        PhysShape b = rand_shape(&rng);
        /* Düzlem-düzlem çifti yerine en az birinin hacimli şekil olmasını sağla */
        if (a.kind == SHAPE_PLANE && b.kind == SHAPE_PLANE) {
            a = shape_from_sphere(sphere_make(rand_vec3(&rng, 2.0f), 0.1f + rng_f32(&rng)));
        }

        Contact cab, cba;
        b32 hab = collide_shapes(&a, &b, &cab);
        b32 hba = collide_shapes(&b, &a, &cba);

        if (!is_valid_vec3(cab.normal) || !is_valid_vec3(cab.point) || is_nan_f32(cab.depth) ||
            !is_valid_vec3(cba.normal) || !is_valid_vec3(cba.point) || is_nan_f32(cba.depth)) {
            bad_nan++;
            continue;
        }

        if (hab != hba) {
            bad_sym++;
            continue;
        }

        if (hab) {
            hit_count++;
            f32 n_len = vec3_len(cab.normal);
            f32 d_diff = m_abs(cab.depth - cba.depth);
            Vec3 n_sum = vec3_add(cab.normal, cba.normal);
            if (cab.depth < 0.0f || m_abs(n_len - 1.0f) > 1e-4f ||
                d_diff > 1e-5f || vec3_len(n_sum) > 1e-5f) {
                bad_sym++;
            }
        }
    }

    CHECK(bad_nan == 0u);
    CHECK(bad_sym == 0u);
    CHECK(hit_count > 10000u);
}

/* ============================================================================
 * 3. Işın izleme (Ray) ve BVH doğrulama testleri
 * ============================================================================ */

TEST(phys_raycast_primitives) {
    RayHit h;

    /* --- Küre: kaçırma, teğet, içeriden başlayan, normal --- */
    Sphere sph = sphere_make(vec3(0.0f, 0.0f, 0.0f), 1.0f);
    CHECK(!raycast_sphere(ray_make(vec3(-3.0f, 2.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), sph, 100.0f, &h));

    /* Teğet (y = 1.0) */
    CHECK(raycast_sphere(ray_make(vec3(-3.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), sph, 100.0f, &h));
    CHECK_NEAR(h.t, 3.0f, 1e-5f);
    check_vec3_near(h.normal, vec3(0.0f, 1.0f, 0.0f), 1e-5f);

    /* İçeriden başlayan ışın (origin = (0,0,0) -> t = 1.0 çıkış noktası) */
    CHECK(raycast_sphere(ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), sph, 100.0f, &h));
    CHECK_NEAR(h.t, 1.0f, 1e-5f);
    check_vec3_near(h.point, vec3(1.0f, 0.0f, 0.0f), 1e-5f);

    /* --- AABB: kaçırma, teğet, içeriden başlayan --- */
    Aabb box = aabb_make(vec3(-1.0f, -1.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f));
    CHECK(!raycast_aabb(ray_make(vec3(-3.0f, 2.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), box, 100.0f, &h));

    /* Teğet (y = 1.0 üst yüz boyunca) */
    CHECK(raycast_aabb(ray_make(vec3(-3.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), box, 100.0f, &h));
    CHECK_NEAR(h.t, 2.0f, 1e-5f);

    /* İçeriden başlayan */
    CHECK(raycast_aabb(ray_make(vec3(0.2f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)), box, 100.0f, &h));
    CHECK_NEAR(h.t, 0.8f, 1e-5f);
    check_vec3_near(h.normal, vec3(1.0f, 0.0f, 0.0f), 1e-5f);

    /* --- Kapsül: kaçırma, teğet, içeriden başlayan --- */
    Capsule cap = capsule_make(vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), 1.0f);
    CHECK(!raycast_capsule(ray_make(vec3(-3.0f, 0.0f, 2.0f), vec3(1.0f, 0.0f, 0.0f)), cap, 100.0f, &h));

    /* Teğet (z = 1.0) */
    CHECK(raycast_capsule(ray_make(vec3(-3.0f, 0.0f, 1.0f), vec3(1.0f, 0.0f, 0.0f)), cap, 100.0f, &h));
    CHECK_NEAR(h.t, 3.0f, 1e-4f);

    /* İçeriden başlayan (eksen boyunca yukarı -> üst kapak y = 2.0) */
    CHECK(raycast_capsule(ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f)), cap, 100.0f, &h));
    CHECK_NEAR(h.t, 2.0f, 1e-5f);
    check_vec3_near(h.normal, vec3(0.0f, 1.0f, 0.0f), 1e-5f);

    /* --- Düzlem & Üçgen ve t sıralaması --- */
    Plane pl = plane_make(vec3(0.0f, 0.0f, -1.0f), -5.0f); /* z = 5 */
    CHECK(raycast_plane(ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)), pl, 100.0f, &h));
    CHECK_NEAR(h.t, 5.0f, 1e-5f);

    Vec3 v0 = vec3(-1.0f, -1.0f, 3.0f);
    Vec3 v1 = vec3( 1.0f, -1.0f, 3.0f);
    Vec3 v2 = vec3( 0.0f,  1.0f, 3.0f);
    CHECK(raycast_triangle(ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)), v0, v1, v2, 100.0f, &h));
    CHECK_NEAR(h.t, 3.0f, 1e-5f);
    CHECK(!raycast_triangle(ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)), v0, v1, v2, 2.0f, &h));

    /* t sıralaması: dünyada birden fazla şekil varken en yakın olanı seçer */
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));
    PhysWorld w;
    phys_world_init(&w, &a, 4u, 4u, 0u);
    phys_world_add_static(&w, shape_from_sphere(sphere_make(vec3(0.0f, 0.0f, 8.0f), 1.0f)), 0.5f, 0.5f);
    phys_world_add_static(&w, shape_from_sphere(sphere_make(vec3(0.0f, 0.0f, 4.0f), 1.0f)), 0.5f, 0.5f);
    phys_world_add_static(&w, shape_from_sphere(sphere_make(vec3(0.0f, 0.0f, 6.0f), 1.0f)), 0.5f, 0.5f);
    CHECK(phys_world_raycast(&w, ray_make(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)), 100.0f, &h));
    CHECK_NEAR(h.t, 3.0f, 1e-5f);
}

TEST(phys_raycast_mesh_bvh_vs_brute_10k) {
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));

    /* ~500 üçgenlik küre ağı üzerinde 10.000 rastgele ışın: BVH ve kaba kuvvet birebir aynı olmalı */
    Mesh* m = mesh_sphere(&a, 2.0f, 16u, 16u);
    Bvh* bvh = bvh_build(&a, m);

    Rng rng;
    rng_seed(&rng, 0x12345678u);

    u32 mismatch = 0u;
    u32 hits = 0u;
    for (u32 i = 0; i < 10000u; i++) {
        Vec3 orig = rand_vec3(&rng, 4.0f);
        Vec3 dir  = rand_vec3(&rng, 1.0f);
        if (vec3_len(dir) < 1e-4f) dir = vec3(1.0f, 0.0f, 0.0f);
        Ray r = ray_make(orig, dir);

        RayHit hb, hk;
        b32 ok_bvh   = raycast_mesh(r, m, bvh, 50.0f, &hb);
        b32 ok_brute = raycast_mesh(r, m, NULL, 50.0f, &hk);

        if (ok_bvh != ok_brute) {
            mismatch++;
            continue;
        }
        if (ok_bvh) {
            hits++;
            if (hb.tri_index != hk.tri_index || m_abs(hb.t - hk.t) > 1e-6f) {
                mismatch++;
            }
        }
    }

    CHECK(mismatch == 0u);
    CHECK(hits > 1000u);
}

/* ============================================================================
 * 4. Fizik dünyası (PhysWorld) ve karakter kontrolcüsü testleri
 * ============================================================================ */

TEST(phys_world_falling_sphere_settles) {
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));

    PhysWorld w;
    phys_world_init(&w, &a, 8u, 8u, 0u);

    Plane ground = plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f);
    phys_world_add_static(&w, shape_from_plane(ground), 0.3f, 0.6f);

    f32 radius = 0.5f;
    u32 sid = phys_world_add_sphere(&w, vec3(0.0f, 3.0f, 0.0f), radius, 1.0f, 0.3f, 0.6f);

    /* Küreyi düşür ve durmasını bekle (300 adım = 2.5 s) */
    for (u32 i = 0; i < 300u; i++) {
        phys_world_step(&w);
    }

    Vec3 settled_pos = w.bodies[sid].pos;
    CHECK(w.bodies[sid].on_ground);
    CHECK_NEAR(settled_pos.y, radius, 1e-4f);

    /* 1000 adım daha koş: derinlik ~0 kalmalı ve kayma < 1e-3 olmalı */
    for (u32 i = 0; i < 1000u; i++) {
        phys_world_step(&w);
    }

    Vec3 final_pos = w.bodies[sid].pos;
    Contact c;
    CHECK(collide_sphere_plane(phys_body_sphere(&w.bodies[sid]), ground, &c));
    CHECK_NEAR(c.depth, 0.0f, 1e-4f);
    CHECK(vec3_len(vec3_sub(final_pos, settled_pos)) < 1e-3f);
}

TEST(phys_world_capsule_slope_limit) {
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));

    f32 max_slope_cos = m_cos(45.0f * M_DEG2RAD);

    /* 1) Eğim sınırı ALTINDA (20° < 45°): kapsül kaymamalı */
    {
        PhysWorld w;
        phys_world_init(&w, &a, 4u, 4u, 0u);
        f32 ang = 20.0f * M_DEG2RAD;
        Vec3 n_gentle = vec3(m_sin(ang), m_cos(ang), 0.0f);
        Plane slope_gentle = plane_make(n_gentle, 0.0f);
        phys_world_add_static(&w, shape_from_plane(slope_gentle), 0.0f, 0.8f);

        f32 r = 0.4f;
        Vec3 base = vec3_scale(n_gentle, r);
        Capsule cap = capsule_make(base, vec3_add(base, vec3(0.0f, 1.0f, 0.0f)), r);
        Vec3 start_a = cap.a;
        Vec3 vel = vec3(0.0f, 0.0f, 0.0f);

        SlideResult sr;
        sr.on_ground = FALSE;
        for (u32 i = 0; i < 120u; i++) {
            vel = vec3_add(vel, vec3_scale(w.gravity, PHYS_FIXED_DT));
            sr = phys_move_and_slide(&w, &cap, &vel, PHYS_FIXED_DT, max_slope_cos);
        }
        CHECK(sr.on_ground);
        CHECK(m_abs(cap.a.x - start_a.x) < 1e-4f);

        /* Dinamik kapsül gövdesi ile de doğrula */
        u32 cid = phys_world_add_capsule(&w, start_a, vec3_add(start_a, vec3(0.0f, 1.0f, 0.0f)),
                                         r, 1.0f, 0.0f, 0.8f);
        w.bodies[cid].max_slope_cos = max_slope_cos;
        Vec3 body_start = w.bodies[cid].pos;
        for (u32 i = 0; i < 120u; i++) {
            phys_world_step(&w);
        }
        CHECK(w.bodies[cid].on_ground);
        CHECK(m_abs(w.bodies[cid].pos.x - body_start.x) < 1e-4f);
    }

    /* 2) Eğim sınırı ÜSTÜNDE (60° > 45°): kapsül aşağı kaymalı */
    {
        PhysWorld w;
        phys_world_init(&w, &a, 4u, 4u, 0u);
        f32 ang = 60.0f * M_DEG2RAD;
        Vec3 n_steep = vec3(m_sin(ang), m_cos(ang), 0.0f);
        Plane slope_steep = plane_make(n_steep, 0.0f);
        phys_world_add_static(&w, shape_from_plane(slope_steep), 0.0f, 0.8f);

        f32 r = 0.4f;
        Vec3 base = vec3_scale(n_steep, r);
        Capsule cap = capsule_make(base, vec3_add(base, vec3(0.0f, 1.0f, 0.0f)), r);
        Vec3 start_a = cap.a;
        Vec3 vel = vec3(0.0f, 0.0f, 0.0f);

        SlideResult sr;
        sr.on_ground = TRUE;
        for (u32 i = 0; i < 120u; i++) {
            vel = vec3_add(vel, vec3_scale(w.gravity, PHYS_FIXED_DT));
            sr = phys_move_and_slide(&w, &cap, &vel, PHYS_FIXED_DT, max_slope_cos);
        }
        CHECK(!sr.on_ground);
        CHECK(cap.a.x > start_a.x + 0.5f);
        CHECK(cap.a.y < start_a.y - 0.5f);

        /* Dinamik kapsül gövdesi ile de doğrula */
        u32 cid = phys_world_add_capsule(&w, start_a, vec3_add(start_a, vec3(0.0f, 1.0f, 0.0f)),
                                         r, 1.0f, 0.0f, 0.8f);
        w.bodies[cid].max_slope_cos = max_slope_cos;
        Vec3 body_start = w.bodies[cid].pos;
        for (u32 i = 0; i < 120u; i++) {
            phys_world_step(&w);
        }
        CHECK(!w.bodies[cid].on_ground);
        CHECK(w.bodies[cid].pos.x > body_start.x + 0.5f);
        CHECK(w.bodies[cid].pos.y < body_start.y - 0.5f);
    }
}

TEST(phys_world_character_no_tunneling_50ms) {
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));

    PhysWorld w;
    phys_world_init(&w, &a, 4u, 4u, 4u);

    /* Zemin (y = 0) ve x = 2.0'da kağıt inceliğinde (0.04 m) dikey duvar AABB'si */
    phys_world_add_static(&w, shape_from_plane(plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f)), 0.0f, 0.5f);
    Aabb thin_wall = aabb_make(vec3(2.0f, -1.0f, -5.0f), vec3(2.04f, 5.0f, 5.0f));
    phys_world_add_static(&w, shape_from_aabb(thin_wall), 0.0f, 0.5f);

    /* x = 0'dan +X yönünde 50 m/s hızla duvara koşan karakter kapsülü (r = 0.35 m) */
    f32 r = 0.35f;
    Capsule cap = capsule_make(vec3(0.0f, r, 0.0f), vec3(0.0f, r + 1.0f, 0.0f), r);
    f32 max_slope_cos = m_cos(45.0f * M_DEG2RAD);

    for (u32 i = 0; i < 60u; i++) {
        Vec3 vel = vec3(50.0f, -9.81f * PHYS_FIXED_DT, 0.0f);
        SlideResult sr = phys_move_and_slide(&w, &cap, &vel, PHYS_FIXED_DT, max_slope_cos);
        UNUSED(sr);
        /* Karakter hiçbir adımda duvarın (x = 2.0) arkasına geçmemeli */
        CHECK(cap.a.x <= 2.0f - r + 1e-3f);
    }
    CHECK_NEAR(cap.a.x, 2.0f - r, 1e-2f);

    /* Ayrıca statik üçgen ağı (Mesh + BVH) duvarına 50 m/s hızla koşan karakter testi */
    PhysWorld wm;
    phys_world_init(&wm, &a, 4u, 4u, 4u);
    Mesh* wall_mesh = mesh_cube(&a, 1.5f); /* [-1.5, 1.5]^3 küp duvar */
    for (u32 vi = 0; vi < wall_mesh->vertex_count; vi++) {
        wall_mesh->px[vi] += 4.0f; /* x in [2.5, 5.5] */
    }
    mesh_compute_bounds(wall_mesh);
    Bvh* wall_bvh = bvh_build(&a, wall_mesh);
    phys_world_add_mesh(&wm, wall_mesh, wall_bvh, 0.0f, 0.5f);

    Capsule cap2 = capsule_make(vec3(0.0f, -0.5f, 0.0f), vec3(0.0f, 0.5f, 0.0f), r);
    for (u32 i = 0; i < 60u; i++) {
        Vec3 vel = vec3(50.0f, 0.0f, 0.0f);
        SlideResult sr = phys_move_and_slide(&wm, &cap2, &vel, PHYS_FIXED_DT, max_slope_cos);
        UNUSED(sr);
        CHECK(cap2.a.x <= 2.5f - r + 1e-3f);
    }
    CHECK_NEAR(cap2.a.x, 2.5f - r, 1e-2f);
}

/* ============================================================================
 * 5. Benchmark: 100k üçgenlik mesh_sphere üzerinde 100k ışın
 * ============================================================================ */

TEST(phys_bvh_benchmark_100k) {
    Arena a;
    arena_init(&a, g_phys_mem, sizeof(g_phys_mem));

    /* 2 * 250 * 200 = 100.000 üçgen */
    Mesh* m = mesh_sphere(&a, 5.0f, 250u, 200u);
    u32 tri_count = m->index_count / 3u;
    CHECK(tri_count == 100000u);

    f64 t0 = plat_time_ms();
    Bvh* bvh = bvh_build(&a, m);
    f64 t1 = plat_time_ms();
    f64 build_ms = t1 - t0;

    CHECK(bvh->tri_count == 100000u);
    CHECK(bvh->node_count > 1000u);

    /* 100.000 ışını önceden hazırla (RNG maliyeti ışın ölçümüne karışmasın) */
    u32 ray_count = 100000u;
    Ray* rays = ARENA_PUSH_ARRAY(&a, Ray, ray_count);
    Rng rng;
    rng_seed(&rng, 0xBEEF2026u);
    for (u32 i = 0; i < ray_count; i++) {
        Vec3 orig = rand_vec3(&rng, 10.0f);
        /* Işınların çoğunu küreye doğru, bir kısmını rastgele yönlendir */
        Vec3 target = rand_vec3(&rng, 4.0f);
        Vec3 dir = vec3_sub(target, orig);
        if (vec3_len(dir) < 1e-4f) dir = vec3(1.0f, 0.0f, 0.0f);
        rays[i] = ray_make(orig, dir);
    }

    u32 hits = 0u;
    f64 r0 = plat_time_ms();
    for (u32 i = 0; i < ray_count; i++) {
        RayHit h;
        if (raycast_mesh(rays[i], m, bvh, 100.0f, &h)) {
            hits++;
        }
    }
    f64 r1 = plat_time_ms();
    f64 ray_ms = r1 - r0;
    f64 rays_per_sec = (ray_ms > 0.001) ? ((f64)ray_count / (ray_ms * 0.001)) : 0.0;

    /* Kaba kuvvet karşılaştırması: aynı 100k üçgenlik ağ üzerinde ilk 200 ışın */
    u32 brute_count = 200u;
    u32 brute_hits = 0u;
    f64 b0 = plat_time_ms();
    for (u32 i = 0; i < brute_count; i++) {
        RayHit h;
        if (raycast_mesh_brute(rays[i], m, 100.0f, &h)) {
            brute_hits++;
        }
    }
    f64 b1 = plat_time_ms();
    f64 brute_ms = b1 - b0;
    f64 brute_rps = (brute_ms > 0.001) ? ((f64)brute_count / (brute_ms * 0.001)) : 0.0;
    f64 speedup = (brute_rps > 0.0) ? (rays_per_sec / brute_rps) : 0.0;

    printf("  [bench M9_B] 100k ucgen BVH kurulum: %.2f ms (dugum: %u) | "
           "100k isin BVH: %.2f ms (%.2f M isin/s, isabet: %u/%u) | "
           "kaba kuvvet: %.0f isin/s (%.0fx hizlanma)\n",
           build_ms, bvh->node_count, ray_ms, rays_per_sec / 1e6, hits, ray_count,
           brute_rps, speedup);

    CHECK(hits > 50000u);
    CHECK(brute_hits > 100u);
}
