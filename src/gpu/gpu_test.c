/* gpu/gpu_test.c — Bağımsız WebGPU RHI demosu: dönen renkli üçgen + dokulu, derinlik testli 3D küp.
 * main.c'ye dokunmadan engine_gpu_test_frame() dışa aktarımını sağlar (M15_B, D-108).
 * İlk karede WGSL shader'larını, buffer/doku/sampler/pipeline/bind_group kaynaklarını oluşturur;
 * her karede dönüş matrislerini güncelleyip tek geçişte hem üçgeni hem küpü çizer.
 */
#include "gpu/rhi.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"
#include "core/embed.h"
#include "matoki/matoki.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/wgsl.h"
#include "gfx3d/mesh.h"
#include "gfx3d/pipeline.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "raster/shade_abi.h"
#include "plat/platform.h"

#define GPU_TEST_W 640u
#define GPU_TEST_H 360u

/* Elle yazılmış WGSL — 1) Dönen renkli üçgen */
static const char k_tri_wgsl[] =
    "struct TriUniforms {\n"
    "    mvp: mat4x4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u_tri: TriUniforms;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) col: vec3<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) pos: vec2<f32>, @location(1) col: vec3<f32>) -> VsOut {\n"
    "    var out: VsOut;\n"
    "    out.pos = u_tri.mvp * vec4<f32>(pos, 0.5, 1.0);\n"
    "    out.col = col;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    return vec4<f32>(in.col, 1.0);\n"
    "}\n";

/* Elle yazılmış WGSL — 2) Dokulu, yönlü ışıklı, derinlik testli 3D küp */
static const char k_cube_wgsl[] =
    "struct CubeUniforms {\n"
    "    mvp: mat4x4<f32>,\n"
    "    model: mat4x4<f32>,\n"
    "    light_dir: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u_cube: CubeUniforms;\n"
    "@group(0) @binding(1) var t_color: texture_2d<f32>;\n"
    "@group(0) @binding(2) var s_color: sampler;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) world_n: vec3<f32>,\n"
    "    @location(1) uv: vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(\n"
    "    @location(0) pos: vec3<f32>,\n"
    "    @location(1) norm: vec3<f32>,\n"
    "    @location(2) uv: vec2<f32>\n"
    ") -> VsOut {\n"
    "    var out: VsOut;\n"
    "    out.pos = u_cube.mvp * vec4<f32>(pos, 1.0);\n"
    "    out.world_n = (u_cube.model * vec4<f32>(norm, 0.0)).xyz;\n"
    "    out.uv = uv;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    let tex = textureSample(t_color, s_color, in.uv);\n"
    "    let n = normalize(in.world_n);\n"
    "    let l = normalize(u_cube.light_dir.xyz);\n"
    "    let ndotl = max(dot(n, l), 0.0);\n"
    "    let lit = tex.rgb * (0.32 + 0.68 * ndotl);\n"
    "    return vec4<f32>(lit, 1.0);\n"
    "}\n";

typedef struct {
    f32 x, y;
    f32 r, g, b;
} TriVertex;

typedef struct {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
} CubeVertex;

typedef struct {
    Mat4 mvp;
    Mat4 model;
    Vec4 light_dir;
} CubeUniforms;

typedef struct {
    Arena        arena;
    Rhi          rhi;
    b32          inited;
    u32          frame_idx;
    RhiTexture   depth_tex;
    /* Üçgen kaynakları */
    RhiShader    tri_sh;
    RhiBuffer    tri_vb;
    RhiBuffer    tri_ib;
    RhiBuffer    tri_ub;
    RhiPipeline  tri_pipe;
    RhiBindGroup tri_bg;
    /* Küp kaynakları */
    RhiShader    cube_sh;
    RhiBuffer    cube_vb;
    RhiBuffer    cube_ib;
    RhiBuffer    cube_ub;
    RhiTexture   cube_tex;
    RhiSampler   cube_samp;
    RhiPipeline  cube_pipe;
    RhiBindGroup cube_bg;
    u32          cube_index_count;
    /* Hata ve piksel geri okuma özeti */
    u32            total_gpu_errors;
    GpuTestSummary summary;
} GpuTestState;

#define GPU_TEST_READBACK_BYTES (GPU_TEST_W * GPU_TEST_H * 4u)

static u8           g_gpu_test_mem[KB(512)] __attribute__((aligned(16)));
static u8           g_gpu_test_readback[GPU_TEST_READBACK_BYTES] __attribute__((aligned(16)));
static GpuTestState g_gpu_test;

