/* state/state_world.h — İşaretçisiz SoA ECS deposu ve fizik gövde dilimi prototipi (S1).
 * Bu başlıktaki tüm yapılar durum bölgesi (StateRegion) içinde yaşar; hiçbir alanda C işaretçisi
 * (T*) veya platforma bağlı boyutlu tip (usize/uptr) bulunmaz. Bölge farklı bir taban adrese
 * kopyalandığında hiçbir ofset/tutamaç düzeltmesi gerekmeden doğrudan çalışmaya devam eder.
 */
#ifndef STATE_STATE_WORLD_H
#define STATE_STATE_WORLD_H

#include "state/state_region.h"

#define STATE_WORLD_ABI_HASH   0x5331574Cu /* "S1WL" */
#define STATE_SPARSE_NIL       0xFFFFFFFFu

/* Bileşen maskesi bitleri */
#define STATE_COMP_TRANSFORM   (1u << 0)
#define STATE_COMP_VELOCITY    (1u << 1)
#define STATE_COMP_HEALTH      (1u << 2)
#define STATE_COMP_PHYS_LINK   (1u << 3)
#define STATE_COMP_SEEKER      (1u << 4)

/* Fizik gövdesi durum bayrakları */
#define STATE_BODY_ACTIVE      (1u << 0)
#define STATE_BODY_ON_GROUND   (1u << 1)

/* Sürüm / nesil kontrollü tipli tutamaç tanımları */
STATE_HANDLE_DEFINE(Entity);
STATE_HANDLE_DEFINE(PhysBody);

/* ----------------------------------------------------------------------------
 * StatePhysBody — İşaretçisiz Fizik Gövdesi (48 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    Vec3                  pos;          /* dünya konumu */
    f32                   radius;       /* çarpışma küresi yarıçapı */
    Vec3                  vel;          /* doğrusal hız */
    f32                   inv_mass;     /* 1 / kütle (0 ise statik) */
    f32                   restitution;  /* esneklik (sekme) katsayısı */
    f32                   damping;      /* doğrusal hız sönümleme */
    StateHandleOf(Entity) owner_entity; /* bağlı ECS varlığı tutamacı */
    u32                   flags;        /* STATE_BODY_* bayrakları */
} StatePhysBody;

STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, pos);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, radius);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, vel);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, inv_mass);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, restitution);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, damping);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, owner_entity);
STATE_ASSERT_NO_PTR_FIELD(StatePhysBody, flags);
STATE_ASSERT_VALUE_STRUCT(StatePhysBody, 48u);
STATE_REF_DEFINE(StatePhysBody);

/* ----------------------------------------------------------------------------
 * StateContact — İşaretçisiz Deterministik Temas Kaydı (32 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    StateHandleOf(PhysBody) body_a;      /* küçük yoğun indeksli gövde tutamacı */
    StateHandleOf(PhysBody) body_b;      /* büyük yoğun indeksli gövde tutamacı */
    Vec3                    normal;      /* temas normali (A -> B) */
    f32                     penetration; /* girinti derinliği */
    f32                     impulse;     /* çözülen normal impuls büyüklüğü */
    u32                     tick;        /* temasın oluştuğu simülasyon adımı */
} StateContact;

STATE_ASSERT_NO_PTR_FIELD(StateContact, body_a);
STATE_ASSERT_NO_PTR_FIELD(StateContact, body_b);
STATE_ASSERT_NO_PTR_FIELD(StateContact, normal);
STATE_ASSERT_NO_PTR_FIELD(StateContact, penetration);
STATE_ASSERT_NO_PTR_FIELD(StateContact, impulse);
STATE_ASSERT_NO_PTR_FIELD(StateContact, tick);
STATE_ASSERT_VALUE_STRUCT(StateContact, 32u);
STATE_REF_DEFINE(StateContact);

/* ----------------------------------------------------------------------------
 * StatePhysSlice — İşaretçisiz Fizik Alt Sistemi Durumu (104 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    StateSlotTable            slots;           /* nesilli gövde tutamaç tablosu (32 B) */
    StateSlice(StatePhysBody) bodies;          /* [max_bodies] yoğun gövde dizisi dilimi */
    StateSlice(u32)           slot_to_dense;   /* [max_bodies] slot_idx -> dense_idx */
    StateSlice(StateHandle)   dense_to_handle; /* [max_bodies] dense_idx -> StateHandle */
    StateSlice(StateContact)  contacts;        /* [max_contacts] temas önbelleği dilimi */
    u32                       body_count;      /* aktif yoğun gövde sayısı */
    u32                       contact_count;   /* bu adımda kaydedilen temas sayısı */
    Vec3                      gravity;         /* yerçekimi ivmesi */
    f32                       ground_y;        /* yatay zemin yüksekliği */
    Vec3                      bounds_half;     /* kapalı arena yarı boyutları */
    u32                       bounce_count;    /* toplam zemin/duvar/gövde sekme sayısı */
} StatePhysSlice;

STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, slots);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, bodies);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, slot_to_dense);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, dense_to_handle);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, contacts);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, body_count);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, contact_count);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, gravity);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, ground_y);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, bounds_half);
STATE_ASSERT_NO_PTR_FIELD(StatePhysSlice, bounce_count);
STATE_ASSERT_VALUE_STRUCT(StatePhysSlice, 104u);
STATE_REF_DEFINE(StatePhysSlice);

