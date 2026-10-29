/* gpu/gpu_scene.c — GPU'da yaşayan 3D sahne, kirli liste (dirty list), compute frustum + Hi-Z
 * occlusion culling, dolaylı çizim (drawIndexedIndirect), 3-kademeli CSM gölge haritası ve
 * 3D kümelenmiş ileri aydınlatma (clustered forward lighting) + PBR WebGPU arka ucu (G1_B / H5).
 * Nesne dönüşümleri ve materyal tablosu GPU storage buffer'da kalıcıdır; CPU kare başına yalnız
 * değişen yuvaları kirli liste üzerinden yükler ve tek dolaylı çizim komutuyla sahneyi basar.
 */
#include "gpu/gpu_scene.h"
#include "core/arena.h"
#include "core/embed.h"
#include "core/log.h"
#include "matoki/matoki.h"
#include "matoki/wgsl.h"
#include "plat/platform.h"

/* --- Sabitler ve Kapasiteler --- */

#define SCENE_RHI_MAX_RESOURCES 512u
#define SCENE_RHI_STREAM_CAP    MB(2)
#define SCENE_ARENA_CAP         MB(3)

#define SCENE_STATIC_VB_CAP     32768u
#define SCENE_DYNAMIC_VB_CAP    16384u
#define SCENE_TOTAL_VB_CAP      (SCENE_STATIC_VB_CAP + SCENE_DYNAMIC_VB_CAP)

#define SCENE_STATIC_IB_CAP     98304u
#define SCENE_DYNAMIC_IB_CAP    49152u
#define SCENE_TOTAL_IB_CAP      (SCENE_STATIC_IB_CAP + SCENE_DYNAMIC_IB_CAP)

#define SCENE_ATLAS_TILE_SIZE   128u
#define SCENE_ATLAS_GRID        4u
#define SCENE_ATLAS_DIM         (SCENE_ATLAS_TILE_SIZE * SCENE_ATLAS_GRID) /* 512x512 */
#define SCENE_ATLAS_MIPS        7u                                         /* 128 -> 64 -> 32 -> 16 -> 8 -> 4 -> 2 */

#define SCENE_AUX_VEC4_COUNT    1024u /* [0..255]=mesh_bounds, [256..511]=batch_meta0, [512..767]=batch_meta1, [768..895]=lights, [1023]=mb_100k */
#define SCENE_INDIRECT_U32S     (GPU_SCENE_MAX_BATCHES * 5u + 4u)
#define SCENE_VISIBLE_CAP       524288u

typedef struct {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
    u32 rgba;
    u32 batch_base;
} GpuSceneVertex;

_Static_assert(sizeof(GpuSceneVertex) == 40, "GpuSceneVertex 40 bayt olmali");

typedef struct {
    f32 view[16];
    f32 proj[16];
    f32 view_proj[16];
    f32 vm_view_proj[16];
    f32 csm_vp[3][16];
    f32 csm_tex[3][16];
    f32 frustum_planes[6][4];
    f32 eye_near[4];
    f32 fwd_far[4];
    f32 sun_dir_pad[4];
    f32 sun_color_pad[4];
    f32 sky_color_pad[4];
    f32 gnd_color_pad[4];
    f32 fog_color_start[4];
    f32 fog_params[4];
    f32 viewport_retro[4];
    f32 csm_radii_scan[4];
    u32 counts[4];
    f32 occluder_box[4];
    f32 occluder_depth[4];
} GpuSceneUniforms;

_Static_assert(sizeof(GpuSceneUniforms) == 944, "GpuSceneUniforms 944 bayt olmali");

typedef struct {
    const Mesh* src_mesh;
    u32         vertex_count;
    u32         index_count;
    u32         base_vertex;
    u32         first_index;
    u32         batch_id;
    u32         batch_first_inst;
    u32         batch_max_inst;
    Vec3        local_center;
    f32         local_radius;
    b32         is_dynamic;
    b32         is_alpha;
} GpuMeshSlot;

typedef struct {
    const u32* src_px;
    u32        src_w;
    u32        src_h;
    u32        slot_id;
} GpuTexSlot;

/* --- Gömülü WGSL Compute Shader (Frustum + Hi-Z Culling + Indirect Args + Clustered Lights) --- */

static const char k_scene_compute_wgsl[] =
    "struct SceneUniforms {\n"
    "    view: mat4x4<f32>,\n"
    "    proj: mat4x4<f32>,\n"
    "    view_proj: mat4x4<f32>,\n"
    "    vm_view_proj: mat4x4<f32>,\n"
    "    csm_vp0: mat4x4<f32>,\n"
    "    csm_vp1: mat4x4<f32>,\n"
    "    csm_vp2: mat4x4<f32>,\n"
    "    csm_tex0: mat4x4<f32>,\n"
    "    csm_tex1: mat4x4<f32>,\n"
    "    csm_tex2: mat4x4<f32>,\n"
    "    frustum_planes: array<vec4<f32>, 6>,\n"
    "    eye_near: vec4<f32>,\n"
    "    fwd_far: vec4<f32>,\n"
    "    sun_dir_pad: vec4<f32>,\n"
    "    sun_color_pad: vec4<f32>,\n"
    "    sky_color_pad: vec4<f32>,\n"
    "    gnd_color_pad: vec4<f32>,\n"
    "    fog_color_start: vec4<f32>,\n"
    "    fog_params: vec4<f32>,\n"
    "    viewport_retro: vec4<f32>,\n"
    "    csm_radii_scan: vec4<f32>,\n"
    "    counts: vec4<u32>,\n"
    "    occluder_box: vec4<f32>,\n"
    "    occluder_depth: vec4<f32>,\n"
    "};\n\n"
    "struct GpuInstance {\n"
    "    r0: vec4<f32>,\n"
    "    r1: vec4<f32>,\n"
    "    r2: vec4<f32>,\n"
    "    mdata: vec4<u32>,\n"
    "};\n\n"
    "@group(0) @binding(0) var<uniform> scene_u: SceneUniforms;\n"
    "@group(0) @binding(1) var<storage, read_write> instances: array<GpuInstance>;\n"
    "@group(0) @binding(2) var<storage, read_write> indirect_args: array<atomic<u32>>;\n"
    "@group(0) @binding(3) var<storage, read_write> visible_instances: array<u32>;\n"
    "@group(0) @binding(4) var<storage, read_write> hiz_pyramid: array<f32>;\n"
    "@group(0) @binding(5) var<storage, read_write> cluster_buf: array<u32>;\n"
    "@group(0) @binding(6) var<storage, read_write> aux_buf: array<vec4<f32>>;\n\n"
    "fn touch_all_compute() {\n"
    "    _ = scene_u.counts.x;\n"
    "    _ = &instances;\n"
    "    _ = &indirect_args;\n"
    "    _ = &visible_instances;\n"
    "    _ = &hiz_pyramid;\n"
    "    _ = &cluster_buf;\n"
    "    _ = &aux_buf;\n"
    "}\n\n"
    "fn hiz_mip_offset(mip: u32) -> u32 {\n"
    "    switch (mip) {\n"
    "        case 0u: { return 0u; }\n"
    "        case 1u: { return 8192u; }\n"
    "        case 2u: { return 10240u; }\n"
    "        case 3u: { return 10752u; }\n"
    "        case 4u: { return 10880u; }\n"
    "        case 5u: { return 10912u; }\n"
    "        case 6u: { return 10920u; }\n"
    "        default: { return 10922u; }\n"
    "    }\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_init_100k(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let idx: u32 = gid.x;\n"
    "    if (idx >= 100000u) { return; }\n"
    "    let dst_slot: u32 = 2400u + idx;\n"
    "    let col: u32 = idx % 400u;\n"
    "    let row: u32 = idx / 400u;\n"
    "    let fx: f32 = (f32(col) - 199.5) * 1.45;\n"
    "    let fz: f32 = (f32(row) - 124.5) * 1.45 - 18.0;\n"
    "    let dist2: f32 = fx * fx + fz * fz;\n"
    "    var off_x: f32 = fx;\n"
    "    var off_z: f32 = fz;\n"
    "    if (dist2 < 64.0) {\n"
    "        off_z = off_z - 14.0;\n"
    "    }\n"
    "    let h_hash: u32 = (idx * 747796405u + 2891336453u);\n"
    "    let h_norm: f32 = f32(h_hash & 1023u) * (1.0 / 1023.0);\n"
    "    let sy: f32 = 0.35 + h_norm * 1.15;\n"
    "    let sx: f32 = 0.32;\n"
    "    let sz: f32 = 0.32;\n"
    "    let py: f32 = sy * 0.5;\n"
    "    let r_u8: u32 = 90u + ((h_hash >> 10u) & 155u);\n"
    "    let g_u8: u32 = 80u + ((h_hash >> 18u) & 155u);\n"
    "    let b_u8: u32 = 95u + ((h_hash >> 24u) & 155u);\n"
    "    let color: u32 = r_u8 | (g_u8 << 8u) | (b_u8 << 16u) | 0xFF000000u;\n"
    "    let metal_u8: u32 = select(20u, 220u, (idx & 3u) == 0u);\n"
    "    let rough_u8: u32 = 45u + ((idx * 17u) & 160u);\n"
    "    let tex_id: u32 = 1u + (idx & 3u);\n"
    "    let flags: u32 = 5u; /* ACTIVE(1) | RECV_SHADOW(4), PBR(0) */\n"
    "    let mat_tex: u32 = metal_u8 | (rough_u8 << 8u) | (tex_id << 16u) | (flags << 24u);\n"
    "    let bm0 = bitcast<vec4<u32>>(aux_buf[1023u]);\n"
    "    let mesh_batch: u32 = bm0.x;\n"
    "    let rad: f32 = sy * 0.87;\n"
    "    var inst: GpuInstance;\n"
    "    inst.r0 = vec4<f32>(sx, 0.0, 0.0, off_x);\n"
    "    inst.r1 = vec4<f32>(0.0, sy, 0.0, py);\n"
    "    inst.r2 = vec4<f32>(0.0, 0.0, sz, off_z);\n"
    "    inst.mdata = vec4<u32>(color, mat_tex, mesh_batch, bitcast<u32>(rad));\n"
    "    instances[dst_slot] = inst;\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_reset_hiz(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let i: u32 = gid.x;\n"
    "    if (i < 256u) {\n"
    "        let bm0 = bitcast<vec4<u32>>(aux_buf[256u + i]);\n"
    "        let bm1 = bitcast<vec4<u32>>(aux_buf[512u + i]);\n"
    "        let base = i * 5u;\n"
    "        atomicStore(&indirect_args[base + 0u], bm0.z);\n"
    "        atomicStore(&indirect_args[base + 1u], 0u);\n"
    "        atomicStore(&indirect_args[base + 2u], bm0.w);\n"
    "        atomicStore(&indirect_args[base + 3u], bm1.x);\n"
    "        atomicStore(&indirect_args[base + 4u], 0u);\n"
    "    }\n"
    "    if (i < 4u) {\n"
    "        atomicStore(&indirect_args[1280u + i], 0u);\n"
    "    }\n"
    "    if (i < 8192u) {\n"
    "        let x: u32 = i & 127u;\n"
    "        let y: u32 = i >> 7u;\n"
    "        let u: f32 = (f32(x) + 0.5) * (1.0 / 128.0);\n"
    "        let v: f32 = (f32(y) + 0.5) * (1.0 / 64.0);\n"
    "        var z: f32 = 0.0;\n"
    "        let box_u0: f32 = scene_u.occluder_box.x;\n"
    "        let box_v0: f32 = scene_u.occluder_box.y;\n"
    "        let box_u1: f32 = scene_u.occluder_box.z;\n"
    "        let box_v1: f32 = scene_u.occluder_box.w;\n"
    "        if (u >= box_u0 && u <= box_u1 && v >= box_v0 && v <= box_v1) {\n"
    "            z = scene_u.occluder_depth.x;\n"
    "        }\n"
    "        hiz_pyramid[i] = z;\n"
    "    }\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_hiz_down_a(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let id: u32 = gid.x;\n"
    "    if (id < 2048u) {\n"
    "        let x: u32 = id & 63u;\n"
    "        let y: u32 = id >> 6u;\n"
    "        let sx: u32 = x * 2u;\n"
    "        let sy: u32 = y * 2u;\n"
    "        let z00: f32 = hiz_pyramid[sy * 128u + sx];\n"
    "        let z10: f32 = hiz_pyramid[sy * 128u + sx + 1u];\n"
    "        let z01: f32 = hiz_pyramid[(sy + 1u) * 128u + sx];\n"
    "        let z11: f32 = hiz_pyramid[(sy + 1u) * 128u + sx + 1u];\n"
    "        hiz_pyramid[8192u + id] = min(min(z00, z10), min(z01, z11));\n"
    "    } else if (id < 2560u) {\n"
    "        let m2_id: u32 = id - 2048u;\n"
    "        let x: u32 = m2_id & 31u;\n"
    "        let y: u32 = m2_id >> 5u;\n"
    "        let sx: u32 = x * 4u;\n"
    "        let sy: u32 = y * 4u;\n"
    "        var mz: f32 = 1.0;\n"
    "        for (var dy: u32 = 0u; dy < 4u; dy = dy + 1u) {\n"
    "            for (var dx: u32 = 0u; dx < 4u; dx = dx + 1u) {\n"
    "                mz = min(mz, hiz_pyramid[(sy + dy) * 128u + (sx + dx)]);\n"
    "            }\n"
    "        }\n"
    "        hiz_pyramid[10240u + m2_id] = mz;\n"
    "    }\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_hiz_down_b(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let id: u32 = gid.x;\n"
    "    if (id < 128u) {\n"
    "        let x: u32 = id & 15u;\n"
    "        let y: u32 = id >> 4u;\n"
    "        let sx: u32 = x * 2u;\n"
    "        let sy: u32 = y * 2u;\n"
    "        let base: u32 = 10240u;\n"
    "        let z00: f32 = hiz_pyramid[base + sy * 32u + sx];\n"
    "        let z10: f32 = hiz_pyramid[base + sy * 32u + sx + 1u];\n"
    "        let z01: f32 = hiz_pyramid[base + (sy + 1u) * 32u + sx];\n"
    "        let z11: f32 = hiz_pyramid[base + (sy + 1u) * 32u + sx + 1u];\n"
    "        hiz_pyramid[10752u + id] = min(min(z00, z10), min(z01, z11));\n"
    "    } else if (id < 160u) {\n"
    "        let m4_id: u32 = id - 128u;\n"
    "        let x: u32 = m4_id & 7u;\n"
    "        let y: u32 = m4_id >> 3u;\n"
    "        let sx: u32 = x * 4u;\n"
    "        let sy: u32 = y * 4u;\n"
    "        var mz: f32 = 1.0;\n"
    "        for (var dy: u32 = 0u; dy < 4u; dy = dy + 1u) {\n"
    "            for (var dx: u32 = 0u; dx < 4u; dx = dx + 1u) {\n"
    "                mz = min(mz, hiz_pyramid[10240u + (sy + dy) * 32u + (sx + dx)]);\n"
    "            }\n"
    "        }\n"
    "        hiz_pyramid[10880u + m4_id] = mz;\n"
    "    } else if (id == 160u) {\n"
    "        hiz_pyramid[10912u] = 0.0;\n"
    "        hiz_pyramid[10920u] = 0.0;\n"
    "        hiz_pyramid[10922u] = 0.0;\n"
    "    }\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_cull_inst(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let inst_id: u32 = gid.x;\n"
    "    let total_inst: u32 = scene_u.counts.x;\n"
    "    if (inst_id >= total_inst) { return; }\n"
    "    let inst: GpuInstance = instances[inst_id];\n"
    "    let flags: u32 = (inst.mdata.y >> 24u) & 0x1Fu;\n"
    "    let shade_mode: u32 = (inst.mdata.y >> 29u) & 0x7u;\n"
    "    if ((flags & 1u) == 0u) { return; }\n"
    "    let mesh_id: u32 = inst.mdata.z & 0xFFFFu;\n"
    "    let batch_id: u32 = (inst.mdata.z >> 16u) & 0xFFFFu;\n"
    "    let rad: f32 = bitcast<f32>(inst.mdata.w);\n"
    "    let mb: vec4<f32> = aux_buf[mesh_id & 255u];\n"
    "    let lc: vec4<f32> = vec4<f32>(mb.xyz, 1.0);\n"
    "    let wc: vec3<f32> = vec3<f32>(dot(inst.r0, lc), dot(inst.r1, lc), dot(inst.r2, lc));\n\n"
    "    /* Viewmodel her zaman görünür */\n"
    "    if ((flags & 8u) == 0u) {\n"
    "        /* 1. 6-düzlemli Frustum Culling */\n"
    "        for (var p: u32 = 0u; p < 6u; p = p + 1u) {\n"
    "            let pl: vec4<f32> = scene_u.frustum_planes[p];\n"
    "            if (dot(pl.xyz, wc) + pl.w < -rad) {\n"
    "                _ = atomicAdd(&indirect_args[1281u], 1u);\n"
    "                return;\n"
    "            }\n"
    "        }\n\n"
    "        /* 2. Hi-Z Occlusion Culling (reverse-Z piramidi) */\n"
    "        if (scene_u.occluder_depth.w > 0.5 && shade_mode != 2u) {\n"
    "            let vpos: vec4<f32> = scene_u.view * vec4<f32>(wc, 1.0);\n"
    "            let dist: f32 = -vpos.z;\n"
    "            let near_z: f32 = scene_u.eye_near.w;\n"
    "            if (dist > near_z + rad * 1.25) {\n"
    "                let z_near_v: f32 = max(dist - rad, near_z);\n"
    "                let sphere_max_z: f32 = clamp(near_z / z_near_v, 0.0, 1.0);\n"
    "                let cot_fov: f32 = scene_u.fog_params.z;\n"
    "                let aspect: f32 = scene_u.fog_params.w;\n"
    "                let proj_r_y: f32 = (rad * cot_fov) / dist;\n"
    "                let proj_r_x: f32 = proj_r_y / aspect;\n"
    "                let ndc_x: f32 = (vpos.x * cot_fov / aspect) / dist;\n"
    "                let ndc_y: f32 = (vpos.y * cot_fov) / dist;\n"
    "                let u_min: f32 = clamp((ndc_x - proj_r_x) * 0.5 + 0.5, 0.0, 0.999);\n"
    "                let u_max: f32 = clamp((ndc_x + proj_r_x) * 0.5 + 0.5, 0.0, 0.999);\n"
    "                let v_min: f32 = clamp(0.5 - (ndc_y + proj_r_y) * 0.5, 0.0, 0.999);\n"
    "                let v_max: f32 = clamp(0.5 - (ndc_y - proj_r_y) * 0.5, 0.0, 0.999);\n"
    "                let fp_w: f32 = (u_max - u_min) * 128.0;\n"
    "                let fp_h: f32 = (v_max - v_min) * 64.0;\n"
    "                let fp: f32 = max(max(fp_w, fp_h), 1.0);\n"
    "                let mip: u32 = min(u32(ceil(log2(fp))), 4u);\n"
    "                let mw: u32 = 128u >> mip;\n"
    "                let mh: u32 = 64u >> mip;\n"
    "                let x0: u32 = min(u32(u_min * f32(mw)), mw - 1u);\n"
    "                let x1: u32 = min(u32(u_max * f32(mw)), mw - 1u);\n"
    "                let y0: u32 = min(u32(v_min * f32(mh)), mh - 1u);\n"
    "                let y1: u32 = min(u32(v_max * f32(mh)), mh - 1u);\n"
    "                let m_off: u32 = hiz_mip_offset(mip);\n"
    "                let hz00: f32 = hiz_pyramid[m_off + y0 * mw + x0];\n"
    "                let hz10: f32 = hiz_pyramid[m_off + y0 * mw + x1];\n"
    "                let hz01: f32 = hiz_pyramid[m_off + y1 * mw + x0];\n"
    "                let hz11: f32 = hiz_pyramid[m_off + y1 * mw + x1];\n"
    "                let occ_z: f32 = min(min(hz00, hz10), min(hz01, hz11));\n"
    "                if (sphere_max_z < occ_z - 0.0002) {\n"
    "                    _ = atomicAdd(&indirect_args[1282u], 1u);\n"
    "                    return;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n\n"
    "    /* 3. Görünür örneği dolaylı çizim listesine atomik ekle */\n"
    "    let bm0 = bitcast<vec4<u32>>(aux_buf[256u + (batch_id & 255u)]);\n"
    "    let slot: u32 = atomicAdd(&indirect_args[(batch_id & 255u) * 5u + 1u], 1u);\n"
    "    if (slot < bm0.y) {\n"
    "        visible_instances[bm0.x + slot] = inst_id;\n"
    "    }\n"
    "    _ = atomicAdd(&indirect_args[1280u], 1u);\n"
    "}\n\n"
    "@compute @workgroup_size(64)\n"
    "fn cs_cluster_lt(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    touch_all_compute();\n"
    "    let cid: u32 = gid.x;\n"
    "    if (cid >= 1024u) { return; }\n"
    "    let cx: u32 = cid & 15u;\n"
    "    let cy: u32 = (cid >> 4u) & 7u;\n"
    "    let cz: u32 = cid >> 7u;\n"
    "    let near_z: f32 = scene_u.eye_near.w;\n"
    "    let far_z: f32 = scene_u.fwd_far.w;\n"
    "    let ratio: f32 = max(far_z / near_z, 2.0);\n"
    "    let z0: f32 = near_z * pow(ratio, f32(cz) / 8.0);\n"
    "    let z1: f32 = near_z * pow(ratio, f32(cz + 1u) / 8.0);\n"
    "    let cot_fov: f32 = scene_u.fog_params.z;\n"
    "    let aspect: f32 = scene_u.fog_params.w;\n"
    "    let tan_y: f32 = 1.0 / max(cot_fov, 0.1);\n"
    "    let tan_x: f32 = tan_y * aspect;\n"
    "    let nx0: f32 = (f32(cx) / 16.0) * 2.0 - 1.0;\n"
    "    let nx1: f32 = (f32(cx + 1u) / 16.0) * 2.0 - 1.0;\n"
    "    let ny0: f32 = 1.0 - (f32(cy + 1u) / 8.0) * 2.0;\n"
    "    let ny1: f32 = 1.0 - (f32(cy) / 8.0) * 2.0;\n"
    "    let min_x: f32 = min(nx0 * z0, nx0 * z1) * tan_x;\n"
    "    let max_x: f32 = max(nx1 * z0, nx1 * z1) * tan_x;\n"
    "    let min_y: f32 = min(ny0 * z0, ny0 * z1) * tan_y;\n"
    "    let max_y: f32 = max(ny1 * z0, ny1 * z1) * tan_y;\n"
    "    let min_z: f32 = -z1;\n"
    "    let max_z: f32 = -z0;\n"
    "    let lcount: u32 = min(scene_u.counts.z, 64u);\n"
    "    let base_off: u32 = cid * 16u;\n"
    "    var found: u32 = 0u;\n"
    "    for (var l: u32 = 0u; l < lcount; l = l + 1u) {\n"
    "        let lp: vec4<f32> = aux_buf[768u + l * 2u];\n"
    "        let lv: vec3<f32> = (scene_u.view * vec4<f32>(lp.xyz, 1.0)).xyz;\n"
    "        let r: f32 = lp.w;\n"
    "        let cx_p: f32 = clamp(lv.x, min_x, max_x);\n"
    "        let cy_p: f32 = clamp(lv.y, min_y, max_y);\n"
    "        let cz_p: f32 = clamp(lv.z, min_z, max_z);\n"
    "        let dx: f32 = lv.x - cx_p;\n"
    "        let dy: f32 = lv.y - cy_p;\n"
    "        let dz: f32 = lv.z - cz_p;\n"
    "        if (dx * dx + dy * dy + dz * dz <= r * r && found < 15u) {\n"
    "            found = found + 1u;\n"
    "            cluster_buf[base_off + found] = l;\n"
    "        }\n"
    "    }\n"
    "    cluster_buf[base_off] = found;\n"
    "}\n";

