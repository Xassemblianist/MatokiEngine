/* tests/test_phys2_cloth.c — M9 Faz 8: kumaş öz-çarpışması ve rijit ↔ kumaş üçgeni teması.
 * Mermi / top bayrağa: üçgen teması kapalıyken parçacıkların arasından geçer, açıkken geçmez (bayrak ağ gibi
 * tutar ya da saptırır). Dikey bırakılan kumaş yerde katlanır: öz-çarpışma kapalıyken katmanlar iç içe,
 * açıkken parçacık çiftleri kalınlığı korur. Belirleyicilik ve maliyet ölçülür.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/phys.h"
#include "phys2/soft.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* rigid(u32 n) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

static Phys2SoftBody* flag(Vec3 origin) {
    const u32 nu = 24, nv = 16;
    Phys2SoftBody* f = phys2_soft_cloth(test_arena(), origin, vec3(3, 0, 0), vec3(0, -2, 0), nu, nv, 0.9f, 0.0f, 1e-4f, 0.05f);
    for (u32 j = 0; j < nv; j++) phys2_soft_pin(f, j * nu);
    phys2_soft_add_tethers(test_arena(), f);
    return f;
}

typedef struct ShotResult {
    b32 passed;      /* mermi kumaşın tamamının ötesine geçti mi */
    f32 end_speed;   /* son hız (m/s) */
    f32 min_z;       /* merminin ulaştığı en küçük z (bayrak z = 0'da) */
    f32 stretch;     /* bayrağın son en büyük uzaması */
    u32 tri_contacts;
} ShotResult;

/* bayrak x-y düzleminde (z = 0), mermi +z'den −z yönünde ortasına */
static ShotResult shoot(b32 on, f32 radius, f32 mass, f32 speed, b32 bullet) {
    Phys2World* pw = rigid(8);
    Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
    phys2_soft_world_init(sw, test_arena(), 2);
    Phys2SoftBody* f = flag(vec3(0, 3, 0));
    f->tri_collide = f->catch_fast = on;
    phys2_soft_world_add(sw, f);
    Phys2BodyDesc d = phys2_desc_sphere(vec3(1.6f, 2.05f, 6.0f), radius, mass);
    d.vel = vec3(0, 0, -speed);
    u32 id = phys2_add_body(pw, &d);
    if (bullet) pw->bodies[id].flags |= PHYS2_BODY_BULLET;
    pw->bodies[id].flags |= PHYS2_BODY_NO_SLEEP;
    ShotResult r = { FALSE, 0, 1e9f, 0, 0 };
    for (u32 s = 0; s < 90; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
        r.tri_contacts += sw->tri_contacts;
        const Phys2Body* b = &pw->bodies[id];
        r.min_z = m_min(r.min_z, b->pos.z);
        f32 cz = 1e9f;
        for (u32 i = 0; i < f->count; i++) cz = m_min(cz, f->x[i].z);
        if (b->pos.z < cz - radius) r.passed = TRUE;
    }
    r.end_speed = vec3_len(pw->bodies[id].vel);
    r.stretch = phys2_soft_max_stretch(f);
    return r;
}

TEST(phys2_cloth_stops_bullet) {
    test_arena_reset();
    /* 0: mermi 50 g, 1: top 4 kg, 2: mermi 1 kg (yalnız bilgi: 160 N·s, 0,9 kg bayrak fiziksel olarak tutamaz) */
    static const char* const L[3][2] = {
        { "mermi 160 m/s (r 0,12, 50 g), üçgen + yakalama KAPALI: geçti", "mermi 50 g, AÇIK: geçti" },
        { "top 28 m/s (r 0,4, 4 kg), KAPALI: geçti", "top 4 kg, AÇIK: geçti" },
        { "mermi 160 m/s, 1 kg, KAPALI: geçti", "mermi 1 kg, AÇIK: geçti" },
    };
    static const f32 R[3] = { 0.12f, 0.4f, 0.12f }, M[3] = { 0.05f, 4.0f, 1.0f }, V[3] = { 160.0f, 28.0f, 160.0f };
    for (u32 c = 0; c < 3; c++) {
        for (u32 on = 0; on < 2; on++) {
            ShotResult r = shoot(on, R[c], M[c], V[c], c != 1);
            test_note(L[c][on], r.passed, 0);
            test_note("  son hız (m/s)", r.end_speed, 2);
            test_note("  en küçük z (bayrak 0'da)", r.min_z, 2);
            test_note("  bayrak son en büyük uzama", r.stretch, 3);
            if (c == 2) continue;
            if (on) CHECK(!r.passed);
            else CHECK(r.passed);   /* sorunu doğrular: parçacık teması tek başına tutmuyor */
        }
    }
}