static void gpu_test_setup_resources(GpuTestState* st) {
    Rhi* rhi = &st->rhi;

    /* 1. Derinlik dokusu (640x360, DEPTH24_PLUS) */
    RhiTextureDesc depth_desc = {
        .width      = GPU_TEST_W,
        .height     = GPU_TEST_H,
        .mip_levels = 1u,
        .format     = RHI_FMT_DEPTH24_PLUS,
        .usage      = RHI_TEX_RENDER_ATTACHMENT
    };
    st->depth_tex = rhi_create_texture(rhi, &depth_desc);

    /* 2. Dönen renkli üçgen kaynakları */
    st->tri_sh = rhi_create_shader(rhi, k_tri_wgsl, (u32)(sizeof(k_tri_wgsl) - 1u));

    static const TriVertex k_tri_verts[3] = {
        {  0.00f,  0.52f, 1.00f, 0.25f, 0.30f },
        { -0.45f, -0.38f, 0.20f, 0.95f, 0.45f },
        {  0.45f, -0.38f, 0.25f, 0.55f, 1.00f }
    };
    static const u16 k_tri_indices[4] = { 0u, 1u, 2u, 0u };

    st->tri_vb = rhi_create_buffer(rhi, (u32)sizeof(k_tri_verts), RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    st->tri_ib = rhi_create_buffer(rhi, (u32)sizeof(k_tri_indices), RHI_BUF_INDEX | RHI_BUF_COPY_DST);
    st->tri_ub = rhi_create_buffer(rhi, (u32)sizeof(Mat4), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    rhi_write_buffer(rhi, st->tri_vb, 0u, k_tri_verts, (u32)sizeof(k_tri_verts));
    rhi_write_buffer(rhi, st->tri_ib, 0u, k_tri_indices, (u32)sizeof(k_tri_indices));

    RhiPipelineDesc tri_pdesc = {
        .vertex_shader   = st->tri_sh,
        .fragment_shader = st->tri_sh,
        .vs_entry        = "vs_main",
        .fs_entry        = "fs_main",
        .vertex_stride   = (u32)sizeof(TriVertex),
        .attrs = {
            { .shader_location = 0u, .offset = 0u, .format = RHI_VFMT_FLOAT32X2 },
            { .shader_location = 1u, .offset = 8u, .format = RHI_VFMT_FLOAT32X3 }
        },
        .attr_count    = 2u,
        .topology      = RHI_TOPOLOGY_TRIANGLE_LIST,
        .cull_mode     = RHI_CULL_NONE,
        .front_face    = RHI_FRONT_CCW,
        .color_format  = RHI_FMT_SURFACE_DEFAULT,
        .blend_mode    = RHI_BLEND_NONE,
        .depth_format  = RHI_FMT_DEPTH24_PLUS,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_LESS
    };
    st->tri_pipe = rhi_create_pipeline(rhi, &tri_pdesc);

    RhiBindGroupDesc tri_bgdesc = {
        .pipeline    = st->tri_pipe,
        .group_index = 0u,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = st->tri_ub, .offset = 0u, .size = (u32)sizeof(Mat4) }
        },
        .entry_count = 1u
    };
    st->tri_bg = rhi_create_bind_group(rhi, &tri_bgdesc);

    /* 3. Dokulu, derinlik testli 3D küp kaynakları */
    st->cube_sh = rhi_create_shader(rhi, k_cube_wgsl, (u32)(sizeof(k_cube_wgsl) - 1u));

    usize mark = arena_mark(&st->arena);
    Mesh* cube = mesh_cube(&st->arena, 0.85f);
    CubeVertex* cverts = ARENA_PUSH_ARRAY(&st->arena, CubeVertex, cube->vertex_count);
    for (u32 i = 0u; i < cube->vertex_count; i++) {
        cverts[i].px = cube->px[i];
        cverts[i].py = cube->py[i];
        cverts[i].pz = cube->pz[i];
        cverts[i].nx = cube->nx[i];
        cverts[i].ny = cube->ny[i];
        cverts[i].nz = cube->nz[i];
        cverts[i].u  = cube->u[i];
        cverts[i].v  = cube->v[i];
    }
    u32 vb_bytes = cube->vertex_count * (u32)sizeof(CubeVertex);
    u32 ib_bytes = cube->index_count * (u32)sizeof(u32);
    st->cube_index_count = cube->index_count;

    st->cube_vb = rhi_create_buffer(rhi, vb_bytes, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    st->cube_ib = rhi_create_buffer(rhi, ib_bytes, RHI_BUF_INDEX | RHI_BUF_COPY_DST);
    st->cube_ub = rhi_create_buffer(rhi, (u32)sizeof(CubeUniforms), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);

    rhi_write_buffer(rhi, st->cube_vb, 0u, cverts, vb_bytes);
    rhi_write_buffer(rhi, st->cube_ib, 0u, cube->indices, ib_bytes);

    /* 64x64 prosedürel dama dokusu (satır adımı 256 bayt -> WebGPU hizalamasına tam uyumlu) */
    Texture proc_tex;
    tex_init(&proc_tex, &st->arena, 64u, 64u, 0u);
    tex_gen_checker(&proc_tex, 8u, FB_RGBA(245, 165, 45, 255), FB_RGBA(45, 185, 235, 255));

    RhiTextureDesc tex_desc = {
        .width      = 64u,
        .height     = 64u,
        .mip_levels = 1u,
        .format     = RHI_FMT_RGBA8_UNORM,
        .usage      = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    st->cube_tex = rhi_create_texture(rhi, &tex_desc);
    rhi_write_texture(rhi, st->cube_tex, 0u, 0u, 0u, 64u, 64u,
                      64u * 4u, proc_tex.px, 64u * 64u * 4u);
    arena_pop_to(&st->arena, mark);

    RhiSamplerDesc samp_desc = {
        .address_u     = RHI_ADDR_REPEAT,
        .address_v     = RHI_ADDR_REPEAT,
        .mag_filter    = RHI_FILTER_NEAREST,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_NEAREST
    };
    st->cube_samp = rhi_create_sampler(rhi, &samp_desc);

    RhiPipelineDesc cube_pdesc = {
        .vertex_shader   = st->cube_sh,
        .fragment_shader = st->cube_sh,
        .vs_entry        = "vs_main",
        .fs_entry        = "fs_main",
        .vertex_stride   = (u32)sizeof(CubeVertex),
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
        .depth_format  = RHI_FMT_DEPTH24_PLUS,
        .depth_test    = TRUE,
        .depth_write   = TRUE,
        .depth_compare = RHI_CMP_LESS
    };
    st->cube_pipe = rhi_create_pipeline(rhi, &cube_pdesc);

    RhiBindGroupDesc cube_bgdesc = {
        .pipeline    = st->cube_pipe,
        .group_index = 0u,
        .entries = {
            { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = st->cube_ub,   .offset = 0u, .size = (u32)sizeof(CubeUniforms) },
            { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = st->cube_tex,  .offset = 0u, .size = 0u },
            { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = st->cube_samp, .offset = 0u, .size = 0u }
        },
        .entry_count = 3u
    };
    st->cube_bg = rhi_create_bind_group(rhi, &cube_bgdesc);
}

EXPORT(engine_gpu_test_frame) u32 engine_gpu_test_frame(f64 now_ms, f32 dt) {
    UNUSED(dt);
    GpuTestState* st = &g_gpu_test;
    Rhi* rhi = &st->rhi;

    if (!st->inited) {
        arena_init(&st->arena, g_gpu_test_mem, sizeof(g_gpu_test_mem));
        rhi_init(rhi, &st->arena, 64u, KB(128));
        rhi_set_active(rhi);
        gpu_test_setup_resources(st);
        st->inited           = TRUE;
        st->frame_idx        = 0u;
        st->total_gpu_errors = 0u;
    } else {
        rhi_set_active(rhi);
        rhi_begin_frame(rhi);
    }

    st->total_gpu_errors += rhi_poll_errors(rhi);

    f32 t = (f32)(now_ms * 0.001);
    f32 aspect = (f32)GPU_TEST_W / (f32)GPU_TEST_H;

    /* Sol yarıda dönen renkli üçgen dönüşümü */
    Mat4 tri_rot   = mat4_trs(vec3(-0.48f, 0.0f, 0.0f), 0.0f, 0.0f, vec3(0.75f / aspect, 0.75f, 1.0f));
    f32  c = m_cos(t * 1.4f);
    f32  s = m_sin(t * 1.4f);
    Mat4 zrot = mat4_identity();
    zrot.m[0] =  c; zrot.m[4] = -s;
    zrot.m[1] =  s; zrot.m[5] =  c;
    Mat4 tri_mvp = mat4_mul(tri_rot, zrot);
    rhi_write_buffer(rhi, st->tri_ub, 0u, &tri_mvp, (u32)sizeof(Mat4));

    /* Sağ yarıda dönen dokulu 3D küp dönüşümü */
    Mat4 proj  = mat4_perspective(50.0f * M_DEG2RAD, aspect, 0.2f, 20.0f);
    Mat4 view  = mat4_look_at(vec3(0.0f, 0.6f, 3.6f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    Mat4 model = mat4_trs(vec3(0.95f, -0.05f, 0.0f), t * 1.1f, t * 0.65f, vec3(1.0f, 1.0f, 1.0f));
    CubeUniforms cu;
    cu.mvp       = mat4_mul(mat4_mul(proj, view), model);
    cu.model     = model;
    cu.light_dir = vec4(0.55f, 0.75f, 0.65f, 0.0f);
    rhi_write_buffer(rhi, st->cube_ub, 0u, &cu, (u32)sizeof(CubeUniforms));

    /* Render geçişi: arka plan temizle + derinlik temizle + üçgen çiz + küp çiz */
    RhiPassDesc pass = {
        .color_texture   = RHI_INVALID_HANDLE, /* 0 = tuval yüzeyi */
        .depth_texture   = st->depth_tex,
        .clear_color     = TRUE,
        .clear_r         = 0.07f,
        .clear_g         = 0.08f,
        .clear_b         = 0.12f,
        .clear_a         = 1.00f,
        .clear_depth     = TRUE,
        .clear_depth_val = 1.00f
    };
    rhi_begin_pass(rhi, &pass);

    /* 1) Renkli üçgen */
    rhi_set_pipeline(rhi, st->tri_pipe);
    rhi_set_bind_group(rhi, 0u, st->tri_bg);
    rhi_set_vertex_buffer(rhi, 0u, st->tri_vb, 0u, 0u);
    rhi_set_index_buffer(rhi, st->tri_ib, RHI_INDEX_UINT16, 0u, 0u);
    rhi_draw_indexed(rhi, 3u, 1u, 0u, 0, 0u);

    /* 2) Dokulu, derinlik testli 3D küp */
    rhi_set_pipeline(rhi, st->cube_pipe);
    rhi_set_bind_group(rhi, 0u, st->cube_bg);
    rhi_set_vertex_buffer(rhi, 0u, st->cube_vb, 0u, 0u);
    rhi_set_index_buffer(rhi, st->cube_ib, RHI_INDEX_UINT32, 0u, 0u);
    rhi_draw_indexed(rhi, st->cube_index_count, 1u, 0u, 0, 0u);

    rhi_end_pass(rhi);
    rhi_present(rhi);

    st->frame_idx++;
    return rhi_stream_bytes(rhi);
}

EXPORT(engine_gpu_test_readback_buf) u8* engine_gpu_test_readback_buf(void) {
    return g_gpu_test_readback;
}

EXPORT(engine_gpu_test_readback_bytes) u32 engine_gpu_test_readback_bytes(void) {
    return GPU_TEST_READBACK_BYTES;
}

EXPORT(engine_gpu_test_analyze_readback) b32 engine_gpu_test_analyze_readback(void) {
    GpuTestState* st = &g_gpu_test;
    Rhi* rhi = &st->rhi;
    if (st->inited) {
        st->total_gpu_errors += rhi_poll_errors(rhi);
    }

    GpuTestSummary sum;
    __builtin_memset(&sum, 0, sizeof(sum));
    sum.width      = GPU_TEST_W;
    sum.height     = GPU_TEST_H;
    sum.gpu_errors = st->total_gpu_errors + rhi->errors.dropped;

    for (u32 y = 0u; y < GPU_TEST_H; y++) {
        const u8* row = g_gpu_test_readback + (usize)y * GPU_TEST_W * 4u;
        for (u32 x = 0u; x < GPU_TEST_W; x++) {
            u32 r = row[x * 4u + 0u];
            u32 g = row[x * 4u + 1u];
            u32 b = row[x * 4u + 2u];
            u32 a = row[x * 4u + 3u];

            /* Arka plan temizleme rengi: (0.07, 0.08, 0.12, 1.0) -> ~(18, 20, 31, 255) */
            b32 is_bg = (r >= 14u && r <= 22u &&
                         g >= 16u && g <= 24u &&
                         b >= 27u && b <= 35u &&
                         a == 255u);
            if (is_bg) {
                sum.bg_pixels++;
                continue;
            }

            if (x < 260u) {
                /* Sol bölge (x in [85, 248]): dönen renkli üçgen (~7381 piksel) */
                sum.tri_pixels++;
                if (r > g + 35u && r > b + 35u) sum.tri_red_pixels++;
                if (g > r + 35u && g > b + 35u) sum.tri_green_pixels++;
                if (b > r + 35u && b > g + 35u) sum.tri_blue_pixels++;
            } else {
                /* Sağ bölge: dama dokulu, yönlü ışıklı 3D küp */
                sum.cube_pixels++;
                if (r > b + 25u) sum.cube_warm_pixels++;
                if (b > r + 25u) sum.cube_cool_pixels++;
            }
        }
    }

    sum.passed = (sum.bg_pixels        >= 100000u &&
                  sum.tri_pixels       >= 6000u   &&
                  sum.tri_red_pixels   >= 500u    &&
                  sum.tri_green_pixels >= 500u    &&
                  sum.tri_blue_pixels  >= 500u    &&
                  sum.cube_pixels      >= 20000u  &&
                  sum.cube_warm_pixels >= 5000u   &&
                  sum.cube_cool_pixels >= 5000u   &&
                  sum.gpu_errors       == 0u) ? TRUE : FALSE;

    st->summary = sum;
    return sum.passed;
}

EXPORT(engine_gpu_test_summary) const GpuTestSummary* engine_gpu_test_summary(void) {
    return &g_gpu_test.summary;
}

/* ============================================================================
 * T2: Matoki CPU JIT <-> WebGPU Diferansiyel Karşılaştırma Test Altyapısı
 *  Tüm 16 çekirdeğin (12 pixel + 4 compute) CPU Wasm JIT ve WebGPU çıktılarını
 *  deterministik girdilerle karşılaştırır, piksel/eleman farklarını ölçer
 *  ve 128x128 RGBA8 fark haritası (error heatmap) üretir.
 * ============================================================================ */

#define GPU_COMPARE_W 128u
#define GPU_COMPARE_H 128u
#define GPU_COMPARE_PIXELS (GPU_COMPARE_W * GPU_COMPARE_H)
#define GPU_COMPARE_BYTES  (GPU_COMPARE_PIXELS * 4u) /* 64 KB */

typedef struct {
    u32   index;
    char  name[32];
    b32   is_compute;
    u32   width;
    u32   height;
    u32   total_count;
    f32   max_diff;
    f32   mean_diff;
    u32   exceeded_count;
    f32   tolerance;
    b32   passed;
    u32   out_buffer_handle;
    u32   out_buffer_bytes;
} GpuCompareResult;

static u8  g_cmp_arena_mem[MB(16)] __attribute__((aligned(16)));
static u8  g_cmp_cpu_buf[GPU_COMPARE_BYTES] __attribute__((aligned(16)));
static u8  g_cmp_gpu_buf[GPU_COMPARE_BYTES] __attribute__((aligned(16)));
static u8  g_cmp_diff_map[GPU_COMPARE_BYTES] __attribute__((aligned(16)));
static GpuCompareResult g_cmp_results[16];
static b32 g_cmp_inited = FALSE;
static Arena g_cmp_arena;
static Rhi   g_cmp_rhi;

/* Ortak prosedürel test dokuları */
static Texture    g_cmp_tex_common;
static Texture    g_cmp_tex_ocean0;
static Texture    g_cmp_tex_ocean1;
static Texture    g_cmp_tex_ocean2;
static Texture    g_cmp_tex_shadow;
static f32        g_cmp_sh_rgba[64u * 64u * 4u] __attribute__((aligned(16)));
static RhiTexture g_cmp_rhi_tex_common;
static RhiTexture g_cmp_rhi_tex_ocean0;
static RhiTexture g_cmp_rhi_tex_ocean1;
static RhiTexture g_cmp_rhi_tex_ocean2;
static RhiTexture g_cmp_rhi_tex_shadow;
static RhiSampler g_cmp_rhi_samp_repeat;
static RhiSampler g_cmp_rhi_samp_clamp;

static b32 cmp_str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return FALSE;
        a++;
        b++;
    }
    return *a == *b;
}

static void cmp_str_copy(char* dst, const char* src, usize max_len) {
    usize i = 0;
    while (i + 1 < max_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void compare_init_textures_once(void) {
    if (g_cmp_inited) return;

    arena_init(&g_cmp_arena, g_cmp_arena_mem, sizeof(g_cmp_arena_mem));
    rhi_init(&g_cmp_rhi, &g_cmp_arena, 256u, (u32)MB(2));

    /* 1. Ortak RGBA8 dama dokusu (64x64, mipmapped 6 levels) */
    tex_init_mipmapped(&g_cmp_tex_common, &g_cmp_arena, 64u, 64u, 0u);
    tex_gen_checker(&g_cmp_tex_common, 8u, FB_RGBA(235, 175, 45, 255), FB_RGBA(45, 175, 235, 255));
    tex_build_mips(&g_cmp_tex_common);

    /* 2. Okyanus Dokuları (3 adet 64x64) */
    tex_init_mipmapped(&g_cmp_tex_ocean0, &g_cmp_arena, 64u, 64u, 0u);
    tex_gen_gradient(&g_cmp_tex_ocean0, FB_RGBA(128, 128, 140, 255), FB_RGBA(135, 120, 125, 255),
                                         FB_RGBA(120, 135, 130, 255), FB_RGBA(128, 128, 145, 255));
    tex_build_mips(&g_cmp_tex_ocean0);

    tex_init_mipmapped(&g_cmp_tex_ocean1, &g_cmp_arena, 64u, 64u, 0u);
    tex_gen_checker(&g_cmp_tex_ocean1, 4u, FB_RGBA(125, 130, 135, 255), FB_RGBA(130, 125, 120, 255));
    tex_build_mips(&g_cmp_tex_ocean1);

    tex_init_mipmapped(&g_cmp_tex_ocean2, &g_cmp_arena, 64u, 64u, 0u);
    tex_gen_noise(&g_cmp_tex_ocean2, 0xA1B2C3D4E5F6ull);
    tex_build_mips(&g_cmp_tex_ocean2);

    /* 3. Gölge Haritası Dokusu (64x64, RGBA32F doku olarak r kanalında derinlik) */
    tex_init(&g_cmp_tex_shadow, &g_cmp_arena, 64u, 64u, TEX_FLAG_F32);
    f32* sh_px = (f32*)g_cmp_tex_shadow.px;
    for (u32 y = 0; y < 64; y++) {
        for (u32 x = 0; x < 64; x++) {
            f32 val = 0.5f + 0.35f * m_sin((f32)x * 0.15f) * m_cos((f32)y * 0.15f);
            sh_px[y * 64 + x] = val;
            g_cmp_sh_rgba[(y * 64 + x) * 4 + 0] = val;
            g_cmp_sh_rgba[(y * 64 + x) * 4 + 1] = 0.0f;
            g_cmp_sh_rgba[(y * 64 + x) * 4 + 2] = 0.0f;
            g_cmp_sh_rgba[(y * 64 + x) * 4 + 3] = 1.0f;
        }
    }

    g_cmp_inited = TRUE;
}

EXPORT(engine_gpu_compare_init_gpu) u32 engine_gpu_compare_init_gpu(void) {
    compare_init_textures_once();
    Rhi* rhi = &g_cmp_rhi;
    rhi_set_active(rhi);
    rhi_begin_frame(rhi);

    RhiTextureDesc desc_common = {
        .width      = 64u,
        .height     = 64u,
        .mip_levels = g_cmp_tex_common.levels,
        .format     = RHI_FMT_RGBA8_UNORM,
        .usage      = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    g_cmp_rhi_tex_common = rhi_create_texture(rhi, &desc_common);
    for (u32 lvl = 0; lvl < g_cmp_tex_common.levels; lvl++) {
        u32 mw = MAX(64u >> lvl, 1u);
        u32 mh = MAX(64u >> lvl, 1u);
        u32 off = tex_mip_offset(&g_cmp_tex_common, lvl);
        u32 row_b = mw * 4u;
        rhi_write_texture(rhi, g_cmp_rhi_tex_common, lvl, 0, 0, mw, mh,
                          row_b, g_cmp_tex_common.px + off, row_b * mh);
    }

    g_cmp_rhi_tex_ocean0 = rhi_create_texture(rhi, &desc_common);
    for (u32 lvl = 0; lvl < g_cmp_tex_ocean0.levels; lvl++) {
        u32 mw = MAX(64u >> lvl, 1u);
        u32 mh = MAX(64u >> lvl, 1u);
        u32 off = tex_mip_offset(&g_cmp_tex_ocean0, lvl);
        u32 row_b = mw * 4u;
        rhi_write_texture(rhi, g_cmp_rhi_tex_ocean0, lvl, 0, 0, mw, mh,
                          row_b, g_cmp_tex_ocean0.px + off, row_b * mh);
    }

    g_cmp_rhi_tex_ocean1 = rhi_create_texture(rhi, &desc_common);
    for (u32 lvl = 0; lvl < g_cmp_tex_ocean1.levels; lvl++) {
        u32 mw = MAX(64u >> lvl, 1u);
        u32 mh = MAX(64u >> lvl, 1u);
        u32 off = tex_mip_offset(&g_cmp_tex_ocean1, lvl);
        u32 row_b = mw * 4u;
        rhi_write_texture(rhi, g_cmp_rhi_tex_ocean1, lvl, 0, 0, mw, mh,
                          row_b, g_cmp_tex_ocean1.px + off, row_b * mh);
    }

    g_cmp_rhi_tex_ocean2 = rhi_create_texture(rhi, &desc_common);
    for (u32 lvl = 0; lvl < g_cmp_tex_ocean2.levels; lvl++) {
        u32 mw = MAX(64u >> lvl, 1u);
        u32 mh = MAX(64u >> lvl, 1u);
        u32 off = tex_mip_offset(&g_cmp_tex_ocean2, lvl);
        u32 row_b = mw * 4u;
        rhi_write_texture(rhi, g_cmp_rhi_tex_ocean2, lvl, 0, 0, mw, mh,
                          row_b, g_cmp_tex_ocean2.px + off, row_b * mh);
    }

    RhiTextureDesc desc_shadow = {
        .width      = 64u,
        .height     = 64u,
        .mip_levels = 1u,
        .format     = RHI_FMT_RGBA32_FLOAT,
        .usage      = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST
    };
    g_cmp_rhi_tex_shadow = rhi_create_texture(rhi, &desc_shadow);
    rhi_write_texture(rhi, g_cmp_rhi_tex_shadow, 0u, 0, 0, 64u, 64u,
                      64u * 16u, g_cmp_sh_rgba, 64u * 64u * 16u);

    RhiSamplerDesc samp_rep = {
        .address_u     = RHI_ADDR_REPEAT,
        .address_v     = RHI_ADDR_REPEAT,
        .mag_filter    = RHI_FILTER_LINEAR,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_LINEAR
    };
    g_cmp_rhi_samp_repeat = rhi_create_sampler(rhi, &samp_rep);

    RhiSamplerDesc samp_clm = {
        .address_u     = RHI_ADDR_CLAMP,
        .address_v     = RHI_ADDR_CLAMP,
        .mag_filter    = RHI_FILTER_LINEAR,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_NEAREST
    };
    g_cmp_rhi_samp_clamp = rhi_create_sampler(rhi, &samp_clm);

    return rhi_stream_bytes(rhi);
}

static void eval_compare_varyings(f32 u, f32 v, const MtkLayout* layout, f32* out_vars) {
    u32 off = 0;
    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* s = &layout->syms[i];
        if (s->kind != MTK_SYM_VARYING) continue;
        if (cmp_str_eq(s->name, "uv")) {
            out_vars[off + 0] = u;
            out_vars[off + 1] = v;
        } else if (cmp_str_eq(s->name, "nrm")) {
            f32 nx = u * 2.0f - 1.0f;
            f32 ny = v * 2.0f - 1.0f;
            f32 nz = 0.85f;
            f32 inv_l = 1.0f / m_sqrt(nx * nx + ny * ny + nz * nz);
            out_vars[off + 0] = nx * inv_l;
            out_vars[off + 1] = ny * inv_l;
            out_vars[off + 2] = nz * inv_l;
        } else if (cmp_str_eq(s->name, "depth")) {
            out_vars[off + 0] = 1.5f + 4.0f * u + 2.5f * v;
        } else if (cmp_str_eq(s->name, "wpos")) {
            out_vars[off + 0] = u * 10.0f - 5.0f;
            out_vars[off + 1] = v * 6.0f - 3.0f;
            out_vars[off + 2] = 1.5f + 4.0f * u + 2.5f * v;
        } else if (cmp_str_eq(s->name, "spos")) {
            out_vars[off + 0] = u;
            out_vars[off + 1] = v;
            out_vars[off + 2] = 0.45f + 0.35f * u;
        } else if (cmp_str_eq(s->name, "v_color")) {
            out_vars[off + 0] = u;
            out_vars[off + 1] = v;
            out_vars[off + 2] = 1.0f - u;
            out_vars[off + 3] = 1.0f;
        } else {
            for (u32 c = 0; c < s->components; c++) {
                out_vars[off + c] = 0.5f;
            }
        }
        off += s->components;
    }
}

static void fill_compare_uniforms(const char* name, u8* ubo, u32 ubo_size) {
    __builtin_memset(ubo, 0, ubo_size);
    f32* f = (f32*)ubo;

    if (cmp_str_eq(name, "branchy.mtk")) {
        f[0] = 1.15f;
    } else if (cmp_str_eq(name, "plasma.mtk")) {
        f[0] = 1.25f;
    } else if (cmp_str_eq(name, "solid.mtk")) {
        f[0] = 0.25f; f[1] = 0.65f; f[2] = 0.85f; f[3] = 1.0f;
    } else if (cmp_str_eq(name, "vcolor.mtk")) {
        f[0] = 0.75f; f[1] = 0.85f; f[2] = 0.95f; f[3] = 1.0f;
    } else if (cmp_str_eq(name, "sprite.mtk")) {
        f[0] = 1.0f; f[1] = 0.95f; f[2] = 0.85f; f[3] = 1.0f;
    } else if (cmp_str_eq(name, "unlit3d.mtk")) {
        f[0] = 0.9f; f[1] = 0.4f; f[2] = 0.2f; f[3] = 1.0f;
        f[4] = 0.1f; f[5] = 0.2f; f[6] = 0.3f; f[7] = 0.05f;
    } else if (cmp_str_eq(name, "particle3d.mtk")) {
        f[0] = 0.2f; f[1] = 0.2f; f[2] = 0.3f; f[3] = 0.04f;
    } else if (cmp_str_eq(name, "lit3d.mtk")) {
        f[0] = 0.5f; f[1] = 0.8f; f[2] = 0.3f; f[3] = 0.0f;
        f[4] = 0.9f; f[5] = 0.95f; f[6] = 1.0f; f[7] = 1.0f;
        f[8] = 0.15f; f[9] = 0.2f; f[10] = 0.3f; f[11] = 0.02f;
    } else if (cmp_str_eq(name, "sky.mtk")) {
        f[0] = 0.4f; f[1] = 0.6f; f[2] = 0.5f; f[3] = 0.0f;
        f[4] = 1.4f; f[5] = 1.2f; f[6] = 0.9f; f[7] = 0.0f;
        f[8] = 0.15f; f[9] = 0.35f; f[10] = 0.75f; f[11] = 0.0f;
        f[12] = 0.65f; f[13] = 0.75f; f[14] = 0.85f; f[15] = 0.0f;
        f[16] = 0.0f; f[17] = 0.5f; f[18] = 1.0f; f[19] = 0.2f;
    } else if (cmp_str_eq(name, "dungeon3d.mtk")) {
        f[0] = 0.15f; f[1] = 0.12f; f[2] = 0.10f; f[3] = 1.0f;
        f[4] = 1.0f;  f[5] = 0.95f; f[6] = 0.9f;  f[7] = 1.0f;
        f[8] = 0.08f; f[9] = 0.06f; f[10] = 0.05f; f[11] = 0.05f;
        f[12] = 0.0f; f[13] = 2.0f; f[14] = 0.0f; f[15] = 0.08f;
        f[16] = 1.2f; f[17] = 0.8f; f[18] = 0.4f; f[19] = 1.0f;
        f[20] = 3.0f; f[21] = 1.5f; f[22] = 2.0f; f[23] = 0.06f;
        f[24] = 0.4f; f[25] = 0.7f; f[26] = 1.1f; f[27] = 1.0f;
        f[28] = -2.5f; f[29] = 1.8f; f[30] = -1.0f; f[31] = 0.05f;
        f[32] = 0.8f; f[33] = 0.3f; f[34] = 0.2f; f[35] = 1.0f;
        f[36] = 0.0f; f[37] = 3.0f; f[38] = 5.0f; f[39] = 0.04f;
        f[40] = 0.6f; f[41] = 0.6f; f[42] = 0.8f; f[43] = 1.0f;
    } else if (cmp_str_eq(name, "ocean.mtk")) {
        f[0] = 0.45f; f[1] = 0.65f; f[2] = 0.45f; f[3] = 0.0f;
        f[4] = 1.3f;  f[5] = 1.1f;  f[6] = 0.85f; f[7] = 0.0f;
        f[8] = 0.12f; f[9] = 0.32f; f[10] = 0.72f; f[11] = 0.0f;
        f[12] = 0.62f; f[13] = 0.72f; f[14] = 0.82f; f[15] = 0.0f;
        f[16] = 0.02f; f[17] = 0.06f; f[18] = 0.15f; f[19] = 0.0f;
        f[20] = 0.05f; f[21] = 0.25f; f[22] = 0.35f; f[23] = 0.0f;
        f[24] = 0.0f;  f[25] = 5.0f;  f[26] = -10.0f; f[27] = 0.0f;
        f[28] = 2.17f; f[29] = 4.33f; f[30] = 0.55f; f[31] = 0.012f;
        f[32] = 0.45f; f[33] = 0.65f; f[34] = 0.85f; f[35] = 0.25f;
    } else if (cmp_str_eq(name, "pbr3d.mtk")) {
        f[0] = 0.5f;  f[1] = 0.75f; f[2] = 0.4f;  f[3] = 0.0f;
        f[4] = 1.5f;  f[5] = 1.35f; f[6] = 1.2f;  f[7] = 0.0f;
        f[8] = 0.25f; f[9] = 0.35f; f[10] = 0.55f; f[11] = 0.0f;
        f[12] = 0.15f; f[13] = 0.12f; f[14] = 0.10f; f[15] = 0.0f;
        f[16] = 0.0f;  f[17] = 1.5f;  f[18] = 4.0f;  f[19] = 0.0f;
        f[20] = 1.0f;  f[21] = 0.95f; f[22] = 0.9f;  f[23] = 1.0f;
        f[24] = 0.35f; f[25] = 0.45f; f[26] = 0.002f; f[27] = 64.0f;
        f[28] = 0.2f;  f[29] = 0.25f; f[30] = 0.35f; f[31] = 0.015f;
    } else if (cmp_str_eq(name, "particle.mtk")) {
        f[0] = 0.016f;
        f[4] = -9.81f;
        f[8] = 0.99f;
        f[12] = 0.0f;
        f[16] = 0.75f;
    }
}

static u32 generate_compare_vertex_shader(const MtkLayout* layout, char* out, u32 cap) {
    u32 pos = 0;
    #define APPEND(s) do { \
        const char* _s = (s); \
        while (*_s && pos + 1 < cap) out[pos++] = *_s++; \
    } while (0)

    APPEND("struct VsOut {\n    @builtin(position) pos: vec4<f32>,\n");
    u32 loc = 0;
    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* s = &layout->syms[i];
        if (s->kind != MTK_SYM_VARYING) continue;
        char line[128];
        const char* tname = (s->components == 1) ? "f32" :
                            (s->components == 2) ? "vec2<f32>" :
                            (s->components == 3) ? "vec3<f32>" : "vec4<f32>";
        fmt_format(line, sizeof(line), "    @location(%u) v_%s: %s,\n", loc++, s->name, tname);
        APPEND(line);
    }
    APPEND("};\n\nstruct VsIn {\n    @location(0) pos: vec2<f32>,\n");
    loc = 0;
    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* s = &layout->syms[i];
        if (s->kind != MTK_SYM_VARYING) continue;
        char line[128];
        const char* tname = (s->components == 1) ? "f32" :
                            (s->components == 2) ? "vec2<f32>" :
                            (s->components == 3) ? "vec3<f32>" : "vec4<f32>";
        fmt_format(line, sizeof(line), "    @location(%u) in_%s: %s,\n", 1u + loc++, s->name, tname);
        APPEND(line);
    }
    APPEND("};\n\n@vertex\nfn vs_main(vin: VsIn) -> VsOut {\n    var out: VsOut;\n");
    APPEND("    out.pos = vec4<f32>(vin.pos, 0.0, 1.0);\n");
    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* s = &layout->syms[i];
        if (s->kind != MTK_SYM_VARYING) continue;
        char line[128];
        fmt_format(line, sizeof(line), "    out.v_%s = vin.in_%s;\n", s->name, s->name);
        APPEND(line);
    }
    APPEND("    return out;\n}\n");
    #undef APPEND
    out[pos] = '\0';
    return pos;
}

