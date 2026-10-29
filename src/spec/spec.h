/* spec/spec.h — Çalışma anı özelleştirme (ARCH_V2 İlke 3 / H1): arketip başına JIT ECS döngüsü.
 * Mevcut src/scene ECS dünyasındaki varlıkları arketip başına hizalı SoA sütunlarına ayırır;
 * arketip sütun ofsetlerini (offset=...) ve simülasyon sabitlerini (dt, g, sürtünme, sınır kutusu)
 * doğrudan mbin ile üretilen dalsız SIMD128 WebAssembly modülüne gömer ve JIT ile yükler.
 */
#ifndef SPEC_SPEC_H
#define SPEC_SPEC_H

#include "core/types.h"
#include "core/arena.h"
#include "scene/ecs.h"

#define SPEC_MAX_ARCHETYPES 16u
#define SPEC_CACHE_CAP      64u
#define SPEC_REPORT_CAP     8192u

/* ECS bileşen tipleri (CompPos ve CompVel) */
typedef struct {
    f32 x, y, z;
} SpecPos;

typedef struct {
    f32 vx, vy, vz;
} SpecVel;

/* 3 sistemin çalışma anında özelleştirilen sabit parametreleri */
typedef struct {
    f32 dt;           /* Zaman adımı (s) */
    f32 gravity;      /* Y ekseni yerçekimi ivmesi */
    f32 drag;         /* Hız sönümleme çarpanı */
    f32 bmin[3];      /* Sınır kutusu alt köşe (min_x, min_y, min_z) */
    f32 bmax[3];      /* Sınır kutusu üst köşe (max_x, max_y, max_z) */
    f32 restitution;  /* Yansıma esneklik katsayısı */
} SpecParams;

/* Arketip SoA bellek yerleşimi: tek bitişik tampon içinde 6 sütunun sabit bayt ofsetleri */
typedef struct {
    u32 mask;         /* Bileşen bit maskesi */
    u32 count;        /* Aktif varlık sayısı */
    u32 cap;          /* 16'nın katına yuvarlanmış sütun kapasitesi (eleman sayısı) */
    u32 off_p[3];     /* px, py, pz sütunlarının bayt ofsetleri (16B hizalı) */
    u32 off_v[3];     /* vx, vy, vz sütunlarının bayt ofsetleri (16B hizalı) */
    u32 total_bytes;  /* Arketip tamponunun toplam bayt boyutu */
} SpecArchLayout;

/* Üretilen JIT çekirdek türleri */
typedef enum {
    SPEC_KERNEL_MOVE    = 0,  /* Sistem 1: pos += vel * dt */
    SPEC_KERNEL_GRAVITY = 1,  /* Sistem 2: vel.y += g * dt; vel *= drag */
    SPEC_KERNEL_BOUNCE  = 2,  /* Sistem 3: sınır kutusunda dalsız yansıma */
    SPEC_KERNEL_FUSED   = 3,  /* 3 sistem birleşik (tek geçişte move + gravity + bounce) */
    SPEC_KERNEL_COUNT   = 4
} SpecKernelKind;

/* JIT çekirdek imzası: (param $base i32, $count i32) -> () */
typedef void (*SpecKernelFn)(u8* base, u32 count);

/* Özelleştirme önbellek anahtarı: (çekirdek türü, arketip SoA ofsetleri, sabit parametreler) */
typedef struct {
    u32            kind;
    u32            off_p[3];
    u32            off_v[3];
    SpecParams     params;
} SpecKey;

/* Derlenmiş varyant kaydı */
typedef struct {
    b32          used;
    u64          hash;
    SpecKey      key;
    const u8*    wasm;
    u32          wasm_len;
    f64          compile_ms;
    SpecKernelFn fn;
} SpecVariant;

/* Arketip/sabit özelleştirme önbelleği */
typedef struct {
    Arena*      arena;
    SpecVariant entries[SPEC_CACHE_CAP];
    u32         count;
    u32         hits;
    u32         misses;
} SpecCache;

