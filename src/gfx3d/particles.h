/* gfx3d/particles.h — Sabit kapasiteli, deterministik 3D parçacık sistemi ve yayıcı (emitter) modülü.
 * Arena tabanlı havuzda nokta/küre yayıcıları, hız/ömür aralıkları, yerçekimi, doğrusal sürtünme,
 * başlangıç->bitiş boyut ve renk eğrileri ile O(1) swap-remove silme işlemlerini gerçekleştirir.
 * Aktif parçacıkları tek çağrıda BillboardBatch üzerinden dinamik Mesh'e dönüştürür.
 */
#ifndef GFX3D_PARTICLES_H
#define GFX3D_PARTICLES_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "raster/framebuffer.h"
#include "gfx3d/billboard.h"

#define PARTICLES_MAX_CAPACITY 65536u

typedef enum {
    EMIT_SHAPE_POINT  = 0,  /* Tek noktadan yayım */
    EMIT_SHAPE_SPHERE = 1   /* Küre hacmi/yüzeyi içinden yayım */
} EmitterShape;

typedef struct {
    EmitterShape shape;         /* Yayıcı şekli (nokta veya küre) */
    Vec3         pos;           /* Yayıcı dünya merkezi */
    f32          radius;        /* Küre yarıçapı (EMIT_SHAPE_SPHERE için, >= 0) */
    Vec3         vel_min;       /* Başlangıç kutusal hız alt sınırı (birim/s) */
    Vec3         vel_max;       /* Başlangıç kutusal hız üst sınırı (birim/s) */
    f32          speed_min;     /* Küresel dışa doğru ek hız alt sınırı (birim/s) */
    f32          speed_max;     /* Küresel dışa doğru ek hız üst sınırı (birim/s) */
    f32          life_min;      /* Parçacık ömrü alt sınırı (saniye, > 0) */
    f32          life_max;      /* Parçacık ömrü üst sınırı (saniye, >= life_min) */
    Vec3         gravity;       /* Yerçekimi / sabit ivme vektörü (birim/s^2) */
    f32          drag;          /* Doğrusal sürtünme katsayısı (1/s, >= 0) */
    f32          size_start;    /* Başlangıç boyutu (> 0) */
    f32          size_end;      /* Bitiş boyutu (>= 0) */
    Vec4         color_start;   /* Başlangıç rengi (RGBA 0..1) */
    Vec4         color_end;     /* Bitiş rengi (RGBA 0..1) */
    f32          rot_min;       /* Başlangıç dönüş açısı alt sınırı (radyan) */
    f32          rot_max;       /* Başlangıç dönüş açısı üst sınırı (radyan) */
    f32          rot_vel_min;   /* Açısal hız alt sınırı (radyan/s) */
    f32          rot_vel_max;   /* Açısal hız üst sınırı (radyan/s) */
    u32          atlas_cols;    /* Doku atlası sütun sayısı (0 veya 1: tam doku) */
    u32          atlas_rows;    /* Doku atlası satır sayısı (0 veya 1: tam doku) */
    u32          atlas_frame;   /* Sabit veya başlangıç atlas karesi */
    b32          animate_atlas; /* TRUE ise ömür boyunca atlas karelerini ilerletir */
} ParticleEmitter;

typedef struct {
    Vec3 pos;           /* Dünya konumu */
    Vec3 vel;           /* Hız vektörü (birim/s) */
    Vec3 gravity;       /* Yerçekimi / sabit ivme (birim/s^2) */
    f32  drag;          /* Sürtünme katsayısı (1/s) */
    f32  age;           /* Geçen ömür (s) */
    f32  life;          /* Toplam ömür (s) */
    f32  inv_life;      /* 1.0f / life */
    f32  size_start;    /* Başlangıç boyutu */
    f32  size_end;      /* Bitiş boyutu */
    f32  size;          /* Güncel boyut */
    f32  rot;           /* Güncel dönüş açısı (radyan) */
    f32  rot_vel;       /* Açısal hız (radyan/s) */
    u32  color0_rgba;   /* Başlangıç rengi (RGBA8) */
    u32  color1_rgba;   /* Bitiş rengi (RGBA8) */
    u32  color;         /* Güncel renk (RGBA8) */
    u16  atlas_cols;    /* Atlas sütun sayısı */
    u16  atlas_rows;    /* Atlas satır sayısı */
    u16  atlas_frame;   /* Güncel atlas karesi */
    u16  animate_atlas; /* Atlas animasyonu aktif mi */
} Particle;

typedef struct {
    Particle* items;          /* Aktif parçacık dizisi [0..count-1] (kapasite: capacity) */
    u32       capacity;       /* Havuz maksimum parçacık kapasitesi */
    u32       count;          /* Aktif parçacık sayısı */
    u32       overflow_count; /* Havuz doluyken atlanan parçacık sayısı */
    u32       skipped_count;  /* Geçersiz yayıcı parametresi nedeniyle atlanan sayısı */
    u32       expired_last;   /* Son particles_update çağrısında ömrü dolan sayısı */
    u64       expired_total;  /* Başlangıçtan beri ömrü dolan toplam parçacık sayısı */
    u64       emitted_total;  /* Başlangıçtan beri üretilen toplam parçacık sayısı */
    Rng       rng;            /* Deterministik PCG32 rastgele sayı üreteci */
    b32       initialized;    /* Başarıyla ayrıldı mı */
} ParticleSystem;

/* Vec4 (0..1) rengi RGBA8 (FB_RGBA) tamsayısına dönüştürür */
u32  particles_pack_color(Vec4 c);

/* Varsayılan, geçerli bir yayıcı yapılandırması döner */
ParticleEmitter particles_default_emitter(void);

/* Sabit kapasiteli parçacık havuzunu verilen Arena üzerinde ayırır ve RNG'yi tohumlar */
b32  particles_init(ParticleSystem* ps, Arena* a, u32 capacity, u64 seed);

/* Havuzu sıfırlar ve RNG tohumunu yeniden kurar */
void particles_reset(ParticleSystem* ps, u64 seed);

/* Yayıcıdan tek seferde count adet parçacık fırlatır; gerçekten üretilen sayıyı döner */
u32  particles_emit_burst(ParticleSystem* ps, const ParticleEmitter* em, u32 count);

/* Tüm aktif parçacıkları dt saniye ilerletir; ömrü dolanları O(1) swap-remove ile siler.
 * Dönüş: bu adımda ömrü dolup silinen parçacık sayısı. */
u32  particles_update(ParticleSystem* ps, f32 dt);

/* Aktif parçacıkları açık bir BillboardBatch içine ekler; eklenen dörtgen sayısını döner */
u32  particles_to_billboards(const ParticleSystem* ps, BillboardBatch* b);

/* Tek çağrıda billboard_begin + parçacık aktarımı + (isteğe bağlı radix sıralama) + billboard_end */
Mesh* particles_build_billboards(const ParticleSystem* ps, BillboardBatch* b,
                                 Mat4 view, b32 sort_back_to_front);

#endif