/* --- Gömülü WGSL Render Shader (3-Cascade CSM Shadow + Clustered PBR/Lit/Unlit/Dungeon/Particle) --- */

static const char k_scene_render_wgsl[] =
    "struct SceneUniforms {\n"
    "    view: mat4x4<f32>,\n"
    "    proj: mat4x4<f32>,\n"
    "    view_proj: mat4x4<f32>,\n"
    "    vm_view_proj: mat4x4<f32>,\n"
    "    csm_vp0: mat4x4<f32>,\n"
    "    csm_vp1: mat4x4<f32>,\n"
    "    csm_vp2: mat4x4<f32>,\n"
    "    csm_tex0: mat4x4<f32>,\n"
    "    csm_tex1: mat4x4<f32>,\n"
    "    csm_tex2: mat4x4<f32>,\n"
    "    frustum_planes: array<vec4<f32>, 6>,\n"
    "    eye_near: vec4<f32>,\n"
    "    fwd_far: vec4<f32>,\n"
    "    sun_dir_pad: vec4<f32>,\n"
    "    sun_color_pad: vec4<f32>,\n"
    "    sky_color_pad: vec4<f32>,\n"
    "    gnd_color_pad: vec4<f32>,\n"
    "    fog_color_start: vec4<f32>,\n"
    "    fog_params: vec4<f32>,\n"
    "    viewport_retro: vec4<f32>,\n"
    "    csm_radii_scan: vec4<f32>,\n"
    "    counts: vec4<u32>,\n"
    "    occluder_box: vec4<f32>,\n"
    "    occluder_depth: vec4<f32>,\n"
    "};\n\n"
    "struct GpuInstance {\n"
    "    r0: vec4<f32>,\n"
    "    r1: vec4<f32>,\n"
    "    r2: vec4<f32>,\n"
    "    mdata: vec4<u32>,\n"
    "};\n\n"
    "@group(0) @binding(0) var<uniform> scene_u: SceneUniforms;\n"
    "@group(0) @binding(1) var<storage, read> instances: array<GpuInstance>;\n"
    "@group(0) @binding(2) var<storage, read> visible_instances: array<u32>;\n"
    "@group(0) @binding(3) var<storage, read> cluster_buf: array<u32>;\n"
    "@group(0) @binding(4) var<storage, read> aux_buf: array<vec4<f32>>;\n"
    "@group(0) @binding(5) var albedo_tex: texture_2d<f32>;\n"
    "@group(0) @binding(6) var shadow_tex: texture_2d<f32>;\n"
    "@group(0) @binding(7) var scene_samp: sampler;\n\n"
    "struct VsIn {\n"
    "    @location(0) pos: vec3<f32>,\n"
    "    @location(1) normal: vec3<f32>,\n"
    "    @location(2) uv: vec2<f32>,\n"
    "    @location(3) color: vec4<f32>,\n"
    "    @location(4) batch_base: u32,\n"
    "};\n\n"
    "struct ShadowVsOut {\n"
    "    @builtin(position) clip_pos: vec4<f32>,\n"
    "};\n\n"
    "fn vs_shadow_impl(vin: VsIn, iid: u32, cascade: u32) -> ShadowVsOut {\n"
    "    var out: ShadowVsOut;\n"
    "    let inst_id: u32 = visible_instances[vin.batch_base + iid];\n"
    "    let inst: GpuInstance = instances[inst_id];\n"
    "    let flags: u32 = (inst.mdata.y >> 24u) & 0x1Fu;\n"
    "    if ((flags & 2u) == 0u) {\n"
    "        out.clip_pos = vec4<f32>(2.0, 2.0, 2.0, 1.0);\n"
    "        return out;\n"
    "    }\n"
    "    let p4: vec4<f32> = vec4<f32>(vin.pos, 1.0);\n"
    "    let wpos: vec4<f32> = vec4<f32>(dot(inst.r0, p4), dot(inst.r1, p4), dot(inst.r2, p4), 1.0);\n"
    "    if (cascade == 0u) {\n"
    "        out.clip_pos = scene_u.csm_vp0 * wpos;\n"
    "    } else if (cascade == 1u) {\n"
    "        out.clip_pos = scene_u.csm_vp1 * wpos;\n"
    "    } else {\n"
    "        out.clip_pos = scene_u.csm_vp2 * wpos;\n"
    "    }\n"
    "    return out;\n"
    "}\n\n"
    "@vertex fn vs_shadow_c0(vin: VsIn, @builtin(instance_index) iid: u32) -> ShadowVsOut {\n"
    "    return vs_shadow_impl(vin, iid, 0u);\n"
    "}\n"
    "@vertex fn vs_shadow_c1(vin: VsIn, @builtin(instance_index) iid: u32) -> ShadowVsOut {\n"
    "    return vs_shadow_impl(vin, iid, 1u);\n"
    "}\n"
    "@vertex fn vs_shadow_c2(vin: VsIn, @builtin(instance_index) iid: u32) -> ShadowVsOut {\n"
    "    return vs_shadow_impl(vin, iid, 2u);\n"
    "}\n\n"
    "@fragment fn fs_shadow(fin: ShadowVsOut) -> @location(0) vec4<f32> {\n"
    "    _ = scene_u.counts.x;\n"
    "    _ = &instances;\n"
    "    _ = &visible_instances;\n"
    "    let z_bits: u32 = bitcast<u32>(clamp(fin.clip_pos.z, 0.0, 1.0));\n"
    "    return unpack4x8unorm(z_bits);\n"
    "}\n\n"
    "struct SceneVsOut {\n"
    "    @builtin(position) clip_pos: vec4<f32>,\n"
    "    @location(0) uv: vec2<f32>,\n"
    "    @location(1) world_nrm: vec3<f32>,\n"
    "    @location(2) world_pos: vec3<f32>,\n"
    "    @location(3) tint: vec4<f32>,\n"
    "    @location(4) mat_info: vec4<f32>, /* x=metallic, y=roughness, z=view_depth, w=0.0 */\n"
    "    @location(5) @interpolate(flat) meta_y: u32,\n"
    "};\n\n"
    "@vertex fn vs_scene(vin: VsIn, @builtin(instance_index) iid: u32) -> SceneVsOut {\n"
    "    var out: SceneVsOut;\n"
    "    let inst_id: u32 = visible_instances[vin.batch_base + iid];\n"
    "    let inst: GpuInstance = instances[inst_id];\n"
    "    let p4: vec4<f32> = vec4<f32>(vin.pos, 1.0);\n"
    "    let wpos: vec3<f32> = vec3<f32>(dot(inst.r0, p4), dot(inst.r1, p4), dot(inst.r2, p4));\n"
    "    let wnrm: vec3<f32> = vec3<f32>(\n"
    "        dot(inst.r0.xyz, vin.normal),\n"
    "        dot(inst.r1.xyz, vin.normal),\n"
    "        dot(inst.r2.xyz, vin.normal)\n"
    "    );\n"
    "    let meta_y: u32 = inst.mdata.y;\n"
    "    let flags: u32 = (meta_y >> 24u) & 0x1Fu;\n"
    "    let shade_mode: u32 = (meta_y >> 29u) & 0x7u;\n"
    "    let rad: f32 = bitcast<f32>(inst.mdata.w);\n"
    "    var clip: vec4<f32>;\n"
    "    if ((flags & 8u) != 0u) {\n"
    "        clip = scene_u.vm_view_proj * vec4<f32>(wpos, 1.0);\n"
    "        if (clip.w > 0.0001) {\n"
    "            let z_ndc: f32 = clamp(clip.z / clip.w, 0.0, 1.0);\n"
    "            clip.z = (0.91 + z_ndc * 0.089) * clip.w;\n"
    "        }\n"
    "    } else {\n"
    "        clip = scene_u.view_proj * vec4<f32>(wpos, 1.0);\n"
    "        if (clip.w > 0.0001) {\n"
    "            if (shade_mode == 2u && rad > 40.0) {\n"
    "                clip.z = 0.0001 * clip.w;\n"
    "            } else {\n"
    "                let z_ndc: f32 = clamp(clip.z / clip.w, 0.0, 1.0);\n"
    "                clip.z = (z_ndc * 0.90) * clip.w;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    let snap_sub: f32 = scene_u.viewport_retro.w;\n"
    "    if (snap_sub > 0.5 && clip.w > 0.0001) {\n"
    "        let vpw: f32 = scene_u.viewport_retro.x;\n"
    "        let vph: f32 = scene_u.viewport_retro.y;\n"
    "        let sx: f32 = (clip.x / clip.w * 0.5 + 0.5) * vpw;\n"
    "        let sy: f32 = (0.5 - clip.y / clip.w * 0.5) * vph;\n"
    "        let gx: f32 = floor(sx / snap_sub + 0.5) * snap_sub;\n"
    "        let gy: f32 = floor(sy / snap_sub + 0.5) * snap_sub;\n"
    "        clip.x = ((gx / vpw) * 2.0 - 1.0) * clip.w;\n"
    "        clip.y = (1.0 - (gy / vph) * 2.0) * clip.w;\n"
    "    }\n"
    "    out.clip_pos = clip;\n"
    "    out.uv = vin.uv;\n"
    "    out.world_nrm = wnrm;\n"
    "    out.world_pos = wpos;\n"
    "    let inst_col: vec4<f32> = unpack4x8unorm(inst.mdata.x);\n"
    "    out.tint = inst_col * vin.color;\n"
    "    let metallic: f32 = f32(meta_y & 0xFFu) * (1.0 / 255.0);\n"
    "    let roughness: f32 = f32((meta_y >> 8u) & 0xFFu) * (1.0 / 255.0);\n"
    "    out.mat_info = vec4<f32>(metallic, roughness, max(clip.w, 0.001), 0.0);\n"
    "    out.meta_y = meta_y;\n"
    "    return out;\n"
    "}\n\n"
    "fn sample_atlas_slot(tex_id: u32, uv: vec2<f32>, dx: vec2<f32>, dy: vec2<f32>, mip_on: bool) -> vec4<f32> {\n"
    "    if (tex_id == 0u) {\n"
    "        return vec4<f32>(1.0, 1.0, 1.0, 1.0);\n"
    "    }\n"
    "    let dmax: f32 = max(dot(dx, dx), dot(dy, dy));\n"
    "    var mip: i32 = 0;\n"
    "    if (mip_on && dmax >= 1.0) {\n"
    "        mip = clamp(i32(0.5 * log2(dmax)), 0, 6);\n"
    "    }\n"
    "    let tile_sz: i32 = 128 >> u32(mip);\n"
    "    let mask: i32 = tile_sz - 1;\n"
    "    let slot_x: i32 = i32(tex_id & 3u) * tile_sz;\n"
    "    let slot_y: i32 = i32((tex_id >> 2u) & 3u) * tile_sz;\n"
    "    let fu: f32 = uv.x * f32(tile_sz) - 0.5;\n"
    "    let fv: f32 = uv.y * f32(tile_sz) - 0.5;\n"
    "    let u0: f32 = floor(fu);\n"
    "    let v0: f32 = floor(fv);\n"
    "    let ax: f32 = fu - u0;\n"
    "    let ay: f32 = fv - v0;\n"
    "    let ix0: i32 = i32(u0) & mask;\n"
    "    let iy0: i32 = i32(v0) & mask;\n"
    "    let ix1: i32 = (ix0 + 1) & mask;\n"
    "    let iy1: i32 = (iy0 + 1) & mask;\n"
    "    let c00: vec4<f32> = textureLoad(albedo_tex, vec2<i32>(slot_x + ix0, slot_y + iy0), mip);\n"
    "    let c10: vec4<f32> = textureLoad(albedo_tex, vec2<i32>(slot_x + ix1, slot_y + iy0), mip);\n"
    "    let c01: vec4<f32> = textureLoad(albedo_tex, vec2<i32>(slot_x + ix0, slot_y + iy1), mip);\n"
    "    let c11: vec4<f32> = textureLoad(albedo_tex, vec2<i32>(slot_x + ix1, slot_y + iy1), mip);\n"
    "    return mix(mix(c00, c10, ax), mix(c01, c11, ax), ay);\n"
    "}\n\n"
    "fn load_shadow_depth(px: i32, py: i32) -> f32 {\n"
    "    let cx: i32 = clamp(px, 0, 3071);\n"
    "    let cy: i32 = clamp(py, 0, 1023);\n"
    "    let rgba: vec4<f32> = textureLoad(shadow_tex, vec2<i32>(cx, cy), 0);\n"
    "    return bitcast<f32>(pack4x8unorm(rgba));\n"
    "}\n\n"
    "fn sample_csm_shadow(wpos: vec3<f32>, ndotl: f32) -> f32 {\n"
    "    let p0: vec3<f32> = (scene_u.csm_tex0 * vec4<f32>(wpos, 1.0)).xyz;\n"
    "    let p1: vec3<f32> = (scene_u.csm_tex1 * vec4<f32>(wpos, 1.0)).xyz;\n"
    "    let p2: vec3<f32> = (scene_u.csm_tex2 * vec4<f32>(wpos, 1.0)).xyz;\n"
    "    var spos: vec3<f32> = p2;\n"
    "    var cascade: i32 = 2;\n"
    "    var bias_scale: f32 = 0.0025;\n"
    "    if (p0.x > 0.03 && p0.x < 0.97 && p0.y > 0.03 && p0.y < 0.97 && p0.z > 0.01 && p0.z < 0.99) {\n"
    "        spos = p0;\n"
    "        cascade = 0;\n"
    "        bias_scale = 0.0012;\n"
    "    } else if (p1.x > 0.03 && p1.x < 0.97 && p1.y > 0.03 && p1.y < 0.97 && p1.z > 0.01 && p1.z < 0.99) {\n"
    "        spos = p1;\n"
    "        cascade = 1;\n"
    "        bias_scale = 0.0018;\n"
    "    } else if (!(p2.x > 0.02 && p2.x < 0.98 && p2.y > 0.02 && p2.y < 0.98 && p2.z > 0.01 && p2.z < 0.99)) {\n"
    "        return 1.0;\n"
    "    }\n"
    "    let sx: f32 = spos.x * 1024.0 - 0.5;\n"
    "    let sy: f32 = spos.y * 1024.0 - 0.5;\n"
    "    let fx: f32 = floor(sx);\n"
    "    let fy: f32 = floor(sy);\n"
    "    let ax: f32 = sx - fx;\n"
    "    let ay: f32 = sy - fy;\n"
    "    let base_x: i32 = cascade * 1024;\n"
    "    let ix0: i32 = base_x + clamp(i32(fx), 0, 1023);\n"
    "    let ix1: i32 = base_x + clamp(i32(fx) + 1, 0, 1023);\n"
    "    let iy0: i32 = clamp(i32(fy), 0, 1023);\n"
    "    let iy1: i32 = clamp(i32(fy) + 1, 0, 1023);\n"
    "    let bias: f32 = bias_scale * (1.5 - ndotl);\n"
    "    let ref_z: f32 = spos.z + bias;\n"
    "    let s00: f32 = select(1.0, 0.0, ref_z < load_shadow_depth(ix0, iy0));\n"
    "    let s10: f32 = select(1.0, 0.0, ref_z < load_shadow_depth(ix1, iy0));\n"
    "    let s01: f32 = select(1.0, 0.0, ref_z < load_shadow_depth(ix0, iy1));\n"
    "    let s11: f32 = select(1.0, 0.0, ref_z < load_shadow_depth(ix1, iy1));\n"
    "    return mix(mix(s00, s10, ax), mix(s01, s11, ax), ay);\n"
    "}\n\n"
    "fn bayer4x4(x: u32, y: u32) -> f32 {\n"
    "    let idx: u32 = ((y & 3u) << 2u) | (x & 3u);\n"
    "    var v: u32 = 0u;\n"
    "    switch (idx) {\n"
    "        case 0u:  { v = 0u; }  case 1u:  { v = 8u; }  case 2u:  { v = 2u; }  case 3u:  { v = 10u; }\n"
    "        case 4u:  { v = 12u; } case 5u:  { v = 4u; }  case 6u:  { v = 14u; } case 7u:  { v = 6u; }\n"
    "        case 8u:  { v = 3u; }  case 9u:  { v = 11u; } case 10u: { v = 1u; }  case 11u: { v = 9u; }\n"
    "        case 12u: { v = 15u; } case 13u: { v = 7u; }  case 14u: { v = 13u; } default:  { v = 5u; }\n"
    "    }\n"
    "    return (f32(v) - 7.5) * (1.0 / 255.0);\n"
    "}\n\n"
    "@fragment fn fs_scene(fin: SceneVsOut) -> @location(0) vec4<f32> {\n"
    "    _ = scene_samp;\n"
    "    _ = cluster_buf[0];\n"
    "    _ = aux_buf[0];\n"
    "    let uv_dx: vec2<f32> = dpdx(fin.uv) * 128.0;\n"
    "    let uv_dy: vec2<f32> = dpdy(fin.uv) * 128.0;\n"
    "    let meta_y: u32 = fin.meta_y;\n"
    "    let tex_id: u32 = (meta_y >> 16u) & 0xFFu;\n"
    "    let flags: u32 = (meta_y >> 24u) & 0x1Fu;\n"
    "    let shade_mode: u32 = (meta_y >> 29u) & 0x7u;\n"
    "    let mip_on: bool = scene_u.sky_color_pad.w > 0.5;\n"
    "    let v_depth: f32 = fin.mat_info.z;\n\n"
    "    /* Parçacık (PARTICLE == 4): yumuşak dairesel disk + sis */\n"
    "    if (shade_mode == 4u) {\n"
    "        let p: vec2<f32> = fin.uv * 2.0 - vec2<f32>(1.0, 1.0);\n"
    "        let r2: f32 = dot(p, p);\n"
    "        if (r2 > 1.0) { discard; }\n"
    "        let falloff: f32 = 1.0 - r2;\n"
    "        let alpha: f32 = fin.tint.a * falloff * falloff;\n"
    "        let fog_s: f32 = scene_u.fog_color_start.w;\n"
    "        let inv_fog: f32 = scene_u.fog_params.y;\n"
    "        let ft: f32 = clamp((v_depth - fog_s) * inv_fog, 0.0, 1.0);\n"
    "        let rgb: vec3<f32> = mix(fin.tint.rgb, scene_u.fog_color_start.rgb, ft * 0.6);\n"
    "        return vec4<f32>(rgb, alpha);\n"
    "    }\n\n"
    "    let texel: vec4<f32> = sample_atlas_slot(tex_id, fin.uv, uv_dx, uv_dy, mip_on);\n"
    "    let base_rgba: vec4<f32> = texel * fin.tint;\n"
    "    let fog_s: f32 = scene_u.fog_color_start.w;\n"
    "    let inv_fog: f32 = scene_u.fog_params.y;\n"
    "    let fog_t: f32 = clamp((v_depth - fog_s) * inv_fog, 0.0, 1.0);\n"
    "    let fog_col: vec3<f32> = scene_u.fog_color_start.rgb;\n\n"
    "    /* Kendinden ışıklı / Gökyüzü (UNLIT == 2) */\n"
    "    if (shade_mode == 2u) {\n"
    "        let out_u: vec3<f32> = mix(base_rgba.rgb, fog_col, fog_t);\n"
    "        return vec4<f32>(out_u, base_rgba.a);\n"
    "    }\n\n"
    "    let N: vec3<f32> = normalize(fin.world_nrm);\n"
    "    let V: vec3<f32> = normalize(scene_u.eye_near.xyz - fin.world_pos);\n\n"
    "    /* 3D Küme (Clustered Forward) indeksini hesapla */\n"
    "    let vpw: f32 = max(scene_u.viewport_retro.x, 1.0);\n"
    "    let vph: f32 = max(scene_u.viewport_retro.y, 1.0);\n"
    "    let cx: u32 = min(u32((fin.clip_pos.x / vpw) * 16.0), 15u);\n"
    "    let cy: u32 = min(u32((fin.clip_pos.y / vph) * 8.0), 7u);\n"
    "    let near_z: f32 = scene_u.eye_near.w;\n"
    "    let far_z: f32 = scene_u.fwd_far.w;\n"
    "    let lratio: f32 = log2(max(far_z / near_z, 2.0));\n"
    "    let cz_f: f32 = (log2(max(v_depth / near_z, 1.0)) / lratio) * 8.0;\n"
    "    let cz: u32 = min(u32(max(cz_f, 0.0)), 7u);\n"
    "    let c_base: u32 = ((cz * 8u + cy) * 16u + cx) * 16u;\n"
    "    let cl_count: u32 = min(cluster_buf[c_base], 15u);\n\n"
    "    /* Zindan (DUNGEON == 3 — 8:Kuyu, dungeon3d.mtk denklemleri) */\n"
    "    if (shade_mode == 3u) {\n"
    "        let alb_d: vec3<f32> = base_rgba.rgb * base_rgba.rgb;\n"
    "        var total_light: vec3<f32> = scene_u.sky_color_pad.rgb;\n"
    "        if ((flags & 8u) != 0u) {\n"
    "            total_light = vec3<f32>(0.55, 0.45, 0.38);\n"
    "        }\n"
    "        if (fin.mat_info.x > 0.5) {\n"
    "            total_light = total_light + vec3<f32>(1.25, 0.55, 0.18);\n"
    "        }\n"
    "        for (var i: u32 = 0u; i < cl_count; i = i + 1u) {\n"
    "            let li: u32 = cluster_buf[c_base + 1u + i];\n"
    "            let lp: vec4<f32> = aux_buf[768u + li * 2u];\n"
    "            let lc: vec4<f32> = aux_buf[768u + li * 2u + 1u];\n"
    "            let to_l: vec3<f32> = lp.xyz - fin.world_pos;\n"
    "            let d2: f32 = dot(to_l, to_l);\n"
    "            let r2: f32 = max(lp.w * lp.w, 0.0001);\n"
    "            let win: f32 = clamp(1.0 - d2 / r2, 0.0, 1.0);\n"
    "            let inv_d: f32 = inverseSqrt(max(d2, 0.0001));\n"
    "            let ndl: f32 = clamp(dot(N, to_l * inv_d) * 0.75 + 0.25, 0.0, 1.0);\n"
    "            let att: f32 = (win * win * ndl) / (1.0 + lc.w * d2);\n"
    "            total_light = total_light + lc.rgb * att;\n"
    "        }\n"
    "        let lin_d: vec3<f32> = mix(alb_d * total_light, fog_col * fog_col, fog_t);\n"
    "        var col_d: vec3<f32> = sqrt(clamp(lin_d, vec3<f32>(0.0), vec3<f32>(1.0)));\n"
    "        if (scene_u.viewport_retro.z > 0.5) {\n"
    "            let px: u32 = u32(fin.clip_pos.x * 0.5);\n"
    "            let py: u32 = u32(fin.clip_pos.y * 0.5);\n"
    "            let dither: f32 = bayer4x4(px, py) * 1.5;\n"
    "            col_d = floor(clamp(col_d + vec3<f32>(dither), vec3<f32>(0.0), vec3<f32>(1.0)) * 31.0 + 0.5) * (1.0 / 31.0);\n"
    "            if ((py & 1u) == 1u) {\n"
    "                col_d = col_d * (1.0 - scene_u.csm_radii_scan.w * 0.35);\n"
    "            }\n"
    "        }\n"
    "        return vec4<f32>(clamp(col_d, vec3<f32>(0.0), vec3<f32>(1.0)), base_rgba.a);\n"
    "    }\n\n"
    "    let L: vec3<f32> = normalize(scene_u.sun_dir_pad.xyz);\n"
    "    let H: vec3<f32> = normalize(V + L);\n"
    "    let NdotL: f32 = max(dot(N, L), 0.0);\n"
    "    let NdotV: f32 = max(dot(N, V), 0.0001);\n"
    "    let NdotH: f32 = max(dot(N, H), 0.0);\n"
    "    let VdotH: f32 = max(dot(V, H), 0.0);\n\n"
    "    var shadow: f32 = 1.0;\n"
    "    if (scene_u.sun_dir_pad.w > 0.5 && (flags & 4u) != 0u && NdotL > 0.0) {\n"
    "        shadow = sample_csm_shadow(fin.world_pos, NdotL);\n"
    "    }\n\n"
    "    let hemi_t: f32 = N.y * 0.5 + 0.5;\n"
    "    let env_irr: vec3<f32> = mix(scene_u.gnd_color_pad.rgb, scene_u.sky_color_pad.rgb, hemi_t);\n\n"
    "    /* Klasik Lambert + Blinn-Phong (LIT == 1) */\n"
    "    if (shade_mode == 1u) {\n"
    "        var spec_s: f32 = NdotH * NdotH;\n"
    "        spec_s = spec_s * spec_s;\n"
    "        spec_s = spec_s * spec_s;\n"
    "        spec_s = spec_s * spec_s;\n"
    "        var lit_c: vec3<f32> = base_rgba.rgb * (env_irr + scene_u.sun_color_pad.rgb * (NdotL * shadow))\n"
    "                             + scene_u.sun_color_pad.rgb * (spec_s * 0.25 * shadow);\n"
    "        for (var i: u32 = 0u; i < cl_count; i = i + 1u) {\n"
    "            let li: u32 = cluster_buf[c_base + 1u + i];\n"
    "            let lp: vec4<f32> = aux_buf[768u + li * 2u];\n"
    "            let lc: vec4<f32> = aux_buf[768u + li * 2u + 1u];\n"
    "            let to_p: vec3<f32> = lp.xyz - fin.world_pos;\n"
    "            let d2: f32 = dot(to_p, to_p);\n"
    "            let Lp: vec3<f32> = to_p * inverseSqrt(max(d2, 0.0001));\n"
    "            let ndlp: f32 = max(dot(N, Lp), 0.0);\n"
    "            let att: f32 = 1.0 / (1.0 + lc.w * d2);\n"
    "            lit_c = lit_c + base_rgba.rgb * lc.rgb * (ndlp * att);\n"
    "        }\n"
    "        let out_l: vec3<f32> = mix(clamp(lit_c, vec3<f32>(0.0), vec3<f32>(1.0)), fog_col, fog_t);\n"
    "        return vec4<f32>(out_l, base_rgba.a);\n"
    "    }\n\n"
    "    /* Cook-Torrance GGX PBR (PBR == 0 — pbr3d.mtk denklemleri + CSM + Clustered Lights) */\n"
    "    let albedo: vec3<f32> = base_rgba.rgb * base_rgba.rgb;\n"
    "    let metallic: f32 = clamp(fin.mat_info.x, 0.0, 1.0);\n"
    "    let roughness: f32 = max(fin.mat_info.y, 0.04);\n"
    "    let a: f32 = roughness * roughness;\n"
    "    let a2: f32 = a * a;\n"
    "    let denom_d: f32 = NdotH * NdotH * (a2 - 1.0) + 1.0;\n"
    "    let D: f32 = a2 / (3.14159265 * denom_d * denom_d);\n"
    "    let r1: f32 = roughness + 1.0;\n"
    "    let k: f32 = (r1 * r1) * 0.125;\n"
    "    let g_v: f32 = NdotV * (1.0 - k) + k;\n"
    "    let g_l: f32 = NdotL * (1.0 - k) + k;\n"
    "    let vis: f32 = 0.25 / (g_v * g_l);\n"
    "    let f0: vec3<f32> = mix(vec3<f32>(0.04), albedo, metallic);\n"
    "    let omv: f32 = 1.0 - VdotH;\n"
    "    let omv2: f32 = omv * omv;\n"
    "    let omv5: f32 = omv2 * omv2 * omv;\n"
    "    let F: vec3<f32> = f0 + (vec3<f32>(1.0) - f0) * omv5;\n"
    "    let kd: vec3<f32> = (vec3<f32>(1.0) - F) * (1.0 - metallic);\n"
    "    let brdf: vec3<f32> = kd * albedo * 0.31830989 + (D * vis) * F;\n"
    "    var lo: vec3<f32> = brdf * scene_u.sun_color_pad.rgb * (NdotL * shadow);\n\n"
    "    /* Kümelenmiş Nokta Işıklar (Clustered Forward Point Lights) */\n"
    "    for (var i: u32 = 0u; i < cl_count; i = i + 1u) {\n"
    "        let li: u32 = cluster_buf[c_base + 1u + i];\n"
    "        let lp: vec4<f32> = aux_buf[768u + li * 2u];\n"
    "        let lc: vec4<f32> = aux_buf[768u + li * 2u + 1u];\n"
    "        let to_p: vec3<f32> = lp.xyz - fin.world_pos;\n"
    "        let d2: f32 = dot(to_p, to_p);\n"
    "        let Lp: vec3<f32> = to_p * inverseSqrt(max(d2, 0.0001));\n"
    "        let ndlp: f32 = max(dot(N, Lp), 0.0);\n"
    "        if (ndlp > 0.0) {\n"
    "            let Hp: vec3<f32> = normalize(V + Lp);\n"
    "            let ndhp: f32 = max(dot(N, Hp), 0.0);\n"
    "            let dp_d: f32 = ndhp * ndhp * (a2 - 1.0) + 1.0;\n"
    "            let Dp: f32 = a2 / (3.14159265 * dp_d * dp_d);\n"
    "            let att: f32 = 1.0 / (1.0 + lc.w * d2);\n"
    "            let p_brdf: vec3<f32> = kd * albedo * 0.31830989 + f0 * (Dp * 0.25);\n"
    "            lo = lo + p_brdf * lc.rgb * (ndlp * att);\n"
    "        }\n"
    "    }\n\n"
    "    let ambient: vec3<f32> = env_irr * (albedo * (1.0 - metallic) + f0 * (0.25 + 0.5 * (1.0 - roughness)));\n"
    "    let hdr: vec3<f32> = mix(lo + ambient, fog_col, fog_t);\n"
    "    let mapped: vec3<f32> = clamp((hdr * (hdr * 2.51 + vec3<f32>(0.03))) / (hdr * (hdr * 2.43 + vec3<f32>(0.59)) + vec3<f32>(0.14)), vec3<f32>(0.0), vec3<f32>(1.0));\n"
    "    return vec4<f32>(sqrt(mapped), base_rgba.a);\n"
    "}\n";