/* dikey kumaş yere düşüp katlanır; dinlenmede uzak çiftlerin en küçük mesafesi */
static void pile(b32 self, f32* min_d, u32* deep, f64* ms) {
    Phys2World* pw = rigid(4);
    Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    g.friction = 0.6f;
    phys2_add_body(pw, &g);
    Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
    phys2_soft_world_init(sw, test_arena(), 2);
    Phys2SoftBody* c = phys2_soft_cloth(test_arena(), vec3(-0.6f, 2.6f, 0), vec3(1.2f, 0, 0), vec3(0.15f, -2.4f, 0.1f), 12, 24, 0.3f, 0.0f, 1e-4f, 0.02f);
    c->self_collide = self;
    c->drag = 0.0f;
    phys2_soft_world_add(sw, c);
    *ms = 0.0;
    for (u32 s = 0; s < 180; s++) {
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
        *ms += sw->step_ms;
    }
    *ms /= 180.0;
    f32 d = 2.0f * c->radius;
    *min_d = 1e9f;
    *deep = 0;
    for (u32 i = 0; i < c->count; i++) {
        for (u32 j = i + 1; j < c->count; j++) {
            Vec3 r0 = vec3_sub(c->x0[i], c->x0[j]);
            if (vec3_dot(r0, r0) < d * d) continue;
            f32 l = vec3_len(vec3_sub(c->x[i], c->x[j]));
            *min_d = m_min(*min_d, l);
            if (l < 0.5f * d) (*deep)++;
        }
    }
}

TEST(phys2_cloth_self_collision_pile) {
    test_arena_reset();
    f32 dm[2];
    u32 deep[2];
    f64 ms[2];
    for (u32 k = 0; k < 2; k++) pile(k == 1, &dm[k], &deep[k], &ms[k]);
    test_note("katlanan kumaş, öz-çarpışma KAPALI: en yakın uzak çift (m)", dm[0], 4);
    test_note("  yarı kalınlıktan yakın çift", deep[0], 0);
    test_note("  adım ms", ms[0], 3);
    test_note("öz-çarpışma AÇIK: en yakın uzak çift (m)", dm[1], 4);
    test_note("  yarı kalınlıktan yakın çift", deep[1], 0);
    test_note("  adım ms", ms[1], 3);
    CHECK(deep[0] > 0);    /* sorunu doğrular */
    CHECK(deep[1] == 0);
}

static u32 flag_scene_hash(f64* ms_out) {
    Phys2World* pw = rigid(16);
    Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
    phys2_soft_world_init(sw, test_arena(), 2);
    sw->wind = vec3(2, 0, 6);
    phys2_soft_world_add(sw, flag(vec3(0, 3, 0)));
    f64 ms = 0.0;
    for (u32 s = 0; s < 150; s++) {
        if (s % 30 == 10) {
            Phys2BodyDesc d = phys2_desc_sphere(vec3(0.5f + (f32)(s % 7) * 0.3f, 2.0f, 8.0f), 0.12f, 0.05f);
            d.vel = vec3(0, 0, -160);
            u32 id = phys2_add_body(pw, &d);
            pw->bodies[id].flags |= PHYS2_BODY_BULLET;
        }
        phys2_step(pw, DT);
        phys2_soft_world_step(sw, pw, DT);
        ms += sw->step_ms;
    }
    if (ms_out) *ms_out = ms / 150.0;
    return phys2_soft_hash(sw) ^ (phys2_state_hash(pw) * 31u);
}

TEST(phys2_cloth_determinism_and_cost) {
    test_arena_reset();
    f64 ms;
    u32 a = flag_scene_hash(&ms), b = flag_scene_hash(NULL);
    test_note("rüzgârda bayrak + 5 mermi: hash", a, 0);
    test_note("  ortalama yumuşak adım ms (öz-çarpışma + üçgen teması açık)", ms, 3);
    CHECK(a == b);
    /* maliyet: aynı bayrak (rüzgâr, mermisiz) özellikler kapalı / açık */
    for (u32 k = 0; k < 4; k++) {
        Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(test_arena(), Phys2SoftWorld);
        phys2_soft_world_init(sw, test_arena(), 2);
        sw->wind = vec3(2, 0, 6);
        Phys2SoftBody* f = flag(vec3(0, 3, 0));
        f->self_collide = (k & 1) != 0;
        f->tri_collide = (k & 2) != 0;
        phys2_soft_world_add(sw, f);
        Phys2World* pw = rigid(4);
        Phys2BodyDesc pole = phys2_desc_box(vec3(-0.1f, 1.5f, 0), vec3(0.08f, 1.5f, 0.08f), 0.0f);
        phys2_add_body(pw, &pole);
        f64 t = 0.0;
        for (u32 s = 0; s < 120; s++) {
            phys2_step(pw, DT);
            phys2_soft_world_step(sw, pw, DT);
            t += sw->step_ms;
        }
        static const char* const L[4] = { "bayrak adım ms: ikisi kapalı", "  öz-çarpışma", "  üçgen teması", "  ikisi açık" };
        test_note(L[k], t / 120.0, 3);
    }
}
