/* tests/test_sim.c — Deterministik simülasyon çekirdeği, rollback ve replay (M16_A) testleri.
 * Durum özeti (hash) ve ofset modeli birim testleri, 1-8 kare gecikmeli iki eş (peer)
 * rollback senkronizasyon testi, 10.000 kare 3D fizik + ECS replay belirlenimcilik testi
 * ve 8 kare geri sarma + yeniden simülasyon maliyet ölçümünü (benchmark) içerir.
 */
#include <stdio.h>
#include <string.h>
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
#include "phys/world.h"
#include "scene/ecs.h"
#include "scene/transform.h"
#include "sim/sim.h"

static u8 g_sim_mem[MB(48)];

/* ============================================================================
 * 1. Hash, Kanonik Float (-0.0f / NaN) ve Ofset Modeli Testleri
 * ============================================================================ */

TEST(sim_hash_canonical_and_desync) {
    u8 buf_a[64];
    u8 buf_b[64];
    for (u32 i = 0u; i < 64u; i++) {
        buf_a[i] = (u8)(i * 7u + 3u);
        buf_b[i] = buf_a[i];
    }

    u64 ha = sim_hash(buf_a, sizeof(buf_a));
    u64 hb = sim_hash(buf_b, sizeof(buf_b));
    CHECK(ha != 0ULL);
    CHECK(ha == hb);

    /* Tek bitlik değişim farklı özet üretmeli (çığ etkisi) */
    buf_b[31] ^= 1u;
    u64 hb_diff = sim_hash(buf_b, sizeof(buf_b));
    CHECK(ha != hb_diff);

    /* -0.0f ve +0.0f aynı kanonik özeti üretmeli */
    f32 pos_zero =  0.0f;
    f32 neg_zero = -0.0f;
    u64 h_pz = sim_hash_final(sim_hash_f32(sim_hash_init(), pos_zero));
    u64 h_nz = sim_hash_final(sim_hash_f32(sim_hash_init(), neg_zero));
    CHECK(h_pz == h_nz);

    /* Farklı NaN bit kalıpları aynı kanonik özeti üretmeli */
    u32 nan1_bits = 0x7FC00001u;
    u32 nan2_bits = 0xFFC00123u;
    f32 nan1 = 0.0f, nan2 = 0.0f;
    memcpy(&nan1, &nan1_bits, sizeof(f32));
    memcpy(&nan2, &nan2_bits, sizeof(f32));
    u64 h_nan1 = sim_hash_final(sim_hash_f32(sim_hash_init(), nan1));
    u64 h_nan2 = sim_hash_final(sim_hash_f32(sim_hash_init(), nan2));
    CHECK(h_nan1 == h_nan2);

    /* PhysShape birliği sıfırlama denetimi: yığın çöpü özeti bozmamalı */
    PhysShape s1, s2;
    memset(&s1, 0xAA, sizeof(s1));
    memset(&s2, 0x55, sizeof(s2));
    s1 = shape_from_sphere(sphere_make(vec3(1.0f, 2.0f, 3.0f), 0.5f));
    s2 = shape_from_sphere(sphere_make(vec3(1.0f, 2.0f, 3.0f), 0.5f));
    CHECK(sim_hash(&s1, sizeof(s1)) == sim_hash(&s2, sizeof(s2)));
}

/* Saf ofset/indeks kullanan durum yapısı (hiç ham işaretçi içermez -> doğrudan state baytları özetlenir) */
typedef struct {
    i32 x, y;
    i32 vx, vy;
    u32 target_idx; /* işaretçi yerine dizi indeksi */
    u32 score;
} OffsetPawn;

typedef struct {
    u32       tick;
    u32       pawn_count;
    SimOffset pawns_off; /* işaretçi yerine durum tabanından bayt ofseti */
} OffsetGameState;

static void offset_game_init(Sim* sim, u32 count) {
    SimOffset hdr_off = sim_state_alloc(sim, sizeof(OffsetGameState), _Alignof(OffsetGameState));
    CHECK(hdr_off == 0u);
    SimOffset arr_off = sim_state_alloc(sim, sizeof(OffsetPawn) * count, _Alignof(OffsetPawn));

    OffsetGameState* gs = SIM_PTR(sim_state(sim), OffsetGameState, hdr_off);
    gs->tick       = 0u;
    gs->pawn_count = count;
    gs->pawns_off  = arr_off;

    OffsetPawn* pawns = SIM_PTR(sim_state(sim), OffsetPawn, gs->pawns_off);
    for (u32 i = 0u; i < count; i++) {
        pawns[i].x          = (i32)(i * 100u);
        pawns[i].y          = (i32)(i * 50u);
        pawns[i].vx         = 0;
        pawns[i].vy         = 0;
        pawns[i].target_idx = (i + 1u) % count;
        pawns[i].score      = 0u;
    }
}

