/* gfx3d/ocean.h — Tessendorf 3 kademeli FFT okyanus simülasyonu ve geometrik ızgara ağı.
 * JONSWAP spektrumu (rüzgâr hızı, fetch, γ=3.3) + cos² yönlü yayılım ve derin su dağılımı (ω=√(gk)).
 * Kademe sınırlarında dalga sayısı (k) kesilerek çift sayım önlenir; her kademede 4 karmaşık 2B IFFT
 * ile 8 gerçek alan (h, Dx, Dz, ∂h/∂x, ∂h/∂z, ∂Dx/∂x, ∂Dz/∂z, ∂Dx/∂z), Jacobian köpüğü ve mipmap üretilir.
 */
#ifndef GFX3D_OCEAN_H
#define GFX3D_OCEAN_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/texture.h"
#include "gfx3d/mesh.h"

#define OCEAN_CASCADES       3u
#define OCEAN_G              9.81f
#define OCEAN_TEX_SLOPE_MAX  1.60f

typedef struct {
    u32 n;      /* 2'nin kuvveti (ör. 128, 128, 64) */
    f32 size;   /* yama kenar uzunluğu (m, ör. 500, 60, 8) */
} OceanCascadeDesc;

typedef struct {
    OceanCascadeDesc cascades[OCEAN_CASCADES];
    f32              wind_speed;      /* U10 rüzgâr hızı (m/s) */
    f32              wind_x, wind_z;  /* rüzgâr yönü */
    f32              fetch;           /* rüzgâr mesafesi (m, ör. 100000) */
    f32              gamma;           /* JONSWAP tepe çarpanı (3.3) */
    f32              reverse;         /* rüzgâra karşı yön payı (0..1) */
    f32              choppy;          /* yatay yer değiştirme çarpanı λ */
    f32              foam_threshold;  /* Jacobian J eşiği (J < eşik -> köpük) */
    f32              foam_decay;      /* köpük sönüm hızı (1/s) */
    f32              foam_scale;      /* köpük üretim kazancı */
    u64              seed;
} OceanDesc;

typedef struct {
    u32     n, log2n;
    f32     size;
    f32     k_min, k_max;             /* bu kademenin aktif [k_min, k_max] aralığı (çift sayım yok) */
    f32*    xi_r;                     /* sabit standart normal çiftleri (rüzgâr değişince yeniden kullanılır) */
    f32*    xi_i;
    f32*    h0r;                      /* h̃0(k) gerçek ve sanal */
    f32*    h0i;
    f32*    h0mr;                     /* conj(h̃0(-k)) gerçek ve sanal */
    f32*    h0mi;
    f32*    omega;                    /* ω(k) = √(g|k|) */
    f32*    kx;                       /* dalga vektörü bileşenleri (rad/m) */
    f32*    kz;
    u32*    bitrev;                   /* bit-ters indeks tablosu [n] */
    f32*    tw_c;                     /* IFFT twiddle cos(2πj/n) [n/2] */
    f32*    tw_s;                     /* IFFT twiddle sin(2πj/n) [n/2] */
    f32*    wr[4];                    /* 4 karmaşık 2B IFFT çalışma tamponu (gerçek) */
    f32*    wi[4];                    /* 4 karmaşık 2B IFFT çalışma tamponu (sanal) */
    /* 8 gerçek çıktı alanı + zamanla sönen Jacobian köpük birikimi [n*n] */
    f32*    height;                   /* h */
    f32*    disp_x;                   /* Dx (λ ile çarpılmamış birim yer değiştirme) */
    f32*    disp_z;                   /* Dz */
    f32*    slope_x;                  /* ∂h/∂x */
    f32*    slope_z;                  /* ∂h/∂z */
    f32*    dxx;                      /* ∂Dx/∂x */
    f32*    dzz;                      /* ∂Dz/∂z */
    f32*    dxz;                      /* ∂Dx/∂z */
    f32*    foam;                     /* [0, 1] zamanla sönen köpük birikimi */
    Texture tex;                      /* RGBA8 (@bilinear @mipmap): R=slope_x, G=slope_z, B=h, A=foam */
} OceanCascade;

