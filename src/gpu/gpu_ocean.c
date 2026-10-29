/* gpu/gpu_ocean.c — "Okyanus" sahnesinin tam çözünürlüklü WebGPU hesaplama + çizim hattı (G1_A).
 * 3 kademeli Tessendorf FFT'yi Matoki @compute -> WGSL çekirdekleriyle GPU'da koşturup
 * sonuçları storage dokulara yazar; kamera merkezli geometrik ızgarayı köşe gölgelendiricisinde
 * dokulardan örnekleyerek yer değiştirir ve ocean.mtk + sky.mtk + tekne ağlarını MSAA 4x ile çizer.
 */
#include "gpu/gpu_ocean.h"
#include "gpu/rhi.h"
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
} GpuOceanFsUniforms;

typedef struct {
    _Alignas(16) Vec4 sun_dir;
    _Alignas(16) Vec4 sun_col;
    _Alignas(16) Vec4 sky_zenith;
    _Alignas(16) Vec4 sky_horizon;
    _Alignas(16) Vec4 params;
} GpuSkyFsUniforms;

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
    u32                depth_w;
    u32                depth_h;
    RhiTexture         depth_tex;
    RhiSampler         lin_sampler;

    /* Compute pipeline'ları */
    RhiComputePipeline cpipe_spectrum;
    RhiComputePipeline cpipe_fft_stage;
    RhiComputePipeline cpipe_unpack;
    RhiComputePipeline cpipe_to_tex;
    GpuCascadeRes      casc[OCEAN_CASCADES];

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
    RhiBindGroup       ocean_fs_bg;

    RhiBuffer          sky_vs_ub;
    RhiBuffer          sky_fs_ub;
    RhiBindGroup       sky_vs_bg;
    RhiBindGroup       sky_fs_bg;

    RhiBuffer          boat_vs_ub[GPU_OCEAN_BOAT_PARTS];
    RhiBuffer          boat_fs_ub[GPU_OCEAN_BOAT_PARTS];
    RhiBindGroup       boat_vs_bg[GPU_OCEAN_BOAT_PARTS];
    RhiBindGroup       boat_fs_bg[GPU_OCEAN_BOAT_PARTS];
} GpuOceanState;

static u8            g_gpu_ocean_mem[MB(6)];
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
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 65536u);
    MtkLayout layout;
    MtkDiag diag;
    __builtin_memset(&layout, 0, sizeof(layout));
    mtk_diag_init(&diag);

    MtkWgslOpts opts;
    opts.frag_group = group;
    opts.entry_name = entry;

    u32 wgsl_len = mtk_compile_wgsl(src, len, name, &opts, scratch,
                                    wgsl_buf, 65536u, &layout, &diag);
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

    /* 1. Doğrusal + tekrarlı örnekleyici */
    RhiSamplerDesc sdesc = {
        .address_u     = RHI_ADDR_REPEAT,
        .address_v     = RHI_ADDR_REPEAT,
        .mag_filter    = RHI_FILTER_LINEAR,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_LINEAR
    };
    st->lin_sampler = rhi_create_sampler(rhi, &sdesc);

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

    /* 4. Render shader'larının derlenmesi (ocean.mtk, sky.mtk, lit3d.mtk, unlit3d.mtk) */
    RhiShader vs_ocean = rhi_create_shader(
        rhi, k_wgsl_ocean_vs, (u32)(sizeof(k_wgsl_ocean_vs) - 1u));
    RhiShader fs_ocean = compile_mtk_shader_to_rhi(
        rhi, scratch, p->ocean_mtk_src, p->ocean_mtk_len, "ocean.mtk", MTK_WGSL_FRAG_GROUP, "frag");

    static const u8 k_sky_attrs[1] = { MTK_WGSL_ATTR_NORMAL };
    RhiShader vs_sky = compile_vs_attrs_to_rhi(rhi, scratch, k_sky_attrs, 1u);
    RhiShader fs_sky = compile_mtk_shader_to_rhi(
        rhi, scratch, p->sky_mtk_src, p->sky_mtk_len, "sky.mtk", MTK_WGSL_FRAG_GROUP, "frag");

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

    RhiBindGroupDesc ofs_bgd = {
        .pipeline    = st->pipe_ocean,
        .group_index = MTK_WGSL_FRAG_GROUP,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->ocean_fs_ub, .size = (u32)sizeof(GpuOceanFsUniforms) },
            { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[0].casc_tex },
            { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler },
            { .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[1].casc_tex },
            { .binding = 4u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler },
            { .binding = 5u, .kind = RHI_BIND_TEXTURE, .resource = st->casc[2].casc_tex },
            { .binding = 6u, .kind = RHI_BIND_SAMPLER, .resource = st->lin_sampler }
        },
        .entry_count = 7u
    };
    st->ocean_fs_bg = rhi_create_bind_group(rhi, &ofs_bgd);

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

    RhiBindGroupDesc sfs_bgd = {
        .pipeline    = st->pipe_sky,
        .group_index = MTK_WGSL_FRAG_GROUP,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->sky_fs_ub, .size = (u32)sizeof(GpuSkyFsUniforms) }
        },
        .entry_count = 1u
    };
    st->sky_fs_bg = rhi_create_bind_group(rhi, &sfs_bgd);

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
    sfsu.params      = vec4((f32)p->sim_time, 0.42f, 1.0f, 0.35f + 0.65f * p->warm);
    rhi_write_buffer(rhi, st->sky_fs_ub, 0u, &sfsu, sizeof(sfsu));

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
    emit_boat_part_draw(st, 5u, &st->cube_mesh, st->pipe_lit_back, vp, bm,
                        vec3(0.0f, 1.32f, -1.72f), vec3(0.10f, 0.10f, 2.95f), 0.0f,
                        vec4(0.85f, 0.70f, 0.52f, 1.0f), sd, fg, fd, FALSE, &tris);
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

    /* (b) Kamera merkezli geometrik okyanus yüzeyi */
    if (p->wireframe != 2u) {
        rhi_set_pipeline(rhi, st->pipe_ocean);
        rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->ocean_fs_bg);
        rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->ocean_vs_bg);
        rhi_set_vertex_buffer(rhi, 0u, st->ocean_grid.vb, 0u, 0u);
        rhi_set_index_buffer(rhi, st->ocean_grid.ib, RHI_INDEX_UINT32, 0u, 0u);
        rhi_draw_indexed(rhi, st->ocean_grid.index_count, 1u, 0u, 0, 0u);
        tris += st->ocean_grid.index_count / 3u;
    }

    /* (c) Atmosferik gök kubbesi (early-Z ile yalnız açık gökyüzü pikselleri) */
    rhi_set_pipeline(rhi, st->pipe_sky);
    rhi_set_bind_group(rhi, MTK_WGSL_FRAG_GROUP, st->sky_fs_bg);
    rhi_set_bind_group(rhi, MTK_WGSL_VERT_GROUP, st->sky_vs_bg);
    rhi_set_vertex_buffer(rhi, 0u, st->sky_mesh.vb, 0u, 0u);
    rhi_set_index_buffer(rhi, st->sky_mesh.ib, RHI_INDEX_UINT32, 0u, 0u);
    rhi_draw_indexed(rhi, st->sky_mesh.index_count, 1u, 0u, 0, 0u);
    tris += st->sky_mesh.index_count / 3u;

    rhi_end_pass(rhi);
    rhi_present(rhi);

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
