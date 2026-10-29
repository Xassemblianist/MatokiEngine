/* tests/test_gelgit.c — "Gelgit" (OYUN1) yelken fiziği, 6 görev, XPBD bez, 3x replay ve 1T=8T testleri.
 * Orsa açısı (°), dalga sörfü hız artışı (%), alabora eşiği (°) ve 3 sn kurtarma ölçümlerini,
 * aynı girdi kaydının 3 kez oynatıldığında aynı 64-bit FNV-1a özeti (hash) ürettiğini ve
 * 1T ile 8T paralel FFT okyanus + tekne simülasyonunun bit-bit aynı özeti verdiğini doğrular.
 */
#include <stdio.h>
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "plat/platform.h"
#include "thread/thread.h"
#include "thread/job.h"
#include "sim/sim.h"
#include "gfx3d/ocean.h"
#include "gfx3d/sail.h"
#include "game/gelgit.h"

static u8 g_gelgit_test_mem[MB(24)];

/* 1. Polar tablo, orsa açısı (~43°), dalga sörfü (%35-%55) ve alabora (~58°, 3 sn) doğrulaması */
TEST(gelgit_polar_and_physics_metrics) {
    Arena a;
    arena_init(&a, g_gelgit_test_mem, sizeof(g_gelgit_test_mem));

    /* Polar eğri sınır ve tepe kontrolleri */
    f32 cl0 = 0.0f, cd0 = 0.0f;
    gelgit_polar_eval(0.0f, 0u, &cl0, &cd0);
    CHECK_NEAR(cl0, 0.0f, 1e-4f);
    CHECK(cd0 >= 0.05f && cd0 <= 0.10f);

    f32 cl_opt = 0.0f, cd_opt = 0.0f;
    gelgit_polar_eval(25.0f * (M_PI / 180.0f), 0u, &cl_opt, &cd_opt);
    CHECK(cl_opt > 1.45f);
    CHECK(cl_opt / cd_opt > 6.0f);

    /* Yükseltme 1 (Yelken Kesimi) C_L'yi %15 artırmalı */
    f32 cl_upg = 0.0f, cd_upg = 0.0f;
    gelgit_polar_eval(25.0f * (M_PI / 180.0f), GELGIT_UPG_SAIL_CUT, &cl_upg, &cd_upg);
    CHECK_NEAR(cl_upg, cl_opt * 1.15f, 1e-3f);

    /* Gerçek CPU FFT okyanus ile fizik metriklerini ölç */
    OceanDesc od = ocean_default_desc();
    Ocean ocean;
    ocean_init(&ocean, &a, &od);

    GelgitPhysicsMetrics m = gelgit_measure_physics(&ocean);
    printf("  [gelgit-metrics] upwind=%.1f deg (v=%.2f m/s), reach=%.2f m/s, surf=%.2f m/s (+%.1f%%), capsize=%.1f deg, recovery=%.2f s\n",
           (double)m.upwind_angle_deg, (double)m.upwind_speed_mps,
           (double)m.reach_speed_mps, (double)m.surf_speed_mps, (double)m.surf_boost_pct,
           (double)m.capsize_threshold_deg, (double)m.recovery_time_sec);

    /* Kabul kriterleri:
     * - Orsa açısı ~42°..46° aralığında VMG > 0 vermeli
     * - Dalga sırtında sörf hız artışı %30..%65 aralığında olmalı
     * - Alabora eşiği ~58° (55°..64°) ve kurtarma süresi ~3.0 sn olmalı
     */
    CHECK(m.upwind_angle_deg >= 40.0f && m.upwind_angle_deg <= 47.0f);
    CHECK(m.surf_boost_pct >= 30.0f && m.surf_boost_pct <= 68.0f);
    CHECK(m.capsize_threshold_deg >= 56.0f && m.capsize_threshold_deg <= 64.0f);
    CHECK_NEAR(m.recovery_time_sec, 3.0f, 0.15f);
}