/* Tek bir arketipin çalışma zamanı SoA deposu */
typedef struct {
    SpecArchLayout layout;
    u8*            data;      /* 16-bayt hizalı SoA veri tamponu [layout.total_bytes] */
    EcsEntity*     entities;  /* [layout.count] EcsWorld geri senkronizasyonu için tutamaklar */
} SpecArchetype;

/* EcsWorld'den çıkarılan çoklu arketip görünümü */
typedef struct {
    SpecArchetype archetypes[SPEC_MAX_ARCHETYPES];
    u32           arch_count;
    u32           total_entities;
    EcsCompId     cid_pos;
    EcsCompId     cid_vel;
} SpecWorld;

/* Yardımcılar: varsayılan parametreler ve arketip yerleşimi hesabı */
SpecParams     spec_default_params(void);
void           spec_arch_layout_init(SpecArchLayout* layout, u32 mask, u32 count, u32 extra_prefix_cols);
const char*    spec_kernel_export_name(SpecKernelKind kind);

/* Arketip düzenine ve sabitlere özel wasm SIMD128 ikilisi üretir (dönen: bayt uzunluğu, hata=0) */
u32            spec_codegen_kernel(SpecKernelKind kind,
                                   const SpecArchLayout* layout,
                                   const SpecParams* params,
                                   b32 shared_memory,
                                   b32 own_memory,
                                   Arena* scratch,
                                   u8* out,
                                   u32 out_cap);

/* Özelleştirme önbelleği yönetimi */
void               spec_cache_init(SpecCache* cache, Arena* perm_arena);
SpecKey            spec_make_key(SpecKernelKind kind, const SpecArchLayout* layout, const SpecParams* params);
const SpecVariant* spec_cache_get(SpecCache* cache, const SpecKey* key, Arena* scratch);

/* EcsWorld <-> SpecWorld dönüşümü ve senkronizasyonu */
void spec_world_build(SpecWorld* sw, EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, Arena* arena);
void spec_world_sync_from_ecs(SpecWorld* sw, const EcsWorld* w);
void spec_world_sync_to_ecs(const SpecWorld* sw, EcsWorld* w);

/* (a) Genel C döngüsü — mevcut EcsQuery (sparse-set sorgu API'si) */
void spec_ecs_step_move(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params);
void spec_ecs_step_gravity(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params);
void spec_ecs_step_bounce(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params);
void spec_ecs_step_all(EcsWorld* w, EcsCompId cid_pos, EcsCompId cid_vel, const SpecParams* params);

/* (a-soa) Genel C SoA döngüsü — çalışma anı işaretçileri ve parametrelerle genel C döngüsü */
void spec_c_soa_move_arch(SpecArchetype* arch, const SpecParams* params);
void spec_c_soa_gravity_arch(SpecArchetype* arch, const SpecParams* params);
void spec_c_soa_bounce_arch(SpecArchetype* arch, const SpecParams* params);
void spec_c_soa_fused_arch(SpecArchetype* arch, const SpecParams* params);

void spec_c_soa_move(SpecWorld* sw, const SpecParams* params);
void spec_c_soa_gravity(SpecWorld* sw, const SpecParams* params);
void spec_c_soa_bounce(SpecWorld* sw, const SpecParams* params);
void spec_c_soa_step_all(SpecWorld* sw, const SpecParams* params);
void spec_c_soa_step_fused(SpecWorld* sw, const SpecParams* params);

/* (b) Özelleştirilmiş JIT Wasm yürütücüleri (arketip başına derlenmiş wasm modülleri) */
b32  spec_jit_run_kernel(SpecWorld* sw, SpecCache* cache, SpecKernelKind kind,
                         const SpecParams* params, Arena* scratch);
b32  spec_jit_step_3sys(SpecWorld* sw, SpecCache* cache, const SpecParams* params, Arena* scratch);
b32  spec_jit_step_fused(SpecWorld* sw, SpecCache* cache, const SpecParams* params, Arena* scratch);

/* Doğrulama ve 100k varlık benchmark koşucusu (engine.wasm ve tarayıcı için) */
i32         spec_bench_verify(Arena* arena);
i32         spec_bench_run(Arena* arena, u32 entity_count);
const char* spec_bench_report_ptr(void);
u32         spec_bench_report_len(void);

#endif /* SPEC_SPEC_H */
