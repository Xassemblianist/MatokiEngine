/* phys2/solver.h — soft step temas çözücüsünün iç arayüzü (world.c ↔ solver_simd_wasm.c).
 * Skaler yol world.c'de (manifold başına); SIMD yol aynı formülü aynı işlem sırasıyla 4 manifoldda
 * birlikte uygular. Renk içi manifoldlar ortak dinamik gövde paylaşmadığından iki yol bit-bit aynıdır.
 * SamurayBalgami/Matoki'den uyarlandı (izinle).
 */
#ifndef PHYS2_SOLVER_H
#define PHYS2_SOLVER_H
#include "phys2/phys.h"

#define PHYS2_SOFT_MAX_BIAS_VEL 3.0f   /* girişim düzeltmesinin üst hızı (m/s) */

typedef struct Phys2Softness {
    f32 bias_rate, mass_scale, impulse_scale;
} Phys2Softness;

#if defined(__wasm_simd128__)
#define PHYS2_HAVE_SIMD 1
/* w->wide için gereken bayt (grup kapasitesi cap) */
usize phys2_solver_simd_bytes(u32 cap);
/* soft_prepare sonrası: renk 0..PHYS_COLORS-1 manifoldlarını 4'lü gruplara kopyala */
void phys2_solver_simd_prepare(Phys2World* w);
void phys2_solver_simd_warm_start(Phys2World* w);
void phys2_solver_simd_solve(Phys2World* w, f32 inv_h, Phys2Softness soft, b32 use_bias);
/* biriken itkileri manifoldlara geri yaz (restitüsyon ve sonraki adımın warm start'ı için) */
void phys2_solver_simd_store(Phys2World* w);
#else
#define PHYS2_HAVE_SIMD 0
#endif

#endif
