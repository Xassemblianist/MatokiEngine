/* tests/test_phys2_shapes.c — M9 Faz 3b: hull, GJK/EPA, kapsül, mesh + BVH, eklemler.
 * Çapraz doğrulama: aynı kutu çiftleri SAT (box_box) ve GJK/EPA + yüz kırpma (hull çekirdeği) ile.
 * Analitik: hull hacmi/merkezi, mesh raycast yüksekliği, sarkaç periyodu 2π√(L/g), ip uzunluğu,
 * motor hızı. Yapısal: Euler (V − E + F = 2), BVH sorgusu = kaba kuvvet.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/pmath.h"
#include "phys2/gjk.h"
#include "phys2/phys.h"
#include "phys2/shapes.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* world(u32 n) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

/* tek kullanımlık gövde (ortak bir dünyada kurulup kopyalanır; test arenası taşmasın) */
static Phys2World* g_tmp_world;
static Arena* g_tmp_arena;

static Phys2Body make_body(const Phys2BodyDesc* d) {
    if (!g_tmp_world || g_tmp_arena != test_arena() || arena_mark(test_arena()) < sizeof(Phys2World)) {
        g_tmp_world = world(2);
        g_tmp_arena = test_arena();
    }
    phys2_world_clear(g_tmp_world);
    u32 id = phys2_add_body(g_tmp_world, d);
    return g_tmp_world->bodies[id];
}

/* ---- hull ---- */

/* 0 = geçerli; 1 Euler, 2 CCW, 3 dışarıda nokta, 4 NULL */
static u32 hull_check(const Phys2ConvexHull* h, const Vec3* pts, u32 n) {
    if (!h) return 4;
    u32 E = h->icount / 2;
    if ((i32)h->vcount - (i32)E + (i32)h->fcount != 2) return 1;
    for (u32 f = 0; f < h->fcount; f++) {
        const Phys2HullFace* hf = &h->faces[f];
        Vec3 a = h->verts[h->idx[hf->first]], b = h->verts[h->idx[hf->first + 1]], c = h->verts[h->idx[hf->first + 2]];
        if (vec3_dot(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)), hf->n) <= 0.0f) return 2;
        for (u32 i = 0; i < n; i++) {
            if (vec3_dot(hf->n, vec3_sub(pts[i], h->offset)) > hf->d + 1e-3f) return 3;
        }
    }
    return 0;
}

static b32 hull_valid(const Phys2ConvexHull* h, const Vec3* pts, u32 n) { return hull_check(h, pts, n) == 0; }

TEST(phys2_hull_build) {
    test_arena_reset();
    Arena* a = test_arena();
    Vec3 cube[8];
    for (u32 i = 0; i < 8; i++) cube[i] = vec3((i & 1) ? 2.0f : -2.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 0.5f : -0.5f);
    Phys2ConvexHull* h = phys2_hull_create(a, cube, 8);
    CHECK(h != NULL);
    CHECK(h->vcount == 8 && h->fcount == 6);
    CHECK_NEAR(h->volume, 8.0, 1e-4);
    CHECK(hull_valid(h, cube, 8));

    Vec3 tet[4] = { vec3(0, 0, 0), vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1) };
    Phys2ConvexHull* t = phys2_hull_create(a, tet, 4);
    CHECK(t && t->vcount == 4 && t->fcount == 4);
    CHECK_NEAR(t->volume, 1.0 / 6.0, 1e-5);
    CHECK_NEAR(t->offset.x, 0.25, 1e-5);
    CHECK(hull_valid(t, tet, 4));

    Phys2Rng r = phys2_rng_make(31);
    u32 ok = 0;
    for (u32 trial = 0; trial < 20; trial++) {
        Vec3 pts[40];
        for (u32 i = 0; i < 40; i++) {
            Vec3 p;
            do p = vec3(phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 1)); while (vec3_dot(p, p) > 1.0f);
            pts[i] = vec3_add(vec3(3, -2, 1), phys2_v3_mul(p, vec3(1.0f, 0.6f, 1.4f)));
        }
        Phys2ConvexHull* rh = phys2_hull_create(a, pts, 40);
        u32 code = hull_check(rh, pts, 40);
        if (code) {
            test_note("hull hata kodu", code, 0);
            if (rh) {
                test_note("  V", rh->vcount, 0);
                test_note("  F", rh->fcount, 0);
                test_note("  E", rh->icount / 2, 0);
            }
        }
        ok += code == 0;
    }
    CHECK(ok == 20);

    Vec3 flat[4] = { vec3(0, 0, 0), vec3(1, 0, 0), vec3(0, 0, 1), vec3(1, 0, 1) };
    CHECK(phys2_hull_create(a, flat, 4) == NULL);
}

