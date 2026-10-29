/* tests/test_phys2_soft.c — M9 Faz 5: XPBD yumuşak cisimler.
 * Analitik: uyumlu yayın statik uzaması e = m·g·α (XPBD dengesi tam), sarkaç periyodu 2π√(L/g).
 * Korunum: jöle hacmi (sıkışmaz tetrahedronlar), iki yönlü çarpışmada toplam momentum.
 * Çarpışma: kumaş küre / mesh üstünde içeri girmez, jöleler birbirine girmez. Bayrak rüzgârda kararlı.
 * Belirleyicilik: aynı sahne iki kez → aynı hash.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/pmath.h"
#include "phys2/phys.h"
#include "phys2/shapes.h"
#include "phys2/soft.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* rigid(u32 n) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

static Phys2SoftWorld* softw(void) {
    Phys2SoftWorld* s = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
    phys2_soft_world_init(s, test_arena(), 8);
    return s;
}

/* iğneli üst parçacık + n−1 asılı parçacık, aralık L (−y), her biri m kütleli */
static Phys2SoftBody* chain(u32 n, f32 L, f32 m, f32 compliance, Vec3 dir) {
    Arena* a = test_arena();
    Phys2SoftBody* b = ARENA_PUSH_STRUCT(a, Phys2SoftBody);
    b->count = n;
    b->x = ARENA_PUSH_ARRAY(a, Vec3, n);
    b->prev = ARENA_PUSH_ARRAY(a, Vec3, n);
    b->v = ARENA_PUSH_ARRAY(a, Vec3, n);
    b->w = ARENA_PUSH_ARRAY(a, f32, n);
    b->dist = ARENA_PUSH_ARRAY(a, Phys2SoftDist, n);
    b->tets = NULL;
    b->tris = NULL;
    for (u32 i = 0; i < n; i++) {
        b->x[i] = vec3_scale(dir, L * (f32)i);
        b->prev[i] = b->x[i];
        b->w[i] = i == 0 ? 0.0f : 1.0f / m;
    }
    for (u32 i = 0; i + 1 < n; i++) {
        Phys2SoftDist* d = &b->dist[b->dist_count++];
        d->a = i;
        d->b = i + 1;
        d->rest = L;
        d->compliance = compliance;
        d->kind = PHYS2_SOFT_STRETCH;
    }
    b->radius = 0.01f;
    b->mass = m * (f32)(n - 1);
    return b;
}

TEST(phys2_soft_spring_static_extension) {
    test_arena_reset();
    /* XPBD dengesi: e = m·g·α (alt adım sayısından bağımsız) */
    const f32 m = 2.0f, alpha = 1e-3f;
    static const u32 SUBS[3] = { 5, 10, 20 };
    static const char* const LBL[3] = { "uzama, 5 alt adım (m)", "uzama, 10 alt adım (m)", "uzama, 20 alt adım (m)" };
    for (u32 k = 0; k < 3; k++) {
        Phys2SoftWorld* sw = softw();
        sw->substeps = SUBS[k];
        Phys2SoftBody* b = chain(2, 1.0f, m, alpha, vec3(0, -1, 0));
        b->damping = 3.0f;
        phys2_soft_world_add(sw, b);
        for (u32 s = 0; s < 300; s++) phys2_soft_world_step(sw, NULL, DT);
        f32 e = -b->x[1].y - 1.0f;
        test_note(LBL[k], e, 5);
        /* 20 alt adımda g·h² ≈ 6,8e-6 m, konum ~1 m'de f32 adımının ~57 katı → yuvarlama ~%1-2 */
        CHECK_NEAR(e, m * 9.81f * alpha, (k == 2 ? 0.02f : 0.01f) * m * 9.81f * alpha);
    }
    test_note("analitik m·g·α (m)", m * 9.81f * alpha, 5);
}

TEST(phys2_soft_pendulum_period) {
    test_arena_reset();
    const f32 L = 2.0f, th = 0.1f;
    Phys2SoftWorld* sw = softw();
    sw->substeps = 10;
    Phys2SoftBody* b = chain(2, L, 1.0f, 0.0f, vec3(m_sin(th), -m_cos(th), 0));
    phys2_soft_world_add(sw, b);
    f32 t = 0.0f, last_x = b->x[1].x, first = -1.0f, cross_t = 0.0f;
    u32 crossings = 0;
    for (u32 s = 0; s < 60 * 12; s++) {
        phys2_soft_world_step(sw, NULL, DT);
        t += DT;
        f32 x = b->x[1].x;
        if (last_x > 0.0f && x <= 0.0f) {
            f32 tc = t - DT * x / (x - last_x);   /* doğrusal ara değer */
            if (first < 0.0f) first = tc;
            cross_t = tc;
            crossings++;
        }
        last_x = x;
    }
    f32 period = (cross_t - first) / (f32)(crossings - 1);
    f32 ana = M_TAU * m_sqrt(L / 9.81f) * (1.0f + th * th / 16.0f);
    test_note("sarkaç periyodu (s)", period, 4);
    test_note("analitik (s)", ana, 4);
    CHECK(crossings >= 3);
    CHECK_NEAR(period, ana, 0.005f * ana);
    test_note("uzunluk hatası (m)", m_abs(vec3_len(b->x[1]) - L), 6);
    CHECK(m_abs(vec3_len(b->x[1]) - L) < 1e-3f);
}