static void offset_game_step(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    UNUSED(dt);
    UNUSED(user);
    OffsetGameState* gs = (OffsetGameState*)state;
    OffsetPawn* pawns = SIM_PTR(state, OffsetPawn, gs->pawns_off);

    for (u32 i = 0u; i < gs->pawn_count; i++) {
        const SimInput* in = &inputs[i % player_count];
        pawns[i].vx = (pawns[i].vx * 3) / 4 + (i32)in->move_x;
        pawns[i].vy = (pawns[i].vy * 3) / 4 + (i32)in->move_y;
        if (in->buttons & SIM_BTN_JUMP) {
            pawns[i].vy += 20;
        }
        pawns[i].x += pawns[i].vx;
        pawns[i].y += pawns[i].vy;

        /* Hedef indeksi üzerinden etkileşim (işaretçi yok) */
        u32 tid = pawns[i].target_idx;
        i64 dx = (i64)pawns[tid].x - (i64)pawns[i].x;
        i64 dy = (i64)pawns[tid].y - (i64)pawns[i].y;
        if (dx * dx + dy * dy < 10000LL) {
            pawns[i].score += 1u + (in->buttons & 7u);
        }
    }
    gs->tick++;
}

TEST(sim_offset_state_and_snapshot_ring) {
    Arena a;
    arena_init(&a, g_sim_mem, sizeof(g_sim_mem));

    SimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = 512u;
    cfg.player_count = 2u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = offset_game_step;
    cfg.hash_fn      = NULL; /* ham state bölgesi doğrudan özetlenir */

    Sim sim_a, sim_b;
    sim_init(&sim_a, &a, &cfg);
    sim_init(&sim_b, &a, &cfg);
    CHECK(sim_state(&sim_a) != sim_state(&sim_b));

    offset_game_init(&sim_a, 4u);
    offset_game_init(&sim_b, 4u);

    /* Farklı bellek adreslerindeki iki eşin başlangıç ham bellek özeti birebir aynı olmalı */
    CHECK(sim_state_hash(&sim_a) == sim_state_hash(&sim_b));

    /* 20 kare ilerlet: 16 karelik halka tamponun sarma (wrap-around) davranışını doğrula */
    for (u32 f = 0u; f < 20u; f++) {
        SimInput in0 = { (f & 1u) ? SIM_BTN_JUMP : 0u, (i16)(f * 3u), -5, 0, 0, f };
        SimInput in1 = { (f & 2u) ? SIM_BTN_FIRE : 0u, -7, (i16)(f * 2u), 0, 0, 0u };
        sim_submit_input(&sim_a, 0u, f, in0);
        sim_submit_input(&sim_a, 1u, f, in1);
        sim_step(&sim_a);
    }

    /* Kare 20'deyken kare 4..19 halkada mevcut, kare 0..3 üzerine yazılmış olmalı */
    CHECK(!sim_restore_snapshot(&sim_a, 3u));
    u64 hash_after_12 = sim_frame_hash(&sim_a, 12u);
    u64 hash_after_19 = sim_frame_hash(&sim_a, 19u);
    CHECK(hash_after_12 != 0ULL && hash_after_19 != 0ULL);

    /* Kare 12'ye geri sar + yeniden simüle et -> kare 19 sonu özeti birebir korunmalı */
    CHECK(sim_rollback_to(&sim_a, 12u));
    CHECK(sim_a.current_frame == 20u);
    CHECK(sim_frame_hash(&sim_a, 12u) == hash_after_12);
    CHECK(sim_state_hash(&sim_a) == hash_after_19);
    CHECK(!sim_check_desync(&sim_a, 19u, hash_after_19));
    CHECK(sim_check_desync(&sim_a, 19u, hash_after_19 ^ 1ULL));
}

/* ============================================================================
 * 2. Örnek 3D Fizik + ECS Oyun Sahnesi (Tek Arena Bölgesinde)
 * ============================================================================ */

#define SCENE_MAX_PLAYERS  2u
#define SCENE_CRATE_COUNT  6u
#define SCENE_MAX_PROJ     8u
#define SCENE_MAX_ENTITIES 64u
#define SCENE_MAX_BODIES   32u
#define SCENE_MAX_STATICS  16u

typedef struct {
    u32 body_idx;      /* PhysWorld.bodies içinde indeks (işaretçi yok) */
    u32 player_id;
    i32 hp;
    u32 cooldown;
    u32 score;
} CompFighter;

typedef struct {
    EcsEntity owner;   /* EcsEntity tutamağı (işaretçi yok) */
    u32       slot;    /* mermi havuzu indeksi */
    u32       ttl;
} CompRocket;

typedef struct {
    f32 angle;
    f32 speed;
} CompOrbiter;

typedef struct {
    const Mesh* arena_mesh;
    const Bvh*  arena_bvh;
} SceneStaticAssets;

typedef struct {
    u32       tick;
    Rng       rng;
    PhysWorld phys;
    EcsWorld  ecs;

    EcsCompId cid_fighter;
    EcsCompId cid_rocket;
    EcsCompId cid_orbiter;

    EcsEntity player_ent[SCENE_MAX_PLAYERS];
    EcsEntity orbiter_ent[SCENE_MAX_PLAYERS];
    EcsEntity crate_ent[SCENE_CRATE_COUNT];
    u32       crate_body[SCENE_CRATE_COUNT];

    u32       proj_body[SCENE_MAX_PROJ];
    EcsEntity proj_ent[SCENE_MAX_PROJ];
} BattleSceneState;

