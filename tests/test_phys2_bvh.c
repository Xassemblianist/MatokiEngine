/* tests/test_phys2_bvh.c — M9 Faz 7: dinamik AABB ağacı ve dünya BVH'si.
 * Yapı: rastgele ekle / taşı / çıkar sonrası ağaç tutarlı, sorgu = kaba kuvvet, yükseklik ~ log n.
 * Eşdeğerlik: aynı sahne SAP (doğrusal raycast / CCD / yumuşak aday) ve BVH ile → durum hash'i HER adımda
 * aynı (çiftler kanonik sıralı). Raycast: iki yolda aynı gövde, t ve normal (bit-bit).
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/pmath.h"
#include "phys2/bvh.h"
#include "phys2/phys.h"
#include "phys2/shapes.h"
#include "phys2/soft.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* world(u32 n, Phys2Broadphase bp) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 16 + 64);
    w->broadphase = bp;
    return w;
}

TEST(phys2_bvh_tree_structure_and_query) {
    test_arena_reset();
    Phys2DynTree t;
    const u32 N = 1500;
    phys2_tree_init(&t, test_arena(), N * 2, 0.1f);
    static u32 proxy[1500];
    static Vec3 mn[1500], mx[1500];
    static b32 live[1500];
    Phys2Rng r = phys2_rng_make(17);
    for (u32 i = 0; i < N; i++) {
        Vec3 c = vec3(phys2_rng_range(&r, -50, 50), phys2_rng_range(&r, -5, 20), phys2_rng_range(&r, -50, 50));
        Vec3 e = vec3(phys2_rng_range(&r, 0.1f, 2), phys2_rng_range(&r, 0.1f, 2), phys2_rng_range(&r, 0.1f, 2));
        mn[i] = vec3_sub(c, e);
        mx[i] = vec3_add(c, e);
        proxy[i] = phys2_tree_insert(&t, mn[i], mx[i], i);
        live[i] = TRUE;
    }
    CHECK(phys2_tree_validate(&t));
    test_note("1500 yaprak: yükseklik", phys2_tree_height(&t), 0);
    test_note("alan oranı (iç düğüm alanı / kök)", phys2_tree_area_ratio(&t), 2);
    u32 moved = 0;
    for (u32 it = 0; it < 3000; it++) {
        u32 i = phys2_rng_u32(&r) % N;
        u32 op = phys2_rng_u32(&r) % 10;
        if (op == 0 && live[i]) {
            phys2_tree_remove(&t, proxy[i]);
            live[i] = FALSE;
        } else if (op == 1 && !live[i]) {
            proxy[i] = phys2_tree_insert(&t, mn[i], mx[i], i);
            live[i] = TRUE;
        } else if (live[i]) {
            Vec3 d = vec3(phys2_rng_range(&r, -0.3f, 0.3f), phys2_rng_range(&r, -0.3f, 0.3f), phys2_rng_range(&r, -0.3f, 0.3f));
            mn[i] = vec3_add(mn[i], d);
            mx[i] = vec3_add(mx[i], d);
            moved += phys2_tree_move(&t, proxy[i], mn[i], mx[i]);
        }
    }
    CHECK(phys2_tree_validate(&t));
    test_note("3000 işlem sonrası yükseklik", phys2_tree_height(&t), 0);
    test_note("yeniden eklenen (şişman kutudan taşan)", moved, 0);
    /* sorgu: şişman kutular ⊇ sıkı kutular → kaba kuvvetle bulunan her sıkı kesişim ağaçta da var */
    static u32 got[1500];
    u32 missing = 0, extra_tight = 0;
    for (u32 q = 0; q < 300; q++) {
        Vec3 c = vec3(phys2_rng_range(&r, -50, 50), phys2_rng_range(&r, -5, 20), phys2_rng_range(&r, -50, 50));
        Vec3 e = vec3(phys2_rng_range(&r, 0.5f, 8), phys2_rng_range(&r, 0.5f, 8), phys2_rng_range(&r, 0.5f, 8));
        Vec3 qmn = vec3_sub(c, e), qmx = vec3_add(c, e);
        u32 n = phys2_tree_query(&t, qmn, qmx, got, 1500);
        for (u32 i = 0; i < N; i++) {
            b32 hit = live[i] && mn[i].x <= qmx.x && mx[i].x >= qmn.x && mn[i].y <= qmx.y && mx[i].y >= qmn.y &&
                      mn[i].z <= qmx.z && mx[i].z >= qmn.z;
            b32 found = FALSE;
            for (u32 k = 0; k < n && !found; k++) found = got[k] == i;
            if (hit && !found) missing++;
            if (found && !live[i]) extra_tight++;
        }
    }
    test_note("sorgu: kaçırılan", missing, 0);
    CHECK(missing == 0 && extra_tight == 0);
}