TEST(phys2_soft_jelly_volume_and_ground) {
    test_arena_reset();
    Phys2World* pw = rigid(4);
    Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(pw, &g);
    Phys2SoftWorld* sw = softw();
    Phys2SoftBody* j = phys2_soft_jelly(test_arena(), vec3(0, 1.5f, 0), 1.0f, 3, 5.0f, 2e-4f, 0.0f);
    phys2_soft_world_add(sw, j);
    f32 v0 = phys2_soft_rest_volume(j), vmin = v0, vmax = v0, lowest = 1e9f;
    for (u32 s = 0; s < 240; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
        f32 v = phys2_soft_volume(j);
        vmin = m_min(vmin, v);
        vmax = m_max(vmax, v);
        for (u32 i = 0; i < j->count; i++) lowest = m_min(lowest, j->x[i].y);
    }
    f32 top = -1e9f;
    for (u32 i = 0; i < j->count; i++) top = m_max(top, j->x[i].y);
    f64 ke = 0.0;
    for (u32 i = 0; i < j->count; i++) ke += 0.5 * (f64)vec3_dot(j->v[i], j->v[i]) / j->w[i];
    test_note("jöle hacmi en küçük / dinlenme", vmin / v0, 4);
    test_note("jöle hacmi en büyük / dinlenme", vmax / v0, 4);
    test_note("en alçak parçacık y (yarıçap)", lowest, 4);
    test_note("", j->radius, 4);
    test_note("oturmuş kafes yüksekliği (dinlenme 0,75 m)", top - lowest, 4);
    test_note("kinetik enerji (J)", ke, 5);
    CHECK(vmin > 0.97f * v0 && vmax < 1.03f * v0);
    CHECK(lowest > j->radius - 0.01f);
    CHECK(ke < 0.01);
}

TEST(phys2_soft_cloth_on_sphere_and_mesh) {
    test_arena_reset();
    /* küre */
    Phys2World* pw = rigid(4);
    Phys2BodyDesc sd = phys2_desc_sphere(vec3(0, 0, 0), 0.6f, 0.0f);
    phys2_add_body(pw, &sd);
    Phys2SoftWorld* sw = softw();
    Phys2SoftBody* c = phys2_soft_cloth(test_arena(), vec3(-1.2f, 1.0f, -1.2f), vec3(2.4f, 0, 0), vec3(0, 0, 2.4f), 20, 20, 0.5f, 0.0f, 1e-5f, 1e-3f);
    phys2_soft_world_add(sw, c);
    f32 worst = 1e9f;
    for (u32 s = 0; s < 180; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
        for (u32 i = 0; i < c->count; i++) worst = m_min(worst, vec3_len(c->x[i]) - 0.6f);
    }
    test_note("kumaş-küre: en küçük yüzey uzaklığı (yarıçap)", worst, 4);
    test_note("", c->radius, 4);
    test_note("kumaş-küre: en büyük uzama", phys2_soft_max_stretch(c), 4);
    CHECK(worst > 0.5f * c->radius);
    CHECK(phys2_soft_max_stretch(c) < 0.1f);

    /* eğimli mesh: y = 0.3x + 0.1 sin(z) değil — düz eğim, köşe paylaşan ızgara */
    Phys2World* pm = rigid(4);
    const u32 n = 16;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_PUSH_ARRAY(test_arena(), Vec3, nv);
    u32* idx = ARENA_PUSH_ARRAY(test_arena(), u32, n * n * 6);
    for (u32 jj = 0; jj <= n; jj++) {
        for (u32 ii = 0; ii <= n; ii++) {
            f32 x = (f32)ii * 0.5f - 4.0f, z = (f32)jj * 0.5f - 4.0f;
            v[jj * (n + 1) + ii] = vec3(x, 0.3f * x + 0.2f * m_abs(z), z);
        }
    }
    u32 k = 0;
    for (u32 jj = 0; jj < n; jj++) {
        for (u32 ii = 0; ii < n; ii++) {
            u32 a0 = jj * (n + 1) + ii, a1 = a0 + 1, b0 = a0 + n + 1, b1 = b0 + 1;
            idx[k++] = a0; idx[k++] = b0; idx[k++] = b1;
            idx[k++] = a0; idx[k++] = b1; idx[k++] = a1;
        }
    }
    Phys2BodyDesc md = phys2_desc_mesh(vec3(0, 0, 0), phys2_trimesh_create(test_arena(), v, nv, idx, n * n * 2));
    phys2_add_body(pm, &md);
    Phys2SoftWorld* sm = softw();
    Phys2SoftBody* c2 = phys2_soft_cloth(test_arena(), vec3(-1.5f, 2.0f, -1.5f), vec3(3, 0, 0), vec3(0, 0, 3), 16, 16, 0.4f, 0.0f, 1e-5f, 1e-3f);
    phys2_soft_world_add(sm, c2);
    f32 below = 1e9f;
    for (u32 s = 0; s < 180; s++) {
        phys2_step(pm, DT);
        phys2_soft_world_step(sm, pm, DT);
        for (u32 i = 0; i < c2->count; i++) {
            Vec3 p = c2->x[i];
            if (m_abs(p.x) < 3.9f && m_abs(p.z) < 3.9f) below = m_min(below, p.y - (0.3f * p.x + 0.2f * m_abs(p.z)));
        }
    }
    test_note("kumaş-mesh: yüzeyin en çok altına / üstüne (dikey)", below, 4);
    CHECK(below > -0.01f);
}

