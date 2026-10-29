/* gpu/gpu_fluid.h — GPU Akışkan / Parçacık Fiziği (Position Based Fluids — Macklin 2013, P1_F).
 * Matoki @compute -> WGSL çekirdekleri ile uzamsal hash ızgarası (counting sort + atomic_add +
 * 3-seviye prefix sum), yoğunluk kısıtı (lambda_i, delta_p_i + s_corr), girdap hapsi + XSPH
 * viskozite (256k parçacık) ve 4 geçişli ekran-uzayı akışkan çizimi (SSFR).
 * Görsel determinizm sınıfı (D-112): GPU, deterministik olmayan.
 */
#ifndef GPU_FLUID_H
#define GPU_FLUID_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gpu/rhi.h"

#define GPU_FLUID_PARTICLES         262144u
#define GPU_FLUID_GRID_DIM          64u
#define GPU_FLUID_GRID_CELLS        262144u
#define GPU_FLUID_L1_BLOCKS         4096u
#define GPU_FLUID_L2_BLOCKS         64u
#define GPU_FLUID_WORKGROUP_SIZE    64u
#define GPU_FLUID_GROUPS_PARTICLES  (GPU_FLUID_PARTICLES / GPU_FLUID_WORKGROUP_SIZE)
#define GPU_FLUID_GROUPS_L1         (GPU_FLUID_L1_BLOCKS / GPU_FLUID_WORKGROUP_SIZE)
#define GPU_FLUID_GROUPS_L2         (GPU_FLUID_L2_BLOCKS / GPU_FLUID_WORKGROUP_SIZE)

/* Katı cisim (rigid body) ve kova (container) etkileşim parametreleri (tek yönlü eşleşme) */
typedef struct {
    Vec3 box_min;       /* Kova sol-alt-arka iç sınırı (m) */
    Vec3 box_max;       /* Kova sağ-üst-ön iç sınırı (m) */
    Vec3 sphere0_pos;   /* Birincil etkileşimli küre merkezi (m) */
    f32  sphere0_rad;   /* Birincil küre yarıçapı (m) */
    Vec3 sphere0_vel;   /* Birincil küre hızı (m/s) */
    Vec3 sphere1_pos;   /* İkincil şamandıra küre merkezi (m) */
    f32  sphere1_rad;   /* İkincil küre yarıçapı (m) */
    Vec3 box_obs_pos;   /* Kova tabanındaki sütun engel merkezi (m) */
    f32  box_obs_rad;   /* Sütun engel yarıçapı (m) */
} GpuFluidSceneParams;

/* PBF + SSFR akışkan yapılandırması */
typedef struct {
    f32 rest_density;       /* Dinlenme yoğunluğu (kg/m^3, varsayılan 1000.0) */
    f32 smoothing_radius;   /* Çekirdek yarıçapı h (m, varsayılan 0.065) */
    f32 cell_size;          /* Uzamsal ızgara hücre boyutu (m, varsayılan 0.050) */
    f32 eps_cfm;            /* CFM gevşetme parametresi epsilon (varsayılan 600.0) */
    f32 scorr_k;            /* Çekme kararsızlığı katsayısı k (varsayılan -0.00008) */
    f32 xsph_c;             /* XSPH viskozite katsayısı c (varsayılan 0.035) */
    f32 vorticity_eps;      /* Girdap hapsi katsayısı (varsayılan 0.35) */
    f32 particle_radius;    /* SSFR ekran-uzayı küre yarıçapı (m, varsayılan 0.032) */
    u32 solver_iterations;  /* Kısıt çözücü iterasyon sayısı (1..2, varsayılan 2) */
} GpuFluidDesc;

/* Kare başına çizim ve simülasyon parametreleri */
typedef struct {
    Mat4                view;
    Mat4                proj;
    Vec3                cam_pos;
    f32                 time_sec;
    f32                 dt;
    b32                 reset_particles;
    GpuFluidDesc        desc;
    GpuFluidSceneParams scene;
} GpuFluidFrameParams;

