/* tests/test_phys2_game.c — oyun fiziği testleri (M9 Faz 3): raycast, araç, karakter.
 * Analitik beklentiler: süspansiyon oturma sıkışması m·g/(4k), fren mesafesi v²/(2a),
 * zıplama tepe yüksekliği v²/(2g). Davranışsal: duvar, kayma, basamak, eğim, itme, belirleyicilik.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#include "test.h"
#include "test_phys2_common.h"
#include "core/math.h"
#include "phys2/character.h"
#include "phys2/vehicle.h"

static const f32 DT = 1.0f / 60.0f;

static Phys2World* world(u32 n) {
    Phys2World* w = ARENA_PUSH_STRUCT(test_arena(), Phys2World);
    phys2_world_init(w, test_arena(), n, n * 8 + 32);
    return w;
}

static u32 add_ground(Phys2World* w) {
    Phys2BodyDesc p = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    return phys2_add_body(w, &p);
}

static u32 add_static_box(Phys2World* w, Vec3 pos, Vec3 half, Phys2Quat rot) {
    Phys2BodyDesc b = phys2_desc_box(pos, half, 0.0f);
    b.rot = rot;
    return phys2_add_body(w, &b);
}

/* ---- raycast ---- */

TEST(phys2_raycast_shapes) {
    test_arena_reset();
    Phys2World* w = world(16);
    u32 ground = add_ground(w);
    Phys2BodyDesc s = phys2_desc_sphere(vec3(0, 0, -10), 1.0f, 1.0f);
    u32 sph = phys2_add_body(w, &s);
    u32 box = add_static_box(w, vec3(5, 0, 0), vec3(1, 1, 1), phys2_quat_from_axis_angle(vec3(1, 0, 0), 0.5f));
    u32 top = add_static_box(w, vec3(0, 2, 0), vec3(0.5f, 0.5f, 0.5f), phys2_quat_identity());
    Phys2RayHit h;

    CHECK(phys2_raycast(w, vec3(0, 5, 0), vec3(0, -1, 0), 100.0f, PHYS2_NO_BODY, &h));
    CHECK(h.body == top);
    CHECK_NEAR(h.t, 2.5, 1e-5);
    CHECK_NEAR(h.normal.y, 1.0, 1e-5);

    CHECK(phys2_raycast(w, vec3(0, 5, 0), vec3(0, -1, 0), 100.0f, top, &h));
    CHECK(h.body == ground);
    CHECK_NEAR(h.t, 5.0, 1e-5);

    CHECK(phys2_raycast(w, vec3(0, 0, 0.0f), vec3(0, 0, -1), 100.0f, top, &h));
    CHECK(h.body == sph);
    CHECK_NEAR(h.t, 9.0, 1e-4);
    CHECK_NEAR(h.normal.z, 1.0, 1e-4);

    /* x ekseni etrafında dönmüş kutu: x yüzü değişmez */
    CHECK(phys2_raycast(w, vec3(0, 0, 0), vec3(1, 0, 0), 100.0f, PHYS2_NO_BODY, &h));
    CHECK(h.body == box);
    CHECK_NEAR(h.t, 4.0, 1e-4);
    CHECK_NEAR(h.normal.x, -1.0, 1e-4);

    /* menzil dışı ve ıska */
    CHECK(!phys2_raycast(w, vec3(0, 0, 0), vec3(1, 0, 0), 3.9f, PHYS2_NO_BODY, &h));
    CHECK(!phys2_raycast(w, vec3(0, 5, 0), vec3(0, 1, 0), 100.0f, PHYS2_NO_BODY, &h));

    /* kutunun içinden başlayan ışın: t = 0 */
    CHECK(phys2_raycast(w, vec3(5, 0, 0), vec3(0, 0, 1), 100.0f, PHYS2_NO_BODY, &h));
    CHECK(h.body == box);
    CHECK_NEAR(h.t, 0.0, 1e-6);
}

/* ---- araç ---- */

