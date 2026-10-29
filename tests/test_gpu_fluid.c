/* tests/test_gpu_fluid.c — P1_F GPU akışkan / parçacık fiziği (PBF + SSFR) birim testleri.
 * 9 adet Matoki @compute çekirdeğinin (shaders/compute/pbf_*.mtk) IR + Wasm SIMD + WGSL
 * derleme doğrulaması, ir_eval_compute ile PBF adımı (3B hücre sayma sıralaması, yoğunluk
 * kısıtı, s_corr, katı cisim çarpışması, girdap hapsi + XSPH) ve RHI komut akışı testi.
 */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "gpu/rhi.h"
#include "gpu/gpu_fluid.h"
#include "matoki/matoki.h"

static u8 g_fluid_test_mem[MB(64)];

static const char* k_pbf_shader_paths[9] = {
    "shaders/compute/pbf_init.mtk",
    "shaders/compute/pbf_grid_clear.mtk",
    "shaders/compute/pbf_predict_hash.mtk",
    "shaders/compute/pbf_scan_block.mtk",
    "shaders/compute/pbf_scan_combine.mtk",
    "shaders/compute/pbf_scatter.mtk",
    "shaders/compute/pbf_lambda.mtk",
    "shaders/compute/pbf_delta.mtk",
    "shaders/compute/pbf_vorticity_xsph.mtk",
};

/* 1) Tüm PBF Matoki @compute gölgelendiricilerinin IR + Wasm SIMD + WGSL derleme doğrulaması */
TEST(gpu_fluid_compute_shaders_compile) {
    Arena a;
    arena_init(&a, g_fluid_test_mem, sizeof(g_fluid_test_mem));

    for (u32 i = 0u; i < 9u; i++) {
        usize mark = arena_mark(&a);

        /* Diskteki shaders/compute/pbf_*.mtk dosyasını oku */
        FILE* f = fopen(k_pbf_shader_paths[i], "rb");
        CHECK(f != NULL);
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        CHECK(fsize > 64 && fsize < 32768);
        char* disk_src = ARENA_PUSH_ARRAY(&a, char, (u32)fsize + 1u);
        usize nread = fread(disk_src, 1, (usize)fsize, f);
        fclose(f);
        disk_src[nread] = '\0';
        CHECK(nread == (usize)fsize);

        /* IR derleme + doğrulama + eniyileme */
        IrFunc ir;
        MtkLayout layout;
        MtkDiag diag;
        __builtin_memset(&ir, 0, sizeof(ir));
        __builtin_memset(&layout, 0, sizeof(layout));
        mtk_diag_init(&diag);

        CHECK(mtk_compile_ir(disk_src, (u32)nread, k_pbf_shader_paths[i], &a, &ir, &layout, &diag));
        CHECK(ir.is_compute == TRUE);
        CHECK(layout.is_compute == TRUE);
        /* RHI_MAX_BINDINGS = 8 sınırına uyum: 1 uniform + en çok 7 storage buffer */
        CHECK(layout.buffer_count >= 1u && layout.buffer_count <= 7u);

        char verr[128];
        CHECK(ir_verify(&ir, verr, sizeof(verr)));
        ir_optimize(&ir);
        CHECK(ir_verify(&ir, verr, sizeof(verr)));

        /* Wasm SIMD kod üretimi */
        u8* wasm_buf = ARENA_PUSH_ARRAY(&a, u8, 131072u);
        MtkCodegenOpts opts = { FALSE, FALSE, MTK_FUSE_NONE };
        char cerr[256];
        u32 wasm_len = mtk_codegen(&ir, &opts, &a, wasm_buf, 131072u, cerr, sizeof(cerr));
        CHECK(wasm_len > 64u);

        /* WGSL kod üretimi */
        char* wgsl_disk = ARENA_PUSH_ARRAY(&a, char, 131072u);
        u32 wlen_disk = mtk_compile_wgsl(disk_src, (u32)nread, k_pbf_shader_paths[i],
                                         NULL, &a, wgsl_disk, 131072u, &layout, &diag);
        CHECK(wlen_disk > 64u);
        CHECK(strstr(wgsl_disk, "@compute @workgroup_size(64)") != NULL);

        arena_pop_to(&a, mark);
    }
}