/* GPU Akışkan RHI çalışma zamanı durumu */
typedef struct {
    b32 initialized;
    b32 particles_seeded;
    u32 frame_index;
    u32 fb_w;
    u32 fb_h;
    u32 ssfr_w;
    u32 ssfr_h;

    GpuFluidDesc        desc;
    GpuFluidSceneParams scene;

    /* Parçacık ve uzamsal hash ızgarası GPU tamponları (262.144 parçacık, 64^3 hücre) */
    RhiBuffer pos_buf;              /* 262144 * vec4 (x, y, z, foam) */
    RhiBuffer vel_buf;              /* 262144 * vec4 (vx, vy, vz, speed_norm) */
    RhiBuffer pred_pos_buf;         /* 262144 * vec4 (tahmini/güncel konum) */
    RhiBuffer sorted_pos_buf;       /* 262144 * vec4 (hücre-sıralı tahmini konum) */
    RhiBuffer sorted_orig_pos_buf;  /* 262144 * vec4 (hücre-sıralı önceki konum) */
    RhiBuffer sorted_vel_buf;       /* 262144 * vec4 (hücre-sıralı hız) */
    RhiBuffer lambda_buf;           /* 262144 * vec4 (lambda_i, rho_ratio, C_i, 0) */

    RhiBuffer cell_counts_buf;      /* 262144 * u32 (atomik hücre sayaçları) */
    RhiBuffer cell_offsets_buf;     /* 262144 * u32 (prefix-sum başlangıç indisleri) */
    RhiBuffer l1_sums_buf;          /* 4096 * u32 (Seviye-1 blok toplamları) */
    RhiBuffer l1_offsets_buf;       /* 4096 * u32 (Seviye-1 blok ofsetleri) */
    RhiBuffer l2_sums_buf;          /* 64 * u32 (Seviye-2 blok toplamları) */
    RhiBuffer l2_offsets_buf;       /* 64 * u32 (Seviye-2 blok ofsetleri) */
    RhiBuffer dummy_sum_buf;        /* 16 * u32 (Seviye-2 üst toplam çıkışı) */
    RhiBuffer particle_cell_buf;    /* 262144 * u32 (parçacığın hücre indisi) */
    RhiBuffer particle_rank_buf;    /* 262144 * u32 (hücre içi atomik sıra) */

    /* Compute üniform tamponları */
    RhiBuffer init_ubuf;            /* pbf_init.mtk üniformları (48 bayt) */
    RhiBuffer clear_ubuf;           /* pbf_grid_clear.mtk üniformları (16 bayt) */
    RhiBuffer scan_l0_ubuf;         /* pbf_scan_block L0 (16 bayt) */
    RhiBuffer scan_l1_ubuf;         /* pbf_scan_block L1 (16 bayt) */
    RhiBuffer scan_l2_ubuf;         /* pbf_scan_block L2 (16 bayt) */
    RhiBuffer sim_ubuf;             /* pbf_* ana PBF üniformları (176 bayt) */

    /* 9 Matoki compute shader + pipeline + bind group */
    RhiShader cs_init;
    RhiShader cs_grid_clear;
    RhiShader cs_predict_hash;
    RhiShader cs_scan_block;
    RhiShader cs_scan_combine;
    RhiShader cs_scatter;
    RhiShader cs_lambda;
    RhiShader cs_delta;
    RhiShader cs_vorticity_xsph;

    RhiComputePipeline pipe_init;
    RhiComputePipeline pipe_grid_clear;
    RhiComputePipeline pipe_predict_hash;
    RhiComputePipeline pipe_scan_block;
    RhiComputePipeline pipe_scan_combine;
    RhiComputePipeline pipe_scatter;
    RhiComputePipeline pipe_lambda;
    RhiComputePipeline pipe_delta;
    RhiComputePipeline pipe_vorticity_xsph;

    RhiBindGroup bg_init;
    RhiBindGroup bg_grid_clear;
    RhiBindGroup bg_predict_hash;
    RhiBindGroup bg_scan_l0;
    RhiBindGroup bg_scan_l1;
    RhiBindGroup bg_scan_l2;
    RhiBindGroup bg_scan_combine;
    RhiBindGroup bg_scatter;
    RhiBindGroup bg_lambda_a;       /* sorted_pos -> lambda */
    RhiBindGroup bg_delta_a;        /* sorted_pos + lambda -> pred_pos */
    RhiBindGroup bg_lambda_b;       /* pred_pos -> lambda (2. iterasyon) */
    RhiBindGroup bg_delta_b;        /* pred_pos + lambda -> sorted_pos (2. iterasyon) */
    RhiBindGroup bg_vort_from_pred; /* pred_pos -> pos_buf + vel_buf (1 iterasyon sonu) */
    RhiBindGroup bg_vort_from_sort; /* sorted_pos -> pos_buf + vel_buf (2 iterasyon sonu) */

    /* SSFR (Screen-Space Fluid Rendering) ara dokuları ve grafik hatları */
    RhiSampler linear_sampler;
    RhiTexture scene_bg_tex;        /* RGBA16_FLOAT (fb_w x fb_h): katı sahne renk + doğrusal derinlik (.a) */
    RhiTexture fluid_depth_tex;     /* RGBA16_FLOAT (ssfr_w x ssfr_h): sıvı küre derinlik + köpük + kalınlık */
    RhiTexture fluid_zbuf_tex;      /* DEPTH24_PLUS (ssfr_w x ssfr_h): donanım Z-testi */
    RhiTexture fluid_blur_tmp_tex;  /* RGBA16_FLOAT (ssfr_w x ssfr_h): yatay bilateral filtre çıkışı */
    RhiTexture fluid_smooth_tex;    /* RGBA16_FLOAT (ssfr_w x ssfr_h): dikey bilateral filtre çıkışı */
    RhiTexture main_depth_tex;      /* DEPTH24_PLUS (fb_w x fb_h): swapchain derinlik eki */

    RhiBuffer ssfr_ubuf;            /* Sahne + kamera + SSFR gölgeleme üniformları (320 bayt) */
    RhiBuffer blur_h_ubuf;          /* Yatay bilateral filtre yönü (16 bayt) */
    RhiBuffer blur_v_ubuf;          /* Dikey bilateral filtre yönü (16 bayt) */

    RhiShader sh_bg;
    RhiShader sh_depth;
    RhiShader sh_blur;
    RhiShader sh_comp;

    RhiPipeline pipe_bg;
    RhiPipeline pipe_depth;
    RhiPipeline pipe_blur;
    RhiPipeline pipe_comp;

    RhiBindGroup bg_pass_bg;
    RhiBindGroup bg_pass_depth;
    RhiBindGroup bg_pass_blur_h;
    RhiBindGroup bg_pass_blur_v;
    RhiBindGroup bg_pass_comp;
} GpuFluid;

