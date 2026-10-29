/* gpu/gpu_ocean.c — "Okyanus" sahnesinin tam çözünürlüklü WebGPU hesaplama + çizim hattı (G1_A).
 * 3 kademeli Tessendorf FFT'yi Matoki @compute -> WGSL çekirdekleriyle GPU'da koşturup
 * sonuçları storage dokulara yazar; kamera merkezli geometrik ızgarayı köşe gölgelendiricisinde
 * dokulardan örnekleyerek yer değiştirir ve ocean.mtk + sky.mtk + tekne ağlarını MSAA 4x ile çizer.
 */
#include "gpu/gpu_ocean.h"
#include "gpu/rhi.h"
#include "gfx3d/atmos.h"
#include "gfx3d/pipeline.h"
#include "matoki/matoki.h"
#include "matoki/wgsl.h"
#include "core/log.h"

#define GPU_OCEAN_MAX_FFT_STAGES 16u
#define GPU_OCEAN_GRID_DIV       160u
#define GPU_OCEAN_GRID_SNAP      2.0f
#define GPU_OCEAN_GRID_RADIUS    520.0f
#define GPU_OCEAN_BOAT_PARTS     10u
#define GPU_OCEAN_MSAA_SAMPLES   4u
#define GPU_CLOUD_Q_W            480u
#define GPU_CLOUD_Q_H            270u

/* ---------- Matoki @compute Kaynakları (3 Kademeli Tessendorf GPU FFT) ---------- */

/* 1. Spektrum evrimi + 8 gerçek alanın 4 karmaşık alana Hermitsel paketlenmesi.
 * Giriş tamponları (h0_buf, kinfo_buf) 2B bit-ters (bit-reversed) sırada yüklenmiştir. */
static const char k_mtk_ocean_spectrum[] =
    "uniform params: vec4;\n"
    "buffer h0_buf: buffer<vec4>;\n"
    "buffer kinfo_buf: buffer<vec4>;\n"
    "buffer out01: buffer<vec4>;\n"
    "buffer out23: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx = i32(global_id);\n"
    "    let n_total = i32(params.y);\n"
    "    if (idx < n_total) {\n"
    "        let h0 = h0_buf[idx];\n"
    "        let ki = kinfo_buf[idx];\n"
    "        let ph = ki.z * params.x;\n"
    "        let cs = cos(ph);\n"
    "        let sn = sin(ph);\n"
    "        let hr = (h0.x + h0.z) * cs + (h0.y - h0.w) * sn;\n"
    "        let hi = (h0.y + h0.w) * cs + (h0.z - h0.x) * sn;\n"
    "        let kx = ki.x;\n"
    "        let kz = ki.y;\n"
    "        let ik = ki.w;\n"
    "        let ux = kx * ik;\n"
    "        let uz = kz * ik;\n"
    "        let kxz = (0.0 - ux) * kz;\n"
    "        let kxx = (0.0 - ux) * kx;\n"
    "        let kzz = (0.0 - uz) * kz;\n"
    "        let dxz_r = kxz * hr;\n"
    "        let dxz_i = kxz * hi;\n"
    "        let dx_r  = (0.0 - ux) * hi;\n"
    "        let dx_i  = ux * hr;\n"
    "        let dz_r  = (0.0 - uz) * hi;\n"
    "        let dz_i  = uz * hr;\n"
    "        let sx_r  = (0.0 - kx) * hi;\n"
    "        let sx_i  = kx * hr;\n"
    "        let sz_r  = (0.0 - kz) * hi;\n"
    "        let sz_i  = kz * hr;\n"
    "        let dxx_r = kxx * hr;\n"
    "        let dxx_i = kxx * hi;\n"
    "        let dzz_r = kzz * hr;\n"
    "        let dzz_i = kzz * hi;\n"
    "        out01[idx] = vec4(hr - dxz_i, hi + dxz_r, dx_r - dz_i, dx_i + dz_r);\n"
    "        out23[idx] = vec4(sx_r - sz_i, sx_i + sz_r, dxx_r - dzz_i, dxx_i + dzz_r);\n"
    "    }\n"
    "}\n";

/* 2. Radix-2 Cooley-Tukey kelebek aşaması (4 karmaşık alanı aynı anda işler).
 * stage_u: x = n, y = half, z = axis (0: satır, 1: sütun), w = n_total. */
static const char k_mtk_ocean_fft_stage[] =
    "uniform stage_u: ivec4;\n"
    "buffer src01: buffer<vec4>;\n"
    "buffer src23: buffer<vec4>;\n"
    "buffer dst01: buffer<vec4>;\n"
    "buffer dst23: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx = i32(global_id);\n"
    "    let n_total = stage_u.w;\n"
    "    if (idx < n_total) {\n"
    "        let n = stage_u.x;\n"
    "        let half = stage_u.y;\n"
    "        let axis = stage_u.z;\n"
    "        let i = idx & (n - 1);\n"
    "        let j = i32(f32(idx) / f32(n));\n"
    "        var coord: i32 = j;\n"
    "        var stride: i32 = n;\n"
    "        if (axis == 0) {\n"
    "            coord = i;\n"
    "            stride = 1;\n"
    "        }\n"
    "        let k = coord & (half - 1);\n"
    "        let step = half * stride;\n"
    "        var p: i32 = idx;\n"
    "        var sgn: f32 = 1.0;\n"
    "        if ((coord & half) != 0) {\n"
    "            p = idx - step;\n"
    "            sgn = -1.0;\n"
    "        }\n"
    "        let q = p + step;\n"
    "        let ang = 3.14159265 * f32(k) / f32(half);\n"
    "        let wc = cos(ang);\n"
    "        let ws = sin(ang);\n"
    "        let vp01 = src01[p];\n"
    "        let vq01 = src01[q];\n"
    "        let xr0 = vq01.x * wc - vq01.y * ws;\n"
    "        let xi0 = vq01.x * ws + vq01.y * wc;\n"
    "        let xr1 = vq01.z * wc - vq01.w * ws;\n"
    "        let xi1 = vq01.z * ws + vq01.w * wc;\n"
    "        dst01[idx] = vec4(\n"
    "            vp01.x + sgn * xr0,\n"
    "            vp01.y + sgn * xi0,\n"
    "            vp01.z + sgn * xr1,\n"
    "            vp01.w + sgn * xi1\n"
    "        );\n"
    "        let vp23 = src23[p];\n"
    "        let vq23 = src23[q];\n"
    "        let xr2 = vq23.x * wc - vq23.y * ws;\n"
    "        let xi2 = vq23.x * ws + vq23.y * wc;\n"
    "        let xr3 = vq23.z * wc - vq23.w * ws;\n"
    "        let xi3 = vq23.z * ws + vq23.w * wc;\n"
    "        dst23[idx] = vec4(\n"
    "            vp23.x + sgn * xr2,\n"
    "            vp23.y + sgn * xi2,\n"
    "            vp23.z + sgn * xr3,\n"
    "            vp23.w + sgn * xi3\n"
    "        );\n"
    "    }\n"
    "}\n";

/* 3. 2B IFFT sonrası 8 gerçek alanı açma, Jacobian beyaz köpük birikimi ve
 * yer değiştirme + eğim/yükseklik/köpük tamponlarını üretme. */
static const char k_mtk_ocean_unpack[] =
    "uniform u0: vec4;\n"
    "uniform u1: vec4;\n"
    "buffer in01: buffer<vec4>;\n"
    "buffer in23: buffer<vec4>;\n"
    "buffer foam_buf: buffer<f32>;\n"
    "buffer disp_out: buffer<vec4>;\n"
    "buffer casc_out: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx = i32(global_id);\n"
    "    let n_total = i32(u1.w);\n"
    "    if (idx < n_total) {\n"
    "        let v01 = in01[idx];\n"
    "        let v23 = in23[idx];\n"
    "        let h   = v01.x;\n"
    "        let dxz = v01.y;\n"
    "        let dx  = v01.z;\n"
    "        let dz  = v01.w;\n"
    "        let sx  = v23.x;\n"
    "        let sz  = v23.y;\n"
    "        let dxx = v23.z;\n"
    "        let dzz = v23.w;\n"
    "        let lam = u0.x;\n"
    "        let jac = (1.0 + lam * dxx) * (1.0 + lam * dzz) - (lam * dxz) * (lam * dxz);\n"
    "        var gen: f32 = 0.0;\n"
    "        if (jac < u0.y) {\n"
    "            gen = clamp((u0.y - jac) * u0.z, 0.0, 1.0);\n"
    "        }\n"
    "        let prev_fm = foam_buf[idx];\n"
    "        let fm = clamp(prev_fm * u0.w + gen * u1.x, 0.0, 1.0);\n"
    "        foam_buf[idx] = fm;\n"
    "        disp_out[idx] = vec4(lam * dx, h, lam * dz, jac);\n"
    "        let r_f = clamp(0.50196078 + sx * u1.y, 0.00392157, 1.0);\n"
    "        let g_f = clamp(0.50196078 + sz * u1.y, 0.00392157, 1.0);\n"
    "        let b_f = clamp(h * u1.z * 0.5 + 0.5, 0.0, 1.0);\n"
    "        casc_out[idx] = vec4(r_f, g_f, b_f, fm);\n"
    "    }\n"
    "}\n";

/* 4. Hesaplanan yer değiştirme ve eğim/köpük alanlarını GPU storage dokularına (rgba16float) yazar. */
static const char k_wgsl_ocean_to_tex[] =
    "struct TexInfo {\n"
    "    info: vec4<u32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u_info: TexInfo;\n"
    "@group(0) @binding(1) var<storage, read_write> disp_in: array<vec4<f32>>;\n"
    "@group(0) @binding(2) var<storage, read_write> casc_in: array<vec4<f32>>;\n"
    "@group(0) @binding(3) var disp_tex: texture_storage_2d<rgba16float, write>;\n"
    "@group(0) @binding(4) var casc_tex: texture_storage_2d<rgba16float, write>;\n"
    "@compute @workgroup_size(64)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    let idx = gid.x;\n"
    "    if (idx >= u_info.info.y) { return; }\n"
    "    let n = u_info.info.x;\n"
    "    let x = i32(idx & (n - 1u));\n"
    "    let y = i32(idx / n);\n"
    "    let coord = vec2<i32>(x, y);\n"
    "    textureStore(disp_tex, coord, disp_in[idx]);\n"
    "    textureStore(casc_tex, coord, casc_in[idx]);\n"
    "}\n";

/* 5. Kamera merkezli geometrik okyanus ızgarası köşe gölgelendiricisi:
 * 3 kademeli yer değiştirme + eğim storage dokularından (disp_tex_0..2, casc_tex_0..2)
 * uzaklık ağırlıklı örnekleme yaparak köşe konumunu ve normalini GPU'da hesaplar. */
static const char k_wgsl_ocean_vs[] =
    "struct OceanVsUniforms {\n"
    "    mvp: mat4x4<f32>,\n"
    "    eye_snap: vec4<f32>,\n"
    "    eye_pos: vec4<f32>,\n"
    "    casc_scales: vec4<f32>,\n"
    "};\n"
    "@group(1) @binding(0) var<uniform> vs_u: OceanVsUniforms;\n"
    "@group(1) @binding(1) var disp_tex_0: texture_2d<f32>;\n"
    "@group(1) @binding(2) var disp_tex_1: texture_2d<f32>;\n"
    "@group(1) @binding(3) var disp_tex_2: texture_2d<f32>;\n"
    "@group(1) @binding(4) var casc_tex_0: texture_2d<f32>;\n"
    "@group(1) @binding(5) var casc_tex_1: texture_2d<f32>;\n"
    "@group(1) @binding(6) var casc_tex_2: texture_2d<f32>;\n"
    "@group(1) @binding(7) var disp_samp: sampler;\n"
    "struct VsIn {\n"
    "    @location(0) xz: vec2<f32>,\n"
    "};\n"
    "struct VsOut {\n"
    "    @builtin(position) clip_pos: vec4<f32>,\n"
    "    @location(0) v_uv: vec2<f32>,\n"
    "    @location(1) v_nrm: vec3<f32>,\n"
    "    @location(2) v_depth: f32,\n"
    "    @location(3) v_wpos: vec3<f32>,\n"
    "};\n"
    "fn smooth_falloff(d: f32, r0: f32, r1: f32) -> f32 {\n"
    "    let t = clamp((d - r0) / (r1 - r0), 0.0, 1.0);\n"
    "    return 1.0 - t * t * (3.0 - 2.0 * t);\n"
    "}\n"
    "@vertex\n"
    "fn vert(vin: VsIn) -> VsOut {\n"
    "    let wx = vs_u.eye_snap.x + vin.xz.x;\n"
    "    let wz = vs_u.eye_snap.y + vin.xz.y;\n"
    "    let dx_e = wx - vs_u.eye_pos.x;\n"
    "    let dz_e = wz - vs_u.eye_pos.z;\n"
    "    let dist = sqrt(dx_e * dx_e + dz_e * dz_e);\n"
    "    let w0 = smooth_falloff(dist, 450.0, 1750.0);\n"
    "    let w1 = smooth_falloff(dist, 55.0, 180.0);\n"
    "    let w2 = smooth_falloff(dist, 12.0, 42.0);\n"
    "    let uv0 = vec2<f32>(wx, wz) * vs_u.casc_scales.x;\n"
    "    let uv1 = vec2<f32>(wx, wz) * vs_u.casc_scales.y;\n"
    "    let uv2 = vec2<f32>(wx, wz) * vs_u.casc_scales.z;\n"
    "    let d0 = textureSampleLevel(disp_tex_0, disp_samp, uv0, 0.0).xyz;\n"
    "    let d1 = textureSampleLevel(disp_tex_1, disp_samp, uv1, 0.0).xyz;\n"
    "    let d2 = textureSampleLevel(disp_tex_2, disp_samp, uv2, 0.0).xyz;\n"
    "    let disp = d0 * w0 + d1 * w1 + d2 * w2;\n"
    "    let zc = vec2<f32>(0.50196078, 0.50196078);\n"
    "    let s0 = textureSampleLevel(casc_tex_0, disp_samp, uv0, 0.0).xy - zc;\n"
    "    let s1 = textureSampleLevel(casc_tex_1, disp_samp, uv1, 0.0).xy - zc;\n"
    "    let s2 = textureSampleLevel(casc_tex_2, disp_samp, uv2, 0.0).xy - zc;\n"
    "    let slope = (s0 * w0 + s1 * w1 + s2 * w2) * (vs_u.eye_pos.w * 2.0);\n"
    "    let nrm = normalize(vec3<f32>(-slope.x, 1.0, -slope.y));\n"
    "    let wpos = vec3<f32>(wx + disp.x, disp.y, wz + disp.z);\n"
    "    let clip = vs_u.mvp * vec4<f32>(wpos, 1.0);\n"
    "    var vout: VsOut;\n"
    "    vout.clip_pos = clip;\n"
    "    vout.v_uv     = uv0;\n"
    "    vout.v_nrm    = nrm;\n"
    "    vout.v_depth  = clip.w;\n"
    "    vout.v_wpos   = wpos;\n"
    "    return vout;\n"
    "}\n";

/* ---------- V1: Hillaire 2020 Atmosfer LUT + 3B Perlin-Worley Bulut Gürültüsü (@compute) ---------- */

static const char k_mtk_atmos_transmittance[] =
    "uniform u_lut_size: vec4;\n"
    "buffer out_trans: buffer<vec4>;\n"
    "const LOG2_E: f32 = 1.44269504;\n"
    "fn compute() {\n"
    "    let gid: u32 = global_id;\n"
    "    let lut: vec4 = u_lut_size;\n"
    "    let w: u32    = u32(lut.x);\n"
    "    let h: u32    = u32(lut.y);\n"
    "    if gid >= w * h { return; }\n"
    "    let y: u32 = u32(floor((f32(gid) + 0.5) / lut.x));\n"
    "    let x: u32 = gid - y * w;\n"
    "    let u: f32 = (f32(x) + 0.5) / lut.x;\n"
    "    let v: f32 = (f32(y) + 0.5) / lut.y;\n"
    "    let r_bot: f32 = lut.z;\n"
    "    let r_top: f32 = lut.w;\n"
    "    let mu: f32   = -0.25 + u * 1.25;\n"
    "    let h_km: f32 = v * v * (r_top - r_bot);\n"
    "    let r: f32    = clamp(r_bot + h_km, r_bot + 0.001, r_top - 0.001);\n"
    "    let disc: f32  = max(0.0, r * r * (mu * mu - 1.0) + r_top * r_top);\n"
    "    let s_max: f32 = max(0.0, -r * mu + sqrt(disc));\n"
    "    let ds: f32    = s_max * 0.08333333;\n"
    "    let beta_r: vec3 = vec3(0.005802, 0.013558, 0.033100);\n"
    "    let beta_m: vec3 = vec3(0.004440, 0.004440, 0.004440);\n"
    "    let beta_o: vec3 = vec3(0.000650, 0.001881, 0.000085);\n"
    "    var od_r: f32 = 0.0;\n"
    "    var od_m: f32 = 0.0;\n"
    "    var od_o: f32 = 0.0;\n"
    "    for i in 0..12 {\n"
    "        let s: f32   = (f32(i) + 0.5) * ds;\n"
    "        let r_s: f32 = sqrt(r * r + s * s + 2.0 * r * s * mu);\n"
    "        let hs: f32  = max(r_s - r_bot, 0.0);\n"
    "        od_r += exp2((-hs * 0.125) * LOG2_E) * ds;\n"
    "        od_m += exp2((-hs * 0.83333333) * LOG2_E) * ds;\n"
    "        od_o += max(0.0, 1.0 - abs(hs - 25.0) * 0.06666667) * ds;\n"
    "    }\n"
    "    let tau: vec3 = beta_r * od_r + beta_m * od_m + beta_o * od_o;\n"
    "    out_trans[gid] = vec4(exp2(-tau * LOG2_E), 1.0);\n"
    "}\n";

static const char k_mtk_atmos_multiscatter[] =
    "uniform u_ms_params: vec4;\n"
    "uniform u_trans_dim: vec4;\n"
    "buffer trans_lut: buffer<vec4>;\n"
    "buffer out_ms:    buffer<vec4>;\n"
    "const ISO_PHASE: f32 = 0.07957747;\n"
    "fn fast_exp_neg(x: f32) -> f32 {\n"
    "    let u: f32 = max(x, 0.0) * 0.5;\n"
    "    let d: f32 = 1.0 + u * (1.0 + u * 0.5);\n"
    "    return 1.0 / (d * d);\n"
    "}\n"
    "fn ray_dist(r: f32, mu: f32, r_bot: f32, r_top: f32) -> f32 {\n"
    "    let db: f32    = r * r * (mu * mu - 1.0) + r_bot * r_bot;\n"
    "    let d_bot: f32 = max(0.0, -r * mu - sqrt(max(0.0, db)));\n"
    "    let dt: f32    = max(0.0, r * r * (mu * mu - 1.0) + r_top * r_top);\n"
    "    let d_top: f32 = max(0.0, -r * mu + sqrt(dt));\n"
    "    let hit_g: f32 = step(0.0, -mu) * step(0.0, db);\n"
    "    return mix(d_top, d_bot, hit_g);\n"
    "}\n"
    "fn compute() {\n"
    "    let gid: u32  = global_id;\n"
    "    let mp: vec4  = u_ms_params;\n"
    "    let td: vec4  = u_trans_dim;\n"
    "    let w: u32    = u32(mp.x);\n"
    "    let h: u32    = u32(mp.y);\n"
    "    if gid >= w * h { return; }\n"
    "    let y: u32 = u32(floor((f32(gid) + 0.5) / mp.x));\n"
    "    let x: u32 = gid - y * w;\n"
    "    let u: f32 = (f32(x) + 0.5) / mp.x;\n"
    "    let v: f32 = (f32(y) + 0.5) / mp.y;\n"
    "    let r_bot: f32 = mp.z;\n"
    "    let r_top: f32 = mp.w;\n"
    "    let mu_s: f32  = -0.2 + u * 1.2;\n"
    "    let h_km: f32  = v * v * (r_top - r_bot);\n"
    "    let r: f32     = clamp(r_bot + h_km, r_bot + 0.01, r_top - 0.01);\n"
    "    let tw: f32    = td.x;\n"
    "    let th: f32    = td.y;\n"
    "    let tw_u: u32  = u32(tw);\n"
    "    let u_sun: f32 = clamp((mu_s + 0.25) * 0.8, 0.0, 1.0);\n"
    "    let ix_s: u32  = u32(clamp(floor(u_sun * tw), 0.0, tw - 1.0));\n"
    "    let beta_r: vec3  = vec3(0.005802, 0.013558, 0.033100);\n"
    "    let beta_ms: vec3 = vec3(0.003996, 0.003996, 0.003996);\n"
    "    let beta_ma: vec3 = vec3(0.000444, 0.000444, 0.000444);\n"
    "    let beta_o: vec3  = vec3(0.000650, 0.001881, 0.000085);\n"
    "    let one3: vec3    = vec3(1.0, 1.0, 1.0);\n"
    "    let r_bot2: f32   = r_bot * r_bot;\n"
    "    var l_2nd: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    var f_ms:  vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    for d in 0..2 {\n"
    "        let mu_v: f32  = -0.55 + f32(d) * 1.10;\n"
    "        let s_max: f32 = ray_dist(r, mu_v, r_bot, r_top);\n"
    "        let ds: f32    = s_max * 0.2;\n"
    "        var trans: vec3 = one3;\n"
    "        var dir_l: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "        var dir_f: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "        for i in 0..5 {\n"
    "            let s: f32   = (f32(i) + 0.5) * ds;\n"
    "            let r_s: f32 = sqrt(r * r + s * s + 2.0 * r * s * mu_v);\n"
    "            let h_s: f32 = max(0.0, r_s - r_bot);\n"
    "            let d_r: f32 = fast_exp_neg(h_s * 0.125);\n"
    "            let d_m: f32 = fast_exp_neg(h_s * 0.83333333);\n"
    "            let d_o: f32 = max(0.0, 1.0 - abs(h_s - 25.0) * 0.06666667);\n"
    "            let scat: vec3   = beta_r * d_r + beta_ms * d_m;\n"
    "            let ext: vec3    = scat + beta_ma * d_m + beta_o * d_o;\n"
    "            let tau_s: vec3  = ext * ds;\n"
    "            let step_t: vec3 = one3 / (one3 + tau_s * (one3 + tau_s * 0.5));\n"
    "            let sint: vec3   = (one3 + tau_s * 0.5) * step_t * ds;\n"
    "            let mu_hor: f32  = -sqrt(max(0.0, 1.0 - r_bot2 / (r_s * r_s)));\n"
    "            let sun_vis: f32 = step(mu_hor, mu_s);\n"
    "            let v_tr: f32    = sqrt(clamp(h_s * 0.01, 0.0, 1.0));\n"
    "            let iy_s: u32    = u32(clamp(floor(v_tr * th), 0.0, th - 1.0));\n"
    "            let t_sun: vec3  = trans_lut[iy_s * tw_u + ix_s].xyz * (sun_vis * ISO_PHASE);\n"
    "            let w_s: vec3    = trans * scat * sint;\n"
    "            dir_f += w_s;\n"
    "            dir_l += w_s * t_sun;\n"
    "            trans  = trans * step_t;\n"
    "        }\n"
    "        let hit_g: f32   = step(0.0, -mu_v) * step(0.0, mu_s);\n"
    "        let t_sun_g: vec3 = trans_lut[ix_s].xyz;\n"
    "        let g_alb: vec3   = vec3(0.04, 0.07, 0.12) * td.z;\n"
    "        dir_l += trans * t_sun_g * g_alb * (max(mu_s, 0.0) * 0.31830988 * hit_g);\n"
    "        l_2nd += dir_l * 0.5;\n"
    "        f_ms  += dir_f * 0.5;\n"
    "    }\n"
    "    out_ms[gid] = vec4(l_2nd / max(one3 - f_ms, vec3(0.05, 0.05, 0.05)), 1.0);\n"
    "}\n";