/* ---- SAT ↔ GJK/EPA çapraz kontrolü ---- */

static f32 max_depth(const Phys2Manifold* m) {
    f32 d = -1.0f;
    for (u32 i = 0; i < m->count; i++) d = m_max(d, m->p[i].depth);
    return d;
}

/* İki kutunun gerçek en küçük ayırma mesafesi: 15 SAT ekseninde en küçük örtüşme (>0 = girişim) */
static f32 sat_mtd(const Phys2Body* A, const Phys2Body* B) {
    Vec3 ax[15];
    u32 n = 0;
    for (u32 i = 0; i < 3; i++) ax[n++] = phys2_mat3_col(&A->rot_m, i);
    for (u32 i = 0; i < 3; i++) ax[n++] = phys2_mat3_col(&B->rot_m, i);
    for (u32 i = 0; i < 3; i++) {
        for (u32 j = 0; j < 3; j++) {
            Vec3 c = vec3_cross(phys2_mat3_col(&A->rot_m, i), phys2_mat3_col(&B->rot_m, j));
            if (vec3_len(c) > 1e-4f) ax[n++] = phys2_v3_norm(c);
        }
    }
    f32 best = 3.4e38f;
    Vec3 d = vec3_sub(B->pos, A->pos);
    for (u32 k = 0; k < n; k++) {
        f32 ra = 0.0f, rb = 0.0f;
        const f32 ha[3] = { A->half.x, A->half.y, A->half.z }, hb[3] = { B->half.x, B->half.y, B->half.z };
        for (u32 i = 0; i < 3; i++) {
            ra += ha[i] * m_abs(vec3_dot(phys2_mat3_col(&A->rot_m, i), ax[k]));
            rb += hb[i] * m_abs(vec3_dot(phys2_mat3_col(&B->rot_m, i), ax[k]));
        }
        best = m_min(best, ra + rb - m_abs(vec3_dot(d, ax[k])));
    }
    return best;
}

static Phys2BodyDesc random_box(Phys2Rng* r, Vec3 pos) {
    Phys2BodyDesc d = phys2_desc_box(pos, vec3(phys2_rng_range(r, 0.3f, 1), phys2_rng_range(r, 0.3f, 1), phys2_rng_range(r, 0.3f, 1)), 1.0f);
    d.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(r, -1, 1), phys2_rng_range(r, -1, 1), phys2_rng_range(r, -1, 1)), phys2_rng_range(r, -3, 3));
    return d;
}

TEST(phys2_epa_depth_equals_sat_mtd) {
    test_arena_reset();
    /* derin girişimler dahil: EPA çekirdek derinliği = 15 eksenli SAT'ın gerçek MTD'si */
    Phys2Rng r = phys2_rng_make(4242);
    u32 n = 0, bad = 0;
    f32 worst = 0.0f;
    for (u32 it = 0; it < 3000; it++) {
        Phys2BodyDesc da = random_box(&r, vec3(0, 0, 0));
        Phys2BodyDesc db = random_box(&r, vec3(phys2_rng_range(&r, -1.2f, 1.2f), phys2_rng_range(&r, -1.2f, 1.2f), phys2_rng_range(&r, -1.2f, 1.2f)));
        Phys2Body A = make_body(&da), B = make_body(&db);
        f32 mtd = sat_mtd(&A, &B);
        if (mtd <= 0.001f) continue;
        Phys2Core ca, cb;
        phys2_body_core(&A, &ca);
        phys2_body_core(&B, &cb);
        Phys2GjkResult g;
        phys2_gjk_distance(&ca, &cb, &g);
        Phys2EpaResult e;
        if (!g.overlap) { bad++; continue; }
        phys2_epa_penetration(&ca, &cb, &g, &e);
        f32 diff = m_abs(e.depth - mtd);
        worst = m_max(worst, diff);
        if (!e.ok || diff > 2e-3f) bad++;
        n++;
    }
    test_note("girişimli çift", n, 0);
    test_note("EPA − MTD en büyük fark", worst, 6);
    CHECK(n > 1000);
    CHECK(bad == 0);
}