/* Varsayılan PBF + SSFR ve sahne parametrelerini doldurur. */
GpuFluidDesc        gpu_fluid_default_desc(void);
GpuFluidSceneParams gpu_fluid_default_scene(void);

/* 9 Matoki compute çekirdeğini WGSL'e derler, 256k parçacık tamponlarını ve SSFR hatlarını kurar. */
b32  gpu_fluid_init(GpuFluid* fl, Rhi* rhi, Arena* scratch, const GpuFluidDesc* desc);

/* Parçacıkları başlangıç baraj yıkılması (dam-break) dizilimine sıfırlamak üzere işaretler. */
void gpu_fluid_reset_particles(GpuFluid* fl);

/* Verilen Rhi bağlamına PBF compute + 4 geçişli SSFR komutlarını kodlar. */
b32  gpu_fluid_encode(GpuFluid* fl, Rhi* rhi, const GpuFluidFrameParams* p, u32 fb_w, u32 fb_h);

/* Tam çözünürlüklü GPU akışkan karesini aktif RHI komut akışına kodlar ve sunar. */
b32  gpu_fluid_render(Arena* scratch, const GpuFluidFrameParams* p);

/* Birim testi yardımcısı: Matoki compute IR yorumlayıcısı (ir_eval_compute) ile küçük bir parçacık
 * kümesinde tam PBF adımını (hash + scan + scatter + lambda + delta + vorticity/XSPH) çalıştırır. */
b32  gpu_fluid_test_step_cpu(Arena* scratch, Vec4* positions, Vec4* velocities, u32 count,
                             const GpuFluidSceneParams* scene, f32 dt,
                             f32* out_max_density_err, u32* out_sorted_total);

#endif /* GPU_FLUID_H */