static const char k_mtk_atmos_skyview[] =
    "uniform u_sky_params: vec4;\n"
    "uniform u_sun_dir:    vec4;\n"
    "uniform u_lut_dims:   vec4;\n"
    "buffer trans_lut: buffer<vec4>;\n"
    "buffer ms_lut:    buffer<vec4>;\n"
    "buffer out_sky:   buffer<vec4>;\n"
    "const PI:       f32 = 3.14159265;\n"
    "const HALF_PI:  f32 = 1.57079633;\n"
    "const R_BOTTOM: f32 = 6360.0;\n"
    "const R_TOP:    f32 = 6460.0;\n"
    "fn fast_exp_neg(x: f32) -> f32 {\n"
    "    let u: f32 = max(x, 0.0) * 0.5;\n"
    "    let d: f32 = 1.0 + u * (1.0 + u * 0.5);\n"
    "    return 1.0 / (d * d);\n"
    "}\n"
    "fn ray_dist(r: f32, mu: f32) -> f32 {\n"
    "    let db: f32    = r * r * (mu * mu - 1.0) + R_BOTTOM * R_BOTTOM;\n"
    "    let d_bot: f32 = max(0.0, -r * mu - sqrt(max(0.0, db)));\n"
    "    let dt: f32    = max(0.0, r * r * (mu * mu - 1.0) + R_TOP * R_TOP);\n"
    "    let d_top: f32 = max(0.0, -r * mu + sqrt(dt));\n"
    "    let hit_g: f32 = step(0.0, -mu) * step(0.0, db);\n"
    "    return mix(d_top, d_bot, hit_g);\n"
    "}\n"
    "fn phase_rayleigh(cos_t: f32) -> f32 {\n"
    "    return 0.05968310 * (1.0 + cos_t * cos_t);\n"
    "}\n"
    "fn phase_mie(cos_t: f32, g: f32) -> f32 {\n"
    "    let g2: f32   = g * g;\n"
    "    let num: f32  = 3.0 * (1.0 - g2) * (1.0 + cos_t * cos_t);\n"
    "    let base: f32 = max(1.0 + g2 - 2.0 * g * cos_t, 0.0001);\n"
    "    return num / (25.1327412 * (2.0 + g2) * base * sqrt(base));\n"
    "}\n"
    "fn compute() {\n"
    "    let gid: u32  = global_id;\n"
    "    let sp: vec4  = u_sky_params;\n"
    "    let sd: vec4  = u_sun_dir;\n"
    "    let ld: vec4  = u_lut_dims;\n"
    "    let w: u32    = u32(sp.x);\n"
    "    let h: u32    = u32(sp.y);\n"
    "    if gid >= w * h { return; }\n"
    "    let y: u32 = u32(floor((f32(gid) + 0.5) / sp.x));\n"
    "    let x: u32 = gid - y * w;\n"
    "    let u: f32 = (f32(x) + 0.5) / sp.x;\n"
    "    let v: f32 = (f32(y) + 0.5) / sp.y;\n"
    "    let azim: f32 = (u - 0.5) * 2.0 * PI;\n"
    "    let sn: f32   = step(0.5, v) * 2.0 - 1.0;\n"
    "    let dv: f32   = (v - 0.5) * 2.0;\n"
    "    let elev: f32 = sn * dv * dv * HALF_PI;\n"
    "    let ce: f32       = cos(elev);\n"
    "    let ray_dir: vec3 = vec3(ce * cos(azim), sin(elev), ce * sin(azim));\n"
    "    let sun_dir: vec3 = normalize(sd.xyz);\n"
    "    let r: f32     = clamp(R_BOTTOM + sp.z, R_BOTTOM + 0.002, R_TOP - 0.01);\n"
    "    let s_max: f32 = min(ray_dist(r, ray_dir.y), 350.0);\n"
    "    let ds: f32    = s_max * 0.1;\n"
    "    let cos_t: f32 = dot(ray_dir, sun_dir);\n"
    "    let pr: f32    = phase_rayleigh(cos_t);\n"
    "    let pm: f32    = phase_mie(cos_t, sp.w);\n"
    "    let tw: f32    = ld.x;\n"
    "    let th: f32    = ld.y;\n"
    "    let mw: f32    = ld.z;\n"
    "    let mh: f32    = ld.w;\n"
    "    let tw_u: u32  = u32(tw);\n"
    "    let mw_u: u32  = u32(mw);\n"
    "    let beta_r: vec3  = vec3(0.005802, 0.013558, 0.033100);\n"
    "    let beta_ms: vec3 = vec3(0.003996, 0.003996, 0.003996);\n"
    "    let beta_me: vec3 = vec3(0.004440, 0.004440, 0.004440);\n"
    "    let beta_o: vec3  = vec3(0.000650, 0.001881, 0.000085);\n"
    "    let one3: vec3    = vec3(1.0, 1.0, 1.0);\n"
    "    let r_bot2: f32   = R_BOTTOM * R_BOTTOM;\n"
    "    var trans: vec3 = one3;\n"
    "    var lum:   vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    for i in 0..10 {\n"
    "        let s: f32     = (f32(i) + 0.5) * ds;\n"
    "        let pos: vec3  = vec3(ray_dir.x * s, r + ray_dir.y * s, ray_dir.z * s);\n"
    "        let r_s: f32   = length(pos);\n"
    "        let h_s: f32   = max(0.0, r_s - R_BOTTOM);\n"
    "        let mu_s: f32  = dot(pos, sun_dir) / max(r_s, 1.0);\n"
    "        let d_r: f32 = fast_exp_neg(h_s * 0.125);\n"
    "        let d_m: f32 = fast_exp_neg(h_s * 0.83333333);\n"
    "        let d_o: f32 = max(0.0, 1.0 - abs(h_s - 25.0) * 0.06666667);\n"
    "        let sr: vec3  = beta_r * d_r;\n"
    "        let sm: vec3  = beta_ms * d_m;\n"
    "        let ext: vec3 = sr + beta_me * d_m + beta_o * d_o;\n"
    "        let tau_s: vec3  = ext * ds;\n"
    "        let step_t: vec3 = one3 / (one3 + tau_s * (one3 + tau_s * 0.5));\n"
    "        let sint: vec3   = (one3 + tau_s * 0.5) * step_t * ds;\n"
    "        let mu_hor: f32  = -sqrt(max(0.0, 1.0 - r_bot2 / (r_s * r_s)));\n"
    "        let sun_vis: f32 = step(mu_hor, mu_s);\n"
    "        let vh: f32      = sqrt(clamp(h_s * 0.01, 0.0, 1.0));\n"
    "        let ut: f32      = clamp((mu_s + 0.25) * 0.8, 0.0, 1.0);\n"
    "        let ixt: u32     = u32(clamp(floor(ut * tw), 0.0, tw - 1.0));\n"
    "        let iyt: u32     = u32(clamp(floor(vh * th), 0.0, th - 1.0));\n"
    "        let t_sun: vec3  = trans_lut[iyt * tw_u + ixt].xyz * sun_vis;\n"
    "        let um: f32      = clamp((mu_s + 0.2) * 0.83333333, 0.0, 1.0);\n"
    "        let ixm: u32     = u32(clamp(floor(um * mw), 0.0, mw - 1.0));\n"
    "        let iym: u32     = u32(clamp(floor(vh * mh), 0.0, mh - 1.0));\n"
    "        let psi_ms: vec3 = ms_lut[iym * mw_u + ixm].xyz;\n"
    "        let single_s: vec3 = (sr * pr + sm * pm) * t_sun;\n"
    "        let multi_s: vec3  = (sr + sm) * psi_ms;\n"
    "        lum   += trans * (single_s + multi_s) * sint;\n"
    "        trans  = trans * step_t;\n"
    "    }\n"
    "    out_sky[gid] = vec4(lum * sd.w, 1.0);\n"
    "}\n";

static const char k_mtk_atmos_aerial[] =
    "uniform u_ap_dims:  vec4;\n"
    "uniform u_sun_dir:  vec4;\n"
    "uniform u_lut_dims: vec4;\n"
    "buffer trans_lut:  buffer<vec4>;\n"
    "buffer ms_lut:     buffer<vec4>;\n"
    "buffer out_aerial: buffer<vec4>;\n"
    "const R_BOTTOM: f32 = 6360.0;\n"
    "fn fast_exp_neg(x: f32) -> f32 {\n"
    "    let u: f32 = max(x, 0.0) * 0.5;\n"
    "    let d: f32 = 1.0 + u * (1.0 + u * 0.5);\n"
    "    return 1.0 / (d * d);\n"
    "}\n"
    "fn phase_rayleigh(cos_t: f32) -> f32 {\n"
    "    return 0.05968310 * (1.0 + cos_t * cos_t);\n"
    "}\n"
    "fn phase_mie(cos_t: f32, g: f32) -> f32 {\n"
    "    let g2: f32   = g * g;\n"
    "    let num: f32  = 3.0 * (1.0 - g2) * (1.0 + cos_t * cos_t);\n"
    "    let base: f32 = max(1.0 + g2 - 2.0 * g * cos_t, 0.0001);\n"
    "    return num / (25.1327412 * (2.0 + g2) * base * sqrt(base));\n"
    "}\n"
    "fn compute() {\n"
    "    let gid: u32  = global_id;\n"
    "    let ap: vec4  = u_ap_dims;\n"
    "    let sd: vec4  = u_sun_dir;\n"
    "    let ld: vec4  = u_lut_dims;\n"
    "    let w: u32    = u32(ap.x);\n"
    "    let h: u32    = u32(ap.y);\n"
    "    let d: u32    = u32(ap.z);\n"
    "    let wh: u32   = w * h;\n"
    "    if gid >= wh * d { return; }\n"
    "    let z: u32   = u32(floor((f32(gid) + 0.5) / f32(wh)));\n"
    "    let rem: u32 = gid - z * wh;\n"
    "    let y: u32   = u32(floor((f32(rem) + 0.5) / ap.x));\n"
    "    let x: u32   = rem - y * w;\n"
    "    let u: f32 = (f32(x) + 0.5) / ap.x;\n"
    "    let v: f32 = (f32(y) + 0.5) / ap.y;\n"
    "    let azim: f32     = (u - 0.5) * 6.2831853;\n"
    "    let elev: f32     = -0.15 + v * 0.60;\n"
    "    let ce: f32       = cos(elev);\n"
    "    let ray_dir: vec3 = vec3(ce * cos(azim), sin(elev), ce * sin(azim));\n"
    "    let sun_dir: vec3 = normalize(sd.xyz);\n"
    "    let max_dist: f32 = (f32(z) + 1.0) * ap.w;\n"
    "    let ds: f32       = max_dist * 0.16666667;\n"
    "    let r: f32        = R_BOTTOM + 0.02;\n"
    "    let cos_t: f32 = dot(ray_dir, sun_dir);\n"
    "    let pr: f32    = phase_rayleigh(cos_t);\n"
    "    let pm: f32    = phase_mie(cos_t, 0.80);\n"
    "    let tw: f32    = ld.x;\n"
    "    let th: f32    = ld.y;\n"
    "    let mw: f32    = ld.z;\n"
    "    let mh: f32    = ld.w;\n"
    "    let tw_u: u32  = u32(tw);\n"
    "    let mw_u: u32  = u32(mw);\n"
    "    let beta_r: vec3  = vec3(0.005802, 0.013558, 0.033100);\n"
    "    let beta_ms: vec3 = vec3(0.003996, 0.003996, 0.003996);\n"
    "    let beta_me: vec3 = vec3(0.004440, 0.004440, 0.004440);\n"
    "    let beta_o: vec3  = vec3(0.000650, 0.001881, 0.000085);\n"
    "    let one3: vec3    = vec3(1.0, 1.0, 1.0);\n"
    "    var trans: vec3 = one3;\n"
    "    var lum:   vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    for i in 0..6 {\n"
    "        let s: f32     = (f32(i) + 0.5) * ds;\n"
    "        let pos: vec3  = vec3(ray_dir.x * s, r + ray_dir.y * s, ray_dir.z * s);\n"
    "        let r_s: f32   = length(pos);\n"
    "        let h_s: f32   = max(0.0, r_s - R_BOTTOM);\n"
    "        let mu_s: f32  = dot(pos, sun_dir) / max(r_s, 1.0);\n"
    "        let d_r: f32 = fast_exp_neg(h_s * 0.125);\n"
    "        let d_m: f32 = fast_exp_neg(h_s * 0.83333333);\n"
    "        let d_o: f32 = max(0.0, 1.0 - abs(h_s - 25.0) * 0.06666667);\n"
    "        let sr: vec3  = beta_r * d_r;\n"
    "        let sm: vec3  = beta_ms * d_m;\n"
    "        let ext: vec3 = sr + beta_me * d_m + beta_o * d_o;\n"
    "        let tau_s: vec3  = ext * ds;\n"
    "        let step_t: vec3 = one3 / (one3 + tau_s * (one3 + tau_s * 0.5));\n"
    "        let sint: vec3   = (one3 + tau_s * 0.5) * step_t * ds;\n"
    "        let vh: f32      = sqrt(clamp(h_s * 0.01, 0.0, 1.0));\n"
    "        let ut: f32      = clamp((max(mu_s, -0.15) + 0.25) * 0.8, 0.0, 1.0);\n"
    "        let ixt: u32     = u32(clamp(floor(ut * tw), 0.0, tw - 1.0));\n"
    "        let iyt: u32     = u32(clamp(floor(vh * th), 0.0, th - 1.0));\n"
    "        let t_sun: vec3  = trans_lut[iyt * tw_u + ixt].xyz;\n"
    "        let um: f32      = clamp((mu_s + 0.2) * 0.83333333, 0.0, 1.0);\n"
    "        let ixm: u32     = u32(clamp(floor(um * mw), 0.0, mw - 1.0));\n"
    "        let iym: u32     = u32(clamp(floor(vh * mh), 0.0, mh - 1.0));\n"
    "        let psi_ms: vec3 = ms_lut[iym * mw_u + ixm].xyz;\n"
    "        let s_tot: vec3  = (sr * pr + sm * pm) * t_sun + (sr + sm) * psi_ms;\n"
    "        lum   += trans * s_tot * sint;\n"
    "        trans  = trans * step_t;\n"
    "    }\n"
    "    let avg_t: f32 = (trans.x + trans.y + trans.z) * 0.33333334;\n"
    "    out_aerial[gid] = vec4(lum * sd.w, avg_t);\n"
    "}\n";

static const char k_mtk_cloud_noise[] =
    "uniform u_noise_dim: vec4;\n"
    "buffer out_noise: buffer<vec4>;\n"
    "fn hash1(x: u32, y: u32, z: u32, mask: u32, seed: u32) -> f32 {\n"
    "    let ux: u32 = x & mask;\n"
    "    let uy: u32 = y & mask;\n"
    "    let uz: u32 = z & mask;\n"
    "    var h: u32  = ux * 374761393 + uy * 668265263 + uz * 2147483647 + seed * 1664525;\n"
    "    h = (h ^ (h >> 13)) * 1274126177;\n"
    "    h = h ^ (h >> 16);\n"
    "    return f32(h & 1023) * 0.0009775171;\n"
    "}\n"
    "fn hash3(x: u32, y: u32, z: u32, mask: u32, seed: u32) -> vec3 {\n"
    "    let ux: u32 = x & mask;\n"
    "    let uy: u32 = y & mask;\n"
    "    let uz: u32 = z & mask;\n"
    "    var h: u32  = ux * 374761393 + uy * 668265263 + uz * 2147483647 + seed * 1664525;\n"
    "    h = (h ^ (h >> 13)) * 1274126177;\n"
    "    h = h ^ (h >> 16);\n"
    "    return vec3(f32(h & 1023), f32((h >> 10) & 1023), f32((h >> 20) & 1023)) * 0.0009775171;\n"
    "}\n"
    "fn worley3(uvw: vec3, freq: f32, mask: u32, seed: u32) -> f32 {\n"
    "    let p: vec3  = uvw * freq;\n"
    "    let ps: vec3 = p - vec3(0.5);\n"
    "    let bx: i32  = i32(floor(ps.x));\n"
    "    let by: i32  = i32(floor(ps.y));\n"
    "    let bz: i32  = i32(floor(ps.z));\n"
    "    var min_d2: f32 = 4.0;\n"
    "    for c in 0..8 {\n"
    "        let cx: i32 = bx + (c & 1);\n"
    "        let cy: i32 = by + ((c >> 1) & 1);\n"
    "        let cz: i32 = bz + ((c >> 2) & 1);\n"
    "        let rnd: vec3  = hash3(u32(cx + 64), u32(cy + 64), u32(cz + 64), mask, seed);\n"
    "        let feat: vec3 = vec3(f32(cx), f32(cy), f32(cz)) + vec3(0.15) + rnd * 0.70;\n"
    "        let diff: vec3 = feat - p;\n"
    "        min_d2 = min(min_d2, dot(diff, diff));\n"
    "    }\n"
    "    return clamp(1.0 - sqrt(min_d2), 0.0, 1.0);\n"
    "}\n"
    "fn smooth_noise3(uvw: vec3, freq: f32, mask: u32, seed: u32) -> f32 {\n"
    "    let p: vec3  = uvw * freq;\n"
    "    let ip: vec3 = floor(p);\n"
    "    let f: vec3  = p - ip;\n"
    "    let u: vec3  = f * f * (vec3(3.0) - f * 2.0);\n"
    "    let ix: u32 = u32(i32(ip.x) + 64);\n"
    "    let iy: u32 = u32(i32(ip.y) + 64);\n"
    "    let iz: u32 = u32(i32(ip.z) + 64);\n"
    "    let n000: f32 = hash1(ix,     iy,     iz,     mask, seed);\n"
    "    let n100: f32 = hash1(ix + 1, iy,     iz,     mask, seed);\n"
    "    let n010: f32 = hash1(ix,     iy + 1, iz,     mask, seed);\n"
    "    let n110: f32 = hash1(ix + 1, iy + 1, iz,     mask, seed);\n"
    "    let n001: f32 = hash1(ix,     iy,     iz + 1, mask, seed);\n"
    "    let n101: f32 = hash1(ix + 1, iy,     iz + 1, mask, seed);\n"
    "    let n011: f32 = hash1(ix,     iy + 1, iz + 1, mask, seed);\n"
    "    let n111: f32 = hash1(ix + 1, iy + 1, iz + 1, mask, seed);\n"
    "    let nx00: f32 = mix(n000, n100, u.x);\n"
    "    let nx10: f32 = mix(n010, n110, u.x);\n"
    "    let nx01: f32 = mix(n001, n101, u.x);\n"
    "    let nx11: f32 = mix(n011, n111, u.x);\n"
    "    return mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z);\n"
    "}\n"
    "fn compute() {\n"
    "    let gid: u32 = global_id;\n"
    "    let n: u32   = u32(u_noise_dim.x);\n"
    "    let nn: u32  = n * n;\n"
    "    if gid >= nn * n { return; }\n"
    "    let z: u32   = u32(floor((f32(gid) + 0.5) / f32(nn)));\n"
    "    let rem: u32 = gid - z * nn;\n"
    "    let y: u32   = u32(floor((f32(rem) + 0.5) / u_noise_dim.x));\n"
    "    let x: u32   = rem - y * n;\n"
    "    let uvw: vec3 = (vec3(f32(x), f32(y), f32(z)) + vec3(0.5)) * u_noise_dim.y;\n"
    "    let p0: f32 = smooth_noise3(uvw, 4.0, 3, 11);\n"
    "    let p1: f32 = smooth_noise3(uvw, 8.0, 7, 23);\n"
    "    let p_fbm: f32 = clamp(p0 * 0.65 + p1 * 0.35, 0.0, 1.0);\n"
    "    let w0: f32 = worley3(uvw, 4.0,  3,  101);\n"
    "    let w1: f32 = worley3(uvw, 8.0,  7,  211);\n"
    "    let w2: f32 = worley3(uvw, 16.0, 15, 307);\n"
    "    let w_fbm: f32 = clamp(w0 * 0.625 + w1 * 0.25 + w2 * 0.125, 0.0, 1.0);\n"
    "    let lo: f32 = 1.0 - w_fbm;\n"
    "    let pw: f32 = clamp((p_fbm - lo) / max(1.0 - lo, 0.001), 0.0, 1.0);\n"
    "    out_noise[gid] = vec4(pw, w_fbm, clamp(w1 * 0.65 + w2 * 0.35, 0.0, 1.0), w2);\n"
    "}\n";