/* Host ve Wasm ortamlarında pbr3d.mtk WGSL derleme doğrulaması için gömülü kaynak yedek metni */
static const char k_pbr3d_mtk_fallback[] =
    "uniform sun_dir: vec3;\n"
    "uniform sun_col: vec3;\n"
    "uniform sky_col: vec3;\n"
    "uniform gnd_col: vec3;\n"
    "uniform eye: vec3;\n"
    "uniform base: vec4;\n"
    "uniform mat: vec4;\n"
    "uniform fog: vec4;\n"
    "uniform albedo: texture2d @bilinear @mipmap;\n"
    "uniform shadow: texture2d_f32 @clamp;\n"
    "in uv: vec2;\n"
    "in nrm: vec3;\n"
    "in depth: f32;\n"
    "in wpos: vec3;\n"
    "in spos: vec3;\n"
    "fn frag() -> rgba {\n"
    "    let t = sample(albedo, uv);\n"
    "    let d = sample_depth(shadow, spos.xy);\n"
    "    let s = select(0.0, 1.0, spos.z + mat.z >= d);\n"
    "    let n = normalize(nrm);\n"
    "    let l = max(dot(n, sun_dir), 0.0) * s;\n"
    "    let c = (t.rgb * (1.0 / 255.0)) * (sky_col + sun_col * l) + gnd_col * 0.01 + eye * 0.0 + base.rgb * 0.0 + fog.rgb * 0.0 + vec3(depth * 0.0 + wpos.x * 0.0);\n"
    "    return vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

