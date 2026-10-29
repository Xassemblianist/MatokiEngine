/* game/gelgit.c — "Gelgit" (OYUN1) deterministik yelken fiziği, fırtına ve görev simülasyonu.
 * 60 Hz sabit adımlı, işaretçisiz GelgitState üzerinde çalışır (D-110, D-112).
 * Görünür rüzgâr (V_aw = V_tw - V_boat), 10 düğümlü C_L/C_D polar eğrisi, 10 noktalı
 * gövde kaldırma kuvveti, dalga sırtı sörfü, alabora/kurtarma ve 6 görevi uygular.
 */
#include "game/gelgit.h"

/* 10 noktalı tekne gövdesi kaldırma kuvveti (buoyancy) prob tablosu */
typedef struct {
    f32 lx, ly, lz;     /* tekne yerel konumu (m) */
    f32 volume_w;       /* toplam deplasman hacmi ağırlığı (toplam = 1.0) */
} HullProbeDef;

static const HullProbeDef k_hull_probes[GELGIT_HULL_PROBES] = {
    {  0.00f, -0.15f,  4.25f, 0.07f }, /* 0: Pruva (baş) */
    {  0.00f, -0.22f, -3.60f, 0.08f }, /* 1: Pupa (kıç) */
    { -1.05f, -0.30f,  2.00f, 0.11f }, /* 2: İskele baş omuzluk */
    {  1.05f, -0.30f,  2.00f, 0.11f }, /* 3: Sancak baş omuzluk */
    { -1.52f, -0.38f,  0.15f, 0.14f }, /* 4: İskele kemere (orta) */
    {  1.52f, -0.38f,  0.15f, 0.14f }, /* 5: Sancak kemere (orta) */
    { -1.25f, -0.32f, -1.95f, 0.11f }, /* 6: İskele kıç omuzluk */
    {  1.25f, -0.32f, -1.95f, 0.11f }, /* 7: Sancak kıç omuzluk */
    {  0.00f, -0.58f,  0.90f, 0.07f }, /* 8: Omurga ön */
    {  0.00f, -0.58f, -1.10f, 0.06f }, /* 9: Omurga arka */
};

/* 10 düğümlü C_L / C_D aerodinamik polar tablosu (alpha derece: 0..180) */
typedef struct {
    f32 alpha_deg;
    f32 cl;
    f32 cd;
} PolarNode;

static const PolarNode k_polar_table[GELGIT_POLAR_NODES] = {
    {   0.0f, 0.00f, 0.06f },
    {   5.0f, 0.42f, 0.08f },
    {  12.0f, 1.15f, 0.11f },
    {  20.0f, 1.48f, 0.16f },
    {  28.0f, 1.60f, 0.25f },
    {  40.0f, 1.32f, 0.48f },
    {  60.0f, 0.92f, 0.88f },
    {  90.0f, 0.35f, 1.32f },
    { 135.0f, 0.12f, 1.45f },
    { 180.0f, 0.00f, 1.38f },
};