TEST(phys2_soft_two_way_momentum) {
    test_arena_reset();
    Phys2World* pw = rigid(4);
    pw->gravity = vec3(0, 0, 0);
    Phys2BodyDesc bd = phys2_desc_box(vec3(2.0f, 0, 0), vec3(0.4f, 0.4f, 0.4f), 2.0f);
    bd.friction = 0.0f;
    u32 box = phys2_add_body(pw, &bd);
    Phys2SoftWorld* sw = softw();
    sw->gravity = vec3(0, 0, 0);
    Phys2SoftBody* j = phys2_soft_jelly(test_arena(), vec3(0, 0, 0), 0.8f, 2, 2.0f, 1e-4f, 0.0f);
    j->damping = 0.0f;
    j->friction = 0.0f;
    phys2_soft_set_velocity(j, vec3(4.0f, 0, 0));
    phys2_soft_world_add(sw, j);
    Vec3 p0 = vec3_add(phys2_soft_momentum(j), vec3_scale(pw->bodies[box].vel, 2.0f));
    for (u32 s = 0; s < 90; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
    }
    Vec3 pj = phys2_soft_momentum(j), pb = vec3_scale(pw->bodies[box].vel, 2.0f);
    Vec3 p1 = vec3_add(pj, pb);
    test_note("momentum önce (kg·m/s)", p0.x, 4);
    test_note("momentum sonra: jöle", pj.x, 4);
    test_note("momentum sonra: kutu", pb.x, 4);
    test_note("toplam", p1.x, 4);
    CHECK(pb.x > 3.0f);                          /* kutu itildi */
    CHECK(m_abs(p1.x - p0.x) < 0.03f * p0.x);
    CHECK(m_abs(p1.y) < 0.05f && m_abs(p1.z) < 0.05f);
}

TEST(phys2_soft_flag_in_wind) {
    test_arena_reset();
    /* 0: bağsız 10 alt adım, 1: bağlı 10, 2: bağlı 20 */
    for (u32 run = 0; run < 3; run++) {
        b32 tether = run > 0;
        Phys2SoftWorld* sw = softw();
        sw->substeps = run == 2 ? 20 : 10;
        sw->wind = vec3(1.0f, 0.0f, 7.0f);
        const u32 nu = 24, nv = 16;
        /* 3×2 m naylon ≈ 150 g/m² → 0,9 kg; başta rüzgâra dik (≈ 176 N), ilk 2 s ölçülmez */
        Phys2SoftBody* f = phys2_soft_cloth(test_arena(), vec3(0, 3, 0), vec3(3, 0, 0), vec3(0, -2, 0), nu, nv, 0.9f, 0.0f, 1e-4f, 0.05f);
        for (u32 j = 0; j < nv; j++) phys2_soft_pin(f, j * nu);
        if (tether) phys2_soft_add_tethers(test_arena(), f);
        phys2_soft_world_add(sw, f);
        f32 vmax = 0.0f, stretch = 0.0f;
        f64 ms = 0.0;
        b32 finite = TRUE;
        for (u32 s = 0; s < 300; s++) {
            phys2_soft_world_step(sw, NULL, DT);
            ms += sw->step_ms;
            if (s < 120) continue;
            stretch = m_max(stretch, phys2_soft_max_stretch(f));
            for (u32 i = 0; i < f->count; i++) {
                f32 sp = vec3_len(f->v[i]);
                if (!(sp == sp)) finite = FALSE;
                vmax = m_max(vmax, sp);
            }
        }
        Vec3 c = vec3(0, 0, 0);
        for (u32 i = 0; i < f->count; i++) c = vec3_add(c, f->x[i]);
        c = vec3_scale(c, 1.0f / (f32)f->count);
        static const char* const LBL[3] = { "bağsız, 10 alt adım: en büyük uzama", "bağlı, 10 alt adım: en büyük uzama",
                                            "bağlı, 20 alt adım: en büyük uzama" };
        test_note(LBL[run], stretch, 4);
        test_note("  rüzgâr yönünde ağırlık merkezi z (m)", c.z, 3);
        test_note("  en büyük hız (m/s)", vmax, 2);
        test_note("  ortalama adım ms (384 parçacık)", ms / 300.0, 3);
        CHECK(finite);
        CHECK(c.z > 0.5f);
        CHECK(vmax < 30.0f);
        CHECK(stretch < 0.06f);   /* tepe değer, serbest kenarın kırbaç anlarında (alt adımla ancak yavaş düşüyor) */
    }
}