/* --- Statik GPU Sahne Durumu --- */

static u8             g_scene_arena_mem[SCENE_ARENA_CAP] __attribute__((aligned(16)));
static GpuInstance    g_cpu_instances[GPU_SCENE_MAX_INSTANCES] __attribute__((aligned(16)));
static u8             g_dirty_flags[GPU_SCENE_MAX_INSTANCES];
static u32            g_dirty_list[GPU_SCENE_MAX_INSTANCES];
static GpuSceneVertex g_dyn_verts[SCENE_DYNAMIC_VB_CAP];
static u32            g_dyn_indices[SCENE_DYNAMIC_IB_CAP];
static u32            g_atlas_staging[SCENE_ATLAS_TILE_SIZE * SCENE_ATLAS_TILE_SIZE];

typedef struct {
    Arena              arena;
    Rhi                rhi;
    b32                initialized;
    b32                gpu_resources_created;
    b32                bench_100k_on;
    b32                bench_100k_gpu_inited;
    u32                bench_100k_batch_id;
    u32                bench_100k_mesh_id;

    u32                dirty_count;
    u32                cur_dyn_inst_count;
    u32                prev_dyn_inst_count;
    u32                static_vb_used;
    u32                static_ib_used;
    u32                dyn_vb_used;
    u32                dyn_ib_used;

    GpuMeshSlot        meshes[GPU_SCENE_MAX_MESHES];
    u32                mesh_count;
    u32                opaque_batch_count;
    u32                alpha_batch_count;
    u32                visible_alloc_cursor;

    GpuTexSlot         textures[GPU_SCENE_MAX_TEXTURES];
    u32                tex_count;

    GpuPointLight      lights[GPU_SCENE_MAX_LIGHTS];
    u32                light_count;

    GpuSceneFrameDesc  frame_desc;
    GpuSceneUniforms   uniforms;
    Vec4               aux_data[SCENE_AUX_VEC4_COUNT];
    GpuSceneStats      stats;
    f64                frame_t0_ms;

    u32                depth_w;
    u32                depth_h;

    /* RHI Tutamaçları */
    RhiShader          sh_compute;
    RhiShader          sh_render;
    RhiShader          sh_mtk_pbr;

    RhiComputePipeline cpipe_init_100k;
    RhiComputePipeline cpipe_reset_hiz;
    RhiComputePipeline cpipe_hiz_down_a;
    RhiComputePipeline cpipe_hiz_down_b;
    RhiComputePipeline cpipe_cull;
    RhiComputePipeline cpipe_cluster;

    RhiPipeline        rpipe_shadow[GPU_SCENE_CSM_CASCADES];
    RhiPipeline        rpipe_opaque;
    RhiPipeline        rpipe_alpha;

    RhiBuffer          buf_uniforms;
    RhiBuffer          buf_instances;
    RhiBuffer          buf_indirect;
    RhiBuffer          buf_visible;
    RhiBuffer          buf_hiz;
    RhiBuffer          buf_clusters;
    RhiBuffer          buf_aux;
    RhiBuffer          buf_vb;
    RhiBuffer          buf_ib;

    RhiTexture         tex_albedo_atlas;
    RhiTexture         tex_shadow_atlas;
    RhiTexture         tex_shadow_depth;
    RhiTexture         tex_main_depth;
    RhiSampler         samp_linear;

    RhiBindGroup       bg_compute_init;
    RhiBindGroup       bg_compute_reset;
    RhiBindGroup       bg_compute_down_a;
    RhiBindGroup       bg_compute_down_b;
    RhiBindGroup       bg_compute_cull;
    RhiBindGroup       bg_compute_cluster;
    RhiBindGroup       bg_shadow[GPU_SCENE_CSM_CASCADES];
    RhiBindGroup       bg_render_opaque;
    RhiBindGroup       bg_render_alpha;
} GpuSceneState;

static GpuSceneState g_scene;

/* --- Yardımcı Matematik ve Frustum / CSM Hesapları --- */

static Vec4 mat4_row_vec(const Mat4* m, u32 r) {
    return vec4(m->m[0 * 4 + r], m->m[1 * 4 + r], m->m[2 * 4 + r], m->m[3 * 4 + r]);
}

static Vec4 normalize_plane(Vec4 p) {
    f32 len = m_sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return (len > 1e-6f) ? vec4_scale(p, 1.0f / len) : p;
}

void gpu_scene_extract_frustum_planes(Mat4 view_proj, Vec4 out_planes[6]) {
    Vec4 r0 = mat4_row_vec(&view_proj, 0u);
    Vec4 r1 = mat4_row_vec(&view_proj, 1u);
    Vec4 r2 = mat4_row_vec(&view_proj, 2u);
    Vec4 r3 = mat4_row_vec(&view_proj, 3u);

    out_planes[0] = normalize_plane(vec4_add(r3, r0)); /* sol:   x >= -w */
    out_planes[1] = normalize_plane(vec4_sub(r3, r0)); /* sağ:   x <=  w */
    out_planes[2] = normalize_plane(vec4_add(r3, r1)); /* alt:   y >= -w */
    out_planes[3] = normalize_plane(vec4_sub(r3, r1)); /* üst:   y <=  w */
    out_planes[4] = normalize_plane(vec4_sub(r3, r2)); /* yakın: z <=  w (reverse-Z near=1) */
    if (r2.x * r2.x + r2.y * r2.y + r2.z * r2.z > 1e-6f) {
        out_planes[5] = normalize_plane(r2);           /* uzak:  z >=  0 (reverse-Z far=0) */
    } else {
        Vec4 nr3 = normalize_plane(r3);                /* sonsuz uzak düzlemde 250m kesim düzlemi */
        out_planes[5] = vec4(-nr3.x, -nr3.y, -nr3.z, 250.0f - nr3.w);
    }
}

void gpu_scene_fit_csm(Vec3 cam_pos, Vec3 cam_fwd, Vec3 sun_dir,
                       Mat4 out_vp[GPU_SCENE_CSM_CASCADES],
                       Mat4 out_tex[GPU_SCENE_CSM_CASCADES],
                       f32  out_radii[GPU_SCENE_CSM_CASCADES]) {
    static const f32 k_radii[3] = { 8.0f, 22.0f, 65.0f };
    static const f32 k_fwd_off[3] = { 2.0f, 8.0f, 24.0f };

    Vec3 sdir = vec3_norm(sun_dir);
    Vec3 up = (m_abs(sdir.y) > 0.99f) ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, 1.0f, 0.0f);
    Mat4 rot = mat4_look_at(vec3(0.0f, 0.0f, 0.0f), vec3_scale(sdir, -1.0f), up);

    Mat4 bias = mat4_identity();
    bias.m[0] = 0.5f;  bias.m[12] = 0.5f;
    bias.m[5] = -0.5f; bias.m[13] = 0.5f;

    for (u32 c = 0u; c < GPU_SCENE_CSM_CASCADES; c++) {
        f32 radius = k_radii[c];
        out_radii[c] = radius;
        Vec3 center = vec3_add(cam_pos, vec3_scale(cam_fwd, k_fwd_off[c]));
        Vec4 lc = mat4_mul_vec4(rot, vec4(center.x, center.y, center.z, 1.0f));
        f32 texel = (2.0f * radius) / (f32)GPU_SCENE_CSM_SIZE;
        f32 cx = m_floor(lc.x / texel) * texel;
        f32 cy = m_floor(lc.y / texel) * texel;
        f32 n = -lc.z - radius - 25.0f;
        f32 f = -lc.z + radius;
        Mat4 proj = mat4_ortho_rz(cx - radius, cx + radius, cy - radius, cy + radius, n, f);
        Mat4 vp = mat4_mul(proj, rot);
        out_vp[c] = vp;
        out_tex[c] = mat4_mul(bias, vp);
    }
}

/* --- CPU Referans Hi-Z Piramidi, Culling ve Clustered Light Fonksiyonları --- */

static u32 hiz_mip_offset_cpu(u32 mip) {
    static const u32 k_off[8] = { 0u, 8192u, 10240u, 10752u, 10880u, 10912u, 10920u, 10922u };
    return k_off[mip < 8u ? mip : 7u];
}

