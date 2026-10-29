/* tests/test_gpu_scene.c — G1_B (ARCH_V2 H5) GPU'da yaşayan sahne birim testleri:
 * dirty list delta yükleme, frustum düzlemi çıkarımı, 3 kademeli CSM matrisleri,
 * Hi-Z 8-mip piramidi + örtülme elemesi, 16x8x8 küme ışık ataması ve 100k nesne < 0.5 ms bütçesi.
 */
#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/camera.h"
#include "gfx3d/mesh.h"
#include "gpu/rhi.h"
#include "gpu/gpu_scene.h"

static GpuInstance make_test_inst(f32 x, f32 y, f32 z, f32 radius, u32 flags, u32 shade_mode) {
    GpuInstance d;
    __builtin_memset(&d, 0, sizeof(d));
    d.r0[0] = 1.0f; d.r0[3] = x;
    d.r1[1] = 1.0f; d.r1[3] = y;
    d.r2[2] = 1.0f; d.r2[3] = z;
    d.color = 0xFFFFFFFFu;
    d.mat_tex = GPU_MAT_TEX_PACK(0u, 128u, 0u, flags, shade_mode);
    d.mesh_batch = 0u;
    d.bound_radius = radius;
    return d;
}

/* 1. Dirty list ve delta instance yükleme testi */
TEST(gpu_scene_dirty_list) {
    CHECK(gpu_scene_init());
    gpu_scene_reset_instances();

    static u8 rhi_mem[KB(256)];
    Arena arena;
    arena_init(&arena, rhi_mem, sizeof(rhi_mem));
    Rhi rhi;
    CHECK(rhi_init(&rhi, &arena, 32u, KB(128)));
    RhiBuffer inst_buf = rhi_create_buffer(&rhi, 64u * (u32)sizeof(GpuInstance),
                                           RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    CHECK(inst_buf != RHI_INVALID_HANDLE);

    for (u32 i = 0u; i < 32u; i++) {
        GpuInstance d = make_test_inst((f32)i * 2.0f, 0.0f, 0.0f, 1.0f,
                                       GPU_INST_ACTIVE | GPU_INST_CAST_SHADOW, GPU_SHADE_PBR);
        CHECK(gpu_scene_set_instance(i, &d));
    }
    CHECK(gpu_scene_dirty_count() == 32u);

    /* İlk karede 32 nesnenin tamamı kirli (tek birleşik aralık olarak gönderilir) */
    rhi_begin_frame(&rhi);
    u32 spans1 = 0u, bytes1 = 0u;
    u32 flushed1 = gpu_scene_flush_dirty(&rhi, inst_buf, &spans1, &bytes1);
    CHECK(flushed1 == 32u);
    CHECK(spans1 == 1u);
    CHECK(bytes1 == 32u * (u32)sizeof(GpuInstance));
    CHECK(gpu_scene_dirty_count() == 0u);

    /* Aynı veriyi tekrar yazmak dirty bitini tetiklememeli */
    for (u32 i = 0u; i < 32u; i++) {
        GpuInstance d = make_test_inst((f32)i * 2.0f, 0.0f, 0.0f, 1.0f,
                                       GPU_INST_ACTIVE | GPU_INST_CAST_SHADOW, GPU_SHADE_PBR);
        CHECK(!gpu_scene_set_instance(i, &d));
    }
    rhi_begin_frame(&rhi);
    u32 spans2 = 0u, bytes2 = 0u;
    u32 flushed2 = gpu_scene_flush_dirty(&rhi, inst_buf, &spans2, &bytes2);
    CHECK(flushed2 == 0u);
    CHECK(spans2 == 0u);
    CHECK(bytes2 == 0u);

    /* Yalnızca 2 ayrık nesneyi (indeks 5 ve 21) değiştir */
    GpuInstance mod5 = make_test_inst(10.0f, 4.0f, 0.0f, 1.0f,
                                      GPU_INST_ACTIVE | GPU_INST_CAST_SHADOW, GPU_SHADE_PBR);
    GpuInstance mod21 = make_test_inst(42.0f, -3.0f, 0.0f, 1.0f,
                                       GPU_INST_ACTIVE | GPU_INST_CAST_SHADOW, GPU_SHADE_PBR);
    CHECK(gpu_scene_set_instance(5u, &mod5));
    CHECK(gpu_scene_set_instance(21u, &mod21));
    CHECK(gpu_scene_dirty_count() == 2u);

    rhi_begin_frame(&rhi);
    u32 spans3 = 0u, bytes3 = 0u;
    u32 flushed3 = gpu_scene_flush_dirty(&rhi, inst_buf, &spans3, &bytes3);
    CHECK(flushed3 == 2u);
    CHECK(spans3 == 2u);
    CHECK(bytes3 == 2u * (u32)sizeof(GpuInstance));
}

/* 2. Frustum düzlemleri ve 3 kademeli CSM matris testi */
TEST(gpu_scene_frustum_and_csm) {
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.0f, 2.0f, 10.0f);
    cam.yaw = 0.0f; /* yaw = 0 -> -Z yönüne bakış */
    cam.pitch = 0.0f;

    Mat4 view = camera_view(&cam);
    Mat4 proj = camera_proj(&cam, 16.0f / 9.0f);
    Mat4 vp   = mat4_mul(proj, view);

    Vec4 planes[6];
    gpu_scene_extract_frustum_planes(vp, planes);
    for (u32 i = 0u; i < 6u; i++) {
        f32 len = m_sqrt(planes[i].x * planes[i].x +
                         planes[i].y * planes[i].y +
                         planes[i].z * planes[i].z);
        CHECK_NEAR(len, 1.0f, 1e-4f);
    }

    /* Kameranın önündeki nokta (0, 2, 0) tüm 6 düzlemin iç tarafında olmalı */
    for (u32 i = 0u; i < 6u; i++) {
        f32 dist = planes[i].x * 0.0f + planes[i].y * 2.0f + planes[i].z * 0.0f + planes[i].w;
        CHECK(dist > -0.01f);
    }

    /* Kameranın arkasındaki nokta (0, 2, 25) en az bir düzlemin dışında olmalı */
    b32 outside = FALSE;
    for (u32 i = 0u; i < 6u; i++) {
        f32 dist = planes[i].x * 0.0f + planes[i].y * 2.0f + planes[i].z * 25.0f + planes[i].w;
        if (dist < -1.0f) outside = TRUE;
    }
    CHECK(outside);

    /* CSM 3 kademe kontrolü */
    Mat4 csm_vp[GPU_SCENE_CSM_CASCADES];
    Mat4 csm_tex[GPU_SCENE_CSM_CASCADES];
    f32  csm_radii[GPU_SCENE_CSM_CASCADES];
    gpu_scene_fit_csm(cam.pos, camera_forward(&cam), vec3(0.4f, 0.8f, 0.3f),
                      csm_vp, csm_tex, csm_radii);
    CHECK(csm_radii[0] < csm_radii[1]);
    CHECK(csm_radii[1] < csm_radii[2]);
}