static Phys2Vehicle* car_world(Phys2World** out_w) {
    Phys2World* w = world(64);
    w->allow_sleep = TRUE;
    add_ground(w);
    Phys2Vehicle* v = ARENA_PUSH_STRUCT(test_arena(), Phys2Vehicle);
    Phys2VehicleDesc d = phys2_vehicle_desc_default(vec3(0, 1.3f, 0));
    phys2_vehicle_create(w, v, &d);
    *out_w = w;
    return v;
}

static void drive(Phys2World* w, Phys2Vehicle* v, u32 steps) {
    for (u32 i = 0; i < steps; i++) {
        phys2_vehicle_update(w, v, DT);
        phys2_step(w, DT);
    }
}

TEST(phys2_vehicle_settles_at_rest_height) {
    test_arena_reset();
    Phys2World* w;
    Phys2Vehicle* v = car_world(&w);
    drive(w, v, 180);
    const Phys2Body* ch = &w->bodies[v->body];
    CHECK(v->wheels_on_ground == 4);
    /* sıkışma = m g / (4 k); şasi yüksekliği = bağlantı ofseti + (dinlenme − sıkışma) + yarıçap */
    f32 comp = 1200.0f * 9.81f / (4.0f * 30000.0f);
    f32 want = 0.25f + 0.5f - comp + 0.4f;
    test_note("araç: şasi yüksekliği", ch->pos.y, 4);
    test_note("araç: beklenen", want, 4);
    CHECK_NEAR(ch->pos.y, want, 0.01);
    for (u32 i = 0; i < 4; i++) CHECK_NEAR(v->wheels[i].compression, comp, 0.01);
    test_note("araç: dinlenme vx", ch->vel.x, 5);
    test_note("araç: dinlenme vy", ch->vel.y, 5);
    test_note("araç: dinlenme vz", ch->vel.z, 5);
    test_note("araç: dinlenme |ω|", vec3_len(ch->ang_vel), 5);
    CHECK(vec3_len(ch->vel) < 0.02f);
    CHECK(phys2_mat3_col(&ch->rot_m, 1).y > 0.9999f);
    CHECK(m_abs(ch->pos.x) < 1e-3f && m_abs(ch->pos.z) < 1e-3f);
}

TEST(phys2_vehicle_accelerates_straight) {
    test_arena_reset();
    Phys2World* w;
    Phys2Vehicle* v = car_world(&w);
    drive(w, v, 60);
    v->throttle = 1.0f;
    drive(w, v, 180);
    const Phys2Body* ch = &w->bodies[v->body];
    test_note("araç: 3 s sonra hız (m/s)", v->speed, 2);
    test_note("araç: yanal sapma (m)", ch->pos.x, 4);
    CHECK(v->speed > 12.0f && v->speed < 25.0f);
    CHECK(ch->pos.z < -15.0f);                 /* ileri = -z */
    CHECK(m_abs(ch->pos.x) < 0.2f);
    CHECK(phys2_mat3_col(&ch->rot_m, 1).y > 0.99f);  /* devrilmedi */
}

TEST(phys2_vehicle_turns_left) {
    test_arena_reset();
    Phys2World* w;
    Phys2Vehicle* v = car_world(&w);
    drive(w, v, 60);
    v->throttle = 0.6f;
    v->steer = 1.0f;
    drive(w, v, 180);
    Vec3 f = phys2_vehicle_forward(w, v);
    const Phys2Body* ch = &w->bodies[v->body];
    test_note("araç: dönüşten sonra ileri x", f.x, 3);
    CHECK(f.x < -0.5f);                        /* sola (−x) döndü */
    CHECK(ch->pos.x < -2.0f);
    CHECK(phys2_mat3_col(&ch->rot_m, 1).y > 0.95f);
}