/* Statik LUT ve 3B gürültü tamponlarını 2B/3B storage dokularına kopyalar */
static const char k_wgsl_static_to_tex[] =
    "@group(0) @binding(0) var<storage, read_write> trans_in: array<vec4<f32>>;\n"
    "@group(0) @binding(1) var<storage, read_write> ms_in:    array<vec4<f32>>;\n"
    "@group(0) @binding(2) var<storage, read_write> noise_in: array<vec4<f32>>;\n"
    "@group(0) @binding(3) var trans_tex: texture_storage_2d<rgba16float, write>;\n"
    "@group(0) @binding(4) var ms_tex:    texture_storage_2d<rgba16float, write>;\n"
    "@group(0) @binding(5) var noise_tex: texture_storage_3d<rgba16float, write>;\n"
    "@group(0) @binding(6) var hist0_tex: texture_storage_2d<rgba16float, write>;\n"
    "@group(0) @binding(7) var hist1_tex: texture_storage_2d<rgba16float, write>;\n"
    "@compute @workgroup_size(64)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    let idx = gid.x;\n"
    "    if (idx < 16384u) {\n"
    "        let tx = i32(idx & 255u);\n"
    "        let ty = i32(idx >> 8u);\n"
    "        textureStore(trans_tex, vec2<i32>(tx, ty), trans_in[idx]);\n"
    "    }\n"
    "    if (idx < 1024u) {\n"
    "        let mx = i32(idx & 31u);\n"
    "        let my = i32(idx >> 5u);\n"
    "        textureStore(ms_tex, vec2<i32>(mx, my), ms_in[idx]);\n"
    "    }\n"
    "    if (idx < 32768u) {\n"
    "        let nx = i32(idx & 31u);\n"
    "        let ny = i32((idx >> 5u) & 31u);\n"
    "        let nz = i32(idx >> 10u);\n"
    "        textureStore(noise_tex, vec3<i32>(nx, ny, nz), noise_in[idx]);\n"
    "    }\n"
    "    if (idx < 129600u) {\n"
    "        let cy = i32(idx / 480u);\n"
    "        let cx = i32(idx - u32(cy) * 480u);\n"
    "        textureStore(hist0_tex, vec2<i32>(cx, cy), vec4<f32>(0.0, 0.0, 0.0, 1.0));\n"
    "        textureStore(hist1_tex, vec2<i32>(cx, cy), vec4<f32>(0.0, 0.0, 0.0, 1.0));\n"
    "    }\n"
    "}\n";

/* Dinamik Sky-View (192x108) ve 3B Aerial Perspective (32x32x16) tamponlarını dokulara yazar */
static const char k_wgsl_dyn_to_tex[] =
    "@group(0) @binding(0) var<storage, read_write> sky_in:    array<vec4<f32>>;\n"
    "@group(0) @binding(1) var<storage, read_write> aerial_in: array<vec4<f32>>;\n"
    "@group(0) @binding(2) var sky_tex:    texture_storage_2d<rgba16float, write>;\n"
    "@group(0) @binding(3) var aerial_tex: texture_storage_3d<rgba16float, write>;\n"
    "@compute @workgroup_size(64)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    let idx = gid.x;\n"
    "    if (idx < 20736u) {\n"
    "        let sy = idx / 192u;\n"
    "        let sx = idx - sy * 192u;\n"
    "        textureStore(sky_tex, vec2<i32>(i32(sx), i32(sy)), sky_in[idx]);\n"
    "    }\n"
    "    if (idx < 16384u) {\n"
    "        let ax = i32(idx & 31u);\n"
    "        let ay = i32((idx >> 5u) & 31u);\n"
    "        let az = i32(idx >> 10u);\n"
    "        textureStore(aerial_tex, vec3<i32>(ax, ay, az), aerial_in[idx]);\n"
    "    }\n"
    "}\n";

/* Çeyrek çözünürlüklü (480x270) hacimsel kümülüs bulut raymarching + ışık yürüyüşü +
 * Beer-Powder zayıflaması + 3B Aerial Perspective + zamansal yeniden izdüşüm (temporal reprojection) */
static const char k_wgsl_cloud_march[] =
    "struct CloudUniforms {\n"
    "    prev_vp:    mat4x4<f32>,\n"
    "    cam_fwd:    vec4<f32>,\n"
    "    cam_right:  vec4<f32>,\n"
    "    cam_up:     vec4<f32>,\n"
    "    eye_time:   vec4<f32>,\n"
    "    sun_cov:    vec4<f32>,\n"
    "    moon_star:  vec4<f32>,\n"
    "    wind_frame: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> cu: CloudUniforms;\n"
    "@group(0) @binding(1) var noise_tex:  texture_3d<f32>;\n"
    "@group(0) @binding(2) var trans_tex:  texture_2d<f32>;\n"
    "@group(0) @binding(3) var sky_tex:    texture_2d<f32>;\n"
    "@group(0) @binding(4) var aerial_tex: texture_3d<f32>;\n"
    "@group(0) @binding(5) var hist_in:    texture_2d<f32>;\n"
    "@group(0) @binding(6) var rep_samp:   sampler;\n"
    "@group(0) @binding(7) var cloud_out:  texture_storage_2d<rgba16float, write>;\n"
    "fn sample_trans_lut(h_km: f32, mu: f32) -> vec3<f32> {\n"
    "    let u = clamp((mu + 0.25) / 1.25, 0.005, 0.995);\n"
    "    let v = clamp(sqrt(clamp(h_km * 0.01, 0.0, 1.0)), 0.012, 0.988);\n"
    "    return textureSampleLevel(trans_tex, rep_samp, vec2<f32>(u, v), 0.0).xyz;\n"
    "}\n"
    "fn sample_sky_lut(dir: vec3<f32>) -> vec3<f32> {\n"
    "    let azim = atan2(dir.z, dir.x);\n"
    "    let u    = fract(azim * 0.15915494 + 0.5);\n"
    "    let sy   = clamp(dir.y, -1.0, 1.0);\n"
    "    let elev = asin(sy);\n"
    "    let sgn  = select(-1.0, 1.0, elev >= 0.0);\n"
    "    let v    = clamp(0.5 + 0.5 * sgn * sqrt(abs(elev) / 1.57079633), 0.01, 0.99);\n"
    "    return textureSampleLevel(sky_tex, rep_samp, vec2<f32>(u, v), 0.0).xyz;\n"
    "}\n"
    "fn hg_phase(cos_t: f32, g: f32) -> f32 {\n"
    "    let g2 = g * g;\n"
    "    let d  = max(1.0 + g2 - 2.0 * g * cos_t, 0.001);\n"
    "    return 0.07957747 * (1.0 - g2) / (d * sqrt(d));\n"
    "}\n"
    "fn sample_cloud_density(pos_km: vec3<f32>, h_frac: f32, cov: f32, wind_ofs: vec3<f32>) -> f32 {\n"
    "    let h_grad = smoothstep(0.0, 0.18, h_frac) * smoothstep(1.0, 0.52, h_frac);\n"
    "    if (h_grad <= 0.001) { return 0.0; }\n"
    "    let uvw_b = pos_km * vec3<f32>(0.095, 0.22, 0.095) + wind_ofs;\n"
    "    let nw    = textureSampleLevel(noise_tex, rep_samp, uvw_b, 0.0);\n"
    "    let base  = nw.r * 0.68 + nw.g * 0.22 + nw.b * 0.10;\n"
    "    let thr   = clamp(1.0 - cov * 1.06, 0.14, 0.92);\n"
    "    let raw_d = clamp((base * h_grad - thr) / max(1.0 - thr, 0.05), 0.0, 1.0);\n"
    "    if (raw_d <= 0.001) { return 0.0; }\n"
    "    let uvw_d = pos_km * vec3<f32>(0.48, 0.75, 0.48) - wind_ofs * 1.35;\n"
    "    let nd    = textureSampleLevel(noise_tex, rep_samp, uvw_d, 0.0);\n"
    "    let erod  = (nd.g * 0.55 + nd.b * 0.30 + nd.a * 0.15) * (1.0 - h_frac * 0.60);\n"
    "    return max(0.0, raw_d - erod * 0.28) * 1.75;\n"
    "}\n"
    "@compute @workgroup_size(8, 8, 1)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    let qw = 480u;\n"
    "    let qh = 270u;\n"
    "    if (gid.x >= qw || gid.y >= qh) { return; }\n"
    "    let uv    = (vec2<f32>(f32(gid.x), f32(gid.y)) + vec2<f32>(0.5)) / vec2<f32>(480.0, 270.0);\n"
    "    let ndc_x = uv.x * 2.0 - 1.0;\n"
    "    let ndc_y = 1.0 - uv.y * 2.0;\n"
    "    let ray_dir = normalize(cu.cam_fwd.xyz + cu.cam_right.xyz * ndc_x + cu.cam_up.xyz * ndc_y);\n"
    "    let cov     = cu.sun_cov.w;\n"
    "    if (ray_dir.y <= 0.008 || cov <= 0.01) {\n"
    "        textureStore(cloud_out, vec2<i32>(i32(gid.x), i32(gid.y)), vec4<f32>(0.0, 0.0, 0.0, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    let r_e    = 6360.0;\n"
    "    let r_cam  = r_e + max(cu.eye_time.y * 0.001, 0.002);\n"
    "    let r_bot  = r_e + 1.45;\n"
    "    let r_top  = r_e + 4.15;\n"
    "    let mu     = ray_dir.y;\n"
    "    let t0_km  = max(0.0, -r_cam * mu + sqrt(max(0.0, r_cam * r_cam * (mu * mu - 1.0) + r_bot * r_bot)));\n"
    "    let t1_km  = min(30.0, -r_cam * mu + sqrt(max(0.0, r_cam * r_cam * (mu * mu - 1.0) + r_top * r_top)));\n"
    "    if (t1_km <= t0_km) {\n"
    "        textureStore(cloud_out, vec2<i32>(i32(gid.x), i32(gid.y)), vec4<f32>(0.0, 0.0, 0.0, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    let frame_u  = u32(cu.wind_frame.z);\n"
    "    let bayer_id = ((gid.x & 1u) + ((gid.y & 1u) << 1u) + frame_u) & 3u;\n"
    "    let jitter   = (f32(bayer_id) + 0.5) * 0.25;\n"
    "    let steps    = 14;\n"
    "    let ds       = (t1_km - t0_km) / 14.0;\n"
    "    let wind_ofs = vec3<f32>(cu.wind_frame.x, 0.0, cu.wind_frame.y) * (cu.eye_time.w * 0.0008);\n"
    "    let eye_km   = cu.eye_time.xyz * 0.001;\n"
    "    let sun_dir  = normalize(cu.sun_cov.xyz);\n"
    "    let moon_dir = normalize(cu.moon_star.xyz);\n"
    "    let night_w  = cu.moon_star.w;\n"
    "    let l_dir    = normalize(mix(sun_dir, moon_dir, night_w));\n"
    "    let cos_l    = dot(ray_dir, l_dir);\n"
    "    let phase    = mix(hg_phase(cos_l, 0.65), hg_phase(cos_l, -0.25), 0.32) + 0.08;\n"
    "    let amb_zen  = sample_sky_lut(vec3<f32>(0.0, 0.85, 0.52));\n"
    "    let amb_hor  = sample_sky_lut( normalize(vec3<f32>(sun_dir.x, 0.08, sun_dir.z)) );\n"
    "    var trans: f32       = 1.0;\n"
    "    var lum:   vec3<f32> = vec3<f32>(0.0);\n"
    "    var w_t:   f32       = 0.0;\n"
    "    var w_sum: f32       = 0.0;\n"
    "    for (var i: i32 = 0; i < steps; i = i + 1) {\n"
    "        if (trans < 0.02) { break; }\n"
    "        let t_km   = t0_km + (f32(i) + jitter) * ds;\n"
    "        let pos_km = eye_km + ray_dir * t_km;\n"
    "        let r_s    = length(vec3<f32>(ray_dir.x * t_km, r_cam + ray_dir.y * t_km, ray_dir.z * t_km));\n"
    "        let h_km   = r_s - r_e;\n"
    "        let h_frac = clamp((h_km - 1.45) / 2.70, 0.0, 1.0);\n"
    "        let dens   = sample_cloud_density(pos_km, h_frac, cov, wind_ofs);\n"
    "        if (dens > 0.001) {\n"
    "            var tau_l: f32 = 0.0;\n"
    "            for (var li: i32 = 1; li <= 4; li = li + 1) {\n"
    "                let ls    = f32(li) * 0.22;\n"
    "                let l_pos = pos_km + l_dir * ls;\n"
    "                let l_hf  = clamp(h_frac + l_dir.y * ls / 2.70, 0.0, 1.0);\n"
    "                tau_l    += sample_cloud_density(l_pos, l_hf, cov, wind_ofs) * 0.22;\n"
    "            }\n"
    "            let beer   = exp2(-tau_l * 1.442695);\n"
    "            let powder = 1.0 - exp2(-tau_l * 2.88539 * 1.442695);\n"
    "            let bp     = mix(beer, 2.0 * beer * powder, 0.55) + exp2(-tau_l * 0.35 * 1.442695) * 0.32;\n"
    "            let sun_t  = sample_trans_lut(h_km, max(sun_dir.y, -0.12));\n"
    "            let day_v  = smoothstep(-0.10, 0.04, sun_dir.y);\n"
    "            let l_col  = sun_t * (14.0 * day_v) + vec3<f32>(0.24, 0.34, 0.55) * (night_w * 2.2);\n"
    "            let amb    = mix(amb_hor, amb_zen, h_frac) * (0.35 + 0.65 * h_frac) +\n"
    "                         vec3<f32>(0.03, 0.05, 0.10) * night_w;\n"
    "            let s_col  = l_col * (bp * phase) + amb * 0.55;\n"
    "            let step_t = exp2(-dens * ds * 1.442695);\n"
    "            let w_step = trans * (1.0 - step_t);\n"
    "            lum   += s_col * w_step;\n"
    "            w_t   += t_km * w_step;\n"
    "            w_sum += w_step;\n"
    "            trans *= step_t;\n"
    "        }\n"
    "    }\n"
    "    let hit_km = select(t0_km, w_t / max(w_sum, 0.0001), w_sum > 0.001);\n"
    "    let h_fade = smoothstep(0.008, 0.11, ray_dir.y) * (1.0 - smoothstep(16.0, 30.0, hit_km));\n"
    "    trans      = mix(1.0, trans, h_fade);\n"
    "    lum       *= h_fade;\n"
    "    if (1.0 - trans > 0.001) {\n"
    "        let ap_u = fract(atan2(ray_dir.z, ray_dir.x) * 0.15915494 + 0.5);\n"
    "        let ap_v = clamp((asin(clamp(ray_dir.y, -1.0, 1.0)) + 0.15) / 0.60, 0.04, 0.96);\n"
    "        let ap_w = clamp(hit_km / 32.0, 0.04, 0.96);\n"
    "        let ap   = textureSampleLevel(aerial_tex, rep_samp, vec3<f32>(ap_u, ap_v, ap_w), 0.0);\n"
    "        lum      = lum * ap.w + ap.xyz * (1.0 - trans) * 0.35;\n"
    "    }\n"
    "    let cur_val = vec4<f32>(lum, trans);\n"
    "    let world_hit = cu.eye_time.xyz + ray_dir * (hit_km * 1000.0);\n"
    "    let p_clip    = cu.prev_vp * vec4<f32>(world_hit, 1.0);\n"
    "    var out_val   = cur_val;\n"
    "    if (p_clip.w > 0.1 && cu.wind_frame.w < 0.99) {\n"
    "        let p_ndc = p_clip.xy / p_clip.w;\n"
    "        let p_uv  = vec2<f32>(p_ndc.x * 0.5 + 0.5, 0.5 - p_ndc.y * 0.5);\n"
    "        if (p_uv.x >= 0.005 && p_uv.x <= 0.995 && p_uv.y >= 0.005 && p_uv.y <= 0.995) {\n"
    "            let prev_val = textureSampleLevel(hist_in, rep_samp, p_uv, 0.0);\n"
    "            out_val      = mix(prev_val, cur_val, cu.wind_frame.w);\n"
    "        }\n"
    "    }\n"
    "    textureStore(cloud_out, vec2<i32>(i32(gid.x), i32(gid.y)), out_val);\n"
    "}\n";