/* 2. Tramola (tacking), 6 görev + Günün Denizi + 3 yükseltme akışı doğrulaması */
TEST(gelgit_tacking_and_6_missions) {
    GelgitState st;

    /* Tramola testi: sancak kontradan iskele kontraya geçişte tack_count artmalı */
    gelgit_state_init(&st, 0x2026u, 1u, 0u, 0u);
    for (u32 g = 0; g < GELGIT_MAX_GUSTS; g++) st.weather.gusts[g].boost_mps = 0.0f;
    st.boat.yaw = 0.80f; /* +46° sancak kontra */
    st.boat.vel = vec3(m_sin(0.80f) * 3.5f, 0.0f, m_cos(0.80f) * 3.5f);

    for (u32 t = 0; t < 30u; t++) {
        SimInput in = {0};
        gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
    }
    CHECK(st.boat.tack_side != 0);

    /* Dümeni kırarak rüzgâr üstünden karşı kontraya (-46°) geç */
    for (u32 t = 0; t < 130u; t++) {
        SimInput in = {0};
        in.buttons = GELGIT_BTN_RUDDER_LEFT;
        gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
    }
    CHECK(st.boat.tack_count >= 1u);

    /* 6 görevin hepsinin başlangıçtan kazanma durumuna (WON) ilerleyebildiğini doğrula */
    for (u32 m_id = 0u; m_id < GELGIT_MISSION_COUNT; m_id++) {
        gelgit_state_init(&st, 0x6E161701u + m_id, m_id, st.upgrades_mask, st.completed_mask);
        CHECK(st.mission_status == GELGIT_STATUS_PLAYING);
        CHECK(gelgit_mission_title(m_id)[0] != '\0');
        CHECK(gelgit_mission_objective(&st)[0] != '\0');

        if (m_id == 3u) {
            /* Görev 4: önce alabora ol, sonra 3 sn kurtar, sonra şamandıraya var */
            st.boat.roll = 1.15f;
            SimInput in0 = {0};
            gelgit_sim_step(&st, &in0, 1u, GELGIT_FIXED_DT, 0);
            CHECK(st.boat.capsized == 1);
            for (u32 t = 0; t < 185u; t++) {
                SimInput in_rec = {0};
                in_rec.buttons = GELGIT_BTN_HIKE;
                gelgit_sim_step(&st, &in_rec, 1u, GELGIT_FIXED_DT, 0);
            }
            CHECK(st.boat.capsized == 0);
            CHECK(st.mission_stage == 1u);
        }

        /* Tüm kapıları sırayla ziyaret et */
        for (u32 w = 0u; w < st.waypoint_count; w++) {
            st.boat.pos.x = st.waypoints[w].x;
            st.boat.pos.z = st.waypoints[w].z;
            SimInput in = {0};
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
        }
        CHECK(st.mission_status == GELGIT_STATUS_WON);
        CHECK((st.completed_mask & (1u << m_id)) != 0u);
    }

    /* Tüm görevler bitince 3 yükseltmenin de açılmış olduğunu doğrula */
    CHECK((st.upgrades_mask & GELGIT_UPG_SAIL_CUT)    != 0u);
    CHECK((st.upgrades_mask & GELGIT_UPG_DEEP_KEEL)   != 0u);
    CHECK((st.upgrades_mask & GELGIT_UPG_COPPER_HULL) != 0u);

    /* Günün Denizi (Mod 6) ve hayalet tekne (inputs[1]) doğrulaması */
    gelgit_state_init(&st, 0x20261029u, GELGIT_MODE_DAILY_SEA, st.upgrades_mask, st.completed_mask);
    SimInput dual_in[2] = {0};
    dual_in[0].buttons = GELGIT_BTN_SHEET_IN;
    dual_in[1].buttons = GELGIT_BTN_RUDDER_RIGHT;
    for (u32 t = 0; t < 30u; t++) {
        gelgit_sim_step(&st, dual_in, 2u, GELGIT_FIXED_DT, 0);
    }
    CHECK(st.ghost_active == 1);
    CHECK(m_abs(st.boat.yaw - st.ghost.yaw) > 0.01f);
}