EXPORT(engine_gpu_compare_count) u32 engine_gpu_compare_count(void) {
    return g_embedded_shader_count;
}

EXPORT(engine_gpu_compare_name) const char* engine_gpu_compare_name(u32 idx) {
    if (idx >= g_embedded_shader_count) return "";
    return g_embedded_shaders[idx].name;
}

EXPORT(engine_gpu_compare_is_compute) b32 engine_gpu_compare_is_compute(u32 idx) {
    if (idx >= g_embedded_shader_count) return FALSE;
    const char* n = g_embedded_shaders[idx].name;
    return (cmp_str_eq(n, "particle.mtk") ||
            cmp_str_eq(n, "prefix_sum_add.mtk") ||
            cmp_str_eq(n, "prefix_sum_scan.mtk") ||
            cmp_str_eq(n, "skinning.mtk"));
}

EXPORT(engine_gpu_compare_dim) void engine_gpu_compare_dim(u32 idx, u32* out_w, u32* out_h) {
    if (engine_gpu_compare_is_compute(idx)) {
        *out_w = (idx == 4u) ? 512u : 1024u;
        *out_h = 1u;
    } else {
        *out_w = GPU_COMPARE_W;
        *out_h = GPU_COMPARE_H;
    }
}

EXPORT(engine_gpu_compare_readback_ptr) u8* engine_gpu_compare_readback_ptr(void) {
    return g_cmp_gpu_buf;
}

