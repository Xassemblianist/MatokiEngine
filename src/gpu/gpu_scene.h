/* gpu/gpu_scene.h — GPU'da yaşayan 3D sahne, kirli liste (dirty list), compute frustum + Hi-Z
 * occlusion culling, dolaylı çizim (drawIndexedIndirect), 3-kademeli CSM gölge haritası ve
 * 3D kümelenmiş ileri aydınlatma (clustered forward lighting) + PBR WebGPU arka ucu (G1_B / H5).
 * WebGPU aktifken 5:3D (100k örnek stres modu dahil), 7:Ocak ve 8:Kuyu sahnelerini GPU'da çizer;
 * WebGPU yokken mevcut CPU rasterizer yoluna dokunmaz.
 */
#ifndef GPU_SCENE_H
#define GPU_SCENE_H

#include "core/types.h"
#include "core/math.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "gfx3d/mesh.h"
#include "gfx3d/camera.h"
#include "gpu/rhi.h"

#define GPU_SCENE_MAX_INSTANCES   102400u
#define GPU_SCENE_BENCH_INSTANCES 100000u
#define GPU_SCENE_DYN_CAPACITY    2400u
#define GPU_SCENE_MAX_MESHES      256u
#define GPU_SCENE_MAX_BATCHES     256u
#define GPU_SCENE_MAX_TEXTURES    16u
#define GPU_SCENE_MAX_LIGHTS      64u

#define GPU_SCENE_CLUSTER_X       16u
#define GPU_SCENE_CLUSTER_Y       8u
#define GPU_SCENE_CLUSTER_Z       8u
#define GPU_SCENE_CLUSTER_COUNT   (GPU_SCENE_CLUSTER_X * GPU_SCENE_CLUSTER_Y * GPU_SCENE_CLUSTER_Z)
#define GPU_SCENE_CLUSTER_STRIDE  16u /* [0]=count, [1..15]=light indices */

#define GPU_SCENE_CSM_CASCADES    3u
#define GPU_SCENE_CSM_SIZE        1024u
#define GPU_SCENE_CSM_ATLAS_W     (GPU_SCENE_CSM_SIZE * GPU_SCENE_CSM_CASCADES)
#define GPU_SCENE_CSM_ATLAS_H     GPU_SCENE_CSM_SIZE

#define GPU_SCENE_HIZ_W           128u
#define GPU_SCENE_HIZ_H           64u
#define GPU_SCENE_HIZ_MIPS        8u
#define GPU_SCENE_HIZ_TOTAL_TEXELS 10923u /* 128*64 + 64*32 + 32*16 + 16*8 + 8*4 + 4*2 + 2*1 + 1*1 */

typedef enum {
    GPU_SHADE_PBR      = 0u,
    GPU_SHADE_LIT      = 1u,
    GPU_SHADE_UNLIT    = 2u,
    GPU_SHADE_DUNGEON  = 3u,
    GPU_SHADE_PARTICLE = 4u
} GpuShaderMode;

typedef enum {
    GPU_INST_ACTIVE      = 1u << 0,
    GPU_INST_CAST_SHADOW = 1u << 1,
    GPU_INST_RECV_SHADOW = 1u << 2,
    GPU_INST_VIEWMODEL   = 1u << 3,
    GPU_INST_OCCLUDER    = 1u << 4
} GpuInstFlags;

#define GPU_MAT_TEX_PACK(metal_u8, rough_u8, tex_id, flags, shade_mode) \
    (((u32)(metal_u8) & 0xFFu) | \
     (((u32)(rough_u8) & 0xFFu) << 8) | \
     (((u32)(tex_id) & 0xFFu) << 16) | \
     ((((u32)(flags) & 0x1Fu) | (((u32)(shade_mode) & 0x7u) << 5)) << 24))