TEST(phys2_gjk_epa_matches_sat_boxes) {
    test_arena_reset();
    /* gerçekçi sığ temaslar: B'yi rastgele yönden A'ya yaklaştır, MTD ∈ [−0,015, 0,06] */
    Phys2Rng r = phys2_rng_make(777);
    u32 both = 0, exist_mismatch = 0, normal_mismatch = 0, depth_bad = 0, bad_normal = 0, bad_sat = 0, cases = 3000;
    f32 worst = 0.0f;
    for (u32 it = 0; it < cases; it++) {
        Phys2BodyDesc da = random_box(&r, vec3(0, 0, 0));
        Phys2BodyDesc db = random_box(&r, vec3(0, 0, 0));
        Vec3 u = phys2_v3_norm(vec3(phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 1), phys2_rng_range(&r, -1, 1)));
        f32 target = phys2_rng_range(&r, -0.015f, 0.06f);
        f32 lo = 0.0f, hi = 4.0f;   /* ikili arama: MTD(t) = target */
        Phys2Body A = make_body(&da), B;
        for (u32 k = 0; k < 40; k++) {
            f32 mid = (lo + hi) * 0.5f;
            db.pos = vec3_scale(u, mid);
            B = make_body(&db);
            if (sat_mtd(&A, &B) > target) lo = mid; else hi = mid;
        }
        db.pos = vec3_scale(u, (lo + hi) * 0.5f);
        B = make_body(&db);
        Phys2Manifold ms, mg;
        u32 ns = phys2_collide(&A, &B, &ms);
        Phys2Core ca, cb;
        phys2_body_core(&A, &ca);
        phys2_body_core(&B, &cb);
        u32 ng = phys2_collide_cores(&ca, &cb, &mg);
        f32 ds = ns ? max_depth(&ms) : -1.0f, dg = ng ? max_depth(&mg) : -1.0f;
        /* pay sınırındaki (|derinlik| < 0.03) farkları sayma */
        if ((ns != 0) != (ng != 0)) {
            if (m_max(ds, dg) > 0.03f) exist_mismatch++;
            continue;
        }
        if (!ns) continue;
        both++;
        /* normaller farklı olabilir (SAT yüz histerezisi vs EPA'nın gerçek MTD yönü); ikisi de
         * en iyi eksenden belirgin biçimde kötü olmamalı: o(n) <= 1.1·MTD + 0.012 */
        if (vec3_dot(ms.normal, mg.normal) < 0.99f) normal_mismatch++;
        f32 mtd = sat_mtd(&A, &B);
        const Vec3 ns_[2] = { ms.normal, mg.normal };
        for (u32 q = 0; q < 2; q++) {
            Vec3 nn = ns_[q];
            f32 ra = 0.0f, rb = 0.0f;
            const f32 ha[3] = { A.half.x, A.half.y, A.half.z }, hb[3] = { B.half.x, B.half.y, B.half.z };
            for (u32 i = 0; i < 3; i++) {
                ra += ha[i] * m_abs(vec3_dot(phys2_mat3_col(&A.rot_m, i), nn));
                rb += hb[i] * m_abs(vec3_dot(phys2_mat3_col(&B.rot_m, i), nn));
            }
            f32 o = ra + rb - m_abs(vec3_dot(vec3_sub(B.pos, A.pos), nn));
            if (o > 1.1f * mtd + 0.012f) {
                bad_normal++;
                if (q == 0) bad_sat++;
                if (bad_normal <= 3) {
                    test_note("  kötü: MTD", mtd, 5);
                    test_note("  kötü: o(n)", o, 5);
                    test_note("  kötü: nokta sayısı", mg.count, 0);
                    test_note("  kötü: GJK derinlik", dg, 5);
                    Phys2GjkResult gg;
                    phys2_gjk_distance(&ca, &cb, &gg);
                    test_note("  kötü: gjk tur", gg.iters, 0);
                    test_note("  kötü: gjk çıkış", gg.exit, 0);
                    test_note("  kötü: gjk simplex", gg.n, 0);
                    test_note("  kötü: gjk mesafe", gg.dist, 5);
                    Vec3 nn2 = vec3_scale(vec3_sub(gg.pb, gg.pa), 1.0f / gg.dist);
                    test_note("  kötü: destekle örtüşme", vec3_dot(vec3_sub(phys2_core_support(&ca, nn2), phys2_core_support(&cb, vec3_scale(nn2, -1.0f))), nn2), 5);
                }
            }
        }
        f32 diff = m_abs(ds - dg);
        worst = m_max(worst, diff);
        if (diff > 0.05f * m_max(ds, dg) + 0.012f) depth_bad++;
    }
    test_note("temaslı çift", both, 0);
    test_note("var/yok uyumsuz", exist_mismatch, 0);
    test_note("farklı normal seçen (dot < 0.99, bilgi)", normal_mismatch, 0);
    test_note("en iyi eksenden belirgin kötü normal", bad_normal, 0);
    test_note("  bunlardan SAT", bad_sat, 0);
    test_note("derinlik toleransı dışı", depth_bad, 0);
    test_note("en büyük derinlik farkı", worst, 4);
    CHECK(both > 500);
    CHECK(exist_mismatch == 0);
    CHECK(bad_normal == 0);
    CHECK(depth_bad == 0);
}