/* 3. Hi-Z piramidi ve Frustum + Hi-Z örtülme elemesi (occlusion culling) testi */
TEST(gpu_scene_hiz_and_culling) {
    static f32 hiz_mips[10928];
    /* Ekranın ortasında [0.25..0.75]x[0.25..0.75] alanında reverse-Z derinliği 0.35 olan engel duvarı */
    gpu_scene_hiz_build_cpu_ref(hiz_mips, vec4(0.25f, 0.25f, 0.75f, 0.75f), 0.35f);

    /* Mip 0 merkez örnekleri 0.35, köşe örnekleri 0.0 olmalı */
    CHECK_NEAR(hiz_mips[32u * GPU_SCENE_HIZ_W + 64u], 0.35f, 1e-5f);
    CHECK_NEAR(hiz_mips[0u], 0.0f, 1e-5f);

    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.0f, 0.0f, 10.0f);
    cam.yaw = 0.0f; /* -Z yönü */
    cam.pitch = 0.0f;
    f32 aspect = 16.0f / 9.0f;
    Mat4 view = camera_view(&cam);
    Mat4 proj = camera_proj(&cam, aspect);
    Mat4 vp   = mat4_mul(proj, view);

    Vec4 planes[6];
    gpu_scene_extract_frustum_planes(vp, planes);

    Vec4 mesh_bounds[1] = { vec4(0.0f, 0.0f, 0.0f, 1.0f) };
    GpuInstance insts[4];
    /* Nesne 0: Engelin önünde (z = 9.2, dist = 0.8 -> reverse-Z near_z / (dist-r) > 0.35) -> GÖRÜNÜR */
    insts[0] = make_test_inst(0.0f, 0.0f, 9.2f, 0.3f, GPU_INST_ACTIVE, GPU_SHADE_PBR);
    /* Nesne 1: Engelin tam arkasında (z = -20.0, dist = 30.0 -> reverse-Z < 0.35) -> Hİ-Z ÖRTÜLÜ */
    insts[1] = make_test_inst(0.0f, 0.0f, -20.0f, 0.4f, GPU_INST_ACTIVE, GPU_SHADE_PBR);
    /* Nesne 2: Frustum dışında (x = 500.0) -> FRUSTUM ELENDİ */
    insts[2] = make_test_inst(500.0f, 0.0f, 0.0f, 1.0f, GPU_INST_ACTIVE, GPU_SHADE_PBR);
    /* Nesne 3: Viewmodel bayraklı (her zaman görünür) */
    insts[3] = make_test_inst(0.0f, 0.0f, 9.5f, 0.2f, GPU_INST_ACTIVE | GPU_INST_VIEWMODEL, GPU_SHADE_PBR);

    GpuIndirectCommand cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.index_count = 36u;
    u32 batch_first = 0u;
    u32 batch_max   = 16u;
    u32 vis_out[16];
    __builtin_memset(vis_out, 0, sizeof(vis_out));
    u32 f_culled = 0u, h_culled = 0u;

    u32 vis_count = gpu_scene_cull_cpu_ref(insts, 4u, mesh_bounds, planes, view,
                                           cam.near_z, proj.m[5], aspect, hiz_mips,
                                           &cmd, 1u, &batch_first, &batch_max,
                                           vis_out, &f_culled, &h_culled);
    CHECK(vis_count == 2u);
    CHECK(f_culled == 1u);
    CHECK(h_culled == 1u);
    CHECK(cmd.instance_count == 2u);
    CHECK(vis_out[0] == 0u);
    CHECK(vis_out[1] == 3u);
}

