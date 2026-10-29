/* gpu/gpu_ocean.h — "Okyanus" sahnesinin tam çözünürlüklü WebGPU hesaplama + çizim hattı (G1_A).
 * 3 kademeli Tessendorf FFT'yi Matoki @compute -> WGSL çekirdekleriyle GPU'da koşturup
 * sonuçları storage dokulara yazar; kamera merkezli geometrik ızgarayı köşe gölgelendiricisinde
 * dokulardan örnekleyerek yer değiştirir ve ocean.mtk + sky.mtk + tekne ağlarını MSAA 4x ile çizer.
 */
#ifndef GPU_OCEAN_H
#define GPU_OCEAN_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/ocean.h"
#include "gfx3d/mesh.h"
#include "raster/texture.h"

typedef struct {
    const Ocean*   ocean;
    f64            sim_time;
    f32            eff_dt;
    b32            spectrum_dirty;
    b32            first_update;
    Mat4           view;
    Mat4           proj;
    Vec3           eye;
    Mat4           boat_model;
    Vec3           sun_dir;
    Vec3           sun_col;
    Vec3           moon_dir;
    f32            star_intensity;
    f32            moon_intensity;
    f32            cloud_coverage;
    f32            cloud_wind_x;
    f32            cloud_wind_z;
    Mat4           prev_vp;
    Vec3           sky_zenith;
    Vec3           sky_horiz;
    Vec3           sea_deep;
    Vec3           sea_shallow;
    Vec3           fog_srgb;
    f32            fog_dens;
    f32            warm;
    u32            wireframe;
    const Mesh*    sky_mesh;
    const Mesh*    hull_mesh;
    const Mesh*    main_sail_mesh;
    const Mesh*    jib_sail_mesh;
    const Mesh*    unit_cube;
    const Texture* tex_hull;
    const Texture* tex_deck;
    const Texture* tex_sail;
    const char*    ocean_mtk_src;
    u32            ocean_mtk_len;
    const char*    sky_mtk_src;
    u32            sky_mtk_len;
    const char*    lit_mtk_src;
    u32            lit_mtk_len;
    const char*    unlit_mtk_src;
    u32            unlit_mtk_len;
} GpuOceanFrameParams;

/* Tam çözünürlüklü GPU okyanus karesini RHI komut akışına kodlar.
 * İlk çağrıda tüm GPU kaynaklarını (compute/render pipeline, storage doku, ağ, doku) oluşturur. */
b32 gpu_ocean_render(Arena* scratch, const GpuOceanFrameParams* p, u32* out_tris);

/* Birim testi yardımcısı: Matoki @compute FFT çekirdeklerini (spektrum, kelebek, açma)
 * derleyip ir_eval_compute ile koşturur ve CPU referans kademesiyle karşılaştırır. */
b32 gpu_ocean_test_compute_fft(Arena* scratch, const Ocean* ref_ocean, u32 cascade_idx,
                               f64 sim_time, f32* out_max_err);

/* Birim testi yardımcısı: V1 atmosfer + 3B bulut gürültüsü Matoki @compute çekirdeklerini
 * (transmittance, multiscatter, skyview, aerial, cloud_noise) derler ve fiziksel çıktılarını doğrular. */
b32 gpu_ocean_test_atmos_compute(Arena* scratch);

#endif /* GPU_OCEAN_H */