typedef struct {
    Vec3 disp;                        /* (λ·Dx, h, λ·Dz) dünya yer değiştirmesi */
    Vec3 normal;                      /* birim yüzey normali */
    f32  slope_x, slope_z;            /* toplam eğim */
    f32  jacobian;                    /* J = (1+λDx_x)(1+λDz_z) - λ²Dx_z² */
    f32  foam;                        /* [0, 1] köpük */
} OceanSample;

typedef struct {
    OceanDesc    d;
    OceanCascade cascades[OCEAN_CASCADES];
    f32          hs;                  /* anlamlı dalga yüksekliği Hs = 4·σ_h (m) */
    f32          theory_hs;           /* deterministik spektrum integralinden teorik Hs (m) */
    b32          has_updated;
    /* Kamera merkezli geometrik büyüyen ızgara ağı */
    Mesh*        mesh;
    f32*         grid_ofs;            /* [-grid_radius, +grid_radius] 1B geometrik ofset tablosu [grid_div+1] */
    u32          grid_div;
    f32          grid_snap;
    f32          grid_radius;
} Ocean;

/* Varsayılan 3 kademeli (L = 500 / 60 / 8 m, N = 128 / 128 / 64, U = 10 m/s, γ = 3.3) tanım */
OceanDesc ocean_default_desc(void);

/* JONSWAP 1B açısal frekans spektrumu S(ω) (m²·s/rad) */
f32  ocean_jonswap_omega(f32 omega, f32 wind_speed, f32 fetch, f32 gamma);

/* Okyanus kademelerini, RNG Gauss tablolarını, twiddle/bitrev dizilerini ve mipmap dokularını kurar */
void ocean_init(Ocean* o, Arena* a, const OceanDesc* d);

/* Rüzgâr hızı/yönü veya fetch/gamma değiştiğinde aynı Gauss çekilişleriyle spektrumu ve Hs'yi günceller */
void ocean_rebuild_spectrum(Ocean* o);

/* Rüzgâr hızı (m/s) ve yönüne (wx, wz) göre JONSWAP parametrelerini (fetch, choppy, köpük)
 * günceller; rüzgâr değişimi eşiği aştıysa spektrumu yeniden kurup TRUE döner. */
b32  ocean_set_wind(Ocean* o, f32 wind_speed, f32 wind_x, f32 wind_z);

/* Yalnızca simülasyon (kaldırma kuvveti ve dalga sörfü) için IFFT yükseklik/eğim alanlarını
 * günceller (doku/mipmap üretimini atlar); 1T ve 8T'de bit-bit özdeştir. */
void ocean_update_sim(Ocean* o, f64 time_sec);

/* Tek kademede idx dalga vektörünün t anındaki karmaşık genliği h̃(k,t) (birim testler için) */
void ocean_cascade_amplitude(const OceanCascade* c, u32 idx, f64 t, f32* out_re, f32* out_im);

/* n noktalı yerinde ters (pozitif üslü, normalize edilmemiş) radix-2 1B/2B karmaşık FFT */
void ocean_fft_1d(const OceanCascade* c, f32* re, f32* im, u32 stride);
void ocean_fft_2d(const OceanCascade* c, f32* re, f32* im);

/* Tüm kademeleri t (saniye) anına günceller: 4×2B paralel IFFT, Jacobian köpük, RGBA8 doku + tex_build_mips */
void ocean_update(Ocean* o, f64 time_sec, f32 dt);

/* Tek kademeden veya tüm kademelerden (uzaklık sönümlü) periyodik bilinear örnekleme */
void ocean_sample_cascade(const OceanCascade* c, f32 choppy, f32 x, f32 z, OceanSample* out);
void ocean_sample(const Ocean* o, f32 x, f32 z, f32 dist_from_eye, OceanSample* out);

/* Kameraya ortalı, uzaklıkla geometrik büyüyen aralıklı okyanus ızgara ağını ayırır ve günceller */
Mesh* ocean_mesh_init(Ocean* o, Arena* a, u32 div, f32 snap, f32 max_radius);
void  ocean_mesh_update(Ocean* o, Vec3 eye);

#endif
