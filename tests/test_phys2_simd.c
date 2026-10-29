/* tests/test_phys2_simd.c — M9 Faz 6: kısıt grafiği renklendirmesi + SIMD temas çözücüsü.
 * Bit-bit eşitlik: aynı sahne renk sırasıyla skaler ve SIMD çözülür, durum hash'i HER adımda karşılaştırılır
 * (kutu yığını, piramit, karışık şekiller + mesh + eklemler, warm start kapalı, uyku açık).
 * Host derlemesinde SIMD yolu yoktur (PHYS2_HAVE_SIMD = 0): test renk sırasının belirleyiciliğini sınar; SIMD ↔ skaler
 * eşitliği wasm'da koşar (node tests/test_phys2_wasm.mjs). Renk geçerliliği: aynı renkteki manifoldlar ortak dinamik gövde paylaşmaz.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/pmath.h"
#include "phys2/phys.h"
#include "phys2/shapes.h"
#include "phys2/solver.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* world(u32 n) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

static Phys2TriMesh* bumpy(void) {
    Arena* a = test_arena();
    const u32 n = 16;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_PUSH_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_PUSH_ARRAY(a, u32, n * n * 6);
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
    return phys2_trimesh_create(a, v, nv, idx, n * n * 2);
}

static const Phys2ConvexHull* g_rock;

/* sahne 0: rastgele düşen kutu yığını, 1: piramit, 2: karışık şekiller + mesh + eklemler */
static void build(Phys2World* w, u32 scene, const Phys2TriMesh* mesh) {
    Phys2Rng r = phys2_rng_make(321 + scene);
    if (scene == 2) {
        Phys2BodyDesc m = phys2_desc_mesh(vec3(0, 0, 0), mesh);
        phys2_add_body(w, &m);
    } else {
        Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
        phys2_add_body(w, &g);
    }
    if (scene == 0) {
        for (u32 i = 0; i < 120; i++) {
            Phys2BodyDesc b = phys2_desc_box(vec3(phys2_rng_range(&r, -2, 2), 0.6f + (f32)i * 0.35f, phys2_rng_range(&r, -2, 2)), vec3(0.4f, 0.3f, 0.5f), 1.0f);
            b.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(&r, -1, 1), 1, phys2_rng_range(&r, -1, 1)), phys2_rng_range(&r, -1, 1));
            phys2_add_body(w, &b);
        }
    } else if (scene == 1) {
        for (u32 l = 0; l < 8; l++) {
            for (u32 iz = 0; iz < 8 - l; iz++) {
                for (u32 ix = 0; ix < 8 - l; ix++) {
                    f32 o = (f32)(7 - l) * 0.5f;
                    Phys2BodyDesc b = phys2_desc_box(vec3(((f32)ix - o) * 1.01f, 0.5f + (f32)l, ((f32)iz - o) * 1.01f), vec3(0.5f, 0.5f, 0.5f), 1.0f);
                    phys2_add_body(w, &b);
                }
            }
        }
        Phys2BodyDesc ball = phys2_desc_sphere(vec3(-12, 4, 0), 0.6f, 20.0f);
        ball.vel = vec3(14, 0, 0);
        phys2_add_body(w, &ball);
    } else {
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
    }
}

/* SIMD ile skaler renk sırası arasındaki ilk farklı adım (0 = hiç) */
static u32 first_mismatch(u32 scene, b32 warm, b32 sleep, u32 steps, u32* colors, u32* overflow) {
    const Phys2TriMesh* mesh = scene == 2 ? bumpy() : NULL;
    Phys2World* a = world(512);
    Phys2World* b = world(512);
    a->simd = FALSE;
    b->simd = TRUE;
    a->warm_start = b->warm_start = warm;
    a->allow_sleep = b->allow_sleep = sleep;
    build(a, scene, mesh);
    build(b, scene, mesh);
    *colors = *overflow = 0;
    for (u32 s = 1; s <= steps; s++) {
        phys2_step(a, DT);
        phys2_step(b, DT);
        *colors = a->stats.colors > *colors ? a->stats.colors : *colors;
        *overflow = a->stats.overflow > *overflow ? a->stats.overflow : *overflow;
        if (phys2_state_hash(a) != phys2_state_hash(b)) return s;
        if (PHYS2_HAVE_SIMD && b->wide_count == 0 && b->cur_count > 8) return 0xFFFFFFFFu;   /* SIMD yolu çalışmadı (yalnız wasm simd128) */
    }
    return 0;
}

TEST(phys2_simd_bit_exact) {
    test_arena_reset();
    Vec3 pts[12];
    Phys2Rng r = phys2_rng_make(4);
    for (u32 i = 0; i < 12; i++) pts[i] = vec3(phys2_rng_range(&r, -0.5f, 0.5f), phys2_rng_range(&r, -0.4f, 0.4f), phys2_rng_range(&r, -0.5f, 0.5f));
    g_rock = phys2_hull_create(test_arena(), pts, 12);
    CHECK(g_rock != NULL);
    static const char* const NAMES[4] = { "yığın", "piramit + top", "karışık + mesh + eklem", "yığın, warm start yok + uyku" };
    for (u32 c = 0; c < 4; c++) {
        u32 colors, overflow;
        u32 scene = c == 3 ? 0 : c;
        u32 bad = first_mismatch(scene, c != 3, c == 3, 240, &colors, &overflow);
        test_note(NAMES[c], bad, 0);
        test_note("  en çok renk", colors, 0);
        test_note("  en çok taşma manifoldu", overflow, 0);
        CHECK(bad == 0);
    }
}

TEST(phys2_graph_coloring_valid) {
    test_arena_reset();
    Phys2World* w = world(512);
    build(w, 1, NULL);
    for (u32 s = 0; s < 30; s++) phys2_step(w, DT);
    u32 bad = 0;
    for (u32 c = 0; c < PHYS2_COLORS; c++) {
        for (u32 i = w->color_start[c]; i < w->color_start[c + 1]; i++) {
            const Phys2Manifold* mi = &w->cur[w->color_order[i]];
            for (u32 j = i + 1; j < w->color_start[c + 1]; j++) {
                const Phys2Manifold* mj = &w->cur[w->color_order[j]];
                const u32 bi[2] = { mi->a, mi->b }, bj[2] = { mj->a, mj->b };
                for (u32 p = 0; p < 2; p++) {
                    for (u32 q = 0; q < 2; q++) {
                        if (bi[p] == bj[q] && w->bodies[bi[p]].inv_mass > 0.0f) bad++;
                    }
                }
            }
        }
    }
    /* her manifold tam bir kez sıralamada */
    u32 seen = 0;
    for (u32 i = 0; i < w->color_start[PHYS2_COLORS + 1]; i++) seen += w->color_order[i] < w->cur_count;
    test_note("manifold", w->cur_count, 0);
    test_note("renk", w->stats.colors, 0);
    test_note("aynı renkte ortak dinamik gövde", bad, 0);
    CHECK(bad == 0);
    CHECK(seen == w->cur_count && w->color_start[PHYS2_COLORS + 1] == w->cur_count);
}