static u64 battle_scene_hash(const void* state, u32 state_size, void* user) {
    UNUSED(state_size);
    UNUSED(user);
    const BattleSceneState* s = (const BattleSceneState*)state;
    u64 h = sim_hash_init();
    h = sim_hash_u32(h, s->tick);
    h = sim_hash_update(h, &s->rng, sizeof(Rng));
    h = sim_hash_phys(h, &s->phys);
    h = sim_hash_ecs(h, &s->ecs);
    h = sim_hash_update(h, s->player_ent,  sizeof(s->player_ent));
    h = sim_hash_update(h, s->orbiter_ent, sizeof(s->orbiter_ent));
    h = sim_hash_update(h, s->crate_ent,   sizeof(s->crate_ent));
    h = sim_hash_update(h, s->crate_body,  sizeof(s->crate_body));
    h = sim_hash_update(h, s->proj_body,   sizeof(s->proj_body));
    h = sim_hash_update(h, s->proj_ent,    sizeof(s->proj_ent));
    return sim_hash_final(h);
}

static void battle_scene_setup(Sim* sim, const SceneStaticAssets* assets, u32 seed) {
    sim_reset(sim);
    Arena* sa = sim_state_arena(sim);

    BattleSceneState* s = ARENA_PUSH_STRUCT(sa, BattleSceneState);
    s->tick = 0u;
    rng_seed(&s->rng, seed);

    /* Fizik dünyasını aynı durum arenası (sa) içinde kur */
    phys_world_init(&s->phys, sa, SCENE_MAX_BODIES, SCENE_MAX_STATICS, 2u);
    s->phys.fixed_dt = sim->fixed_dt;

    /* Statik zemin ve arena duvarları */
    phys_world_add_static(&s->phys, shape_from_plane(plane_make(vec3(0.0f, 1.0f, 0.0f), 0.0f)), 0.2f, 0.7f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f, -12.0f), vec3(12.0f, 4.0f, -11.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f,  11.0f), vec3(12.0f, 4.0f,  12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3(-12.0f, 0.0f, -12.0f), vec3(-11.0f, 4.0f, 12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_aabb(aabb_make(vec3( 11.0f, 0.0f, -12.0f), vec3( 12.0f, 4.0f, 12.0f))), 0.3f, 0.5f);
    phys_world_add_static(&s->phys, shape_from_sphere(sphere_make(vec3(0.0f, 0.5f, 0.0f), 1.2f)), 0.4f, 0.6f);

    if (assets && assets->arena_mesh && assets->arena_bvh) {
        phys_world_add_mesh(&s->phys, assets->arena_mesh, assets->arena_bvh, 0.2f, 0.6f);
    }

    /* ECS dünyasını aynı durum arenası (sa) içinde kur */
    ecs_world_init(&s->ecs, sa, SCENE_MAX_ENTITIES);
    transform_register(&s->ecs);
    s->cid_fighter = ecs_register(&s->ecs, "Fighter", sizeof(CompFighter), _Alignof(CompFighter), NULL);
    s->cid_rocket  = ecs_register(&s->ecs, "Rocket",  sizeof(CompRocket),  _Alignof(CompRocket),  NULL);
    s->cid_orbiter = ecs_register(&s->ecs, "Orbiter", sizeof(CompOrbiter), _Alignof(CompOrbiter), NULL);

    EcsCompId cid_tr = transform_comp_id(&s->ecs);

    /* Oyuncu kapsülleri ve çocuk yörünge taretleri */
    for (u32 p = 0u; p < SCENE_MAX_PLAYERS; p++) {
        f32 px = (p == 0u) ? -4.0f : 4.0f;
        f32 pz = (p == 0u) ? -2.0f : 2.0f;
        u32 bidx = phys_world_add_capsule(&s->phys,
                                          vec3(px, 0.5f, pz),
                                          vec3(px, 1.3f, pz),
                                          0.4f, 1.0f, 0.1f, 0.8f);

        EcsEntity pe = ecs_spawn(&s->ecs);
        s->player_ent[p] = pe;
        Transform tr = transform_trs(vec3(px, 0.9f, pz), quat_identity(), vec3(1.0f, 1.0f, 1.0f));
        ecs_set(&s->ecs, pe, cid_tr, &tr);

        CompFighter cf = { bidx, p, 100, 0u, 0u };
        ecs_set(&s->ecs, pe, s->cid_fighter, &cf);

        /* Hiyerarşik çocuk entity (oyuncunun etrafında dönen uydu) */
        EcsEntity oe = ecs_spawn(&s->ecs);
        s->orbiter_ent[p] = oe;
        Transform otr = transform_trs(vec3(0.8f, 0.5f, 0.0f), quat_identity(), vec3(0.3f, 0.3f, 0.3f));
        ecs_set(&s->ecs, oe, cid_tr, &otr);
        CompOrbiter co = { (f32)p * M_PI, 2.5f + (f32)p * 0.5f };
        ecs_set(&s->ecs, oe, s->cid_orbiter, &co);
        transform_set_parent(&s->ecs, oe, pe);
    }

    /* Dinamik fizik küreleri (sandık/toplar) */
    for (u32 i = 0u; i < SCENE_CRATE_COUNT; i++) {
        f32 angle = (f32)i * (M_TAU / (f32)SCENE_CRATE_COUNT);
        Vec3 pos = vec3(m_cos(angle) * 3.0f, 1.5f + (f32)i * 0.25f, m_sin(angle) * 3.0f);
        u32 bidx = phys_world_add_sphere(&s->phys, pos, 0.45f, 0.8f, 0.45f, 0.5f);
        s->crate_body[i] = bidx;

        EcsEntity ce = ecs_spawn(&s->ecs);
        s->crate_ent[i] = ce;
        Transform ctr = transform_trs(pos, quat_identity(), vec3(0.9f, 0.9f, 0.9f));
        ecs_set(&s->ecs, ce, cid_tr, &ctr);
    }

    /* Mermi fizik gövdesi havuzu (aktif değilken inv_mass = 0 ve uzak konumda) */
    for (u32 i = 0u; i < SCENE_MAX_PROJ; i++) {
        Vec3 park = vec3((f32)i * 2.0f - 8.0f, -50.0f, 0.0f);
        u32 bidx = phys_world_add_sphere(&s->phys, park, 0.25f, 0.0f, 0.6f, 0.2f);
        s->proj_body[i] = bidx;
        s->proj_ent[i]  = ECS_NULL;
    }

    transform_update(&s->ecs);
}

static void battle_scene_step(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    UNUSED(user);
    BattleSceneState* s = (BattleSceneState*)state;
    EcsCompId cid_tr = transform_comp_id(&s->ecs);

    /* 1) Oyuncu girdilerini fizik gövdelerine ve silah sistemine uygula */
    EcsQuery qf = ecs_query(&s->ecs, (1u << s->cid_fighter) | (1u << cid_tr));
    while (ecs_query_next(&qf)) {
        CompFighter* cf = (CompFighter*)ecs_query_get(&qf, s->cid_fighter);
        const SimInput* in = &inputs[cf->player_id % player_count];
        PhysBody* b = &s->phys.bodies[cf->body_idx];

        f32 mx = (f32)in->move_x * (1.0f / 32767.0f);
        f32 mz = (f32)in->move_y * (1.0f / 32767.0f);
        f32 spd = (in->buttons & SIM_BTN_SPRINT) ? 9.0f : 6.0f;

        b->vel.x = b->vel.x * 0.75f + mx * spd * 0.25f;
        b->vel.z = b->vel.z * 0.75f + mz * spd * 0.25f;

        if ((in->buttons & SIM_BTN_JUMP) && b->on_ground) {
            b->vel.y = 5.5f;
            b->on_ground = FALSE;
        }

        if (cf->cooldown > 0u) cf->cooldown--;
        if ((in->buttons & SIM_BTN_FIRE) && cf->cooldown == 0u) {
            /* Boş bir mermi yuvası bul ve ateşle */
            for (u32 slot = 0u; slot < SCENE_MAX_PROJ; slot++) {
                if (s->proj_ent[slot] == ECS_NULL || !ecs_alive(&s->ecs, s->proj_ent[slot])) {
                    EcsEntity re = ecs_spawn(&s->ecs);
                    s->proj_ent[slot] = re;

                    Vec3 dir = vec3(mx, 0.15f, mz);
                    if (vec3_dot(dir, dir) < 1e-4f) {
                        dir = (cf->player_id == 0u) ? vec3(1.0f, 0.1f, 0.0f) : vec3(-1.0f, 0.1f, 0.0f);
                    }
                    dir = vec3_norm(dir);

                    PhysBody* pb = &s->phys.bodies[s->proj_body[slot]];
                    pb->pos      = vec3_add(b->pos, vec3_scale(dir, 0.9f));
                    pb->vel      = vec3_scale(dir, 14.0f);
                    pb->inv_mass = 2.0f;

                    Transform rtr = transform_trs(pb->pos, quat_identity(), vec3(0.25f, 0.25f, 0.25f));
                    ecs_set(&s->ecs, re, cid_tr, &rtr);
                    CompRocket cr = { qf.entity, slot, 24u };
                    ecs_set(&s->ecs, re, s->cid_rocket, &cr);

                    cf->cooldown = 8u;
                    cf->score += 1u;
                    break;
                }
            }
        }
    }

    /* 2) 3D fizik dünyasını bir sabit adım ilerlet */
    phys_world_step(&s->phys);

    /* 3) Fizik konumlarını ECS Transform bileşenlerine aktar */
    for (u32 p = 0u; p < SCENE_MAX_PLAYERS; p++) {
        EcsEntity pe = s->player_ent[p];
        CompFighter* cf = (CompFighter*)ecs_get(&s->ecs, pe, s->cid_fighter);
        Transform* tr   = (Transform*)ecs_get(&s->ecs, pe, cid_tr);
        if (cf && tr) {
            tr->pos = s->phys.bodies[cf->body_idx].pos;
            f32 yaw = (f32)inputs[p % player_count].look_dx * 0.01f;
            tr->rot = quat_from_axis_angle(vec3(0.0f, 1.0f, 0.0f), yaw);
        }
    }

    for (u32 i = 0u; i < SCENE_CRATE_COUNT; i++) {
        Transform* ctr = (Transform*)ecs_get(&s->ecs, s->crate_ent[i], cid_tr);
        if (ctr) {
            ctr->pos = s->phys.bodies[s->crate_body[i]].pos;
        }
    }

    /* 4) Mermileri güncelle; süresi bitenleri sorgu içinde ertelenmiş olarak sil */
    EcsQuery qr = ecs_query(&s->ecs, (1u << s->cid_rocket) | (1u << cid_tr));
    while (ecs_query_next(&qr)) {
        CompRocket* cr = (CompRocket*)ecs_query_get(&qr, s->cid_rocket);
        Transform*  tr = (Transform*)ecs_query_get(&qr, cid_tr);
        PhysBody*   pb = &s->phys.bodies[s->proj_body[cr->slot]];
        tr->pos = pb->pos;

        if (cr->ttl <= 1u) {
            pb->inv_mass = 0.0f;
            pb->vel      = vec3(0.0f, 0.0f, 0.0f);
            pb->pos      = vec3((f32)cr->slot * 2.0f - 8.0f, -50.0f, 0.0f);
            s->proj_ent[cr->slot] = ECS_NULL;
            ecs_destroy(&s->ecs, qr.entity);
        } else {
            cr->ttl--;
        }
    }

    /* 5) Yörünge uydularını döndür ve hiyerarşik dünya matrislerini hesapla */
    EcsQuery qo = ecs_query(&s->ecs, (1u << s->cid_orbiter) | (1u << cid_tr));
    while (ecs_query_next(&qo)) {
        CompOrbiter* co = (CompOrbiter*)ecs_query_get(&qo, s->cid_orbiter);
        Transform*   tr = (Transform*)ecs_query_get(&qo, cid_tr);
        co->angle += co->speed * dt;
        if (co->angle > M_TAU) co->angle -= M_TAU;
        tr->pos = vec3(m_cos(co->angle) * 0.8f, 0.5f, m_sin(co->angle) * 0.8f);
    }

    transform_update(&s->ecs);
    s->tick++;
}

static SimInput make_test_input(u32 player, u32 frame) {
    Rng r;
    /* Girdi kalıbı ~4-6 karede bir değişir ki hem doğru tahmin hem yanlış tahmin + rollback tetiklensin */
    u32 epoch = frame / 3u;
    rng_seed_seq(&r, (u64)epoch * 1315423911ULL + (u64)player * 97ULL, player + 1u);

    SimInput in = sim_input_zero();
    u32 bits = rng_u32(&r);
    if ((bits & 3u) == 0u)      in.move_x =  24000;
    else if ((bits & 3u) == 1u) in.move_x = -24000;
    else                        in.move_x = (i16)((i32)(bits & 0xFFu) * 64 - 8192);

    if (((bits >> 2u) & 3u) == 0u)      in.move_y =  24000;
    else if (((bits >> 2u) & 3u) == 1u) in.move_y = -24000;
    else                                in.move_y = (i16)((i32)((bits >> 8u) & 0xFFu) * 64 - 8192);

    in.look_dx = (i16)((i32)((bits >> 16u) & 0x3Fu) - 32);
    in.look_dy = 0;

    if ((frame + player * 5u) % 11u == 0u) in.buttons |= SIM_BTN_JUMP;
    if ((frame + player * 3u) % 7u  == 0u) in.buttons |= SIM_BTN_FIRE;
    if ((bits >> 24u) & 1u)                in.buttons |= SIM_BTN_SPRINT;
    in.aux = epoch;
    return in;
}

/* ============================================================================
 * 3. İki Eş (Peer) Simülasyon: 1-8 Kare Gecikmeli Girdi + Rollback Testi
 * ============================================================================ */

TEST(sim_peer_rollback_delay_1_to_8) {
    Arena a;
    arena_init(&a, g_sim_mem, sizeof(g_sim_mem));

    Mesh* ramp = mesh_cube(&a, 1.5f);
    for (u32 i = 0u; i < ramp->vertex_count; i++) {
        ramp->px[i] += 3.5f;
        ramp->py[i] -= 0.8f;
    }
    mesh_compute_bounds(ramp);
    Bvh* ramp_bvh = bvh_build(&a, ramp);
    SceneStaticAssets assets = { ramp, ramp_bvh };

    SimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = KB(64);
    cfg.player_count = 2u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = battle_scene_step;
    cfg.hash_fn      = battle_scene_hash;
    cfg.user         = &assets;

    Sim peer_a, peer_b;
    sim_init(&peer_a, &a, &cfg);
    sim_init(&peer_b, &a, &cfg);

    /* A) Sabit gecikmeler d = 1..8 kare: her karede gecikmeli girdi akışı + rollback */
    for (u32 delay = 1u; delay <= 8u; delay++) {
        battle_scene_setup(&peer_a, &assets, 0x1000u + delay);
        battle_scene_setup(&peer_b, &assets, 0x1000u + delay);
        CHECK(sim_state_hash(&peer_a) == sim_state_hash(&peer_b));

        const u32 FRAMES = 240u;
        for (u32 f = 0u; f < FRAMES; f++) {
            SimInput in0 = make_test_input(0u, f);
            SimInput in1 = make_test_input(1u, f);

            /* Eş A: tüm girdileri gecikmesiz alır */
            sim_submit_input(&peer_a, 0u, f, in0);
            sim_submit_input(&peer_a, 1u, f, in1);
            sim_step(&peer_a);

            /* Eş B: oyuncu 0 yerel (gecikmesiz), oyuncu 1 ise `delay` kare gecikmeli gelir */
            sim_submit_input(&peer_b, 0u, f, in0);
            if (f >= delay) {
                u32 arrived_f = f - delay;
                SimInput delayed_in1 = make_test_input(1u, arrived_f);
                CHECK(sim_submit_input(&peer_b, 1u, arrived_f, delayed_in1));
            }
            sim_step(&peer_b);

            /* Gecikmeli girdisi ulaşmış olan tüm karelerde (0 .. f - delay) özetler birebir aynı olmalı */
            if (f >= delay) {
                u32 confirmed_f = f - delay;
                u64 ha = sim_frame_hash(&peer_a, confirmed_f);
                u64 hb = sim_frame_hash(&peer_b, confirmed_f);
                CHECK(ha != 0ULL);
                CHECK(ha == hb);
                CHECK(!sim_check_desync(&peer_b, confirmed_f, ha));
            }
        }

        /* Kuyrukta kalan son `delay` karelik gecikmeli girdileri teslim et ve rollback'i çöz */
        for (u32 rem = FRAMES - delay; rem < FRAMES; rem++) {
            CHECK(sim_submit_input(&peer_b, 1u, rem, make_test_input(1u, rem)));
        }
        sim_resolve_rollback(&peer_b);

        /* Tüm karelerin özetleri ve son durum özeti birebir aynı olmalı */
        CHECK(peer_b.stats.rollback_count > 0ULL);
        CHECK(peer_b.stats.max_rollback_depth == delay);
        CHECK(sim_state_hash(&peer_a) == sim_state_hash(&peer_b));
        for (u32 f = 0u; f < FRAMES; f++) {
            CHECK(sim_frame_hash(&peer_a, f) == sim_frame_hash(&peer_b, f));
        }
    }

    /* B) Değişken (jitter: 1..8 kare rastgele gecikme ve sırasız paket varışı) + her karede yakalama */
    battle_scene_setup(&peer_a, &assets, 0x9999u);
    battle_scene_setup(&peer_b, &assets, 0x9999u);

    Rng net_rng;
    rng_seed(&net_rng, 0xDEADBEEFULL);
    const u32 JITTER_FRAMES = 400u;
    u32 arrival_frame[400];
    b32 delivered[400];
    for (u32 f = 0u; f < JITTER_FRAMES; f++) {
        u32 d = 1u + (rng_u32(&net_rng) % 8u); /* 1..8 kare gecikme */
        arrival_frame[f] = f + d;
        delivered[f] = FALSE;
    }

    for (u32 f = 0u; f < JITTER_FRAMES; f++) {
        SimInput in0 = make_test_input(0u, f);
        SimInput in1 = make_test_input(1u, f);

        sim_submit_input(&peer_a, 0u, f, in0);
        sim_submit_input(&peer_a, 1u, f, in1);
        sim_step(&peer_a);

        sim_submit_input(&peer_b, 0u, f, in0);
        /* Bu karede veya daha önce varış zamanı gelmiş (sırasız dahil) paketleri teslim et */
        for (u32 k = 0u; k <= f; k++) {
            if (!delivered[k] && arrival_frame[k] <= f) {
                CHECK(sim_submit_input(&peer_b, 1u, k, make_test_input(1u, k)));
                delivered[k] = TRUE;
            }
        }
        sim_step(&peer_b);

        /* Tüm k <= min_undelivered - 1 kareleri için iki eşin kare özetleri birebir aynı olmalı */
        u32 sync_limit = f + 1u;
        for (u32 k = 0u; k <= f; k++) {
            if (!delivered[k]) {
                sync_limit = k;
                break;
            }
        }
        for (u32 k = (sync_limit > 16u) ? (sync_limit - 16u) : 0u; k < sync_limit; k++) {
            CHECK(sim_frame_hash(&peer_a, k) == sim_frame_hash(&peer_b, k));
        }
    }

    /* Kalan uçuş halindeki paketleri teslim et, rollback çöz -> her karede özetler aynı */
    for (u32 k = 0u; k < JITTER_FRAMES; k++) {
        if (!delivered[k]) {
            CHECK(sim_submit_input(&peer_b, 1u, k, make_test_input(1u, k)));
            delivered[k] = TRUE;
        }
    }
    sim_resolve_rollback(&peer_b);
    CHECK(sim_state_hash(&peer_a) == sim_state_hash(&peer_b));

    /* C) Saf ofset/indeks modeli (hash_fn == NULL, ham state baytları doğrudan özetlenir):
     * Eş B her adımda 1..8 kare yanlış tahminle ilerleyip gerçek girdiyi alınca geri sarar;
     * rollback sonrası her karede sim_state_hash(&peer_a) == sim_state_hash(&peer_b) doğrulanır. */
    SimConfig off_cfg;
    memset(&off_cfg, 0, sizeof(off_cfg));
    off_cfg.state_size   = 512u;
    off_cfg.player_count = 2u;
    off_cfg.snapshot_cap = 16u;
    off_cfg.fixed_dt     = SIM_DEFAULT_DT;
    off_cfg.step_fn      = offset_game_step;
    off_cfg.hash_fn      = NULL;

    Sim off_a, off_b;
    sim_init(&off_a, &a, &off_cfg);
    sim_init(&off_b, &a, &off_cfg);
    offset_game_init(&off_a, 6u);
    offset_game_init(&off_b, 6u);

    for (u32 f = 0u; f < 160u; f++) {
        SimInput in0 = make_test_input(0u, f);
        SimInput in1 = make_test_input(1u, f);

        sim_submit_input(&off_a, 0u, f, in0);
        sim_submit_input(&off_a, 1u, f, in1);
        sim_step(&off_a);

        /* Eş B: oyuncu 0 gerçek, oyuncu 1 önce tahminle adım atar */
        sim_submit_input(&off_b, 0u, f, in0);
        sim_step(&off_b);

        /* Her 1..8 karelik pencerede gecikmeli gerçek girdiler ulaşıp rollback çözülünce özetler eşleşir */
        u32 window = 1u + (f % 8u);
        if ((f + 1u) % window == 0u || f == 159u) {
            u32 start_k = (f + 1u >= 8u) ? (f + 1u - 8u) : 0u;
            for (u32 k = start_k; k <= f; k++) {
                CHECK(sim_submit_input(&off_b, 1u, k, make_test_input(1u, k)));
            }
            sim_resolve_rollback(&off_b);
            CHECK(sim_state_hash(&off_a) == sim_state_hash(&off_b));
        }
    }
}

/* ============================================================================
 * 4. 10.000 Kare 3D Fizik + ECS Replay Belirlenimcilik Testi
 * ============================================================================ */

static b32 check_scene_no_nan(const BattleSceneState* s) {
    for (u32 i = 0u; i < s->phys.body_count; i++) {
        Vec3 p = s->phys.bodies[i].pos;
        Vec3 v = s->phys.bodies[i].vel;
        if (p.x != p.x || p.y != p.y || p.z != p.z) return FALSE;
        if (v.x != v.x || v.y != v.y || v.z != v.z) return FALSE;
    }
    EcsCompId cid_tr = transform_comp_id(&s->ecs);
    const EcsStore* ts = &s->ecs.stores[cid_tr];
    for (u32 i = 0u; i < ts->count; i++) {
        const Transform* tr = (const Transform*)(ts->data + (usize)i * (usize)ts->stride);
        for (u32 m = 0u; m < 16u; m++) {
            if (tr->world.m[m] != tr->world.m[m]) return FALSE;
        }
    }
    return TRUE;
}

TEST(sim_replay_10000_frames_phys_ecs) {
    Arena a;
    arena_init(&a, g_sim_mem, sizeof(g_sim_mem));

    Mesh* ramp = mesh_cube(&a, 1.2f);
    for (u32 i = 0u; i < ramp->vertex_count; i++) {
        ramp->px[i] -= 2.5f;
        ramp->py[i] -= 0.7f;
    }
    mesh_compute_bounds(ramp);
    Bvh* ramp_bvh = bvh_build(&a, ramp);
    SceneStaticAssets assets = { ramp, ramp_bvh };

    const u32 TOTAL_FRAMES = 10000u;
    const u32 SEED         = 0x20261029u;

    SimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = KB(64);
    cfg.player_count = 2u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = battle_scene_step;
    cfg.hash_fn      = battle_scene_hash;
    cfg.user         = &assets;

    Sim sim1, sim2;
    sim_init(&sim1, &a, &cfg);
    sim_init(&sim2, &a, &cfg);

    SimReplay rep_orig;
    sim_replay_init(&rep_orig, &a, 2u, TOTAL_FRAMES, SEED, SIM_DEFAULT_DT);

    u64* hashes_run1 = ARENA_PUSH_ARRAY(&a, u64, TOTAL_FRAMES);
    u64* hashes_run2 = ARENA_PUSH_ARRAY(&a, u64, TOTAL_FRAMES);

    /* 1. Koşu: 10.000 kare fizik + ECS simüle et ve girdileri SimReplay'e kaydet */
    battle_scene_setup(&sim1, &assets, SEED);
    sim_attach_replay(&sim1, &rep_orig);

    for (u32 f = 0u; f < TOTAL_FRAMES; f++) {
        sim_submit_input(&sim1, 0u, f, make_test_input(0u, f));
        sim_submit_input(&sim1, 1u, f, make_test_input(1u, f));
        sim_step(&sim1);
        hashes_run1[f] = sim_state_hash(&sim1);
    }
    sim_attach_replay(&sim1, NULL);
    CHECK(rep_orig.frame_count == TOTAL_FRAMES);
    CHECK(check_scene_no_nan((const BattleSceneState*)sim_state_const(&sim1)));

    /* Replay ikili serileştirme + geri yükleme gidiş-dönüş doğrulaması */
    u32 bin_cap = 64u + TOTAL_FRAMES * 2u * (u32)sizeof(SimInput);
    u8* bin_buf = ARENA_PUSH_ARRAY(&a, u8, bin_cap);
    u32 written = sim_replay_save(&rep_orig, bin_buf, bin_cap);
    CHECK(written > sizeof(SimInput) * TOTAL_FRAMES * 2u);

    SimReplay rep_loaded;
    CHECK(sim_replay_load(&rep_loaded, &a, bin_buf, written));
    CHECK(rep_loaded.frame_count == TOTAL_FRAMES);
    CHECK(rep_loaded.seed == SEED);

    /* 2. Koşu: ikinci Sim örneğinde (farklı bellek adresi) yüklenen kaydı 10.000 kare oynat */
    battle_scene_setup(&sim2, &assets, rep_loaded.seed);
    CHECK(sim_replay_run(&sim2, &rep_loaded, hashes_run2, TOTAL_FRAMES));
    CHECK(sim2.current_frame == TOTAL_FRAMES);
    CHECK(check_scene_no_nan((const BattleSceneState*)sim_state_const(&sim2)));

    /* 10.000 karenin tamamında hash dizileri bit-bit aynı olmalı */
    u32 mismatches = 0u;
    u32 distinct_transitions = 0u;
    for (u32 f = 0u; f < TOTAL_FRAMES; f++) {
        if (hashes_run1[f] != hashes_run2[f]) {
            mismatches++;
        }
        if (f > 0u && hashes_run1[f] != hashes_run1[f - 1u]) {
            distinct_transitions++;
        }
    }
    CHECK(mismatches == 0u);
    CHECK(distinct_transitions > 9900u);
    CHECK(sim_state_hash(&sim1) == sim_state_hash(&sim2));
}

/* ============================================================================
 * 5. Benchmark: 8 Kare Geri Sarma + Yeniden Simülasyon Maliyeti (Örnek Sahne)
 * ============================================================================ */

static f64 sort_median(f64* v, u32 n) {
    for (u32 i = 1u; i < n; i++) {
        f64 key = v[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2u];
}

TEST(sim_benchmark_8_frame_rollback) {
    Arena a;
    arena_init(&a, g_sim_mem, sizeof(g_sim_mem));

    /* ~500 üçgenlik BVH'lı statik engel ağı + 16 dinamik fizik gövdesi + ECS hiyerarşisi */
    Mesh* sphere_mesh = mesh_sphere(&a, 1.5f, 16u, 16u);
    Bvh*  sphere_bvh  = bvh_build(&a, sphere_mesh);
    SceneStaticAssets assets = { sphere_mesh, sphere_bvh };

    SimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_size   = KB(32);
    cfg.player_count = 2u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = battle_scene_step;
    cfg.hash_fn      = battle_scene_hash;
    cfg.user         = &assets;

    Sim sim;
    sim_init(&sim, &a, &cfg);
    battle_scene_setup(&sim, &assets, 0xBEEF2026u);

    /* Sahneyi ısındırmak ve aktif mermiler/çarpışmalar oluşturmak için 120 kare ilerlet */
    for (u32 f = 0u; f < 120u; f++) {
        sim_submit_input(&sim, 0u, f, make_test_input(0u, f));
        sim_submit_input(&sim, 1u, f, make_test_input(1u, f));
        sim_step(&sim);
    }

    u32 base_frame = sim.current_frame; /* 120 */
    u32 target_frame = base_frame - 8u; /* 112 (8 kare geri sarma) */
    u64 expected_hash = sim_state_hash(&sim);

    /* Isınma (20 tur) + Ölçüm (101 tur medyan) */
    const u32 WARMUP = 20u;
    const u32 ITERS  = 101u;
    f64 rb_samples[101];
    f64 snap_samples[101];
    f64 hash_samples[101];

    for (u32 i = 0u; i < WARMUP + ITERS; i++) {
        f64 ts0 = plat_time_ms();
        CHECK(sim_restore_snapshot(&sim, target_frame));
        f64 ts1 = plat_time_ms();
        /* current_frame'i tekrar base_frame yapıp tam 8 kare rollback+resim ölçelim */
        sim.current_frame = base_frame;

        f64 t0 = plat_time_ms();
        CHECK(sim_rollback_to(&sim, target_frame));
        f64 t1 = plat_time_ms();

        f64 th0 = plat_time_ms();
        u64 h = sim_state_hash(&sim);
        f64 th1 = plat_time_ms();
        CHECK(h == expected_hash);

        if (i >= WARMUP) {
            snap_samples[i - WARMUP] = ts1 - ts0;
            rb_samples[i - WARMUP]   = t1 - t0;
            hash_samples[i - WARMUP] = th1 - th0;
        }
    }

    f64 med_rb_ms   = sort_median(rb_samples, ITERS);
    f64 med_step_ms = med_rb_ms / 8.0;
    f64 med_snap_ms = sort_median(snap_samples, ITERS);
    f64 med_hash_ms = sort_median(hash_samples, ITERS);

    printf("  [bench M16_A] durum=%u KB | snapshot geri yukleme: %.4f ms | "
           "1 kare sim: %.4f ms | 8 kare rollback+resim: %.4f ms | hash: %.4f ms\n",
           sim.state_size / 1024u, med_snap_ms, med_step_ms, med_rb_ms, med_hash_ms);

    CHECK(sim.current_frame == base_frame);
    CHECK(sim_state_hash(&sim) == expected_hash);
}
