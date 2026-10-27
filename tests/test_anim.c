/* tests/test_anim.c — İskelet (Skeleton), animasyon klibi (STEP/LINEAR/CUBICSPLINE, döngü, kırpma,
 * karıştırma), CPU skinning (bind pozu, 90° analitik bükülme), bozuk/düşmanca girdi denetimleri,
 * arm_skin.glb 20.000 iterasyon ASan fuzz testi ve 10k köşe × 32 eklem skinning benchmark'ı.
 */
#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/math.h"
#include "plat/platform.h"
#include "anim/skeleton.h"
#include "anim/clip.h"
#include "anim/skin.h"
#include "asset/gltf.h"
#include "raster/framebuffer.h"
#include "raster/raster.h"
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 g_anim_mem[MB(32)];
static u8 g_glb_file[MB(1)];

static u32 read_bin_file(const char* path, u8* dst, u32 cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    u32 n = (u32)fread(dst, 1, cap, f);
    fclose(f);
    return n;
}

static f32 make_nan_f32(void) {
    u32 bits = 0x7fc00001u;
    f32 v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

TEST(anim_quat_slerp_nlerp) {
    Quat q0 = quat_identity();
    Quat q90 = quat_from_axis_angle(vec3(0.0f, 0.0f, 1.0f), M_HALF_PI);

    /* t = 0.0 ve t = 1.0 uç değerleri */
    Quat s0 = quat_slerp(q0, q90, 0.0f);
    Quat s1 = quat_slerp(q0, q90, 1.0f);
    CHECK_NEAR(s0.x, q0.x, 1e-5); CHECK_NEAR(s0.y, q0.y, 1e-5);
    CHECK_NEAR(s0.z, q0.z, 1e-5); CHECK_NEAR(s0.w, q0.w, 1e-5);
    CHECK_NEAR(s1.x, q90.x, 1e-5); CHECK_NEAR(s1.y, q90.y, 1e-5);
    CHECK_NEAR(s1.z, q90.z, 1e-5); CHECK_NEAR(s1.w, q90.w, 1e-5);

    /* t = 0.5 -> 45 derece, t = 0.25 -> 22.5 derece */
    Quat q45_ref  = quat_from_axis_angle(vec3(0.0f, 0.0f, 1.0f), M_PI * 0.25f);
    Quat q225_ref = quat_from_axis_angle(vec3(0.0f, 0.0f, 1.0f), M_PI * 0.125f);
    Quat s_mid = quat_slerp(q0, q90, 0.5f);
    Quat s_qtr = quat_slerp(q0, q90, 0.25f);
    CHECK_NEAR(s_mid.z, q45_ref.z, 1e-5);
    CHECK_NEAR(s_mid.w, q45_ref.w, 1e-5);
    CHECK_NEAR(s_qtr.z, q225_ref.z, 1e-5);
    CHECK_NEAR(s_qtr.w, q225_ref.w, 1e-5);

    /* NLERP orta noktada (t=0.5) SLERP ile birebir aynıdır ve her yerde birim uzunluktadır */
    Quat n_mid = quat_nlerp(q0, q90, 0.5f);
    CHECK_NEAR(n_mid.z, q45_ref.z, 1e-5);
    CHECK_NEAR(n_mid.w, q45_ref.w, 1e-5);
    Quat n_qtr = quat_nlerp(q0, q90, 0.25f);
    f32 n_len = m_sqrt(n_qtr.x * n_qtr.x + n_qtr.y * n_qtr.y + n_qtr.z * n_qtr.z + n_qtr.w * n_qtr.w);
    CHECK_NEAR(n_len, 1.0f, 1e-5);

    /* En kısa yol (negatif iç çarpım / antipodal yarı küre) */
    Quat q90_neg = quat(-q90.x, -q90.y, -q90.z, -q90.w);
    Quat s_short = quat_slerp(q0, q90_neg, 0.5f);
    CHECK_NEAR(s_short.z, q45_ref.z, 1e-5);
    CHECK_NEAR(s_short.w, q45_ref.w, 1e-5);
}

TEST(anim_clip_step_linear_cubicspline_clamp_loop) {
    Arena a;
    arena_init(&a, g_anim_mem, MB(4));

    Skeleton skel;
    CHECK(skeleton_init(&skel, &a, 2u));
    skel.parents[0] = -1;
    skel.parents[1] = 0;

    static const f32 times3[3] = { 0.0f, 1.0f, 2.0f };
    static const f32 step_vals[9] = {
        1.0f, 2.0f, 3.0f,
        4.0f, 5.0f, 6.0f,
        7.0f, 8.0f, 9.0f
    };
    static const f32 lin_vals[9] = {
        0.0f,  0.0f, 0.0f,
        10.0f, 20.0f, 30.0f,
        20.0f, 0.0f, 60.0f
    };
    /* CUBICSPLINE: t=[0, 1], k=0: in=(0,0,0), v0=(0,0,0), out=(0,2,0); k=1: in=(0,2,0), v1=(1,1,0), out=(0,0,0) */
    static const f32 times2[2] = { 0.0f, 1.0f };
    static const f32 cubic_vals[18] = {
        0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f,   0.0f, 2.0f, 0.0f,
        0.0f, 2.0f, 0.0f,   1.0f, 1.0f, 0.0f,   0.0f, 0.0f, 0.0f
    };

    AnimSampler smps[3];
    smps[0] = (AnimSampler){ 3u, 3u, ANIM_INTERP_STEP,        times3, step_vals };
    smps[1] = (AnimSampler){ 3u, 3u, ANIM_INTERP_LINEAR,      times3, lin_vals };
    smps[2] = (AnimSampler){ 2u, 3u, ANIM_INTERP_CUBICSPLINE, times2, cubic_vals };

    AnimChannel ch_step  = { 0u, 0u, 0, ANIM_PATH_TRANSLATION };
    AnimChannel ch_lin   = { 1u, 0u, 0, ANIM_PATH_TRANSLATION };
    AnimChannel ch_cubic = { 2u, 1u, 1, ANIM_PATH_TRANSLATION };

    AnimClip clip_step  = { "step",  2.0f, smps, 3u, &ch_step,  1u };
    AnimClip clip_lin   = { "lin",   2.0f, smps, 3u, &ch_lin,   1u };
    AnimClip clip_cubic = { "cubic", 1.0f, smps, 3u, &ch_cubic, 1u };

    CHECK(clip_validate(&clip_step, 2u));
    CHECK(clip_validate(&clip_lin, 2u));
    CHECK(clip_validate(&clip_cubic, 2u));

    JointPose poses[2];

    /* 1. STEP: sınır dışı kırpma ve aralık içi basamak davranışı */
    CHECK(clip_sample(&clip_step, &skel, -5.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 1.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 0.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 1.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 0.99f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 1.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 1.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 4.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 1.99f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 4.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 2.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 7.0f, 1e-5);
    CHECK(clip_sample(&clip_step, &skel, 99.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 7.0f, 1e-5);

    /* 2. LINEAR: analitik değerler, sınır kırpma ve döngü (loop) */
    CHECK(clip_sample(&clip_lin, &skel, -1.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 0.0f, 1e-5);
    CHECK(clip_sample(&clip_lin, &skel, 0.25f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 2.5f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 5.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.z, 7.5f, 1e-5);
    CHECK(clip_sample(&clip_lin, &skel, 1.5f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 15.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 10.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.z, 45.0f, 1e-5);
    CHECK(clip_sample(&clip_lin, &skel, 10.0f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 20.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 0.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.z, 60.0f, 1e-5);

    /* Döngü: duration=2.0 iken t=2.25 -> 0.25, t=-0.5 -> 1.5 */
    CHECK(clip_sample(&clip_lin, &skel, 2.25f, TRUE, poses));
    CHECK_NEAR(poses[0].translation.x, 2.5f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 5.0f, 1e-5);
    CHECK(clip_sample(&clip_lin, &skel, -0.5f, TRUE, poses));
    CHECK_NEAR(poses[0].translation.x, 15.0f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 10.0f, 1e-5);

    /* 3. CUBICSPLINE: u=0.0, u=0.25, u=0.5, u=1.0 analitik Hermite değerleri */
    CHECK(clip_sample(&clip_cubic, &skel, 0.0f, FALSE, poses));
    CHECK_NEAR(poses[1].translation.x, 0.0f, 1e-5);
    CHECK_NEAR(poses[1].translation.y, 0.0f, 1e-5);

    /* u=0.5: h00=0.5, h10=0.125, h01=0.5, h11=-0.125 -> (0.5, 0.5, 0) */
    CHECK(clip_sample(&clip_cubic, &skel, 0.5f, FALSE, poses));
    CHECK_NEAR(poses[1].translation.x, 0.5f, 1e-5);
    CHECK_NEAR(poses[1].translation.y, 0.5f, 1e-5);
    CHECK_NEAR(poses[1].translation.z, 0.0f, 1e-5);

    /* u=0.25: h00=27/32, h10=9/64, h01=5/32, h11=-3/64
     * x = (5/32)*1 = 0.15625
     * y = (9/64)*2 + (5/32)*1 + (-3/64)*2 = 12/64 + 10/64 = 22/64 = 0.34375 */
    CHECK(clip_sample(&clip_cubic, &skel, 0.25f, FALSE, poses));
    CHECK_NEAR(poses[1].translation.x, 0.15625f, 1e-5);
    CHECK_NEAR(poses[1].translation.y, 0.34375f, 1e-5);

    CHECK(clip_sample(&clip_cubic, &skel, 1.0f, FALSE, poses));
    CHECK_NEAR(poses[1].translation.x, 1.0f, 1e-5);
    CHECK_NEAR(poses[1].translation.y, 1.0f, 1e-5);

    /* 4. İki animasyon arasında ağırlıklı karıştırma */
    JointPose blended[2];
    CHECK(clip_sample_blended(&clip_step, 1.0f, FALSE, &clip_lin, 0.5f, FALSE,
                              0.25f, TRUE, &skel, blended));
    /* step(t=1.0)=(4,5,6), lin(t=0.5)=(5,10,15); w=0.25 -> (4.25, 6.25, 8.25) */
    CHECK_NEAR(blended[0].translation.x, 4.25f, 1e-5);
    CHECK_NEAR(blended[0].translation.y, 6.25f, 1e-5);
    CHECK_NEAR(blended[0].translation.z, 8.25f, 1e-5);
}

TEST(anim_gltf_bind_pose_and_90deg_bend) {
    u32 n = read_bin_file("tests/assets/arm_skin.glb", g_glb_file, sizeof(g_glb_file));
    CHECK(n > 0);
    Arena out, scratch;
    arena_init(&out, g_anim_mem, MB(12));
    arena_init(&scratch, g_anim_mem + MB(12), MB(16));

    GltfModel m;
    b32 ok = gltf_load(g_glb_file, n, &out, &scratch, NULL, NULL, &m);
    if (!ok) printf("arm_skin.glb hata: %s\n", m.err);
    CHECK(ok);
    if (!ok) return;

    /* Uyarı olmamalı ("skin yok sayildi" / "animasyon yok sayildi" kaldırıldı) */
    CHECK(m.warnings == 0u);
    CHECK(m.skin_count == 1u);
    CHECK(m.anim_count == 3u);
    CHECK(m.draw_count == 1u);
    CHECK(m.draws[0].skin == 0);
    CHECK(m.draws[0].skinned != NULL);

    const Skeleton* skel = &m.skins[0];
    const SkinnedMesh* sm = m.draws[0].skinned;
    const Mesh* bind = sm->bind_mesh;
    Mesh* skinned_out = m.draws[0].mesh;
    CHECK(skel->joint_count == 3u);
    CHECK(skeleton_validate(skel));
    CHECK(skin_validate_weights(sm->joints, sm->weights, sm->vertex_count, skel->joint_count));

    /* 1. Bind pozunda skinning sonucu == orijinal mesh (+-1e-5) */
    Mat4 world_mats[3], skin_mats[3];
    CHECK(skeleton_compute_matrices(skel, NULL, world_mats, skin_mats));
    CHECK(skin_mesh(sm, skin_mats, skel->joint_count, skinned_out));

    for (u32 i = 0; i < bind->vertex_count; i++) {
        CHECK_NEAR(skinned_out->px[i], bind->px[i], 1e-5);
        CHECK_NEAR(skinned_out->py[i], bind->py[i], 1e-5);
        CHECK_NEAR(skinned_out->pz[i], bind->pz[i], 1e-5);
        CHECK_NEAR(skinned_out->nx[i], bind->nx[i], 1e-5);
        CHECK_NEAR(skinned_out->ny[i], bind->ny[i], 1e-5);
        CHECK_NEAR(skinned_out->nz[i], bind->nz[i], 1e-5);
    }

    /* 2. Animasyon 0 ("bend_linear") t = 1.0 saniyede dirsek (eklem 1, y=1) +Z etrafında 90° bükülür:
     * Uç köşe (köşe 20, bind konum=(0, 2, 0), bind normal=(0, 1, 0))
     * Analitik yeni konum: (-1, 1, 0), yeni normal: (-1, 0, 0) */
    JointPose poses[3];
    CHECK(clip_sample(&m.animations[0], skel, 1.0f, FALSE, poses));
    CHECK(skeleton_compute_matrices(skel, poses, world_mats, skin_mats));
    CHECK(skin_mesh(sm, skin_mats, skel->joint_count, skinned_out));

    u32 tip = bind->vertex_count - 1u;
    CHECK_NEAR(bind->px[tip], 0.0f, 1e-6);
    CHECK_NEAR(bind->py[tip], 2.0f, 1e-6);
    CHECK_NEAR(bind->pz[tip], 0.0f, 1e-6);

    CHECK_NEAR(skinned_out->px[tip], -1.0f, 1e-5);
    CHECK_NEAR(skinned_out->py[tip],  1.0f, 1e-5);
    CHECK_NEAR(skinned_out->pz[tip],  0.0f, 1e-5);
    CHECK_NEAR(skinned_out->nx[tip], -1.0f, 1e-5);
    CHECK_NEAR(skinned_out->ny[tip],  0.0f, 1e-5);
    CHECK_NEAR(skinned_out->nz[tip],  0.0f, 1e-5);

    /* 3. t = 0.5 saniyede (45° bükülme): uç köşe (-sqrt(2)/2, 1 + sqrt(2)/2, 0) */
    f32 s45 = m_sqrt(0.5f);
    CHECK(clip_sample(&m.animations[0], skel, 0.5f, FALSE, poses));
    CHECK(skeleton_compute_matrices(skel, poses, world_mats, skin_mats));
    CHECK(skin_mesh(sm, skin_mats, skel->joint_count, skinned_out));
    CHECK_NEAR(skinned_out->px[tip], -s45,        1e-5);
    CHECK_NEAR(skinned_out->py[tip],  1.0f + s45, 1e-5);
    CHECK_NEAR(skinned_out->pz[tip],  0.0f,       1e-5);
    CHECK_NEAR(skinned_out->nx[tip], -s45,        1e-5);
    CHECK_NEAR(skinned_out->ny[tip],  s45,        1e-5);

    /* 4. GLB içindeki STEP ve CUBICSPLINE animasyonlarını da doğrula */
    CHECK(clip_sample(&m.animations[1], skel, 1.5f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 2.0f, 1e-5);
    CHECK(clip_sample(&m.animations[2], skel, 0.5f, FALSE, poses));
    CHECK_NEAR(poses[0].translation.x, 0.5f, 1e-5);
    CHECK_NEAR(poses[0].translation.y, 0.5f, 1e-5);

    /* 5. Çıktı Mesh'i mevcut gfx3d boru hattıyla çizilebilmeli (yalnız-derinlik geçişi) */
    Framebuffer fb;
    Rasterizer rast;
    fb_init(&fb, &out, 64, 64);
    fb_init_depth(&fb, &out);
    fb_clear_depth(&fb, 0.0f);
    raster_begin(&rast, &fb, &scratch);

    G3dContext g3d;
    Vec3 eye = vec3(0.0f, 1.0f, 4.0f);
    Mat4 view = mat4_look_at(eye, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    Mat4 proj = mat4_perspective_rz(M_PI / 3.0f, 1.0f, 0.1f);
    g3d_begin(&g3d, &rast, &scratch, view, proj, eye);

    G3dMaterial gmat;
    memset(&gmat, 0, sizeof(gmat));
    gmat.cull = G3D_CULL_NONE;
    gmat.depth_test = TRUE;
    gmat.depth_write = TRUE;
    CHECK(g3d_draw(&g3d, skinned_out, &m.draws[0].world, &gmat));
    raster_flush(&rast);
    CHECK(g3d.stats.tris_out > 0u);
}

TEST(anim_hostile_and_corrupt_inputs) {
    Arena a;
    arena_init(&a, g_anim_mem, MB(8));
    f32 nan_v = make_nan_f32();

    /* 1. Döngüsel eklem hiyerarşisi (0 -> 1 -> 2 -> 0) ve öz-ebeveyn (0 -> 0) */
    Skeleton skel;
    CHECK(skeleton_init(&skel, &a, 3u));
    skel.parents[0] = 2;
    skel.parents[1] = 0;
    skel.parents[2] = 1;
    CHECK(!skeleton_validate(&skel));

    /* Temizlemeden doğrudan skeleton_compute_matrices çağrılsa bile sonsuz döngüye girmez */
    Mat4 wm[3], sm_mats[3];
    CHECK(skeleton_compute_matrices(&skel, NULL, wm, sm_mats));
    for (u32 j = 0; j < 3u; j++) {
        for (u32 c = 0; c < 16u; c++) CHECK(anim_f32_finite(sm_mats[j].m[c]));
    }
    u32 fixes = skeleton_sanitize(&skel);
    CHECK(fixes > 0u);
    CHECK(skeleton_validate(&skel));

    /* 2. Eklem indeksi >= eklem sayısı, ağırlık toplamı 0, NaN ağırlıklar */
    Mesh* bm = mesh_cube(&a, 0.5f);
    Mesh* out_m = skin_clone_mesh(&a, bm);
    u32 vc = bm->vertex_count;
    u16* joints  = ARENA_PUSH_ARRAY(&a, u16, vc * 4u);
    f32* weights = ARENA_PUSH_ARRAY(&a, f32, vc * 4u);

    /* Köşe 0: eklem indeksi >= eklem sayısı (99 >= 3) */
    joints[0] = 99u; joints[1] = 0u; joints[2] = 0u; joints[3] = 0u;
    weights[0] = 1.0f; weights[1] = 0.0f; weights[2] = 0.0f; weights[3] = 0.0f;
    CHECK(!skin_validate_weights(joints, weights, 1u, 3u));

    /* Köşe 1: ağırlık toplamı 0 */
    joints[4] = 0u; joints[5] = 1u; joints[6] = 2u; joints[7] = 0u;
    weights[4] = 0.0f; weights[5] = 0.0f; weights[6] = 0.0f; weights[7] = 0.0f;
    CHECK(!skin_validate_weights(joints + 4, weights + 4, 1u, 3u));

    /* Köşe 2: NaN ağırlık */
    joints[8] = 0u; joints[9] = 1u; joints[10] = 0u; joints[11] = 0u;
    weights[8] = nan_v; weights[9] = 1.0f; weights[10] = 0.0f; weights[11] = 0.0f;
    CHECK(!skin_validate_weights(joints + 8, weights + 8, 1u, 3u));

    for (u32 i = 3; i < vc; i++) {
        joints[i * 4] = 0u;
        weights[i * 4] = 1.0f;
    }

    /* Doğrudan bozuk ağırlık/eklem ve NaN matrisle skin_mesh çağrısı: çökmez ve NaN üretmez */
    SkinnedMesh sk_mesh = { bm, vc, joints, weights };
    sm_mats[0].m[0] = nan_v;
    CHECK(skin_mesh(&sk_mesh, sm_mats, 3u, out_m));
    for (u32 i = 0; i < vc; i++) {
        CHECK(anim_f32_finite(out_m->px[i]));
        CHECK(anim_f32_finite(out_m->py[i]));
        CHECK(anim_f32_finite(out_m->pz[i]));
        CHECK(anim_f32_finite(out_m->nx[i]));
        CHECK(anim_f32_finite(out_m->ny[i]));
        CHECK(anim_f32_finite(out_m->nz[i]));
    }

    /* skin_sanitize_weights sonrası skin_validate_weights geçmeli */
    u32 w_fixes = skin_sanitize_weights(joints, weights, vc, 3u);
    CHECK(w_fixes == 3u);
    CHECK(skin_validate_weights(joints, weights, vc, 3u));

    /* 3. 0 anahtar kareli örnekleyici ve NaN zaman/değer */
    AnimSampler bad_smp = { 0u, 3u, ANIM_INTERP_LINEAR, NULL, NULL };
    AnimChannel bad_ch  = { 0u, 0u, 0, ANIM_PATH_TRANSLATION };
    AnimClip    bad_clip = { "empty", 0.0f, &bad_smp, 1u, &bad_ch, 1u };
    CHECK(!clip_validate(&bad_clip, 3u));

    JointPose poses[3];
    CHECK(!clip_sample(&bad_clip, &skel, 0.5f, FALSE, poses));
    /* Bozuk klip örneklendiğinde bind pozu korunur */
    CHECK_NEAR(poses[0].scale.x, 1.0f, 1e-6);

    /* 4. Bozuk glTF JSON: döngüsel node ve 0 anahtar kareli animasyon */
    static const char* cyclic_gltf =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"children\":[1]},{\"children\":[0]}],"
        "\"skins\":[{\"joints\":[0,1]}],"
        "\"animations\":[{\"samplers\":[],\"channels\":[]}]}";
    Arena out2, scr2;
    arena_init(&out2, g_anim_mem + MB(8), MB(4));
    arena_init(&scr2, g_anim_mem + MB(12), MB(4));
    GltfModel gm;
    b32 gok = gltf_load((const u8*)cyclic_gltf, (u32)strlen(cyclic_gltf), &out2, &scr2, NULL, NULL, &gm);
    CHECK(!gok || gm.warnings > 0u);
    CHECK(gm.warnings >= 2u); /* döngüsel hiyerarşi + boş animasyon uyarıları */
}

TEST(anim_glb_fuzz_20000) {
    static u8 orig[8192], buf[8192];
    u32 n = read_bin_file("tests/assets/arm_skin.glb", orig, sizeof(orig));
    CHECK(n > 0 && n <= sizeof(orig));

    Rng r;
    rng_seed(&r, 102030u);
    Arena out, scratch;
    u32 ok_count = 0;

    for (u32 it = 0; it < 20000u; it++) {
        memcpy(buf, orig, n);
        u32 muts = 1u + rng_u32(&r) % 6u;
        for (u32 k = 0; k < muts; k++) {
            buf[rng_u32(&r) % n] = (u8)rng_u32(&r);
        }
        arena_init(&out, g_anim_mem, MB(12));
        arena_init(&scratch, g_anim_mem + MB(12), MB(16));
        GltfModel m;
        if (gltf_load(buf, n, &out, &scratch, NULL, NULL, &m)) {
            ok_count++;
            /* Yüklenen iskelet, animasyon ve skin'leri gerçekten çalıştır (ASan sınır denetimi) */
            for (u32 si = 0; si < m.skin_count; si++) {
                const Skeleton* sk = &m.skins[si];
                if (sk->joint_count == 0u) continue;
                JointPose poses[SKEL_MAX_JOINTS];
                Mat4 skin_mats[SKEL_MAX_JOINTS];
                if (m.anim_count > 0u) {
                    clip_sample(&m.animations[0], sk, 0.5f, TRUE, poses);
                    skeleton_compute_matrices(sk, poses, NULL, skin_mats);
                } else {
                    skeleton_compute_matrices(sk, NULL, NULL, skin_mats);
                }
                for (u32 d = 0; d < m.draw_count; d++) {
                    if (m.draws[d].skin == (i32)si && m.draws[d].skinned) {
                        skin_mesh(m.draws[d].skinned, skin_mats, sk->joint_count, m.draws[d].mesh);
                    }
                }
            }
        }
    }
    CHECK(ok_count > 0u);
}

static f64 sort_median(f64* arr, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f64 v = arr[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && arr[j] > v) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = v;
    }
    return arr[n / 2u];
}

TEST(anim_bench_10k_verts_32_joints) {
    Arena a;
    arena_init(&a, g_anim_mem, sizeof(g_anim_mem));

    const u32 V_COUNT = 10000u;
    const u32 J_COUNT = 32u;

    Skeleton skel;
    CHECK(skeleton_init(&skel, &a, J_COUNT));
    for (u32 j = 0; j < J_COUNT; j++) {
        skel.parents[j] = (j == 0u) ? -1 : (i32)(j - 1u);
        skel.bind_pose[j] = joint_pose_trs(
            vec3(0.0f, 0.25f, 0.0f),
            quat_from_axis_angle(vec3(0.0f, 0.0f, 1.0f), 0.05f),
            vec3(1.0f, 1.0f, 1.0f)
        );
    }

    Mat4 bind_world[32], skin_mats[32];
    CHECK(skeleton_compute_matrices(&skel, NULL, bind_world, NULL));
    for (u32 j = 0; j < J_COUNT; j++) {
        skel.inv_bind[j] = mat4_inverse(bind_world[j]);
    }

    /* Bükülmüş poz üret */
    JointPose anim_poses[32];
    for (u32 j = 0; j < J_COUNT; j++) {
        anim_poses[j] = joint_pose_trs(
            vec3(0.0f, 0.25f, 0.0f),
            quat_from_axis_angle(vec3(0.0f, 0.0f, 1.0f), 0.12f),
            vec3(1.0f, 1.0f, 1.0f)
        );
    }
    CHECK(skeleton_compute_matrices(&skel, anim_poses, NULL, skin_mats));

    Mesh* bind_m = mesh_alloc(&a, V_COUNT, 3u, FALSE);
    Mesh* out_m  = mesh_alloc(&a, V_COUNT, 3u, FALSE);
    u16* joints  = ARENA_PUSH_ARRAY(&a, u16, V_COUNT * 4u);
    f32* weights = ARENA_PUSH_ARRAY(&a, f32, V_COUNT * 4u);

    for (u32 i = 0; i < V_COUNT; i++) {
        f32 t = (f32)i / (f32)V_COUNT;
        bind_m->px[i] = m_cos(t * M_TAU) * 0.5f;
        bind_m->py[i] = t * 8.0f;
        bind_m->pz[i] = m_sin(t * M_TAU) * 0.5f;
        bind_m->nx[i] = m_cos(t * M_TAU);
        bind_m->ny[i] = 0.0f;
        bind_m->nz[i] = m_sin(t * M_TAU);

        u16 j0 = (u16)((i * 3u) % J_COUNT);
        u16 j1 = (u16)((j0 + 1u) % J_COUNT);
        u16 j2 = (u16)((j0 + 2u) % J_COUNT);
        u16 j3 = (u16)((j0 + 3u) % J_COUNT);
        joints[i * 4 + 0] = j0;
        joints[i * 4 + 1] = j1;
        joints[i * 4 + 2] = j2;
        joints[i * 4 + 3] = j3;
        weights[i * 4 + 0] = 0.4f;
        weights[i * 4 + 1] = 0.3f;
        weights[i * 4 + 2] = 0.2f;
        weights[i * 4 + 3] = 0.1f;
    }

    SkinnedMesh sm = { bind_m, V_COUNT, joints, weights };

    const u32 WARMUP = 5u;
    const u32 ITERS  = 31u;
    f64 t_verts[31], t_full[31];

    for (u32 it = 0; it < WARMUP + ITERS; it++) {
        f64 t0 = plat_time_ms();
        skin_vertices(skin_mats, J_COUNT,
                      bind_m->px, bind_m->py, bind_m->pz,
                      bind_m->nx, bind_m->ny, bind_m->nz,
                      joints, weights, V_COUNT,
                      out_m->px, out_m->py, out_m->pz,
                      out_m->nx, out_m->ny, out_m->nz);
        f64 dt = plat_time_ms() - t0;
        if (it >= WARMUP) t_verts[it - WARMUP] = dt;
    }

    for (u32 it = 0; it < WARMUP + ITERS; it++) {
        f64 t0 = plat_time_ms();
        skeleton_compute_matrices(&skel, anim_poses, NULL, skin_mats);
        skin_mesh(&sm, skin_mats, J_COUNT, out_m);
        f64 dt = plat_time_ms() - t0;
        if (it >= WARMUP) t_full[it - WARMUP] = dt;
    }

    f64 med_verts = sort_median(t_verts, ITERS);
    f64 med_full  = sort_median(t_full, ITERS);
    printf("  [bench M10_C] 10k kose, 32 eklem skinning (host ASan/O1): "
           "skin_vertices medyan %.3f ms (%.1f M kose/s), "
           "matris+skin+bounds medyan %.3f ms\n",
           med_verts, (10.0 / (med_verts > 1e-6 ? med_verts : 1e-6)), med_full);
    CHECK(anim_f32_finite(out_m->px[V_COUNT - 1u]));
}

TEST(anim_skin_wasm_simd_vs_scalar) {
    int rc_link = system(
        "clang --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--allow-undefined "
        "-Wl,--export=skin_vertices -Wl,--export=skin_vertices_scalar "
        "build/wasm/src/anim/skin_wasm.o build/wasm/src/anim/skin.o build/wasm/src/core/math.o "
        "-o build/wasm/skin_simd_test.wasm"
    );
    CHECK(rc_link == 0);
    if (rc_link != 0) return;

    int rc_node = system(
        "node -e '"
        "const fs = require(\"fs\");"
        "const bytes = fs.readFileSync(\"build/wasm/skin_simd_test.wasm\");"
        "const mod = new WebAssembly.Module(bytes);"
        "const inst = new WebAssembly.Instance(mod, { env: { mesh_alloc: ()=>0, mesh_compute_bounds: ()=>{} } });"
        "const mem = inst.exports.memory;"
        "mem.grow(64);"
        "const N = 10000, J = 32;"
        "let off = 65536;"
        "const pMats = off; off += J * 64;"
        "const pPx = off; off += N * 4;"
        "const pPy = off; off += N * 4;"
        "const pPz = off; off += N * 4;"
        "const pNx = off; off += N * 4;"
        "const pNy = off; off += N * 4;"
        "const pNz = off; off += N * 4;"
        "const pJts = off; off += N * 8;"
        "const pWts = off; off += N * 16;"
        "const pOutS = off; off += N * 24;"
        "const pOutV = off; off += N * 24;"
        "const f32 = new Float32Array(mem.buffer);"
        "const u16 = new Uint16Array(mem.buffer);"
        "for (let j = 0; j < J; j++) {"
        "  const a = j * 0.17; const c = Math.cos(a), s = Math.sin(a);"
        "  const b = (pMats >> 2) + j * 16;"
        "  f32[b+0]=c; f32[b+1]=s; f32[b+2]=0; f32[b+3]=0;"
        "  f32[b+4]=-s; f32[b+5]=c; f32[b+6]=0; f32[b+7]=0;"
        "  f32[b+8]=0; f32[b+9]=0; f32[b+10]=1; f32[b+11]=0;"
        "  f32[b+12]=j*0.1; f32[b+13]=j*0.2; f32[b+14]=-j*0.05; f32[b+15]=1;"
        "}"
        "for (let i = 0; i < N; i++) {"
        "  f32[(pPx>>2)+i] = Math.cos(i*0.01);"
        "  f32[(pPy>>2)+i] = i*0.001;"
        "  f32[(pPz>>2)+i] = Math.sin(i*0.01);"
        "  f32[(pNx>>2)+i] = Math.cos(i*0.02);"
        "  f32[(pNy>>2)+i] = 0.0;"
        "  f32[(pNz>>2)+i] = Math.sin(i*0.02);"
        "  const jb = (pJts>>1) + i*4;"
        "  u16[jb+0] = i % J; u16[jb+1] = (i+1)%J; u16[jb+2] = (i+2)%J; u16[jb+3] = (i+3)%J;"
        "  const wb = (pWts>>2) + i*4;"
        "  f32[wb+0] = 0.4; f32[wb+1] = 0.3; f32[wb+2] = 0.2; f32[wb+3] = 0.1;"
        "}"
        "inst.exports.skin_vertices_scalar(pMats, J, pPx, pPy, pPz, pNx, pNy, pNz, pJts, pWts, N,"
        "  pOutS, pOutS+N*4, pOutS+N*8, pOutS+N*12, pOutS+N*16, pOutS+N*20);"
        "inst.exports.skin_vertices(pMats, J, pPx, pPy, pPz, pNx, pNy, pNz, pJts, pWts, N,"
        "  pOutV, pOutV+N*4, pOutV+N*8, pOutV+N*12, pOutV+N*16, pOutV+N*20);"
        "let maxDiff = 0;"
        "for (let i = 0; i < N * 6; i++) {"
        "  const d = Math.abs(f32[(pOutS>>2)+i] - f32[(pOutV>>2)+i]);"
        "  if (!(d <= 1e-5)) { console.error(\"diff\", i, d); process.exit(1); }"
        "  if (d > maxDiff) maxDiff = d;"
        "}"
        "const { performance } = require(\"perf_hooks\");"
        "for (let k = 0; k < 10; k++) inst.exports.skin_vertices(pMats, J, pPx, pPy, pPz, pNx, pNy, pNz, pJts, pWts, N, pOutV, pOutV+N*4, pOutV+N*8, pOutV+N*12, pOutV+N*16, pOutV+N*20);"
        "const ts = [], tv = [];"
        "for (let k = 0; k < 31; k++) {"
        "  let t0 = performance.now();"
        "  inst.exports.skin_vertices_scalar(pMats, J, pPx, pPy, pPz, pNx, pNy, pNz, pJts, pWts, N, pOutS, pOutS+N*4, pOutS+N*8, pOutS+N*12, pOutS+N*16, pOutS+N*20);"
        "  ts.push(performance.now() - t0);"
        "  t0 = performance.now();"
        "  inst.exports.skin_vertices(pMats, J, pPx, pPy, pPz, pNx, pNy, pNz, pJts, pWts, N, pOutV, pOutV+N*4, pOutV+N*8, pOutV+N*12, pOutV+N*16, pOutV+N*20);"
        "  tv.push(performance.now() - t0);"
        "}"
        "ts.sort((a,b)=>a-b); tv.sort((a,b)=>a-b);"
        "console.log(`  [bench M10_C wasm V8] 10k kose, 32 eklem: skaler medyan ${ts[15].toFixed(3)} ms, SIMD medyan ${tv[15].toFixed(3)} ms (${(ts[15]/tv[15]).toFixed(2)}x, max_diff=${maxDiff})`);"
        "'"
    );
    remove("build/wasm/skin_simd_test.wasm");
    CHECK(rc_node == 0);
}