/* 3. Görsel sınıf XPBD 9x9 yelken bezi (SailCloth) doğrulaması */
TEST(gelgit_sail_cloth_xpbd) {
    Arena a;
    arena_init(&a, g_gelgit_test_mem, sizeof(g_gelgit_test_mem));

    SailCloth sail;
    Mesh* m = sail_cloth_init(&sail, &a, -2.55f, 0.55f, 0.25f, 0.65f, 1.05f, 7.60f, 0.42f);
    CHECK(m != 0);
    CHECK(m->vertex_count == SAIL_VERT_COUNT);

    for (u32 i = 0; i < 30u; i++) {
        sail_cloth_step(&sail, vec3(8.5f, 0.0f, -4.0f), -0.45f, 0.35f, GELGIT_FIXED_DT);
    }

    /* Direk kenarı (i=SAIL_GRID_DIV) sabit kalmalı, orta göbek (i=4, j=4) rüzgârla şişmeli */
    f32 mast_x = sail.px[4 * SAIL_GRID_N + SAIL_GRID_DIV];
    f32 mid_x  = sail.px[4 * SAIL_GRID_N + 4];
    CHECK_NEAR(mast_x, 0.0f, 1e-4f);
    CHECK(m_abs(mid_x) > 0.08f);
}

/* 4. Replay doğrulaması: aynı girdi kaydı 3 kez oynatıldığında birebir aynı 64-bit FNV-1a özeti üretmeli */
TEST(gelgit_replay_3x_identical_hash) {
    Arena a;
    arena_init(&a, g_gelgit_test_mem, sizeof(g_gelgit_test_mem));

    const u32 TICKS = 300u;
    const u32 SEED  = 0x6E161701u;

    SimConfig cfg = {0};
    cfg.state_size   = sizeof(GelgitState) + 64u;
    cfg.player_count = 1u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = GELGIT_FIXED_DT;
    cfg.step_fn      = gelgit_sim_step;
    cfg.hash_fn      = gelgit_state_hash;
    cfg.user         = 0;

    Sim sim;
    sim_init(&sim, &a, &cfg);
    SimOffset off = sim_state_alloc(&sim, sizeof(GelgitState), _Alignof(GelgitState));
    CHECK(off == 0u);

    SimReplay rep;
    sim_replay_init(&rep, &a, 1u, TICKS + 16u, SEED, GELGIT_FIXED_DT);
    sim_attach_replay(&sim, &rep);

    GelgitState* st = (GelgitState*)sim_state(&sim);
    gelgit_state_init(st, SEED, 5u, GELGIT_UPG_SAIL_CUT | GELGIT_UPG_DEEP_KEEL, 0u);

    /* Canlı çalıştırma ve girdi kaydı */
    for (u32 t = 0; t < TICKS; t++) {
        SimInput in = {0};
        if ((t % 90u) < 40u)       in.buttons |= GELGIT_BTN_RUDDER_LEFT;
        else if ((t % 90u) < 80u)  in.buttons |= GELGIT_BTN_RUDDER_RIGHT;
        if ((t % 50u) < 25u)       in.buttons |= GELGIT_BTN_SHEET_IN;
        else                       in.buttons |= GELGIT_BTN_SHEET_OUT;
        if ((t % 70u) < 35u)       in.buttons |= GELGIT_BTN_HIKE;
        in.move_x = (i16)(((i32)(t * 173u) % 20000) - 10000);

        sim_submit_input(&sim, 0u, t, in);
        sim_step(&sim);
    }

    u64 live_hash = sim_state_hash(&sim);
    sim_attach_replay(&sim, 0);

    u32 rec_cap = 32u + TICKS * (u32)sizeof(SimInput) + 64u;
    u8* rec_buf = ARENA_PUSH_ARRAY(&a, u8, rec_cap);
    u32 rec_bytes = sim_replay_save(&rep, rec_buf, rec_cap);
    CHECK(rec_bytes > 0u);

    /* Aynı girdi kaydını 3 kez baştan sona oynat ve her birinin 64-bit özetini karşılaştır */
    u64 replay_hashes[3] = {0, 0, 0};
    for (u32 run = 0; run < 3u; run++) {
        sim_reset(&sim);
        SimOffset roff = sim_state_alloc(&sim, sizeof(GelgitState), _Alignof(GelgitState));
        CHECK(roff == 0u);
        GelgitState* rst = (GelgitState*)sim_state(&sim);
        gelgit_state_init(rst, SEED, 5u, GELGIT_UPG_SAIL_CUT | GELGIT_UPG_DEEP_KEEL, 0u);

        SimReplay loaded_rep;
        CHECK(sim_replay_load(&loaded_rep, &a, rec_buf, rec_bytes) == TRUE);
        CHECK(sim_replay_run(&sim, &loaded_rep, 0, 0) == TRUE);
        replay_hashes[run] = sim_state_hash(&sim);
    }

    printf("  [gelgit-replay] live=0x%016llx, run1=0x%016llx, run2=0x%016llx, run3=0x%016llx\n",
           (unsigned long long)live_hash,
           (unsigned long long)replay_hashes[0],
           (unsigned long long)replay_hashes[1],
           (unsigned long long)replay_hashes[2]);

    CHECK(replay_hashes[0] == live_hash);
    CHECK(replay_hashes[0] == replay_hashes[1]);
    CHECK(replay_hashes[1] == replay_hashes[2]);
}

