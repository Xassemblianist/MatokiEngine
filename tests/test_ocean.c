/* tests/test_ocean.c — M17_A 3 kademeli Tessendorf FFT okyanus birim testleri.
 * 1B/2B FFT <-> saf DFT denkliği, kademe periyodikliği ve ayrık dalga sayısı bantları,
 * JONSWAP spektrum varyansı ve anlamlı dalga yüksekliği Hs (U=10 m/s),
 * deterministiklik, Jacobian köpüğü ve ocean.mtk / sky.mtk derleme testleri.
 */
#include "test.h"
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/embed.h"
#include "gfx3d/ocean.h"
#include "gfx3d/atmos.h"
#include "gpu/gpu_ocean.h"
#include "matoki/matoki.h"
#include "../build/gen/embed_shaders.c"

static u8 g_ocean_test_mem[MB(32)];

/* 1B ve 2B radix-2 IFFT'nin saf O(N²) / O(N⁴) DFT toplamıyla eşleşmesi */
TEST(ocean_fft_matches_naive_dft) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));

    OceanDesc d = ocean_default_desc();
    d.cascades[0].n = 32u;
    d.cascades[1].n = 16u;
    d.cascades[2].n = 16u;
    Ocean o;
    ocean_init(&o, &a, &d);

    /* (a) 1B FFT testi (N = 32) */
    const OceanCascade* c1 = &o.cascades[0];
    u32 n1 = c1->n;
    f32 in_re[32], in_im[32], fft_re[32], fft_im[32];
    Rng rng;
    rng_seed(&rng, 0xCAFE1234ULL);
    for (u32 k = 0; k < n1; k++) {
        in_re[k]  = rng_f32(&rng) * 2.0f - 1.0f;
        in_im[k]  = rng_f32(&rng) * 2.0f - 1.0f;
        fft_re[k] = in_re[k];
        fft_im[k] = in_im[k];
    }
    ocean_fft_1d(c1, fft_re, fft_im, 1u);

    for (u32 x = 0; x < n1; x++) {
        f32 sum_r = 0.0f, sum_i = 0.0f;
        for (u32 k = 0; k < n1; k++) {
            f32 ang = M_TAU * (f32)(k * x) / (f32)n1;
            f32 cs  = m_cos(ang), sn = m_sin(ang);
            sum_r += in_re[k] * cs - in_im[k] * sn;
            sum_i += in_re[k] * sn + in_im[k] * cs;
        }
        CHECK_NEAR(fft_re[x], sum_r, 1e-3f);
        CHECK_NEAR(fft_im[x], sum_i, 1e-3f);
    }

    /* (b) 2B FFT testi (16×16) */
    const OceanCascade* c2 = &o.cascades[1];
    u32 n2 = c2->n;
    u32 nn = n2 * n2;
    f32* src_r = ARENA_PUSH_ARRAY(&a, f32, nn);
    f32* src_i = ARENA_PUSH_ARRAY(&a, f32, nn);
    f32* out_r = ARENA_PUSH_ARRAY(&a, f32, nn);
    f32* out_i = ARENA_PUSH_ARRAY(&a, f32, nn);
    for (u32 i = 0; i < nn; i++) {
        src_r[i] = rng_f32(&rng) - 0.5f;
        src_i[i] = rng_f32(&rng) - 0.5f;
        out_r[i] = src_r[i];
        out_i[i] = src_i[i];
    }
    ocean_fft_2d(c2, out_r, out_i);

    for (u32 z = 0; z < n2; z += 3u) {
        for (u32 x = 0; x < n2; x += 3u) {
            f32 sr = 0.0f, si = 0.0f;
            for (u32 kz = 0; kz < n2; kz++) {
                for (u32 kx = 0; kx < n2; kx++) {
                    f32 ang = M_TAU * ((f32)(kx * x + kz * z) / (f32)n2);
                    f32 cs  = m_cos(ang), sn = m_sin(ang);
                    u32 idx = kz * n2 + kx;
                    sr += src_r[idx] * cs - src_i[idx] * sn;
                    si += src_r[idx] * sn + src_i[idx] * cs;
                }
            }
            u32 oidx = z * n2 + x;
            CHECK_NEAR(out_r[oidx], sr, 2e-3f);
            CHECK_NEAR(out_i[oidx], si, 2e-3f);
        }
    }
}

