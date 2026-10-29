/* gpu/gpu_test.c — Bağımsız WebGPU RHI demosu: dönen renkli üçgen + dokulu, derinlik testli 3D küp.
 * main.c'ye dokunmadan engine_gpu_test_frame() dışa aktarımını sağlar (M15_B, D-108).
 * İlk karede WGSL shader'larını, buffer/doku/sampler/pipeline/bind_group kaynaklarını oluşturur;
 * her karede dönüş matrislerini güncelleyip tek geçişte hem üçgeni hem küpü çizer.
 */
#include "gpu/rhi.h"
#include "core/math.h"
#include "core/log.h"
#include "gfx3d/mesh.h"
#include "gfx3d/pipeline.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
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