/* 2) PBF tam adımının (sayma sıralaması + yoğunluk kısıtı + katı cisim + girdap/XSPH) doğrulaması */
TEST(gpu_fluid_pbf_cpu_eval) {
    Arena a;
    arena_init(&a, g_fluid_test_mem, sizeof(g_fluid_test_mem));

    /* (0.0, 0.50, 0.0) ızgara köşesi etrafındaki 8 hücrede 2'şer parçacık (toplam 16 parçacık).
     * Her hücrede <= 3 parçacık olduğu için komşu arama sınırı doyuma ulaşmaz ve sıkışık küme
     * (r ~ 0.012..0.035 < h = 0.065) PBF yoğunluk kısıtıyla (C_i > 0) dışa doğru genişler. */
    const u32 count = 16u;
    Vec4 pos[16];
    Vec4 vel[16];
    for (u32 oct = 0u; oct < 8u; oct++) {
        f32 sx = (oct & 1u) ? 1.0f : -1.0f;
        f32 sy = (oct & 2u) ? 1.0f : -1.0f;
        f32 sz = (oct & 4u) ? 1.0f : -1.0f;
        pos[oct * 2u + 0u] = vec4(sx * 0.006f, 0.50f + sy * 0.006f, sz * 0.006f, 0.0f);
        pos[oct * 2u + 1u] = vec4(sx * 0.014f, 0.50f + sy * 0.014f, sz * 0.014f, 0.0f);
        vel[oct * 2u + 0u] = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        vel[oct * 2u + 1u] = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    /* Başlangıç ortalama çiftler-arası uzaklığı ölç */
    f32 dist_sum_before = 0.0f;
    u32 pair_cnt = 0u;
    for (u32 i = 0u; i < count; i++) {
        for (u32 j = i + 1u; j < count; j++) {
            f32 dx = pos[i].x - pos[j].x;
            f32 dy = pos[i].y - pos[j].y;
            f32 dz = pos[i].z - pos[j].z;
            dist_sum_before += m_sqrt(dx * dx + dy * dy + dz * dz);
            pair_cnt++;
        }
    }
    f32 avg_dist_before = dist_sum_before / (f32)pair_cnt;

    GpuFluidSceneParams scene = gpu_fluid_default_scene();
    /* Engeller uzakta iken yoğunluk kısıtının aşırı sıkışmış kümeyi genişlettiğini doğrula */
    scene.sphere0_pos = vec3(-0.90f, 0.55f, 0.0f);
    scene.sphere0_rad = 0.14f;
    scene.sphere0_vel = vec3( 0.0f,  0.0f,  0.0f);
    scene.sphere1_pos = vec3( 0.90f, 0.55f, 0.0f);
    scene.sphere1_rad = 0.10f;
    scene.box_obs_pos = vec3( 0.90f, 0.05f, 0.90f);
    scene.box_obs_rad = 0.10f;

    f32 max_dens_err_0 = 0.0f, max_dens_err_1 = 0.0f;
    u32 sorted_total = 0u;
    CHECK(gpu_fluid_test_step_cpu(&a, pos, vel, count, &scene, 0.006f,
                                  &max_dens_err_0, &sorted_total));
    CHECK(sorted_total == count);
    CHECK(gpu_fluid_test_step_cpu(&a, pos, vel, count, &scene, 0.006f,
                                  &max_dens_err_1, &sorted_total));
    CHECK(sorted_total == count);

    f32 dist_sum_after = 0.0f;
    for (u32 i = 0u; i < count; i++) {
        for (u32 j = i + 1u; j < count; j++) {
            f32 dx = pos[i].x - pos[j].x;
            f32 dy = pos[i].y - pos[j].y;
            f32 dz = pos[i].z - pos[j].z;
            dist_sum_after += m_sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    f32 avg_dist_after = dist_sum_after / (f32)pair_cnt;
    CHECK(avg_dist_after > avg_dist_before);
    CHECK(max_dens_err_1 < max_dens_err_0);

    /* Şimdi küre 0'ı kümenin sol kenarına getir ve +X yönünde hızla hareket ettir */
    scene.sphere0_pos = vec3(pos[0].x - 0.08f, pos[0].y, pos[0].z);
    scene.sphere0_rad = 0.14f;
    scene.sphere0_vel = vec3(2.80f, 0.0f, 0.0f);
    CHECK(gpu_fluid_test_step_cpu(&a, pos, vel, count, &scene, 0.006f,
                                  &max_dens_err_1, &sorted_total));
    CHECK(sorted_total == count);

    u32 pushed_by_sphere = 0u;
    for (u32 i = 0u; i < count; i++) {
        /* Sonlu ve kova sınırları içinde olmalı */
        CHECK(pos[i].x == pos[i].x && pos[i].y == pos[i].y && pos[i].z == pos[i].z);
        CHECK(vel[i].x == vel[i].x && vel[i].y == vel[i].y && vel[i].z == vel[i].z);
        CHECK(pos[i].x >= scene.box_min.x && pos[i].x <= scene.box_max.x);
        CHECK(pos[i].y >= scene.box_min.y && pos[i].y <= scene.box_max.y);
        CHECK(pos[i].z >= scene.box_min.z && pos[i].z <= scene.box_max.z);

        /* Küre 0'ın içinden dışarı itilmiş olmalı */
        f32 dx0 = pos[i].x - scene.sphere0_pos.x;
        f32 dy0 = pos[i].y - scene.sphere0_pos.y;
        f32 dz0 = pos[i].z - scene.sphere0_pos.z;
        f32 d0  = m_sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0);
        CHECK(d0 >= scene.sphere0_rad - 1e-3f);
        if (vel[i].x > 0.20f) pushed_by_sphere++;
    }
    CHECK(pushed_by_sphere > 0u);
}

/* 3) RHI komut akışı kodlama testi (256k PBF compute + 4 geçişli SSFR render) */
TEST(gpu_fluid_rhi_stream_encode) {
    Arena a;
    arena_init(&a, g_fluid_test_mem, sizeof(g_fluid_test_mem));

    engine_gpu_set_available(1u);
    engine_gpu_set_viewport_size(1920u, 1080u);
    engine_gpu_clear_cmds();

    GpuFluidFrameParams fp;
    __builtin_memset(&fp, 0, sizeof(fp));
    fp.view            = mat4_look_at(vec3(1.8f, 1.6f, 2.5f), vec3(0.0f, 0.6f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    fp.proj            = mat4_perspective(52.0f * M_DEG2RAD, 16.0f / 9.0f, 0.1f, 45.0f);
    fp.cam_pos         = vec3(1.8f, 1.6f, 2.5f);
    fp.time_sec        = 1.0f;
    fp.dt              = 0.0083f;
    fp.reset_particles = TRUE;
    fp.desc            = gpu_fluid_default_desc();
    fp.scene           = gpu_fluid_default_scene();

    CHECK(gpu_fluid_render(&a, &fp));
    CHECK(engine_gpu_cmd_bytes() > 1000u);

    engine_gpu_clear_cmds();
    engine_gpu_set_available(0u);
}