/* ---- kapsül ---- */

TEST(phys2_capsule_contacts) {
    test_arena_reset();
    Phys2Manifold m;
    Phys2BodyDesc pd = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    Phys2Body P = make_body(&pd);
    /* yatay kapsül düzlemde: 2 nokta */
    Phys2BodyDesc cd = phys2_desc_capsule(vec3(0, 0.45f, 0), 0.5f, 1.0f, 1.0f);
    cd.rot = phys2_quat_from_axis_angle(vec3(0, 0, 1), M_PI * 0.5f);
    Phys2Body C = make_body(&cd);
    CHECK(phys2_collide(&C, &P, &m) == 2);
    CHECK_NEAR(m.normal.y, -1.0, 1e-5);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-4);
    CHECK_NEAR(m.p[1].depth, 0.05, 1e-4);

    /* dik kesişen kapsüller: 1 nokta, d = r1 + r2 − mesafe */
    Phys2BodyDesc c1 = phys2_desc_capsule(vec3(0, 0, 0), 0.3f, 1.0f, 1.0f);
    c1.rot = phys2_quat_from_axis_angle(vec3(0, 0, 1), M_PI * 0.5f);   /* x ekseni */
    Phys2BodyDesc c2 = phys2_desc_capsule(vec3(0, 0.5f, 0), 0.3f, 1.0f, 1.0f);
    c2.rot = phys2_quat_from_axis_angle(vec3(1, 0, 0), M_PI * 0.5f);   /* z ekseni */
    Phys2Body A = make_body(&c1), B = make_body(&c2);
    CHECK(phys2_collide(&A, &B, &m) == 1);
    CHECK_NEAR(m.normal.y, 1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.1, 1e-4);

    /* paralel kapsüller (kısmi örtüşme): 2 nokta */
    Phys2BodyDesc c3 = phys2_desc_capsule(vec3(0.8f, 0.5f, 0), 0.3f, 1.0f, 1.0f);
    c3.rot = c1.rot;
    Phys2Body Cc = make_body(&c3);
    CHECK(phys2_collide(&A, &Cc, &m) == 2);
    CHECK_NEAR(m.normal.y, 1.0, 1e-4);
    for (u32 i = 0; i < 2; i++) CHECK_NEAR(m.p[i].depth, 0.1, 1e-4);

    /* kutu üstünde yatan kapsül: 2 nokta (doğru parçası yüze paralel) */
    Phys2BodyDesc bd = phys2_desc_box(vec3(0, -0.5f, 0), vec3(2, 0.5f, 2), 1.0f);
    Phys2Body Bx = make_body(&bd);
    CHECK(phys2_collide(&C, &Bx, &m) == 2);
    CHECK_NEAR(m.normal.y, -1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-3);

    /* küre-hull (genel yol) */
    Vec3 cube[8];
    for (u32 i = 0; i < 8; i++) cube[i] = vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    Phys2ConvexHull* h = phys2_hull_create(test_arena(), cube, 8);
    Phys2BodyDesc hd = phys2_desc_hull(vec3(0, 0, 0), h, 1.0f);
    Phys2BodyDesc sd = phys2_desc_sphere(vec3(0.3f, 1.4f, -0.2f), 0.5f, 1.0f);
    Phys2Body H = make_body(&hd), S = make_body(&sd);
    CHECK(phys2_collide(&S, &H, &m) == 1);
    CHECK_NEAR(m.normal.y, -1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.1, 1e-4);
}