/* Deterministik Xorshift32 PRNG */
static u32 prng_next(u32* state) {
    u32 x = *state;
    if (x == 0u) x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static f32 prng_f32(u32* state) {
    return (f32)(prng_next(state) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

static f32 prng_range(u32* state, f32 lo, f32 hi) {
    return lo + (hi - lo) * prng_f32(state);
}

static void mem_zero(void* ptr, usize n) {
    u8* p = (u8*)ptr;
    for (usize i = 0; i < n; i++) p[i] = 0;
}

static i32 clamp_i32(i32 x, i32 lo, i32 hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Açıyı [-PI, +PI] aralığına sarar */
static f32 wrap_pi(f32 a) {
    while (a >  M_PI) a -= M_TAU;
    while (a < -M_PI) a += M_TAU;
    return a;
}

GelgitTuning gelgit_default_tuning(void) {
    GelgitTuning t;
    t.sail_area   = 22.0f;
    t.boat_mass   = 680.0f;
    t.keel_gm     = 1.25f;
    t.surf_gain   = 1.55f;
    t.capsize_deg = 58.0f;
    t.rudder_gain = 1.00f;
    return t;
}

void gelgit_polar_eval(f32 alpha_rad, u32 upgrades_mask, f32* out_cl, f32* out_cd) {
    f32 deg = m_abs(alpha_rad) * (180.0f / M_PI);
    deg = m_clamp(deg, 0.0f, 180.0f);

    f32 cl = 0.0f;
    f32 cd = 0.06f;

    for (u32 i = 0; i + 1 < GELGIT_POLAR_NODES; i++) {
        f32 a0 = k_polar_table[i].alpha_deg;
        f32 a1 = k_polar_table[i + 1].alpha_deg;
        if (deg >= a0 && deg <= a1) {
            f32 t = (deg - a0) / (a1 - a0);
            cl = m_lerp(k_polar_table[i].cl, k_polar_table[i + 1].cl, t);
            cd = m_lerp(k_polar_table[i].cd, k_polar_table[i + 1].cd, t);
            break;
        }
    }

    if (upgrades_mask & GELGIT_UPG_SAIL_CUT) {
        cl *= 1.15f;
    }

    if (out_cl) *out_cl = cl;
    if (out_cd) *out_cd = cd;
}

void gelgit_sample_wind(const GelgitWeather* w, f32 x, f32 z, Vec3* out_wind, f32* out_gust_factor) {
    f32 gust_boost = 0.0f;
    f32 gust_norm  = 0.0f;

    for (u32 i = 0; i < GELGIT_MAX_GUSTS; i++) {
        const GelgitGust* g = &w->gusts[i];
        if (g->radius <= 0.1f) continue;
        f32 dx = x - g->x;
        f32 dz = z - g->z;
        f32 d2 = dx * dx + dz * dz;
        f32 r2 = g->radius * g->radius;
        if (d2 < r2) {
            f32 q = 1.0f - (d2 / r2);
            f32 w_cell = q * q;
            gust_boost += g->boost_mps * w_cell;
            if (w_cell > gust_norm) gust_norm = w_cell;
        }
    }

    f32 total_speed = w->cur_wind_speed + gust_boost;
    if (out_wind) {
        *out_wind = vec3(w->cur_wind_x * total_speed, 0.0f, w->cur_wind_z * total_speed);
    }
    if (out_gust_factor) {
        *out_gust_factor = m_clamp(gust_norm, 0.0f, 1.0f);
    }
}

static void setup_archipelago(GelgitState* st) {
    /* 4 sabit ada: Liman Adası, Kuzey Kayası, Doğu Resifi, Fırtına Burnu */
    st->islands[0] = (GelgitIsland){   0.0f,  -72.0f, 26.0f, 11.0f };
    st->islands[1] = (GelgitIsland){ -95.0f,  155.0f, 30.0f, 18.0f };
    st->islands[2] = (GelgitIsland){ 165.0f,   85.0f, 24.0f, 14.0f };
    st->islands[3] = (GelgitIsland){  30.0f,  310.0f, 34.0f, 22.0f };

    /* 2 deniz feneri: Liman Feneri ve Kuzey Fırtına Feneri */
    st->lighthouses[0] = (GelgitLighthouse){  16.0f,  -55.0f,  6.5f, 14.0f, 0.0f };
    st->lighthouses[1] = (GelgitLighthouse){  12.0f,  288.0f, 11.0f, 16.0f, 2.1f };
}

static void init_boat_at(GelgitBoat* b, f32 x, f32 z, f32 yaw_rad, f32 sheet_rad) {
    mem_zero(b, sizeof(*b));
    b->pos          = vec3(x, 0.08f, z);
    b->yaw          = yaw_rad;
    b->sheet_angle  = sheet_rad;
    b->boom_angle   = sheet_rad;
    b->cargo_health = 100.0f;
}

static void init_gusts(GelgitWeather* w, u32 seed, f32 boost_scale) {
    u32 rng = seed ^ 0xA53C9E11u;
    for (u32 i = 0; i < GELGIT_MAX_GUSTS; i++) {
        GelgitGust* g = &w->gusts[i];
        g->x         = prng_range(&rng, -180.0f, 180.0f);
        g->z         = prng_range(&rng,  -60.0f, 300.0f);
        g->radius    = prng_range(&rng,   28.0f,  52.0f);
        g->boost_mps = prng_range(&rng,    3.2f,   6.0f) * boost_scale;
        f32 spd      = w->base_wind_speed * 0.65f;
        g->vx        = w->cur_wind_x * spd;
        g->vz        = w->cur_wind_z * spd;
    }
}

void gelgit_start_mission(GelgitState* st, u32 mission_id) {
    if (mission_id >= GELGIT_TOTAL_MODES) mission_id = 0u;

    u32 saved_seed      = st->seed ? st->seed : 0x6E161701u;
    u32 saved_upgrades  = st->upgrades_mask;
    u32 saved_completed = st->completed_mask;
    u32 saved_gold      = st->gold;
    f32 saved_best      = st->best_daily_time;
    GelgitTuning saved_tuning = st->tuning;
    if (saved_tuning.sail_area <= 1.0f) {
        saved_tuning = gelgit_default_tuning();
    }

    mem_zero(st, sizeof(*st));
    st->seed            = saved_seed;
    st->mission_id      = mission_id;
    st->mission_stage   = 0u;
    st->mission_status  = GELGIT_STATUS_PLAYING;
    st->upgrades_mask   = saved_upgrades;
    st->completed_mask  = saved_completed;
    st->gold            = saved_gold;
    st->best_daily_time = saved_best;
    st->tuning          = saved_tuning;

    setup_archipelago(st);

    /* Varsayılan rüzgâr: Kuzeyden güneye (0, -1) esiyor -> Kuzey (+Z) rüzgâr üstü (upwind)! */
    GelgitWeather* w = &st->weather;
    w->base_wind_dir   = M_PI; /* sin(PI)=0, cos(PI)=-1 -> rüzgâr -Z yönüne eser, +Z orsa yönüdür */
    w->base_wind_speed = 10.5f;
    w->cur_wind_speed  = 10.5f;
    w->cur_wind_x      = 0.0f;
    w->cur_wind_z      = -1.0f;
    w->storm_intensity = 0.08f;
    w->target_storm    = 0.08f;
    w->front_line_dist = 420.0f;
    w->front_speed     = 0.0f;
    w->sun_elev        = 0.20f;   /* altın saat gün batımı (~11.5°) */
    w->sun_azim        = 0.62f;

    switch (mission_id) {
        case 0u: /* Görev 1 (Öğretici 1): Rüzgârı Yakala (Apaz seyrinde ıskota ayarı) */
            w->base_wind_speed = 10.5f;
            w->cur_wind_speed  = 10.5f;
            /* Başta ıskota çok laçka (1.35 rad) başlar; oyuncu W ile çekip hızlanarak 2 kapıdan geçer */
            init_boat_at(&st->boat, 0.0f, 0.0f, M_PI * 0.5f, 1.35f);
            st->waypoint_count = 2u;
            st->waypoints[0] = (GelgitWaypoint){  65.0f,  5.0f, 16.0f, 0u };
            st->waypoints[1] = (GelgitWaypoint){ 135.0f, 12.0f, 18.0f, 0u };
            init_gusts(w, saved_seed + 11u, 0.8f);
            break;

        case 1u: /* Görev 2 (Öğretici 2): Zikzak (Orsa & Tramola / Tacking) */
            w->base_wind_speed = 11.5f;
            w->cur_wind_speed  = 11.5f;
            /* Kuzeydeki (+Z) şamandıraya ~44° orsa açısıyla ulaşıp en az 2 tramola atma */
            init_boat_at(&st->boat, -25.0f, -10.0f, 0.76f, 0.25f);
            st->waypoint_count = 2u;
            st->waypoints[0] = (GelgitWaypoint){  28.0f,  45.0f, 18.0f, 0u };
            st->waypoints[1] = (GelgitWaypoint){ -18.0f,  98.0f, 20.0f, 0u };
            init_gusts(w, saved_seed + 23u, 0.9f);
            break;

        case 2u: /* Görev 3 (Öğretici 3): Dalga Sırtı (Wave Surfing) */
            w->base_wind_speed = 15.0f;
            w->cur_wind_speed  = 15.0f;
            w->storm_intensity = 0.52f;
            w->target_storm    = 0.52f;
            w->sun_elev        = 0.14f;
            /* Güneye (-Z, pupa/geniş apaz) dalgalarla aynı yönde sörf */
            init_boat_at(&st->boat, 0.0f, 120.0f, M_PI * 0.92f, 1.05f);
            st->waypoint_count = 1u;
            st->waypoints[0] = (GelgitWaypoint){ -15.0f, -15.0f, 24.0f, 0u };
            init_gusts(w, saved_seed + 37u, 1.35f);
            break;

        case 3u: /* Görev 4 (Öğretici 4): Alabora ve Kurtarma */
            w->base_wind_speed = 16.5f;
            w->cur_wind_speed  = 16.5f;
            w->storm_intensity = 0.68f;
            w->target_storm    = 0.68f;
            w->sun_elev        = 0.10f;
            /* Başlangıçta sert borda rüzgârında yatık başlar; alabora sonrası 3 sn kurtarma + şamandıra */
            init_boat_at(&st->boat, 0.0f, 20.0f, M_PI * 0.5f, 0.22f);
            st->boat.roll = 0.92f; /* ~53°, ilk hamlede devrilmeye hazır veya doğrudan test edilebilir */
            st->waypoint_count = 1u;
            st->waypoints[0] = (GelgitWaypoint){ 75.0f, 20.0f, 20.0f, 0u };
            init_gusts(w, saved_seed + 53u, 1.6f);
            break;

        case 4u: /* Görev 5 (Teslimat 1): Limandan Fener Adasına Kargo */
            w->base_wind_speed = 11.5f;
            w->cur_wind_speed  = 11.5f;
            w->storm_intensity = 0.18f;
            w->target_storm    = 0.25f;
            init_boat_at(&st->boat, 0.0f, -38.0f, 0.65f, 0.42f);
            st->boat.has_cargo    = 1;
            st->boat.cargo_health = 100.0f;
            st->boat.cargo_mass   = 95.0f;
            st->waypoint_count = 2u;
            st->waypoints[0] = (GelgitWaypoint){  75.0f,  25.0f, 20.0f, 0u };
            st->waypoints[1] = (GelgitWaypoint){ 138.0f,  78.0f, 22.0f, 2u }; /* Doğu Resifi İskelesi */
            init_gusts(w, saved_seed + 71u, 1.1f);
            break;

        case 5u: /* Görev 6 (Teslimat 2 - Fırtına): Fırtına Cephesinde Acil İlaç Teslimatı */
            w->base_wind_speed = 17.5f;
            w->cur_wind_speed  = 17.5f;
            w->storm_intensity = 0.85f;
            w->target_storm    = 1.00f;
            w->front_line_dist = 90.0f;
            w->front_speed     = 4.5f;
            w->sun_elev        = 0.05f;
            init_boat_at(&st->boat, 0.0f, -32.0f, 0.72f, 0.32f);
            st->boat.has_cargo    = 1;
            st->boat.cargo_health = 100.0f;
            st->boat.cargo_mass   = 110.0f;
            st->waypoint_count = 2u;
            st->waypoints[0] = (GelgitWaypoint){ -48.0f, 110.0f, 22.0f, 0u };
            st->waypoints[1] = (GelgitWaypoint){  15.0f, 265.0f, 24.0f, 2u }; /* Kuzey Fırtına Feneri */
            init_gusts(w, saved_seed + 97u, 1.85f);
            break;

        case GELGIT_MODE_DAILY_SEA: { /* Mod 6: Günün Denizi (Seeded parkur + hayalet tekne) */
            u32 rng = saved_seed ^ 0xD41175EAu;
            f32 w_spd = prng_range(&rng, 11.5f, 15.5f);
            f32 storm = prng_range(&rng,  0.20f,  0.65f);
            w->base_wind_speed = w_spd;
            w->cur_wind_speed  = w_spd;
            w->storm_intensity = storm;
            w->target_storm    = storm;
            init_boat_at(&st->boat,  0.0f, -25.0f, 0.75f, 0.38f);
            init_boat_at(&st->ghost, 0.0f, -25.0f, 0.75f, 0.38f);
            st->waypoint_count = 3u;
            st->waypoints[0] = (GelgitWaypoint){  72.0f,  35.0f, 18.0f, 0u };
            st->waypoints[1] = (GelgitWaypoint){ -25.0f, 115.0f, 18.0f, 0u };
            st->waypoints[2] = (GelgitWaypoint){   5.0f,  -5.0f, 20.0f, 0u };
            init_gusts(w, rng, 1.25f);
            break;
        }

        case GELGIT_MODE_FREE_SAIL:
        default:
            w->base_wind_speed = 11.5f;
            w->cur_wind_speed  = 11.5f;
            w->storm_intensity = 0.15f;
            w->target_storm    = 0.15f;
            init_boat_at(&st->boat, 0.0f, -30.0f, 0.78f, 0.40f);
            st->waypoint_count = 0u;
            init_gusts(w, saved_seed + 131u, 1.0f);
            break;
    }
}

void gelgit_state_init(GelgitState* st, u32 seed, u32 mission_id, u32 upgrades_mask, u32 completed_mask) {
    mem_zero(st, sizeof(*st));
    st->seed           = seed ? seed : 0x6E161701u;
    st->upgrades_mask  = upgrades_mask;
    st->completed_mask = completed_mask;
    st->tuning         = gelgit_default_tuning();
    gelgit_start_mission(st, mission_id);
}

static void step_weather(GelgitState* st, f32 dt) {
    GelgitWeather* w = &st->weather;

    /* Fırtına cephesi ilerlemesi */
    if (w->front_speed > 0.0f && w->front_line_dist > -200.0f) {
        w->front_line_dist -= w->front_speed * dt;
    }

    f32 storm_diff = w->target_storm - w->storm_intensity;
    w->storm_intensity += m_clamp(storm_diff, -0.08f * dt, 0.08f * dt);
    w->storm_intensity = m_clamp(w->storm_intensity, 0.0f, 1.0f);

    /* Rüzgâr yönü ve hızında deterministik mikro salınım */
    f32 t_sec = (f32)st->tick * GELGIT_FIXED_DT;
    f32 dir_osc = 0.04f * m_sin(t_sec * 0.17f);
    f32 cur_dir = w->base_wind_dir + dir_osc;
    w->cur_wind_x = m_sin(cur_dir);
    w->cur_wind_z = m_cos(cur_dir);
    w->cur_wind_speed = w->base_wind_speed * (1.0f + 0.22f * w->storm_intensity);

    /* Kedi pençesi (gust) hücrelerini rüzgârla sürükle ve sınır dışına çıkanları sar */
    for (u32 i = 0; i < GELGIT_MAX_GUSTS; i++) {
        GelgitGust* g = &w->gusts[i];
        g->vx = w->cur_wind_x * (w->cur_wind_speed * 0.65f);
        g->vz = w->cur_wind_z * (w->cur_wind_speed * 0.65f);
        g->x += g->vx * dt;
        g->z += g->vz * dt;
        if (g->z < -140.0f) g->z += 480.0f;
        if (g->z >  340.0f) g->z -= 480.0f;
        if (g->x < -220.0f) g->x += 440.0f;
        if (g->x >  220.0f) g->x -= 440.0f;
    }

    /* Fırtınada deterministik şimşek ve gök gürültüsü */
    if (w->lightning_flash > 0.0f) {
        w->lightning_flash = m_max(0.0f, w->lightning_flash - 3.2f * dt);
    }
    if (w->storm_intensity > 0.60f) {
        u32 h = (st->tick * 747796405u) ^ st->seed;
        h = ((h >> 16) ^ h) * 0x45d9f3bu;
        if ((st->tick % 240u) == 180u && (h & 3u) == 0u) {
            w->lightning_flash = 1.0f;
            w->thunder_tick    = st->tick;
        }
    }

    /* Deniz feneri fazlarını döndür */
    for (u32 i = 0; i < GELGIT_MAX_LIGHTHOUSES; i++) {
        st->lighthouses[i].beam_phase = wrap_pi(st->lighthouses[i].beam_phase + 1.15f * dt);
    }
}

static void step_single_boat(
    GelgitBoat*         b,
    const GelgitState*  st,
    const SimInput*     in,
    f32                 dt,
    const Ocean*        ocean
) {
    const GelgitTuning*  tun = &st->tuning;
    const GelgitWeather* w   = &st->weather;
    u32 upg = st->upgrades_mask;

    /* 1. Dümen girdi tamponu (100 ms = 6 tick) ve kontrol girdileri */
    i16 raw_steer = in ? in->move_x : 0;
    if (in) {
        if (in->buttons & GELGIT_BTN_RUDDER_LEFT)  raw_steer = -32767;
        if (in->buttons & GELGIT_BTN_RUDDER_RIGHT) raw_steer =  32767;
    }
    u32 buf_idx = b->rudder_buf_pos % GELGIT_RUDDER_BUF_TICKS;
    b->rudder_buf[buf_idx] = raw_steer;
    b->rudder_buf_pos = (buf_idx + 1u) % GELGIT_RUDDER_BUF_TICKS;

    f32 steer_sum = 0.0f;
    for (u32 i = 0; i < GELGIT_RUDDER_BUF_TICKS; i++) {
        steer_sum += (f32)b->rudder_buf[i] * (1.0f / 32767.0f);
    }
    f32 steer_cmd = m_clamp(steer_sum * (1.0f / (f32)GELGIT_RUDDER_BUF_TICKS), -1.0f, 1.0f);

    /* İvme-tabanlı dümen açısı (rad, -0.55 .. +0.55) */
    f32 target_rudder = steer_cmd * 0.55f;
    f32 rudder_acc    = (target_rudder - b->rudder_angle) * 28.0f - b->rudder_vel * 8.5f;
    b->rudder_vel   += rudder_acc * dt;
    b->rudder_angle  = m_clamp(b->rudder_angle + b->rudder_vel * dt, -0.55f, 0.55f);

    /* Iskota (sheet) kontrolü: W / move_y > 0 yelkeni kasar (küçültür), S / move_y < 0 laçka eder (büyütür) */
    f32 sheet_rate = 0.0f;
    if (in) {
        if (in->buttons & GELGIT_BTN_SHEET_IN)  sheet_rate -= 0.85f;
        if (in->buttons & GELGIT_BTN_SHEET_OUT) sheet_rate += 0.85f;
        if (sheet_rate == 0.0f && in->move_y != 0) {
            sheet_rate = -(f32)in->move_y * (0.85f / 32767.0f);
        }
    }
    b->sheet_angle = m_clamp(b->sheet_angle + sheet_rate * dt, 0.21f, 1.40f);

    /* Ağırlık atma (Hiking - Shift) */
    f32 hike_target = (in && (in->buttons & GELGIT_BTN_HIKE)) ? 1.0f : 0.0f;
    b->hike_amount += (hike_target - b->hike_amount) * m_min(1.0f, 7.0f * dt);

    /* 2. Tekne yerel eksenleri: fwd = (sin(yaw), 0, cos(yaw)), right = (cos(yaw), 0, -sin(yaw)) */
    f32 sy = m_sin(b->yaw);
    f32 cy = m_cos(b->yaw);
    Vec3 fwd   = vec3(sy, 0.0f,  cy);
    Vec3 right = vec3(cy, 0.0f, -sy);

    /* 3. 10 noktalı CPU FFT okyanus kaldırma kuvveti (buoyancy) ve boyuna dalga eğimi */
    f32 total_mass = tun->boat_mass + (b->has_cargo ? b->cargo_mass : 0.0f);
    if (total_mass < 250.0f) total_mass = 250.0f;

    f32 sp = m_sin(b->pitch);
    f32 cp = m_cos(b->pitch);
    f32 sr = m_sin(b->roll);
    f32 cr = m_cos(b->roll);

    f32 buoy_fy     = 0.0f;
    f32 torque_p    = 0.0f;
    f32 torque_r    = 0.0f;
    f32 sub_sum     = 0.0f;
    f32 h_bow       = 0.0f;
    f32 h_stern     = 0.0f;
    f32 slope_world = 0.0f;

    /* Maksimum kaldırma kapasitesi: tam batmada ağırlığın 2.6 katı */
    f32 max_buoy_force = total_mass * 9.81f * 2.6f;

    /* Uzun periyotlu taşıyıcı okyanus soluyanı (swell): L = 48 m, c = 8.66 m/s */
    f32 t_sec     = (f32)st->tick * GELGIT_FIXED_DT;
    f32 swell_amp = (1.10f * w->storm_intensity) * m_clamp((w->cur_wind_speed - 6.0f) * 0.11f, 0.0f, 1.4f);
    f32 k_swell   = 0.1309f; /* 2*PI / 48m */
    f32 om_swell  = 1.133f;  /* sqrt(g * k_swell) */

    for (u32 i = 0; i < GELGIT_HULL_PROBES; i++) {
        const HullProbeDef* pr = &k_hull_probes[i];
        /* Yerel prob noktasını dünya koordinatına taşı */
        f32 wx = b->pos.x + right.x * pr->lx + fwd.x * pr->lz;
        f32 wz = b->pos.z + right.z * pr->lx + fwd.z * pr->lz;
        f32 wy = b->pos.y + pr->ly * cr * cp - pr->lz * sp + pr->lx * sr;

        f32 s_coord = wx * w->cur_wind_x + wz * w->cur_wind_z;
        f32 water_y = swell_amp * m_sin(k_swell * s_coord - om_swell * t_sec);
        if (ocean) {
            OceanSample os;
            ocean_sample(ocean, wx, wz, 0.0f, &os);
            water_y += os.disp.y;
            if (i == 0u) {
                /* Pruvadaki yüzey normalinden ileri yön eğimi katkısı */
                slope_world = -(os.normal.x * fwd.x + os.normal.z * fwd.z);
            }
        }
        if (i == 0u) h_bow   = water_y;
        if (i == 1u) h_stern = water_y;

        f32 depth = water_y - wy;
        if (depth > 0.0f) {
            f32 sub = m_clamp(depth / 0.72f, 0.0f, 1.4f);
            sub_sum += m_min(sub, 1.0f) * pr->volume_w;

            /* Prob düşey hızı */
            f32 v_vert = b->vel.y - b->vel_pitch * pr->lz + b->vel_roll * pr->lx;
            f32 f_probe = max_buoy_force * pr->volume_w * sub - (total_mass * 4.2f * pr->volume_w) * v_vert;
            if (f_probe < 0.0f) f_probe = 0.0f;

            buoy_fy  += f_probe;
            torque_p -= f_probe * pr->lz * 0.42f;
            torque_r += f_probe * pr->lx * 0.35f;
        }
    }

    b->submerged_ratio = m_clamp(sub_sum, 0.0f, 1.0f);
    /* Kıçtaki su yüksekliği pruvadan büyükse (h_stern > h_bow), tekne dalga yüzünden aşağı kayıyordur */
    f32 hull_slope = (h_stern - h_bow) * (1.0f / 7.85f);
    b->wave_slope_fwd = 0.70f * hull_slope + 0.30f * slope_world;

    /* 4. Gerçek rüzgâr (V_tw) ve görünür rüzgâr (V_aw = V_tw - V_boat) */
    Vec3 tw_vec = vec3(0.0f, 0.0f, 0.0f);
    gelgit_sample_wind(w, b->pos.x, b->pos.z, &tw_vec, &b->gust_factor);
    b->tw_x     = tw_vec.x;
    b->tw_z     = tw_vec.z;
    b->tw_speed = m_sqrt(tw_vec.x * tw_vec.x + tw_vec.z * tw_vec.z);

    /* Rüzgârın geldiği yön (upwind) ile pruva arasındaki açı (0° = rüzgâra karşı, 180° = pupa) */
    f32 up_x = -w->cur_wind_x;
    f32 up_z = -w->cur_wind_z;
    f32 cos_tw = m_clamp(fwd.x * up_x + fwd.z * up_z, -1.0f, 1.0f);
    f32 sin_tw = m_sqrt(m_max(0.0f, 1.0f - cos_tw * cos_tw));
    b->tw_angle_deg = m_atan2(sin_tw, cos_tw) * (180.0f / M_PI);

    /* Görünür rüzgârın geldiği yön vektörü: W_from = V_boat - V_tw */
    f32 from_wx = b->vel.x - tw_vec.x;
    f32 from_wz = b->vel.z - tw_vec.z;
    f32 aw_from_fwd  = from_wx * fwd.x   + from_wz * fwd.z;
    f32 aw_from_stbd = from_wx * right.x + from_wz * right.z;

    b->aw_x     = -aw_from_stbd;
    b->aw_z     = -aw_from_fwd;
    b->aw_speed = m_sqrt(aw_from_fwd * aw_from_fwd + aw_from_stbd * aw_from_stbd);
    b->aw_angle = m_atan2(aw_from_stbd, aw_from_fwd); /* 0 = pruvadan, +PI/2 = sancaktan */

    /* Tramola (tacking) algılama: orsa seyrinde rüzgâr kontrası değiştiğinde sayacı artır */
    f32 abs_beta = m_abs(b->aw_angle);
    if (abs_beta > 0.22f && b->tw_angle_deg < 110.0f && b->forward_speed > 0.35f) {
        i32 new_side = (b->aw_angle >= 0.0f) ? 1 : -1;
        if (b->tack_side != 0 && b->tack_side != new_side) {
            b->tack_count++;
        }
        b->tack_side = new_side;
    }

    /* Bumba rüzgâr altı (leeward) tarafa açılır */
    f32 target_boom = (b->aw_angle >= 0.0f) ? -b->sheet_angle : b->sheet_angle;
    b->boom_angle += (target_boom - b->boom_angle) * m_min(1.0f, 9.0f * dt);

    /* 5. Alabora (Capsize) durumunda kurtarma mekaniği */
    if (b->capsized) {
        b->cl             = 0.0f;
        b->cd             = 0.2f;
        b->luffing        = 1.0f;
        b->surf_intensity = 0.0f;

        b->vel.x *= (1.0f - 2.5f * dt);
        b->vel.z *= (1.0f - 2.5f * dt);
        f32 cap_sign = (b->roll >= 0.0f) ? 1.0f : -1.0f;
        b->roll     += (cap_sign * 1.42f - b->roll) * m_min(1.0f, 4.0f * dt);
        b->vel_roll  = 0.0f;

        b->pos.y += ((h_bow + h_stern) * 0.5f - 0.12f - b->pos.y) * m_min(1.0f, 5.0f * dt);
        b->pos.x += b->vel.x * dt;
        b->pos.z += b->vel.z * dt;

        b32 recovering = in && ((in->buttons & GELGIT_BTN_HIKE) || (in->buttons & GELGIT_BTN_INTERACT));
        if (recovering) {
            b->capsize_timer += dt;
            /* Kurtarma sırasında tekne yavaşça doğrulur */
            f32 p = m_clamp(b->capsize_timer / 3.0f, 0.0f, 1.0f);
            b->roll = cap_sign * 1.42f * (1.0f - p * 0.85f);
            if (b->capsize_timer >= 3.0f) {
                b->capsized      = 0;
                b->capsize_timer = 0.0f;
                b->roll          = 0.0f;
                b->vel_roll      = 0.0f;
                b->sheet_angle   = 1.15f; /* güvenli başlamak için ıskota laçka */
            }
        } else {
            b->capsize_timer = m_max(0.0f, b->capsize_timer - 0.5f * dt);
        }

        b->forward_speed = b->vel.x * fwd.x + b->vel.z * fwd.z;
        b->leeway_speed  = b->vel.x * right.x + b->vel.z * right.z;
        b->vmg_upwind    = b->vel.x * up_x + b->vel.z * up_z;
        return;
    }

    /* 6. Yelken Aerodinamiği (C_L / C_D polar tablosu + ölü bölge / luffing) */
    f32 min_beta_deg = (upg & GELGIT_UPG_SAIL_CUT) ? 25.0f : 28.0f;
    f32 min_tw_deg   = (upg & GELGIT_UPG_SAIL_CUT) ? 38.5f : 41.5f;
    f32 beta_deg     = abs_beta * (180.0f / M_PI);

    f32 alpha_rad = abs_beta - b->sheet_angle;
    if (alpha_rad < 0.0f) alpha_rad = 0.0f;
    b->aoa_deg = alpha_rad * (180.0f / M_PI);

    f32 cl = 0.0f, cd = 0.06f;
    gelgit_polar_eval(alpha_rad, upg, &cl, &cd);

    /* Ölü bölge (Irons) ve aşırı gevşek ıskota yapraklama (luffing) çarpanı */
    f32 luff_irons = m_clamp((min_beta_deg + 5.0f - beta_deg) * (1.0f / 8.0f), 0.0f, 1.0f);
    f32 luff_tw    = m_clamp((min_tw_deg + 3.0f - b->tw_angle_deg) * (1.0f / 6.0f), 0.0f, 1.0f);
    f32 luff_slack = m_clamp((4.5f - b->aoa_deg) * (1.0f / 4.5f), 0.0f, 1.0f);
    b->luffing     = m_max(m_max(luff_irons, luff_tw), luff_slack);

    f32 sail_eff = (1.0f - b->luffing) * m_max(0.25f, m_cos(b->roll));
    cl *= sail_eff;
    b->cl = cl;
    b->cd = cd;

    /* Dinamik basınç: q = 0.5 * rho_air * V_aw^2 * A_sail */
    f32 q_dyn   = 0.5f * 1.225f * b->aw_speed * b->aw_speed * tun->sail_area;
    f32 f_lift  = q_dyn * cl;
    f32 f_drag  = q_dyn * cd;

    f32 sin_b = m_sin(abs_beta);
    f32 cos_b = m_cos(abs_beta);

    /* İleri itki ve yanal (yatırma) kuvveti */
    f32 f_drive = f_lift * sin_b - f_drag * cos_b;
    f32 f_side  = f_lift * cos_b + f_drag * sin_b;
    f32 lee_dir = (b->aw_angle >= 0.0f) ? -1.0f : 1.0f; /* rüzgâr sancaktansa iskeleye (-right) iter */

    /* 7. Hidrodinamik (Salma direnci, gövde sürüklemesi ve dalga sörfü) */
    f32 v_fwd = b->vel.x * fwd.x   + b->vel.z * fwd.z;
    f32 v_lat = b->vel.x * right.x + b->vel.z * right.z;

    /* Dalga sırtında sörf (Wave Surfing): dalga eğimi pozitif ve hız eşiği aşılmışsa */
    f32 surf_slope_thresh = (upg & GELGIT_UPG_COPPER_HULL) ? 0.035f : 0.042f;
    f32 surf_speed_thresh = (upg & GELGIT_UPG_COPPER_HULL) ? 2.8f   : 3.2f;
    f32 target_surf = 0.0f;
    if (b->wave_slope_fwd > surf_slope_thresh && v_fwd > surf_speed_thresh) {
        f32 s_norm = m_clamp((b->wave_slope_fwd - surf_slope_thresh) * 9.5f, 0.0f, 1.0f);
        f32 v_norm = m_clamp((v_fwd - surf_speed_thresh) * 0.35f, 0.25f, 1.0f);
        target_surf = s_norm * v_norm;
    }
    b->surf_intensity += (target_surf - b->surf_intensity) * m_min(1.0f, 6.0f * dt);
    if (b->surf_intensity > 0.20f) {
        b->surf_time_total += dt;
    }

    /* Sörf kuvveti: dalga eğiminden yerçekimi ivmesi + azalan dalga sürüklemesi (planing) */
    f32 f_surf = 0.0f;
    if (b->wave_slope_fwd > 0.0f && v_fwd > 1.5f) {
        f_surf = total_mass * 9.81f * b->wave_slope_fwd * tun->surf_gain * (0.4f + 0.6f * b->surf_intensity);
    }

    f32 hull_drag_k = (upg & GELGIT_UPG_COPPER_HULL) ? 14.2f : 16.8f;
    /* Sörf (planing) esnasında su altı ıslak yüzey sürüklemesi %42'ye kadar düşer */
    hull_drag_k *= (1.0f - 0.42f * b->surf_intensity);

    f32 f_hull_drag = -hull_drag_k * v_fwd * m_abs(v_fwd) - 24.0f * v_fwd;

    /* Salma (keel) yanal direnci: kaymayı (leeway) bastırarak 43° orsa seyrini mümkün kılar */
    f32 keel_lat_k = (upg & GELGIT_UPG_DEEP_KEEL) ? 560.0f : 420.0f;
    f32 f_keel_lat = -keel_lat_k * v_lat * m_abs(v_lat) - 520.0f * v_lat;

    /* Net yatay ivme */
    f32 f_total_fwd = f_drive + f_surf + f_hull_drag;
    f32 f_total_lat = f_side * lee_dir + f_keel_lat;

    f32 ax = (fwd.x * f_total_fwd + right.x * f_total_lat) / total_mass;
    f32 az = (fwd.z * f_total_fwd + right.z * f_total_lat) / total_mass;
    f32 ay = (buoy_fy - total_mass * 9.81f) / total_mass;

    b->vel.x += ax * dt;
    b->vel.z += az * dt;
    b->vel.y += ay * dt;
    b->vel.y *= (1.0f - 1.8f * dt);

    b->pos.x += b->vel.x * dt;
    b->pos.z += b->vel.z * dt;
    b->pos.y += b->vel.y * dt;

    /* 8. Ada çarpışma kontrolü (yumuşak geri sekme) */
    for (u32 i = 0; i < GELGIT_MAX_ISLANDS; i++) {
        const GelgitIsland* isl = &st->islands[i];
        f32 dx = b->pos.x - isl->x;
        f32 dz = b->pos.z - isl->z;
        f32 d2 = dx * dx + dz * dz;
        f32 min_r = isl->radius + 3.5f;
        if (d2 < min_r * min_r && d2 > 0.001f) {
            f32 d = m_sqrt(d2);
            f32 nx = dx / d;
            f32 nz = dz / d;
            b->pos.x = isl->x + nx * min_r;
            b->pos.z = isl->z + nz * min_r;
            f32 vn = b->vel.x * nx + b->vel.z * nz;
            if (vn < 0.0f) {
                b->vel.x -= 1.4f * vn * nx;
                b->vel.z -= 1.4f * vn * nz;
                if (b->has_cargo) {
                    b->cargo_health = m_max(0.0f, b->cargo_health - 8.0f);
                }
            }
        }
    }

    /* 9. Yalpa (Roll), Baş-Kıç (Pitch) ve Pruva Dönüşü (Yaw) */
    f32 gm_eff = tun->keel_gm * ((upg & GELGIT_UPG_DEEP_KEEL) ? 1.22f : 1.0f);
    f32 m_heel  = f_side * lee_dir * 4.55f * m_max(0.2f, m_cos(b->roll));
    f32 m_right = -total_mass * 9.81f * gm_eff * m_sin(b->roll);
    f32 hike_sign = (b->roll >= 0.0f) ? -1.0f : 1.0f;
    f32 m_hike  = hike_sign * b->hike_amount * 1850.0f;

    f32 roll_inertia = total_mass * 2.15f;
    f32 roll_acc = (m_heel + m_right + m_hike + torque_r) / roll_inertia - b->vel_roll * 2.65f;
    b->vel_roll += roll_acc * dt;
    b->roll     += b->vel_roll * dt;

    f32 pitch_inertia = total_mass * 5.8f;
    f32 pitch_acc = (torque_p - total_mass * 9.81f * 2.2f * m_sin(b->pitch)) / pitch_inertia - b->vel_pitch * 3.4f;
    b->vel_pitch += pitch_acc * dt;
    b->pitch      = m_clamp(b->pitch + b->vel_pitch * dt, -0.55f, 0.55f);

    /* Dümen torku: ileri hızla artar + düşük hızda bile manevra kabiliyeti sağlar */
    f32 flow_spd = m_max(1.35f, m_abs(v_fwd) + 0.22f * b->aw_speed);
    f32 yaw_rate_target = b->rudder_angle * tun->rudder_gain * (0.62f + 0.16f * flow_spd);
    b->vel_yaw += (yaw_rate_target - b->vel_yaw) * m_min(1.0f, 6.5f * dt);
    b->yaw      = wrap_pi(b->yaw + b->vel_yaw * dt);

    /* Telemetri güncellemesi */
    b->forward_speed = b->vel.x * fwd.x   + b->vel.z * fwd.z;
    b->leeway_speed  = b->vel.x * right.x + b->vel.z * right.z;
    b->vmg_upwind    = b->vel.x * up_x    + b->vel.z * up_z;
    if (b->forward_speed > b->peak_speed) {
        b->peak_speed = b->forward_speed;
    }

    /* Pruva dalga çarpma / sprey şiddeti ve kargo hasarı */
    f32 impact = m_clamp((b->submerged_ratio - 0.45f) * 1.6f + m_abs(b->vel_pitch) * 0.45f, 0.0f, 1.0f);
    b->bow_impact = impact;
    if (b->has_cargo && (m_abs(b->roll) > 0.72f || impact > 0.78f)) {
        b->cargo_health = m_max(0.0f, b->cargo_health - 2.4f * dt);
    }

    /* Alabora eşiği kontrolü */
    f32 capsize_rad = tun->capsize_deg * (M_PI / 180.0f);
    if (m_abs(b->roll) >= capsize_rad) {
        b->capsized      = 1;
        b->capsize_timer = 0.0f;
        b->capsize_count++;
        if (b->has_cargo) {
            b->cargo_health = m_max(0.0f, b->cargo_health - 25.0f);
        }
    }
}

static void evaluate_mission(GelgitState* st, f32 dt) {
    if (st->mission_status != GELGIT_STATUS_PLAYING) return;
    st->mission_timer += dt;

    GelgitBoat* b = &st->boat;

    /* Görev 4 (id=3) özel aşama: önce alabora olup kurtarmayı deneyimle, sonra şamandıraya git */
    if (st->mission_id == 3u) {
        if (st->mission_stage == 0u && b->capsize_count > 0u && !b->capsized) {
            st->mission_stage = 1u;
        }
        if (st->mission_stage == 1u && st->waypoint_count > 0u) {
            const GelgitWaypoint* wp = &st->waypoints[0];
            f32 dx = b->pos.x - wp->x;
            f32 dz = b->pos.z - wp->z;
            if (dx * dx + dz * dz <= wp->radius * wp->radius) {
                st->mission_status  = GELGIT_STATUS_WON;
                st->completed_mask |= (1u << st->mission_id);
                st->upgrades_mask  |= GELGIT_UPG_DEEP_KEEL;
                st->gold           += 100u;
            }
        }
        return;
    }

    /* Görev 3 (id=2: Dalga Sırtı): ya şamandıraya ulaş ya da 2.0 sn sörf + 8.2 m/s tepe hıza ulaş */
    if (st->mission_id == 2u && b->surf_time_total >= 2.0f && b->peak_speed >= 8.2f) {
        st->mission_status  = GELGIT_STATUS_WON;
        st->completed_mask |= (1u << st->mission_id);
        st->gold           += 100u;
        return;
    }

    /* Kargo sağlığı sıfırlanırsa görev başarısız olur */
    if (b->has_cargo && b->cargo_health <= 0.0f) {
        st->mission_status = GELGIT_STATUS_FAILED;
        return;
    }

    if (st->waypoint_count == 0u) return;
    if (st->mission_stage < st->waypoint_count) {
        const GelgitWaypoint* wp = &st->waypoints[st->mission_stage];
        f32 dx = b->pos.x - wp->x;
        f32 dz = b->pos.z - wp->z;
        if (dx * dx + dz * dz <= wp->radius * wp->radius && !b->capsized) {
            st->mission_stage++;
            if (st->mission_stage >= st->waypoint_count) {
                st->mission_status = GELGIT_STATUS_WON;
                if (st->mission_id < GELGIT_MISSION_COUNT) {
                    st->completed_mask |= (1u << st->mission_id);
                    u32 reward = (st->mission_id >= 4u) ? 180u : 100u;
                    st->gold += reward;
                    /* İlk 2 görevde yelken kesimi, 4. görevde derin salma, 5. görevde bakır kaplama açılır */
                    if (st->mission_id == 1u) st->upgrades_mask |= GELGIT_UPG_SAIL_CUT;
                    if (st->mission_id == 3u) st->upgrades_mask |= GELGIT_UPG_DEEP_KEEL;
                    if (st->mission_id == 4u) st->upgrades_mask |= GELGIT_UPG_COPPER_HULL;
                } else if (st->mission_id == GELGIT_MODE_DAILY_SEA) {
                    if (st->best_daily_time <= 0.0f || st->mission_timer < st->best_daily_time) {
                        st->best_daily_time = st->mission_timer;
                    }
                }
            }
        }
    }
}

void gelgit_sim_step(void* state_ptr, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    GelgitState* st = (GelgitState*)state_ptr;
    if (!st) return;

    f32 step_dt = (dt > 0.0f) ? dt : GELGIT_FIXED_DT;
    st->tick++;

    step_weather(st, step_dt);

    Ocean* ocean = 0;
    if (user) {
        GelgitSimCtx* ctx = (GelgitSimCtx*)user;
        ocean = ctx->ocean;
        if (ocean && ctx->update_ocean_each_tick) {
            ocean_set_wind(ocean, st->weather.cur_wind_speed, st->weather.cur_wind_x, st->weather.cur_wind_z);
            ocean_update_sim(ocean, (f32)st->tick * GELGIT_FIXED_DT);
        }
    }

    const SimInput* in0 = (inputs && player_count >= 1u) ? &inputs[0] : 0;
    step_single_boat(&st->boat, st, in0, step_dt, ocean);

    if (st->mission_id == GELGIT_MODE_DAILY_SEA && player_count >= 2u && inputs) {
        st->ghost_active = 1;
        step_single_boat(&st->ghost, st, &inputs[1], step_dt, ocean);
    }

    evaluate_mission(st, step_dt);
}

static u64 hash_boat(u64 h, const GelgitBoat* b) {
    h = sim_hash_f32(h, b->pos.x);
    h = sim_hash_f32(h, b->pos.y);
    h = sim_hash_f32(h, b->pos.z);
    h = sim_hash_f32(h, b->vel.x);
    h = sim_hash_f32(h, b->vel.y);
    h = sim_hash_f32(h, b->vel.z);
    h = sim_hash_f32(h, b->pitch);
    h = sim_hash_f32(h, b->roll);
    h = sim_hash_f32(h, b->yaw);
    h = sim_hash_f32(h, b->vel_pitch);
    h = sim_hash_f32(h, b->vel_roll);
    h = sim_hash_f32(h, b->vel_yaw);
    h = sim_hash_f32(h, b->rudder_angle);
    h = sim_hash_f32(h, b->rudder_vel);
    h = sim_hash_f32(h, b->sheet_angle);
    h = sim_hash_f32(h, b->boom_angle);
    h = sim_hash_f32(h, b->hike_amount);
    h = sim_hash_f32(h, b->forward_speed);
    h = sim_hash_f32(h, b->leeway_speed);
    h = sim_hash_f32(h, b->vmg_upwind);
    h = sim_hash_f32(h, b->surf_intensity);
    h = sim_hash_f32(h, b->surf_time_total);
    h = sim_hash_f32(h, b->peak_speed);
    h = sim_hash_u32(h, (u32)b->capsized);
    h = sim_hash_f32(h, b->capsize_timer);
    h = sim_hash_u32(h, b->capsize_count);
    h = sim_hash_u32(h, (u32)b->tack_side);
    h = sim_hash_u32(h, b->tack_count);
    h = sim_hash_u32(h, (u32)b->has_cargo);
    h = sim_hash_f32(h, b->cargo_health);
    for (u32 i = 0; i < GELGIT_RUDDER_BUF_TICKS; i++) {
        h = sim_hash_u32(h, (u32)(u16)b->rudder_buf[i]);
    }
    h = sim_hash_u32(h, b->rudder_buf_pos);
    return h;
}

u64 gelgit_state_hash(const void* state_ptr, u32 state_size, void* user) {
    (void)state_size;
    (void)user;
    const GelgitState* st = (const GelgitState*)state_ptr;
    u64 h = sim_hash_init();
    h = sim_hash_u32(h, st->tick);
    h = sim_hash_u32(h, st->seed);
    h = sim_hash_u32(h, st->mission_id);
    h = sim_hash_u32(h, st->mission_stage);
    h = sim_hash_u32(h, st->mission_status);
    h = sim_hash_u32(h, st->completed_mask);
    h = sim_hash_u32(h, st->upgrades_mask);
    h = sim_hash_u32(h, st->gold);
    h = sim_hash_f32(h, st->mission_timer);

    h = sim_hash_f32(h, st->weather.cur_wind_speed);
    h = sim_hash_f32(h, st->weather.cur_wind_x);
    h = sim_hash_f32(h, st->weather.cur_wind_z);
    h = sim_hash_f32(h, st->weather.storm_intensity);
    h = sim_hash_f32(h, st->weather.front_line_dist);
    for (u32 i = 0; i < GELGIT_MAX_GUSTS; i++) {
        h = sim_hash_f32(h, st->weather.gusts[i].x);
        h = sim_hash_f32(h, st->weather.gusts[i].z);
    }

    h = hash_boat(h, &st->boat);
    if (st->ghost_active) {
        h = hash_boat(h, &st->ghost);
    }
    return sim_hash_final(h);
}

SimInput gelgit_build_input(const InputState* in, u32 fb_w, u32 fb_h) {
    SimInput out;
    mem_zero(&out, sizeof(out));
    if (!in) return out;

    i32 mx = 0;
    i32 my = 0;

    if (in->keys['A'] || in->keys[37]) mx -= 32767;
    if (in->keys['D'] || in->keys[39]) mx += 32767;
    if (in->keys['W'] || in->keys[38]) my += 32767;
    if (in->keys['S'] || in->keys[40]) my -= 32767;

    if (mx < 0) out.buttons |= GELGIT_BTN_RUDDER_LEFT;
    if (mx > 0) out.buttons |= GELGIT_BTN_RUDDER_RIGHT;
    if (my > 0) out.buttons |= GELGIT_BTN_SHEET_IN;
    if (my < 0) out.buttons |= GELGIT_BTN_SHEET_OUT;

    if (in->keys[16])  out.buttons |= GELGIT_BTN_HIKE;
    if (in->keys[' ']) out.buttons |= GELGIT_BTN_INTERACT;

    /* Dokunmatik / fare sol tuş basılıyken ekranın sol yarısı dümen, sağ yarısı ıskota kontrolü sağlar */
    if ((in->mouse_buttons & 1u) && fb_w > 0u && fb_h > 0u) {
        f32 nx = (f32)in->mouse_x / (f32)fb_w;
        f32 ny = (f32)in->mouse_y / (f32)fb_h;
        if (ny > 0.25f) {
            if (nx < 0.45f) {
                f32 steer = m_clamp((nx - 0.22f) / 0.18f, -1.0f, 1.0f);
                mx = (i32)(steer * 32767.0f);
            } else if (nx > 0.55f) {
                f32 sheet = m_clamp((0.62f - ny) / 0.28f, -1.0f, 1.0f);
                my = (i32)(sheet * 32767.0f);
            }
        }
    }

    out.move_x = (i16)clamp_i32(mx, -32767, 32767);
    out.move_y = (i16)clamp_i32(my, -32767, 32767);
    return out;
}

const char* gelgit_mission_title(u32 mission_id) {
    switch (mission_id) {
        case 0u: return "GOREV 1: RUZGARI YAKALA (APAZ & ISKOTA)";
        case 1u: return "GOREV 2: ZIKZAK (ORSA & TRAMOLA)";
        case 2u: return "GOREV 3: DALGA SIRTI (SORF)";
        case 3u: return "GOREV 4: ALABORA VE KURTARMA";
        case 4u: return "GOREV 5: FENER ADASINA KARGO TESLIMATI";
        case 5u: return "GOREV 6: FIRTINA CEPHESINDE ACIL ILAC";
        case 6u: return "GUNUN DENIZI (SEEDED YARIS + HAYALET TEKNE)";
        default: return "SERBEST SEYIR (ACIK TAKIMADA)";
    }
}

const char* gelgit_mission_objective(const GelgitState* st) {
    if (!st) return "";
    if (st->mission_status == GELGIT_STATUS_WON) {
        return "GOREV TAMAMLANDI! (N: SONRAKI GOREV / R: TEKRAR)";
    }
    if (st->mission_status == GELGIT_STATUS_FAILED) {
        return "KARGO HASAR ALDI! (R: GOREVI YENIDEN BASLAT)";
    }
    if (st->boat.capsized) {
        return "ALABORA OLDUN! SHIFT VEYA SPACE BASILI TUTARAK TEKNEYI DOGRULT (3 SN)";
    }
    switch (st->mission_id) {
        case 0u: return "W BASILI TUTARAK ISKOTAYI CEK (YELKENI DOLDUR) VE 2 SAMANDIRADAN GEC";
        case 1u: return "RUZGARA ~44 DERECE ORSA ACISIYLA ZIKZAK CIZIP KUZEYDEKI SAMANDIRALARA ULAS";
        case 2u: return "DALGA YUZUNDEN ASAGI KAYARKEN SORF YAP (+40% HIZ) VE SAMANDIRAYA VAR";
        case 3u:
            return (st->mission_stage == 0u)
                ? "BORDA RUZGARINDA ALABORA OL, SONRA SHIFT BASILI TUTARAK 3 SN'DE KURTAR"
                : "KURTARMA BASARILI! SIMDI DOGUDAKI SAMANDIRAYA ULAS";
        case 4u: return "LIMANDAN ALINAN KARGOYU DOGU RESIFI ISKELESINE SAGLAM TESLIM ET";
        case 5u: return "FIRTINA CEPHESINI YARARAK KUZEY FENERINE ACIL ILAC KARGOSUNU ULASTIR";
        case 6u: return "3 KAPILI PARKURU EN KISA SUREDE BITIR (HAYALET TEKNEYLE YARIS)";
        default: return "SERBEST SEYIR — 1..8 ILE GOREV SEC, TAB ILE FIZIK AYARLARINI AC";
    }
}

GelgitPhysicsMetrics gelgit_measure_physics(Ocean* ocean) {
    GelgitPhysicsMetrics m;
    mem_zero(&m, sizeof(m));

    /* 1. Rüzgâra karşı (orsa) açısı taraması: 35°..58° arasında net VMG > 0.35 m/s veren en dar açıyı bul */
    m.upwind_angle_deg = 60.0f;
    for (u32 deg = 35u; deg <= 58u; deg++) {
        GelgitState st;
        gelgit_state_init(&st, 0x1234u, GELGIT_MODE_FREE_SAIL, 0u, 0u);
        /* Kedi pençesi hamlelerini ve fırtınayı kapatarak saf aerodinamik/hidrodinamik dengeyi ölç */
        for (u32 g = 0; g < GELGIT_MAX_GUSTS; g++) st.weather.gusts[g].boost_mps = 0.0f;
        st.weather.base_wind_speed = 11.0f;
        st.weather.cur_wind_speed  = 11.0f;
        st.weather.storm_intensity = 0.0f;
        st.weather.target_storm    = 0.0f;

        f32 target_yaw = (f32)deg * (M_PI / 180.0f);
        init_boat_at(&st.boat, 0.0f, 0.0f, target_yaw, 0.24f);

        for (u32 t = 0; t < 360u; t++) {
            SimInput in = {0};
            f32 yaw_err = wrap_pi(target_yaw - st.boat.yaw);
            in.move_x = (i16)clamp_i32((i32)(yaw_err * 90000.0f), -32767, 32767);
            in.buttons = GELGIT_BTN_HIKE;
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
        }

        if (st.boat.vmg_upwind > 0.38f && st.boat.forward_speed > 2.2f) {
            m.upwind_angle_deg = (f32)deg;
            m.upwind_speed_mps = st.boat.forward_speed;
            break;
        }
    }

    /* 2. Düz su geniş apaz hızı vs dalga sırtında sörf hızı */
    {
        GelgitState st;
        gelgit_state_init(&st, 0x5678u, GELGIT_MODE_FREE_SAIL, 0u, 0u);
        for (u32 g = 0; g < GELGIT_MAX_GUSTS; g++) st.weather.gusts[g].boost_mps = 0.0f;
        st.weather.base_wind_speed = 12.5f;
        st.weather.cur_wind_speed  = 12.5f;
        st.weather.storm_intensity = 0.0f;
        st.weather.target_storm    = 0.0f;

        f32 reach_yaw = 140.0f * (M_PI / 180.0f);
        init_boat_at(&st.boat, 0.0f, 0.0f, reach_yaw, 0.92f);
        for (u32 t = 0; t < 360u; t++) {
            SimInput in = {0};
            f32 yaw_err = wrap_pi(reach_yaw - st.boat.yaw);
            in.move_x = (i16)clamp_i32((i32)(yaw_err * 90000.0f), -32767, 32767);
            in.buttons = GELGIT_BTN_HIKE;
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
        }
        m.reach_speed_mps = st.boat.forward_speed;

        /* Şimdi aynı temel rüzgârda dalgalı deniz üzerinde dalga sırtı sörfünü ölç */
        st.weather.storm_intensity = 0.82f;
        st.weather.target_storm    = 0.82f;
        if (ocean) {
            ocean_set_wind(ocean, 15.0f, 0.0f, -1.0f);
            ocean_update_sim(ocean, 12.5f);
        }
        GelgitSimCtx ctx = { ocean, 0 };
        f32 peak_surf = m.reach_speed_mps;
        for (u32 phase = 0; phase < 12u; phase++) {
            st.tick = 0u;
            init_boat_at(&st.boat, 0.0f, (f32)phase * 4.0f, reach_yaw, 0.92f);
            st.boat.vel = vec3(m_sin(reach_yaw) * m.reach_speed_mps, 0.0f, m_cos(reach_yaw) * m.reach_speed_mps);
            for (u32 t = 0; t < 120u; t++) {
                SimInput in = {0};
                f32 yaw_err = wrap_pi(reach_yaw - st.boat.yaw);
                in.move_x = (i16)clamp_i32((i32)(yaw_err * 90000.0f), -32767, 32767);
                in.buttons = GELGIT_BTN_HIKE;
                gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, &ctx);
                if (st.boat.forward_speed > peak_surf) {
                    peak_surf = st.boat.forward_speed;
                }
            }
        }
        m.surf_speed_mps = peak_surf;
        if (m.reach_speed_mps > 0.1f) {
            m.surf_boost_pct = ((m.surf_speed_mps - m.reach_speed_mps) / m.reach_speed_mps) * 100.0f;
        }
    }

    /* 3. Alabora eşiği (°) ve 3 sn kurtarma süresi doğrulaması */
    {
        GelgitState st;
        gelgit_state_init(&st, 0x9ABCu, 3u, 0u, 0u);
        st.weather.base_wind_speed = 22.0f;
        st.weather.cur_wind_speed  = 22.0f;
        init_boat_at(&st.boat, 0.0f, 0.0f, M_PI * 0.5f, 0.22f);
        st.boat.roll = 0.85f;

        for (u32 t = 0; t < 180u; t++) {
            SimInput in = {0};
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
            if (st.boat.capsized) {
                m.capsize_threshold_deg = m_abs(st.boat.roll) * (180.0f / M_PI);
                break;
            }
        }
        u32 rec_ticks = 0u;
        while (st.boat.capsized && rec_ticks < 300u) {
            SimInput in = {0};
            in.buttons = GELGIT_BTN_HIKE;
            gelgit_sim_step(&st, &in, 1u, GELGIT_FIXED_DT, 0);
            rec_ticks++;
        }
        m.recovery_time_sec = (f32)rec_ticks * GELGIT_FIXED_DT;
    }

    return m;
}
