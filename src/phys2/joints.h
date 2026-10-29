/* phys2/joints.h — eklem çözücüsünün dünya içi arayüzü (world.c kullanır).
 * Eklemler temaslardan önce çözülür (Box2D sırası). Soft step'te her alt adımda yumuşak
 * (60 Hz, ζ = 2) + relax geçişi; SI'da Baumgarte. Biriken itkiler adımlar arası warm start'tır.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_JOINTS_H
#define PHYS2_JOINTS_H
#include "phys2/phys.h"

b32  phys2_joints_block_pair(const Phys2World* w, u32 a, u32 b);
void phys2_joints_prepare(Phys2World* w, f32 h);
void phys2_joints_warm_start(Phys2World* w);
void phys2_joints_solve(Phys2World* w, f32 h, b32 use_bias, b32 soft);

#endif