/* ---- SAP ↔ BVH eşdeğerliği ---- */

static const Phys2ConvexHull* g_rock;
static Phys2TriMesh* g_mesh;

static void build(Phys2World* w, u32 scene) {
    Phys2Rng r = phys2_rng_make(900 + scene);
    if (scene == 1) {
        Phys2BodyDesc m = phys2_desc_mesh(vec3(0, 0, 0), g_mesh);
        phys2_add_body(w, &m);
    } else {
        Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
        phys2_add_body(w, &g);
    }
    if (scene == 0) {          /* düşen yığın + uyku */
        for (u32 i = 0; i < 150; i++) {
            Phys2BodyDesc b = phys2_desc_box(vec3(phys2_rng_range(&r, -3, 3), 0.6f + (f32)i * 0.3f, phys2_rng_range(&r, -3, 3)), vec3(0.4f, 0.3f, 0.5f), 1.0f);
            b.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(&r, -1, 1), 1, phys2_rng_range(&r, -1, 1)), phys2_rng_range(&r, -1, 1));
            phys2_add_body(w, &b);
        }
    } else if (scene == 1) {   /* karışık şekiller mesh üstünde + eklem zinciri */
        for (u32 i = 0; i < 60; i++) {
            Vec3 p = vec3(phys2_rng_range(&r, -5, 5), 2.0f + (f32)i * 0.4f, phys2_rng_range(&r, -5, 5));
            Phys2BodyDesc d;
            if (i % 4 == 0) d = phys2_desc_capsule(p, 0.25f, 0.4f, 1.0f);
            else if (i % 4 == 1) d = phys2_desc_hull(p, g_rock, 2.0f);
            else if (i % 4 == 2) d = phys2_desc_sphere(p, 0.35f, 1.0f);
            else d = phys2_desc_box(p, vec3(0.3f, 0.4f, 0.3f), 1.0f);
            d.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 1), 1), phys2_rng_range(&r, -2, 2));
            phys2_add_body(w, &d);
        }
        u32 prev = PHYS2_NO_BODY;
        for (u32 i = 0; i < 6; i++) {
            Phys2BodyDesc c = phys2_desc_box(vec3(-3.0f + (f32)i * 0.6f, 5.0f, 6.0f), vec3(0.28f, 0.1f, 0.2f), 1.0f);
            u32 id = phys2_add_body(w, &c);
            Phys2JointDesc j = phys2_joint_desc(PHYS2_JOINT_BALL, prev, id, vec3(-3.3f + (f32)i * 0.6f, 5.0f, 6.0f));
            phys2_add_joint(w, &j);
            prev = id;
        }
    } else {                   /* şehir: statik bloklar, düşen kutular, mermiler (CCD, PHYS2_BODY_BULLET) */
        for (u32 z = 0; z < 12; z++) {
            for (u32 x = 0; x < 12; x++) {
                f32 h = phys2_rng_range(&r, 1, 6);
                Phys2BodyDesc s = phys2_desc_box(vec3((f32)x * 4.0f - 22, h * 0.5f, (f32)z * 4.0f - 22), vec3(1.5f, h * 0.5f, 1.5f), 0.0f);
                phys2_add_body(w, &s);
            }
        }
        for (u32 i = 0; i < 80; i++) {
            Phys2BodyDesc b = phys2_desc_box(vec3(phys2_rng_range(&r, -22, 22), 8.0f + (f32)i * 0.2f, phys2_rng_range(&r, -22, 22)), vec3(0.3f, 0.3f, 0.3f), 1.0f);
            phys2_add_body(w, &b);
        }
        for (u32 i = 0; i < 12; i++) {
            Phys2BodyDesc s = phys2_desc_sphere(vec3(-30, 2.0f + (f32)i * 0.4f, phys2_rng_range(&r, -20, 20)), 0.1f, 0.2f);
            s.vel = vec3(250.0f, phys2_rng_range(&r, -5, 5), phys2_rng_range(&r, -20, 20));
            u32 id = phys2_add_body(w, &s);
            if (i % 2) w->bodies[id].flags |= PHYS2_BODY_BULLET;
        }
    }
}

