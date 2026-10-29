/* tests/test_phys2_port.c — phys2 taşımasının eklentileri: düzeltilen 3 hatanın regresyon testleri ve
 * belirlenimcilik (aynı sahne iki koşu; seri ↔ paralel yol: dar faz, renkli kumaş, okyanus).
 * Hata 1: uyandırılan ada o adımda düzlem çifti almıyordu. Hata 2: karakter mesh ile çarpışmıyor ve tüm gövdeleri
 * tarıyordu. Hata 3: çift anahtarı 16 bit → 65 536'dan fazla gövdede bozulma (artık 32+32 bit).
 * Paralel yol host'ta iş sistemine sahte worker sayısı (engine_set_worker_count) verilerek koşturulur:
 * dilimler yine ana thread'de çalışır, ama bölüşüm / geçici tampon / sıralı tüketim kodu birebir sınanır.
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/phys.h"
#include "phys2/shapes.h"
#include "phys2/character.h"
#include "phys2/soft.h"
#include "phys2/ocean.h"
#include "phys2/par.h"

static const f32 DT = 1.0f / 60.0f;

static b32 same_bytes(const void* a, const void* b, usize n) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i]) return FALSE;
    }
    return TRUE;
}

static Phys2World* new_world(u32 bodies, u32 manifolds) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), bodies, manifolds);
    return w;
}

/* iş sistemine sahte worker'lar: thread_count() > 1 olur, dilimleri ana thread çalıştırır */
static void fake_workers(u32 n) { engine_set_worker_count(n); }

/* ---- hata 1: uyanan ada düzlemle aynı adımda temas eder ---- */

static u32 pair_manifolds(const Phys2World* w, u32 a, u32 b) {
    u32 n = 0;
    for (u32 i = 0; i < w->cur_count; i++) {
        if (w->cur[i].a == a && w->cur[i].b == b && w->cur[i].count) n++;
    }
    return n;
}

