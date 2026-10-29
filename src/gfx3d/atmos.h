/* gfx3d/atmos.h — Hillaire 2020 fiziksel atmosfer modeli, hacimsel bulut gürültüsü ve gün döngüsü.
 * Rayleigh + Mie + Ozon saçılım/soğurum katsayıları, Transmittance / Multi-Scattering / Sky-View /
 * Aerial Perspective LUT referans değerlendiricileri, Beer-Powder zayıflaması ve 3B Perlin-Worley
 * bulut gürültüsü üreticisini içerir. GPU compute çekirdekleri ile birebir aynı fiziği paylaşır.
 */
#ifndef GFX3D_ATMOS_H
#define GFX3D_ATMOS_H

#include "core/types.h"
#include "core/math.h"

#define ATMOS_TRANS_W          256u
#define ATMOS_TRANS_H          64u
#define ATMOS_MS_W             32u
#define ATMOS_MS_H             32u
#define ATMOS_SKY_W            192u
#define ATMOS_SKY_H            108u
#define ATMOS_AERIAL_W         32u
#define ATMOS_AERIAL_H         32u
#define ATMOS_AERIAL_D         16u
#define CLOUD_NOISE_N          32u

#define ATMOS_TRANSMITTANCE_W  ATMOS_TRANS_W
#define ATMOS_TRANSMITTANCE_H  ATMOS_TRANS_H
#define ATMOS_MULTISCATTER_W   ATMOS_MS_W
#define ATMOS_MULTISCATTER_H   ATMOS_MS_H
#define ATMOS_SKYVIEW_W        ATMOS_SKY_W
#define ATMOS_SKYVIEW_H        ATMOS_SKY_H
#define CLOUD_NOISE_3D_N       CLOUD_NOISE_N

/* Gün döngüsü zaman önayarları (radyan cinsinden güneş yüksekliği) */
#define ATMOS_ELEV_NOON    0.92f   /* ~53° öğle güneşi */
#define ATMOS_ELEV_SUNSET  0.035f  /* ~2.0° gün batımı / altın saat */
#define ATMOS_ELEV_NIGHT  -0.45f   /* ~-26° gece (ay ve yıldızlar aktif) */

typedef struct {
    /* Gezegen ve atmosfer geometrisi (km) */
    f32  r_bottom;          /* 6360.0 km */
    f32  r_top;             /* 6460.0 km */
    /* Rayleigh saçılımı (1/km) ve ölçek yüksekliği (km) */
    Vec3 rayleigh_scat;     /* (0.005802, 0.013558, 0.033100) */
    f32  rayleigh_h;        /* 8.0 km */
    /* Mie saçılımı / sönümlenmesi (1/km), ölçek yüksekliği (km) ve asimetri g */
    Vec3 mie_scat;          /* (0.003996, 0.003996, 0.003996) */
    Vec3 mie_ext;           /* (0.004440, 0.004440, 0.004440) */
    f32  mie_h;             /* 1.2 km */
    f32  mie_g;             /* 0.80 */
    /* Ozon (Chappuis bandı) soğurumu (1/km) ve çadır profili (km) */
    Vec3 ozone_abs;         /* (0.000650, 0.001881, 0.000085) */
    f32  ozone_center_h;    /* 25.0 km */
    f32  ozone_half_w;      /* 15.0 km */
    /* Yeryüzü / okyanus ortalama albedosu */
    Vec3 ground_albedo;     /* (0.04, 0.07, 0.12) */

    /* Gün döngüsü ve hacimsel bulut durumu */
    f32  sun_elev;          /* güneş yüksekliği (radyan, [-1.2, +1.4]) */
    f32  sun_azim;          /* güneş azimutu (radyan) */
    Vec3 sun_dir;           /* birim güneş yönü */
    Vec3 moon_dir;          /* birim ay yönü */
    f32  cloud_coverage;    /* [0, 1] bulut örtüsü oranı */
    f32  cloud_bottom_km;   /* 1.50 km */
    f32  cloud_top_km;      /* 4.20 km */
    f32  cloud_wind_x;      /* bulut sürüklenme hızı X (m/s) */
    f32  cloud_wind_z;      /* bulut sürüklenme hızı Z (m/s) */
    f32  day_cycle_speed;   /* otomatik gün döngüsü hızı (rad/s, 0 = duraklatılmış) */

    /* Türetilmiş aydınlatma ve CPU yedek renkleri (atmos_update_lighting ile güncellenir) */
    Vec3 sky_zenith;
    Vec3 sky_horizon;
    Vec3 sun_color;
    Vec3 fog_color;
    f32  star_intensity;    /* [0, 1] gece yıldız parlaklığı */
    f32  moon_intensity;    /* [0, 1] gece ay ışığı katkısı */
} AtmosParams;