TEST(phys2_vehicle_brakes) {
    test_arena_reset();
    Phys2World* w;
    Phys2Vehicle* v = car_world(&w);
    drive(w, v, 60);
    v->throttle = 1.0f;
    while (v->speed < 15.0f) drive(w, v, 1);
    f32 v0 = v->speed;
    f32 z0 = w->bodies[v->body].pos.z;
    v->throttle = 0.0f;
    v->brake = 1.0f;
    u32 steps = 0;
    while (v->speed > 0.2f && steps < 300) {
        drive(w, v, 1);
        steps++;
    }
    f32 dist = z0 - w->bodies[v->body].pos.z;
    /* a = min(fren/m, μ g) */
    f32 a = m_min(16000.0f / 1200.0f, 1.3f * 9.81f);
    f32 want = v0 * v0 / (2.0f * a);
    test_note("araç: fren mesafesi (m)", dist, 2);
    test_note("araç: analitik (m)", want, 2);
    CHECK(steps < 300);
    CHECK_NEAR(dist, want, 0.25 * want);
}

TEST(phys2_vehicle_determinism) {
    test_arena_reset();
    u32 h[2];
    for (u32 run = 0; run < 2; run++) {
        Phys2World* w;
        Phys2Vehicle* v = car_world(&w);
        for (u32 k = 0; k < 5; k++) {
            Phys2BodyDesc b = phys2_desc_box(vec3(-2.0f + (f32)k, 0.5f, -15.0f), vec3(0.5f, 0.5f, 0.5f), 20.0f);
            phys2_add_body(w, &b);
        }
        drive(w, v, 30);
        v->throttle = 1.0f;
        v->steer = 0.3f;
        drive(w, v, 200);
        h[run] = phys2_state_hash(w);
    }
    CHECK(h[0] == h[1]);
}

/* ---- karakter ---- */

static void walk(Phys2World* w, Phys2Character* c, Vec3 wish, u32 steps) {
    for (u32 i = 0; i < steps; i++) {
        phys2_character_update(w, c, wish, FALSE, DT);
        phys2_step(w, DT);
    }
}

static Phys2Character* new_char(Vec3 pos) {
    Phys2Character* c = ARENA_PUSH_STRUCT(test_arena(), Phys2Character);
    Phys2CharacterDesc d = phys2_character_desc_default(pos);
    phys2_character_init(c, &d);
    return c;
}

TEST(phys2_character_lands_and_walks) {
    test_arena_reset();
    Phys2World* w = world(16);
    add_ground(w);
    Phys2Character* c = new_char(vec3(0, 3, 0));
    walk(w, c, vec3(0, 0, 0), 120);
    CHECK(c->grounded);
    CHECK_NEAR(c->pos.y, 0.0, 0.01);
    f32 z0 = c->pos.z;
    walk(w, c, vec3(0, 0, -1), 120);
    test_note("karakter: 2 s yürüme (m)", z0 - c->pos.z, 3);
    CHECK_NEAR(z0 - c->pos.z, 10.0, 0.2);
    CHECK_NEAR(c->pos.y, 0.0, 0.01);
}

TEST(phys2_character_wall_and_slide) {
    test_arena_reset();
    Phys2World* w = world(16);
    add_ground(w);
    add_static_box(w, vec3(0, 2, -3.5f), vec3(10, 2, 0.5f), phys2_quat_identity());   /* yüz z = -3 */
    Phys2Character* c = new_char(vec3(0, 0, 0));
    walk(w, c, vec3(0, 0, -1), 120);
    test_note("karakter: duvarda z", c->pos.z, 4);
    CHECK(c->pos.z > -3.0f + 0.35f - 0.02f);
    CHECK(c->pos.z < -2.5f);
    /* çapraz: duvar boyunca −x'e kayar */
    walk(w, c, vec3(-0.7071f, 0, -0.7071f), 60);
    test_note("karakter: kayma x", c->pos.x, 3);
    CHECK(c->pos.x < -2.5f);
    CHECK(c->pos.z > -3.0f + 0.35f - 0.02f);
}