/* 6. Fiziksel gökyüzü + güneş/ay diski + gece yıldız alanı + hacimsel bulut birleştirme gölgelendiricisi */
static const char k_wgsl_sky_fs[] =
    "struct SkyFsUniforms {\n"
    "    sun_dir:     vec4<f32>,\n"
    "    sun_col:     vec4<f32>,\n"
    "    sky_zenith:  vec4<f32>,\n"
    "    sky_horizon: vec4<f32>,\n"
    "    params:      vec4<f32>,\n"
    "    moon_dir:    vec4<f32>,\n"
    "    viewport:    vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u_sky: SkyFsUniforms;\n"
    "@group(0) @binding(1) var sky_lut:   texture_2d<f32>;\n"
    "@group(0) @binding(2) var trans_lut: texture_2d<f32>;\n"
    "@group(0) @binding(3) var cloud_tex: texture_2d<f32>;\n"
    "@group(0) @binding(4) var clp_samp:  sampler;\n"
    "struct FsIn {\n"
    "    @builtin(position) frag_coord: vec4<f32>,\n"
    "    @location(0) v_nrm: vec3<f32>,\n"
    "};\n"
    "fn sample_sky_lut(dir: vec3<f32>) -> vec3<f32> {\n"
    "    let azim = atan2(dir.z, dir.x);\n"
    "    let u    = fract(azim * 0.15915494 + 0.5);\n"
    "    let sy   = clamp(dir.y, -1.0, 1.0);\n"
    "    let elev = asin(sy);\n"
    "    let sgn  = select(-1.0, 1.0, elev >= 0.0);\n"
    "    let v    = clamp(0.5 + 0.5 * sgn * sqrt(abs(elev) / 1.57079633), 0.005, 0.995);\n"
    "    return textureSampleLevel(sky_lut, clp_samp, vec2<f32>(u, v), 0.0).xyz;\n"
    "}\n"
    "fn sample_trans_lut(h_km: f32, mu: f32) -> vec3<f32> {\n"
    "    let u = clamp((mu + 0.25) / 1.25, 0.002, 0.998);\n"
    "    let v = clamp(sqrt(clamp(h_km * 0.01, 0.0, 1.0)), 0.008, 0.992);\n"
    "    return textureSampleLevel(trans_lut, clp_samp, vec2<f32>(u, v), 0.0).xyz;\n"
    "}\n"
    "fn star_layer(dir: vec3<f32>, scale: f32, seed: f32, density_thr: f32) -> vec3<f32> {\n"
    "    let p  = dir * scale;\n"
    "    let ip = floor(p);\n"
    "    let fp = p - ip - vec3<f32>(0.5);\n"
    "    let h1 = fract(sin(dot(ip, vec3<f32>(127.1, 311.7, 74.7)) + seed) * 43758.5453);\n"
    "    let h2 = fract(sin(dot(ip, vec3<f32>(269.5, 183.3, 246.1)) + seed) * 43758.5453);\n"
    "    if (h1 < density_thr) { return vec3<f32>(0.0); }\n"
    "    let jit = (vec3<f32>(h1, h2, fract(h1 * 17.3 + h2)) - vec3<f32>(0.5)) * 0.55;\n"
    "    let d2  = dot(fp - jit, fp - jit);\n"
    "    let br  = exp2(-d2 * 68.0) * smoothstep(density_thr, 1.0, h1);\n"
    "    let c_warm = vec3<f32>(1.0, 0.82, 0.65);\n"
    "    let c_cool = vec3<f32>(0.68, 0.84, 1.0);\n"
    "    return mix(c_warm, c_cool, h2) * (br * 2.8);\n"
    "}\n"
    "@fragment\n"
    "fn frag(fin: FsIn) -> @location(0) vec4<f32> {\n"
    "    let dir      = normalize(fin.v_nrm);\n"
    "    let sun_dir  = normalize(u_sky.sun_dir.xyz);\n"
    "    let moon_dir = normalize(u_sky.moon_dir.xyz);\n"
    "    let star_int = u_sky.moon_dir.w;\n"
    "    let night_int = u_sky.viewport.z;\n"
    "    var hdr      = sample_sky_lut(dir);\n"
    "    /* Gece atmosferik airglow ve ay ışığı saçılımı */\n"
    "    let n_zen    = vec3<f32>(0.012, 0.024, 0.065);\n"
    "    let n_hor    = vec3<f32>(0.032, 0.058, 0.125);\n"
    "    let n_sky    = mix(n_hor, n_zen, pow(clamp(dir.y, 0.0, 1.0), 0.55));\n"
    "    hdr         += n_sky * night_int;\n"
    "    /* Gece yıldız alanı ve Samanyolu kuşağı (ufukta atmosferik sönümlü) */\n"
    "    if (star_int > 0.005 && dir.y > 0.0) {\n"
    "        let hor_fade = smoothstep(0.01, 0.22, dir.y);\n"
    "        let s1 = star_layer(dir, 140.0, 1.7, 0.968);\n"
    "        let s2 = star_layer(dir, 260.0, 5.3, 0.976) * 0.65;\n"
    "        let mw_band = exp2(-abs(dir.x * 0.65 + dir.z * 0.35 - 0.15) * 9.0);\n"
    "        let mw_col  = vec3<f32>(0.018, 0.028, 0.055) * mw_band;\n"
    "        hdr += (s1 + s2 + mw_col) * (star_int * hor_fade);\n"
    "    }\n"
    "    /* Fiziksel Güneş diski (kenar kararmalı + Transmittance LUT renkli) ve korona */\n"
    "    let s_dot = dot(dir, sun_dir);\n"
    "    if (dir.y > -0.04 && sun_dir.y > -0.08) {\n"
    "        let t_sun  = sample_trans_lut(0.02, max(dir.y, 0.0));\n"
    "        let disk   = smoothstep(0.99945, 0.99978, s_dot);\n"
    "        let corona = pow(max(s_dot, 0.0), 256.0) * 1.4 + pow(max(s_dot, 0.0), 16.0) * 0.18;\n"
    "        let h_vis  = smoothstep(-0.02, 0.02, dir.y);\n"
    "        hdr       += t_sun * (disk * 28.0 + corona * 4.5) * h_vis;\n"
    "    }\n"
    "    /* Gece Ay diski ve gümüşi-mavi ay halesi */\n"
    "    if (night_int > 0.01 && dir.y > 0.0) {\n"
    "        let m_dot   = dot(dir, moon_dir);\n"
    "        let m_disk  = smoothstep(0.99925, 0.99962, m_dot);\n"
    "        let m_glow  = pow(max(m_dot, 0.0), 192.0) * 0.85 + pow(max(m_dot, 0.0), 18.0) * 0.12;\n"
    "        let crater  = 0.82 + 0.18 * sin(dir.x * 180.0) * cos(dir.y * 180.0);\n"
    "        let m_col   = vec3<f32>(0.78, 0.88, 1.05);\n"
    "        hdr        += m_col * (m_disk * crater * 6.5 + m_glow) * night_int;\n"
    "    }\n"
    "    /* Çeyrek çözünürlüklü + zamansal yeniden izdüşümlü hacimsel bulutların birleştirilmesi */\n"
    "    let suv   = clamp(fin.frag_coord.xy / u_sky.viewport.xy, vec2<f32>(0.002), vec2<f32>(0.998));\n"
    "    let cloud = textureSampleLevel(cloud_tex, clp_samp, suv, 0.0);\n"
    "    hdr       = hdr * cloud.w + cloud.xyz;\n"
    "    /* Yumuşak ACES ton eşlemesi ve sRGB gama düzeltmesi */\n"
    "    let mapped = (hdr * (hdr * 2.51 + vec3<f32>(0.03))) /\n"
    "                 (hdr * (hdr * 2.43 + vec3<f32>(0.59)) + vec3<f32>(0.14));\n"
    "    let srgb   = sqrt(clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0)));\n"
    "    return vec4<f32>(srgb, 1.0);\n"
    "}\n";

/* 7. Atmosfer LUT + Hacimsel Bulut yansıması + 3B Aerial Perspective destekli GPU Okyanus gölgelendiricisi */
static const char k_wgsl_ocean_fs[] =
    "struct OceanFsUniforms {\n"
    "    sun_dir:     vec4<f32>,\n"
    "    sun_col:     vec4<f32>,\n"
    "    sky_zenith:  vec4<f32>,\n"
    "    sky_horizon: vec4<f32>,\n"
    "    sea_deep:    vec4<f32>,\n"
    "    sea_shallow: vec4<f32>,\n"
    "    eye:         vec4<f32>,\n"
    "    scales:      vec4<f32>,\n"
    "    params:      vec4<f32>,\n"
    "    moon_dir:    vec4<f32>,\n"
    "    viewport:    vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u_oc: OceanFsUniforms;\n"
    "@group(0) @binding(1) var casc_0:     texture_2d<f32>;\n"
    "@group(0) @binding(2) var casc_1:     texture_2d<f32>;\n"
    "@group(0) @binding(3) var casc_2:     texture_2d<f32>;\n"
    "@group(0) @binding(4) var sky_lut:    texture_2d<f32>;\n"
    "@group(0) @binding(5) var aerial_lut: texture_3d<f32>;\n"
    "@group(0) @binding(6) var cloud_tex:  texture_2d<f32>;\n"
    "@group(0) @binding(7) var rep_samp:   sampler;\n"
    "struct FsIn {\n"
    "    @builtin(position) frag_coord: vec4<f32>,\n"
    "    @location(0) v_uv:    vec2<f32>,\n"
    "    @location(1) v_nrm:   vec3<f32>,\n"
    "    @location(2) v_depth: f32,\n"
    "    @location(3) v_wpos:  vec3<f32>,\n"
    "};\n"
    "fn sample_sky_lut(dir: vec3<f32>) -> vec3<f32> {\n"
    "    let azim = atan2(dir.z, dir.x);\n"
    "    let u    = fract(azim * 0.15915494 + 0.5);\n"
    "    let sy   = clamp(abs(dir.y), 0.01, 1.0);\n"
    "    let elev = asin(sy);\n"
    "    let v    = clamp(0.5 + 0.5 * sqrt(elev / 1.57079633), 0.51, 0.99);\n"
    "    return textureSampleLevel(sky_lut, rep_samp, vec2<f32>(u, v), 0.0).xyz;\n"
    "}\n"
    "fn tone_map_srgb(hdr: vec3<f32>) -> vec3<f32> {\n"
    "    let m = (hdr * (hdr * 2.51 + vec3<f32>(0.03))) /\n"
    "            (hdr * (hdr * 2.43 + vec3<f32>(0.59)) + vec3<f32>(0.14));\n"
    "    return sqrt(clamp(m, vec3<f32>(0.0), vec3<f32>(1.0)));\n"
    "}\n"
    "@fragment\n"
    "fn frag(fin: FsIn) -> @location(0) vec4<f32> {\n"
    "    let c0 = textureSample(casc_0, rep_samp, fin.v_uv);\n"
    "    let c1 = textureSample(casc_1, rep_samp, fin.v_uv * u_oc.scales.x);\n"
    "    let c2 = textureSample(casc_2, rep_samp, fin.v_uv * u_oc.scales.y);\n"
    "    let df = 1.0 / (1.0 + fin.v_depth * 0.008);\n"
    "    let zc = vec2<f32>(0.50196078);\n"
    "    let sl = ((c0.xy - zc) + (c1.xy - zc) * (0.85 * df) + (c2.xy - zc) * (0.65 * df * df)) * u_oc.scales.z;\n"
    "    let n  = normalize(vec3<f32>(fin.v_nrm.x - sl.x * 0.65, fin.v_nrm.y, fin.v_nrm.z - sl.y * 0.65));\n"
    "    let v  = normalize(u_oc.eye.xyz - fin.v_wpos);\n"
    "    let r  = reflect(-v, n);\n"
    "    let ndv   = max(dot(n, v), 0.001);\n"
    "    let om    = 1.0 - ndv;\n"
    "    let om2   = om * om;\n"
    "    let fres  = 0.02 + 0.98 * (om2 * om2 * om);\n"
    "    let night = u_oc.viewport.z;\n"
    "    /* Fiziksel atmosfer LUT + hacimsel bulut yansıması */\n"
    "    var sky_hdr = sample_sky_lut(r) + vec3<f32>(0.018, 0.032, 0.075) * night;\n"
    "    let suv = clamp(vec2<f32>(fin.frag_coord.x / u_oc.viewport.x,\n"
    "                              0.48 - clamp(r.y, 0.0, 0.85) * 0.45) + n.xz * 0.035,\n"
    "                    vec2<f32>(0.01), vec2<f32>(0.99));\n"
    "    let cld = textureSampleLevel(cloud_tex, rep_samp, suv, 0.0);\n"
    "    sky_hdr = sky_hdr * cld.w + cld.xyz * 0.75;\n"
    "    let refl_srgb = tone_map_srgb(sky_hdr);\n"
    "    /* Yüzey altı turkuaz saçılım ve derin su rengi */\n"
    "    let sun_dir  = normalize(u_oc.sun_dir.xyz);\n"
    "    let moon_dir = normalize(u_oc.moon_dir.xyz);\n"
    "    let l_dir    = normalize(mix(sun_dir, moon_dir, night));\n"
    "    let crest    = clamp((c0.z - 0.48) * 1.8 + max(1.0 - n.y, 0.0) * 1.4, 0.0, 1.0);\n"
    "    let back_lit = max(dot(l_dir, -v), 0.0) * 0.6 + 0.4;\n"
    "    let sss      = clamp(crest * back_lit, 0.0, 1.0);\n"
    "    let day_dim  = mix(1.0, 0.22, night);\n"
    "    let water_c  = mix(u_oc.sea_deep.xyz, u_oc.sea_shallow.xyz, sss) * day_dim;\n"
    "    /* Güneş ve Ay speküler parıltısı (Beckmann-benzeri mikro-yüzey) */\n"
    "    let h_vec = normalize(v + l_dir);\n"
    "    let ndh   = max(dot(n, h_vec), 0.0);\n"
    "    let ndl   = max(dot(n, l_dir), 0.0);\n"
    "    let sp_t  = pow(ndh, 256.0) * 2.2;\n"
    "    let sp_w  = pow(ndh, 32.0) * 0.16;\n"
    "    let spec  = u_oc.sun_col.xyz * ((sp_t + sp_w) * (0.25 + 0.75 * fres) * step(0.0, ndl));\n"
    "    /* Jacobian beyaz köpük */\n"
    "    let fm_raw = max(c0.w, max(c1.w * 0.90, c2.w * 0.75));\n"
    "    let foam   = smoothstep(0.08, 0.72, fm_raw);\n"
    "    let foam_c = (u_oc.sky_horizon.xyz * 0.45 + u_oc.sun_col.xyz * (0.45 * max(ndl, 0.25)) +\n"
    "                  vec3<f32>(0.22)) * mix(1.0, 0.35, night);\n"
    "    var col = mix(water_c, refl_srgb, fres) + spec;\n"
    "    col     = mix(col, min(foam_c, vec3<f32>(0.98)), foam);\n"
    "    /* 3B Aerial Perspective LUT + ufuk sönümlenmesi */\n"
    "    let view_ray = -v;\n"
    "    let ap_u = fract(atan2(view_ray.z, view_ray.x) * 0.15915494 + 0.5);\n"
    "    let ap_v = clamp((asin(clamp(view_ray.y, -1.0, 1.0)) + 0.15) / 0.60, 0.04, 0.96);\n"
    "    let ap_w = clamp((fin.v_depth * 0.001) / 32.0, 0.04, 0.96);\n"
    "    let ap   = textureSampleLevel(aerial_lut, rep_samp, vec3<f32>(ap_u, ap_v, ap_w), 0.0);\n"
    "    let ap_srgb = tone_map_srgb(ap.xyz + sample_sky_lut(vec3<f32>(view_ray.x, 0.02, view_ray.z)));\n"
    "    let fog_f   = clamp(1.0 - exp2(-fin.v_depth * u_oc.scales.w * 1.442695), 0.0, 1.0);\n"
    "    col = mix(col, ap_srgb, fog_f);\n"
    "    return vec4<f32>(clamp(col, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);\n"
    "}\n";

/* ---------- GPU Veri Yapıları ---------- */

typedef struct {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
} GpuVertex3d;

_Static_assert(sizeof(GpuVertex3d) == 32, "GpuVertex3d layout");

typedef struct {
    Mat4 mvp;
    Mat4 model;
    Mat4 normal_mat;
    Mat4 shadow_mat;
} GpuVsUniforms;

_Static_assert(sizeof(GpuVsUniforms) == 256, "GpuVsUniforms layout");

typedef struct {
    Mat4 mvp;
    Vec4 eye_snap;
    Vec4 eye_pos;
    Vec4 casc_scales;
} GpuOceanVsUniforms;

_Static_assert(sizeof(GpuOceanVsUniforms) == 112, "GpuOceanVsUniforms layout");

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 sea_deep;
    _Alignas(16) Vec4 sea_shallow;
    _Alignas(16) Vec4 eye;
    _Alignas(16) Vec4 scales;
    _Alignas(16) Vec4 params;
    _Alignas(16) Vec4 moon_dir;
    _Alignas(16) Vec4 viewport;
} GpuOceanFsUniforms;

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 params;
    _Alignas(16) Vec4 moon_dir;
    _Alignas(16) Vec4 viewport;
} GpuSkyFsUniforms;

typedef struct {
    Mat4              prev_vp;
    _Alignas(16) Vec4 cam_fwd;
    _Alignas(16) Vec4 cam_right;
    _Alignas(16) Vec4 cam_up;
    _Alignas(16) Vec4 eye_time;
    _Alignas(16) Vec4 sun_cov;
    _Alignas(16) Vec4 moon_star;
    _Alignas(16) Vec4 wind_frame;
} GpuCloudUniforms;

_Static_assert(sizeof(GpuCloudUniforms) == 176, "GpuCloudUniforms layout");

typedef struct {
    _Alignas(16) Vec4 light_dir;
    _Alignas(16) Vec4 base;
    _Alignas(16) Vec4 fog;
} GpuLitFsUniforms;

typedef struct {
    _Alignas(16) Vec4 color;
    _Alignas(16) Vec4 fog;
} GpuUnlitFsUniforms;

typedef struct {
    u32          n;
    u32          log2n;
    u32          num_stages;
    RhiBuffer    h0_buf;
    RhiBuffer    kinfo_buf;
    RhiBuffer    fft_a01;
    RhiBuffer    fft_a23;
    RhiBuffer    fft_b01;
    RhiBuffer    fft_b23;
    RhiBuffer    foam_buf;
    RhiBuffer    disp_buf;
    RhiBuffer    casc_buf;
    RhiTexture   disp_tex;
    RhiTexture   casc_tex;
    RhiBuffer    spec_ub;
    RhiBuffer    unpack_ub;
    RhiBuffer    to_tex_ub;
    RhiBuffer    stage_ub[GPU_OCEAN_MAX_FFT_STAGES];
    RhiBindGroup spec_bg;
    RhiBindGroup unpack_bg;
    RhiBindGroup to_tex_bg;
    RhiBindGroup stage_bg[GPU_OCEAN_MAX_FFT_STAGES];
} GpuCascadeRes;

typedef struct {
    RhiBuffer vb;
    RhiBuffer ib;
    u32       index_count;
} GpuMeshRes;

typedef struct {
    Arena              arena;
    Rhi                rhi;
    b32                initialized;
    b32                static_luts_built;
    u32                cloud_ping;
    u32                frame_idx;
    f32                last_sun_y;
    f32                last_cov;
    u32                depth_w;
    u32                depth_h;
    RhiTexture         depth_tex;
    RhiSampler         lin_sampler;
    RhiSampler         clamp_sampler;

    /* Okyanus FFT compute pipeline'ları */
    RhiComputePipeline cpipe_spectrum;
    RhiComputePipeline cpipe_fft_stage;
    RhiComputePipeline cpipe_unpack;
    RhiComputePipeline cpipe_to_tex;
    GpuCascadeRes      casc[OCEAN_CASCADES];

    /* V1 Atmosfer + Bulut compute pipeline'ları */
    RhiComputePipeline cpipe_atmos_trans;
    RhiComputePipeline cpipe_atmos_ms;
    RhiComputePipeline cpipe_atmos_sky;
    RhiComputePipeline cpipe_atmos_aerial;
    RhiComputePipeline cpipe_cloud_noise;
    RhiComputePipeline cpipe_static_to_tex;
    RhiComputePipeline cpipe_dyn_to_tex;
    RhiComputePipeline cpipe_cloud_march;

    /* V1 Atmosfer + Bulut storage tamponları ve 2B/3B LUT dokuları */
    RhiBuffer          buf_trans_lut;
    RhiBuffer          buf_ms_lut;
    RhiBuffer          buf_sky_lut;
    RhiBuffer          buf_aerial_lut;
    RhiBuffer          buf_cloud_noise;

    RhiTexture         tex_trans_lut;
    RhiTexture         tex_ms_lut;
    RhiTexture         tex_sky_lut;
    RhiTexture         tex_aerial_3d;
    RhiTexture         tex_cloud_noise_3d;
    RhiTexture         tex_cloud_hist[2];

    RhiBuffer          ub_atmos_trans;
    RhiBuffer          ub_atmos_ms;
    RhiBuffer          ub_atmos_sky;
    RhiBuffer          ub_atmos_aerial;
    RhiBuffer          ub_cloud_noise;
    RhiBuffer          ub_cloud_march;

    RhiBindGroup       bg_atmos_trans;
    RhiBindGroup       bg_atmos_ms;
    RhiBindGroup       bg_atmos_sky;
    RhiBindGroup       bg_atmos_aerial;
    RhiBindGroup       bg_cloud_noise;
    RhiBindGroup       bg_static_to_tex;
    RhiBindGroup       bg_dyn_to_tex;
    RhiBindGroup       bg_cloud_march[2];

    /* Render pipeline'ları */
    RhiPipeline        pipe_ocean;
    RhiPipeline        pipe_sky;
    RhiPipeline        pipe_lit_back;
    RhiPipeline        pipe_lit_none;
    RhiPipeline        pipe_unlit;

    /* Ağlar ve dokular */
    GpuMeshRes         ocean_grid;
    GpuMeshRes         sky_mesh;
    GpuMeshRes         hull_mesh;
    GpuMeshRes         main_sail_mesh;
    GpuMeshRes         jib_sail_mesh;
    GpuMeshRes         cube_mesh;
    RhiTexture         tex_hull;
    RhiTexture         tex_deck;
    RhiTexture         tex_sail;

    /* Render uniform tamponları ve bind group'ları */
    RhiBuffer          ocean_vs_ub;
    RhiBuffer          ocean_fs_ub;
    RhiBindGroup       ocean_vs_bg;
    RhiBindGroup       ocean_fs_bg[2];

    RhiBuffer          sky_vs_ub;
    RhiBuffer          sky_fs_ub;
    RhiBindGroup       sky_vs_bg;
    RhiBindGroup       sky_fs_bg[2];

    RhiBuffer          boat_vs_ub[GPU_OCEAN_BOAT_PARTS];
    RhiBuffer          boat_fs_ub[GPU_OCEAN_BOAT_PARTS];
    RhiBindGroup       boat_vs_bg[GPU_OCEAN_BOAT_PARTS];
    RhiBindGroup       boat_fs_bg[GPU_OCEAN_BOAT_PARTS];

    /* Ek 3D dünya nesneleri (adalar, iskele, deniz fenerleri, şamandıralar, hayalet tekne) */
    RhiBuffer          extra_vs_ub[GPU_OCEAN_MAX_EXTRAS];
    RhiBuffer          extra_lit_fs_ub[GPU_OCEAN_MAX_EXTRAS];
    RhiBuffer          extra_unlit_fs_ub[GPU_OCEAN_MAX_EXTRAS];
    RhiBindGroup       extra_lit_vs_bg[GPU_OCEAN_MAX_EXTRAS];
    RhiBindGroup       extra_lit_fs_bg[GPU_OCEAN_MAX_EXTRAS];
    RhiBindGroup       extra_unlit_vs_bg[GPU_OCEAN_MAX_EXTRAS];
    RhiBindGroup       extra_unlit_fs_bg[GPU_OCEAN_MAX_EXTRAS];
} GpuOceanState;

static u8            g_gpu_ocean_mem[MB(8)];
static GpuOceanState g_gpu_ocean;

/* ---------- Yardımcı Fonksiyonlar ---------- */

static inline f32 gpu_ocean_exp(f32 x) {
    return m_exp2(x * 1.44269504f);
}

static Mat4 gpu_mat4_transpose(Mat4 a) {
    Mat4 r;
    for (u32 c = 0u; c < 4u; c++) {
        for (u32 row = 0u; row < 4u; row++) {
            r.m[c * 4u + row] = a.m[row * 4u + c];
        }
    }
    return r;
}

static RhiShader compile_mtk_shader_to_rhi(Rhi* rhi, Arena* scratch,
                                           const char* src, u32 len, const char* name,
                                           u32 group, const char* entry) {
    usize mark = arena_mark(scratch);
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 131072u);
    MtkLayout layout;
    MtkDiag diag;
    __builtin_memset(&layout, 0, sizeof(layout));
    mtk_diag_init(&diag);

    MtkWgslOpts opts;
    opts.frag_group = group;
    opts.entry_name = entry;

    u32 wgsl_len = mtk_compile_wgsl(src, len, name, &opts, scratch,
                                    wgsl_buf, 131072u, &layout, &diag);
    RhiShader sh = RHI_INVALID_HANDLE;
    if (wgsl_len > 0u) {
        sh = rhi_create_shader(rhi, wgsl_buf, wgsl_len);
    }
    arena_pop_to(scratch, mark);
    return sh;
}