/* Hillaire 2020 Dünya atmosferi varsayılan parametrelerini döndürür */
AtmosParams atmos_default_params(void);

/* Güneş yükseklik ve azimut açılarını ayarlar, sun_dir/moon_dir ve aydınlatma renklerini günceller */
void atmos_set_sun_angles(AtmosParams* p, f32 sun_elev, f32 sun_azim);

/* Merkezden r yarıçapındaki noktadan mu = cos(zenit) yönünde r_sphere küresine pozitif kesişim mesafesi (yoksa -1) */
f32  atmos_ray_sphere_intersect(f32 r, f32 mu, f32 r_sphere);

/* Işının atmosfer dışına çıkmadan önce yeryüzüne (r_bottom) çarpıp çarpmadığını döndürür */
b32  atmos_hits_ground(f32 r, f32 mu, f32 r_bottom);

/* h_km irtifasında Rayleigh saçılımı, Mie saçılımı ve toplam sönümlenme (extinction) katsayılarını hesaplar */
void atmos_medium_sample(const AtmosParams* p, f32 h_km,
                         Vec3* out_scat_r, Vec3* out_scat_m, Vec3* out_ext);

/* Transmittance LUT değerlendiricisi: h_km irtifasından mu = cos(zenit) yönünde atmosfer üstüne geçirgenlik T in [0,1]^3 */
Vec3 atmos_eval_transmittance(const AtmosParams* p, f32 h_km, f32 mu);

/* Multi-Scattering LUT değerlendiricisi: h_km irtifası ve mu_s = cos(güneş zenit) için çoklu saçılım Ψ_ms */
Vec3 atmos_eval_multiscatter(const AtmosParams* p, f32 h_km, f32 mu_s);

/* Sky-View değerlendiricisi: verilen bakış yönü (ray_dir) ve güneş yönü (sun_dir) için fiziksel gökyüzü radyansı */
Vec3 atmos_eval_sky_ray(const AtmosParams* p, f32 eye_h_km, Vec3 ray_dir, Vec3 sun_dir);

/* Aerial Perspective değerlendiricisi: max_dist_km mesafeye kadar saçılan ışık ve ortalama geçirgenlik */
void atmos_eval_aerial(const AtmosParams* p, f32 eye_h_km, Vec3 ray_dir, Vec3 sun_dir,
                       f32 max_dist_km, Vec3* out_inscatter, f32* out_trans);

/* Güneş/ay konumuna göre CPU yedek renklerini ve okyanus/gemi aydınlatma vektörlerini günceller */
void atmos_update_lighting(AtmosParams* p);

/* Beer-Powder hacimsel bulut zayıflama fonksiyonu: 2 * exp(-d) * (1 - exp(-2*d)) */
f32  atmos_beer_powder(f32 optical_depth);

/* 3B Periyodik Perlin-Worley + Worley bulut gürültüsü tek voksel değerlendiricisi ([0, 1]^4) */
void atmos_cloud_noise_texel(u32 x, u32 y, u32 z, u32 n, f32 out_rgba[4]);

#endif /* GFX3D_ATMOS_H */
