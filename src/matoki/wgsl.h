/* matoki/wgsl.h — Matoki IR → WebGPU Shading Language (WGSL) kod üreticisi (D-108).
 * Optimizasyon sonrası skaler SSA IR'dan (IrFunc) piksel başına çalışan WGSL fragment
 * shader'ı ve gfx3d varying sözleşmesine (G3dAttr) uygun WGSL vertex shader'ı üretir.
 * Uniform tamponu 16-bayt hizalı array<vec4<f32>, N>, dokular texture_2d<f32> + sampler.
 */
#ifndef MATOKI_WGSL_H
#define MATOKI_WGSL_H

#include "core/types.h"
#include "core/arena.h"
#include "matoki/matoki.h"

/* Varsayılan @group ve @binding numaraları (D-108 RHI sözleşmesi) */
#define MTK_WGSL_FRAG_GROUP       0u
#define MTK_WGSL_UBO_BINDING      0u
#define MTK_WGSL_TEX_BINDING(s)   (1u + (u32)(s) * 2u)
#define MTK_WGSL_SAMP_BINDING(s)  (2u + (u32)(s) * 2u)
#define MTK_WGSL_BUF_BINDING(s)   (1u + (u32)(s))

#define MTK_WGSL_VERT_GROUP       1u
#define MTK_WGSL_VERT_UBO_BINDING 0u

/* Vertex shader giriş öznitelik konumları (VsIn @location) */
#define MTK_WGSL_VS_LOC_POS       0u
#define MTK_WGSL_VS_LOC_NORMAL    1u
#define MTK_WGSL_VS_LOC_UV        2u
#define MTK_WGSL_VS_LOC_COLOR     3u

/* gfx3d/pipeline.h G3dAttr ile birebir uyumlu varying öznitelik kimlikleri */
typedef enum {
    MTK_WGSL_ATTR_UV         = 0,   /* vec2<f32>, VsIn @location(2) */
    MTK_WGSL_ATTR_NORMAL     = 1,   /* vec3<f32>, dünya uzayı (normal_mat ile), VsIn @location(1) */
    MTK_WGSL_ATTR_VIEW_DEPTH = 2,   /* f32, görüş derinliği (= clip_pos.w) */
    MTK_WGSL_ATTR_COLOR      = 3,   /* vec4<f32>, köşe rengi, VsIn @location(3) */
    MTK_WGSL_ATTR_WORLD_POS  = 4,   /* vec3<f32>, dünya konumu (model * pos) */
    MTK_WGSL_ATTR_SHADOW_POS = 5,   /* vec3<f32>, gölge haritası uzayı (shadow_mat * pos) */
    MTK_WGSL_ATTR_COUNT      = 6
} MtkWgslAttr;

#define MTK_WGSL_MAX_ATTRS 6u

typedef struct {
    u32         frag_group;   /* bind group numarası (varsayılan 0) */
    const char* entry_name;   /* giriş fonksiyonu adı (NULL ise "frag") */
} MtkWgslOpts;

/* Optimizasyon sonrası IrFunc'tan NUL sonlu WGSL fragment shader metni üretir.
 * layout != NULL ise varying @location(n) bildirimleri ve uniform yorumları MtkLayout'tan alınır;
 * layout == NULL ise her skaler varying ayrı @location(k) f32 olarak bildirilir.
 * Başarıda yazılan karakter sayısını (NUL hariç) döner; hata durumunda 0 döner ve err doldurulur. */
u32 mtk_wgsl_frag(const IrFunc* f, const MtkLayout* layout, const MtkWgslOpts* opts,
                  char* out, u32 out_cap, char* err, u32 err_cap);

/* gfx3d varying sözleşmesine (mvp, model, normal_mat, shadow_mat + attrs[0..attr_count-1])
 * uyan NUL sonlu WGSL vertex shader metni üretir.
 * Başarıda yazılan karakter sayısını (NUL hariç) döner; hata durumunda 0 döner ve err doldurulur. */
u32 mtk_wgsl_vert(const u8* attrs, u32 attr_count,
                  char* out, u32 out_cap, char* err, u32 err_cap);

/* Optimizasyon sonrası compute IrFunc'tan (@compute @workgroup_size(64)) NUL sonlu WGSL üretir.
 * Başarıda yazılan karakter sayısını (NUL hariç) döner; hata durumunda 0 döner ve err doldurulur. */
u32 mtk_wgsl_compute(const IrFunc* f, const MtkLayout* layout, const MtkWgslOpts* opts,
                     char* out, u32 out_cap, char* err, u32 err_cap);

/* mtk_compile_ir + ir_optimize + mtk_wgsl_frag/compute -> NUL sonlu WGSL shader metni.
 * Başarıda yazılan karakter sayısını (NUL hariç) döner; hata durumunda 0 döner ve diag doldurulur. */
u32 mtk_compile_wgsl(const char* src, u32 len, const char* filename, const MtkWgslOpts* o,
                     Arena* a, char* out, u32 out_cap, MtkLayout* layout, MtkDiag* diag);

#endif
