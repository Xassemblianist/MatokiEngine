/* game/gelgit.h — "Gelgit" (OYUN1) deterministik denizcilik, yelken fiziği, fırtına ve görev çekirdeği.
 * Sim-kritik durum (GelgitState) işaretçisiz düz bellek yapısında tutulur (D-110, D-112);
 * 60 Hz sabit tamsayı tick ile ilerler, 1T ve 8T'de bit-bit aynı FNV-1a özetini (hash) verir.
 * Görünür rüzgâr polar tablosu (C_L/C_D), salma/dümen hidrodinamiği, 10 noktalı CPU FFT kaldırma
 * kuvveti, dalga sörfü, ağırlık atma (hiking), alabora kurtarma, 6 görev ve Günün Denizi'ni içerir.
 */
#ifndef GAME_GELGIT_H
#define GAME_GELGIT_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "plat/input.h"
#include "sim/sim.h"
#include "gfx3d/ocean.h"

#define GELGIT_TICK_HZ          60u
#define GELGIT_FIXED_DT         (1.0f / 60.0f)
#define GELGIT_HULL_PROBES      10u
#define GELGIT_POLAR_NODES      10u
#define GELGIT_MAX_GUSTS        8u
#define GELGIT_MAX_ISLANDS      4u
#define GELGIT_MAX_LIGHTHOUSES  2u
#define GELGIT_MAX_WAYPOINTS    4u
#define GELGIT_MISSION_COUNT    6u
#define GELGIT_MODE_DAILY_SEA   6u
#define GELGIT_MODE_FREE_SAIL   7u
#define GELGIT_TOTAL_MODES      8u
#define GELGIT_RUDDER_BUF_TICKS 6u   /* 100 ms @ 60 Hz girdi tamponu */

/* SimInput.buttons bit eşlemeleri */
#define GELGIT_BTN_RUDDER_LEFT  SIM_BTN_LEFT
#define GELGIT_BTN_RUDDER_RIGHT SIM_BTN_RIGHT
#define GELGIT_BTN_SHEET_IN     SIM_BTN_UP      /* W: ıskotayı kas (yelkeni çek) */
#define GELGIT_BTN_SHEET_OUT    SIM_BTN_DOWN    /* S: ıskotayı laçka et (yelkeni sal) */
#define GELGIT_BTN_HIKE         SIM_BTN_SPRINT  /* Shift: ağırlık at (hiking) / kurtarma */
#define GELGIT_BTN_INTERACT     SIM_BTN_JUMP    /* Space: yük al-bırak / kurtarma */

/* Tekne yükseltme bayrakları */
#define GELGIT_UPG_SAIL_CUT     (1u << 0)       /* +15% C_L, 3° daha keskin orsa */
#define GELGIT_UPG_DEEP_KEEL    (1u << 1)       /* +22% doğrultma momenti (GM), -25% kayma */
#define GELGIT_UPG_COPPER_HULL  (1u << 2)       /* -15% gövde sürüklemesi, erken sörf */

typedef enum {
    GELGIT_STATUS_PLAYING = 0,
    GELGIT_STATUS_WON     = 1,
    GELGIT_STATUS_FAILED  = 2
} GelgitMissionStatus;

/* Debug HUD'dan canlı değiştirilebilir fizik sabitleri */
typedef struct {
    f32 sail_area;      /* m^2 (varsayılan 22.0) */
    f32 boat_mass;      /* kg  (varsayılan 680.0) */
    f32 keel_gm;        /* m   (varsayılan 1.25) */
    f32 surf_gain;      /* çarpan (varsayılan 2.45) */
    f32 capsize_deg;    /* derece (varsayılan 58.0) */
    f32 rudder_gain;    /* çarpan (varsayılan 1.00) */
} GelgitTuning;

/* Su yüzeyinde hareket eden "kedi pençesi" (gust) rüzgâr hamlesi hücresi */
typedef struct {
    f32 x, z;           /* dünya merkezi (m) */
    f32 vx, vz;         /* ilerleme hızı (m/s) */
    f32 radius;         /* etki yarıçapı (m) */
    f32 boost_mps;      /* ek rüzgâr hızı (m/s) */
} GelgitGust;