void gpu_scene_hiz_build_cpu_ref(f32* hiz_mips, Vec4 occluder_ndc_box, f32 occluder_depth) {
    if (!hiz_mips) return;
    /* Mip 0: 128 x 64 */
    for (u32 y = 0u; y < GPU_SCENE_HIZ_H; y++) {
        f32 v = ((f32)y + 0.5f) * (1.0f / (f32)GPU_SCENE_HIZ_H);
        for (u32 x = 0u; x < GPU_SCENE_HIZ_W; x++) {
            f32 u = ((f32)x + 0.5f) * (1.0f / (f32)GPU_SCENE_HIZ_W);
            f32 z = 0.0f;
            if (u >= occluder_ndc_box.x && u <= occluder_ndc_box.z &&
                v >= occluder_ndc_box.y && v <= occluder_ndc_box.w) {
                z = occluder_depth;
            }
            hiz_mips[y * GPU_SCENE_HIZ_W + x] = z;
        }
    }
    /* Mip 1..7: her üst seviye 2x2 alt bloğun minimumu (reverse-Z muhafazakâr derinlik) */
    for (u32 mip = 1u; mip < GPU_SCENE_HIZ_MIPS; mip++) {
        u32 pw = GPU_SCENE_HIZ_W >> (mip - 1u);
        u32 ph = GPU_SCENE_HIZ_H >> (mip - 1u);
        if (pw == 0u) pw = 1u;
        if (ph == 0u) ph = 1u;
        u32 cw = GPU_SCENE_HIZ_W >> mip;
        u32 ch = GPU_SCENE_HIZ_H >> mip;
        if (cw == 0u) cw = 1u;
        if (ch == 0u) ch = 1u;
        u32 p_off = hiz_mip_offset_cpu(mip - 1u);
        u32 c_off = hiz_mip_offset_cpu(mip);
        for (u32 y = 0u; y < ch; y++) {
            for (u32 x = 0u; x < cw; x++) {
                u32 sx0 = MIN(x * 2u, pw - 1u);
                u32 sx1 = MIN(x * 2u + 1u, pw - 1u);
                u32 sy0 = MIN(y * 2u, ph - 1u);
                u32 sy1 = MIN(y * 2u + 1u, ph - 1u);
                f32 z00 = hiz_mips[p_off + sy0 * pw + sx0];
                f32 z10 = hiz_mips[p_off + sy0 * pw + sx1];
                f32 z01 = hiz_mips[p_off + sy1 * pw + sx0];
                f32 z11 = hiz_mips[p_off + sy1 * pw + sx1];
                hiz_mips[c_off + y * cw + x] = m_min(m_min(z00, z10), m_min(z01, z11));
            }
        }
    }
}

u32 gpu_scene_cull_cpu_ref(const GpuInstance* instances, u32 count,
                           const Vec4* mesh_bounds, const Vec4 frustum_planes[6],
                           Mat4 view, f32 near_z, f32 cot_half_fov, f32 aspect,
                           const f32* hiz_mips,
                           GpuIndirectCommand* indirect_cmds, u32 batch_count,
                           const u32* batch_first_inst, const u32* batch_max_inst,
                           u32* visible_out, u32* out_frustum_culled, u32* out_hiz_culled) {
    u32 vis_total = 0u, f_culled = 0u, h_culled = 0u;
    for (u32 b = 0u; b < batch_count; b++) {
        indirect_cmds[b].instance_count = 0u;
    }

    for (u32 i = 0u; i < count; i++) {
        const GpuInstance* inst = &instances[i];
        u32 flags = (inst->mat_tex >> 24u) & 0x1Fu;
        u32 shade_mode = (inst->mat_tex >> 29u) & 0x7u;
        if (!(flags & GPU_INST_ACTIVE)) continue;

        u32 mesh_id  = inst->mesh_batch & 0xFFFFu;
        u32 batch_id = (inst->mesh_batch >> 16u) & 0xFFFFu;
        f32 rad      = inst->bound_radius;
        Vec4 mb      = mesh_bounds ? mesh_bounds[mesh_id & 255u] : vec4(0.0f, 0.0f, 0.0f, 1.0f);
        Vec3 wc = vec3(
            inst->r0[0] * mb.x + inst->r0[1] * mb.y + inst->r0[2] * mb.z + inst->r0[3],
            inst->r1[0] * mb.x + inst->r1[1] * mb.y + inst->r1[2] * mb.z + inst->r1[3],
            inst->r2[0] * mb.x + inst->r2[1] * mb.y + inst->r2[2] * mb.z + inst->r2[3]
        );

        if (!(flags & GPU_INST_VIEWMODEL)) {
            b32 outside = FALSE;
            for (u32 p = 0u; p < 6u; p++) {
                Vec4 pl = frustum_planes[p];
                if (pl.x * wc.x + pl.y * wc.y + pl.z * wc.z + pl.w < -rad) {
                    outside = TRUE;
                    break;
                }
            }
            if (outside) {
                f_culled++;
                continue;
            }

            if (hiz_mips && shade_mode != GPU_SHADE_UNLIT) {
                Vec4 vpos = mat4_mul_vec4(view, vec4(wc.x, wc.y, wc.z, 1.0f));
                f32 dist = -vpos.z;
                if (dist > near_z + rad * 1.25f) {
                    f32 z_near_v = m_max(dist - rad, near_z);
                    f32 sphere_max_z = m_clamp(near_z / z_near_v, 0.0f, 1.0f);
                    f32 proj_r_y = (rad * cot_half_fov) / dist;
                    f32 proj_r_x = proj_r_y / aspect;
                    f32 ndc_x = (vpos.x * cot_half_fov / aspect) / dist;
                    f32 ndc_y = (vpos.y * cot_half_fov) / dist;
                    f32 u_min = m_clamp((ndc_x - proj_r_x) * 0.5f + 0.5f, 0.0f, 0.999f);
                    f32 u_max = m_clamp((ndc_x + proj_r_x) * 0.5f + 0.5f, 0.0f, 0.999f);
                    f32 v_min = m_clamp(0.5f - (ndc_y + proj_r_y) * 0.5f, 0.0f, 0.999f);
                    f32 v_max = m_clamp(0.5f - (ndc_y - proj_r_y) * 0.5f, 0.0f, 0.999f);
                    f32 fp_w = (u_max - u_min) * (f32)GPU_SCENE_HIZ_W;
                    f32 fp_h = (v_max - v_min) * (f32)GPU_SCENE_HIZ_H;
                    f32 fp = m_max(m_max(fp_w, fp_h), 1.0f);
                    u32 mip = (u32)m_ceil(m_log2(fp));
                    if (mip > 4u) mip = 4u;
                    u32 mw = GPU_SCENE_HIZ_W >> mip;
                    u32 mh = GPU_SCENE_HIZ_H >> mip;
                    u32 x0 = MIN((u32)(u_min * (f32)mw), mw - 1u);
                    u32 x1 = MIN((u32)(u_max * (f32)mw), mw - 1u);
                    u32 y0 = MIN((u32)(v_min * (f32)mh), mh - 1u);
                    u32 y1 = MIN((u32)(v_max * (f32)mh), mh - 1u);
                    u32 m_off = hiz_mip_offset_cpu(mip);
                    f32 hz00 = hiz_mips[m_off + y0 * mw + x0];
                    f32 hz10 = hiz_mips[m_off + y0 * mw + x1];
                    f32 hz01 = hiz_mips[m_off + y1 * mw + x0];
                    f32 hz11 = hiz_mips[m_off + y1 * mw + x1];
                    f32 occ_z = m_min(m_min(hz00, hz10), m_min(hz01, hz11));
                    if (sphere_max_z < occ_z - 0.0002f) {
                        h_culled++;
                        continue;
                    }
                }
            }
        }

        if (batch_id < batch_count) {
            u32 slot = indirect_cmds[batch_id].instance_count++;
            if (visible_out && batch_first_inst && batch_max_inst && slot < batch_max_inst[batch_id]) {
                visible_out[batch_first_inst[batch_id] + slot] = i;
            }
        }
        vis_total++;
    }

    if (out_frustum_culled) *out_frustum_culled = f_culled;
    if (out_hiz_culled) *out_hiz_culled = h_culled;
    return vis_total;
}

void gpu_scene_cluster_lights_cpu_ref(const GpuPointLight* lights, u32 light_count,
                                      Mat4 view, f32 near_z, f32 far_z,
                                      f32 cot_half_fov, f32 aspect,
                                      u32* cluster_out) {
    if (!cluster_out) return;
    f32 ratio = m_max(far_z / near_z, 2.0f);
    f32 tan_y = 1.0f / m_max(cot_half_fov, 0.1f);
    f32 tan_x = tan_y * aspect;
    u32 lcount = MIN(light_count, GPU_SCENE_MAX_LIGHTS);

    for (u32 cid = 0u; cid < GPU_SCENE_CLUSTER_COUNT; cid++) {
        u32 cx = cid & 15u;
        u32 cy = (cid >> 4u) & 7u;
        u32 cz = cid >> 7u;
        f32 z0 = near_z * m_pow(ratio, (f32)cz / 8.0f);
        f32 z1 = near_z * m_pow(ratio, (f32)(cz + 1u) / 8.0f);
        f32 nx0 = ((f32)cx / 16.0f) * 2.0f - 1.0f;
        f32 nx1 = ((f32)(cx + 1u) / 16.0f) * 2.0f - 1.0f;
        f32 ny0 = 1.0f - ((f32)(cy + 1u) / 8.0f) * 2.0f;
        f32 ny1 = 1.0f - ((f32)cy / 8.0f) * 2.0f;
        f32 min_x = m_min(nx0 * z0, nx0 * z1) * tan_x;
        f32 max_x = m_max(nx1 * z0, nx1 * z1) * tan_x;
        f32 min_y = m_min(ny0 * z0, ny0 * z1) * tan_y;
        f32 max_y = m_max(ny1 * z0, ny1 * z1) * tan_y;
        f32 min_z = -z1;
        f32 max_z = -z0;

        u32 base = cid * GPU_SCENE_CLUSTER_STRIDE;
        u32 found = 0u;
        for (u32 l = 0u; l < lcount; l++) {
            Vec4 lv = mat4_mul_vec4(view, vec4(lights[l].pos[0], lights[l].pos[1], lights[l].pos[2], 1.0f));
            f32 r = lights[l].radius;
            f32 cx_p = m_clamp(lv.x, min_x, max_x);
            f32 cy_p = m_clamp(lv.y, min_y, max_y);
            f32 cz_p = m_clamp(lv.z, min_z, max_z);
            f32 dx = lv.x - cx_p, dy = lv.y - cy_p, dz = lv.z - cz_p;
            if (dx * dx + dy * dy + dz * dz <= r * r && found < 15u) {
                found++;
                cluster_out[base + found] = l;
            }
        }
        cluster_out[base] = found;
    }
}

/* --- Kirli Liste (Dirty List) ve Kalıcı Örnek (Instance) Yönetimi --- */

b32 gpu_scene_set_instance(u32 slot_idx, const GpuInstance* inst) {
    if (slot_idx >= GPU_SCENE_MAX_INSTANCES || !inst) return FALSE;
    if (__builtin_memcmp(&g_cpu_instances[slot_idx], inst, sizeof(GpuInstance)) == 0) {
        return FALSE;
    }
    g_cpu_instances[slot_idx] = *inst;
    if (!g_dirty_flags[slot_idx]) {
        g_dirty_flags[slot_idx] = 1u;
        g_dirty_list[g_scene.dirty_count++] = slot_idx;
    }
    return TRUE;
}

u32 gpu_scene_dirty_count(void) {
    return g_scene.dirty_count;
}

u32 gpu_scene_flush_dirty(Rhi* rhi, RhiBuffer dst_buf, u32* out_spans, u32* out_bytes) {
    u32 count = g_scene.dirty_count;
    u32 spans = 0u;
    u32 bytes = 0u;

    /* Bitişik kirli yuvaları tek rhi_write_buffer çağrısında birleştir */
    for (u32 i = 0u; i < count; i++) {
        u32 idx = g_dirty_list[i];
        if (!g_dirty_flags[idx]) continue;

        u32 start = idx;
        u32 end   = idx + 1u;
        g_dirty_flags[idx] = 0u;

        while (end < GPU_SCENE_MAX_INSTANCES && g_dirty_flags[end]) {
            g_dirty_flags[end] = 0u;
            end++;
        }

        u32 span_len = (end - start) * (u32)sizeof(GpuInstance);
        if (rhi && dst_buf != RHI_INVALID_HANDLE) {
            rhi_write_buffer(rhi, dst_buf, start * (u32)sizeof(GpuInstance),
                             &g_cpu_instances[start], span_len);
        }
        spans++;
        bytes += span_len;
    }

    g_scene.dirty_count = 0u;
    if (out_spans) *out_spans = spans;
    if (out_bytes) *out_bytes = bytes;
    return count;
}

void gpu_scene_reset_instances(void) {
    __builtin_memset(g_cpu_instances, 0, sizeof(g_cpu_instances));
    __builtin_memset(g_dirty_flags, 0, sizeof(g_dirty_flags));
    g_scene.dirty_count         = 0u;
    g_scene.cur_dyn_inst_count  = 0u;
    g_scene.prev_dyn_inst_count = 0u;
    g_scene.bench_100k_gpu_inited = FALSE;
}

/* --- GPU Kaynaklarının İlk Kurulumu --- */

static const char* find_pbr3d_mtk_src(u32* out_len) {
#ifdef __wasm__
    for (u32 i = 0u; i < g_embedded_shader_count; i++) {
        const char* a = g_embedded_shaders[i].name;
        const char* b = "pbr3d.mtk";
        u32 k = 0u;
        while (a[k] && b[k] && a[k] == b[k]) k++;
        if (a[k] == '\0' && b[k] == '\0') {
            *out_len = g_embedded_shaders[i].len;
            return g_embedded_shaders[i].src;
        }
    }
#endif
    u32 n = 0u;
    while (k_pbr3d_mtk_fallback[n] != '\0') n++;
    *out_len = n;
    return k_pbr3d_mtk_fallback;
}