/* ---- mesh + BVH ---- */

static Phys2TriMesh* slope_grid(u32 n, f32 cell, f32 sx, f32 sz) {
    Arena* a = test_arena();
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_PUSH_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_PUSH_ARRAY(a, u32, n * n * 6);
    f32 half = (f32)n * cell * 0.5f;
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i * cell - half, z = (f32)j * cell - half;
            v[j * (n + 1) + i] = vec3(x, sx * x + sz * z, z);
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
    return phys2_trimesh_create(a, v, nv, idx, n * n * 2);
}

TEST(phys2_mesh_bvh_and_raycast) {
    test_arena_reset();
    Phys2TriMesh* m = slope_grid(32, 1.0f, 0.2f, 0.1f);
    Phys2Rng r = phys2_rng_make(5);
    u32 mism = 0;
    static u32 got[4096];
    for (u32 it = 0; it < 200; it++) {
        Vec3 c = vec3(phys2_rng_range(&r, -18, 18), phys2_rng_range(&r, -6, 6), phys2_rng_range(&r, -18, 18));
        Vec3 e = vec3(phys2_rng_range(&r, 0.1f, 3), phys2_rng_range(&r, 0.1f, 3), phys2_rng_range(&r, 0.1f, 3));
        Vec3 mn = vec3_sub(c, e), mx = vec3_add(c, e);
        u32 n = phys2_trimesh_query_aabb(m, mn, mx, got, 4096);
        u32 brute = 0;
        for (u32 t = 0; t < m->tri_count; t++) {
            Vec3 a, b, cc;
            phys2_trimesh_tri(m, t, &a, &b, &cc);
            Vec3 tmn = vec3(m_min(a.x, m_min(b.x, cc.x)), m_min(a.y, m_min(b.y, cc.y)), m_min(a.z, m_min(b.z, cc.z)));
            Vec3 tmx = vec3(m_max(a.x, m_max(b.x, cc.x)), m_max(a.y, m_max(b.y, cc.y)), m_max(a.z, m_max(b.z, cc.z)));
            if (tmn.x <= mx.x && tmx.x >= mn.x && tmn.y <= mx.y && tmx.y >= mn.y && tmn.z <= mx.z && tmx.z >= mn.z) brute++;
        }
        if (n != brute) mism++;
    }
    CHECK(mism == 0);
    test_note("BVH düğüm", m->node_count, 0);

    /* düzlem üzerinde raycast: y = 0.2x + 0.1z */
    Phys2World* w = world(4);
    Phys2BodyDesc md = phys2_desc_mesh(vec3(0, 0, 0), m);
    phys2_add_body(w, &md);
    f32 worst = 0.0f;
    for (u32 it = 0; it < 500; it++) {
        f32 x = phys2_rng_range(&r, -15, 15), z = phys2_rng_range(&r, -15, 15);
        Phys2RayHit h;
        if (!phys2_raycast(w, vec3(x, 20, z), vec3(0, -1, 0), 100.0f, PHYS2_NO_BODY, &h)) { worst = 1e9f; continue; }
        worst = m_max(worst, m_abs(h.point.y - (0.2f * x + 0.1f * z)));
    }
    test_note("mesh raycast en büyük yükseklik hatası", worst, 6);
    CHECK(worst < 1e-4f);

    /* kapsül ve hull raycast */
    Phys2BodyDesc cd = phys2_desc_capsule(vec3(30, 0, 0), 0.5f, 1.0f, 1.0f);
    phys2_add_body(w, &cd);
    Phys2RayHit h;
    CHECK(phys2_raycast(w, vec3(30, 10, 0), vec3(0, -1, 0), 100.0f, PHYS2_NO_BODY, &h));
    CHECK_NEAR(h.t, 10.0 - 1.5, 1e-4);
    CHECK(phys2_raycast(w, vec3(25, 0.5f, 0), vec3(1, 0, 0), 100.0f, PHYS2_NO_BODY, &h));
    CHECK_NEAR(h.t, 4.5, 1e-4);
}

static Phys2TriMesh* flat_grid(u32 n, f32 cell) { return slope_grid(n, cell, 0.0f, 0.0f); }