/* Kademe periyodikliği h(x + k·L, z + m·L) == h(x, z) ve ayrık dalga sayısı bantları */
TEST(ocean_cascade_periodicity) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));

    OceanDesc d = ocean_default_desc();
    Ocean o;
    ocean_init(&o, &a, &d);
    ocean_update(&o, 2.75, 0.016f);

    /* Kademe kesim sınırları ayrık olmalı: k_min[c] == k_max[c-1] */
    CHECK(o.cascades[0].k_min == 0.0f);
    CHECK_NEAR(o.cascades[1].k_min, o.cascades[0].k_max, 1e-5f);
    CHECK_NEAR(o.cascades[2].k_min, o.cascades[1].k_max, 1e-5f);
    CHECK(o.cascades[2].k_max > o.cascades[2].k_min);

    for (u32 c = 0; c < OCEAN_CASCADES; c++) {
        const OceanCascade* cas = &o.cascades[c];
        f32 L = cas->size;
        f32 pts[4][2] = {
            { 13.7f, -28.4f },
            { -0.5f * L + 1.23f, 0.25f * L },
            { 0.0f, 0.0f },
            { L * 0.73f, -L * 0.41f }
        };
        for (u32 p = 0; p < 4u; p++) {
            OceanSample s0, s1, s2;
            f32 x = pts[p][0], z = pts[p][1];
            ocean_sample_cascade(cas, o.d.choppy, x, z, &s0);
            ocean_sample_cascade(cas, o.d.choppy, x + L, z - 2.0f * L, &s1);
            ocean_sample_cascade(cas, o.d.choppy, x - 3.0f * L, z + L, &s2);

            CHECK_NEAR(s0.disp.x,   s1.disp.x,   1e-4f);
            CHECK_NEAR(s0.disp.y,   s1.disp.y,   1e-4f);
            CHECK_NEAR(s0.disp.z,   s1.disp.z,   1e-4f);
            CHECK_NEAR(s0.normal.x, s1.normal.x, 1e-4f);
            CHECK_NEAR(s0.normal.y, s1.normal.y, 1e-4f);
            CHECK_NEAR(s0.normal.z, s1.normal.z, 1e-4f);
            CHECK_NEAR(s0.jacobian, s1.jacobian, 1e-4f);
            CHECK_NEAR(s0.foam,     s1.foam,     1e-4f);

            CHECK_NEAR(s0.disp.y,   s2.disp.y,   1e-4f);
            CHECK_NEAR(s0.slope_x,  s2.slope_x,  1e-4f);
            CHECK_NEAR(s0.slope_z,  s2.slope_z,  1e-4f);
        }
    }
}

/* JONSWAP spektrumu varyansı, Parseval özdeşliği ve U=10 m/s için anlamlı dalga yüksekliği Hs */
TEST(ocean_jonswap_variance_and_hs) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));

    OceanDesc d = ocean_default_desc();   /* U = 10 m/s, fetch = 100 km, gamma = 3.3 */
    Ocean o;
    ocean_init(&o, &a, &d);
    ocean_update(&o, 5.0, 0.016f);

    /* U = 10 m/s açık denizde Hs = 4*sigma_h ~ 1.4 .. 2.3 m aralığında olmalı */
    CHECK(o.theory_hs >= 1.25f && o.theory_hs <= 2.40f);
    CHECK(o.hs        >= 1.20f && o.hs        <= 2.50f);

    /* IFFT sonrası ızgara üzerindeki gerçek yüzey varyansı Parseval ile uyumlu olmalı */
    f64 total_var = 0.0;
    for (u32 c = 0; c < OCEAN_CASCADES; c++) {
        const OceanCascade* cas = &o.cascades[c];
        u32 nn = cas->n * cas->n;
        f64 sum_h2 = 0.0;
        for (u32 i = 0; i < nn; i++) {
            f64 h = (f64)cas->height[i];
            sum_h2 += h * h;
        }
        total_var += sum_h2 / (f64)nn;
    }
    f32 measured_hs = 4.0f * m_sqrt((f32)total_var);
    CHECK(measured_hs >= 1.15f && measured_hs <= 2.55f);
    CHECK_NEAR(measured_hs, o.hs, 0.35f);

    /* Rüzgâr hızı arttıkça Hs monoton büyümeli */
    o.d.wind_speed = 6.0f;
    ocean_rebuild_spectrum(&o);
    f32 hs_6 = o.theory_hs;

    o.d.wind_speed = 10.0f;
    ocean_rebuild_spectrum(&o);
    f32 hs_10 = o.theory_hs;

    o.d.wind_speed = 16.0f;
    ocean_rebuild_spectrum(&o);
    f32 hs_16 = o.theory_hs;

    CHECK(hs_6 > 0.35f && hs_6 < hs_10);
    CHECK(hs_10 < hs_16 && hs_16 > 3.0f);
}