static b32 ensure_gpu_resources(void) {
    if (g_scene.gpu_resources_created) return TRUE;
    Rhi* rhi = &g_scene.rhi;

    /* 1. pbr3d.mtk -> WGSL derleme doğrulaması */
    usize mark = arena_mark(&g_scene.arena);
    u32 mtk_len = 0u;
    const char* mtk_src = find_pbr3d_mtk_src(&mtk_len);
    char* mtk_wgsl = (char*)arena_push(&g_scene.arena, 65536u, 16u);
    MtkLayout mtk_layout;
    MtkDiag mtk_diag;
    u32 wlen = mtk_compile_wgsl(mtk_src, mtk_len, "shaders/pbr3d.mtk", NULL,
                                &g_scene.arena, mtk_wgsl, 65536u, &mtk_layout, &mtk_diag);
    if (wlen > 0u) {
        g_scene.sh_mtk_pbr = rhi_create_shader(rhi, mtk_wgsl, wlen);
    }
    arena_pop_to(&g_scene.arena, mark);

    /* 2. Compute ve Render WGSL modülleri */
    g_scene.sh_compute = rhi_create_shader(rhi, k_scene_compute_wgsl,
                                           (u32)(sizeof(k_scene_compute_wgsl) - 1u));
    g_scene.sh_render  = rhi_create_shader(rhi, k_scene_render_wgsl,
                                           (u32)(sizeof(k_scene_render_wgsl) - 1u));
    if (g_scene.sh_compute == RHI_INVALID_HANDLE || g_scene.sh_render == RHI_INVALID_HANDLE) {
        return FALSE;
    }

    /* 3. Compute Boru Hatları */
    RhiComputePipelineDesc cdesc;
    cdesc.shader = g_scene.sh_compute;
    cdesc.entry = "cs_init_100k";
    g_scene.cpipe_init_100k = rhi_create_compute_pipeline(rhi, &cdesc);
    cdesc.entry = "cs_reset_hiz";
    g_scene.cpipe_reset_hiz = rhi_create_compute_pipeline(rhi, &cdesc);
    cdesc.entry = "cs_hiz_down_a";
    g_scene.cpipe_hiz_down_a = rhi_create_compute_pipeline(rhi, &cdesc);
    cdesc.entry = "cs_hiz_down_b";
    g_scene.cpipe_hiz_down_b = rhi_create_compute_pipeline(rhi, &cdesc);
    cdesc.entry = "cs_cull_inst";
    g_scene.cpipe_cull = rhi_create_compute_pipeline(rhi, &cdesc);
    cdesc.entry = "cs_cluster_lt";
    g_scene.cpipe_cluster = rhi_create_compute_pipeline(rhi, &cdesc);

    /* 4. Render Boru Hatları (3x CSM Gölge + Opak PBR + Alfa Parçacık) */
    RhiPipelineDesc pdesc;
    __builtin_memset(&pdesc, 0, sizeof(pdesc));
    pdesc.vertex_shader   = g_scene.sh_render;
    pdesc.fragment_shader = g_scene.sh_render;
    pdesc.fs_entry        = "fs_shadow";
    pdesc.vertex_stride   = (u32)sizeof(GpuSceneVertex);
    pdesc.attr_count      = 5u;
    pdesc.attrs[0] = (RhiVertexAttr){ .shader_location = 0u, .offset = 0u,  .format = RHI_VFMT_FLOAT32X3 };
    pdesc.attrs[1] = (RhiVertexAttr){ .shader_location = 1u, .offset = 12u, .format = RHI_VFMT_FLOAT32X3 };
    pdesc.attrs[2] = (RhiVertexAttr){ .shader_location = 2u, .offset = 24u, .format = RHI_VFMT_FLOAT32X2 };
    pdesc.attrs[3] = (RhiVertexAttr){ .shader_location = 3u, .offset = 32u, .format = RHI_VFMT_UNORM8X4  };
    pdesc.attrs[4] = (RhiVertexAttr){ .shader_location = 4u, .offset = 36u, .format = RHI_VFMT_UINT32    };
    pdesc.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
    pdesc.cull_mode       = RHI_CULL_NONE;
    pdesc.front_face      = RHI_FRONT_CCW;
    pdesc.color_format    = RHI_FMT_RGBA8_UNORM;
    pdesc.blend_mode      = RHI_BLEND_NONE;
    pdesc.depth_format    = RHI_FMT_DEPTH32_FLOAT;
    pdesc.depth_test      = TRUE;
    pdesc.depth_write     = TRUE;
    pdesc.depth_compare   = RHI_CMP_GREATER;
    pdesc.sample_count    = 1u;

    static const char* const k_csm_vs[3] = { "vs_shadow_c0", "vs_shadow_c1", "vs_shadow_c2" };
    for (u32 c = 0u; c < GPU_SCENE_CSM_CASCADES; c++) {
        pdesc.vs_entry = k_csm_vs[c];
        g_scene.rpipe_shadow[c] = rhi_create_pipeline(rhi, &pdesc);
    }

    pdesc.vs_entry     = "vs_scene";
    pdesc.fs_entry     = "fs_scene";
    pdesc.color_format = RHI_FMT_SURFACE_DEFAULT;
    pdesc.blend_mode   = RHI_BLEND_NONE;
    pdesc.depth_write  = TRUE;
    pdesc.sample_count = 4u;
    g_scene.rpipe_opaque = rhi_create_pipeline(rhi, &pdesc);

    pdesc.blend_mode   = RHI_BLEND_ALPHA;
    pdesc.depth_write  = FALSE;
    g_scene.rpipe_alpha = rhi_create_pipeline(rhi, &pdesc);

    /* 5. GPU Tamponları */
    g_scene.buf_uniforms  = rhi_create_buffer(rhi, (u32)sizeof(GpuSceneUniforms),
                                              RHI_BUF_UNIFORM | RHI_BUF_COPY_DST);
    g_scene.buf_instances = rhi_create_buffer(rhi, GPU_SCENE_MAX_INSTANCES * (u32)sizeof(GpuInstance),
                                              RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    g_scene.buf_indirect  = rhi_create_buffer(rhi, SCENE_INDIRECT_U32S * 4u,
                                              RHI_BUF_STORAGE | RHI_BUF_INDIRECT | RHI_BUF_COPY_DST);
    g_scene.buf_visible   = rhi_create_buffer(rhi, SCENE_VISIBLE_CAP * 4u,
                                              RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    g_scene.buf_hiz       = rhi_create_buffer(rhi, GPU_SCENE_HIZ_TOTAL_TEXELS * 4u,
                                              RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    g_scene.buf_clusters  = rhi_create_buffer(rhi, GPU_SCENE_CLUSTER_COUNT * GPU_SCENE_CLUSTER_STRIDE * 4u,
                                              RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    g_scene.buf_aux       = rhi_create_buffer(rhi, SCENE_AUX_VEC4_COUNT * (u32)sizeof(Vec4),
                                              RHI_BUF_STORAGE | RHI_BUF_COPY_DST);
    g_scene.buf_vb        = rhi_create_buffer(rhi, SCENE_TOTAL_VB_CAP * (u32)sizeof(GpuSceneVertex),
                                              RHI_BUF_VERTEX | RHI_BUF_COPY_DST);
    g_scene.buf_ib        = rhi_create_buffer(rhi, SCENE_TOTAL_IB_CAP * 4u,
                                              RHI_BUF_INDEX | RHI_BUF_COPY_DST);

    /* 6. Dokular (512x512 Mipmapli Albedo Atlası + 3072x1024 3-Kademeli CSM Gölge Atlası) */
    RhiTextureDesc tdesc;
    tdesc.width      = SCENE_ATLAS_DIM;
    tdesc.height     = SCENE_ATLAS_DIM;
    tdesc.mip_levels = SCENE_ATLAS_MIPS;
    tdesc.format     = RHI_FMT_RGBA8_UNORM;
    tdesc.usage      = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
    g_scene.tex_albedo_atlas = rhi_create_texture(rhi, &tdesc);

    tdesc.width      = GPU_SCENE_CSM_ATLAS_W;
    tdesc.height     = GPU_SCENE_CSM_ATLAS_H;
    tdesc.mip_levels = 1u;
    tdesc.format     = RHI_FMT_RGBA8_UNORM;
    tdesc.usage      = RHI_TEX_RENDER_ATTACHMENT | RHI_TEX_SAMPLED;
    g_scene.tex_shadow_atlas = rhi_create_texture(rhi, &tdesc);

    tdesc.format     = RHI_FMT_DEPTH32_FLOAT;
    tdesc.usage      = RHI_TEX_RENDER_ATTACHMENT;
    g_scene.tex_shadow_depth = rhi_create_texture(rhi, &tdesc);

    u32 vp_w = engine_gpu_viewport_width();
    u32 vp_h = engine_gpu_viewport_height();
    if (vp_w == 0u) vp_w = 1920u;
    if (vp_h == 0u) vp_h = 1080u;
    tdesc.width      = vp_w;
    tdesc.height     = vp_h;
    tdesc.mip_levels = 1u;
    tdesc.format     = RHI_FMT_DEPTH32_FLOAT;
    tdesc.usage      = RHI_TEX_RENDER_ATTACHMENT;
    g_scene.tex_main_depth = rhi_create_texture(rhi, &tdesc);
    g_scene.depth_w = vp_w;
    g_scene.depth_h = vp_h;

    RhiSamplerDesc sdesc = {
        .address_u     = RHI_ADDR_CLAMP,
        .address_v     = RHI_ADDR_CLAMP,
        .mag_filter    = RHI_FILTER_LINEAR,
        .min_filter    = RHI_FILTER_LINEAR,
        .mipmap_filter = RHI_FILTER_LINEAR
    };
    g_scene.samp_linear = rhi_create_sampler(rhi, &sdesc);

    /* Doku Yuvası 0: beyaz varsayılan doku (tüm mip seviyeleri) */
    for (u32 i = 0u; i < SCENE_ATLAS_TILE_SIZE * SCENE_ATLAS_TILE_SIZE; i++) {
        g_atlas_staging[i] = 0xFFFFFFFFu;
    }
    for (u32 m = 0u; m < SCENE_ATLAS_MIPS; m++) {
        u32 tsz = SCENE_ATLAS_TILE_SIZE >> m;
        rhi_write_texture(rhi, g_scene.tex_albedo_atlas, m, 0u, 0u, tsz, tsz,
                          tsz * 4u, g_atlas_staging, tsz * tsz * 4u);
    }
    g_scene.textures[0] = (GpuTexSlot){ .src_px = NULL, .src_w = 1u, .src_h = 1u, .slot_id = 0u };
    g_scene.tex_count = 1u;

    /* 7. Bind Grupları (WebGPU auto-layout gereği her pipeline kendi bind grubunu alır) */
    RhiBindGroupDesc bg_c;
    __builtin_memset(&bg_c, 0, sizeof(bg_c));
    bg_c.group_index = 0u;
    bg_c.entry_count = 7u;
    bg_c.entries[0] = (RhiBindEntry){ 0u, RHI_BIND_BUFFER,         g_scene.buf_uniforms,  0u, 0u };
    bg_c.entries[1] = (RhiBindEntry){ 1u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_instances, 0u, 0u };
    bg_c.entries[2] = (RhiBindEntry){ 2u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_indirect,  0u, 0u };
    bg_c.entries[3] = (RhiBindEntry){ 3u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_visible,   0u, 0u };
    bg_c.entries[4] = (RhiBindEntry){ 4u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_hiz,       0u, 0u };
    bg_c.entries[5] = (RhiBindEntry){ 5u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_clusters,  0u, 0u };
    bg_c.entries[6] = (RhiBindEntry){ 6u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_aux,       0u, 0u };

    bg_c.pipeline = g_scene.cpipe_init_100k;
    g_scene.bg_compute_init = rhi_create_bind_group(rhi, &bg_c);
    bg_c.pipeline = g_scene.cpipe_reset_hiz;
    g_scene.bg_compute_reset = rhi_create_bind_group(rhi, &bg_c);
    bg_c.pipeline = g_scene.cpipe_hiz_down_a;
    g_scene.bg_compute_down_a = rhi_create_bind_group(rhi, &bg_c);
    bg_c.pipeline = g_scene.cpipe_hiz_down_b;
    g_scene.bg_compute_down_b = rhi_create_bind_group(rhi, &bg_c);
    bg_c.pipeline = g_scene.cpipe_cull;
    g_scene.bg_compute_cull = rhi_create_bind_group(rhi, &bg_c);
    bg_c.pipeline = g_scene.cpipe_cluster;
    g_scene.bg_compute_cluster = rhi_create_bind_group(rhi, &bg_c);

    for (u32 c = 0u; c < GPU_SCENE_CSM_CASCADES; c++) {
        RhiBindGroupDesc bg_s;
        __builtin_memset(&bg_s, 0, sizeof(bg_s));
        bg_s.pipeline    = g_scene.rpipe_shadow[c];
        bg_s.group_index = 0u;
        bg_s.entry_count = 3u;
        bg_s.entries[0] = (RhiBindEntry){ 0u, RHI_BIND_BUFFER,         g_scene.buf_uniforms,  0u, 0u };
        bg_s.entries[1] = (RhiBindEntry){ 1u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_instances, 0u, 0u };
        bg_s.entries[2] = (RhiBindEntry){ 2u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_visible,   0u, 0u };
        g_scene.bg_shadow[c] = rhi_create_bind_group(rhi, &bg_s);
    }

    RhiBindGroupDesc bg_r;
    __builtin_memset(&bg_r, 0, sizeof(bg_r));
    bg_r.group_index = 0u;
    bg_r.entry_count = 8u;
    bg_r.entries[0] = (RhiBindEntry){ 0u, RHI_BIND_BUFFER,         g_scene.buf_uniforms,     0u, 0u };
    bg_r.entries[1] = (RhiBindEntry){ 1u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_instances,    0u, 0u };
    bg_r.entries[2] = (RhiBindEntry){ 2u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_visible,      0u, 0u };
    bg_r.entries[3] = (RhiBindEntry){ 3u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_clusters,     0u, 0u };
    bg_r.entries[4] = (RhiBindEntry){ 4u, RHI_BIND_STORAGE_BUFFER, g_scene.buf_aux,          0u, 0u };
    bg_r.entries[5] = (RhiBindEntry){ 5u, RHI_BIND_TEXTURE,        g_scene.tex_albedo_atlas, 0u, 0u };
    bg_r.entries[6] = (RhiBindEntry){ 6u, RHI_BIND_TEXTURE,        g_scene.tex_shadow_atlas, 0u, 0u };
    bg_r.entries[7] = (RhiBindEntry){ 7u, RHI_BIND_SAMPLER,        g_scene.samp_linear,      0u, 0u };
    bg_r.pipeline = g_scene.rpipe_opaque;
    g_scene.bg_render_opaque = rhi_create_bind_group(rhi, &bg_r);
    bg_r.pipeline = g_scene.rpipe_alpha;
    g_scene.bg_render_alpha = rhi_create_bind_group(rhi, &bg_r);

    g_scene.gpu_resources_created = TRUE;
    return TRUE;
}

b32 gpu_scene_init(void) {
    if (g_scene.initialized) return TRUE;
    __builtin_memset(&g_scene, 0, sizeof(g_scene));
    arena_init(&g_scene.arena, g_scene_arena_mem, sizeof(g_scene_arena_mem));
    if (!rhi_init(&g_scene.rhi, &g_scene.arena, SCENE_RHI_MAX_RESOURCES, SCENE_RHI_STREAM_CAP)) {
        return FALSE;
    }
    /* gpu_ocean ve gpu_test tutamaçlarıyla (gen=1) çakışmaması için nesil tabanını 32'den başlat */
    for (u32 i = 0u; i <= SCENE_RHI_MAX_RESOURCES; i++) {
        g_scene.rhi.slots[i].gen = 32u;
    }
    gpu_scene_reset_instances();
    g_scene.initialized = TRUE;
    return TRUE;
}

/* --- Doku ve Ağ (Mesh) Önbellekleme --- */

u32 gpu_scene_register_texture(const Texture* tex) {
    if (!tex || !tex->px || tex->w == 0u || tex->h == 0u) return 0u;
    for (u32 i = 1u; i < g_scene.tex_count; i++) {
        if (g_scene.textures[i].src_px == tex->px &&
            g_scene.textures[i].src_w == tex->w &&
            g_scene.textures[i].src_h == tex->h) {
            return i;
        }
    }
    if (g_scene.tex_count >= GPU_SCENE_MAX_TEXTURES) {
        return 1u;
    }
    u32 slot = g_scene.tex_count++;
    g_scene.textures[slot] = (GpuTexSlot){
        .src_px  = tex->px,
        .src_w   = tex->w,
        .src_h   = tex->h,
        .slot_id = slot
    };

    /* 128x128 mip 0 oluştur ve kutu filtresiyle mip 1..6 indirge */
    u32 sx_tile = (slot & 3u);
    u32 sy_tile = (slot >> 2u) & 3u;
    u32 sw = tex->w;
    u32 sh = tex->h;
    for (u32 y = 0u; y < SCENE_ATLAS_TILE_SIZE; y++) {
        u32 ty = (y * sh) / SCENE_ATLAS_TILE_SIZE;
        for (u32 x = 0u; x < SCENE_ATLAS_TILE_SIZE; x++) {
            u32 tx = (x * sw) / SCENE_ATLAS_TILE_SIZE;
            g_atlas_staging[y * SCENE_ATLAS_TILE_SIZE + x] = tex->px[ty * sw + tx];
        }
    }

    for (u32 m = 0u; m < SCENE_ATLAS_MIPS; m++) {
        u32 tsz = SCENE_ATLAS_TILE_SIZE >> m;
        rhi_write_texture(&g_scene.rhi, g_scene.tex_albedo_atlas, m,
                          sx_tile * tsz, sy_tile * tsz, tsz, tsz,
                          tsz * 4u, g_atlas_staging, tsz * tsz * 4u);
        if (m + 1u < SCENE_ATLAS_MIPS) {
            u32 nsz = tsz >> 1u;
            for (u32 y = 0u; y < nsz; y++) {
                for (u32 x = 0u; x < nsz; x++) {
                    u32 c00 = g_atlas_staging[(y * 2u) * tsz + (x * 2u)];
                    u32 c10 = g_atlas_staging[(y * 2u) * tsz + (x * 2u + 1u)];
                    u32 c01 = g_atlas_staging[(y * 2u + 1u) * tsz + (x * 2u)];
                    u32 c11 = g_atlas_staging[(y * 2u + 1u) * tsz + (x * 2u + 1u)];
                    u32 r = ((c00 & 0xFFu) + (c10 & 0xFFu) + (c01 & 0xFFu) + (c11 & 0xFFu) + 2u) >> 2u;
                    u32 g = (((c00 >> 8u) & 0xFFu) + ((c10 >> 8u) & 0xFFu) +
                             ((c01 >> 8u) & 0xFFu) + ((c11 >> 8u) & 0xFFu) + 2u) >> 2u;
                    u32 b = (((c00 >> 16u) & 0xFFu) + ((c10 >> 16u) & 0xFFu) +
                             ((c01 >> 16u) & 0xFFu) + ((c11 >> 16u) & 0xFFu) + 2u) >> 2u;
                    u32 a = (((c00 >> 24u) & 0xFFu) + ((c10 >> 24u) & 0xFFu) +
                             ((c01 >> 24u) & 0xFFu) + ((c11 >> 24u) & 0xFFu) + 2u) >> 2u;
                    g_atlas_staging[y * nsz + x] = r | (g << 8u) | (b << 16u) | (a << 24u);
                }
            }
        }
    }
    return slot;
}

static void pack_mesh_vertices(GpuSceneVertex* dst, const Mesh* m, u32 batch_base) {
    for (u32 i = 0u; i < m->vertex_count; i++) {
        dst[i].px = m->px[i];
        dst[i].py = m->py[i];
        dst[i].pz = m->pz[i];
        dst[i].nx = m->nx ? m->nx[i] : 0.0f;
        dst[i].ny = m->ny ? m->ny[i] : 1.0f;
        dst[i].nz = m->nz ? m->nz[i] : 0.0f;
        dst[i].u  = m->u ? m->u[i] : 0.0f;
        dst[i].v  = m->v ? m->v[i] : 0.0f;
        dst[i].rgba = m->color ? m->color[i] : 0xFFFFFFFFu;
        dst[i].batch_base = batch_base;
    }
}

u32 gpu_scene_register_mesh(const Mesh* m, b32 is_dynamic, b32 is_alpha) {
    if (!m || m->vertex_count == 0u || m->index_count == 0u) return 0u;

    if (!is_dynamic) {
        for (u32 i = 0u; i < g_scene.mesh_count; i++) {
            if (!g_scene.meshes[i].is_dynamic &&
                g_scene.meshes[i].src_mesh == m &&
                g_scene.meshes[i].vertex_count == m->vertex_count &&
                g_scene.meshes[i].index_count == m->index_count &&
                g_scene.meshes[i].is_alpha == is_alpha) {
                return i;
            }
        }
    }

    if (g_scene.mesh_count >= GPU_SCENE_MAX_MESHES) return 0u;

    u32 base_v = 0u, first_i = 0u;
    if (is_dynamic) {
        if (g_scene.dyn_vb_used + m->vertex_count > SCENE_DYNAMIC_VB_CAP ||
            g_scene.dyn_ib_used + m->index_count > SCENE_DYNAMIC_IB_CAP) {
            return 0u;
        }
        base_v  = SCENE_STATIC_VB_CAP + g_scene.dyn_vb_used;
        first_i = SCENE_STATIC_IB_CAP + g_scene.dyn_ib_used;
    } else {
        if (g_scene.static_vb_used + m->vertex_count > SCENE_STATIC_VB_CAP ||
            g_scene.static_ib_used + m->index_count > SCENE_STATIC_IB_CAP) {
            return 0u;
        }
        base_v  = g_scene.static_vb_used;
        first_i = g_scene.static_ib_used;
    }

    u32 id = g_scene.mesh_count++;
    u32 batch_id = is_alpha ? (240u + g_scene.alpha_batch_count++) : g_scene.opaque_batch_count++;
    u32 batch_cap = (!is_dynamic && !is_alpha && m->vertex_count == 24u &&
                     m->index_count == 36u && m->radius < 1.8f &&
                     g_scene.visible_alloc_cursor < 102400u)
                        ? 102400u
                        : 1024u;
    if (g_scene.visible_alloc_cursor + batch_cap > SCENE_VISIBLE_CAP) {
        batch_cap = (SCENE_VISIBLE_CAP > g_scene.visible_alloc_cursor)
                        ? (SCENE_VISIBLE_CAP - g_scene.visible_alloc_cursor)
                        : 64u;
    }
    u32 batch_first = g_scene.visible_alloc_cursor;
    g_scene.visible_alloc_cursor += batch_cap;

    GpuMeshSlot* slot = &g_scene.meshes[id];
    slot->src_mesh         = m;
    slot->vertex_count     = m->vertex_count;
    slot->index_count      = m->index_count;
    slot->base_vertex      = base_v;
    slot->first_index      = first_i;
    slot->batch_id         = batch_id;
    slot->batch_first_inst = batch_first;
    slot->batch_max_inst   = batch_cap;
    slot->local_center     = m->center;
    slot->local_radius     = m->radius > 0.001f ? m->radius : 1.0f;
    slot->is_dynamic       = is_dynamic;
    slot->is_alpha         = is_alpha;

    if (is_dynamic) {
        pack_mesh_vertices(&g_dyn_verts[g_scene.dyn_vb_used], m, batch_first);
        __builtin_memcpy(&g_dyn_indices[g_scene.dyn_ib_used], m->indices, m->index_count * 4u);
        rhi_write_buffer(&g_scene.rhi, g_scene.buf_vb,
                         base_v * (u32)sizeof(GpuSceneVertex),
                         &g_dyn_verts[g_scene.dyn_vb_used],
                         m->vertex_count * (u32)sizeof(GpuSceneVertex));
        rhi_write_buffer(&g_scene.rhi, g_scene.buf_ib,
                         first_i * 4u,
                         &g_dyn_indices[g_scene.dyn_ib_used],
                         m->index_count * 4u);
        g_scene.dyn_vb_used += m->vertex_count;
        g_scene.dyn_ib_used += m->index_count;
    } else {
        usize mark = arena_mark(&g_scene.arena);
        GpuSceneVertex* tmp_v = ARENA_PUSH_ARRAY_RAW(&g_scene.arena, GpuSceneVertex, m->vertex_count);
        pack_mesh_vertices(tmp_v, m, batch_first);
        rhi_write_buffer(&g_scene.rhi, g_scene.buf_vb,
                         base_v * (u32)sizeof(GpuSceneVertex),
                         tmp_v, m->vertex_count * (u32)sizeof(GpuSceneVertex));
        rhi_write_buffer(&g_scene.rhi, g_scene.buf_ib,
                         first_i * 4u, m->indices, m->index_count * 4u);
        arena_pop_to(&g_scene.arena, mark);
        g_scene.static_vb_used += m->vertex_count;
        g_scene.static_ib_used += m->index_count;
    }

    return id;
}

/* --- Kare Çizim Akışı --- */

void gpu_scene_set_100k_mode(b32 enable) {
    g_scene.bench_100k_on = enable ? TRUE : FALSE;
}

b32 gpu_scene_100k_mode(void) {
    return g_scene.bench_100k_on;
}

b32 gpu_scene_begin_frame(const GpuSceneFrameDesc* desc) {
    if (!desc) return FALSE;
    if (!g_scene.initialized && !gpu_scene_init()) return FALSE;

    g_scene.frame_t0_ms = plat_time_ms();
    rhi_set_active(&g_scene.rhi);
    rhi_begin_frame(&g_scene.rhi);
    rhi_poll_errors(&g_scene.rhi);

    if (!ensure_gpu_resources()) return FALSE;

    u32 vp_w = engine_gpu_viewport_width();
    u32 vp_h = engine_gpu_viewport_height();
    if (vp_w == 0u) vp_w = 1920u;
    if (vp_h == 0u) vp_h = 1080u;
    if (vp_w != g_scene.depth_w || vp_h != g_scene.depth_h) {
        if (g_scene.tex_main_depth != RHI_INVALID_HANDLE) {
            rhi_destroy_resource(&g_scene.rhi, g_scene.tex_main_depth);
        }
        RhiTextureDesc ddesc = {
            .width      = vp_w,
            .height     = vp_h,
            .mip_levels = 1u,
            .format     = RHI_FMT_DEPTH32_FLOAT,
            .usage      = RHI_TEX_RENDER_ATTACHMENT
        };
        g_scene.tex_main_depth = rhi_create_texture(&g_scene.rhi, &ddesc);
        g_scene.depth_w = vp_w;
        g_scene.depth_h = vp_h;
    }

    /* Dinamik ağları (parçacık billboard vb.) kare başında sıfırla, statik ağları koru */
    while (g_scene.mesh_count > 0u && g_scene.meshes[g_scene.mesh_count - 1u].is_dynamic) {
        GpuMeshSlot* last = &g_scene.meshes[g_scene.mesh_count - 1u];
        if (last->is_alpha && g_scene.alpha_batch_count > 0u) g_scene.alpha_batch_count--;
        else if (!last->is_alpha && g_scene.opaque_batch_count > 0u) g_scene.opaque_batch_count--;
        g_scene.visible_alloc_cursor = last->batch_first_inst;
        g_scene.mesh_count--;
    }
    if (g_scene.mesh_count > 200u ||
        g_scene.static_vb_used > SCENE_STATIC_VB_CAP - 4096u ||
        g_scene.static_ib_used > SCENE_STATIC_IB_CAP - 12288u) {
        g_scene.mesh_count = 0u;
        g_scene.opaque_batch_count = 0u;
        g_scene.alpha_batch_count = 0u;
        g_scene.static_vb_used = 0u;
        g_scene.static_ib_used = 0u;
        g_scene.visible_alloc_cursor = 0u;
        g_scene.bench_100k_gpu_inited = FALSE;
    }
    g_scene.dyn_vb_used = 0u;
    g_scene.dyn_ib_used = 0u;
    g_scene.cur_dyn_inst_count = 0u;
    g_scene.light_count = 0u;
    g_scene.frame_desc = *desc;
    if (desc->stress_100k) {
        g_scene.bench_100k_on = TRUE;
    }

    return TRUE;
}

void gpu_scene_add_point_light(Vec3 pos, f32 radius, Vec3 color, f32 atten) {
    if (g_scene.light_count >= GPU_SCENE_MAX_LIGHTS) return;
    GpuPointLight* l = &g_scene.lights[g_scene.light_count++];
    l->pos[0]   = pos.x;
    l->pos[1]   = pos.y;
    l->pos[2]   = pos.z;
    l->radius   = radius;
    l->color[0] = color.x;
    l->color[1] = color.y;
    l->color[2] = color.z;
    l->atten    = atten;
}

b32 gpu_scene_submit_draw(const Mesh* m, const Mat4* model, u32 color,
                          const Texture* tex, f32 metallic, f32 roughness,
                          GpuShaderMode shade_mode, u32 inst_flags, b32 is_dynamic_mesh) {
    if (!m || !model || g_scene.cur_dyn_inst_count >= GPU_SCENE_DYN_CAPACITY) return FALSE;

    b32 is_alpha = (shade_mode == GPU_SHADE_PARTICLE);
    u32 mesh_id = gpu_scene_register_mesh(m, is_dynamic_mesh, is_alpha);
    u32 tex_id  = gpu_scene_register_texture(tex);
    const GpuMeshSlot* ms = &g_scene.meshes[mesh_id];

    const f32* mm = model->m;
    f32 sx2 = mm[0] * mm[0] + mm[1] * mm[1] + mm[2] * mm[2];
    f32 sy2 = mm[4] * mm[4] + mm[5] * mm[5] + mm[6] * mm[6];
    f32 sz2 = mm[8] * mm[8] + mm[9] * mm[9] + mm[10] * mm[10];
    f32 max_scale = m_sqrt(m_max(sx2, m_max(sy2, sz2)));

    u32 metal_u8 = (u32)(m_clamp(metallic, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 rough_u8 = (u32)(m_clamp(roughness, 0.0f, 1.0f) * 255.0f + 0.5f);

    GpuInstance inst;
    inst.r0[0] = mm[0]; inst.r0[1] = mm[4]; inst.r0[2] = mm[8];  inst.r0[3] = mm[12];
    inst.r1[0] = mm[1]; inst.r1[1] = mm[5]; inst.r1[2] = mm[9];  inst.r1[3] = mm[13];
    inst.r2[0] = mm[2]; inst.r2[1] = mm[6]; inst.r2[2] = mm[10]; inst.r2[3] = mm[14];
    inst.color = color;
    inst.mat_tex = GPU_MAT_TEX_PACK(metal_u8, rough_u8, tex_id,
                                    inst_flags | GPU_INST_ACTIVE, shade_mode);
    inst.mesh_batch = (mesh_id & 0xFFFFu) | ((ms->batch_id & 0xFFFFu) << 16u);
    inst.bound_radius = ms->local_radius * max_scale;

    u32 slot = g_scene.cur_dyn_inst_count++;
    gpu_scene_set_instance(slot, &inst);

    /* 100k stres modu için ilk katı küp/sütun ağını kaydet */
    if (!is_dynamic_mesh && !is_alpha && m->index_count == 36u) {
        if (g_scene.bench_100k_mesh_id != mesh_id || g_scene.bench_100k_batch_id != ms->batch_id) {
            g_scene.bench_100k_mesh_id    = mesh_id;
            g_scene.bench_100k_batch_id   = ms->batch_id;
            g_scene.bench_100k_gpu_inited = FALSE;
        }
    }

    return TRUE;
}

static void pack_u32_vec4(Vec4* dst, u32 x, u32 y, u32 z, u32 w) {
    u32 raw[4] = { x, y, z, w };
    __builtin_memcpy(dst, raw, 16u);
}

u32 gpu_scene_end_frame(void) {
    Rhi* rhi = &g_scene.rhi;
    const GpuSceneFrameDesc* fd = &g_scene.frame_desc;

    /* 1. Önceki karede aktif olup bu karede çizilmeyen dinamik yuvaları devre dışı bırak */
    for (u32 i = g_scene.cur_dyn_inst_count; i < g_scene.prev_dyn_inst_count; i++) {
        GpuInstance inactive = g_cpu_instances[i];
        inactive.mat_tex &= ~(GPU_INST_ACTIVE << 24u);
        gpu_scene_set_instance(i, &inactive);
    }
    g_scene.prev_dyn_inst_count = g_scene.cur_dyn_inst_count;

    /* 2. 100k stres modu aktifse her kare küçük bir animasyonlu alt kümeyi (16 örnek) kirli listeyle güncelle */
    u32 total_inst_limit = g_scene.cur_dyn_inst_count;
    if (g_scene.bench_100k_on) {
        total_inst_limit = GPU_SCENE_MAX_INSTANCES;
        u32 wave_base = 2400u + (((u32)(fd->time_sec * 60.0f) * 16u) % 99968u);
        u32 mb_packed = (g_scene.bench_100k_mesh_id & 0xFFFFu) |
                        ((g_scene.bench_100k_batch_id & 0xFFFFu) << 16u);
        for (u32 k = 0u; k < 16u; k++) {
            u32 idx = (wave_base - 2400u) + k;
            u32 col = idx % 400u;
            u32 row = idx / 400u;
            f32 fx = ((f32)col - 199.5f) * 1.45f;
            f32 fz = ((f32)row - 124.5f) * 1.45f - 18.0f;
            if (fx * fx + fz * fz < 64.0f) fz -= 14.0f;
            f32 sy = 0.6f + 0.35f * m_sin(fd->time_sec * 3.0f + (f32)k * 0.4f);
            GpuInstance d_inst;
            d_inst.r0[0] = 0.32f; d_inst.r0[1] = 0.0f; d_inst.r0[2] = 0.0f;  d_inst.r0[3] = fx;
            d_inst.r1[0] = 0.0f;  d_inst.r1[1] = sy;   d_inst.r1[2] = 0.0f;  d_inst.r1[3] = sy * 0.5f;
            d_inst.r2[0] = 0.0f;  d_inst.r2[1] = 0.0f; d_inst.r2[2] = 0.32f; d_inst.r2[3] = fz;
            d_inst.color = 0xFF50C0FFu;
            d_inst.mat_tex = GPU_MAT_TEX_PACK(200u, 60u, 1u + (k & 3u),
                                              GPU_INST_ACTIVE | GPU_INST_RECV_SHADOW, GPU_SHADE_PBR);
            d_inst.mesh_batch = mb_packed;
            d_inst.bound_radius = sy * 0.87f;
            gpu_scene_set_instance(2400u + idx, &d_inst);
        }
    }

    /* 3. Kirli listeyi (dirty list) GPU instance tamponuna boşalt */
    u32 dirty_spans = 0u, dirty_bytes = 0u;
    u32 dirty_cnt = gpu_scene_flush_dirty(rhi, g_scene.buf_instances, &dirty_spans, &dirty_bytes);

    /* 4. Yardımcı tampon (aux_buf): mesh sınırları, batch meta verileri ve nokta ışıklar */
    __builtin_memset(g_scene.aux_data, 0, sizeof(g_scene.aux_data));
    for (u32 m = 0u; m < g_scene.mesh_count; m++) {
        const GpuMeshSlot* ms = &g_scene.meshes[m];
        g_scene.aux_data[m & 255u] = vec4(ms->local_center.x, ms->local_center.y,
                                          ms->local_center.z, ms->local_radius);
        u32 b = ms->batch_id & 255u;
        pack_u32_vec4(&g_scene.aux_data[256u + b],
                      ms->batch_first_inst, ms->batch_max_inst,
                      ms->index_count, ms->first_index);
        pack_u32_vec4(&g_scene.aux_data[512u + b],
                      ms->base_vertex, ms->is_alpha ? 1u : 0u, m, 0u);
    }
    for (u32 l = 0u; l < g_scene.light_count; l++) {
        const GpuPointLight* pl = &g_scene.lights[l];
        g_scene.aux_data[768u + l * 2u + 0u] = vec4(pl->pos[0], pl->pos[1], pl->pos[2], pl->radius);
        g_scene.aux_data[768u + l * 2u + 1u] = vec4(pl->color[0], pl->color[1], pl->color[2], pl->atten);
    }
    u32 mb_100k = (g_scene.bench_100k_mesh_id & 0xFFFFu) |
                  ((g_scene.bench_100k_batch_id & 0xFFFFu) << 16u);
    pack_u32_vec4(&g_scene.aux_data[1023u], mb_100k, 0u, 0u, 0u);
    rhi_write_buffer(rhi, g_scene.buf_aux, 0u, g_scene.aux_data, sizeof(g_scene.aux_data));

    /* 5. Sahne Uniformları (Frustum düzlemleri + 3 CSM kademesi + Hi-Z örtücü kutusu) */
    GpuSceneUniforms* u = &g_scene.uniforms;
    __builtin_memset(u, 0, sizeof(*u));
    __builtin_memcpy(u->view,         fd->view.m,         64u);
    __builtin_memcpy(u->proj,         fd->proj.m,         64u);
    __builtin_memcpy(u->view_proj,    fd->view_proj.m,    64u);
    __builtin_memcpy(u->vm_view_proj, fd->vm_view_proj.m, 64u);

    Mat4 csm_vp[3], csm_tex[3];
    f32  csm_radii[3];
    gpu_scene_fit_csm(fd->eye, fd->cam_fwd, fd->sun_dir, csm_vp, csm_tex, csm_radii);
    for (u32 c = 0u; c < 3u; c++) {
        __builtin_memcpy(u->csm_vp[c],  csm_vp[c].m,  64u);
        __builtin_memcpy(u->csm_tex[c], csm_tex[c].m, 64u);
    }

    Vec4 planes[6];
    gpu_scene_extract_frustum_planes(fd->view_proj, planes);
    for (u32 p = 0u; p < 6u; p++) {
        u->frustum_planes[p][0] = planes[p].x;
        u->frustum_planes[p][1] = planes[p].y;
        u->frustum_planes[p][2] = planes[p].z;
        u->frustum_planes[p][3] = planes[p].w;
    }

    f32 near_z = fd->near_z > 0.01f ? fd->near_z : 0.1f;
    f32 far_z  = fd->far_z  > 2.0f  ? fd->far_z  : 120.0f;
    f32 fov_y  = fd->fov_y  > 0.1f  ? fd->fov_y  : (M_PI / 3.0f);
    f32 cot_fov = 1.0f / m_tan(fov_y * 0.5f);
    f32 aspect  = fd->aspect > 0.1f ? fd->aspect : (16.0f / 9.0f);

    u->eye_near[0] = fd->eye.x; u->eye_near[1] = fd->eye.y; u->eye_near[2] = fd->eye.z; u->eye_near[3] = near_z;
    u->fwd_far[0]  = fd->cam_fwd.x; u->fwd_far[1] = fd->cam_fwd.y; u->fwd_far[2] = fd->cam_fwd.z; u->fwd_far[3] = far_z;
    u->sun_dir_pad[0] = fd->sun_dir.x; u->sun_dir_pad[1] = fd->sun_dir.y; u->sun_dir_pad[2] = fd->sun_dir.z;
    u->sun_dir_pad[3] = fd->shadows_on ? 1.0f : 0.0f;
    u->sun_color_pad[0] = fd->sun_color.x; u->sun_color_pad[1] = fd->sun_color.y; u->sun_color_pad[2] = fd->sun_color.z;
    u->sun_color_pad[3] = fd->pbr_on ? 1.0f : 0.0f;
    u->sky_color_pad[0] = fd->sky_color.x; u->sky_color_pad[1] = fd->sky_color.y; u->sky_color_pad[2] = fd->sky_color.z;
    u->sky_color_pad[3] = fd->mipmap_on ? 1.0f : 0.0f;
    u->gnd_color_pad[0] = fd->ground_color.x; u->gnd_color_pad[1] = fd->ground_color.y; u->gnd_color_pad[2] = fd->ground_color.z;
    u->gnd_color_pad[3] = fd->time_sec;
    u->fog_color_start[0] = fd->fog_color.x; u->fog_color_start[1] = fd->fog_color.y; u->fog_color_start[2] = fd->fog_color.z;
    u->fog_color_start[3] = fd->fog_start;
    f32 fog_span = fd->fog_end - fd->fog_start;
    u->fog_params[0] = fd->fog_end;
    u->fog_params[1] = (fog_span > 0.001f) ? (1.0f / fog_span) : 0.0f;
    u->fog_params[2] = cot_fov;
    u->fog_params[3] = aspect;
    u->viewport_retro[0] = (f32)g_scene.depth_w;
    u->viewport_retro[1] = (f32)g_scene.depth_h;
    u->viewport_retro[2] = fd->retro_on ? 1.0f : 0.0f;
    u->viewport_retro[3] = (f32)fd->snap_subpixel;
    u->csm_radii_scan[0] = csm_radii[0];
    u->csm_radii_scan[1] = csm_radii[1];
    u->csm_radii_scan[2] = csm_radii[2];
    u->csm_radii_scan[3] = fd->retro_scanline;
    u->counts[0] = total_inst_limit;
    u->counts[1] = g_scene.opaque_batch_count + g_scene.alpha_batch_count;
    u->counts[2] = g_scene.light_count;
    u->counts[3] = g_scene.bench_100k_on ? 1u : 0u;

    /* Hi-Z birincil örtücü: 100k modunda ön plan monolit duvarını (0, 1.8, 4.0) ekran uzayına projekte et */
    if (g_scene.bench_100k_on) {
        Vec4 mv = mat4_mul_vec4(fd->view, vec4(0.0f, 1.8f, 4.0f, 1.0f));
        f32 mdist = -mv.z;
        if (mdist > near_z + 1.0f) {
            f32 hx = (2.4f * cot_fov / aspect) / mdist;
            f32 hy = (1.4f * cot_fov) / mdist;
            f32 nx = (mv.x * cot_fov / aspect) / mdist;
            f32 ny = (mv.y * cot_fov) / mdist;
            u->occluder_box[0]   = m_clamp((nx - hx) * 0.5f + 0.5f, 0.0f, 1.0f);
            u->occluder_box[1]   = m_clamp(0.5f - (ny + hy) * 0.5f, 0.0f, 1.0f);
            u->occluder_box[2]   = m_clamp((nx + hx) * 0.5f + 0.5f, 0.0f, 1.0f);
            u->occluder_box[3]   = m_clamp(0.5f - (ny - hy) * 0.5f, 0.0f, 1.0f);
            u->occluder_depth[0] = near_z / (mdist + 0.5f);
            u->occluder_depth[3] = 1.0f;
        } else {
            u->occluder_box[0]   = 2.0f;
            u->occluder_box[1]   = 2.0f;
            u->occluder_box[2]   = 1.0f;
            u->occluder_box[3]   = 1.0f;
            u->occluder_depth[0] = 0.0f;
            u->occluder_depth[3] = 0.0f;
        }
    } else {
        u->occluder_box[0]   = 2.0f;
        u->occluder_box[1]   = 2.0f;
        u->occluder_box[2]   = 1.0f;
        u->occluder_box[3]   = 1.0f;
        u->occluder_depth[0] = 0.0f;
        u->occluder_depth[3] = 1.0f;
    }

    rhi_write_buffer(rhi, g_scene.buf_uniforms, 0u, u, sizeof(*u));

    /* 6. Compute Geçişi: (isteğe bağlı 100k ilk kurulum) -> Hi-Z kurulum + indirgeme -> Culling -> Clustered Lights */
    rhi_begin_compute_pass(rhi);

    if (g_scene.bench_100k_on && !g_scene.bench_100k_gpu_inited) {
        rhi_set_compute_pipeline(rhi, g_scene.cpipe_init_100k);
        rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_init);
        rhi_dispatch(rhi, (GPU_SCENE_BENCH_INSTANCES + 63u) / 64u, 1u, 1u);
        g_scene.bench_100k_gpu_inited = TRUE;
    }

    rhi_set_compute_pipeline(rhi, g_scene.cpipe_reset_hiz);
    rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_reset);
    rhi_dispatch(rhi, (8192u + 63u) / 64u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, g_scene.cpipe_hiz_down_a);
    rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_down_a);
    rhi_dispatch(rhi, (2560u + 63u) / 64u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, g_scene.cpipe_hiz_down_b);
    rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_down_b);
    rhi_dispatch(rhi, 3u, 1u, 1u);

    u32 cull_groups = (total_inst_limit + 63u) / 64u;
    if (cull_groups == 0u) cull_groups = 1u;
    rhi_set_compute_pipeline(rhi, g_scene.cpipe_cull);
    rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_cull);
    rhi_dispatch(rhi, cull_groups, 1u, 1u);

    rhi_set_compute_pipeline(rhi, g_scene.cpipe_cluster);
    rhi_set_bind_group(rhi, 0u, g_scene.bg_compute_cluster);
    rhi_dispatch(rhi, GPU_SCENE_CLUSTER_COUNT / 64u, 1u, 1u);

    rhi_end_compute_pass(rhi);

    /* 7. 3-Kademeli CSM Gölge Haritası Geçişi (3072 x 1024) */
    if (fd->shadows_on && g_scene.opaque_batch_count > 0u) {
        RhiPassDesc spass = {
            .color_texture   = g_scene.tex_shadow_atlas,
            .depth_texture   = g_scene.tex_shadow_depth,
            .clear_color     = TRUE,
            .clear_r         = 0.0f,
            .clear_g         = 0.0f,
            .clear_b         = 0.0f,
            .clear_a         = 0.0f,
            .clear_depth     = TRUE,
            .clear_depth_val = 0.0f,
            .sample_count    = 1u
        };
        rhi_begin_pass(rhi, &spass);
        rhi_set_vertex_buffer(rhi, 0u, g_scene.buf_vb, 0u, 0u);
        rhi_set_index_buffer(rhi, g_scene.buf_ib, RHI_INDEX_UINT32, 0u, 0u);

        /* 100k modunda uzak kademede yalnız sahne ön plan nesnelerinin gölgesini çiz (batch 1..N) */
        for (u32 c = 0u; c < GPU_SCENE_CSM_CASCADES; c++) {
            rhi_set_viewport(rhi, (f32)(c * GPU_SCENE_CSM_SIZE), 0.0f,
                             (f32)GPU_SCENE_CSM_SIZE, (f32)GPU_SCENE_CSM_SIZE, 0.0f, 1.0f);
            rhi_set_pipeline(rhi, g_scene.rpipe_shadow[c]);
            rhi_set_bind_group(rhi, 0u, g_scene.bg_shadow[c]);
            rhi_draw_indexed_indirect(rhi, g_scene.buf_indirect, 0u, g_scene.opaque_batch_count);
        }
        rhi_end_pass(rhi);
    }

    /* 8. Ana Render Geçişi (Clustered Forward PBR + CSM + Dolaylı Çizim) */
    RhiPassDesc mpass = {
        .color_texture   = RHI_INVALID_HANDLE,
        .depth_texture   = g_scene.tex_main_depth,
        .clear_color     = TRUE,
        .clear_r         = fd->clear_color.x,
        .clear_g         = fd->clear_color.y,
        .clear_b         = fd->clear_color.z,
        .clear_a         = 1.0f,
        .clear_depth     = TRUE,
        .clear_depth_val = 0.0f,
        .sample_count    = 4u
    };
    rhi_begin_pass(rhi, &mpass);
    rhi_set_viewport(rhi, 0.0f, 0.0f, (f32)g_scene.depth_w, (f32)g_scene.depth_h, 0.0f, 1.0f);
    rhi_set_vertex_buffer(rhi, 0u, g_scene.buf_vb, 0u, 0u);
    rhi_set_index_buffer(rhi, g_scene.buf_ib, RHI_INDEX_UINT32, 0u, 0u);

    if (g_scene.opaque_batch_count > 0u) {
        rhi_set_pipeline(rhi, g_scene.rpipe_opaque);
        rhi_set_bind_group(rhi, 0u, g_scene.bg_render_opaque);
        rhi_draw_indexed_indirect(rhi, g_scene.buf_indirect, 0u, g_scene.opaque_batch_count);
    }
    if (g_scene.alpha_batch_count > 0u) {
        rhi_set_pipeline(rhi, g_scene.rpipe_alpha);
        rhi_set_bind_group(rhi, 0u, g_scene.bg_render_alpha);
        rhi_draw_indexed_indirect(rhi, g_scene.buf_indirect, 240u * 20u, g_scene.alpha_batch_count);
    }

    rhi_end_pass(rhi);
    rhi_present(rhi);

    f32 submit_ms = (f32)(plat_time_ms() - g_scene.frame_t0_ms);
    u32 active_cnt = g_scene.cur_dyn_inst_count + (g_scene.bench_100k_on ? GPU_SCENE_BENCH_INSTANCES : 0u);
    g_scene.stats.active_instances  = active_cnt;
    g_scene.stats.dirty_instances   = dirty_cnt;
    g_scene.stats.dirty_spans       = dirty_spans;
    g_scene.stats.dirty_bytes       = dirty_bytes;
    g_scene.stats.visible_instances = active_cnt;
    g_scene.stats.batch_count       = g_scene.opaque_batch_count + g_scene.alpha_batch_count;
    g_scene.stats.light_count       = g_scene.light_count;
    g_scene.stats.csm_cascades      = fd->shadows_on ? GPU_SCENE_CSM_CASCADES : 0u;
    g_scene.stats.cmd_bytes         = rhi_stream_bytes(rhi);
    g_scene.stats.cmd_count         = rhi_stream_cmd_count(rhi);
    g_scene.stats.cpu_submit_ms     = submit_ms;
    g_scene.stats.gpu_frame_us      = engine_gpu_frame_time_us();

    return rhi_stream_bytes(rhi);
}

const GpuSceneStats* gpu_scene_stats(void) {
    return &g_scene.stats;
}
