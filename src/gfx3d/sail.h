/* gfx3d/sail.h — Görsel sınıf (D-112) XPBD yelken kumaşı simülasyonu ve dinamik Mesh üretimi.
 * 9×9 parçacık ızgarası üzerinde yapısal/kesme/bükülme mesafe kısıtlarını XPBD ile çözer;
 * görünür rüzgâr basıncıyla şişer, ölü bölgede (luffing) firar kenarında çırpınma dalgası üretir
 * ve bumba açısına göre döndürülmüş köşe konumları + normallerini Mesh'e yazar.
 */
#ifndef GFX3D_SAIL_H
#define GFX3D_SAIL_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/mesh.h"

#define SAIL_GRID_DIV       8u
#define SAIL_GRID_N         (SAIL_GRID_DIV + 1u)
#define SAIL_VERT_COUNT     (SAIL_GRID_N * SAIL_GRID_N)
#define SAIL_MAX_CONSTR     256u

typedef struct {
    u16 a, b;
    f32 rest_len;
    f32 compliance;
} SailConstraint;

typedef struct {
    f32 mast_z;                      /* direk/dayanak menteşe Z koordinatı */
    f32 z_bot0, z_bot1;
    f32 z_top0, z_top1;
    f32 y_bot, y_top;
    f32 max_belly;
    f32 boom_angle;                  /* mevcut bumba dönüş açısı (radyan, Y ekseni etrafında) */
    f32 flutter_phase;               /* yapraklama (luffing) fazı */

    /* Parçacık durumu (yelken yerel düzleminde) */
    f32 rx[SAIL_VERT_COUNT];
    f32 ry[SAIL_VERT_COUNT];
    f32 rz[SAIL_VERT_COUNT];
    f32 px[SAIL_VERT_COUNT];
    f32 py[SAIL_VERT_COUNT];
    f32 pz[SAIL_VERT_COUNT];
    f32 vx[SAIL_VERT_COUNT];
    f32 vy[SAIL_VERT_COUNT];
    f32 vz[SAIL_VERT_COUNT];
    f32 inv_m[SAIL_VERT_COUNT];

    SailConstraint constr[SAIL_MAX_CONSTR];
    u32            constr_count;
    Mesh*          mesh;
} SailCloth;

/* Yelken kumaşını ve bağlı 9×9 üçgen ağını (Mesh) kalıcı arenada ilklendirir */
Mesh* sail_cloth_init(SailCloth* sc, Arena* a,
                      f32 z_bot0, f32 z_bot1, f32 z_top0, f32 z_top1,
                      f32 y_bot, f32 y_top, f32 max_belly);

/* Parçacıkları dinlenme konumuna sıfırlar */
void  sail_cloth_reset(SailCloth* sc);

/* Görünür rüzgâr (tekne yerel uzayında), bumba açısı ve yapraklama (luffing 0..1) ile
 * XPBD adımını ilerletir ve sc->mesh köşe/normal dizilerini günceller. */
void  sail_cloth_step(SailCloth* sc, Vec3 local_app_wind, f32 target_boom_angle,
                      f32 luffing, f32 dt);

#endif /* GFX3D_SAIL_H */