/* 4. 16x8x8 3D Küme (Clustered Forward) ışık atama testi */
TEST(gpu_scene_clustered_lighting) {
    static u32 cluster_buf[GPU_SCENE_CLUSTER_COUNT * GPU_SCENE_CLUSTER_STRIDE];
    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.0f, 0.0f, 10.0f);
    cam.yaw = 0.0f; /* -Z yönü */
    cam.pitch = 0.0f;
    f32 aspect = 16.0f / 9.0f;
    Mat4 view = camera_view(&cam);
    Mat4 proj = camera_proj(&cam, aspect);

    GpuPointLight lights[2];
    __builtin_memset(lights, 0, sizeof(lights));
    /* Işık 0: kameranın 5m önünde merkezde (0, 0, 5) */
    lights[0].pos[0] = 0.0f; lights[0].pos[1] = 0.0f; lights[0].pos[2] = 5.0f;
    lights[0].radius = 3.0f;
    lights[0].color[0] = 2.0f; lights[0].color[1] = 1.0f; lights[0].color[2] = 0.5f;
    lights[0].atten = 0.2f;

    /* Işık 1: kameranın arkasında (0, 0, 30) -> frustum kümesi dışında */
    lights[1].pos[0] = 0.0f; lights[1].pos[1] = 0.0f; lights[1].pos[2] = 30.0f;
    lights[1].radius = 2.0f;
    lights[1].color[0] = 1.0f; lights[1].color[1] = 1.0f; lights[1].color[2] = 1.0f;
    lights[1].atten = 0.2f;

    gpu_scene_cluster_lights_cpu_ref(lights, 2u, view, cam.near_z, 120.0f,
                                     proj.m[5], aspect, cluster_buf);

    u32 non_empty = 0u;
    b32 found_light0 = FALSE;
    b32 found_light1 = FALSE;
    for (u32 c = 0u; c < GPU_SCENE_CLUSTER_COUNT; c++) {
        u32 base = c * GPU_SCENE_CLUSTER_STRIDE;
        u32 cnt = cluster_buf[base];
        if (cnt > 0u) {
            non_empty++;
            for (u32 k = 0u; k < cnt; k++) {
                if (cluster_buf[base + 1u + k] == 0u) found_light0 = TRUE;
                if (cluster_buf[base + 1u + k] == 1u) found_light1 = TRUE;
            }
        }
    }
    CHECK(non_empty > 0u);
    CHECK(found_light0);
    CHECK(!found_light1);
}