TEST(phys2_character_steps) {
    test_arena_reset();
    Phys2World* w = world(16);
    add_ground(w);
    add_static_box(w, vec3(-3, 0.15f, -8), vec3(1.0f, 0.15f, 6.0f), phys2_quat_identity());   /* 0,3 m basamak, z -2..-14 */
    add_static_box(w, vec3(3, 0.4f, -8), vec3(1.0f, 0.4f, 6.0f), phys2_quat_identity());      /* 0,8 m duvar */
    Phys2Character* a = new_char(vec3(-3, 0, 0));
    Phys2Character* b = new_char(vec3(3, 0, 0));
    walk(w, a, vec3(0, 0, -1), 6);   /* zemine otur */
    walk(w, b, vec3(0, 0, -1), 6);
    for (u32 i = 0; i < 90; i++) {
        phys2_character_update(w, a, vec3(0, 0, -1), FALSE, DT);
        phys2_character_update(w, b, vec3(0, 0, -1), FALSE, DT);
        phys2_step(w, DT);
    }
    test_note("karakter: 0,3 m basamak sonrası y", a->pos.y, 3);
    test_note("karakter: 0,3 m basamak sonrası z", a->pos.z, 3);
    test_note("karakter: 0,8 m önünde z", b->pos.z, 3);
    CHECK_NEAR(a->pos.y, 0.3, 0.02);
    CHECK(a->pos.z < -3.0f);
    CHECK_NEAR(b->pos.y, 0.0, 0.02);
    CHECK(b->pos.z > -2.0f + 0.35f - 0.02f);
}

static f32 slope_drift(f32 deg) {
    Phys2World* w = world(8);
    f32 a = deg * (M_PI / 180.0f);
    Phys2BodyDesc p = phys2_desc_plane(vec3(0, m_cos(a), m_sin(a)), 0.0f);
    phys2_add_body(w, &p);
    Phys2Character* c = new_char(vec3(0, 0.05f, 0));
    walk(w, c, vec3(0, 0, 0), 10);
    Vec3 p0 = c->pos;
    walk(w, c, vec3(0, 0, 0), 120);
    return vec3_len(vec3_sub(c->pos, p0));
}

TEST(phys2_character_slope_limit) {
    test_arena_reset();
    f32 d30 = slope_drift(30.0f), d60 = slope_drift(60.0f);
    test_note("karakter: 30° eğimde kayma (m)", d30, 4);
    test_note("karakter: 60° eğimde kayma (m)", d60, 3);
    CHECK(d30 < 0.01f);
    CHECK(d60 > 1.0f);
}

TEST(phys2_character_jump_height) {
    test_arena_reset();
    Phys2World* w = world(8);
    add_ground(w);
    Phys2Character* c = new_char(vec3(0, 0, 0));
    walk(w, c, vec3(0, 0, 0), 10);
    phys2_character_update(w, c, vec3(0, 0, 0), TRUE, DT);
    phys2_step(w, DT);
    f32 apex = c->pos.y;
    for (u32 i = 0; i < 120; i++) {
        phys2_character_update(w, c, vec3(0, 0, 0), FALSE, DT);
        phys2_step(w, DT);
        apex = m_max(apex, c->pos.y);
    }
    f32 want = 5.5f * 5.5f / (2.0f * 9.81f);
    test_note("karakter: zıplama tepe (m)", apex, 3);
    CHECK_NEAR(apex, want, 0.1);
    CHECK(c->grounded);
}

TEST(phys2_character_pushes_box) {
    test_arena_reset();
    Phys2World* w = world(16);
    w->allow_sleep = TRUE;
    add_ground(w);
    Phys2BodyDesc b = phys2_desc_box(vec3(0, 0.5f, -3), vec3(0.5f, 0.5f, 0.5f), 10.0f);
    u32 box = phys2_add_body(w, &b);
    Phys2Character* c = new_char(vec3(0, 0, 0));
    walk(w, c, vec3(0, 0, 0), 60);   /* kutu uykuya dalsın */
    CHECK(w->bodies[box].flags & PHYS2_BODY_ASLEEP);
    walk(w, c, vec3(0, 0, -1), 120);
    test_note("karakter: itilen kutu z", w->bodies[box].pos.z, 3);
    CHECK(w->bodies[box].pos.z < -3.3f);
}