TEST(phys2_soft_jellies_stack) {
    test_arena_reset();
    Phys2World* pw = rigid(4);
    Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(pw, &g);
    Phys2SoftWorld* sw = softw();
    Phys2SoftBody* a = phys2_soft_jelly(test_arena(), vec3(0, 0.6f, 0), 1.0f, 3, 4.0f, 2e-4f, 0.0f);
    /* not: büyük kaydırmada (≈ 0,8 kafes adımı) üstteki jöle küre "yumurta kolisi" yüzeyinde yavaşça kayıp
     * düşüyor (bilinen sınırlama: parçacık-parçacık teması, yüzey üçgeni yok) */
    Phys2SoftBody* b = phys2_soft_jelly(test_arena(), vec3(0.08f, 2.0f, 0.04f), 1.0f, 3, 4.0f, 2e-4f, 0.0f);
    phys2_soft_world_add(sw, a);
    phys2_soft_world_add(sw, b);
    for (u32 s = 0; s < 240; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
    }
    f32 dmin = 1e9f;
    for (u32 i = 0; i < a->count; i++) {
        for (u32 k = 0; k < b->count; k++) dmin = m_min(dmin, vec3_len(vec3_sub(a->x[i], b->x[k])));
    }
    f32 ya = 0, yb = 0;
    for (u32 i = 0; i < a->count; i++) ya += a->x[i].y;
    for (u32 i = 0; i < b->count; i++) yb += b->x[i].y;
    ya /= (f32)a->count;
    yb /= (f32)b->count;
    test_note("iki jöle: en yakın parçacık çifti (m)", dmin, 4);
    test_note("temas mesafesi rA + rB (m)", a->radius + b->radius, 4);
    test_note("alt / üst ağırlık merkezi y", ya, 3);
    test_note("", yb, 3);
    CHECK(dmin > 0.8f * (a->radius + b->radius));
    CHECK(yb > ya + 0.7f);
    CHECK(phys2_soft_volume(b) > 0.97f * phys2_soft_rest_volume(b));
}

static u32 mixed_scene_hash(void) {
    Phys2World* pw = rigid(16);
    Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(pw, &g);
    Phys2BodyDesc bx = phys2_desc_box(vec3(0.3f, 0.5f, 0), vec3(0.5f, 0.5f, 0.5f), 3.0f);
    phys2_add_body(pw, &bx);
    Phys2BodyDesc sp = phys2_desc_sphere(vec3(-1.5f, 3.0f, 0.2f), 0.3f, 2.0f);
    sp.vel = vec3(2, 0, 0);
    phys2_add_body(pw, &sp);
    Phys2SoftWorld* sw = softw();
    sw->wind = vec3(0, 0, 4);
    Phys2SoftBody* j = phys2_soft_jelly(test_arena(), vec3(0.2f, 1.8f, 0), 0.8f, 2, 2.0f, 2e-4f, 0.0f);
    Phys2SoftBody* f = phys2_soft_cloth(test_arena(), vec3(-1, 3, -1), vec3(2, 0, 0), vec3(0, 0, 2), 10, 10, 0.3f, 0.0f, 1e-4f, 0.01f);
    phys2_soft_world_add(sw, j);
    phys2_soft_world_add(sw, f);
    for (u32 s = 0; s < 120; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
    }
    return phys2_soft_hash(sw) ^ (phys2_state_hash(pw) * 31u);
}

TEST(phys2_soft_determinism) {
    test_arena_reset();
    u32 a = mixed_scene_hash(), b = mixed_scene_hash();
    test_note("hash", a, 0);
    CHECK(a == b);
}