/* Hava, güneş ve yaklaşan fırtına cephesi durumu */
typedef struct {
    f32 base_wind_speed;   /* temel rüzgâr hızı (m/s) */
    f32 base_wind_dir;     /* rüzgârın estiği yön açısı (rad, 0=+Z, PI/2=+X) */
    f32 cur_wind_speed;    /* cephe etkisi dahil etkin genel rüzgâr hızı (m/s) */
    f32 cur_wind_x;        /* birim rüzgâr yönü X */
    f32 cur_wind_z;        /* birim rüzgâr yönü Z */
    f32 storm_intensity;   /* 0.0 (altın gün batımı) .. 1.0 (tam fırtına, Hs > 6m) */
    f32 target_storm;      /* görev hedef fırtına şiddeti */
    f32 front_line_dist;   /* fırtına cephesi hattının merkeze uzaklığı (m) */
    f32 front_speed;       /* cephe yaklaşma hızı (m/s) */
    f32 sun_elev;          /* güneş yüksekliği (rad) */
    f32 sun_azim;          /* güneş azimutu (rad) */
    f32 lightning_flash;   /* 0..1 şimşek çakma parlaklığı */
    u32 thunder_tick;      /* son gök gürültüsü tick damgası */
    GelgitGust gusts[GELGIT_MAX_GUSTS];
} GelgitWeather;

/* Tekne simülasyon durumu (ana oyuncu ve hayalet tekne için ortak) */
typedef struct {
    Vec3 pos;              /* dünya konumu (m) */
    Vec3 vel;              /* dünya hızı (m/s) */
    f32  pitch;            /* baş-kıç açısı (rad) */
    f32  roll;             /* yalpa/yatma açısı (rad) */
    f32  yaw;              /* pruva yönü (rad, 0=+Z, +PI/2=+X) */
    f32  vel_pitch;
    f32  vel_roll;
    f32  vel_yaw;

    f32  rudder_angle;     /* dümen açısı (rad, -0.55 .. +0.55) */
    f32  rudder_vel;       /* dümen açısal hızı (rad/s) */
    f32  sheet_angle;      /* ıskota açısı (rad, 0.21 .. 1.40 -> 12°..80°) */
    f32  boom_angle;       /* rüzgâr tarafına göre işaretli bumba açısı (rad) */
    f32  hike_amount;      /* 0..1 mürettebat ağırlık atma (Shift) */

    /* Aerodinamik ve hidrodinamik telemetri */
    f32  tw_x, tw_z;       /* tekne konumundaki gerçek rüzgâr vektörü (m/s) */
    f32  tw_speed;         /* yerel gerçek rüzgâr hızı (m/s) */
    f32  gust_factor;      /* 0..1 yerel kedi pençesi hamle şiddeti */
    f32  aw_x, aw_z;       /* tekne yerel uzayında görünür rüzgâr (m/s) */
    f32  aw_speed;         /* görünür rüzgâr hızı (m/s) */
    f32  aw_angle;         /* görünür rüzgâr açısı (rad, -PI..PI, 0=pruva) */
    f32  tw_angle_deg;     /* gerçek rüzgâra göre seyir açısı (0°=rüzgâra karşı, 180°=pupa) */
    f32  aoa_deg;          /* yelken hücum açısı alpha (derece) */
    f32  cl, cd;           /* anlık kaldırma ve sürükleme katsayıları */
    f32  luffing;          /* 0..1 yelken boşa düşme / yapraklama oranı */

    f32  forward_speed;    /* ileri hız (m/s) */
    f32  leeway_speed;     /* yanal kayma hızı (m/s) */
    f32  vmg_upwind;       /* rüzgâr üstüne net ilerleme hızı (m/s) */
    f32  wave_slope_fwd;   /* gövde altındaki boyuna dalga eğimi */
    f32  surf_intensity;   /* 0..1 dalga sörfü şiddeti */
    f32  surf_time_total;  /* toplam sörf süresi (s) */
    f32  peak_speed;       /* ulaşılan en yüksek hız (m/s) */
    f32  bow_impact;       /* 0..1 dalgaya çarpma / pruva sprey şiddeti */
    f32  submerged_ratio;  /* suya batmış hacim oranı */

    /* Alabora ve tramola (tacking) durumu */
    b32  capsized;
    f32  capsize_timer;    /* 0..3.0 s kurtarma sayacı */
    u32  capsize_count;
    i32  tack_side;        /* -1: iskele kontra, +1: sancak kontra, 0: nötr */
    u32  tack_count;       /* tamamlanan tramola sayısı */

    /* Yük (kargo) durumu */
    b32  has_cargo;
    f32  cargo_health;     /* 0..100 (%) */
    f32  cargo_mass;       /* kg */

    /* 100 ms (6 tick) dümen girdi tamponu */
    i16  rudder_buf[GELGIT_RUDDER_BUF_TICKS];
    u32  rudder_buf_pos;
    u16  _pad;
} GelgitBoat;

/* Ada, deniz feneri ve görev şamandırası tanımları */
typedef struct {
    f32 x, z;
    f32 radius;
    f32 height;
} GelgitIsland;