/* 5. 1T ve 8T paralel FFT okyanus + tekne simülasyonu bit-bit aynı özeti üretmeli */
TEST(gelgit_1t_vs_8t_determinism) {
    Arena a;
    arena_init(&a, g_gelgit_test_mem, sizeof(g_gelgit_test_mem));

    const u32 TICKS = 120u;
    u64 hash_1t = 0;
    u64 hash_8t = 0;
    double ms_per_tick_1t = 0.0;
    double ms_per_tick_8t = 0.0;

    for (u32 mode = 0; mode < 2u; mode++) {
        usize mark = arena_mark(&a);
        job_system_init();
        /* mode 0: 1T (0 worker), mode 1: 8T (7 worker kuyruk yolu) */
        engine_set_worker_count(mode == 0u ? 0u : 7u);

        OceanDesc od = ocean_default_desc();
        Ocean ocean;
        ocean_init(&ocean, &a, &od);

        GelgitState st;
        gelgit_state_init(&st, 0x6E161701u, 5u, GELGIT_UPG_SAIL_CUT, 0u);
        GelgitSimCtx ctx = { &ocean, 1 };

        f64 t0 = plat_time_ms();

        for (u32 t = 0; t < TICKS; t++) {
            SimInput in = {0};
            if ((t & 15u) < 8u) in.buttons |= GELGIT_BTN_RUDDER_LEFT | GELGIT_BTN_HIKE;
            else                in.buttons |= GELGIT_BTN_RUDDER_RIGHT | GELGIT_BTN_SHEET_IN;
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, &ctx);
        }

        f64 t1 = plat_time_ms();
        double elapsed_ms = (double)(t1 - t0);

        u64 h = gelgit_state_hash(&st, sizeof(st), 0);
        if (mode == 0u) {
            hash_1t = h;
            ms_per_tick_1t = elapsed_ms / (double)TICKS;
        } else {
            hash_8t = h;
            ms_per_tick_8t = elapsed_ms / (double)TICKS;
        }

        engine_set_worker_count(0u);
        arena_pop_to(&a, mark);
    }

    printf("  [gelgit-1T-8T] hash_1T=0x%016llx (%.3f ms/tick), hash_8T=0x%016llx (%.3f ms/tick)\n",
           (unsigned long long)hash_1t, ms_per_tick_1t,
           (unsigned long long)hash_8t, ms_per_tick_8t);

    CHECK(hash_1t != 0ULL);
    CHECK(hash_1t == hash_8t);
}