static void woken_island_scene(Phys2Broadphase mode) {
    test_arena_reset();
    Phys2World* w = new_world(8, 128);
    w->broadphase = mode;
    Phys2BodyDesc pd = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    u32 plane = phys2_add_body(w, &pd);
    Phys2BodyDesc bd = phys2_desc_box(vec3(0, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 1.0f);
    u32 box = phys2_add_body(w, &bd);
    Phys2BodyDesc sd = phys2_desc_sphere(vec3(0, 1.25f, 0), 0.3f, 1.0f);
    u32 ball = phys2_add_body(w, &sd);
    /* kutuyu (kendi adasında) uyut: çocuk uyku durumu elle kurulur */
    w->bodies[box].flags |= PHYS2_BODY_ASLEEP;
    w->bodies[box].island = 5;
    phys2_step(w, DT);
    CHECK(!(w->bodies[box].flags & PHYS2_BODY_ASLEEP));        /* top değdi → ada uyandı */
    CHECK(pair_manifolds(w, plane, box) == 1);                  /* uyanan kutu aynı adımda düzlemle temas etti */
    CHECK(pair_manifolds(w, plane, ball) == 0);                 /* top havada */
    CHECK(pair_manifolds(w, box, ball) == 1);
}

TEST(phys2_port_woken_island_gets_plane_pair_bvh) { woken_island_scene(PHYS2_BP_BVH); }
TEST(phys2_port_woken_island_gets_plane_pair_sap) { woken_island_scene(PHYS2_BP_SAP); }

/* uyanan ada düzlemden başka statik komşularıyla da (aynı adımda) çiftlenir */
TEST(phys2_port_woken_island_gets_static_pair) {
    test_arena_reset();
    Phys2World* w = new_world(8, 128);
    Phys2BodyDesc fd = phys2_desc_box(vec3(0, -0.5f, 0), vec3(5, 0.5f, 5), 0.0f);   /* statik zemin kutusu */
    u32 floor_id = phys2_add_body(w, &fd);
    Phys2BodyDesc bd = phys2_desc_box(vec3(0, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 1.0f);
    u32 box = phys2_add_body(w, &bd);
    Phys2BodyDesc sd = phys2_desc_sphere(vec3(0, 1.25f, 0), 0.3f, 1.0f);
    phys2_add_body(w, &sd);
    w->bodies[box].flags |= PHYS2_BODY_ASLEEP;
    w->bodies[box].island = 3;
    phys2_step(w, DT);
    CHECK(!(w->bodies[box].flags & PHYS2_BODY_ASLEEP));
    CHECK(pair_manifolds(w, floor_id, box) == 1);
}

/* ---- hata 2: karakter mesh ile çarpışır; yalnızca yakındaki gövdeleri sınar ---- */

static Phys2TriMesh* floor_and_wall_mesh(void) {
    Arena* a = test_arena();
    /* zemin 20×20 (y = 0, +y normal) + x = 2'de duvar (−x normal, z ∈ [−5, 5], y ∈ [0, 3]) */
    static const Vec3 v[8] = {
        { -10, 0, -10 }, { -10, 0, 10 }, { 10, 0, 10 }, { 10, 0, -10 },
        { 2, 0, -5 }, { 2, 0, 5 }, { 2, 3, 5 }, { 2, 3, -5 },
    };
    static const u32 idx[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    return phys2_trimesh_create(a, v, 8, idx, 4);
}

TEST(phys2_port_character_stands_on_mesh_floor) {
    test_arena_reset();
    Phys2World* w = new_world(8, 64);
    Phys2TriMesh* m = floor_and_wall_mesh();
    Phys2BodyDesc md = phys2_desc_mesh(vec3(0, 0, 0), m);
    u32 mesh_id = phys2_add_body(w, &md);
    Phys2Character c;
    Phys2CharacterDesc d = phys2_character_desc_default(vec3(-4, 1.0f, 0));
    phys2_character_init(&c, &d);
    for (u32 i = 0; i < 180; i++) phys2_character_update(w, &c, vec3(0, 0, 0), FALSE, DT);
    CHECK(c.grounded);
    CHECK(c.ground_body == mesh_id);
    CHECK(c.pos.y > -0.01f && c.pos.y < 0.05f);   /* mesh yok sayılırsa karakter sonsuza düşerdi */
}

TEST(phys2_port_character_blocked_by_mesh_wall) {
    test_arena_reset();
    Phys2World* w = new_world(8, 64);
    Phys2TriMesh* m = floor_and_wall_mesh();
    Phys2BodyDesc md = phys2_desc_mesh(vec3(0, 0, 0), m);
    phys2_add_body(w, &md);
    Phys2Character c;
    Phys2CharacterDesc d = phys2_character_desc_default(vec3(-1, 0.05f, 0));
    phys2_character_init(&c, &d);
    for (u32 i = 0; i < 150; i++) phys2_character_update(w, &c, vec3(1, 0, 0), FALSE, DT);
    CHECK(c.pos.x > 1.0f);                       /* duvara yürüdü */
    CHECK(c.pos.x <= 2.0f - c.radius + 0.02f);   /* ama geçemedi */
    CHECK(c.grounded);
}

TEST(phys2_port_character_query_is_local) {
    test_arena_reset();
    const u32 n = 2000;
    Phys2World* w = new_world(n + 8, n * 2 + 64);
    Phys2BodyDesc pd = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(w, &pd);
    /* karakterden uzakta 2000 statik kutu (x ≥ 20) */
    for (u32 i = 0; i < n; i++) {
        Phys2BodyDesc b = phys2_desc_box(vec3(20.0f + (f32)(i % 50) * 3.0f, 0.5f, ((f32)(i / 50) - 20.0f) * 3.0f), vec3(0.5f, 0.5f, 0.5f), 0.0f);
        phys2_add_body(w, &b);
    }
    Phys2BodyDesc near_box = phys2_desc_box(vec3(2.0f, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 0.0f);
    phys2_add_body(w, &near_box);
    Phys2Character c;
    Phys2CharacterDesc d = phys2_character_desc_default(vec3(0, 0.3f, 0));
    phys2_character_init(&c, &d);
    for (u32 i = 0; i < 30; i++) phys2_character_update(w, &c, vec3(0, 0, 0), FALSE, DT);
    CHECK(c.grounded);
    CHECK(c.last_tests >= 1);                  /* en az düzlem */
    CHECK(c.last_tests <= 12);                 /* O(N) tarama (> 2000) değil */
    /* BVH ve SAP aynı sonucu verir */
    Phys2World* w2 = new_world(n + 8, n * 2 + 64);
    w2->broadphase = PHYS2_BP_SAP;
    phys2_add_body(w2, &pd);
    for (u32 i = 0; i < n; i++) {
        Phys2BodyDesc b = phys2_desc_box(vec3(20.0f + (f32)(i % 50) * 3.0f, 0.5f, ((f32)(i / 50) - 20.0f) * 3.0f), vec3(0.5f, 0.5f, 0.5f), 0.0f);
        phys2_add_body(w2, &b);
    }
    phys2_add_body(w2, &near_box);
    Phys2Character c2;
    phys2_character_init(&c2, &d);
    for (u32 i = 0; i < 30; i++) phys2_character_update(w2, &c2, vec3(0, 0, 0), FALSE, DT);
    CHECK(c2.pos.x == c.pos.x && c2.pos.y == c.pos.y && c2.pos.z == c.pos.z);
    CHECK(c2.last_tests == c.last_tests);
}

/* ---- hata 3: 65 536'dan fazla gövde ---- */

static void many_bodies_scene(Phys2Broadphase mode) {
    test_arena_reset();
    const u32 statics = 65600;   /* dinamik gövdeler 16 bit sınırını (65 535) aşan indekslere düşer */
    Phys2World* w = new_world(statics + 16, 4096);
    w->broadphase = mode;
    Phys2BodyDesc pd = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(w, &pd);
    for (u32 i = 0; i < statics; i++) {   /* x ekseninde sıralı, uzak (y = 50) küçük statik kutular */
        Phys2BodyDesc b = phys2_desc_box(vec3(100.0f + (f32)i * 3.0f, 50.0f, 0), vec3(0.4f, 0.4f, 0.4f), 0.0f);
        phys2_add_body(w, &b);
    }
    Phys2BodyDesc a = phys2_desc_box(vec3(0, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 1.0f);
    Phys2BodyDesc b = phys2_desc_box(vec3(0.9f, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 1.0f);   /* a ile 0,1 m girişim */
    u32 ia = phys2_add_body(w, &a);
    u32 ib = phys2_add_body(w, &b);
    CHECK(ia > 65535 && ib > 65535);
    phys2_step(w, DT);
    CHECK(pair_manifolds(w, ia, ib) == 1);   /* 16 bit anahtarda (ia, ib) çifti başka çiftle karışırdı */
    CHECK(pair_manifolds(w, 0, ia) == 1);    /* düzlem çifti */
    CHECK(pair_manifolds(w, 0, ib) == 1);
    u32 hits[8];
    u32 nh = phys2_query_aabb(w, vec3(-1, 0, -1), vec3(2, 2, 1), hits, 8);
    CHECK(nh == 2 && hits[0] == ia && hits[1] == ib);   /* sorgu sonucu artan ve 16 bit sınırını aşan indeksler doğru */
    for (u32 s = 0; s < 30; s++) phys2_step(w, DT);
    CHECK(w->bodies[ia].pos.y > 0.4f && w->bodies[ia].pos.y < 0.6f);   /* yerde dengeli */
    CHECK(w->bodies[ib].pos.y > 0.4f && w->bodies[ib].pos.y < 0.6f);
}

TEST(phys2_port_more_than_65536_bodies_bvh) { many_bodies_scene(PHYS2_BP_BVH); }
TEST(phys2_port_more_than_65536_bodies_sap) { many_bodies_scene(PHYS2_BP_SAP); }

/* ---- belirlenimcilik: sahne kurucuları ---- */

static void build_pile(Phys2World* w, u32 n, u32 seed) {
    Phys2BodyDesc ground = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(w, &ground);
    Phys2Rng r = phys2_rng_make(seed);
    u32 side = 1;
    while (side * side * side < n) side++;
    u32 k = 0;
    for (u32 y = 0; y < side && k < n; y++) {
        for (u32 z = 0; z < side && k < n; z++) {
            for (u32 x = 0; x < side && k < n; x++, k++) {
                Vec3 p = vec3(((f32)x - (f32)side * 0.5f) * 1.3f + phys2_rng_range(&r, -0.1f, 0.1f), 0.7f + (f32)y * 1.3f,
                              ((f32)z - (f32)side * 0.5f) * 1.3f + phys2_rng_range(&r, -0.1f, 0.1f));
                Phys2BodyDesc b;
                if (k % 7 == 3) b = phys2_desc_sphere(p, 0.5f, 1.0f);
                else if (k % 7 == 5) b = phys2_desc_capsule(p, 0.3f, 0.3f, 1.0f);
                else b = phys2_desc_box(p, vec3(0.5f, 0.5f, 0.5f), 1.0f);
                b.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(&r, -1, 1), 1.0f, phys2_rng_range(&r, -1, 1)), phys2_rng_range(&r, -0.4f, 0.4f));
                phys2_add_body(w, &b);
            }
        }
    }
}

static Phys2TriMesh* bumpy_mesh(void) {
    Arena* a = test_arena();
    const u32 n = 12;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_PUSH_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_PUSH_ARRAY(a, u32, n * n * 6);
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i - 6.0f, z = (f32)j - 6.0f;
            v[j * (n + 1) + i] = vec3(x, 0.3f * m_sin(x * 0.7f) * m_cos(z * 0.5f), z);
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

/* mesh zemin + karışık gövdeler (mesh çiftleri paralel evrede seri tüketilir) */
static void build_mesh_pile(Phys2World* w, const Phys2TriMesh* mesh, u32 n) {
    Phys2BodyDesc m = phys2_desc_mesh(vec3(0, 0, 0), mesh);
    phys2_add_body(w, &m);
    Phys2Rng r = phys2_rng_make(77);
    for (u32 i = 0; i < n; i++) {
        Vec3 p = vec3(phys2_rng_range(&r, -4, 4), 1.0f + (f32)(i % 6) * 1.1f, phys2_rng_range(&r, -4, 4));
        Phys2BodyDesc d;
        if (i % 3 == 0) d = phys2_desc_sphere(p, 0.35f, 1.0f);
        else if (i % 3 == 1) d = phys2_desc_capsule(p, 0.25f, 0.35f, 1.0f);
        else d = phys2_desc_box(p, vec3(0.3f, 0.35f, 0.3f), 1.0f);
        phys2_add_body(w, &d);
    }
}

#define DET_STEPS 90

typedef struct DetRun {
    u32 hash[DET_STEPS];
    u32 max_pairs;
    u32 manifolds;
} DetRun;

static void run_world(Phys2World* w, u32 threads, DetRun* out) {
    w->threads = threads;
    for (u32 s = 0; s < DET_STEPS; s++) {
        phys2_step(w, DT);
        out->hash[s] = phys2_state_hash(w);
        if (w->stats.pairs > out->max_pairs) out->max_pairs = w->stats.pairs;
    }
    out->manifolds = w->stats.manifolds;
}

static b32 same_hashes(const DetRun* a, const DetRun* b) {
    for (u32 s = 0; s < DET_STEPS; s++) {
        if (a->hash[s] != b->hash[s]) return FALSE;
    }
    return TRUE;
}

/* aynı sahne iki koşu → aynı hash dizisi */
TEST(phys2_port_det_two_runs_same_hash) {
    test_arena_reset();
    fake_workers(0);
    DetRun r1 = { { 0 }, 0, 0 }, r2 = { { 0 }, 0, 0 };
    Phys2World* w1 = new_world(512, 8192);
    build_pile(w1, 300, 5);
    run_world(w1, 1, &r1);
    Phys2World* w2 = new_world(512, 8192);
    build_pile(w2, 300, 5);
    run_world(w2, 1, &r2);
    CHECK(same_hashes(&r1, &r2));
    CHECK(r1.hash[DET_STEPS - 1] != r1.hash[0]);   /* sahne gerçekten hareket etti */
}

/* seri (threads = 1) ↔ paralel (8 thread) dar faz: her adımda aynı hash */
TEST(phys2_port_det_narrow_serial_equals_parallel) {
    test_arena_reset();
    DetRun a = { { 0 }, 0, 0 }, b = { { 0 }, 0, 0 };
    fake_workers(0);
    Phys2World* ws = new_world(512, 8192);
    build_pile(ws, 300, 11);
    run_world(ws, 1, &a);
    fake_workers(8);
    Phys2World* wp = new_world(512, 8192);
    build_pile(wp, 300, 11);
    run_world(wp, 0, &b);
    fake_workers(0);
    CHECK(a.max_pairs >= PHYS2_PAR_MIN_PAIRS);   /* paralel yol gerçekten devreye girdi */
    CHECK(same_hashes(&a, &b));
    CHECK(a.manifolds == b.manifolds);
}

TEST(phys2_port_det_narrow_mesh_serial_equals_parallel) {
    test_arena_reset();
    DetRun a = { { 0 }, 0, 0 }, b = { { 0 }, 0, 0 };
    const Phys2TriMesh* mesh = bumpy_mesh();
    fake_workers(0);
    Phys2World* ws = new_world(256, 8192);
    build_mesh_pile(ws, mesh, 180);
    run_world(ws, 1, &a);
    fake_workers(8);
    Phys2World* wp = new_world(256, 8192);
    build_mesh_pile(wp, mesh, 180);
    run_world(wp, 0, &b);
    fake_workers(0);
    CHECK(a.max_pairs >= PHYS2_PAR_MIN_PAIRS);
    CHECK(same_hashes(&a, &b));
}

/* uyku + uyandırma yolu da (adalar uyur, sonra bir top ortaya düşer) seri ↔ paralel aynı */
TEST(phys2_port_det_sleep_wake_serial_equals_parallel) {
    test_arena_reset();
    u32 hs[2][240];
    for (u32 pass = 0; pass < 2; pass++) {
        fake_workers(pass ? 8 : 0);
        Phys2World* w = new_world(256, 8192);
        w->allow_sleep = TRUE;
        w->sleep_delay = 0.2f;
        w->threads = pass ? 0 : 1;
        build_pile(w, 150, 21);
        for (u32 s = 0; s < 240; s++) {
            if (s == 150) {
                Phys2BodyDesc ball = phys2_desc_sphere(vec3(0, 8, 0), 0.6f, 30.0f);
                u32 id = phys2_add_body(w, &ball);
                w->bodies[id].vel = vec3(0, -12, 0);
            }
            phys2_step(w, DT);
            hs[pass][s] = phys2_state_hash(w);
        }
    }
    fake_workers(0);
    b32 same = TRUE;
    for (u32 s = 0; s < 240; s++) same = same && hs[0][s] == hs[1][s];
    CHECK(same);
}

/* ---- kumaş: renklendirme geçerli; renkli çözüm thread sayısından bağımsız ---- */

static Phys2SoftBody* big_cloth(u32 n) {
    Phys2SoftBody* c = phys2_soft_cloth(test_arena(), vec3(0, 10, 0), vec3(8, 0, 0), vec3(0, 0, 8), n, n, 4.0f, 0.0f, 1e-4f, 0.02f);
    for (u32 i = 0; i < n; i++) phys2_soft_pin(c, i);   /* üst sıra */
    c->self_collide = FALSE;
    c->tri_collide = FALSE;
    return c;
}

TEST(phys2_port_cloth_coloring_is_valid) {
    test_arena_reset();
    Phys2SoftBody* c = big_cloth(40);
    phys2_soft_set_colored(test_arena(), c, TRUE);
    CHECK(c->colored);
    CHECK(c->dist_color_start[PHYS2_SOFT_MAX_COLORS] == c->dist_count);
    /* her kısıt tam bir kez ve aynı (taşma dışı) renkteki kısıtlar ortak parçacık paylaşmaz */
    u8* seen = ARENA_PUSH_ARRAY(test_arena(), u8, c->dist_count);
    for (u32 col = 0; col < PHYS2_SOFT_MAX_COLORS; col++) {
        u8* used = ARENA_PUSH_ARRAY(test_arena(), u8, c->count);
        for (u32 q = c->dist_color_start[col]; q < c->dist_color_start[col + 1]; q++) {
            u32 k = c->dist_order[q];
            CHECK(!seen[k]);
            seen[k] = 1;
            if (col == PHYS2_SOFT_MAX_COLORS - 1) continue;
            CHECK(!used[c->dist[k].a] && !used[c->dist[k].b]);
            used[c->dist[k].a] = used[c->dist[k].b] = 1;
        }
    }
    for (u32 k = 0; k < c->dist_count; k++) CHECK(seen[k]);
    /* jöle: tetrahedron renkleri de geçerli */
    Phys2SoftBody* j = phys2_soft_jelly(test_arena(), vec3(0, 3, 0), 1.0f, 3, 2.0f, 1e-4f, 1e-5f);
    phys2_soft_set_colored(test_arena(), j, TRUE);
    CHECK(j->tet_color_start[PHYS2_SOFT_MAX_COLORS] == j->tet_count);
    u8* tseen = ARENA_PUSH_ARRAY(test_arena(), u8, j->tet_count);
    for (u32 col = 0; col < PHYS2_SOFT_MAX_COLORS; col++) {
        u8* used = ARENA_PUSH_ARRAY(test_arena(), u8, j->count);
        for (u32 q = j->tet_color_start[col]; q < j->tet_color_start[col + 1]; q++) {
            u32 t = j->tet_order[q];
            CHECK(!tseen[t]);
            tseen[t] = 1;
            if (col == PHYS2_SOFT_MAX_COLORS - 1) continue;
            for (u32 c4 = 0; c4 < 4; c4++) {
                CHECK(!used[j->tets[t].v[c4]]);
                used[j->tets[t].v[c4]] = 1;
            }
        }
    }
}

static u32 run_cloth(u32 n, b32 colored, u32 threads, u32 steps, f32* stretch) {
    Phys2SoftBody* c = big_cloth(n);
    phys2_soft_set_colored(test_arena(), c, colored);
    Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
    phys2_soft_world_init(sw, test_arena(), 2);
    sw->threads = threads;
    phys2_soft_world_add(sw, c);
    for (u32 s = 0; s < steps; s++) phys2_soft_world_step(sw, NULL, DT);
    if (stretch) *stretch = phys2_soft_max_stretch(c);
    return phys2_soft_hash(sw);
}

TEST(phys2_port_det_cloth_colored_serial_equals_parallel) {
    test_arena_reset();
    const u32 n = 100;   /* renk grupları SOFT_PAR_MIN'i aşar → paralel dilimler gerçekten kullanılır */
    f32 st1, st2, st3, stp;
    fake_workers(0);
    u32 h_seq = run_cloth(n, FALSE, 1, 12, &stp);
    u32 h_col1 = run_cloth(n, TRUE, 1, 12, &st1);
    u32 h_col1b = run_cloth(n, TRUE, 1, 12, &st3);
    fake_workers(8);
    u32 h_col8 = run_cloth(n, TRUE, 0, 12, &st2);
    fake_workers(0);
    CHECK(h_col1 == h_col1b);   /* aynı koşu iki kez */
    CHECK(h_col1 == h_col8);    /* seri renkli ↔ paralel renkli: bit-bit */
    CHECK(st1 == st2 && st1 == st3);
    CHECK(h_seq != h_col1);     /* sıra farklı → renkli çözüm ayrı bir (ama eşit derecede geçerli) sonuç */
    CHECK(st1 < 0.25f);         /* renk sırası kumaşı uzatmadan tutar (dizi sırası bu sahnede çok daha fazla uzar) */
    test_note("kumaş uzaması: dizi sırası", stp, 4);
    test_note("kumaş uzaması: renk sırası", st1, 4);
}

TEST(phys2_port_det_jelly_colored_serial_equals_parallel) {
    test_arena_reset();
    u32 hs[2];
    for (u32 pass = 0; pass < 2; pass++) {
        fake_workers(pass ? 8 : 0);
        Phys2SoftBody* j = phys2_soft_jelly(test_arena(), vec3(0, 2, 0), 1.0f, 4, 2.0f, 1e-5f, 1e-6f);
        phys2_soft_set_colored(test_arena(), j, TRUE);
        Phys2World* pw = new_world(4, 64);
        Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
        phys2_add_body(pw, &g);
        Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
        phys2_soft_world_init(sw, test_arena(), 2);
        sw->threads = pass ? 0 : 1;
        phys2_soft_world_add(sw, j);
        for (u32 s = 0; s < 60; s++) {
            phys2_step(pw, DT);
            phys2_soft_world_step(sw, pw, DT);
        }
        hs[pass] = phys2_soft_hash(sw);
        CHECK(m_abs(phys2_soft_volume(j) / phys2_soft_rest_volume(j) - 1.0f) < 0.2f);
    }
    fake_workers(0);
    CHECK(hs[0] == hs[1]);
}

/* ---- okyanus ---- */

TEST(phys2_port_det_ocean_serial_equals_parallel) {
    test_arena_reset();
    Phys2OceanDesc d = { 128, 80.0f, 1.0f, 0.3f, 14.0f, 0.4f, 0.0f, 1.2f, 4242 };
    Phys2Ocean* a = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    Phys2Ocean* b = ARENA_PUSH_STRUCT(test_arena(), Phys2Ocean);
    phys2_ocean_init(a, test_arena(), &d);
    phys2_ocean_init(b, test_arena(), &d);
    fake_workers(0);
    a->threads = 1;
    phys2_ocean_update(a, 3.7);
    fake_workers(8);
    b->threads = 0;
    phys2_ocean_update(b, 3.7);
    fake_workers(0);
    usize bytes = sizeof(f32) * 128 * 128;
    CHECK(same_bytes(a->height, b->height, bytes));
    CHECK(same_bytes(a->disp_x, b->disp_x, bytes));
    CHECK(same_bytes(a->disp_z, b->disp_z, bytes));
    CHECK(same_bytes(a->slope_x, b->slope_x, bytes));
    CHECK(same_bytes(a->slope_z, b->slope_z, bytes));
    f64 e = 0.0;
    for (u32 i = 0; i < 128 * 128; i++) e += (f64)a->height[i] * a->height[i];
    CHECK(e > 0.0);   /* alan gerçekten üretildi */
}