typedef struct {
    f32 x, z;
    f32 base_y;
    f32 tower_h;
    f32 beam_phase;
} GelgitLighthouse;

typedef struct {
    f32 x, z;
    f32 radius;
    u32 kind;              /* 0: şamandıra kapısı, 1: yük alma iskelesi, 2: teslimat iskelesi */
} GelgitWaypoint;

/* Tamamen işaretçisiz, snapshot/replay/hash uyumlu simülasyon durumu (D-110, D-112) */
typedef struct {
    u32              tick;
    u32              seed;
    u32              mission_id;
    u32              mission_stage;
    u32              mission_status;    /* GelgitMissionStatus */
    u32              waypoint_count;
    u32              completed_mask;
    u32              upgrades_mask;
    u32              gold;
    f32              mission_timer;
    f32              best_daily_time;
    b32              ghost_active;

    GelgitTuning     tuning;
    GelgitWeather    weather;
    GelgitBoat       boat;
    GelgitBoat       ghost;

    GelgitIsland     islands[GELGIT_MAX_ISLANDS];
    GelgitLighthouse lighthouses[GELGIT_MAX_LIGHTHOUSES];
    GelgitWaypoint   waypoints[GELGIT_MAX_WAYPOINTS];
} GelgitState;

/* Durum yapısının platformdan bağımsız ve makul boyutta olduğunun derleme zamanı kanıtı */
_Static_assert(sizeof(GelgitState) < 4096, "GelgitState boyutu 4 KB altinda olmali");

/* SimStepFn user bağlamı: CPU FFT okyanus örneklemesi için */
typedef struct {
    Ocean* ocean;
    b32    update_ocean_each_tick;
} GelgitSimCtx;

/* Fizik doğrulama ve kabul kriteri ölçüm sonuçları */
typedef struct {
    f32 upwind_angle_deg;       /* Rüzgâra karşı VMG > 0 olan minimum orsa açısı (°) */
    f32 upwind_speed_mps;       /* Orsa seyrindeki denge hızı (m/s) */
    f32 reach_speed_mps;        /* Düz/apaz seyrindeki temel hız (m/s) */
    f32 surf_speed_mps;         /* Dalga sırtında sörf tepe hızı (m/s) */
    f32 surf_boost_pct;         /* Sörf hız artışı yüzdesi (%) */
    f32 capsize_threshold_deg;  /* Alabora gerçekleşme açısı (°) */
    f32 recovery_time_sec;      /* Alaboradan kurtarma süresi (s) */
} GelgitPhysicsMetrics;

/* Varsayılan ayar sabitlerini döner */
GelgitTuning gelgit_default_tuning(void);

/* Hücum açısı alpha (radyan, 0..PI) için C_L ve C_D polar tablosunu örnekler */
void gelgit_polar_eval(f32 alpha_rad, u32 upgrades_mask, f32* out_cl, f32* out_cd);

/* Verilen noktadaki (x, z) gerçek rüzgâr vektörünü ve hamle (gust) şiddetini hesaplar */
void gelgit_sample_wind(const GelgitWeather* w, f32 x, f32 z, Vec3* out_wind, f32* out_gust_factor);

/* Takımada coğrafyasını, hava durumunu ve seçilen görevi (0..7) sıfırdan kurar */
void gelgit_state_init(GelgitState* st, u32 seed, u32 mission_id, u32 upgrades_mask, u32 completed_mask);

/* Mevcut görevi veya modu (0..7) korunan yükseltmelerle yeniden başlatır */
void gelgit_start_mission(GelgitState* st, u32 mission_id);

/* SimStepFn uyumlu deterministik 60 Hz simülasyon adımı (inputs[0]=oyuncu, inputs[1]=hayalet) */
void gelgit_sim_step(void* state_ptr, const SimInput* inputs, u32 player_count, f32 dt, void* user);

/* Kanonik float (-0.0f -> +0.0f, NaN -> qNaN) kurallarıyla GelgitState'in 64-bit FNV-1a özeti */
u64  gelgit_state_hash(const void* state_ptr, u32 state_size, void* user);

/* Klavye / fare / dokunmatik durumundan deterministik SimInput üretir */
SimInput gelgit_build_input(const InputState* in, u32 fb_w, u32 fb_h);

/* Görev başlığı ve kısa hedef açıklamasını döner */
const char* gelgit_mission_title(u32 mission_id);
const char* gelgit_mission_objective(const GelgitState* st);

/* Orsa açısı (°), sörf hız artışı (%) ve alabora eşiğini (°) gerçek simülasyonla ölçer */
GelgitPhysicsMetrics gelgit_measure_physics(Ocean* ocean);

#endif /* GAME_GELGIT_H */