static inline u32 gpu_pack_rgba(Vec4 c) {
    u32 r = (u32)(m_clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 g = (u32)(m_clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 b = (u32)(m_clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 a = (u32)(m_clamp(c.w, 0.0f, 1.0f) * 255.0f + 0.5f);
    return FB_RGBA(r, g, b, a);
}

typedef struct {
    f32 r0[4];        /* Dünya dönüşümü 0. satır: (m00, m01, m02, tx) */
    f32 r1[4];        /* Dünya dönüşümü 1. satır: (m10, m11, m12, ty) */
    f32 r2[4];        /* Dünya dönüşümü 2. satır: (m20, m21, m22, tz) */
    u32 color;        /* RGBA8 taban renk (FB_RGBA paketlemesi) */
    u32 mat_tex;      /* GPU_MAT_TEX_PACK(metallic, roughness, tex_id, flags, shade_mode) */
    u32 mesh_batch;   /* bit 0..15: mesh_id, bit 16..31: batch_id */
    f32 bound_radius; /* Dünya uzayı sınır küresi yarıçapı */
} GpuInstance;

_Static_assert(sizeof(GpuInstance) == 64, "GpuInstance 64 bayt olmali");

typedef struct {
    f32 pos[3];
    f32 radius;
    f32 color[3];
    f32 atten;
} GpuPointLight;

_Static_assert(sizeof(GpuPointLight) == 32, "GpuPointLight 32 bayt olmali");

typedef struct {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    i32 base_vertex;
    u32 first_instance;
} GpuIndirectCommand;

_Static_assert(sizeof(GpuIndirectCommand) == 20, "GpuIndirectCommand 20 bayt olmali");

typedef struct {
    Mat4 view;
    Mat4 proj;
    Mat4 view_proj;
    Mat4 vm_view_proj;
    Vec3 eye;
    Vec3 cam_fwd;
    f32  near_z;
    f32  far_z;
    f32  fov_y;
    f32  aspect;
    Vec3 sun_dir;
    Vec3 sun_color;
    Vec3 sky_color;
    Vec3 ground_color;
    Vec3 fog_color;
    f32  fog_start;
    f32  fog_end;
    Vec3 clear_color;
    b32  shadows_on;
    b32  pbr_on;
    b32  mipmap_on;
    b32  retro_on;
    u32  snap_subpixel;
    f32  retro_scanline;
    b32  stress_100k;
    f32  time_sec;
} GpuSceneFrameDesc;

typedef struct {
    u32 active_instances;
    u32 dirty_instances;
    u32 dirty_spans;
    u32 dirty_bytes;
    u32 visible_instances;
    u32 frustum_culled;
    u32 hiz_culled;
    u32 batch_count;
    u32 light_count;
    u32 csm_cascades;
    u32 cmd_bytes;
    u32 cmd_count;
    f32 cpu_submit_ms;
    u32 gpu_frame_us;
} GpuSceneStats;

/* GPU sahnesini hazırlar / sıfırlar. */
b32  gpu_scene_init(void);
void gpu_scene_reset_instances(void);

/* Kalıcı örnek tablosunu günceller; yalnızca değişen yuvaları kirli listeye (dirty list) ekler.
 * Dönüş: yuva değiştiyse ve kirli işaretlendiyse TRUE, önceki değerle bit-bit aynıysa FALSE. */
b32  gpu_scene_set_instance(u32 slot_idx, const GpuInstance* inst);
u32  gpu_scene_dirty_count(void);
u32  gpu_scene_flush_dirty(Rhi* rhi, RhiBuffer dst_buf, u32* out_spans, u32* out_bytes);

/* Ağ ve doku kaydı (GPU önbelleğinde yoksa yükler, varsa mevcut kimliği döner) */
u32  gpu_scene_register_mesh(const Mesh* m, b32 is_dynamic, b32 is_alpha);
u32  gpu_scene_register_texture(const Texture* tex);

/* Kare çizim akışı (r3d ve scene_kuyu tarafından çağrılır) */
b32  gpu_scene_begin_frame(const GpuSceneFrameDesc* desc);
void gpu_scene_add_point_light(Vec3 pos, f32 radius, Vec3 color, f32 atten);
b32  gpu_scene_submit_draw(const Mesh* m, const Mat4* model, u32 color,
                           const Texture* tex, f32 metallic, f32 roughness,
                           GpuShaderMode shade_mode, u32 inst_flags, b32 is_dynamic_mesh);
u32  gpu_scene_end_frame(void);

const GpuSceneStats* gpu_scene_stats(void);

/* 100k örnek stres modu kontrolü (scene3d ve dışa aktarılan JS kıyaslama kancası) */
void gpu_scene_set_100k_mode(b32 enable);
b32  gpu_scene_100k_mode(void);

/* --- CPU Referans Fonksiyonları (birim testleri ve doğrulama için WGSL ile birebir aynı matematik) --- */
void gpu_scene_extract_frustum_planes(Mat4 view_proj, Vec4 out_planes[6]);
void gpu_scene_fit_csm(Vec3 cam_pos, Vec3 cam_fwd, Vec3 sun_dir,
                       Mat4 out_vp[GPU_SCENE_CSM_CASCADES],
                       Mat4 out_tex[GPU_SCENE_CSM_CASCADES],
                       f32  out_radii[GPU_SCENE_CSM_CASCADES]);
void gpu_scene_hiz_build_cpu_ref(f32* hiz_mips, Vec4 occluder_ndc_box, f32 occluder_depth);
u32  gpu_scene_cull_cpu_ref(const GpuInstance* instances, u32 count,
                            const Vec4* mesh_bounds, const Vec4 frustum_planes[6],
                            Mat4 view, f32 near_z, f32 cot_half_fov, f32 aspect,
                            const f32* hiz_mips,
                            GpuIndirectCommand* indirect_cmds, u32 batch_count,
                            const u32* batch_first_inst, const u32* batch_max_inst,
                            u32* visible_out, u32* out_frustum_culled, u32* out_hiz_culled);
void gpu_scene_cluster_lights_cpu_ref(const GpuPointLight* lights, u32 light_count,
                                      Mat4 view, f32 near_z, f32 far_z,
                                      f32 cot_half_fov, f32 aspect,
                                      u32* cluster_out);

#endif