/* Deterministiklik (aynı tohum -> bit-bit aynı çıktı), Jacobian köpüğü ve kamera merkezli ızgara */
TEST(ocean_determinism_and_foam) {
    Arena a1, a2;
    arena_init(&a1, g_ocean_test_mem, MB(16));
    arena_init(&a2, g_ocean_test_mem + MB(16), MB(16));

    OceanDesc d = ocean_default_desc();
    d.seed = 0x9988776655443322ULL;
    Ocean o1, o2;
    ocean_init(&o1, &a1, &d);
    ocean_init(&o2, &a2, &d);

    ocean_update(&o1, 1.25, 0.016f);
    ocean_update(&o2, 1.25, 0.016f);
    ocean_update(&o1, 3.50, 0.032f);
    ocean_update(&o2, 3.50, 0.032f);

    u32 foam_nonzero = 0u;
    for (u32 c = 0; c < OCEAN_CASCADES; c++) {
        u32 nn = o1.cascades[c].n * o1.cascades[c].n;
        CHECK(__builtin_memcmp(o1.cascades[c].height,  o2.cascades[c].height,  nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].disp_x,  o2.cascades[c].disp_x,  nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].disp_z,  o2.cascades[c].disp_z,  nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].slope_x, o2.cascades[c].slope_x, nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].slope_z, o2.cascades[c].slope_z, nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].foam,    o2.cascades[c].foam,    nn * sizeof(f32)) == 0);
        CHECK(__builtin_memcmp(o1.cascades[c].tex.px,  o2.cascades[c].tex.px,  nn * sizeof(u32)) == 0);
        for (u32 i = 0; i < nn; i++) {
            if (o1.cascades[c].foam[i] > 0.01f) foam_nonzero++;
        }
    }
    CHECK(foam_nonzero > 0u);

    /* Kamera merkezli geometrik ızgara testi */
    Mesh* m = ocean_mesh_init(&o1, &a1, 32u, 2.0f, 200.0f);
    CHECK(m != NULL);
    CHECK(m->vertex_count == 33u * 33u);
    CHECK(m->index_count  == 32u * 32u * 6u);
    /* Merkezde adım aralığı kenardaki adım aralığından küçük olmalı (geometrik büyüme) */
    f32 step_center = o1.grid_ofs[17] - o1.grid_ofs[16];
    f32 step_outer  = o1.grid_ofs[32] - o1.grid_ofs[31];
    CHECK(step_center > 0.0f && step_outer > step_center * 2.5f);

    ocean_mesh_update(&o1, vec3(14.3f, 8.0f, -22.7f));
    CHECK(m->radius > 100.0f && m->radius < 400.0f);
}