TEST(phys2_bodies_rest_on_mesh) {
    test_arena_reset();
    Phys2World* w = world(16);
    Phys2BodyDesc md = phys2_desc_mesh(vec3(0, 0, 0), flat_grid(16, 1.0f));
    phys2_add_body(w, &md);
    /* kutu üçgen köşegenlerinin üstünde, döndürülmüş */
    Phys2BodyDesc bd = phys2_desc_box(vec3(0.3f, 0.8f, 0.2f), vec3(0.5f, 0.5f, 0.5f), 1.0f);
    bd.rot = phys2_quat_from_axis_angle(vec3(0, 1, 0), 0.6f);
    u32 box = phys2_add_body(w, &bd);
    Phys2BodyDesc sd = phys2_desc_sphere(vec3(-3, 1, 2), 0.4f, 1.0f);
    u32 sph = phys2_add_body(w, &sd);
    Phys2BodyDesc cd = phys2_desc_capsule(vec3(3, 1, -2), 0.3f, 0.6f, 1.0f);
    cd.rot = phys2_quat_from_axis_angle(vec3(1, 0, 0), M_PI * 0.5f);
    u32 cap = phys2_add_body(w, &cd);
    Vec3 pts[12];
    for (u32 i = 0; i < 12; i++) {
        f32 a = (f32)i * (M_TAU / 6.0f);
        pts[i] = vec3(m_cos(a) * 0.5f, i < 6 ? -0.3f : 0.3f, m_sin(a) * 0.5f);   /* altıgen prizma */
    }
    Phys2ConvexHull* hx = phys2_hull_create(test_arena(), pts, 12);
    Phys2BodyDesc hd = phys2_desc_hull(vec3(-2, 1, -3), hx, 1.0f);
    u32 hul = phys2_add_body(w, &hd);
    for (u32 i = 0; i < 300; i++) phys2_step(w, DT);
    const Phys2Body* B = &w->bodies[box];
    test_note("mesh üstünde kutu y", B->pos.y, 4);
    CHECK_NEAR(B->pos.y, 0.5, 0.01);
    CHECK(phys2_mat3_col(&B->rot_m, 1).y > 0.999f);
    CHECK(vec3_len(B->vel) < 0.02f);
    CHECK_NEAR(w->bodies[sph].pos.y, 0.4, 0.01);
    CHECK_NEAR(w->bodies[cap].pos.y, 0.3, 0.01);
    CHECK(vec3_len(w->bodies[cap].ang_vel) < 0.05f);
    CHECK_NEAR(w->bodies[hul].pos.y, 0.3, 0.01);
    CHECK(phys2_mat3_col(&w->bodies[hul].rot_m, 1).y > 0.999f);
}

/* ---- eklemler ---- */

static f32 pendulum_period(i32 solver, f32 L) {
    Phys2World* w = world(4);
    phys2_world_set_solver(w, solver);
    const f32 th = 0.1f;
    Phys2BodyDesc s = phys2_desc_sphere(vec3(m_sin(th) * L, 10.0f - m_cos(th) * L, 0), 0.02f, 1.0f);
    u32 bob = phys2_add_body(w, &s);
    Phys2JointDesc j = phys2_joint_desc(PHYS2_JOINT_BALL, PHYS2_NO_BODY, bob, vec3(0, 10, 0));
    phys2_add_joint(w, &j);
    f32 prev = w->bodies[bob].pos.x;
    f32 t = 0.0f, first = -1.0f, last = 0.0f;
    u32 crossings = 0;
    for (u32 i = 0; i < 600; i++) {
        phys2_step(w, DT);
        t += DT;
        f32 x = w->bodies[bob].pos.x;
        if ((prev > 0.0f) != (x > 0.0f)) {
            f32 tc = t - DT * x / (x - prev);   /* doğrusal ara değer */
            if (first < 0.0f) first = tc;
            last = tc;
            crossings++;
        }
        prev = x;
    }
    return crossings > 1 ? 2.0f * (last - first) / (f32)(crossings - 1) : 0.0f;
}

TEST(phys2_joint_pendulum_period) {
    test_arena_reset();
    const f32 L = 2.0f;
    f32 want = M_TAU * m_sqrt(L / 9.81f);
    f32 soft = pendulum_period(PHYS2_SOLVER_SOFT, L), si = pendulum_period(PHYS2_SOLVER_SI, L);
    test_note("sarkaç periyodu soft (s)", soft, 4);
    test_note("sarkaç periyodu SI (s)", si, 4);
    test_note("analitik 2π√(L/g)", want, 4);
    CHECK_NEAR(soft, want, 0.015 * want);
    CHECK_NEAR(si, want, 0.015 * want);
}