static u32 first_mismatch(u32 scene, b32 sleep, u32 steps, b32 soft) {
    Phys2World* a = world(512, PHYS2_BP_SAP);
    Phys2World* b = world(512, PHYS2_BP_BVH);
    a->allow_sleep = b->allow_sleep = sleep;
    build(a, scene);
    build(b, scene);
    Phys2SoftWorld* sa = NULL;
    Phys2SoftWorld* sb = NULL;
    if (soft) {
        sa = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
        sb = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
        phys2_soft_world_init(sa, test_arena(), 4);
        phys2_soft_world_init(sb, test_arena(), 4);
        Phys2SoftWorld* both[2] = { sa, sb };
        for (u32 k = 0; k < 2; k++) {
            phys2_soft_world_add(both[k], phys2_soft_jelly(test_arena(), vec3(0.5f, 6.0f, 0.2f), 1.0f, 3, 3.0f, 2e-4f, 0.0f));
            phys2_soft_world_add(both[k], phys2_soft_cloth(test_arena(), vec3(-2, 8, -2), vec3(4, 0, 0), vec3(0, 0, 4), 12, 12, 0.4f, 0.0f, 1e-4f, 0.01f));
        }
    }
    for (u32 s = 1; s <= steps; s++) {
        phys2_step(a, DT);
        phys2_step(b, DT);
        if (soft) {
            phys2_soft_world_step(sa, a, DT);
            phys2_soft_world_step(sb, b, DT);
            if (phys2_soft_hash(sa) != phys2_soft_hash(sb)) return s;
        }
        if (phys2_state_hash(a) != phys2_state_hash(b)) return s;
    }
    return 0;
}

TEST(phys2_bvh_equals_sap) {
    test_arena_reset();
    Vec3 pts[12];
    Phys2Rng r = phys2_rng_make(4);
    for (u32 i = 0; i < 12; i++) pts[i] = vec3(phys2_rng_range(&r, -0.5f, 0.5f), phys2_rng_range(&r, -0.4f, 0.4f), phys2_rng_range(&r, -0.5f, 0.5f));
    g_rock = phys2_hull_create(test_arena(), pts, 12);
    /* mesh zemin */
    const u32 n = 16;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_PUSH_ARRAY(test_arena(), Vec3, nv);
    u32* idx = ARENA_PUSH_ARRAY(test_arena(), u32, n * n * 6);
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i - 8.0f, z = (f32)j - 8.0f;
            v[j * (n + 1) + i] = vec3(x, 0.4f * m_sin(x * 0.7f) * m_cos(z * 0.5f), z);
        }
    }
    u32 k = 0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 a0 = j * (n + 1) + i, a1 = a0 + 1, b0 = a0 + n + 1, b1 = b0 + 1;
            idx[k++] = a0; idx[k++] = b0; idx[k++] = b1;
            idx[k++] = a0; idx[k++] = b1; idx[k++] = a1;
        }
    }
    g_mesh = phys2_trimesh_create(test_arena(), v, nv, idx, n * n * 2);
    static const char* const NAMES[4] = { "yığın + uyku: ilk farklı adım", "karışık + mesh + eklem: ilk farklı adım",
                                          "şehir + CCD mermileri: ilk farklı adım", "yığın + jöle + kumaş: ilk farklı adım" };
    for (u32 c = 0; c < 4; c++) {
        u32 bad = first_mismatch(c == 3 ? 0 : c, c == 0, 240, c == 3);
        test_note(NAMES[c], bad, 0);
        CHECK(bad == 0);
    }
}

TEST(phys2_bvh_raycast_equals_linear) {
    test_arena_reset();
    Phys2World* a = world(512, PHYS2_BP_SAP);
    Phys2World* b = world(512, PHYS2_BP_BVH);
    build(a, 2);
    build(b, 2);
    for (u32 s = 0; s < 30; s++) {   /* dinamik gövdeler hareket etsin (ağaç adım sonunda güncel) */
        phys2_step(a, DT);
        phys2_step(b, DT);
    }
    Phys2Rng r = phys2_rng_make(8);
    u32 diff = 0, hits = 0;
    for (u32 q = 0; q < 2000; q++) {
        Vec3 o = vec3(phys2_rng_range(&r, -30, 30), phys2_rng_range(&r, 0.5f, 15), phys2_rng_range(&r, -30, 30));
        Vec3 d = phys2_v3_norm(vec3(phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 0.3f), phys2_rng_range(&r, -1, 1)));
        Phys2RayHit ha, hb;
        b32 ka = phys2_raycast(a, o, d, 80.0f, PHYS2_NO_BODY, &ha);
        b32 kb = phys2_raycast(b, o, d, 80.0f, PHYS2_NO_BODY, &hb);
        if (ka != kb) { diff++; continue; }
        if (!ka) continue;
        hits++;
        if (ha.body != hb.body || phys2_f32_bits(ha.t) != phys2_f32_bits(hb.t) || phys2_f32_bits(ha.normal.x) != phys2_f32_bits(hb.normal.x) ||
            phys2_f32_bits(ha.normal.y) != phys2_f32_bits(hb.normal.y) || phys2_f32_bits(ha.normal.z) != phys2_f32_bits(hb.normal.z)) diff++;
    }
    test_note("2000 ışın: isabet", hits, 0);
    test_note("SAP (doğrusal) ↔ BVH farklı sonuç", diff, 0);
    CHECK(diff == 0 && hits > 500);
}