EXPORT(engine_gpu_compare_readback_bytes) u32 engine_gpu_compare_readback_bytes(void) {
    return GPU_COMPARE_BYTES;
}

EXPORT(engine_gpu_compare_diff_map_ptr) u8* engine_gpu_compare_diff_map_ptr(void) {
    return g_cmp_diff_map;
}

EXPORT(engine_gpu_compare_result) const GpuCompareResult* engine_gpu_compare_result(u32 idx) {
    if (idx >= 16u) return NULL;
    return &g_cmp_results[idx];
}

/* 1. CPU JIT Çalıştırma */
EXPORT(engine_gpu_compare_run_cpu) b32 engine_gpu_compare_run_cpu(u32 idx) {
    if (idx >= g_embedded_shader_count) return FALSE;
    compare_init_textures_once();

    const EmbeddedFile* emb = &g_embedded_shaders[idx];
    b32 is_comp = engine_gpu_compare_is_compute(idx);

    usize mark = arena_mark(&g_cmp_arena);
    MtkCodegenOpts copts = {
#if PLAT_SHARED_MEMORY
        .shared_memory = TRUE,
#else
        .shared_memory = FALSE,
#endif
        .own_memory = FALSE,
        .fuse_blend = MTK_FUSE_NONE
    };
    MtkLayout layout;
    MtkDiag diag;
    __builtin_memset(&layout, 0, sizeof(layout));
    mtk_diag_init(&diag);

    u8* wasm_buf = ARENA_PUSH_ARRAY(&g_cmp_arena, u8, 65536u);
    u32 wasm_len = mtk_compile_wasm(emb->src, emb->len, emb->name,
                                    &copts, &g_cmp_arena, wasm_buf, 65536u,
                                    &layout, &diag);
    if (wasm_len == 0u) {
        LOG("CPU JIT derleme hatasi (%s): %s", emb->name, diag.text);
        arena_pop_to(&g_cmp_arena, mark);
        return FALSE;
    }

    const char* exp_name = is_comp ? "compute" : "shade";
    u32 exp_len = (u32)(is_comp ? 7u : 5u);
    i32 jit_idx = plat_jit_load(wasm_buf, wasm_len, exp_name, exp_len);
    if (jit_idx < 0) {
        LOG("plat_jit_load hatasi (%s)", emb->name);
        arena_pop_to(&g_cmp_arena, mark);
        return FALSE;
    }

    if (!is_comp) {
        /* Pixel Shader CPU JIT Yürütme (128x128 = 4096 quad) */
        __builtin_memset(g_cmp_cpu_buf, 0, GPU_COMPARE_BYTES);

        u32 ubo_sz = ALIGN_UP(layout.uniform_bytes, 16u);
        u8* ubo = ARENA_PUSH_ARRAY(&g_cmp_arena, u8, MAX(ubo_sz, 16u));
        fill_compare_uniforms(emb->name, ubo, ubo_sz);

        Texture active_tex[4];
        __builtin_memset(active_tex, 0, sizeof(active_tex));
        if (cmp_str_eq(emb->name, "ocean.mtk")) {
            active_tex[0] = g_cmp_tex_ocean0;
            active_tex[1] = g_cmp_tex_ocean1;
            active_tex[2] = g_cmp_tex_ocean2;
        } else if (cmp_str_eq(emb->name, "pbr3d.mtk")) {
            active_tex[0] = g_cmp_tex_common;
            active_tex[1] = g_cmp_tex_shadow;
        } else {
            active_tex[0] = g_cmp_tex_common;
        }

        /* 4 köşedeki varying değerlerini hesapla */
        f32 v_c00[16], v_c10[16], v_c11[16], v_c01[16];
        eval_compare_varyings(0.0f, 0.0f, &layout, v_c00);
        eval_compare_varyings(1.0f, 0.0f, &layout, v_c10);
        eval_compare_varyings(1.0f, 1.0f, &layout, v_c11);
        eval_compare_varyings(0.0f, 1.0f, &layout, v_c01);

        u32 nvar = layout.varying_count;
        QuadHeader quads[256];
        f32* varyings = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, nvar * 256u * 4u);
        f32* out_rgba = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, 4u * 256u * 4u);

        ShadeBatch batch;
        batch.varying_count = nvar;
        batch.quads         = quads;
        batch.varyings      = varyings;
        batch.uniforms      = ubo;
        batch.out_rgba      = out_rgba;
        batch.textures      = active_tex;
        batch.fb_px         = NULL;
        batch.fb_stride     = 0u;

        ShadeFn fn = (ShadeFn)(usize)jit_idx;

        /* 64x64 = 4096 quad'ı 256'lık 16 batch halinde işle */
        for (u32 b_idx = 0u; b_idx < 16u; b_idx++) {
            u32 q_base = b_idx * 256u;
            batch.count = 256u;

            for (u32 q = 0u; q < 256u; q++) {
                u32 q_id = q_base + q;
                u32 qx = q_id % 64u;
                u32 qy = q_id / 64u;
                quads[q].mask = 0x0Fu;
                quads[q].x = (u16)(qx * 2u);
                quads[q].y = (u16)(qy * 2u);

                for (u32 lane = 0u; lane < 4u; lane++) {
                    u32 px = qx * 2u + (lane & 1u);
                    u32 py = qy * 2u + (lane >> 1u);
                    f32 u = ((f32)px + 0.5f) / (f32)GPU_COMPARE_W;
                    f32 v = ((f32)py + 0.5f) / (f32)GPU_COMPARE_H;

                    f32 w00 = (1.0f - u) * (1.0f - v);
                    f32 w10 = u * (1.0f - v);
                    f32 w11 = u * v;
                    f32 w01 = (1.0f - u) * v;

                    for (u32 v_idx = 0u; v_idx < nvar; v_idx++) {
                        f32 val = v_c00[v_idx] * w00 + v_c10[v_idx] * w10 +
                                  v_c11[v_idx] * w11 + v_c01[v_idx] * w01;
                        varyings[(v_idx * 256u + q) * 4u + lane] = val;
                    }
                }
            }

            fn(&batch);

            for (u32 q = 0u; q < 256u; q++) {
                u32 q_id = q_base + q;
                u32 qx = q_id % 64u;
                u32 qy = q_id / 64u;

                for (u32 lane = 0u; lane < 4u; lane++) {
                    u32 px = qx * 2u + (lane & 1u);
                    u32 py = qy * 2u + (lane >> 1u);
                    u32 p_idx = py * GPU_COMPARE_W + px;

                    if ((quads[q].mask & (1u << lane)) == 0u) {
                        /* Discard: temizleme renginde bırak (siyah/saydam) */
                        g_cmp_cpu_buf[p_idx * 4u + 0u] = 0u;
                        g_cmp_cpu_buf[p_idx * 4u + 1u] = 0u;
                        g_cmp_cpu_buf[p_idx * 4u + 2u] = 0u;
                        g_cmp_cpu_buf[p_idx * 4u + 3u] = 0u;
                    } else {
                        f32 r = out_rgba[(0u * 256u + q) * 4u + lane];
                        f32 g = out_rgba[(1u * 256u + q) * 4u + lane];
                        f32 b = out_rgba[(2u * 256u + q) * 4u + lane];
                        f32 a = out_rgba[(3u * 256u + q) * 4u + lane];

                        r = CLAMP(r, 0.0f, 1.0f) * 255.0f + 0.5f;
                        g = CLAMP(g, 0.0f, 1.0f) * 255.0f + 0.5f;
                        b = CLAMP(b, 0.0f, 1.0f) * 255.0f + 0.5f;
                        a = CLAMP(a, 0.0f, 1.0f) * 255.0f + 0.5f;

                        g_cmp_cpu_buf[p_idx * 4u + 0u] = (u8)r;
                        g_cmp_cpu_buf[p_idx * 4u + 1u] = (u8)g;
                        g_cmp_cpu_buf[p_idx * 4u + 2u] = (u8)b;
                        g_cmp_cpu_buf[p_idx * 4u + 3u] = (u8)a;
                    }
                }
            }
        }
    } else {
        /* Compute Shader CPU JIT Yürütme */
        __builtin_memset(g_cmp_cpu_buf, 0, GPU_COMPARE_BYTES);
        MtkComputeFn cfn = (MtkComputeFn)(usize)jit_idx;

        if (cmp_str_eq(emb->name, "particle.mtk")) {
            u8 ubo[80];
            fill_compare_uniforms(emb->name, ubo, sizeof(ubo));

            f32* pos = (f32*)g_cmp_cpu_buf;
            f32* vel = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, 1024u * 4u);
            Rng rng;
            rng_seed(&rng, 0x123456789ABCDEF0ull);
            for (u32 i = 0u; i < 1024u * 4u; i += 4u) {
                pos[i + 0] = rng_f32(&rng) * 20.0f - 10.0f;
                pos[i + 1] = rng_f32(&rng) * 4.0f - 1.5f;
                pos[i + 2] = rng_f32(&rng) * 20.0f - 10.0f;
                pos[i + 3] = rng_f32(&rng) * 2.0f;
                vel[i + 0] = rng_f32(&rng) * 6.0f - 3.0f;
                vel[i + 1] = rng_f32(&rng) * 6.0f - 5.0f;
                vel[i + 2] = rng_f32(&rng) * 6.0f - 3.0f;
                vel[i + 3] = 1.0f;
            }

            MtkComputeBatch b = {0};
            b.start = 0u; b.count = 1024u; b.total_count = 1024u; b.buffer_count = 2u;
            b.uniforms = ubo;
            b.buffers[0] = pos;
            b.buffers[1] = vel;
            cfn(&b);
        } else if (cmp_str_eq(emb->name, "prefix_sum_scan.mtk")) {
            u32* in_d = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 1024u);
            u32* out_d = (u32*)g_cmp_cpu_buf;
            u32* b_sums = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 256u);
            Rng rng;
            rng_seed(&rng, 0x9876543210FEDCBAull);
            for (u32 i = 0u; i < 1024u; i++) in_d[i] = rng_u32(&rng) % 17u;

            MtkComputeBatch b = {0};
            b.start = 0u; b.count = 256u; b.total_count = 256u; b.buffer_count = 3u;
            b.buffers[0] = in_d;
            b.buffers[1] = out_d;
            b.buffers[2] = b_sums;
            cfn(&b);
        } else if (cmp_str_eq(emb->name, "prefix_sum_add.mtk")) {
            u32* out_d = (u32*)g_cmp_cpu_buf;
            u32* b_offs = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 256u);
            for (u32 i = 0u; i < 1024u; i++) out_d[i] = (i & 3u) * 5u;
            u32 acc = 0u;
            for (u32 i = 0u; i < 256u; i++) { b_offs[i] = acc; acc += 20u; }

            MtkComputeBatch b = {0};
            b.start = 0u; b.count = 1024u; b.total_count = 1024u; b.buffer_count = 2u;
            b.buffers[0] = out_d;
            b.buffers[1] = b_offs;
            cfn(&b);
        } else if (cmp_str_eq(emb->name, "skinning.mtk")) {
            enum { N_SKIN = 512u, N_BONES = 32u };
            f32* in_pos     = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            f32* in_norm    = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            u32* in_joints  = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, N_SKIN);
            f32* in_weights = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            f32* bone_mats  = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_BONES * 3u * 4u);
            f32* out_pos    = (f32*)g_cmp_cpu_buf;
            f32* out_norm   = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);

            Rng rng;
            rng_seed(&rng, 0x42424242ull);
            for (u32 i = 0u; i < N_BONES * 12u; i++) bone_mats[i] = rng_f32(&rng) * 0.5f;
            for (u32 b = 0u; b < N_BONES; b++) {
                bone_mats[b * 12u + 0u] += 1.0f;
                bone_mats[b * 12u + 5u] += 1.0f;
                bone_mats[b * 12u + 10u] += 1.0f;
            }
            for (u32 i = 0u; i < N_SKIN; i++) {
                in_pos[i * 4u + 0] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 1] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 2] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 3] = 1.0f;

                in_norm[i * 4u + 0] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 1] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 2] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 3] = 0.0f;

                u32 b0 = rng_u32(&rng) % N_BONES;
                u32 b1 = rng_u32(&rng) % N_BONES;
                u32 b2 = rng_u32(&rng) % N_BONES;
                u32 b3 = rng_u32(&rng) % N_BONES;
                in_joints[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

                f32 w0 = rng_f32(&rng) + 0.1f;
                f32 w1 = rng_f32(&rng) + 0.1f;
                f32 w2 = rng_f32(&rng) + 0.1f;
                f32 w3 = rng_f32(&rng) + 0.1f;
                f32 inv_w = 1.0f / (w0 + w1 + w2 + w3);
                in_weights[i * 4u + 0] = w0 * inv_w;
                in_weights[i * 4u + 1] = w1 * inv_w;
                in_weights[i * 4u + 2] = w2 * inv_w;
                in_weights[i * 4u + 3] = w3 * inv_w;
            }

            MtkComputeBatch b = {0};
            b.start = 0u; b.count = N_SKIN; b.total_count = N_SKIN; b.buffer_count = 7u;
            b.buffers[0] = in_pos;
            b.buffers[1] = in_norm;
            b.buffers[2] = in_joints;
            b.buffers[3] = in_weights;
            b.buffers[4] = bone_mats;
            b.buffers[5] = out_pos;
            b.buffers[6] = out_norm;
            cfn(&b);
        }
    }

    arena_pop_to(&g_cmp_arena, mark);
    return TRUE;
}