/* ocean.mtk ve sky.mtk gölgelendiricilerinin IR + Wasm JIT + WGSL derleme doğrulaması */
TEST(ocean_shaders_compile) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));

    const char* names[2] = { "ocean.mtk", "sky.mtk" };
    for (u32 s = 0; s < 2u; s++) {
        const EmbeddedFile* found = NULL;
        for (u32 i = 0; i < g_embedded_shader_count; i++) {
            const char* a_n = g_embedded_shaders[i].name;
            const char* b_n = names[s];
            u32 k = 0;
            while (a_n[k] && b_n[k] && a_n[k] == b_n[k]) k++;
            if (a_n[k] == '\0' && b_n[k] == '\0') {
                found = &g_embedded_shaders[i];
                break;
            }
        }
        CHECK(found != NULL);
        if (!found) continue;

        IrFunc ir;
        MtkLayout layout;
        MtkDiag diag;
        __builtin_memset(&ir, 0, sizeof(ir));
        __builtin_memset(&layout, 0, sizeof(layout));
        mtk_diag_init(&diag);

        CHECK(mtk_compile_ir(found->src, found->len, found->name, &a, &ir, &layout, &diag));
        ir_optimize(&ir);

        u8* wasm_buf = ARENA_PUSH_ARRAY(&a, u8, 65536u);
        MtkCodegenOpts opts = { FALSE, FALSE, MTK_FUSE_NONE };
        char err[256];
        u32 wasm_len = mtk_codegen(&ir, &opts, &a, wasm_buf, 65536u, err, sizeof(err));
        CHECK(wasm_len > 64u);

        char* wgsl_buf = ARENA_PUSH_ARRAY(&a, char, 65536u);
        u32 wgsl_len = mtk_compile_wgsl(found->src, found->len, found->name, NULL, &a,
                                        wgsl_buf, 65536u, &layout, &diag);
        CHECK(wgsl_len > 64u);
    }
}

/* Matoki @compute FFT çekirdeklerinin (spektrum, kelebek, açma) WGSL derlemesi + CPU referans denkliği */
TEST(ocean_gpu_matoki_compute_fft) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));

    OceanDesc d = ocean_default_desc();
    d.cascades[0].n = 32u;
    d.cascades[1].n = 16u;
    d.cascades[2].n = 16u;
    Ocean o;
    ocean_init(&o, &a, &d);
    ocean_update(&o, 3.25, 0.016f);

    f32 max_err = 1.0f;
    CHECK(gpu_ocean_test_compute_fft(&a, &o, 2u, 3.25, &max_err));
    CHECK(max_err < 1e-3f);
}

