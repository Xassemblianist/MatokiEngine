/* gfx3d/vertex.h — Vertex aşaması çekirdekleri: SoA konum dönüşümü (model → klip uzayı).
 * wasm'da 4 köşe birden f32x4 ile (vertex_wasm.c), host'ta skaler (vertex.c); ikisi bit-bit aynı:
 * x' = ((m0*x + m4*y) + m8*z) + m12 sırası korunur (mat4_mul_vec4 ile aynı, FMA yok).
 */
#ifndef GFX3D_VERTEX_H
#define GFX3D_VERTEX_H

#include "core/types.h"
#include "core/math.h"

/* out_* dizileri en az n eleman; n 4'ün katı olmak zorunda değil */
void g3d_transform_positions(const Mat4* m, const f32* px, const f32* py, const f32* pz, u32 n,
                             f32* out_x, f32* out_y, f32* out_z, f32* out_w);

#endif