/* 2. GPU RHI Komut Akışı Üretimi */
EXPORT(engine_gpu_compare_emit_gpu) u32 engine_gpu_compare_emit_gpu(u32 idx) {
    if (idx >= g_embedded_shader_count) return 0u;
    compare_init_textures_once();

    const EmbeddedFile* emb = &g_embedded_shaders[idx];
    b32 is_comp = engine_gpu_compare_is_compute(idx);
    Rhi* rhi = &g_cmp_rhi;

    rhi_set_active(rhi);
    rhi_begin_frame(rhi);

    usize mark = arena_mark(&g_cmp_arena);
    MtkLayout layout;
    MtkDiag diag;
    __builtin_memset(&layout, 0, sizeof(layout));
    mtk_diag_init(&diag);

    char* wgsl_buf = ARENA_PUSH_ARRAY(&g_cmp_arena, char, 65536u);
    MtkWgslOpts wopts = { .frag_group = 0u, .entry_name = is_comp ? "compute" : "frag" };
    u32 wgsl_len = mtk_compile_wgsl(emb->src, emb->len, emb->name,
                                    &wopts, &g_cmp_arena, wgsl_buf, 65536u,
                                    &layout, &diag);
    if (wgsl_len == 0u) {
        LOG("WGSL derleme hatasi (%s): %s", emb->name, diag.text);
        arena_pop_to(&g_cmp_arena, mark);
        return 0u;
    }

    RhiShader sh = rhi_create_shader(rhi, wgsl_buf, wgsl_len);

    if (!is_comp) {
        /* Pixel Shader GPU Pipeline & Draw Quad */
        char* vs_wgsl = ARENA_PUSH_ARRAY(&g_cmp_arena, char, 16384u);
        u32 vs_len = generate_compare_vertex_shader(&layout, vs_wgsl, 16384u);
        RhiShader vs_sh = rhi_create_shader(rhi, vs_wgsl, vs_len);

        u32 ubo_sz = ALIGN_UP(layout.uniform_bytes, 16u);
        u8* ubo_bytes = ARENA_PUSH_ARRAY(&g_cmp_arena, u8, MAX(ubo_sz, 16u));
        fill_compare_uniforms(emb->name, ubo_bytes, ubo_sz);

        RhiBuffer ubo_buf = rhi_create_buffer(rhi, MAX(ubo_sz, 16u), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
        rhi_write_buffer(rhi, ubo_buf, 0u, ubo_bytes, MAX(ubo_sz, 16u));

        /* Vertex Buffer & Attrs */
        RhiPipelineDesc pdesc;
        __builtin_memset(&pdesc, 0, sizeof(pdesc));
        pdesc.vertex_shader   = vs_sh;
        pdesc.fragment_shader = sh;
        pdesc.vs_entry        = "vs_main";
        pdesc.fs_entry        = "frag";
        pdesc.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
        pdesc.cull_mode       = RHI_CULL_NONE;
        pdesc.front_face      = RHI_FRONT_CCW;
        pdesc.color_format    = RHI_FMT_SURFACE_DEFAULT;
        pdesc.blend_mode      = RHI_BLEND_NONE;
        pdesc.depth_format    = 0;
        pdesc.depth_test      = FALSE;
        pdesc.depth_write     = FALSE;

        pdesc.attrs[0].shader_location = 0u;
        pdesc.attrs[0].offset          = 0u;
        pdesc.attrs[0].format          = RHI_VFMT_FLOAT32X2; /* pos */

        u32 stride = 8u;
        u32 attr_cnt = 1u;
        for (u32 i = 0u; i < layout.sym_count; i++) {
            const MtkSymbol* s = &layout.syms[i];
            if (s->kind != MTK_SYM_VARYING) continue;
            pdesc.attrs[attr_cnt].shader_location = attr_cnt;
            pdesc.attrs[attr_cnt].offset          = stride;
            pdesc.attrs[attr_cnt].format = (s->components == 1u) ? RHI_VFMT_FLOAT32 :
                                           (s->components == 2u) ? RHI_VFMT_FLOAT32X2 :
                                           (s->components == 3u) ? RHI_VFMT_FLOAT32X3 : RHI_VFMT_FLOAT32X4;
            stride += s->components * 4u;
            attr_cnt++;
        }
        pdesc.vertex_stride = stride;
        pdesc.attr_count    = attr_cnt;

        RhiPipeline pipe = rhi_create_pipeline(rhi, &pdesc);

        /* 4 köşe vertex verisi */
        u32 vert_f32_stride = stride / 4u;
        f32* vdata = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, 4u * vert_f32_stride);
        static const f32 k_ndc_xy[8] = {
            -1.0f,  1.0f,
             1.0f,  1.0f,
             1.0f, -1.0f,
            -1.0f, -1.0f
        };
        static const f32 k_uv_st[8] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f
        };
        for (u32 k = 0u; k < 4u; k++) {
            f32* vptr = vdata + k * vert_f32_stride;
            vptr[0] = k_ndc_xy[k * 2u + 0u];
            vptr[1] = k_ndc_xy[k * 2u + 1u];
            eval_compare_varyings(k_uv_st[k * 2u + 0u], k_uv_st[k * 2u + 1u], &layout, vptr + 2u);
        }

        RhiBuffer vb = rhi_create_buffer(rhi, 4u * stride, RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
        rhi_write_buffer(rhi, vb, 0u, vdata, 4u * stride);

        static const u16 k_quad_indices[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
        RhiBuffer ib = rhi_create_buffer(rhi, sizeof(k_quad_indices), RHI_BUF_INDEX | RHI_BUF_COPY_DST);
        rhi_write_buffer(rhi, ib, 0u, k_quad_indices, sizeof(k_quad_indices));

        /* Bind Group */
        RhiBindGroupDesc bgdesc;
        __builtin_memset(&bgdesc, 0, sizeof(bgdesc));
        bgdesc.pipeline    = pipe;
        bgdesc.group_index = 0u;

        u32 b_cnt = 0u;
        bgdesc.entries[b_cnt].binding  = 0u;
        bgdesc.entries[b_cnt].kind     = RHI_BIND_BUFFER;
        bgdesc.entries[b_cnt].resource = ubo_buf;
        bgdesc.entries[b_cnt].offset   = 0u;
        bgdesc.entries[b_cnt].size     = MAX(ubo_sz, 16u);
        b_cnt++;

        if (cmp_str_eq(emb->name, "ocean.mtk")) {
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_ocean0 };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_repeat };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_ocean1 };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 4u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_repeat };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 5u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_ocean2 };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 6u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_repeat };
        } else if (cmp_str_eq(emb->name, "pbr3d.mtk")) {
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_common };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_repeat };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 3u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_shadow };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 4u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_clamp };
        } else if (layout.texture_count > 0u) {
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = g_cmp_rhi_tex_common };
            bgdesc.entries[b_cnt++] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = g_cmp_rhi_samp_repeat };
        }
        bgdesc.entry_count = b_cnt;

        RhiBindGroup bg = rhi_create_bind_group(rhi, &bgdesc);

        /* Render Pass: Doğrudan yüzey tuvaline çiz */
        RhiPassDesc pass = {
            .color_texture   = RHI_INVALID_HANDLE,
            .depth_texture   = RHI_INVALID_HANDLE,
            .clear_color     = TRUE,
            .clear_r         = 0.0f,
            .clear_g         = 0.0f,
            .clear_b         = 0.0f,
            .clear_a         = 0.0f,
            .clear_depth     = FALSE,
            .clear_depth_val = 1.0f
        };
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, pipe);
        rhi_set_bind_group(rhi, 0u, bg);
        rhi_set_vertex_buffer(rhi, 0u, vb, 0u, 0u);
        rhi_set_index_buffer(rhi, ib, RHI_INDEX_UINT16, 0u, 0u);
        rhi_draw_indexed(rhi, 6u, 1u, 0u, 0, 0u);
        rhi_end_pass(rhi);
        rhi_present(rhi);

        g_cmp_results[idx].out_buffer_handle = 0u;
        g_cmp_results[idx].out_buffer_bytes  = GPU_COMPARE_BYTES;
    } else {
        /* Compute Shader GPU Pipeline & Dispatch */
        RhiComputePipelineDesc cpdesc = { .shader = sh, .entry = "compute" };
        RhiComputePipeline cpipe = rhi_create_compute_pipeline(rhi, &cpdesc);

        RhiBindGroupDesc cbgdesc;
        __builtin_memset(&cbgdesc, 0, sizeof(cbgdesc));
        cbgdesc.pipeline    = cpipe;
        cbgdesc.group_index = 0u;

        if (cmp_str_eq(emb->name, "particle.mtk")) {
            u8 ubo[80];
            fill_compare_uniforms(emb->name, ubo, sizeof(ubo));
            RhiBuffer u_buf = rhi_create_buffer(rhi, sizeof(ubo), RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
            rhi_write_buffer(rhi, u_buf, 0u, ubo, sizeof(ubo));

            f32* pos = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, 1024u * 4u);
            f32* vel = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, 1024u * 4u);
            Rng rng;
            rng_seed(&rng, 0x123456789ABCDEF0ull);
            for (u32 i = 0u; i < 1024u * 4u; i += 4u) {
                pos[i + 0] = rng_f32(&rng) * 20.0f - 10.0f;
                pos[i + 1] = rng_f32(&rng) * 4.0f - 1.5f;
                pos[i + 2] = rng_f32(&rng) * 20.0f - 10.0f;
                pos[i + 3] = rng_f32(&rng) * 2.0f;
                vel[i + 0] = rng_f32(&rng) * 6.0f - 3.0f;
                vel[i + 1] = rng_f32(&rng) * 6.0f - 5.0f;
                vel[i + 2] = rng_f32(&rng) * 6.0f - 3.0f;
                vel[i + 3] = 1.0f;
            }

            RhiBuffer b_pos = rhi_create_buffer(rhi, 1024u * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST | RHI_BUF_COPY_SRC);
            RhiBuffer b_vel = rhi_create_buffer(rhi, 1024u * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            rhi_write_buffer(rhi, b_pos, 0u, pos, 1024u * 16u);
            rhi_write_buffer(rhi, b_vel, 0u, vel, 1024u * 16u);

            cbgdesc.entries[0] = (RhiBindEntry){ .binding = 0u, .kind = RHI_BIND_BUFFER, .resource = u_buf, .size = sizeof(ubo) };
            cbgdesc.entries[1] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_BUFFER, .resource = b_pos, .size = 1024u * 16u };
            cbgdesc.entries[2] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_BUFFER, .resource = b_vel, .size = 1024u * 16u };
            cbgdesc.entry_count = 3u;
            RhiBindGroup cbg = rhi_create_bind_group(rhi, &cbgdesc);

            rhi_begin_compute_pass(rhi);
            rhi_set_compute_pipeline(rhi, cpipe);
            rhi_set_bind_group(rhi, 0u, cbg);
            rhi_dispatch(rhi, 1024u / 64u, 1u, 1u);
            rhi_end_compute_pass(rhi);

            g_cmp_results[idx].out_buffer_handle = b_pos;
            g_cmp_results[idx].out_buffer_bytes  = 1024u * 16u;
        } else if (cmp_str_eq(emb->name, "prefix_sum_scan.mtk")) {
            u32* in_d = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 1024u);
            Rng rng;
            rng_seed(&rng, 0x9876543210FEDCBAull);
            for (u32 i = 0u; i < 1024u; i++) in_d[i] = rng_u32(&rng) % 17u;

            RhiBuffer b_in  = rhi_create_buffer(rhi, 1024u * 4u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_out = rhi_create_buffer(rhi, 1024u * 4u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST | RHI_BUF_COPY_SRC);
            RhiBuffer b_sum = rhi_create_buffer(rhi, 256u * 4u,  RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            rhi_write_buffer(rhi, b_in, 0u, in_d, 1024u * 4u);

            cbgdesc.entries[0] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_BUFFER, .resource = b_in,  .size = 1024u * 4u };
            cbgdesc.entries[1] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_BUFFER, .resource = b_out, .size = 1024u * 4u };
            cbgdesc.entries[2] = (RhiBindEntry){ .binding = 3u, .kind = RHI_BIND_BUFFER, .resource = b_sum, .size = 256u * 4u };
            cbgdesc.entry_count = 3u;
            RhiBindGroup cbg = rhi_create_bind_group(rhi, &cbgdesc);

            rhi_begin_compute_pass(rhi);
            rhi_set_compute_pipeline(rhi, cpipe);
            rhi_set_bind_group(rhi, 0u, cbg);
            rhi_dispatch(rhi, 256u / 64u, 1u, 1u);
            rhi_end_compute_pass(rhi);

            g_cmp_results[idx].out_buffer_handle = b_out;
            g_cmp_results[idx].out_buffer_bytes  = 1024u * 4u;
        } else if (cmp_str_eq(emb->name, "prefix_sum_add.mtk")) {
            u32* out_d = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 1024u);
            u32* b_offs = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, 256u);
            for (u32 i = 0u; i < 1024u; i++) out_d[i] = (i & 3u) * 5u;
            u32 acc = 0u;
            for (u32 i = 0u; i < 256u; i++) { b_offs[i] = acc; acc += 20u; }

            RhiBuffer b_out = rhi_create_buffer(rhi, 1024u * 4u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST | RHI_BUF_COPY_SRC);
            RhiBuffer b_off = rhi_create_buffer(rhi, 256u * 4u,  RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            rhi_write_buffer(rhi, b_out, 0u, out_d,  1024u * 4u);
            rhi_write_buffer(rhi, b_off, 0u, b_offs, 256u * 4u);

            cbgdesc.entries[0] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_BUFFER, .resource = b_out, .size = 1024u * 4u };
            cbgdesc.entries[1] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_BUFFER, .resource = b_off, .size = 256u * 4u };
            cbgdesc.entry_count = 2u;
            RhiBindGroup cbg = rhi_create_bind_group(rhi, &cbgdesc);

            rhi_begin_compute_pass(rhi);
            rhi_set_compute_pipeline(rhi, cpipe);
            rhi_set_bind_group(rhi, 0u, cbg);
            rhi_dispatch(rhi, 1024u / 64u, 1u, 1u);
            rhi_end_compute_pass(rhi);

            g_cmp_results[idx].out_buffer_handle = b_out;
            g_cmp_results[idx].out_buffer_bytes  = 1024u * 4u;
        } else if (cmp_str_eq(emb->name, "skinning.mtk")) {
            enum { N_SKIN = 512u, N_BONES = 32u };
            f32* in_pos     = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            f32* in_norm    = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            u32* in_joints  = ARENA_PUSH_ARRAY(&g_cmp_arena, u32, N_SKIN);
            f32* in_weights = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_SKIN * 4u);
            f32* bone_mats  = ARENA_PUSH_ARRAY(&g_cmp_arena, f32, N_BONES * 3u * 4u);

            Rng rng;
            rng_seed(&rng, 0x42424242ull);
            for (u32 i = 0u; i < N_BONES * 12u; i++) bone_mats[i] = rng_f32(&rng) * 0.5f;
            for (u32 b = 0u; b < N_BONES; b++) {
                bone_mats[b * 12u + 0u] += 1.0f;
                bone_mats[b * 12u + 5u] += 1.0f;
                bone_mats[b * 12u + 10u] += 1.0f;
            }
            for (u32 i = 0u; i < N_SKIN; i++) {
                in_pos[i * 4u + 0] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 1] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 2] = rng_f32(&rng) * 4.0f - 2.0f;
                in_pos[i * 4u + 3] = 1.0f;

                in_norm[i * 4u + 0] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 1] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 2] = rng_f32(&rng) * 2.0f - 1.0f;
                in_norm[i * 4u + 3] = 0.0f;

                u32 b0 = rng_u32(&rng) % N_BONES;
                u32 b1 = rng_u32(&rng) % N_BONES;
                u32 b2 = rng_u32(&rng) % N_BONES;
                u32 b3 = rng_u32(&rng) % N_BONES;
                in_joints[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

                f32 w0 = rng_f32(&rng) + 0.1f;
                f32 w1 = rng_f32(&rng) + 0.1f;
                f32 w2 = rng_f32(&rng) + 0.1f;
                f32 w3 = rng_f32(&rng) + 0.1f;
                f32 inv_w = 1.0f / (w0 + w1 + w2 + w3);
                in_weights[i * 4u + 0] = w0 * inv_w;
                in_weights[i * 4u + 1] = w1 * inv_w;
                in_weights[i * 4u + 2] = w2 * inv_w;
                in_weights[i * 4u + 3] = w3 * inv_w;
            }

            RhiBuffer b_p  = rhi_create_buffer(rhi, N_SKIN * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_n  = rhi_create_buffer(rhi, N_SKIN * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_j  = rhi_create_buffer(rhi, N_SKIN * 4u,  RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_w  = rhi_create_buffer(rhi, N_SKIN * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_bm = rhi_create_buffer(rhi, N_BONES * 48u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
            RhiBuffer b_op = rhi_create_buffer(rhi, N_SKIN * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST | RHI_BUF_COPY_SRC);
            RhiBuffer b_on = rhi_create_buffer(rhi, N_SKIN * 16u, RHI_BUF_STORAGE | RHI_BUF_COPY_DST);

            rhi_write_buffer(rhi, b_p,  0u, in_pos,     N_SKIN * 16u);
            rhi_write_buffer(rhi, b_n,  0u, in_norm,    N_SKIN * 16u);
            rhi_write_buffer(rhi, b_j,  0u, in_joints,  N_SKIN * 4u);
            rhi_write_buffer(rhi, b_w,  0u, in_weights, N_SKIN * 16u);
            rhi_write_buffer(rhi, b_bm, 0u, bone_mats,  N_BONES * 48u);

            cbgdesc.entries[0] = (RhiBindEntry){ .binding = 1u, .kind = RHI_BIND_BUFFER, .resource = b_p,  .size = N_SKIN * 16u };
            cbgdesc.entries[1] = (RhiBindEntry){ .binding = 2u, .kind = RHI_BIND_BUFFER, .resource = b_n,  .size = N_SKIN * 16u };
            cbgdesc.entries[2] = (RhiBindEntry){ .binding = 3u, .kind = RHI_BIND_BUFFER, .resource = b_j,  .size = N_SKIN * 4u };
            cbgdesc.entries[3] = (RhiBindEntry){ .binding = 4u, .kind = RHI_BIND_BUFFER, .resource = b_w,  .size = N_SKIN * 16u };
            cbgdesc.entries[4] = (RhiBindEntry){ .binding = 5u, .kind = RHI_BIND_BUFFER, .resource = b_bm, .size = N_BONES * 48u };
            cbgdesc.entries[5] = (RhiBindEntry){ .binding = 6u, .kind = RHI_BIND_BUFFER, .resource = b_op, .size = N_SKIN * 16u };
            cbgdesc.entries[6] = (RhiBindEntry){ .binding = 7u, .kind = RHI_BIND_BUFFER, .resource = b_on, .size = N_SKIN * 16u };
            cbgdesc.entry_count = 7u;
            RhiBindGroup cbg = rhi_create_bind_group(rhi, &cbgdesc);

            rhi_begin_compute_pass(rhi);
            rhi_set_compute_pipeline(rhi, cpipe);
            rhi_set_bind_group(rhi, 0u, cbg);
            rhi_dispatch(rhi, N_SKIN / 64u, 1u, 1u);
            rhi_end_compute_pass(rhi);

            g_cmp_results[idx].out_buffer_handle = b_op;
            g_cmp_results[idx].out_buffer_bytes  = N_SKIN * 16u;
        }
        rhi_present(rhi);
    }

    arena_pop_to(&g_cmp_arena, mark);
    return rhi_stream_bytes(rhi);
}

/* 3. Çıktı Karşılaştırma, Tolerans ve Fark Haritası */
EXPORT(engine_gpu_compare_verify) b32 engine_gpu_compare_verify(u32 idx) {
    if (idx >= g_embedded_shader_count) return FALSE;
    const EmbeddedFile* emb = &g_embedded_shaders[idx];
    b32 is_comp = engine_gpu_compare_is_compute(idx);

    GpuCompareResult* res = &g_cmp_results[idx];
    u32 saved_handle = res->out_buffer_handle;
    u32 saved_bytes  = res->out_buffer_bytes;
    __builtin_memset(res, 0, sizeof(*res));
    res->index             = idx;
    res->out_buffer_handle = saved_handle;
    res->out_buffer_bytes  = saved_bytes;
    cmp_str_copy(res->name, emb->name, sizeof(res->name));
    res->is_compute = is_comp;

    if (!is_comp) {
        res->width       = GPU_COMPARE_W;
        res->height      = GPU_COMPARE_H;
        res->total_count = GPU_COMPARE_PIXELS;
        res->tolerance   = (cmp_str_eq(emb->name, "plasma.mtk") || cmp_str_eq(emb->name, "ocean.mtk"))
                           ? (3.0f / 255.0f) : (2.0f / 255.0f);

        f32 sum_diff = 0.0f;
        f32 max_diff = 0.0f;
        u32 exceeded = 0u;

        for (u32 p = 0u; p < GPU_COMPARE_PIXELS; p++) {
            u32 c_r = g_cmp_cpu_buf[p * 4u + 0u];
            u32 c_g = g_cmp_cpu_buf[p * 4u + 1u];
            u32 c_b = g_cmp_cpu_buf[p * 4u + 2u];
            u32 c_a = g_cmp_cpu_buf[p * 4u + 3u];

            u32 g_r = g_cmp_gpu_buf[p * 4u + 0u];
            u32 g_g = g_cmp_gpu_buf[p * 4u + 1u];
            u32 g_b = g_cmp_gpu_buf[p * 4u + 2u];
            u32 g_a = g_cmp_gpu_buf[p * 4u + 3u];

            i32 dr = (i32)c_r - (i32)g_r; if (dr < 0) dr = -dr;
            i32 dg = (i32)c_g - (i32)g_g; if (dg < 0) dg = -dg;
            i32 db = (i32)c_b - (i32)g_b; if (db < 0) db = -db;
            i32 da = (i32)c_a - (i32)g_a; if (da < 0) da = -da;

            i32 max_ch = MAX(MAX(dr, dg), MAX(db, da));
            f32 diff_f = (f32)max_ch / 255.0f;
            sum_diff += diff_f;
            if (diff_f > max_diff) max_diff = diff_f;
            if (diff_f > res->tolerance) exceeded++;

            /* Fark haritası renklendirme */
            if (max_ch == 0) {
                /* Tam eşleşme: koyu lacivert */
                g_cmp_diff_map[p * 4u + 0u] = 5u;
                g_cmp_diff_map[p * 4u + 1u] = 12u;
                g_cmp_diff_map[p * 4u + 2u] = 25u;
                g_cmp_diff_map[p * 4u + 3u] = 255u;
            } else if (diff_f <= res->tolerance) {
                /* Tolerans içi fark: yeşil ton */
                g_cmp_diff_map[p * 4u + 0u] = 10u;
                g_cmp_diff_map[p * 4u + 1u] = (u8)MIN(60 + max_ch * 70, 255);
                g_cmp_diff_map[p * 4u + 2u] = 30u;
                g_cmp_diff_map[p * 4u + 3u] = 255u;
            } else {
                /* Tolerans aşımı: parlak kırmızı */
                g_cmp_diff_map[p * 4u + 0u] = 255u;
                g_cmp_diff_map[p * 4u + 1u] = 20u;
                g_cmp_diff_map[p * 4u + 2u] = 20u;
                g_cmp_diff_map[p * 4u + 3u] = 255u;
            }
        }

        res->max_diff       = max_diff;
        res->mean_diff      = sum_diff / (f32)GPU_COMPARE_PIXELS;
        res->exceeded_count = exceeded;
        res->passed         = (exceeded == 0u);
    } else {
        /* Compute Shader Doğrulama */
        u32 elem_cnt = (idx == 4u) ? 512u : 1024u;
        res->width       = elem_cnt;
        res->height      = 1u;
        res->total_count = elem_cnt;

        f32 sum_diff = 0.0f;
        f32 max_diff = 0.0f;
        u32 exceeded = 0u;

        b32 is_uint = (cmp_str_eq(emb->name, "prefix_sum_scan.mtk") ||
                       cmp_str_eq(emb->name, "prefix_sum_add.mtk"));
        res->tolerance = is_uint ? 0.0f : 1e-4f;

        if (is_uint) {
            const u32* c_ptr = (const u32*)g_cmp_cpu_buf;
            const u32* g_ptr = (const u32*)g_cmp_gpu_buf;
            for (u32 i = 0u; i < elem_cnt; i++) {
                u32 cv = c_ptr[i];
                u32 gv = g_ptr[i];
                f32 d = (cv == gv) ? 0.0f : (f32)((cv > gv) ? (cv - gv) : (gv - cv));
                sum_diff += d;
                if (d > max_diff) max_diff = d;
                if (cv != gv) exceeded++;
            }
        } else {
            /* float4 tamponları (particle: pos, skinning: out_pos) */
            const f32* c_ptr = (const f32*)g_cmp_cpu_buf;
            const f32* g_ptr = (const f32*)g_cmp_gpu_buf;
            for (u32 i = 0u; i < elem_cnt * 4u; i++) {
                f32 d = c_ptr[i] - g_ptr[i];
                if (d < 0.0f) d = -d;
                sum_diff += d;
                if (d > max_diff) max_diff = d;
                if (d > res->tolerance) exceeded++;
            }
            sum_diff /= 4.0f;
        }

        res->max_diff       = max_diff;
        res->mean_diff      = sum_diff / (f32)elem_cnt;
        res->exceeded_count = exceeded;
        res->passed         = (exceeded == 0u);

        /* Compute çıktısını 128x128 ızgara olarak fark haritasına çiz */
        __builtin_memset(g_cmp_diff_map, 0, GPU_COMPARE_BYTES);
        u32 blk_w = GPU_COMPARE_W / 32u; /* 4 px */
        u32 blk_h = GPU_COMPARE_H / 32u; /* 4 px */
        for (u32 i = 0u; i < MIN(elem_cnt, 1024u); i++) {
            u32 gx = i % 32u;
            u32 gy = i / 32u;
            b32 has_err = FALSE;
            if (is_uint) {
                has_err = (((const u32*)g_cmp_cpu_buf)[i] != ((const u32*)g_cmp_gpu_buf)[i]);
            } else {
                for (u32 c = 0u; c < 4u; c++) {
                    f32 d = ((const f32*)g_cmp_cpu_buf)[i * 4u + c] - ((const f32*)g_cmp_gpu_buf)[i * 4u + c];
                    if (d < 0.0f) d = -d;
                    if (d > res->tolerance) has_err = TRUE;
                }
            }
            u8 cr = has_err ? 255u : 15u;
            u8 cg = has_err ? 20u : 180u;
            u8 cb = has_err ? 20u : 40u;
            for (u32 by = 0u; by < blk_h; by++) {
                for (u32 bx = 0u; bx < blk_w; bx++) {
                    u32 px = gx * blk_w + bx;
                    u32 py = gy * blk_h + by;
                    u32 p = py * GPU_COMPARE_W + px;
                    g_cmp_diff_map[p * 4u + 0u] = cr;
                    g_cmp_diff_map[p * 4u + 1u] = cg;
                    g_cmp_diff_map[p * 4u + 2u] = cb;
                    g_cmp_diff_map[p * 4u + 3u] = 255u;
                }
            }
        }
    }

    return res->passed;
}