static RhiShader compile_vs_attrs_to_rhi(Rhi* rhi, Arena* scratch,
                                         const u8* attrs, u32 attr_count) {
    usize mark = arena_mark(scratch);
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 16384u);
    char err[256];
    u32 wgsl_len = mtk_wgsl_vert(attrs, attr_count, wgsl_buf, 16384u, err, sizeof(err));
    RhiShader sh = RHI_INVALID_HANDLE;
    if (wgsl_len > 0u) {
        sh = rhi_create_shader(rhi, wgsl_buf, wgsl_len);
    }
    arena_pop_to(scratch, mark);
    return sh;
}

static void upload_mesh_3d(Rhi* rhi, Arena* scratch, const Mesh* m, GpuMeshRes* out) {
    usize mark = arena_mark(scratch);
    u32 vc = m->vertex_count;
    u32 ic = m->index_count;
    GpuVertex3d* verts = ARENA_PUSH_ARRAY(scratch, GpuVertex3d, vc);
    for (u32 i = 0u; i < vc; i++) {
        verts[i].px = m->px[i];
        verts[i].py = m->py[i];
        verts[i].pz = m->pz[i];
        verts[i].nx = m->nx ? m->nx[i] : 0.0f;
        verts[i].ny = m->ny ? m->ny[i] : 1.0f;
        verts[i].nz = m->nz ? m->nz[i] : 0.0f;
        verts[i].u  = m->u  ? m->u[i]  : 0.0f;
        verts[i].v  = m->v  ? m->v[i]  : 0.0f;
    }
    u32 vb_bytes = vc * (u32)sizeof(GpuVertex3d);
    u32 ib_bytes = ic * (u32)sizeof(u32);
    out->vb = rhi_create_buffer(rhi, vb_bytes, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    out->ib = rhi_create_buffer(rhi, ib_bytes, RHI_BUF_INDEX | RHI_BUF_COPY_DST);
    out->index_count = ic;
    rhi_write_buffer(rhi, out->vb, 0u, verts, vb_bytes);
    rhi_write_buffer(rhi, out->ib, 0u, m->indices, ib_bytes);
    arena_pop_to(scratch, mark);
}

static void update_mesh_3d_vb(Rhi* rhi, Arena* scratch, const Mesh* m, const GpuMeshRes* res) {
    if (!m || !res || res->vb == RHI_INVALID_HANDLE) return;
    usize mark = arena_mark(scratch);
    u32 vc = m->vertex_count;
    GpuVertex3d* verts = ARENA_PUSH_ARRAY(scratch, GpuVertex3d, vc);
    for (u32 i = 0u; i < vc; i++) {
        verts[i].px = m->px[i];
        verts[i].py = m->py[i];
        verts[i].pz = m->pz[i];
        verts[i].nx = m->nx ? m->nx[i] : 0.0f;
        verts[i].ny = m->ny ? m->ny[i] : 1.0f;
        verts[i].nz = m->nz ? m->nz[i] : 0.0f;
        verts[i].u  = m->u  ? m->u[i]  : 0.0f;
        verts[i].v  = m->v  ? m->v[i]  : 0.0f;
    }
    rhi_write_buffer(rhi, res->vb, 0u, verts, vc * (u32)sizeof(GpuVertex3d));
    arena_pop_to(scratch, mark);
}

static RhiTexture upload_texture_rgba8(Rhi* rhi, const Texture* tex) {
    RhiTextureDesc td = {
        .width      = tex->w,
        .height     = tex->h,
        .mip_levels = 1u,
        .format     = RHI_FMT_RGBA8_UNORM,
        .usage      = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    RhiTexture h = rhi_create_texture(rhi, &td);
    rhi_write_texture(rhi, h, 0u, 0u, 0u, tex->w, tex->h,
                      tex->w * 4u, tex->px, tex->w * tex->h * 4u);
    return h;
}

static void upload_cascade_spectrum(Rhi* rhi, Arena* scratch, const OceanCascade* c,
                                    const GpuCascadeRes* gr, b32 upload_kinfo) {
    usize mark = arena_mark(scratch);
    u32 n  = c->n;
    u32 nn = n * n;
    Vec4* h0_pack = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    for (u32 j = 0u; j < n; j++) {
        u32 br_j = c->bitrev[j];
        for (u32 i = 0u; i < n; i++) {
            u32 br_i = c->bitrev[i];
            u32 src  = j * n + i;
            u32 dst  = br_j * n + br_i;
            h0_pack[dst] = vec4(c->h0r[src], c->h0i[src], c->h0mr[src], c->h0mi[src]);
        }
    }
    rhi_write_buffer(rhi, gr->h0_buf, 0u, h0_pack, nn * (u32)sizeof(Vec4));

    if (upload_kinfo) {
        Vec4* ki_pack = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
        for (u32 j = 0u; j < n; j++) {
            u32 br_j = c->bitrev[j];
            for (u32 i = 0u; i < n; i++) {
                u32 br_i = c->bitrev[i];
                u32 src  = j * n + i;
                u32 dst  = br_j * n + br_i;
                f32 kx = c->kx[src], kz = c->kz[src];
                f32 k2 = kx * kx + kz * kz;
                f32 kl = (k2 > 1e-12f) ? m_sqrt(k2) : 0.0f;
                f32 ik = (kl > 1e-12f) ? (1.0f / kl) : 0.0f;
                ki_pack[dst] = vec4(kx, kz, c->omega[src], ik);
            }
        }
        rhi_write_buffer(rhi, gr->kinfo_buf, 0u, ki_pack, nn * (u32)sizeof(Vec4));
    }
    arena_pop_to(scratch, mark);
}

static void build_gpu_ocean_grid(Rhi* rhi, Arena* scratch, GpuMeshRes* out) {
    usize mark = arena_mark(scratch);
    u32 div      = GPU_OCEAN_GRID_DIV;
    u32 verts_1d = div + 1u;
    u32 vcount   = verts_1d * verts_1d;
    u32 icount   = div * div * 6u;

    f32* ofs = ARENA_PUSH_ARRAY(scratch, f32, verts_1d);
    f32 r_fine = 28.0f;
    f32 r_far  = GPU_OCEAN_GRID_RADIUS - r_fine;
    for (u32 i = 0u; i < verts_1d; i++) {
        f32 u   = (2.0f * (f32)i) / (f32)div - 1.0f;
        f32 s   = m_abs(u);
        f32 sgn = (u >= 0.0f) ? 1.0f : -1.0f;
        f32 s2  = s * s;
        ofs[i]  = sgn * (r_fine * s + r_far * s2 * s2);
    }

    f32* vb_data = ARENA_PUSH_ARRAY(scratch, f32, vcount * 2u);
    for (u32 j = 0u; j < verts_1d; j++) {
        for (u32 i = 0u; i < verts_1d; i++) {
            u32 vi = (j * verts_1d + i) * 2u;
            vb_data[vi + 0u] = ofs[i];
            vb_data[vi + 1u] = ofs[j];
        }
    }

    u32* ib_data = ARENA_PUSH_ARRAY(scratch, u32, icount);
    u32* ix = ib_data;
    for (u32 j = 0u; j < div; j++) {
        for (u32 i = 0u; i < div; i++) {
            u32 a0 = j * verts_1d + i;
            u32 a1 = a0 + 1u;
            u32 b0 = a0 + verts_1d;
            u32 b1 = b0 + 1u;
            *ix++ = a0; *ix++ = b0; *ix++ = b1;
            *ix++ = a0; *ix++ = b1; *ix++ = a1;
        }
    }

    u32 vb_bytes = vcount * 2u * (u32)sizeof(f32);
    u32 ib_bytes = icount * (u32)sizeof(u32);
    out->vb = rhi_create_buffer(rhi, vb_bytes, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    out->ib = rhi_create_buffer(rhi, ib_bytes, RHI_BUF_INDEX | RHI_BUF_COPY_DST);
    out->index_count = icount;
    rhi_write_buffer(rhi, out->vb, 0u, vb_data, vb_bytes);
    rhi_write_buffer(rhi, out->ib, 0u, ib_data, ib_bytes);
    arena_pop_to(scratch, mark);
}

/* ---------- GPU Kaynaklarının İlk Kurulumu ---------- */

static b32 gpu_ocean_init_resources(GpuOceanState* st, Arena* scratch, const GpuOceanFrameParams* p) {
    arena_init(&st->arena, g_gpu_ocean_mem, sizeof(g_gpu_ocean_mem));
    if (!rhi_init(&st->rhi, &st->arena, 512u, MB(4))) {
        return FALSE;
    }
    Rhi* rhi = &st->rhi;
    rhi_set_active(rhi);

    /* 1. Doğrusal + tekrarlı ve doğrusal + kenar-kıstırmalı örnekleyiciler */
    RhiSamplerDesc sdesc = {
        .address_u     = RHI_ADDR_REPEAT,
        .address_v     = RHI_ADDR_REPEAT,
        .mag_filter    = RHI_FILTER_LINEAR,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_LINEAR
    };
    st->lin_sampler = rhi_create_sampler(rhi, &sdesc);
    sdesc.address_u = RHI_ADDR_CLAMP;
    sdesc.address_v = RHI_ADDR_CLAMP;
    st->clamp_sampler = rhi_create_sampler(rhi, &sdesc);

    /* 2. Matoki @compute -> WGSL çekirdeklerinin derlenmesi ve compute pipeline'ları */
    RhiShader cs_spec = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_ocean_spectrum, (u32)(sizeof(k_mtk_ocean_spectrum) - 1u),
        "ocean_spectrum.mtk", 0u, "main");
    RhiShader cs_stage = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_ocean_fft_stage, (u32)(sizeof(k_mtk_ocean_fft_stage) - 1u),
        "ocean_fft_stage.mtk", 0u, "main");
    RhiShader cs_unpack = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_ocean_unpack, (u32)(sizeof(k_mtk_ocean_unpack) - 1u),
        "ocean_unpack.mtk", 0u, "main");
    RhiShader cs_to_tex = rhi_create_shader(
        rhi, k_wgsl_ocean_to_tex, (u32)(sizeof(k_wgsl_ocean_to_tex) - 1u));

    RhiComputePipelineDesc cp_desc = { .shader = cs_spec, .entry = "main" };
    st->cpipe_spectrum = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader = cs_stage;
    st->cpipe_fft_stage = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader = cs_unpack;
    st->cpipe_unpack = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader = cs_to_tex;
    st->cpipe_to_tex = rhi_create_compute_pipeline(rhi, &cp_desc);

    /* 2b. V1 Hillaire Atmosfer LUT + 3B Bulut Gürültüsü + Raymarching compute pipeline'ları */
    RhiShader cs_at_trans = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_atmos_transmittance, (u32)(sizeof(k_mtk_atmos_transmittance) - 1u),
        "atmos_transmittance.mtk", 0u, "main");
    RhiShader cs_at_ms = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_atmos_multiscatter, (u32)(sizeof(k_mtk_atmos_multiscatter) - 1u),
        "atmos_multiscatter.mtk", 0u, "main");
    RhiShader cs_at_sky = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_atmos_skyview, (u32)(sizeof(k_mtk_atmos_skyview) - 1u),
        "atmos_skyview.mtk", 0u, "main");
    RhiShader cs_at_aer = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_atmos_aerial, (u32)(sizeof(k_mtk_atmos_aerial) - 1u),
        "atmos_aerial.mtk", 0u, "main");
    RhiShader cs_cl_noise = compile_mtk_shader_to_rhi(
        rhi, scratch, k_mtk_cloud_noise, (u32)(sizeof(k_mtk_cloud_noise) - 1u),
        "cloud_noise.mtk", 0u, "main");
    RhiShader cs_stat_tex = rhi_create_shader(
        rhi, k_wgsl_static_to_tex, (u32)(sizeof(k_wgsl_static_to_tex) - 1u));
    RhiShader cs_dyn_tex = rhi_create_shader(
        rhi, k_wgsl_dyn_to_tex, (u32)(sizeof(k_wgsl_dyn_to_tex) - 1u));
    RhiShader cs_cl_march = rhi_create_shader(
        rhi, k_wgsl_cloud_march, (u32)(sizeof(k_wgsl_cloud_march) - 1u));

    cp_desc.shader        = cs_at_trans;
    st->cpipe_atmos_trans = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_at_ms;
    st->cpipe_atmos_ms    = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_at_sky;
    st->cpipe_atmos_sky   = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_at_aer;
    st->cpipe_atmos_aerial = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_cl_noise;
    st->cpipe_cloud_noise = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_stat_tex;
    st->cpipe_static_to_tex = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_dyn_tex;
    st->cpipe_dyn_to_tex  = rhi_create_compute_pipeline(rhi, &cp_desc);
    cp_desc.shader        = cs_cl_march;
    st->cpipe_cloud_march = rhi_create_compute_pipeline(rhi, &cp_desc);

    u32 trans_bytes  = ATMOS_TRANSMITTANCE_W * ATMOS_TRANSMITTANCE_H * (u32)sizeof(Vec4);
    u32 ms_bytes     = ATMOS_MULTISCATTER_W * ATMOS_MULTISCATTER_H * (u32)sizeof(Vec4);
    u32 sky_bytes    = ATMOS_SKYVIEW_W * ATMOS_SKYVIEW_H * (u32)sizeof(Vec4);
    u32 aerial_bytes = ATMOS_AERIAL_W * ATMOS_AERIAL_H * ATMOS_AERIAL_D * (u32)sizeof(Vec4);
    u32 noise_bytes  = CLOUD_NOISE_3D_N * CLOUD_NOISE_3D_N * CLOUD_NOISE_3D_N * (u32)sizeof(Vec4);

    st->buf_trans_lut   = rhi_create_buffer(rhi, trans_bytes,  RHI_BUF_STORAGE);
    st->buf_ms_lut      = rhi_create_buffer(rhi, ms_bytes,     RHI_BUF_STORAGE);
    st->buf_sky_lut     = rhi_create_buffer(rhi, sky_bytes,    RHI_BUF_STORAGE);
    st->buf_aerial_lut  = rhi_create_buffer(rhi, aerial_bytes, RHI_BUF_STORAGE);
    st->buf_cloud_noise = rhi_create_buffer(rhi, noise_bytes,  RHI_BUF_STORAGE);

    RhiTextureDesc td_trans = {
        .width = ATMOS_TRANSMITTANCE_W, .height = ATMOS_TRANSMITTANCE_H, .mip_levels = 1u,
        .format = RHI_FMT_RGBA16_FLOAT, .usage = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_trans_lut = rhi_create_texture(rhi, &td_trans);

    RhiTextureDesc td_ms = {
        .width = ATMOS_MULTISCATTER_W, .height = ATMOS_MULTISCATTER_H, .mip_levels = 1u,
        .format = RHI_FMT_RGBA16_FLOAT, .usage = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_ms_lut = rhi_create_texture(rhi, &td_ms);

    RhiTextureDesc td_sky = {
        .width = ATMOS_SKYVIEW_W, .height = ATMOS_SKYVIEW_H, .mip_levels = 1u,
        .format = RHI_FMT_RGBA16_FLOAT, .usage = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_sky_lut = rhi_create_texture(rhi, &td_sky);

    RhiTextureDesc td_aerial = {
        .width      = ATMOS_AERIAL_W,
        .height     = ATMOS_AERIAL_H,
        .mip_levels = RHI_MIP_DEPTH(1u, ATMOS_AERIAL_D),
        .format     = RHI_FMT_RGBA16_FLOAT,
        .usage      = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_aerial_3d = rhi_create_texture(rhi, &td_aerial);

    RhiTextureDesc td_noise = {
        .width      = CLOUD_NOISE_3D_N,
        .height     = CLOUD_NOISE_3D_N,
        .mip_levels = RHI_MIP_DEPTH(1u, CLOUD_NOISE_3D_N),
        .format     = RHI_FMT_RGBA16_FLOAT,
        .usage      = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_cloud_noise_3d = rhi_create_texture(rhi, &td_noise);

    RhiTextureDesc td_cloud = {
        .width = GPU_CLOUD_Q_W, .height = GPU_CLOUD_Q_H, .mip_levels = 1u,
        .format = RHI_FMT_RGBA16_FLOAT, .usage = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
    };
    st->tex_cloud_hist[0] = rhi_create_texture(rhi, &td_cloud);
    st->tex_cloud_hist[1] = rhi_create_texture(rhi, &td_cloud);

    st->ub_atmos_trans  = rhi_create_buffer(rhi, 16u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ub_atmos_ms     = rhi_create_buffer(rhi, 32u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ub_atmos_sky    = rhi_create_buffer(rhi, 48u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ub_atmos_aerial = rhi_create_buffer(rhi, 48u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ub_cloud_noise  = rhi_create_buffer(rhi, 16u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ub_cloud_march  = rhi_create_buffer(rhi, (u32)sizeof(GpuCloudUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    Vec4 u_tr = vec4((f32)ATMOS_TRANSMITTANCE_W, (f32)ATMOS_TRANSMITTANCE_H, 6360.0f, 6460.0f);
    rhi_write_buffer(rhi, st->ub_atmos_trans, 0u, &u_tr, sizeof(u_tr));

    Vec4 u_ms[2] = {
        vec4((f32)ATMOS_MULTISCATTER_W, (f32)ATMOS_MULTISCATTER_H, 6360.0f, 6460.0f),
        vec4((f32)ATMOS_TRANSMITTANCE_W, (f32)ATMOS_TRANSMITTANCE_H, 0.10f, 0.0f)
    };
    rhi_write_buffer(rhi, st->ub_atmos_ms, 0u, u_ms, sizeof(u_ms));

    Vec4 u_ns = vec4((f32)CLOUD_NOISE_3D_N, 1.0f / (f32)CLOUD_NOISE_3D_N, 0.0f, 0.0f);
    rhi_write_buffer(rhi, st->ub_cloud_noise, 0u, &u_ns, sizeof(u_ns));

    RhiBindGroupDesc bgd_trans = {
        .pipeline = st->cpipe_atmos_trans, .group_index = 0u,
        .entries = {
            { .binding = 0u,                       .kind = RHI_BIND_BUFFER,         .resource = st->ub_atmos_trans, .size = 16u },
            { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_trans_lut,  .size = trans_bytes }
        },
        .entry_count = 2u
    };
    st->bg_atmos_trans = rhi_create_bind_group(rhi, &bgd_trans);

    RhiBindGroupDesc bgd_ms = {
        .pipeline = st->cpipe_atmos_ms, .group_index = 0u,
        .entries = {
            { .binding = 0u,                       .kind = RHI_BIND_BUFFER,         .resource = st->ub_atmos_ms,   .size = 32u },
            { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_trans_lut, .size = trans_bytes },
            { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_ms_lut,    .size = ms_bytes }
        },
        .entry_count = 3u
    };
    st->bg_atmos_ms = rhi_create_bind_group(rhi, &bgd_ms);

    RhiBindGroupDesc bgd_sky = {
        .pipeline = st->cpipe_atmos_sky, .group_index = 0u,
        .entries = {
            { .binding = 0u,                       .kind = RHI_BIND_BUFFER,         .resource = st->ub_atmos_sky,  .size = 48u },
            { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_trans_lut, .size = trans_bytes },
            { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_ms_lut,    .size = ms_bytes },
            { .binding = MTK_WGSL_BUF_BINDING(2u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_sky_lut,   .size = sky_bytes }
        },
        .entry_count = 4u
    };
    st->bg_atmos_sky = rhi_create_bind_group(rhi, &bgd_sky);

    RhiBindGroupDesc bgd_aer = {
        .pipeline = st->cpipe_atmos_aerial, .group_index = 0u,
        .entries = {
            { .binding = 0u,                       .kind = RHI_BIND_BUFFER,         .resource = st->ub_atmos_aerial, .size = 48u },
            { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_trans_lut,   .size = trans_bytes },
            { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_ms_lut,      .size = ms_bytes },
            { .binding = MTK_WGSL_BUF_BINDING(2u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_aerial_lut,  .size = aerial_bytes }
        },
        .entry_count = 4u
    };
    st->bg_atmos_aerial = rhi_create_bind_group(rhi, &bgd_aer);

    RhiBindGroupDesc bgd_noise = {
        .pipeline = st->cpipe_cloud_noise, .group_index = 0u,
        .entries = {
            { .binding = 0u,                       .kind = RHI_BIND_BUFFER,         .resource = st->ub_cloud_noise,  .size = 16u },
            { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = st->buf_cloud_noise, .size = noise_bytes }
        },
        .entry_count = 2u
    };
    st->bg_cloud_noise = rhi_create_bind_group(rhi, &bgd_noise);

    RhiBindGroupDesc bgd_stat = {
        .pipeline = st->cpipe_static_to_tex, .group_index = 0u,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = st->buf_trans_lut,       .size = trans_bytes },
            { .binding = 1u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = st->buf_ms_lut,          .size = ms_bytes },
            { .binding = 2u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = st->buf_cloud_noise,     .size = noise_bytes },
            { .binding = 3u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_trans_lut },
            { .binding = 4u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_ms_lut },
            { .binding = 5u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_cloud_noise_3d },
            { .binding = 6u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_cloud_hist[0] },
            { .binding = 7u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_cloud_hist[1] }
        },
        .entry_count = 8u
    };
    st->bg_static_to_tex = rhi_create_bind_group(rhi, &bgd_stat);

    RhiBindGroupDesc bgd_dyn = {
        .pipeline = st->cpipe_dyn_to_tex, .group_index = 0u,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = st->buf_sky_lut,    .size = sky_bytes },
            { .binding = 1u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = st->buf_aerial_lut, .size = aerial_bytes },
            { .binding = 2u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_sky_lut },
            { .binding = 3u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_aerial_3d }
        },
        .entry_count = 4u
    };
    st->bg_dyn_to_tex = rhi_create_bind_group(rhi, &bgd_dyn);

    for (u32 ping = 0u; ping < 2u; ping++) {
        RhiBindGroupDesc bgd_cm = {
            .pipeline = st->cpipe_cloud_march, .group_index = 0u,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,          .resource = st->ub_cloud_march, .size = (u32)sizeof(GpuCloudUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE,         .resource = st->tex_cloud_noise_3d },
                { .binding = 2u, .kind = RHI_BIND_TEXTURE,         .resource = st->tex_trans_lut },
                { .binding = 3u, .kind = RHI_BIND_TEXTURE,         .resource = st->tex_sky_lut },
                { .binding = 4u, .kind = RHI_BIND_TEXTURE,         .resource = st->tex_aerial_3d },
                { .binding = 5u, .kind = RHI_BIND_TEXTURE,         .resource = st->tex_cloud_hist[ping ^ 1u] },
                { .binding = 6u, .kind = RHI_BIND_SAMPLER,         .resource = st->lin_sampler },
                { .binding = 7u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = st->tex_cloud_hist[ping] }
            },
            .entry_count = 8u
        };
        st->bg_cloud_march[ping] = rhi_create_bind_group(rhi, &bgd_cm);
    }

    /* 3. Her FFT kademesi için storage tamponları, storage dokuları ve bind group'lar */
    for (u32 ci = 0u; ci < OCEAN_CASCADES; ci++) {
        const OceanCascade* oc = &p->ocean->cascades[ci];
        GpuCascadeRes* gr = &st->casc[ci];
        u32 n  = oc->n;
        u32 nn = n * n;
        u32 v4_bytes  = nn * (u32)sizeof(Vec4);
        u32 f32_bytes = nn * (u32)sizeof(f32);

        gr->n          = n;
        gr->log2n      = oc->log2n;
        gr->num_stages = oc->log2n * 2u;

        gr->h0_buf    = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
        gr->kinfo_buf = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
        gr->fft_a01   = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);
        gr->fft_a23   = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);
        gr->fft_b01   = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);
        gr->fft_b23   = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);
        gr->foam_buf  = rhi_create_buffer(rhi, f32_bytes, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
        gr->disp_buf  = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);
        gr->casc_buf  = rhi_create_buffer(rhi, v4_bytes,  RHI_BUF_STORAGE);

        RhiTextureDesc st_td = {
            .width      = n,
            .height     = n,
            .mip_levels = 1u,
            .format     = RHI_FMT_RGBA16_FLOAT,
            .usage      = RHI_TEX_STORAGE | RHI_TEX_SAMPLED
        };
        gr->disp_tex = rhi_create_texture(rhi, &st_td);
        gr->casc_tex = rhi_create_texture(rhi, &st_td);

        gr->spec_ub   = rhi_create_buffer(rhi, 16u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        gr->unpack_ub = rhi_create_buffer(rhi, 32u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        gr->to_tex_ub = rhi_create_buffer(rhi, 16u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

        u32 tex_info[4] = { n, nn, 0u, 0u };
        rhi_write_buffer(rhi, gr->to_tex_ub, 0u, tex_info, sizeof(tex_info));

        upload_cascade_spectrum(rhi, scratch, oc, gr, TRUE);

        RhiBindGroupDesc spec_bgd = {
            .pipeline    = st->cpipe_spectrum,
            .group_index = 0u,
            .entries = {
                { .binding = 0u,                   .kind = RHI_BIND_BUFFER,         .resource = gr->spec_ub,   .size = 16u },
                { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->h0_buf,    .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->kinfo_buf, .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(2u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->fft_a01,   .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(3u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->fft_a23,   .size = v4_bytes }
            },
            .entry_count = 5u
        };
        gr->spec_bg = rhi_create_bind_group(rhi, &spec_bgd);

        for (u32 s = 0u; s < gr->num_stages; s++) {
            u32 axis = (s < gr->log2n) ? 0u : 1u;
            u32 pass = (s < gr->log2n) ? s : (s - gr->log2n);
            u32 half = 1u << pass;
            i32 su[4] = { (i32)n, (i32)half, (i32)axis, (i32)nn };
            gr->stage_ub[s] = rhi_create_buffer(rhi, 16u, RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
            rhi_write_buffer(rhi, gr->stage_ub[s], 0u, su, sizeof(su));

            RhiBuffer s01 = ((s & 1u) == 0u) ? gr->fft_a01 : gr->fft_b01;
            RhiBuffer s23 = ((s & 1u) == 0u) ? gr->fft_a23 : gr->fft_b23;
            RhiBuffer d01 = ((s & 1u) == 0u) ? gr->fft_b01 : gr->fft_a01;
            RhiBuffer d23 = ((s & 1u) == 0u) ? gr->fft_b23 : gr->fft_a23;

            RhiBindGroupDesc st_bgd = {
                .pipeline    = st->cpipe_fft_stage,
                .group_index = 0u,
                .entries = {
                    { .binding = 0u,                   .kind = RHI_BIND_BUFFER,         .resource = gr->stage_ub[s], .size = 16u },
                    { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = s01,             .size = v4_bytes },
                    { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = s23,             .size = v4_bytes },
                    { .binding = MTK_WGSL_BUF_BINDING(2u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = d01,             .size = v4_bytes },
                    { .binding = MTK_WGSL_BUF_BINDING(3u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = d23,             .size = v4_bytes }
                },
                .entry_count = 5u
            };
            gr->stage_bg[s] = rhi_create_bind_group(rhi, &st_bgd);
        }

        RhiBindGroupDesc unp_bgd = {
            .pipeline    = st->cpipe_unpack,
            .group_index = 0u,
            .entries = {
                { .binding = 0u,                   .kind = RHI_BIND_BUFFER,         .resource = gr->unpack_ub, .size = 32u },
                { .binding = MTK_WGSL_BUF_BINDING(0u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->fft_a01,   .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(1u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->fft_a23,   .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(2u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->foam_buf,  .size = f32_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(3u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->disp_buf,  .size = v4_bytes },
                { .binding = MTK_WGSL_BUF_BINDING(4u), .kind = RHI_BIND_STORAGE_BUFFER, .resource = gr->casc_buf,  .size = v4_bytes }
            },
            .entry_count = 6u
        };
        gr->unpack_bg = rhi_create_bind_group(rhi, &unp_bgd);

        RhiBindGroupDesc tt_bgd = {
            .pipeline    = st->cpipe_to_tex,
            .group_index = 0u,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,          .resource = gr->to_tex_ub, .size = 16u },
                { .binding = 1u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = gr->disp_buf,  .size = v4_bytes },
                { .binding = 2u, .kind = RHI_BIND_STORAGE_BUFFER,  .resource = gr->casc_buf,  .size = v4_bytes },
                { .binding = 3u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = gr->disp_tex },
                { .binding = 4u, .kind = RHI_BIND_STORAGE_TEXTURE, .resource = gr->casc_tex }
            },
            .entry_count = 5u
        };
        gr->to_tex_bg = rhi_create_bind_group(rhi, &tt_bgd);
    }

    /* 4. Render shader'larının derlenmesi (k_wgsl_ocean_fs, k_wgsl_sky_fs, lit3d.mtk, unlit3d.mtk) */
    (void)p->ocean_mtk_src;
    (void)p->ocean_mtk_len;
    (void)p->sky_mtk_src;
    (void)p->sky_mtk_len;
    RhiShader vs_ocean = rhi_create_shader(
        rhi, k_wgsl_ocean_vs, (u32)(sizeof(k_wgsl_ocean_vs) - 1u));
    RhiShader fs_ocean = rhi_create_shader(
        rhi, k_wgsl_ocean_fs, (u32)(sizeof(k_wgsl_ocean_fs) - 1u));

    static const u8 k_sky_attrs[1] = { MTK_WGSL_ATTR_NORMAL };
    RhiShader vs_sky = compile_vs_attrs_to_rhi(rhi, scratch, k_sky_attrs, 1u);
    RhiShader fs_sky = rhi_create_shader(
        rhi, k_wgsl_sky_fs, (u32)(sizeof(k_wgsl_sky_fs) - 1u));

    static const u8 k_lit_attrs[3] = {
        MTK_WGSL_ATTR_UV, MTK_WGSL_ATTR_NORMAL, MTK_WGSL_ATTR_VIEW_DEPTH
    };
    RhiShader vs_lit = compile_vs_attrs_to_rhi(rhi, scratch, k_lit_attrs, 3u);
    RhiShader fs_lit = compile_mtk_shader_to_rhi(
        rhi, scratch, p->lit_mtk_src, p->lit_mtk_len, "lit3d.mtk", MTK_WGSL_FRAG_GROUP, "frag");

    static const u8 k_unlit_attrs[2] = {
        MTK_WGSL_ATTR_NORMAL, MTK_WGSL_ATTR_VIEW_DEPTH
    };
    RhiShader vs_unlit = compile_vs_attrs_to_rhi(rhi, scratch, k_unlit_attrs, 2u);
    RhiShader fs_unlit = compile_mtk_shader_to_rhi(
        rhi, scratch, p->unlit_mtk_src, p->unlit_mtk_len, "unlit3d.mtk", MTK_WGSL_FRAG_GROUP, "frag");

    /* 5. Render pipeline'ları (MSAA 4x, Reverse-Z GREATER) */
    RhiPipelineDesc ocean_pd = {
        .vertex_shader   = vs_ocean,
        .fragment_shader = fs_ocean,
        .vs_entry        = "vert",
        .fs_entry        = "frag",
        .vertex_stride   = 8u,
        .attrs = {
            { .shader_location = 0u, .offset = 0u, .format = RHI_VFMT_FLOAT32X2 }
        },
        .attr_count    = 1u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_BACK,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_SURFACE_DEFAULT,
        .blend_mode    = RHI_BLEND_NONE,
        .depth_format  = RHI_FMT_DEPTH32_FLOAT,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_GREATER,
        .sample_count  = GPU_OCEAN_MSAA_SAMPLES
    };
    st->pipe_ocean = rhi_create_pipeline(rhi, &ocean_pd);

    RhiPipelineDesc sky_pd = {
        .vertex_shader   = vs_sky,
        .fragment_shader = fs_sky,
        .vs_entry        = "vert",
        .fs_entry        = "frag",
        .vertex_stride   = (u32)sizeof(GpuVertex3d),
        .attrs = {
            { .shader_location = 0u, .offset = 0u,  .format = RHI_VFMT_FLOAT32X3 },
            { .shader_location = 1u, .offset = 12u, .format = RHI_VFMT_FLOAT32X3 }
        },
        .attr_count    = 2u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_FRONT,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_SURFACE_DEFAULT,
        .blend_mode    = RHI_BLEND_NONE,
        .depth_format  = RHI_FMT_DEPTH32_FLOAT,
        .depth_test    = TRUE,
        .depth_write   = FALSE,
        .depth_compare = RHI_CMP_GREATER,
        .sample_count  = GPU_OCEAN_MSAA_SAMPLES
    };
    st->pipe_sky = rhi_create_pipeline(rhi, &sky_pd);

    RhiPipelineDesc lit_pd = {
        .vertex_shader   = vs_lit,
        .fragment_shader = fs_lit,
        .vs_entry        = "vert",
        .fs_entry        = "frag",
        .vertex_stride   = (u32)sizeof(GpuVertex3d),
        .attrs = {
            { .shader_location = 0u, .offset = 0u,  .format = RHI_VFMT_FLOAT32X3 },
            { .shader_location = 1u, .offset = 12u, .format = RHI_VFMT_FLOAT32X3 },
            { .shader_location = 2u, .offset = 24u, .format = RHI_VFMT_FLOAT32X2 }
        },
        .attr_count    = 3u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_BACK,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_SURFACE_DEFAULT,
        .blend_mode    = RHI_BLEND_NONE,
        .depth_format  = RHI_FMT_DEPTH32_FLOAT,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_GREATER,
        .sample_count  = GPU_OCEAN_MSAA_SAMPLES
    };
    st->pipe_lit_back = rhi_create_pipeline(rhi, &lit_pd);
    lit_pd.cull_mode  = RHI_CULL_NONE;
    st->pipe_lit_none = rhi_create_pipeline(rhi, &lit_pd);

    RhiPipelineDesc unlit_pd = {
        .vertex_shader   = vs_unlit,
        .fragment_shader = fs_unlit,
        .vs_entry        = "vert",
        .fs_entry        = "frag",
        .vertex_stride   = (u32)sizeof(GpuVertex3d),
        .attrs = {
            { .shader_location = 0u, .offset = 0u,  .format = RHI_VFMT_FLOAT32X3 },
            { .shader_location = 1u, .offset = 12u, .format = RHI_VFMT_FLOAT32X3 }
        },
        .attr_count    = 2u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_BACK,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_SURFACE_DEFAULT,
        .blend_mode    = RHI_BLEND_NONE,
        .depth_format  = RHI_FMT_DEPTH32_FLOAT,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_GREATER,
        .sample_count  = GPU_OCEAN_MSAA_SAMPLES
    };
    st->pipe_unlit = rhi_create_pipeline(rhi, &unlit_pd);

    /* 6. Ağların ve tekne dokularının yüklenmesi */
    build_gpu_ocean_grid(rhi, scratch, &st->ocean_grid);
    upload_mesh_3d(rhi, scratch, p->sky_mesh,       &st->sky_mesh);
    upload_mesh_3d(rhi, scratch, p->hull_mesh,      &st->hull_mesh);
    upload_mesh_3d(rhi, scratch, p->main_sail_mesh, &st->main_sail_mesh);
    upload_mesh_3d(rhi, scratch, p->jib_sail_mesh,  &st->jib_sail_mesh);
    upload_mesh_3d(rhi, scratch, p->unit_cube,      &st->cube_mesh);

    st->tex_hull = upload_texture_rgba8(rhi, p->tex_hull);
    st->tex_deck = upload_texture_rgba8(rhi, p->tex_deck);
    st->tex_sail = upload_texture_rgba8(rhi, p->tex_sail);

    /* 7. Render uniform tamponları ve bind group'lar */
    st->ocean_vs_ub = rhi_create_buffer(rhi, (u32)sizeof(GpuOceanVsUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->ocean_fs_ub = rhi_create_buffer(rhi, (u32)sizeof(GpuOceanFsUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    RhiBindGroupDesc ovs_bgd = {
        .pipeline    = st->pipe_ocean,
        .group_index = MTK_WGSL_VERT_GROUP,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->ocean_vs_ub, .size = (u32)sizeof(GpuOceanVsUniforms) },
            { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[0].disp_tex },
            { .binding = 2u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[1].disp_tex },
            { .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[2].disp_tex },
            { .binding = 4u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[0].casc_tex },
            { .binding = 5u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[1].casc_tex },
            { .binding = 6u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[2].casc_tex },
            { .binding = 7u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler }
        },
        .entry_count = 8u
    };
    st->ocean_vs_bg = rhi_create_bind_group(rhi, &ovs_bgd);

    st->sky_vs_ub = rhi_create_buffer(rhi, (u32)sizeof(GpuVsUniforms),    RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    st->sky_fs_ub = rhi_create_buffer(rhi, (u32)sizeof(GpuSkyFsUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    RhiBindGroupDesc svs_bgd = {
        .pipeline    = st->pipe_sky,
        .group_index = MTK_WGSL_VERT_GROUP,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->sky_vs_ub, .size = (u32)sizeof(GpuVsUniforms) }
        },
        .entry_count = 1u
    };
    st->sky_vs_bg = rhi_create_bind_group(rhi, &svs_bgd);

    for (u32 ping = 0u; ping < 2u; ping++) {
        RhiBindGroupDesc ofs_bgd = {
            .pipeline    = st->pipe_ocean,
            .group_index = MTK_WGSL_FRAG_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->ocean_fs_ub, .size = (u32)sizeof(GpuOceanFsUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[0].casc_tex },
                { .binding = 2u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[1].casc_tex },
                { .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[2].casc_tex },
                { .binding = 4u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_sky_lut },
                { .binding = 5u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_aerial_3d },
                { .binding = 6u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_cloud_hist[ping] },
                { .binding = 7u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler }
            },
            .entry_count = 8u
        };
        st->ocean_fs_bg[ping] = rhi_create_bind_group(rhi, &ofs_bgd);

        RhiBindGroupDesc sfs_bgd = {
            .pipeline    = st->pipe_sky,
            .group_index = MTK_WGSL_FRAG_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->sky_fs_ub, .size = (u32)sizeof(GpuSkyFsUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_sky_lut },
                { .binding = 2u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_trans_lut },
                { .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_cloud_hist[ping] },
                { .binding = 4u, .kind = RHI_BIND_SAMPLER, .resource = st->clamp_sampler }
            },
            .entry_count = 5u
        };
        st->sky_fs_bg[ping] = rhi_create_bind_group(rhi, &sfs_bgd);
    }

    /* Tekne parçaları (0..9): 3 ve 7 unlit, 0,8,9 lit_none, diğerleri lit_back */
    const RhiTexture part_tex[GPU_OCEAN_BOAT_PARTS] = {
        st->tex_hull, st->tex_deck, st->tex_hull, RHI_INVALID_HANDLE,
        st->tex_deck, st->tex_deck, st->tex_deck, RHI_INVALID_HANDLE,
        st->tex_sail, st->tex_sail
    };
    const RhiPipeline part_pipe[GPU_OCEAN_BOAT_PARTS] = {
        st->pipe_lit_none, st->pipe_lit_back, st->pipe_lit_back, st->pipe_unlit,
        st->pipe_lit_back, st->pipe_lit_back, st->pipe_lit_back, st->pipe_unlit,
        st->pipe_lit_none, st->pipe_lit_none
    };

    for (u32 i = 0u; i < GPU_OCEAN_BOAT_PARTS; i++) {
        b32 is_unlit = (i == 3u || i == 7u);
        u32 fs_size  = is_unlit ? (u32)sizeof(GpuUnlitFsUniforms) : (u32)sizeof(GpuLitFsUniforms);
        st->boat_vs_ub[i] = rhi_create_buffer(rhi, (u32)sizeof(GpuVsUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        st->boat_fs_ub[i] = rhi_create_buffer(rhi, fs_size,                    RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

        RhiBindGroupDesc bvs_bgd = {
            .pipeline    = part_pipe[i],
            .group_index = MTK_WGSL_VERT_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->boat_vs_ub[i], .size = (u32)sizeof(GpuVsUniforms) }
            },
            .entry_count = 1u
        };
        st->boat_vs_bg[i] = rhi_create_bind_group(rhi, &bvs_bgd);

        if (is_unlit) {
            RhiBindGroupDesc bfs_bgd = {
                .pipeline    = part_pipe[i],
                .group_index = MTK_WGSL_FRAG_GROUP,
                .entries = {
                    { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->boat_fs_ub[i], .size = fs_size }
                },
                .entry_count = 1u
            };
            st->boat_fs_bg[i] = rhi_create_bind_group(rhi, &bfs_bgd);
        } else {
            RhiBindGroupDesc bfs_bgd = {
                .pipeline    = part_pipe[i],
                .group_index = MTK_WGSL_FRAG_GROUP,
                .entries = {
                    { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->boat_fs_ub[i], .size = fs_size },
                    { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = part_tex[i] },
                    { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler }
                },
                .entry_count = 3u
            };
            st->boat_fs_bg[i] = rhi_create_bind_group(rhi, &bfs_bgd);
        }
    }

    /* Ek 3D dünya nesneleri için uniform tamponları ve bind group'lar */
    for (u32 i = 0u; i < GPU_OCEAN_MAX_EXTRAS; i++) {
        st->extra_vs_ub[i]       = rhi_create_buffer(rhi, (u32)sizeof(GpuVsUniforms),      RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        st->extra_lit_fs_ub[i]   = rhi_create_buffer(rhi, (u32)sizeof(GpuLitFsUniforms),   RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        st->extra_unlit_fs_ub[i] = rhi_create_buffer(rhi, (u32)sizeof(GpuUnlitFsUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

        RhiBindGroupDesc lvs_bgd = {
            .pipeline    = st->pipe_lit_back,
            .group_index = MTK_WGSL_VERT_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->extra_vs_ub[i], .size = (u32)sizeof(GpuVsUniforms) }
            },
            .entry_count = 1u
        };
        st->extra_lit_vs_bg[i] = rhi_create_bind_group(rhi, &lvs_bgd);

        RhiBindGroupDesc lfs_bgd = {
            .pipeline    = st->pipe_lit_back,
            .group_index = MTK_WGSL_FRAG_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->extra_lit_fs_ub[i], .size = (u32)sizeof(GpuLitFsUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->tex_deck },
                { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler }
            },
            .entry_count = 3u
        };
        st->extra_lit_fs_bg[i] = rhi_create_bind_group(rhi, &lfs_bgd);

        RhiBindGroupDesc uvs_bgd = {
            .pipeline    = st->pipe_unlit,
            .group_index = MTK_WGSL_VERT_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->extra_vs_ub[i], .size = (u32)sizeof(GpuVsUniforms) }
            },
            .entry_count = 1u
        };
        st->extra_unlit_vs_bg[i] = rhi_create_bind_group(rhi, &uvs_bgd);

        RhiBindGroupDesc ufs_bgd = {
            .pipeline    = st->pipe_unlit,
            .group_index = MTK_WGSL_FRAG_GROUP,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->extra_unlit_fs_ub[i], .size = (u32)sizeof(GpuUnlitFsUniforms) }
            },
            .entry_count = 1u
        };
        st->extra_unlit_fs_bg[i] = rhi_create_bind_group(rhi, &ufs_bgd);
    }

    st->initialized = TRUE;
    return !rhi->overflow && rhi->dropped_cmds == 0u;
}

static void emit_boat_part_draw(GpuOceanState* st, u32 idx, const GpuMeshRes* mesh,
                                RhiPipeline pipe, Mat4 vp, Mat4 boat_m,
                                Vec3 pos, Vec3 scale, f32 rot_x,
                                Vec4 color, Vec3 light_dir, Vec3 fog_srgb, f32 fog_dens,
                                b32 is_unlit, u32* io_tris) {
    Rhi* rhi = &st->rhi;
    Mat4 local = mat4_mul(mat4_translate(pos), mat4_mul(mat4_rotate_x(rot_x), mat4_scale(scale)));
    Mat4 model = mat4_mul(boat_m, local);

    GpuVsUniforms vsu;
    vsu.mvp        = mat4_mul(vp, model);
    vsu.model      = model;
    vsu.normal_mat = gpu_mat4_transpose(mat4_inverse(model));
    vsu.shadow_mat = mat4_identity();
    rhi_write_buffer(rhi, st->boat_vs_ub[idx], 0u, &vsu, sizeof(vsu));

    if (is_unlit) {
        GpuUnlitFsUniforms fsu;
        fsu.color = color;
        fsu.fog   = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
        rhi_write_buffer(rhi, st->boat_fs_ub[idx], 0u, &fsu, sizeof(fsu));
    } else {
        GpuLitFsUniforms fsu;
        fsu.light_dir = vec4(light_dir.x, light_dir.y, light_dir.z, 0.0f);
        fsu.base      = color;
        fsu.fog       = vec4(fog_srgb.x, fog_srgb.y, fog_srgb.z, fog_dens);
        rhi_write_buffer(rhi, st->boat_fs_ub[idx], 0u, &fsu, sizeof(fsu));
    }

    rhi_set_pipeline(rhi, pipe);
    rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->boat_fs_bg[idx]);
    rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->boat_vs_bg[idx]);
    rhi_set_vertex_buffer(rhi, 0u, mesh->vb, 0u, 0u);
    rhi_set_index_buffer(rhi, mesh->ib, RHI_INDEX_UINT32, 0u, 0u);
    rhi_draw_indexed(rhi, mesh->index_count, 1u, 0u, 0, 0u);
    *io_tris += mesh->index_count / 3u;
}

/* ---------- Kare Başına GPU Komut Akışı Kodlaması ---------- */

b32 gpu_ocean_render(Arena* scratch, const GpuOceanFrameParams* p, u32* out_tris) {
    if (!scratch || !p || !p->ocean) return FALSE;
    GpuOceanState* st = &g_gpu_ocean;

    if (!st->initialized) {
        if (!gpu_ocean_init_resources(st, scratch, p)) {
            return FALSE;
        }
    } else {
        rhi_set_active(&st->rhi);
        rhi_begin_frame(&st->rhi);
    }

    Rhi* rhi = &st->rhi;
    rhi_poll_errors(rhi);

    u32 vp_w = engine_gpu_viewport_width();
    u32 vp_h = engine_gpu_viewport_height();
    if (vp_w == 0u) vp_w = 1920u;
    if (vp_h == 0u) vp_h = 1080u;

    if (st->depth_tex == RHI_INVALID_HANDLE || st->depth_w != vp_w || st->depth_h != vp_h) {
        if (st->depth_tex != RHI_INVALID_HANDLE) {
            rhi_destroy_resource(rhi, st->depth_tex);
        }
        RhiTextureDesc dtd = {
            .width      = vp_w,
            .height     = vp_h,
            .mip_levels = 1u,
            .format     = RHI_FMT_DEPTH32_FLOAT,
            .usage      = RHI_TEX_RENDER_ATTACHMENT
        };
        st->depth_tex = rhi_create_texture(rhi, &dtd);
        st->depth_w   = vp_w;
        st->depth_h   = vp_h;
    }

    /* Rüzgâr değiştiyse yeni h0 spektrumunu GPU'ya yükle */
    if (p->spectrum_dirty) {
        for (u32 ci = 0u; ci < OCEAN_CASCADES; ci++) {
            upload_cascade_spectrum(rhi, scratch, &p->ocean->cascades[ci], &st->casc[ci], FALSE);
        }
    }

    /* 1. Compute Pass: 3 Kademeli Tessendorf FFT + Unpack + Storage Doku Yazımı */
    f32 lam      = p->ocean->d.choppy;
    f32 j_thr    = p->ocean->d.foam_threshold;
    f32 f_gain   = p->ocean->d.foam_scale;
    f32 dt       = p->eff_dt;
    f32 decay    = p->first_update ? 0.0f : ((dt > 0.0f) ? gpu_ocean_exp(-p->ocean->d.foam_decay * dt) : 1.0f);
    f32 inject   = p->first_update ? 1.0f : (dt > 0.0f ? m_min(dt * 3.8f, 1.0f) : 0.0f);
    f32 s_enc_nm = (127.0f / 255.0f) / OCEAN_TEX_SLOPE_MAX;
    f32 h_norm   = 1.0f / m_max(p->ocean->hs, 0.5f);

    for (u32 ci = 0u; ci < OCEAN_CASCADES; ci++) {
        GpuCascadeRes* gr = &st->casc[ci];
        u32 nn = gr->n * gr->n;
        Vec4 spec_u = vec4((f32)p->sim_time, (f32)nn, 0.0f, 0.0f);
        rhi_write_buffer(rhi, gr->spec_ub, 0u, &spec_u, sizeof(spec_u));

        Vec4 unp_u[2] = {
            vec4(lam, j_thr, f_gain, decay),
            vec4(inject, s_enc_nm, h_norm, (f32)nn)
        };
        rhi_write_buffer(rhi, gr->unpack_ub, 0u, unp_u, sizeof(unp_u));
    }

    rhi_begin_compute_pass(rhi);
    for (u32 ci = 0u; ci < OCEAN_CASCADES; ci++) {
        GpuCascadeRes* gr = &st->casc[ci];
        u32 nn     = gr->n * gr->n;
        u32 groups = (nn + 63u) / 64u;

        rhi_set_compute_pipeline(rhi, st->cpipe_spectrum);
        rhi_set_bind_group(rhi, 0u, gr->spec_bg);
        rhi_dispatch(rhi, groups, 1u, 1u);

        rhi_set_compute_pipeline(rhi, st->cpipe_fft_stage);
        for (u32 s = 0u; s < gr->num_stages; s++) {
            rhi_set_bind_group(rhi, 0u, gr->stage_bg[s]);
            rhi_dispatch(rhi, groups, 1u, 1u);
        }

        rhi_set_compute_pipeline(rhi, st->cpipe_unpack);
        rhi_set_bind_group(rhi, 0u, gr->unpack_bg);
        rhi_dispatch(rhi, groups, 1u, 1u);

        rhi_set_compute_pipeline(rhi, st->cpipe_to_tex);
        rhi_set_bind_group(rhi, 0u, gr->to_tex_bg);
        rhi_dispatch(rhi, groups, 1u, 1u);
    }
    rhi_end_compute_pass(rhi);

    /* 1b. V1 Hillaire Atmosfer LUT'ları + 3B Bulut Gürültüsü + Çeyrek Çözünürlüklü Bulut Raymarching */
    u32 ping = st->cloud_ping & 1u;

    /* Dinamik atmosfer uniform tamponları (Sky-View 192x108 ve Aerial Perspective 32x32x16) */
    f32 cam_h_km = m_max(p->eye.y * 0.001f, 0.015f);
    Vec4 u_sky_lut[3] = {
        vec4((f32)ATMOS_SKYVIEW_W, (f32)ATMOS_SKYVIEW_H, cam_h_km, 0.80f),
        vec4(p->sun_dir.x, p->sun_dir.y, p->sun_dir.z, 20.0f),
        vec4((f32)ATMOS_TRANSMITTANCE_W, (f32)ATMOS_TRANSMITTANCE_H,
             (f32)ATMOS_MULTISCATTER_W, (f32)ATMOS_MULTISCATTER_H)
    };
    rhi_write_buffer(rhi, st->ub_atmos_sky, 0u, u_sky_lut, sizeof(u_sky_lut));

    Vec4 u_aer_lut[3] = {
        vec4((f32)ATMOS_AERIAL_W, (f32)ATMOS_AERIAL_H, (f32)ATMOS_AERIAL_D, 2.0f),
        vec4(p->sun_dir.x, p->sun_dir.y, p->sun_dir.z, 20.0f),
        vec4((f32)ATMOS_TRANSMITTANCE_W, (f32)ATMOS_TRANSMITTANCE_H,
             (f32)ATMOS_MULTISCATTER_W, (f32)ATMOS_MULTISCATTER_H)
    };
    rhi_write_buffer(rhi, st->ub_atmos_aerial, 0u, u_aer_lut, sizeof(u_aer_lut));

    /* Kamera baz vektörlerini view ve proj matrislerinden çıkar */
    f32 proj_x = (m_abs(p->proj.m[0]) > 1e-4f) ? (1.0f / p->proj.m[0]) : 1.38f;
    f32 proj_y = (m_abs(p->proj.m[5]) > 1e-4f) ? (1.0f / p->proj.m[5]) : 0.78f;
    Vec3 c_right = vec3_scale(vec3(p->view.m[0], p->view.m[4], p->view.m[8]), proj_x);
    Vec3 c_up    = vec3_scale(vec3(p->view.m[1], p->view.m[5], p->view.m[9]), proj_y);
    Vec3 c_fwd   = vec3(-p->view.m[2], -p->view.m[6], -p->view.m[10]);

    f32 sun_jump = m_abs(p->sun_dir.y - st->last_sun_y) + m_abs(p->cloud_coverage - st->last_cov);
    f32 blend_a  = (!st->static_luts_built || p->first_update || sun_jump > 0.04f) ? 1.0f : 0.16f;
    st->last_sun_y = p->sun_dir.y;
    st->last_cov   = p->cloud_coverage;

    GpuCloudUniforms cl_u;
    cl_u.prev_vp    = p->prev_vp;
    cl_u.cam_fwd    = vec4(c_fwd.x, c_fwd.y, c_fwd.z, 0.0f);
    cl_u.cam_right  = vec4(c_right.x, c_right.y, c_right.z, 0.0f);
    cl_u.cam_up     = vec4(c_up.x, c_up.y, c_up.z, 0.0f);
    cl_u.eye_time   = vec4(p->eye.x, p->eye.y, p->eye.z, (f32)p->sim_time);
    cl_u.sun_cov    = vec4(p->sun_dir.x, p->sun_dir.y, p->sun_dir.z, p->cloud_coverage);
    cl_u.moon_star  = vec4(p->moon_dir.x, p->moon_dir.y, p->moon_dir.z, p->moon_intensity);
    cl_u.wind_frame = vec4(p->cloud_wind_x, p->cloud_wind_z, (f32)(st->frame_idx & 1023u), blend_a);
    rhi_write_buffer(rhi, st->ub_cloud_march, 0u, &cl_u, sizeof(cl_u));

    /* İlk karede statik LUT'ları (Transmittance 256x64, Multi-scatter 32x32, 3B Perlin-Worley 32^3) üret */
    if (!st->static_luts_built) {
        rhi_begin_compute_pass(rhi);

        rhi_set_compute_pipeline(rhi, st->cpipe_atmos_trans);
        rhi_set_bind_group(rhi, 0u, st->bg_atmos_trans);
        rhi_dispatch(rhi, (ATMOS_TRANSMITTANCE_W * ATMOS_TRANSMITTANCE_H + 63u) / 64u, 1u, 1u);

        rhi_set_compute_pipeline(rhi, st->cpipe_atmos_ms);
        rhi_set_bind_group(rhi, 0u, st->bg_atmos_ms);
        rhi_dispatch(rhi, (ATMOS_MULTISCATTER_W * ATMOS_MULTISCATTER_H + 63u) / 64u, 1u, 1u);

        rhi_set_compute_pipeline(rhi, st->cpipe_cloud_noise);
        rhi_set_bind_group(rhi, 0u, st->bg_cloud_noise);
        rhi_dispatch(rhi, (CLOUD_NOISE_3D_N * CLOUD_NOISE_3D_N * CLOUD_NOISE_3D_N + 63u) / 64u, 1u, 1u);

        rhi_set_compute_pipeline(rhi, st->cpipe_static_to_tex);
        rhi_set_bind_group(rhi, 0u, st->bg_static_to_tex);
        rhi_dispatch(rhi, (GPU_CLOUD_Q_W * GPU_CLOUD_Q_H + 63u) / 64u, 1u, 1u);

        rhi_end_compute_pass(rhi);
        st->static_luts_built = TRUE;
    }

    /* Kare başına ölçülen (RHI_PASS_TIMED_SKY) Atmosfer + Bulut compute pass'i */
    rhi_begin_compute_pass_ex(rhi, RHI_PASS_TIMED_SKY);

    rhi_set_compute_pipeline(rhi, st->cpipe_atmos_sky);
    rhi_set_bind_group(rhi, 0u, st->bg_atmos_sky);
    rhi_dispatch(rhi, (ATMOS_SKYVIEW_W * ATMOS_SKYVIEW_H + 63u) / 64u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, st->cpipe_atmos_aerial);
    rhi_set_bind_group(rhi, 0u, st->bg_atmos_aerial);
    rhi_dispatch(rhi, (ATMOS_AERIAL_W * ATMOS_AERIAL_H * ATMOS_AERIAL_D + 63u) / 64u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, st->cpipe_dyn_to_tex);
    rhi_set_bind_group(rhi, 0u, st->bg_dyn_to_tex);
    rhi_dispatch(rhi, (ATMOS_SKYVIEW_W * ATMOS_SKYVIEW_H + 63u) / 64u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, st->cpipe_cloud_march);
    rhi_set_bind_group(rhi, 0u, st->bg_cloud_march[ping]);
    rhi_dispatch(rhi, (GPU_CLOUD_Q_W + 7u) / 8u, (GPU_CLOUD_Q_H + 7u) / 8u, 1u);

    rhi_end_compute_pass(rhi);

    /* 2. Render Uniform Güncellemeleri */
    Mat4 vp = mat4_mul(p->proj, p->view);

    f32 snap = GPU_OCEAN_GRID_SNAP;
    f32 cx   = m_floor(p->eye.x / snap + 0.5f) * snap;
    f32 cz   = m_floor(p->eye.z / snap + 0.5f) * snap;
    f32 l0   = p->ocean->cascades[0].size;
    f32 l1   = p->ocean->cascades[1].size;
    f32 l2   = p->ocean->cascades[2].size;

    GpuOceanVsUniforms ovsu;
    ovsu.mvp         = vp;
    ovsu.eye_snap    = vec4(cx, cz, p->ocean->d.choppy, 1.0f / l0);
    ovsu.eye_pos     = vec4(p->eye.x, p->eye.y, p->eye.z, OCEAN_TEX_SLOPE_MAX);
    ovsu.casc_scales = vec4(1.0f / l0, 1.0f / l1, 1.0f / l2, 0.0f);
    rhi_write_buffer(rhi, st->ocean_vs_ub, 0u, &ovsu, sizeof(ovsu));

    GpuOceanFsUniforms ofsu;
    ofsu.sun_dir     = vec4(p->sun_dir.x, p->sun_dir.y, p->sun_dir.z, 0.0f);
    ofsu.sun_col     = vec4(p->sun_col.x, p->sun_col.y, p->sun_col.z, 0.0f);
    ofsu.sky_zenith  = vec4(p->sky_zenith.x, p->sky_zenith.y, p->sky_zenith.z, 0.0f);
    ofsu.sky_horizon = vec4(p->sky_horiz.x, p->sky_horiz.y, p->sky_horiz.z, 0.0f);
    ofsu.sea_deep    = vec4(p->sea_deep.x, p->sea_deep.y, p->sea_deep.z, 0.0f);
    ofsu.sea_shallow = vec4(p->sea_shallow.x, p->sea_shallow.y, p->sea_shallow.z, 0.0f);
    ofsu.eye         = vec4(p->eye.x, p->eye.y, p->eye.z, 0.0f);
    ofsu.scales      = vec4(l0 / l1, l0 / l2, OCEAN_TEX_SLOPE_MAX * 2.0f, p->fog_dens);
    ofsu.params      = vec4(1.25f, 1.15f, 1.10f, 0.35f + 0.65f * p->warm);
    ofsu.moon_dir    = vec4(p->moon_dir.x, p->moon_dir.y, p->moon_dir.z, p->star_intensity);
    ofsu.viewport    = vec4((f32)vp_w, (f32)vp_h, p->moon_intensity, p->cloud_coverage);
    rhi_write_buffer(rhi, st->ocean_fs_ub, 0u, &ofsu, sizeof(ofsu));

    Mat4 sky_model = mat4_translate(p->eye);
    GpuVsUniforms svsu;
    svsu.mvp        = mat4_mul(vp, sky_model);
    svsu.model      = sky_model;
    svsu.normal_mat = mat4_identity();
    svsu.shadow_mat = mat4_identity();
    rhi_write_buffer(rhi, st->sky_vs_ub, 0u, &svsu, sizeof(svsu));

    GpuSkyFsUniforms sfsu;
    sfsu.sun_dir     = vec4(p->sun_dir.x, p->sun_dir.y, p->sun_dir.z, 0.0f);
    sfsu.sun_col     = vec4(p->sun_col.x, p->sun_col.y, p->sun_col.z, 0.0f);
    sfsu.sky_zenith  = vec4(p->sky_zenith.x, p->sky_zenith.y, p->sky_zenith.z, 0.0f);
    sfsu.sky_horizon = vec4(p->sky_horiz.x, p->sky_horiz.y, p->sky_horiz.z, 0.0f);
    f32 cloud_val    = (p->cloud_cover > 0.0f) ? p->cloud_cover : 0.42f;
    sfsu.params      = vec4((f32)p->sim_time, cloud_val, 1.0f, 0.35f + 0.65f * p->warm);
    sfsu.moon_dir    = vec4(p->moon_dir.x, p->moon_dir.y, p->moon_dir.z, p->star_intensity);
    sfsu.viewport    = vec4((f32)vp_w, (f32)vp_h, p->moon_intensity, p->cloud_coverage);
    rhi_write_buffer(rhi, st->sky_fs_ub, 0u, &sfsu, sizeof(sfsu));

    /* XPBD yelken ağları dinamikse köşe tamponlarını güncelle */
    if (p->dynamic_sails) {
        update_mesh_3d_vb(rhi, scratch, p->main_sail_mesh, &st->main_sail_mesh);
        update_mesh_3d_vb(rhi, scratch, p->jib_sail_mesh,  &st->jib_sail_mesh);
    }

    /* 3. Render Pass (MSAA 4x, Reverse-Z clear_depth = 0.0) */
    RhiPassDesc pass = {
        .color_texture   = RHI_INVALID_HANDLE,
        .depth_texture   = st->depth_tex,
        .clear_color     = TRUE,
        .clear_r         = 0.047f,
        .clear_g         = 0.094f,
        .clear_b         = 0.165f,
        .clear_a         = 1.0f,
        .clear_depth     = TRUE,
        .clear_depth_val = 0.0f,
        .sample_count    = GPU_OCEAN_MSAA_SAMPLES
    };
    rhi_begin_pass(rhi, &pass);
    rhi_set_viewport(rhi, 0.0f, 0.0f, (f32)vp_w, (f32)vp_h, 0.0f, 1.0f);

    u32 tris = 0u;

    /* (a) Yüzen tekne ağları (10 parça) */
    Mat4 bm = p->boat_model;
    Vec3 sd = p->sun_dir;
    Vec3 fg = p->fog_srgb;
    f32  fd = p->fog_dens;

    emit_boat_part_draw(st, 0u, &st->hull_mesh, st->pipe_lit_none, vp, bm,
                        vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                        vec4(1.0f, 0.94f, 0.88f, 1.0f), sd, fg, fd, FALSE, &tris);
    emit_boat_part_draw(st, 1u, &st->cube_mesh, st->pipe_lit_back, vp, bm,
                        vec3(0.0f, 0.78f, -1.35f), vec3(1.38f, 0.62f, 1.85f), 0.0f,
                        vec4(0.95f, 0.90f, 0.82f, 1.0f), sd, fg, fd, FALSE, &tris);
    emit_boat_part_draw(st, 2u, &st->cube_mesh, st->pipe_lit_back, vp, bm,
                        vec3(0.0f, 1.13f, -1.35f), vec3(1.52f, 0.12f, 2.02f), 0.0f,
                        vec4(0.75f, 0.45f, 0.28f, 1.0f), sd, fg, fd, FALSE, &tris);
    emit_boat_part_draw(st, 3u, &st->cube_mesh, st->pipe_unlit, vp, bm,
                        vec3(0.0f, 0.84f, -1.35f), vec3(1.42f, 0.22f, 0.85f), 0.0f,
                        vec4(1.0f, 0.78f, 0.35f, 1.0f), sd, fg, fd, TRUE, &tris);
    emit_boat_part_draw(st, 4u, &st->cube_mesh, st->pipe_lit_back, vp, bm,
                        vec3(0.0f, 3.15f, -0.25f), vec3(0.14f, 5.60f, 0.14f), 0.0f,
                        vec4(0.88f, 0.74f, 0.55f, 1.0f), sd, fg, fd, FALSE, &tris);
    {
        /* Bumba direk etrafında boom_angle kadar döner */
        Mat4 boom_pivot = mat4_mul(bm, mat4_mul(mat4_translate(vec3(0.0f, 0.0f, -0.25f)),
                                   mat4_mul(mat4_rotate_y(p->boom_angle), mat4_translate(vec3(0.0f, 0.0f, 0.25f)))));
        emit_boat_part_draw(st, 5u, &st->cube_mesh, st->pipe_lit_back, vp, boom_pivot,
                            vec3(0.0f, 1.32f, -1.72f), vec3(0.10f, 0.10f, 2.95f), 0.0f,
                            vec4(0.85f, 0.70f, 0.52f, 1.0f), sd, fg, fd, FALSE, &tris);
    }
    emit_boat_part_draw(st, 6u, &st->cube_mesh, st->pipe_lit_back, vp, bm,
                        vec3(0.0f, 1.02f, 4.15f), vec3(0.10f, 0.10f, 1.75f), -0.18f,
                        vec4(0.85f, 0.70f, 0.52f, 1.0f), sd, fg, fd, FALSE, &tris);
    emit_boat_part_draw(st, 7u, &st->cube_mesh, st->pipe_unlit, vp, bm,
                        vec3(0.12f, 5.88f, -0.55f), vec3(0.04f, 0.16f, 0.65f), 0.0f,
                        vec4(0.92f, 0.22f, 0.16f, 1.0f), sd, fg, fd, TRUE, &tris);
    emit_boat_part_draw(st, 8u, &st->main_sail_mesh, st->pipe_lit_none, vp, bm,
                        vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                        vec4(1.0f, 0.97f, 0.92f, 1.0f), sd, fg, fd, FALSE, &tris);
    emit_boat_part_draw(st, 9u, &st->jib_sail_mesh, st->pipe_lit_none, vp, bm,
                        vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 1.0f, 1.0f), 0.0f,
                        vec4(0.98f, 0.94f, 0.88f, 1.0f), sd, fg, fd, FALSE, &tris);

    /* (a2) Ek 3D dünya nesneleri (adalar, iskele, deniz fenerleri, şamandıralar, hayalet tekne) */
    u32 n_extra = p->extra_count;
    if (n_extra > GPU_OCEAN_MAX_EXTRAS) n_extra = GPU_OCEAN_MAX_EXTRAS;
    for (u32 i = 0u; i < n_extra; i++) {
        const GpuOceanExtraDraw* ed = &p->extras[i];
        const GpuMeshRes* mesh = ed->use_hull_mesh ? &st->hull_mesh : &st->cube_mesh;

        GpuVsUniforms vsu;
        vsu.mvp        = mat4_mul(vp, ed->model);
        vsu.model      = ed->model;
        vsu.normal_mat = gpu_mat4_transpose(mat4_inverse(ed->model));
        vsu.shadow_mat = mat4_identity();
        rhi_write_buffer(rhi, st->extra_vs_ub[i], 0u, &vsu, sizeof(vsu));

        if (ed->is_unlit) {
            GpuUnlitFsUniforms fsu;
            fsu.color = ed->color;
            fsu.fog   = vec4(fg.x, fg.y, fg.z, fd);
            rhi_write_buffer(rhi, st->extra_unlit_fs_ub[i], 0u, &fsu, sizeof(fsu));
            rhi_set_pipeline(rhi, st->pipe_unlit);
            rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->extra_unlit_fs_bg[i]);
            rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->extra_unlit_vs_bg[i]);
        } else {
            GpuLitFsUniforms fsu;
            fsu.light_dir = vec4(sd.x, sd.y, sd.z, 0.0f);
            fsu.base      = ed->color;
            fsu.fog       = vec4(fg.x, fg.y, fg.z, fd);
            rhi_write_buffer(rhi, st->extra_lit_fs_ub[i], 0u, &fsu, sizeof(fsu));
            rhi_set_pipeline(rhi, st->pipe_lit_back);
            rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->extra_lit_fs_bg[i]);
            rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->extra_lit_vs_bg[i]);
        }

        rhi_set_vertex_buffer(rhi, 0u, mesh->vb, 0u, 0u);
        rhi_set_index_buffer(rhi, mesh->ib, RHI_INDEX_UINT32, 0u, 0u);
        rhi_draw_indexed(rhi, mesh->index_count, 1u, 0u, 0, 0u);
        tris += mesh->index_count / 3u;
    }

    /* (b) Kamera merkezli geometrik okyanus yüzeyi */
    if (p->wireframe != 2u) {
        rhi_set_pipeline(rhi, st->pipe_ocean);
        rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->ocean_fs_bg[ping]);
        rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->ocean_vs_bg);
        rhi_set_vertex_buffer(rhi, 0u, st->ocean_grid.vb, 0u, 0u);
        rhi_set_index_buffer(rhi, st->ocean_grid.ib, RHI_INDEX_UINT32, 0u, 0u);
        rhi_draw_indexed(rhi, st->ocean_grid.index_count, 1u, 0u, 0, 0u);
        tris += st->ocean_grid.index_count / 3u;
    }

    /* (c) Atmosferik gök kubbesi (early-Z ile yalnız açık gökyüzü pikselleri) */
    rhi_set_pipeline(rhi, st->pipe_sky);
    rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->sky_fs_bg[ping]);
    rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->sky_vs_bg);
    rhi_set_vertex_buffer(rhi, 0u, st->sky_mesh.vb, 0u, 0u);
    rhi_set_index_buffer(rhi, st->sky_mesh.ib, RHI_INDEX_UINT32, 0u, 0u);
    rhi_draw_indexed(rhi, st->sky_mesh.index_count, 1u, 0u, 0, 0u);
    tris += st->sky_mesh.index_count / 3u;

    rhi_end_pass(rhi);
    rhi_present(rhi);

    st->cloud_ping = ping ^ 1u;
    st->frame_idx++;

    if (out_tris) *out_tris = tris;
    return !rhi->overflow && rhi->dropped_cmds == 0u;
}

/* ---------- Birim Testi Yardımcı Fonksiyonu (Matoki @compute FFT Doğrulaması) ---------- */

b32 gpu_ocean_test_compute_fft(Arena* scratch, const Ocean* ref_ocean, u32 cascade_idx,
                               f64 sim_time, f32* out_max_err) {
    if (!scratch || !ref_ocean || cascade_idx >= OCEAN_CASCADES) return FALSE;
    usize mark = arena_mark(scratch);

    IrFunc ir_spec, ir_stage, ir_unp;
    MtkLayout lay;
    MtkDiag diag;
    __builtin_memset(&ir_spec, 0, sizeof(ir_spec));
    __builtin_memset(&ir_stage, 0, sizeof(ir_stage));
    __builtin_memset(&ir_unp, 0, sizeof(ir_unp));
    __builtin_memset(&lay, 0, sizeof(lay));
    mtk_diag_init(&diag);

    if (!mtk_compile_ir(k_mtk_ocean_spectrum, (u32)(sizeof(k_mtk_ocean_spectrum) - 1u),
                        "ocean_spectrum.mtk", scratch, &ir_spec, &lay, &diag)) {
        LOG("ocean_spectrum.mtk hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_ocean_fft_stage, (u32)(sizeof(k_mtk_ocean_fft_stage) - 1u),
                        "ocean_fft_stage.mtk", scratch, &ir_stage, &lay, &diag)) {
        LOG("ocean_fft_stage.mtk hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_ocean_unpack, (u32)(sizeof(k_mtk_ocean_unpack) - 1u),
                        "ocean_unpack.mtk", scratch, &ir_unp, &lay, &diag)) {
        LOG("ocean_unpack.mtk hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    ir_optimize(&ir_spec);
    ir_optimize(&ir_stage);
    ir_optimize(&ir_unp);

    /* WGSL derlemesini de doğrula */
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 32768u);
    if (mtk_compile_wgsl(k_mtk_ocean_spectrum, (u32)(sizeof(k_mtk_ocean_spectrum) - 1u),
                         "ocean_spectrum.mtk", NULL, scratch, wgsl_buf, 32768u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_ocean_fft_stage, (u32)(sizeof(k_mtk_ocean_fft_stage) - 1u),
                         "ocean_fft_stage.mtk", NULL, scratch, wgsl_buf, 32768u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_ocean_unpack, (u32)(sizeof(k_mtk_ocean_unpack) - 1u),
                         "ocean_unpack.mtk", NULL, scratch, wgsl_buf, 32768u, &lay, &diag) == 0u) {
        LOG("ocean compute wgsl hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    const OceanCascade* oc = &ref_ocean->cascades[cascade_idx];
    u32 n  = oc->n;
    u32 nn = n * n;

    Vec4* h0_pack = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* ki_pack = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* a01     = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* a23     = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* b01     = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* b23     = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    f32*  foam    = ARENA_PUSH_ARRAY(scratch, f32,  nn);
    Vec4* disp    = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    Vec4* casc    = ARENA_PUSH_ARRAY(scratch, Vec4, nn);
    __builtin_memset(foam, 0, nn * sizeof(f32));

    for (u32 j = 0u; j < n; j++) {
        u32 br_j = oc->bitrev[j];
        for (u32 i = 0u; i < n; i++) {
            u32 br_i = oc->bitrev[i];
            u32 src  = j * n + i;
            u32 dst  = br_j * n + br_i;
            h0_pack[dst] = vec4(oc->h0r[src], oc->h0i[src], oc->h0mr[src], oc->h0mi[src]);
            f32 kx = oc->kx[src], kz = oc->kz[src];
            f32 k2 = kx * kx + kz * kz;
            f32 kl = (k2 > 1e-12f) ? m_sqrt(k2) : 0.0f;
            f32 ik = (kl > 1e-12f) ? (1.0f / kl) : 0.0f;
            ki_pack[dst] = vec4(kx, kz, oc->omega[src], ik);
        }
    }

    Vec4 spec_u = vec4((f32)sim_time, (f32)nn, 0.0f, 0.0f);
    MtkComputeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.start        = 0u;
    batch.count        = nn;
    batch.total_count  = nn;
    batch.buffer_count = 4u;
    batch.uniforms     = (const u8*)&spec_u;
    batch.buffers[0]   = h0_pack;
    batch.buffers[1]   = ki_pack;
    batch.buffers[2]   = a01;
    batch.buffers[3]   = a23;
    ir_eval_compute(&ir_spec, &batch);

    u32 num_stages = oc->log2n * 2u;
    for (u32 s = 0u; s < num_stages; s++) {
        u32 axis = (s < oc->log2n) ? 0u : 1u;
        u32 pass = (s < oc->log2n) ? s : (s - oc->log2n);
        u32 half = 1u << pass;
        _Alignas(16) i32 su[4] = { (i32)n, (i32)half, (i32)axis, (i32)nn };
        Vec4* s01 = ((s & 1u) == 0u) ? a01 : b01;
        Vec4* s23 = ((s & 1u) == 0u) ? a23 : b23;
        Vec4* d01 = ((s & 1u) == 0u) ? b01 : a01;
        Vec4* d23 = ((s & 1u) == 0u) ? b23 : a23;
        batch.buffer_count = 4u;
        batch.uniforms     = (const u8*)su;
        batch.buffers[0]   = s01;
        batch.buffers[1]   = s23;
        batch.buffers[2]   = d01;
        batch.buffers[3]   = d23;
        ir_eval_compute(&ir_stage, &batch);
    }

    f32 lam      = ref_ocean->d.choppy;
    f32 j_thr    = ref_ocean->d.foam_threshold;
    f32 f_gain   = ref_ocean->d.foam_scale;
    f32 s_enc_nm = (127.0f / 255.0f) / OCEAN_TEX_SLOPE_MAX;
    f32 h_norm   = 1.0f / m_max(ref_ocean->hs, 0.5f);
    Vec4 unp_u[2] = {
        vec4(lam, j_thr, f_gain, 0.0f),
        vec4(1.0f, s_enc_nm, h_norm, (f32)nn)
    };
    batch.buffer_count = 5u;
    batch.uniforms     = (const u8*)unp_u;
    batch.buffers[0]   = a01;
    batch.buffers[1]   = a23;
    batch.buffers[2]   = foam;
    batch.buffers[3]   = disp;
    batch.buffers[4]   = casc;
    ir_eval_compute(&ir_unp, &batch);

    f32 max_err = 0.0f;
    for (u32 i = 0u; i < nn; i++) {
        f32 eh  = m_abs(disp[i].y - oc->height[i]);
        f32 edx = m_abs(disp[i].x - lam * oc->disp_x[i]);
        f32 edz = m_abs(disp[i].z - lam * oc->disp_z[i]);
        if (eh  > max_err) max_err = eh;
        if (edx > max_err) max_err = edx;
        if (edz > max_err) max_err = edz;
    }

    if (out_max_err) *out_max_err = max_err;
    arena_pop_to(scratch, mark);
    return max_err < 5e-3f;
}

/* ---------- V1 Atmosfer LUT ve 3B Bulut Gürültüsü @compute Birim Testi ---------- */

b32 gpu_ocean_test_atmos_compute(Arena* scratch) {
    if (!scratch) return FALSE;
    usize mark = arena_mark(scratch);

    IrFunc ir_tr, ir_ms, ir_sky, ir_aer, ir_ns;
    MtkLayout lay;
    MtkDiag diag;
    __builtin_memset(&ir_tr, 0, sizeof(ir_tr));
    __builtin_memset(&ir_ms, 0, sizeof(ir_ms));
    __builtin_memset(&ir_sky, 0, sizeof(ir_sky));
    __builtin_memset(&ir_aer, 0, sizeof(ir_aer));
    __builtin_memset(&ir_ns, 0, sizeof(ir_ns));
    __builtin_memset(&lay, 0, sizeof(lay));
    mtk_diag_init(&diag);

    if (!mtk_compile_ir(k_mtk_atmos_transmittance, (u32)(sizeof(k_mtk_atmos_transmittance) - 1u),
                        "atmos_transmittance.mtk", scratch, &ir_tr, &lay, &diag)) {
        LOG("atmos_transmittance.mtk IR hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_atmos_multiscatter, (u32)(sizeof(k_mtk_atmos_multiscatter) - 1u),
                        "atmos_multiscatter.mtk", scratch, &ir_ms, &lay, &diag)) {
        LOG("atmos_multiscatter.mtk IR hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_atmos_skyview, (u32)(sizeof(k_mtk_atmos_skyview) - 1u),
                        "atmos_skyview.mtk", scratch, &ir_sky, &lay, &diag)) {
        LOG("atmos_skyview.mtk IR hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_atmos_aerial, (u32)(sizeof(k_mtk_atmos_aerial) - 1u),
                        "atmos_aerial.mtk", scratch, &ir_aer, &lay, &diag)) {
        LOG("atmos_aerial.mtk IR hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }
    if (!mtk_compile_ir(k_mtk_cloud_noise, (u32)(sizeof(k_mtk_cloud_noise) - 1u),
                        "cloud_noise.mtk", scratch, &ir_ns, &lay, &diag)) {
        LOG("cloud_noise.mtk IR hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    ir_optimize(&ir_tr);
    ir_optimize(&ir_ms);
    ir_optimize(&ir_sky);
    ir_optimize(&ir_aer);
    ir_optimize(&ir_ns);

    /* Tüm 5 @compute çekirdeğinin WGSL derlemesini doğrula */
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 131072u);
    if (mtk_compile_wgsl(k_mtk_atmos_transmittance, (u32)(sizeof(k_mtk_atmos_transmittance) - 1u),
                         "atmos_transmittance.mtk", NULL, scratch, wgsl_buf, 131072u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_atmos_multiscatter, (u32)(sizeof(k_mtk_atmos_multiscatter) - 1u),
                         "atmos_multiscatter.mtk", NULL, scratch, wgsl_buf, 131072u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_atmos_skyview, (u32)(sizeof(k_mtk_atmos_skyview) - 1u),
                         "atmos_skyview.mtk", NULL, scratch, wgsl_buf, 131072u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_atmos_aerial, (u32)(sizeof(k_mtk_atmos_aerial) - 1u),
                         "atmos_aerial.mtk", NULL, scratch, wgsl_buf, 131072u, &lay, &diag) == 0u ||
        mtk_compile_wgsl(k_mtk_cloud_noise, (u32)(sizeof(k_mtk_cloud_noise) - 1u),
                         "cloud_noise.mtk", NULL, scratch, wgsl_buf, 131072u, &lay, &diag) == 0u) {
        LOG("V1 atmos/cloud WGSL codegen hata: %s", diag.text);
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    /* Küçük alt-küme üzerinde CPU IR yorumlayıcısını çalıştırarak fiziksel çıktıları denetle */
    Vec4* tr_buf  = ARENA_PUSH_ARRAY(scratch, Vec4, 256u * 64u);
    Vec4* ms_buf  = ARENA_PUSH_ARRAY(scratch, Vec4, 32u * 32u);
    Vec4* sky_buf = ARENA_PUSH_ARRAY(scratch, Vec4, 16u * 16u);
    Vec4* aer_buf = ARENA_PUSH_ARRAY(scratch, Vec4, 8u * 8u * 4u);
    Vec4* ns_buf  = ARENA_PUSH_ARRAY(scratch, Vec4, 8u * 8u * 8u);
    for (u32 i = 0u; i < 256u * 64u; i++) tr_buf[i] = vec4(0.8f, 0.85f, 0.9f, 1.0f);
    for (u32 i = 0u; i < 32u * 32u;  i++) ms_buf[i] = vec4(0.05f, 0.08f, 0.12f, 1.0f);

    MtkComputeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));

    Vec4 u_tr = vec4(64.0f, 16.0f, 6360.0f, 6460.0f);
    batch.start        = 0u;
    batch.count        = 64u * 16u;
    batch.total_count  = 64u * 16u;
    batch.buffer_count = 1u;
    batch.uniforms     = (const u8*)&u_tr;
    batch.buffers[0]   = tr_buf;
    ir_eval_compute(&ir_tr, &batch);

    /* Transmittance: zenitte (sağ üst) geçirgenlik ufuktan (sol alt) yüksek olmalı */
    Vec4 tr_hor = tr_buf[0 * 64 + 8];
    Vec4 tr_zen = tr_buf[0 * 64 + 63];
    if (!(tr_zen.x > tr_hor.x && tr_zen.z > tr_hor.z && tr_zen.x <= 1.0f && tr_hor.x >= 0.0f)) {
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    /* Multi-scatter, Sky-View ve Aerial Perspective çekirdeklerini de yürüt */
    Vec4 u_ms[2] = {
        vec4(8.0f, 8.0f, 6360.0f, 6460.0f),
        vec4(64.0f, 16.0f, 0.10f, 0.0f)
    };
    batch.count        = 64u;
    batch.total_count  = 64u;
    batch.buffer_count = 2u;
    batch.uniforms     = (const u8*)u_ms;
    batch.buffers[0]   = tr_buf;
    batch.buffers[1]   = ms_buf;
    ir_eval_compute(&ir_ms, &batch);

    Vec4 u_sky[3] = {
        vec4(16.0f, 16.0f, 0.05f, 0.80f),
        vec4(0.35f, 0.75f, -0.56f, 20.0f),
        vec4(64.0f, 16.0f, 8.0f, 8.0f)
    };
    batch.count        = 256u;
    batch.total_count  = 256u;
    batch.buffer_count = 3u;
    batch.uniforms     = (const u8*)u_sky;
    batch.buffers[0]   = tr_buf;
    batch.buffers[1]   = ms_buf;
    batch.buffers[2]   = sky_buf;
    ir_eval_compute(&ir_sky, &batch);

    Vec4 u_aer[3] = {
        vec4(8.0f, 8.0f, 4.0f, 2.0f),
        vec4(0.35f, 0.75f, -0.56f, 20.0f),
        vec4(64.0f, 16.0f, 8.0f, 8.0f)
    };
    batch.count        = 256u;
    batch.total_count  = 256u;
    batch.buffer_count = 3u;
    batch.uniforms     = (const u8*)u_aer;
    batch.buffers[0]   = tr_buf;
    batch.buffers[1]   = ms_buf;
    batch.buffers[2]   = aer_buf;
    ir_eval_compute(&ir_aer, &batch);

    if (!(ms_buf[32].x > 0.0f && sky_buf[200].z > 0.0f && aer_buf[128].w > 0.0f && aer_buf[128].w <= 1.0f)) {
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    /* 3B Bulut gürültüsü: [0, 1] aralığında ve varyans > 0 olmalı */
    Vec4 u_ns = vec4(8.0f, 1.0f / 8.0f, 0.0f, 0.0f);
    batch.count        = 512u;
    batch.total_count  = 512u;
    batch.buffer_count = 1u;
    batch.uniforms     = (const u8*)&u_ns;
    batch.buffers[0]   = ns_buf;
    ir_eval_compute(&ir_ns, &batch);

    f32 ns_min = 1.0f, ns_max = 0.0f;
    for (u32 i = 0u; i < 512u; i++) {
        f32 v = ns_buf[i].x;
        if (v < 0.0f || v > 1.0f) {
            arena_pop_to(scratch, mark);
            return FALSE;
        }
        if (v < ns_min) ns_min = v;
        if (v > ns_max) ns_max = v;
    }
    if ((ns_max - ns_min) < 0.10f) {
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    arena_pop_to(scratch, mark);
    return TRUE;
}