/* ----------------------------------------------------------------------------
 * StateEcsSoA — İşaretçisiz SoA (Structure of Arrays) ECS Deposu (136 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    StateSlotTable          slots;         /* nesilli varlık tutamaç tablosu (32 B) */
    u32                     dense_count;   /* aktif yoğun varlık sayısı */
    u32                     max_entities;  /* maksimum varlık kapasitesi */
    StateSlice(u32)         sparse;        /* [max_entities] slot_idx -> dense_idx */
    StateSlice(StateHandle) dense_entity;  /* [max_entities] dense_idx -> varlık tutamacı */

    /* SoA bileşen sütunları ([max_entities] uzunluklu tipli dilimler) */
    StateSlice(f32)         pos_x;
    StateSlice(f32)         pos_y;
    StateSlice(f32)         pos_z;
    StateSlice(f32)         vel_x;
    StateSlice(f32)         vel_y;
    StateSlice(f32)         vel_z;
    StateSlice(i32)         hp;
    StateSlice(u32)         comp_mask;
    StateSlice(StateHandle) phys_body;     /* bağlı fizik gövdesi tutamacı */
    StateSlice(StateHandle) target_entity; /* takip edilen hedef varlık tutamacı */
} StateEcsSoA;

STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, slots);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, dense_count);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, max_entities);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, sparse);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, dense_entity);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, pos_x);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, pos_y);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, pos_z);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, vel_x);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, vel_y);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, vel_z);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, hp);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, comp_mask);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, phys_body);
STATE_ASSERT_NO_PTR_FIELD(StateEcsSoA, target_entity);
STATE_ASSERT_VALUE_STRUCT(StateEcsSoA, 136u);
STATE_REF_DEFINE(StateEcsSoA);

/* ----------------------------------------------------------------------------
 * StateWorld — Durum Bölgesi Kök Yapısı (280 bayt)
 * ---------------------------------------------------------------------------- */

typedef struct {
    u32            abi_version;     /* STATE_REGION_VERSION */
    u32            step_count;      /* tamamlanan simülasyon adımı sayısı */
    u64            rng_state;       /* deterministik PCG32 durumu */
    u64            rng_inc;         /* deterministik PCG32 akış sabiti */
    StateEcsSoA    ecs;             /* işaretçisiz SoA ECS deposu (136 B) */
    StatePhysSlice phys;            /* işaretçisiz fizik gövde dilimi (104 B) */
    u32            spawned_total;   /* toplam oluşturulan varlık sayısı */
    u32            destroyed_total; /* toplam silinen varlık sayısı */
    u64            checksum_acc;    /* adım içi yürüyen özet akümülatörü */
} StateWorld;

STATE_ASSERT_NO_PTR_FIELD(StateWorld, abi_version);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, step_count);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, rng_state);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, rng_inc);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, ecs);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, phys);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, spawned_total);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, destroyed_total);
STATE_ASSERT_NO_PTR_FIELD(StateWorld, checksum_acc);
STATE_ASSERT_VALUE_STRUCT(StateWorld, 280u);
STATE_REF_DEFINE(StateWorld);

/* ----------------------------------------------------------------------------
 * StateWorld, StateEcsSoA ve StatePhysSlice API'si
 * ---------------------------------------------------------------------------- */

StateRef(StateWorld)    state_world_init(void* base, u32 cap, u32 max_entities,
                                         u32 max_bodies, u32 max_contacts, u32 seed);
StateWorld*             state_world_get(void* base);
const StateWorld*       state_world_get_const(const void* base);

/* Fizik gövdesi yönetimi ve adımlama */
StateHandleOf(PhysBody) state_phys_add_body(void* base, Vec3 pos, Vec3 vel, f32 radius,
                                            f32 mass, f32 restitution,
                                            StateHandleOf(Entity) owner);
b32                     state_phys_remove_body(void* base, StateHandleOf(PhysBody) h);
b32                     state_phys_body_valid(const void* base, StateHandleOf(PhysBody) h);
StatePhysBody*          state_phys_get_body(void* base, StateHandleOf(PhysBody) h);
const StatePhysBody*    state_phys_get_body_const(const void* base, StateHandleOf(PhysBody) h);
void                    state_phys_step(void* base, f32 dt);

/* ECS SoA varlık yönetimi ve adımlama */
StateHandleOf(Entity)   state_ecs_spawn(void* base, Vec3 pos, Vec3 vel, i32 hp, u32 mask);
b32                     state_ecs_destroy(void* base, StateHandleOf(Entity) e);
b32                     state_ecs_alive(const void* base, StateHandleOf(Entity) e);
u32                     state_ecs_dense_index(const void* base, StateHandleOf(Entity) e);
void                    state_ecs_bind_body(void* base, StateHandleOf(Entity) e,
                                            StateHandleOf(PhysBody) body);
void                    state_ecs_set_target(void* base, StateHandleOf(Entity) e,
                                             StateHandleOf(Entity) target);

/* Tek bir sabit zaman adımı (SoA yapay zeka/takip + fizik + senkronizasyon + yaşam döngüsü) */
void                    state_world_step(void* base, f32 dt);

/* Örnek sahne doldurma (test ve benchmark için deterministik başlangıç durumu) */
void                    state_world_populate_demo(void* base, u32 entity_count, u32 body_count);

#endif
