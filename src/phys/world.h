/* phys/world.h — 3D fizik dünyası (PhysWorld) ve karakter kontrolcüsü (move_and_slide).
 * Sabit zaman adımlı (1/120 s) yarı-örtük Euler entegrasyonu, yerçekimi, dinamik küre/kapsül
 * gövdeler, statik şekiller ve BVH hızlandırmalı statik üçgen ağlarını (Mesh) yönetir.
 * Yüksek hızlarda (ör. 50 m/s) tünellemeyi önlemek için yarıçap-oranlı alt adımlama kullanır.
 */
#ifndef PHYS_WORLD_H
#define PHYS_WORLD_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/mesh.h"
#include "phys/shapes.h"
#include "phys/collide.h"
#include "phys/bvh.h"
#include "phys/raycast.h"

#define PHYS_FIXED_DT          (1.0f / 120.0f)
#define PHYS_DEFAULT_GRAVITY   (-9.81f)
#define PHYS_DEFAULT_SLOPE_COS 0.70710678f   /* cos(45°) */

typedef enum {
    BODY_SPHERE  = 0,
    BODY_CAPSULE = 1
} PhysBodyKind;

typedef struct {
    PhysBodyKind kind;
    Vec3 pos;            /* küre merkezi veya kapsül orta noktası */
    Vec3 vel;            /* doğrusal hız (m/s) */
    Vec3 seg_offset;     /* kapsül için yarı-eksen vektörü: a = pos - seg_offset, b = pos + seg_offset */
    f32  radius;
    f32  inv_mass;       /* 1 / kütle (0 ise sonsuz kütle) */
    f32  restitution;    /* esneklik katsayısı [0..1] */
    f32  friction;       /* sürtünme katsayısı [0..1] */
    f32  max_slope_cos;  /* durulabilir maksimum eğim açısının kosinüsü */
    b32  on_ground;      /* son adımda eğim sınırı altındaki bir zemine değdi mi */
    Vec3 ground_normal;
} PhysBody;

typedef struct {
    PhysShape shape;
    f32       restitution;
    f32       friction;
} PhysStaticShape;

typedef struct {
    const Mesh* mesh;
    const Bvh*  bvh;
    f32         restitution;
    f32         friction;
} PhysStaticMesh;

typedef struct {
    b32  on_ground;      /* eğim sınırı altındaki bir yüzeyde duruyor mu */
    b32  hit_wall;       /* eğim sınırı üstündeki dik eğim veya duvara çarptı mı */
    b32  hit_ceiling;    /* tavana çarptı mı */
    Vec3 ground_normal;  /* yerde ise zemin birim normali */
    u32  contact_count;  /* çözülen temas sayısı */
} SlideResult;

typedef struct {
    Vec3             gravity;
    f32              fixed_dt;
    f32              accum;
    PhysBody*        bodies;
    u32              body_count;
    u32              body_cap;
    PhysStaticShape* statics;
    u32              static_count;
    u32              static_cap;
    PhysStaticMesh*  meshes;
    u32              mesh_count;
    u32              mesh_cap;
} PhysWorld;

void phys_world_init(PhysWorld* w, Arena* a, u32 max_bodies, u32 max_statics, u32 max_meshes);

u32  phys_world_add_sphere(PhysWorld* w, Vec3 center, f32 radius,
                           f32 mass, f32 restitution, f32 friction);
u32  phys_world_add_capsule(PhysWorld* w, Vec3 a, Vec3 b, f32 radius,
                            f32 mass, f32 restitution, f32 friction);
u32  phys_world_add_static(PhysWorld* w, PhysShape shape, f32 restitution, f32 friction);
u32  phys_world_add_mesh(PhysWorld* w, const Mesh* mesh, const Bvh* bvh,
                         f32 restitution, f32 friction);

Sphere    phys_body_sphere(const PhysBody* b);
Capsule   phys_body_capsule(const PhysBody* b);
PhysShape phys_body_shape(const PhysBody* b);

/* Tek bir sabit zaman adımı (w->fixed_dt = 1/120 s) ilerletir. */
void phys_world_step(PhysWorld* w);

/* Değişken dt'yi biriktirip 1/120 s'lik sabit adımlarla ilerletir; koşulan adım sayısını döner. */
u32  phys_world_update(PhysWorld* w, f32 dt);

/* Karakter kontrolcüsü: kapsül veya küreyi vel*dt kadar hareket ettirir, çarpışmalarda
 * yüzey boyunca kaydırır (basamak yok, max_slope_cos eğim sınırı var, tünelleme yok). */
SlideResult phys_move_and_slide(const PhysWorld* w, Capsule* cap, Vec3* vel,
                                f32 dt, f32 max_slope_cos);
SlideResult phys_move_and_slide_sphere(const PhysWorld* w, Sphere* s, Vec3* vel,
                                       f32 dt, f32 max_slope_cos);

/* Dünyadaki tüm statik şekiller, statik mesh'ler ve dinamik gövdeler üzerinde en yakın ışın isabeti. */
b32  phys_world_raycast(const PhysWorld* w, Ray r, f32 t_max, RayHit* out);

#endif