/* V1 Hillaire 2020 fiziksel atmosfer LUT, Beer-Powder zayıflaması, 3B Perlin-Worley gürültü ve gün döngüsü */
TEST(ocean_v1_hillaire_atmosphere_and_clouds) {
    AtmosParams ap = atmos_default_params();

    /* (a) Transmittance LUT fiziksel özellikleri */
    Vec3 t_zen = atmos_eval_transmittance(&ap, 0.02f, 0.95f);
    Vec3 t_hor = atmos_eval_transmittance(&ap, 0.02f, 0.04f);
    Vec3 t_hi  = atmos_eval_transmittance(&ap, 25.0f, 0.20f);
    Vec3 t_lo  = atmos_eval_transmittance(&ap, 0.50f, 0.20f);

    CHECK(t_zen.x > 0.75f && t_zen.z > 0.55f && t_zen.x <= 1.0f);
    CHECK(t_zen.x > t_hor.x && t_zen.z > t_hor.z);
    /* Gün batımı ufuk ışınında Rayleigh mavi saçılımı yüzünden kırmızı geçirgenlik maviden çok büyük olmalı */
    CHECK(t_hor.x > t_hor.z * 2.5f);
    /* İrtifa arttıkça atmosferik optik derinlik azalmalı (geçirgenlik artmalı) */
    CHECK(t_hi.x > t_lo.x && t_hi.y > t_lo.y && t_hi.z > t_lo.z);

    /* (b) Çoklu saçılım (Multi-Scattering) pozitifliği */
    Vec3 ms = atmos_eval_multiscatter(&ap, 1.0f, 0.6f);
    CHECK(ms.x > 0.0f && ms.y > 0.0f && ms.z > 0.0f);
    CHECK(ms.x < 1.0f && ms.y < 1.0f && ms.z < 1.0f);

    /* (c) Sky-View: öğle vakti zenitte mavi baskın, gün batımı ufkunda turuncu-kırmızı baskın */
    Vec3 sun_noon   = vec3_norm(vec3(0.0f, 0.85f, -0.52f));
    Vec3 sun_sunset = vec3_norm(vec3(0.0f, 0.04f, -0.99f));
    Vec3 sky_noon_z = atmos_eval_sky_ray(&ap, 0.02f, vec3(0.0f, 0.95f, -0.31f), sun_noon);
    Vec3 sky_set_h  = atmos_eval_sky_ray(&ap, 0.02f, vec3(0.0f, 0.05f, -0.99f), sun_sunset);
    CHECK(sky_noon_z.z > sky_noon_z.x * 1.4f);
    CHECK(sky_set_h.x  > sky_set_h.z  * 1.3f);

    /* (d) 3B Aerial Perspective: mesafe arttıkça geçirgenlik azalmalı, saçılan ışık artmalı */
    Vec3 ap_near_ins, ap_far_ins;
    f32  ap_near_tr = 0.0f, ap_far_tr = 0.0f;
    atmos_eval_aerial(&ap, 0.05f, vec3(0.0f, 0.04f, -0.99f), sun_noon, 2.0f,  &ap_near_ins, &ap_near_tr);
    atmos_eval_aerial(&ap, 0.05f, vec3(0.0f, 0.04f, -0.99f), sun_noon, 28.0f, &ap_far_ins,  &ap_far_tr);
    CHECK(ap_near_tr > ap_far_tr && ap_near_tr <= 1.0f && ap_far_tr > 0.0f);
    CHECK(ap_far_ins.z > ap_near_ins.z);

    /* (e) Beer-Powder hacimsel bulut ışık zayıflaması */
    f32 bp0 = atmos_beer_powder(0.0f);
    f32 bp1 = atmos_beer_powder(0.6f);
    f32 bp6 = atmos_beer_powder(6.0f);
    CHECK(bp0 >= 0.0f && bp0 <= 1.5f);
    CHECK(bp1 > 0.25f && bp6 < bp1);

    /* (f) 3B Perlin-Worley bulut gürültüsü [0, 1] sınırları ve varyansı */
    f32 r_min = 1.0f, r_max = 0.0f;
    for (u32 z = 0; z < 8u; z++) {
        for (u32 y = 0; y < 8u; y++) {
            for (u32 x = 0; x < 8u; x++) {
                f32 nw[4];
                atmos_cloud_noise_texel(x * 4u, y * 4u, z * 4u, 32u, nw);
                CHECK(nw[0] >= 0.0f && nw[0] <= 1.0f);
                CHECK(nw[1] >= 0.0f && nw[1] <= 1.0f);
                CHECK(nw[2] >= 0.0f && nw[2] <= 1.0f);
                CHECK(nw[3] >= 0.0f && nw[3] <= 1.0f);
                if (nw[0] < r_min) r_min = nw[0];
                if (nw[0] > r_max) r_max = nw[0];
            }
        }
    }
    CHECK((r_max - r_min) > 0.15f);

    /* Periyodiklik: x + N == x */
    f32 p0[4], p1[4];
    atmos_cloud_noise_texel(5u, 11u, 19u, 32u, p0);
    atmos_cloud_noise_texel(5u + 32u, 11u + 64u, 19u + 32u, 32u, p1);
    CHECK_NEAR(p0[0], p1[0], 1e-5f);
    CHECK_NEAR(p0[1], p1[1], 1e-5f);
    CHECK_NEAR(p0[2], p1[2], 1e-5f);
    CHECK_NEAR(p0[3], p1[3], 1e-5f);

    /* (g) Gün döngüsü (Öğle / Gün Batımı / Gece) aydınlatma geçişleri */
    atmos_set_sun_angles(&ap, 0.80f, 0.35f);
    CHECK(ap.star_intensity < 0.01f && ap.sun_dir.y > 0.65f);
    atmos_set_sun_angles(&ap, -0.45f, 0.35f);
    CHECK(ap.star_intensity > 0.70f && ap.moon_intensity > 0.70f && ap.moon_dir.y > 0.20f);
}

/* V1 Matoki @compute çekirdeklerinin (atmos_transmittance, atmos_multiscatter, atmos_skyview,
 * atmos_aerial, cloud_noise) IR + WGSL derleme ve CPU yorumlayıcı doğrulaması */
TEST(ocean_v1_gpu_atmos_and_cloud_compute) {
    Arena a;
    arena_init(&a, g_ocean_test_mem, sizeof(g_ocean_test_mem));
    CHECK(gpu_ocean_test_atmos_compute(&a));
}