/* 5. 100k nesne modu: RHI dolaylı çizim akışı ve < 0.5 ms CPU gönderim bütçesi */
TEST(gpu_scene_100k_stream_and_budget) {
    static u8 mesh_mem[KB(64)];
    Arena m_arena;
    arena_init(&m_arena, mesh_mem, sizeof(mesh_mem));
    Mesh* cube = mesh_cube(&m_arena, 0.5f);
    CHECK(cube != NULL);

    engine_gpu_set_available(1u);
    engine_gpu_set_viewport_size(1920u, 1080u);
    CHECK(gpu_scene_init());
    u32 mid = gpu_scene_register_mesh(cube, FALSE, FALSE);
    CHECK(mid == 0u);

    Camera cam;
    camera_init(&cam, CAM_FLY);
    cam.pos = vec3(0.0f, 6.0f, 18.0f);
    cam.yaw = 0.0f;
    cam.pitch = -0.15f;

    GpuSceneFrameDesc fdesc;
    __builtin_memset(&fdesc, 0, sizeof(fdesc));
    fdesc.view          = camera_view(&cam);
    fdesc.proj          = camera_proj(&cam, 16.0f / 9.0f);
    fdesc.view_proj     = mat4_mul(fdesc.proj, fdesc.view);
    fdesc.vm_view_proj  = fdesc.view_proj;
    fdesc.eye           = cam.pos;
    fdesc.cam_fwd       = camera_forward(&cam);
    fdesc.near_z        = cam.near_z;
    fdesc.far_z         = 220.0f;
    fdesc.fov_y         = cam.fov_y;
    fdesc.aspect        = 16.0f / 9.0f;
    fdesc.sun_dir       = vec3(0.4f, 0.8f, 0.3f);
    fdesc.sun_color     = vec3(2.4f, 2.2f, 1.9f);
    fdesc.sky_color     = vec3(0.25f, 0.35f, 0.55f);
    fdesc.ground_color  = vec3(0.12f, 0.10f, 0.08f);
    fdesc.fog_color     = vec3(0.5f, 0.6f, 0.75f);
    fdesc.fog_start     = 25.0f;
    fdesc.fog_end       = 160.0f;
    fdesc.clear_color   = fdesc.fog_color;
    fdesc.shadows_on    = TRUE;
    fdesc.pbr_on        = TRUE;
    fdesc.mipmap_on     = TRUE;
    fdesc.stress_100k   = TRUE;

    gpu_scene_set_100k_mode(TRUE);

    /* Isınma karesi (GPU kaynaklarının ve 100k compute başlatmasının çalışması) */
    fdesc.time_sec = 0.0f;
    CHECK(gpu_scene_begin_frame(&fdesc));
    gpu_scene_end_frame();

    /* Tam durağan kare (time_sec sabit -> 0 dirty örnek) */
    CHECK(gpu_scene_begin_frame(&fdesc));
    gpu_scene_end_frame();
    CHECK(gpu_scene_stats()->dirty_instances == 0u);

    /* Akan kareler: 100k nesne GPU'da yaşar, yalnızca 16 animasyonlu nesne dirty list ile güncellenir */
    f32 total_ms = 0.0f;
    for (u32 iter = 0u; iter < 5u; iter++) {
        fdesc.time_sec = (f32)(iter + 1u) * 0.016f;
        CHECK(gpu_scene_begin_frame(&fdesc));
        gpu_scene_end_frame();
        const GpuSceneStats* st = gpu_scene_stats();
        CHECK(st != NULL);
        CHECK(st->active_instances >= 100000u);
        CHECK(st->dirty_instances == 16u);
        CHECK(st->dirty_spans == 1u);
        CHECK(st->batch_count <= 6u);
        total_ms += st->cpu_submit_ms;
    }
    f32 avg_ms = total_ms / 5.0f;
    CHECK(avg_ms < 0.5f);

    gpu_scene_set_100k_mode(FALSE);
    engine_gpu_set_available(0u);
}