TEST(phys2_joint_chain_and_rope) {
    test_arena_reset();
    Phys2World* w = world(32);
    u32 prev = PHYS2_NO_BODY;
    u32 first_joint = w->joint_count;
    for (u32 i = 0; i < 10; i++) {
        Phys2BodyDesc c = phys2_desc_capsule(vec3(0.5f + (f32)i, 10, 0), 0.1f, 0.4f, 1.0f);
        c.rot = phys2_quat_from_axis_angle(vec3(0, 0, 1), M_PI * 0.5f);   /* x boyunca */
        u32 id = phys2_add_body(w, &c);
        Phys2JointDesc j = phys2_joint_desc(PHYS2_JOINT_BALL, prev, id, vec3((f32)i, 10, 0));
        phys2_add_joint(w, &j);
        prev = id;
    }
    f32 worst = 0.0f;
    for (u32 s = 0; s < 240; s++) {
        phys2_step(w, DT);
        for (u32 k = first_joint; k < w->joint_count; k++) worst = m_max(worst, phys2_joint_error(w, k));
    }
    test_note("zincir: en büyük eklem hatası (m)", worst, 4);
    CHECK(worst < 0.05f);   /* yumuşak eklemler (60 Hz) en ağır anda birkaç cm esner */
    CHECK(phys2_kinetic_energy(w) < 2000.0);

    /* ip: 1 m'den 3 m'ye kadar serbest düşer, sonra gergin kalır */
    Phys2World* w2 = world(4);
    Phys2BodyDesc s = phys2_desc_sphere(vec3(0, 9, 0), 0.1f, 1.0f);
    u32 b = phys2_add_body(w2, &s);
    Phys2JointDesc rope = phys2_joint_desc(PHYS2_JOINT_DISTANCE, PHYS2_NO_BODY, b, vec3(0, 10, 0));
    rope.anchor_b = vec3(0, 9, 0);
    rope.min_length = 0.0f;
    rope.max_length = 3.0f;
    phys2_add_joint(w2, &rope);
    for (u32 i = 0; i < 20; i++) phys2_step(w2, DT);
    f32 y_free = w2->bodies[b].pos.y;
    CHECK(y_free < 9.0f && y_free > 7.0f);   /* henüz gerilmedi, serbest düşüşte */
    for (u32 i = 0; i < 300; i++) phys2_step(w2, DT);
    test_note("ip: son y", w2->bodies[b].pos.y, 4);
    CHECK_NEAR(w2->bodies[b].pos.y, 7.0, 0.03);

    /* çubuk: iki küre atılıp döner, mesafe sabit */
    Phys2World* w3 = world(4);
    w3->gravity = vec3(0, 0, 0);
    Phys2BodyDesc s1 = phys2_desc_sphere(vec3(0, 0, 0), 0.1f, 1.0f), s2 = phys2_desc_sphere(vec3(1.5f, 0, 0), 0.1f, 2.0f);
    s1.vel = vec3(0, 3, 1);
    s2.vel = vec3(1, -2, 0);
    u32 i1 = phys2_add_body(w3, &s1), i2 = phys2_add_body(w3, &s2);
    Phys2JointDesc rod = phys2_joint_desc(PHYS2_JOINT_DISTANCE, i1, i2, vec3(0, 0, 0));
    rod.anchor_b = vec3(1.5f, 0, 0);
    u32 jr = phys2_add_joint(w3, &rod);
    f32 rod_err = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys2_step(w3, DT);
        rod_err = m_max(rod_err, phys2_joint_error(w3, jr));
    }
    test_note("çubuk: en büyük uzunluk hatası (m)", rod_err, 5);
    CHECK(rod_err < 0.015f);
}

TEST(phys2_joint_hinge_plane_limit_motor) {
    test_arena_reset();
    /* menteşe sarkacı: z ekseni etrafında; z yönünde sapma olmamalı */
    Phys2World* w = world(8);
    Phys2BodyDesc b = phys2_desc_box(vec3(1.5f, 10, 0.3f), vec3(0.5f, 0.1f, 0.3f), 1.0f);
    u32 bob = phys2_add_body(w, &b);
    Phys2JointDesc h = phys2_joint_desc(PHYS2_JOINT_HINGE, PHYS2_NO_BODY, bob, vec3(0, 10, 0.3f));
    h.axis = vec3(0, 0, 1);
    u32 jh = phys2_add_joint(w, &h);
    f32 zdev = 0.0f, wxy = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys2_step(w, DT);
        zdev = m_max(zdev, m_abs(w->bodies[bob].pos.z - 0.3f));
        wxy = m_max(wxy, m_abs(w->bodies[bob].ang_vel.x) + m_abs(w->bodies[bob].ang_vel.y));
    }
    test_note("menteşe: z sapması", zdev, 6);
    CHECK(zdev < 2e-3f);
    CHECK(wxy < 1e-2f);
    CHECK(phys2_joint_error(w, jh) < 0.01f);

    /* açı sınırı: [-0.5, 0.5] rad; yatay başlar, aşağı salınır ama sınırda durur */
    Phys2World* w2 = world(8);
    Phys2BodyDesc b2 = phys2_desc_box(vec3(1.5f, 10, 0), vec3(0.5f, 0.1f, 0.3f), 1.0f);
    u32 bob2 = phys2_add_body(w2, &b2);
    Phys2JointDesc hl = phys2_joint_desc(PHYS2_JOINT_HINGE, PHYS2_NO_BODY, bob2, vec3(0, 10, 0));
    hl.axis = vec3(0, 0, 1);
    hl.limit = TRUE;
    hl.lower = -0.5f;
    hl.upper = 0.5f;
    u32 jl = phys2_add_joint(w2, &hl);
    f32 amin = 0.0f, amax = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys2_step(w2, DT);
        amin = m_min(amin, w2->joints[jl].angle);
        amax = m_max(amax, w2->joints[jl].angle);
    }
    test_note("menteşe sınırı: en küçük açı", amin, 4);
    CHECK(amin > -0.5f - 0.03f);
    CHECK(amax < 0.5f + 0.03f);
    CHECK(amin < -0.45f);   /* sınıra gerçekten ulaştı */

    /* motor: x ekseninde simetrik tekerlek, 5 rad/s */
    Phys2World* w3 = world(8);
    Phys2BodyDesc wheel = phys2_desc_box(vec3(0, 5, 0), vec3(0.1f, 0.5f, 0.5f), 2.0f);
    u32 wh = phys2_add_body(w3, &wheel);
    Phys2JointDesc hm = phys2_joint_desc(PHYS2_JOINT_HINGE, PHYS2_NO_BODY, wh, vec3(0, 5, 0));
    hm.axis = vec3(1, 0, 0);
    hm.motor = TRUE;
    hm.motor_speed = 5.0f;
    hm.max_motor_torque = 50.0f;
    phys2_add_joint(w3, &hm);
    for (u32 i = 0; i < 60; i++) phys2_step(w3, DT);
    test_note("motor: açısal hız (rad/s)", w3->bodies[wh].ang_vel.x, 4);
    CHECK_NEAR(w3->bodies[wh].ang_vel.x, 5.0, 0.1);
    CHECK_NEAR(w3->bodies[wh].pos.y, 5.0, 0.01);
}

TEST(phys2_joint_determinism) {
    test_arena_reset();
    u32 hsh[2];
    for (u32 run = 0; run < 2; run++) {
        Phys2World* w = world(32);
        w->allow_sleep = TRUE;
        Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
        phys2_add_body(w, &g);
        u32 prev = PHYS2_NO_BODY;
        for (u32 i = 0; i < 6; i++) {
            Phys2BodyDesc c = phys2_desc_box(vec3(0.5f + (f32)i, 6, 0), vec3(0.45f, 0.1f, 0.2f), 1.0f);
            u32 id = phys2_add_body(w, &c);
            Phys2JointDesc j = phys2_joint_desc(i ? PHYS2_JOINT_HINGE : PHYS2_JOINT_BALL, prev, id, vec3((f32)i, 6, 0));
            j.axis = vec3(0, 0, 1);
            phys2_add_joint(w, &j);
            prev = id;
        }
        for (u32 k = 0; k < 4; k++) {
            Phys2BodyDesc s = phys2_desc_sphere(vec3(-4.0f + (f32)k, 1 + (f32)k, 0.1f * (f32)k), 0.3f, 2.0f);
            s.vel = vec3(6, 3, 0);
            phys2_add_body(w, &s);
        }
        for (u32 i = 0; i < 240; i++) phys2_step(w, DT);
        hsh[run] = phys2_state_hash(w);
    }
    CHECK(hsh[0] == hsh[1]);
}
