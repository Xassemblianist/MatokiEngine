/* gpu/gpu_fluid.c — GPU Akışkan / Parçacık Fiziği (Position Based Fluids — Macklin 2013, P1_F).
 * 9 Matoki @compute çekirdeği ile uzamsal hash ızgarası (counting sort + atomic_add + 3-seviye
 * prefix sum), yoğunluk kısıtı (lambda_i, delta_p_i + s_corr), girdap hapsi + XSPH viskozite
 * ve 4 geçişli ekran-uzayı akışkan çizimi (SSFR: derinlik -> bilateral yumuşatma -> kırılma/köpük).
 */
#include "gpu/gpu_fluid.h"
#include "matoki/matoki.h"
#include "matoki/ir.h"
#include "matoki/wgsl.h"
#include "core/log.h"

/* ---------------------------------------------------------------------------
 * Matoki @compute Çekirdek Kaynakları (shaders/compute/pbf_*.mtk ile birebir)
 * --------------------------------------------------------------------------- */

static const char k_mtk_pbf_init[] =
    "uniform particle_count: u32;\n"
    "uniform init_origin: vec4;\n"
    "uniform init_vel: vec4;\n"
    "buffer positions: buffer<vec4>;\n"
    "buffer velocities: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    if (idx < particle_count) {\n"
    "        let ix: u32 = idx & 63u;\n"
    "        let iz: u32 = (idx >> 6u) & 63u;\n"
    "        let iy: u32 = (idx >> 12u) & 63u;\n"
    "        let h1: u32 = (idx * 747796405u) + 2891336453u;\n"
    "        let h2: u32 = ((h1 >> 13u) ^ h1) * 1597334677u;\n"
    "        let jx: f32 = (f32(h1 & 255u) * 0.0039215686) - 0.5;\n"
    "        let jy: f32 = (f32((h1 >> 8u) & 255u) * 0.0039215686) - 0.5;\n"
    "        let jz: f32 = (f32(h2 & 255u) * 0.0039215686) - 0.5;\n"
    "        let sp: f32 = init_origin.w;\n"
    "        let ja: f32 = init_vel.w;\n"
    "        let px: f32 = init_origin.x + (f32(ix) + jx * ja) * sp;\n"
    "        let py: f32 = init_origin.y + (f32(iy) + jy * ja) * sp;\n"
    "        let pz: f32 = init_origin.z + (f32(iz) + jz * ja) * sp;\n"
    "        let h_factor: f32 = 1.35 - f32(iy) * 0.012;\n"
    "        let vx: f32 = init_vel.x * h_factor + jx * 0.15;\n"
    "        let vy: f32 = init_vel.y + jy * 0.10;\n"
    "        let vz: f32 = init_vel.z + jz * 0.15;\n"
    "        positions[idx] = vec4(px, py, pz, 1.0);\n"
    "        velocities[idx] = vec4(vx, vy, vz, 0.25);\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_grid_clear[] =
    "uniform cell_count: u32;\n"
    "buffer cell_counts: buffer<u32>;\n"
    "fn compute() {\n"
    "    let i: u32 = global_id;\n"
    "    if (i < cell_count) {\n"
    "        cell_counts[i] = 0u;\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_predict_hash[] =
    "uniform particle_count: u32;\n"
    "uniform grid_params: vec4;\n"
    "uniform box_min: vec4;\n"
    "uniform box_max: vec4;\n"
    "uniform kernel_params: vec4;\n"
    "uniform solver_params: vec4;\n"
    "uniform vort_params: vec4;\n"
    "uniform sphere0: vec4;\n"
    "uniform sphere0_vel: vec4;\n"
    "uniform sphere1: vec4;\n"
    "uniform box_obs: vec4;\n"
    "buffer positions: buffer<vec4>;\n"
    "buffer velocities: buffer<vec4>;\n"
    "buffer pred_pos: buffer<vec4>;\n"
    "buffer cell_counts: buffer<u32>;\n"
    "buffer particle_cell: buffer<u32>;\n"
    "buffer particle_rank: buffer<u32>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    if (idx < particle_count) {\n"
    "        let pos_f: vec4 = positions[idx];\n"
    "        let vel_f: vec4 = velocities[idx];\n"
    "        let dt: f32 = box_min.w;\n"
    "        let grav_y: f32 = box_max.w;\n"
    "        let vx: f32 = vel_f.x;\n"
    "        let vy: f32 = vel_f.y + grav_y * dt;\n"
    "        let vz: f32 = vel_f.z;\n"
    "        let px: f32 = clamp(pos_f.x + vx * dt, box_min.x, box_max.x);\n"
    "        let py: f32 = clamp(pos_f.y + vy * dt, box_min.y, box_max.y);\n"
    "        let pz: f32 = clamp(pos_f.z + vz * dt, box_min.z, box_max.z);\n"
    "        pred_pos[idx] = vec4(px, py, pz, pos_f.w);\n"
    "        let inv_cs: f32 = grid_params.w;\n"
    "        let gx: i32 = clamp(i32((px - grid_params.x) * inv_cs), 0, 63);\n"
    "        let gy: i32 = clamp(i32((py - grid_params.y) * inv_cs), 0, 63);\n"
    "        let gz: i32 = clamp(i32((pz - grid_params.z) * inv_cs), 0, 63);\n"
    "        let cell: u32 = u32(gx) + (u32(gz) << 6u) + (u32(gy) << 12u);\n"
    "        let rank: u32 = atomic_add(cell_counts, cell, 1u);\n"
    "        particle_cell[idx] = cell;\n"
    "        particle_rank[idx] = rank;\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_scan_block[] =
    "uniform block_count: u32;\n"
    "buffer in_counts: buffer<u32>;\n"
    "buffer out_offsets: buffer<u32>;\n"
    "buffer out_sums: buffer<u32>;\n"
    "fn compute() {\n"
    "    let b: u32 = global_id;\n"
    "    if (b < block_count) {\n"
    "        let base: u32 = b << 6u;\n"
    "        var acc: u32 = 0u;\n"
    "        for k in 0..64 {\n"
    "            let idx: u32 = base + u32(k);\n"
    "            let c: u32 = in_counts[idx];\n"
    "            out_offsets[idx] = acc;\n"
    "            acc = acc + c;\n"
    "        }\n"
    "        out_sums[b] = acc;\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_scan_combine[] =
    "uniform cell_count: u32;\n"
    "buffer cell_offsets: buffer<u32>;\n"
    "buffer l1_offsets: buffer<u32>;\n"
    "buffer l2_offsets: buffer<u32>;\n"
    "fn compute() {\n"
    "    let i: u32 = global_id;\n"
    "    if (i < cell_count) {\n"
    "        let b1: u32 = i >> 6u;\n"
    "        let b2: u32 = i >> 12u;\n"
    "        cell_offsets[i] = cell_offsets[i] + l1_offsets[b1] + l2_offsets[b2];\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_scatter[] =
    "uniform particle_count: u32;\n"
    "uniform grid_params: vec4;\n"
    "uniform box_min: vec4;\n"
    "uniform box_max: vec4;\n"
    "uniform kernel_params: vec4;\n"
    "uniform solver_params: vec4;\n"
    "uniform vort_params: vec4;\n"
    "uniform sphere0: vec4;\n"
    "uniform sphere0_vel: vec4;\n"
    "uniform sphere1: vec4;\n"
    "uniform box_obs: vec4;\n"
    "buffer positions: buffer<vec4>;\n"
    "buffer pred_pos: buffer<vec4>;\n"
    "buffer cell_offsets: buffer<u32>;\n"
    "buffer particle_cell: buffer<u32>;\n"
    "buffer particle_rank: buffer<u32>;\n"
    "buffer sorted_pos: buffer<vec4>;\n"
    "buffer sorted_orig_pos: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    if (idx < particle_count) {\n"
    "        let cell: u32 = particle_cell[idx];\n"
    "        let rank: u32 = particle_rank[idx];\n"
    "        let dst: u32 = cell_offsets[cell] + rank;\n"
    "        if (dst < particle_count) {\n"
    "            sorted_pos[dst] = pred_pos[idx];\n"
    "            sorted_orig_pos[dst] = positions[idx];\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_lambda[] =
    "uniform particle_count: u32;\n"
    "uniform grid_params: vec4;\n"
    "uniform box_min: vec4;\n"
    "uniform box_max: vec4;\n"
    "uniform kernel_params: vec4;\n"
    "uniform solver_params: vec4;\n"
    "uniform vort_params: vec4;\n"
    "uniform sphere0: vec4;\n"
    "uniform sphere0_vel: vec4;\n"
    "uniform sphere1: vec4;\n"
    "uniform box_obs: vec4;\n"
    "buffer sorted_pos: buffer<vec4>;\n"
    "buffer cell_counts: buffer<u32>;\n"
    "buffer cell_offsets: buffer<u32>;\n"
    "buffer lambda_buf: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    let last_p: u32 = max(particle_count, 1u) - 1u;\n"
    "    let i_safe: u32 = min(idx, last_p);\n"
    "    let pi_v4: vec4 = sorted_pos[i_safe];\n"
    "    let pi: vec3 = vec3(pi_v4.x, pi_v4.y, pi_v4.z);\n"
    "    let h: f32 = kernel_params.x;\n"
    "    let h2: f32 = kernel_params.y;\n"
    "    let poly6_s: f32 = kernel_params.z;\n"
    "    let spiky_s: f32 = kernel_params.w;\n"
    "    let eps_cfm: f32 = solver_params.x;\n"
    "    let inv_cs: f32 = grid_params.w;\n"
    "    let rx: f32 = (pi.x - grid_params.x) * inv_cs - 0.5;\n"
    "    let ry: f32 = (pi.y - grid_params.y) * inv_cs - 0.5;\n"
    "    let rz: f32 = (pi.z - grid_params.z) * inv_cs - 0.5;\n"
    "    let bx: i32 = clamp(i32(floor(rx)), 0, 62);\n"
    "    let by: i32 = clamp(i32(floor(ry)), 0, 62);\n"
    "    let bz: i32 = clamp(i32(floor(rz)), 0, 62);\n"
    "    var rho_norm: f32 = vort_params.w;\n"
    "    var grad_i: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    var sum_grad2: f32 = 0.0;\n"
    "    for oct in 0..8 {\n"
    "        let ox: i32 = bx + (oct & 1);\n"
    "        let oz: i32 = bz + ((oct >> 1) & 1);\n"
    "        let oy: i32 = by + ((oct >> 2) & 1);\n"
    "        let ncell: u32 = u32(ox) + (u32(oz) << 6u) + (u32(oy) << 12u);\n"
    "        let start: u32 = cell_offsets[ncell];\n"
    "        let cnt: u32 = min(cell_counts[ncell], 3u);\n"
    "        for s in 0..3 {\n"
    "            let su: u32 = u32(s);\n"
    "            let valid_m: u32 = u32((i32(su) - i32(cnt)) >> 31);\n"
    "            let raw_j: u32 = min(start + su, last_p);\n"
    "            let j: u32 = i_safe ^ ((raw_j ^ i_safe) & valid_m);\n"
    "            let pj_v4: vec4 = sorted_pos[j];\n"
    "            let diff: vec3 = pi - vec3(pj_v4.x, pj_v4.y, pj_v4.z);\n"
    "            let r2: f32 = dot(diff, diff);\n"
    "            let gate: f32 = step(0.00000001, r2) * step(r2, h2);\n"
    "            let d2: f32 = max(h2 - r2, 0.0) * gate;\n"
    "            rho_norm = rho_norm + poly6_s * d2 * d2 * d2;\n"
    "            let r: f32 = sqrt(max(r2, 0.00000001));\n"
    "            let hr: f32 = max(h - r, 0.0) * gate;\n"
    "            let gs: f32 = (spiky_s * hr * hr) / r;\n"
    "            let gj: vec3 = diff * gs;\n"
    "            grad_i = grad_i + gj;\n"
    "            sum_grad2 = sum_grad2 + dot(gj, gj);\n"
    "        }\n"
    "    }\n"
    "    sum_grad2 = sum_grad2 + dot(grad_i, grad_i);\n"
    "    let c_i: f32 = max(rho_norm - 1.0, -0.25);\n"
    "    let lam: f32 = -c_i / (sum_grad2 + eps_cfm);\n"
    "    if (idx < particle_count) {\n"
    "        lambda_buf[idx] = vec4(lam, rho_norm, c_i, 0.0);\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_delta[] =
    "uniform particle_count: u32;\n"
    "uniform grid_params: vec4;\n"
    "uniform box_min: vec4;\n"
    "uniform box_max: vec4;\n"
    "uniform kernel_params: vec4;\n"
    "uniform solver_params: vec4;\n"
    "uniform vort_params: vec4;\n"
    "uniform sphere0: vec4;\n"
    "uniform sphere0_vel: vec4;\n"
    "uniform sphere1: vec4;\n"
    "uniform box_obs: vec4;\n"
    "buffer sorted_pos: buffer<vec4>;\n"
    "buffer lambda_buf: buffer<vec4>;\n"
    "buffer cell_counts: buffer<u32>;\n"
    "buffer cell_offsets: buffer<u32>;\n"
    "buffer pred_pos: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    let last_p: u32 = max(particle_count, 1u) - 1u;\n"
    "    let i_safe: u32 = min(idx, last_p);\n"
    "    let pi_v4: vec4 = sorted_pos[i_safe];\n"
    "    let pi: vec3 = vec3(pi_v4.x, pi_v4.y, pi_v4.z);\n"
    "    let lam_v4: vec4 = lambda_buf[i_safe];\n"
    "    let lam_i: f32 = lam_v4.x;\n"
    "    let rho_i: f32 = lam_v4.y;\n"
    "    let h: f32 = kernel_params.x;\n"
    "    let h2: f32 = kernel_params.y;\n"
    "    let spiky_s: f32 = kernel_params.w;\n"
    "    let scorr_k: f32 = solver_params.y;\n"
    "    let inv_dq: f32 = solver_params.z;\n"
    "    let inv_cs: f32 = grid_params.w;\n"
    "    let rx: f32 = (pi.x - grid_params.x) * inv_cs - 0.5;\n"
    "    let ry: f32 = (pi.y - grid_params.y) * inv_cs - 0.5;\n"
    "    let rz: f32 = (pi.z - grid_params.z) * inv_cs - 0.5;\n"
    "    let bx: i32 = clamp(i32(floor(rx)), 0, 62);\n"
    "    let by: i32 = clamp(i32(floor(ry)), 0, 62);\n"
    "    let bz: i32 = clamp(i32(floor(rz)), 0, 62);\n"
    "    var dp: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    for oct in 0..8 {\n"
    "        let ox: i32 = bx + (oct & 1);\n"
    "        let oz: i32 = bz + ((oct >> 1) & 1);\n"
    "        let oy: i32 = by + ((oct >> 2) & 1);\n"
    "        let ncell: u32 = u32(ox) + (u32(oz) << 6u) + (u32(oy) << 12u);\n"
    "        let start: u32 = cell_offsets[ncell];\n"
    "        let cnt: u32 = min(cell_counts[ncell], 3u);\n"
    "        for s in 0..3 {\n"
    "            let su: u32 = u32(s);\n"
    "            let valid_m: u32 = u32((i32(su) - i32(cnt)) >> 31);\n"
    "            let raw_j: u32 = min(start + su, last_p);\n"
    "            let j: u32 = i_safe ^ ((raw_j ^ i_safe) & valid_m);\n"
    "            let pj_v4: vec4 = sorted_pos[j];\n"
    "            let diff: vec3 = pi - vec3(pj_v4.x, pj_v4.y, pj_v4.z);\n"
    "            let r2: f32 = dot(diff, diff);\n"
    "            let gate: f32 = step(0.00000001, r2) * step(r2, h2);\n"
    "            let lam_j: f32 = lambda_buf[j].x;\n"
    "            let diff2: f32 = max(h2 - r2, 0.0) * gate;\n"
    "            let w_ratio: f32 = diff2 * inv_dq;\n"
    "            let w_r2: f32 = w_ratio * w_ratio;\n"
    "            let scorr: f32 = scorr_k * w_r2 * w_r2;\n"
    "            let r: f32 = sqrt(max(r2, 0.00000001));\n"
    "            let hr: f32 = max(h - r, 0.0) * gate;\n"
    "            let gs: f32 = (spiky_s * hr * hr) / r;\n"
    "            dp = dp + diff * ((lam_i + lam_j + scorr) * gs);\n"
    "        }\n"
    "    }\n"
    "    let max_dp: f32 = h * 0.45;\n"
    "    var np: vec3 = pi + clamp(dp, vec3(-max_dp, -max_dp, -max_dp), vec3(max_dp, max_dp, max_dp));\n"
    "    let s0_c: vec3 = vec3(sphere0.x, sphere0.y, sphere0.z);\n"
    "    let d0: vec3 = np - s0_c;\n"
    "    let dist0: f32 = max(length(d0), 0.0001);\n"
    "    let push0: f32 = max(sphere0.w - dist0, 0.0);\n"
    "    np = np + d0 * (push0 / dist0);\n"
    "    let s1_c: vec3 = vec3(sphere1.x, sphere1.y, sphere1.z);\n"
    "    let d1: vec3 = np - s1_c;\n"
    "    let dist1: f32 = max(length(d1), 0.0001);\n"
    "    let push1: f32 = max(sphere1.w - dist1, 0.0);\n"
    "    np = np + d1 * (push1 / dist1);\n"
    "    let ob_c: vec3 = vec3(box_obs.x, np.y, box_obs.z);\n"
    "    let dob: vec3 = np - ob_c;\n"
    "    let dist_ob: f32 = max(length(dob), 0.0001);\n"
    "    let in_ob_y: f32 = step(np.y, box_obs.y + 0.85);\n"
    "    let push_ob: f32 = max(box_obs.w - dist_ob, 0.0) * in_ob_y;\n"
    "    np = np + dob * (push_ob / dist_ob);\n"
    "    np = clamp(np, vec3(box_min.x, box_min.y, box_min.z), vec3(box_max.x, box_max.y, box_max.z));\n"
    "    if (idx < particle_count) {\n"
    "        pred_pos[idx] = vec4(np.x, np.y, np.z, rho_i);\n"
    "    }\n"
    "}\n";

static const char k_mtk_pbf_vorticity_xsph[] =
    "uniform particle_count: u32;\n"
    "uniform grid_params: vec4;\n"
    "uniform box_min: vec4;\n"
    "uniform box_max: vec4;\n"
    "uniform kernel_params: vec4;\n"
    "uniform solver_params: vec4;\n"
    "uniform vort_params: vec4;\n"
    "uniform sphere0: vec4;\n"
    "uniform sphere0_vel: vec4;\n"
    "uniform sphere1: vec4;\n"
    "uniform box_obs: vec4;\n"
    "buffer pred_pos: buffer<vec4>;\n"
    "buffer sorted_orig_pos: buffer<vec4>;\n"
    "buffer cell_counts: buffer<u32>;\n"
    "buffer cell_offsets: buffer<u32>;\n"
    "buffer positions: buffer<vec4>;\n"
    "buffer velocities: buffer<vec4>;\n"
    "fn compute() {\n"
    "    let idx: u32 = global_id;\n"
    "    let last_p: u32 = max(particle_count, 1u) - 1u;\n"
    "    let i_safe: u32 = min(idx, last_p);\n"
    "    let pi_v4: vec4 = pred_pos[i_safe];\n"
    "    let pi: vec3 = vec3(pi_v4.x, pi_v4.y, pi_v4.z);\n"
    "    let orig_v4: vec4 = sorted_orig_pos[i_safe];\n"
    "    let orig_p: vec3 = vec3(orig_v4.x, orig_v4.y, orig_v4.z);\n"
    "    let dt: f32 = box_min.w;\n"
    "    let inv_dt: f32 = 1.0 / max(dt, 0.0001);\n"
    "    var vi: vec3 = (pi - orig_p) * inv_dt;\n"
    "    let h: f32 = kernel_params.x;\n"
    "    let h2: f32 = kernel_params.y;\n"
    "    let poly6_s: f32 = kernel_params.z;\n"
    "    let spiky_s: f32 = kernel_params.w;\n"
    "    let xsph_c: f32 = solver_params.w;\n"
    "    let vort_eps_dt: f32 = vort_params.x;\n"
    "    let wall_damp: f32 = vort_params.y;\n"
    "    let max_spd: f32 = vort_params.z;\n"
    "    let inv_cs: f32 = grid_params.w;\n"
    "    let rx: f32 = (pi.x - grid_params.x) * inv_cs - 0.5;\n"
    "    let ry: f32 = (pi.y - grid_params.y) * inv_cs - 0.5;\n"
    "    let rz: f32 = (pi.z - grid_params.z) * inv_cs - 0.5;\n"
    "    let bx: i32 = clamp(i32(floor(rx)), 0, 62);\n"
    "    let by: i32 = clamp(i32(floor(ry)), 0, 62);\n"
    "    let bz: i32 = clamp(i32(floor(rz)), 0, 62);\n"
    "    var xsph_sum: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    var omega: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    var eta: vec3 = vec3(0.0, 0.0, 0.0);\n"
    "    for oct in 0..8 {\n"
    "        let ox: i32 = bx + (oct & 1);\n"
    "        let oz: i32 = bz + ((oct >> 1) & 1);\n"
    "        let oy: i32 = by + ((oct >> 2) & 1);\n"
    "        let ncell: u32 = u32(ox) + (u32(oz) << 6u) + (u32(oy) << 12u);\n"
    "        let start: u32 = cell_offsets[ncell];\n"
    "        let cnt: u32 = min(cell_counts[ncell], 3u);\n"
    "        for s in 0..3 {\n"
    "            let su: u32 = u32(s);\n"
    "            let valid_m: u32 = u32((i32(su) - i32(cnt)) >> 31);\n"
    "            let raw_j: u32 = min(start + su, last_p);\n"
    "            let j: u32 = i_safe ^ ((raw_j ^ i_safe) & valid_m);\n"
    "            let pj_v4: vec4 = pred_pos[j];\n"
    "            let pj: vec3 = vec3(pj_v4.x, pj_v4.y, pj_v4.z);\n"
    "            let diff: vec3 = pi - pj;\n"
    "            let r2: f32 = dot(diff, diff);\n"
    "            let gate: f32 = step(0.00000001, r2) * step(r2, h2);\n"
    "            let oj_v4: vec4 = sorted_orig_pos[j];\n"
    "            let vj: vec3 = (pj - vec3(oj_v4.x, oj_v4.y, oj_v4.z)) * inv_dt;\n"
    "            let vij: vec3 = (vj - vi) * gate;\n"
    "            let d2: f32 = max(h2 - r2, 0.0) * gate;\n"
    "            let w: f32 = poly6_s * d2 * d2 * d2;\n"
    "            xsph_sum = xsph_sum + vij * w;\n"
    "            let r: f32 = sqrt(max(r2, 0.00000001));\n"
    "            let hr: f32 = max(h - r, 0.0) * gate;\n"
    "            let gs: f32 = (spiky_s * hr * hr) / r;\n"
    "            let grad_w: vec3 = diff * gs;\n"
    "            omega = omega + cross(vij, grad_w);\n"
    "            eta = eta + grad_w * length(vij);\n"
    "        }\n"
    "    }\n"
    "    let eta_len: f32 = max(length(eta), 0.0001);\n"
    "    let eta_gate: f32 = step(0.0002, eta_len);\n"
    "    let n_vort: vec3 = eta * (eta_gate / eta_len);\n"
    "    let f_vort: vec3 = cross(n_vort, omega) * vort_eps_dt;\n"
    "    vi = vi + xsph_sum * xsph_c + f_vort;\n"
    "    let s0_c: vec3 = vec3(sphere0.x, sphere0.y, sphere0.z);\n"
    "    let dist0: f32 = length(pi - s0_c);\n"
    "    let near_s0: f32 = step(dist0, sphere0.w + h * 0.6) * 0.35;\n"
    "    let s0_v: vec3 = vec3(sphere0_vel.x, sphere0_vel.y, sphere0_vel.z);\n"
    "    vi = mix(vi, s0_v, near_s0);\n"
    "    if (pi.x <= (box_min.x + 0.002) && vi.x < 0.0) { vi.x = -vi.x * wall_damp; }\n"
    "    if (pi.x >= (box_max.x - 0.002) && vi.x > 0.0) { vi.x = -vi.x * wall_damp; }\n"
    "    if (pi.y <= (box_min.y + 0.002) && vi.y < 0.0) { vi.y = -vi.y * wall_damp; }\n"
    "    if (pi.z <= (box_min.z + 0.002) && vi.z < 0.0) { vi.z = -vi.z * wall_damp; }\n"
    "    if (pi.z >= (box_max.z - 0.002) && vi.z > 0.0) { vi.z = -vi.z * wall_damp; }\n"
    "    let spd: f32 = length(vi);\n"
    "    if (spd > max_spd) {\n"
    "        vi = vi * (max_spd / spd);\n"
    "    }\n"
    "    let prev_foam: f32 = orig_v4.w;\n"
    "    let new_foam: f32 = clamp(prev_foam * 0.96 + clamp((spd - 1.1) * 0.14, 0.0, 0.45), 0.0, 1.0);\n"
    "    let norm_spd: f32 = clamp(spd / max_spd, 0.0, 1.0);\n"
    "    if (idx < particle_count) {\n"
    "        positions[idx] = vec4(pi.x, pi.y, pi.z, new_foam);\n"
    "        velocities[idx] = vec4(vi.x, vi.y, vi.z, norm_spd);\n"
    "    }\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * 4 Geçişli Ekran-Uzayı Akışkan Çizimi (SSFR) WGSL Şeyderleri
 * --------------------------------------------------------------------------- */

static const char k_wgsl_ssfr_bg[] =
    "struct SsfrUniforms {\n"
    "    view_proj: mat4x4<f32>,\n"
    "    inv_view_proj: mat4x4<f32>,\n"
    "    cam_pos: vec4<f32>,\n"
    "    cam_fwd: vec4<f32>,\n"
    "    cam_right: vec4<f32>,\n"
    "    cam_up: vec4<f32>,\n"
    "    box_min: vec4<f32>,\n"
    "    box_max: vec4<f32>,\n"
    "    sphere0: vec4<f32>,\n"
    "    sphere1: vec4<f32>,\n"
    "    box_obs: vec4<f32>,\n"
    "    params0: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u: SsfrUniforms;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) uv: vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {\n"
    "    var p = array<vec2<f32>, 3>(\n"
    "        vec2<f32>(-1.0, -1.0),\n"
    "        vec2<f32>( 3.0, -1.0),\n"
    "        vec2<f32>(-1.0,  3.0)\n"
    "    );\n"
    "    let xy = p[vid];\n"
    "    var out: VsOut;\n"
    "    out.pos = vec4<f32>(xy, 0.0, 1.0);\n"
    "    out.uv = vec2<f32>(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "fn ray_sphere(ro: vec3<f32>, rd: vec3<f32>, sph: vec4<f32>) -> f32 {\n"
    "    let oc = ro - sph.xyz;\n"
    "    let b = dot(oc, rd);\n"
    "    let c = dot(oc, oc) - sph.w * sph.w;\n"
    "    let h = b * b - c;\n"
    "    if (h < 0.0) { return -1.0; }\n"
    "    let t = -b - sqrt(h);\n"
    "    return select(-1.0, t, t > 0.001);\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    let ndc = vec4<f32>(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, 1.0, 1.0);\n"
    "    let wp4 = u.inv_view_proj * ndc;\n"
    "    let wp = wp4.xyz / max(wp4.w, 0.00001);\n"
    "    let ro = u.cam_pos.xyz;\n"
    "    let rd = normalize(wp - ro);\n"
    "    let sun_dir = normalize(vec3<f32>(0.52, 0.78, 0.34));\n"
    "\n"
    "    let sky_t = clamp(rd.y * 0.65 + 0.35, 0.0, 1.0);\n"
    "    var col = mix(vec3<f32>(0.76, 0.85, 0.94), vec3<f32>(0.18, 0.42, 0.72), sky_t);\n"
    "    let sun_dot = max(dot(rd, sun_dir), 0.0);\n"
    "    col = col + vec3<f32>(1.0, 0.92, 0.75) * pow(sun_dot, 96.0) * 0.6;\n"
    "    var best_t = 100.0;\n"
    "\n"
    "    if (rd.y < -0.0001) {\n"
    "        let tf = -ro.y / rd.y;\n"
    "        if (tf > 0.001 && tf < best_t) {\n"
    "            best_t = tf;\n"
    "            let hp = ro + rd * tf;\n"
    "            let cx = floor(hp.x * 2.0);\n"
    "            let cz = floor(hp.z * 2.0);\n"
    "            let chk = abs((cx + cz) - 2.0 * floor((cx + cz) * 0.5));\n"
    "            let base_c = mix(vec3<f32>(0.24, 0.27, 0.32), vec3<f32>(0.34, 0.38, 0.44), chk);\n"
    "            let fade = exp(-tf * 0.06);\n"
    "            col = mix(vec3<f32>(0.58, 0.68, 0.80), base_c, fade);\n"
    "        }\n"
    "    }\n"
    "\n"
    "    let bmin = u.box_min.xyz;\n"
    "    let bmax = u.box_max.xyz;\n"
    "    if (rd.y < -0.0001) {\n"
    "        let tb = (bmin.y - ro.y) / rd.y;\n"
    "        if (tb > 0.001 && tb < best_t) {\n"
    "            let hp = ro + rd * tb;\n"
    "            if (hp.x >= bmin.x && hp.x <= bmax.x && hp.z >= bmin.z && hp.z <= bmax.z) {\n"
    "                best_t = tb;\n"
    "                let tx = abs(fract(hp.x * 5.0) - 0.5);\n"
    "                let tz = abs(fract(hp.z * 5.0) - 0.5);\n"
    "                let line = smoothstep(0.44, 0.49, max(tx, tz));\n"
    "                let tile_c = mix(vec3<f32>(0.56, 0.72, 0.82), vec3<f32>(0.20, 0.34, 0.46), line);\n"
    "                let d_s0 = length(hp.xz - u.sphere0.xz);\n"
    "                let d_s1 = length(hp.xz - u.sphere1.xz);\n"
    "                let d_ob = length(hp.xz - u.box_obs.xz);\n"
    "                let sh0 = smoothstep(u.sphere0.w * 0.5, u.sphere0.w * 1.4, d_s0);\n"
    "                let sh1 = smoothstep(u.sphere1.w * 0.5, u.sphere1.w * 1.4, d_s1);\n"
    "                let sh2 = smoothstep(u.box_obs.w * 0.6, u.box_obs.w * 1.5, d_ob);\n"
    "                col = tile_c * (0.45 + 0.55 * sh0 * sh1 * sh2);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    if (rd.z < -0.0001) {\n"
    "        let tw = (bmin.z - ro.z) / rd.z;\n"
    "        if (tw > 0.001 && tw < best_t) {\n"
    "            let hp = ro + rd * tw;\n"
    "            if (hp.x >= bmin.x && hp.x <= bmax.x && hp.y >= bmin.y && hp.y <= bmax.y * 0.65) {\n"
    "                best_t = tw;\n"
    "                let gx = abs(fract(hp.x * 5.0) - 0.5);\n"
    "                let gy = abs(fract(hp.y * 5.0) - 0.5);\n"
    "                let gl = smoothstep(0.44, 0.49, max(gx, gy));\n"
    "                col = mix(vec3<f32>(0.44, 0.58, 0.70), vec3<f32>(0.18, 0.28, 0.38), gl);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    if (abs(rd.x) > 0.0001) {\n"
    "        let target_x = select(bmin.x, bmax.x, rd.x > 0.0);\n"
    "        let txw = (target_x - ro.x) / rd.x;\n"
    "        if (txw > 0.001 && txw < best_t) {\n"
    "            let hp = ro + rd * txw;\n"
    "            if (hp.z >= bmin.z && hp.z <= bmax.z && hp.y >= bmin.y && hp.y <= bmax.y * 0.65) {\n"
    "                best_t = txw;\n"
    "                let gz = abs(fract(hp.z * 5.0) - 0.5);\n"
    "                let gy = abs(fract(hp.y * 5.0) - 0.5);\n"
    "                let gl = smoothstep(0.44, 0.49, max(gz, gy));\n"
    "                col = mix(vec3<f32>(0.38, 0.52, 0.64), vec3<f32>(0.16, 0.25, 0.35), gl);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    let t_s0 = ray_sphere(ro, rd, u.sphere0);\n"
    "    if (t_s0 > 0.0 && t_s0 < best_t) {\n"
    "        best_t = t_s0;\n"
    "        let hp = ro + rd * t_s0;\n"
    "        let n = normalize(hp - u.sphere0.xyz);\n"
    "        let ndl = max(dot(n, sun_dir), 0.0);\n"
    "        let h_vec = normalize(sun_dir - rd);\n"
    "        let spec = pow(max(dot(n, h_vec), 0.0), 48.0);\n"
    "        let base = vec3<f32>(0.95, 0.58, 0.14);\n"
    "        col = base * (0.28 + 0.72 * ndl) + vec3<f32>(1.0, 0.95, 0.85) * spec * 0.65;\n"
    "    }\n"
    "\n"
    "    let t_s1 = ray_sphere(ro, rd, u.sphere1);\n"
    "    if (t_s1 > 0.0 && t_s1 < best_t) {\n"
    "        best_t = t_s1;\n"
    "        let hp = ro + rd * t_s1;\n"
    "        let n = normalize(hp - u.sphere1.xyz);\n"
    "        let ndl = max(dot(n, sun_dir), 0.0);\n"
    "        let h_vec = normalize(sun_dir - rd);\n"
    "        let spec = pow(max(dot(n, h_vec), 0.0), 40.0);\n"
    "        let stripe = smoothstep(-0.08, 0.08, sin(hp.y * 18.0));\n"
    "        let base = mix(vec3<f32>(0.88, 0.22, 0.18), vec3<f32>(0.94, 0.92, 0.88), stripe * 0.4);\n"
    "        col = base * (0.28 + 0.72 * ndl) + vec3<f32>(1.0) * spec * 0.5;\n"
    "    }\n"
    "\n"
    "    let ob_top = vec3<f32>(u.box_obs.x, u.box_obs.y + 0.65, u.box_obs.z);\n"
    "    let ob_bot = vec3<f32>(u.box_obs.x, u.box_obs.y + 0.15, u.box_obs.z);\n"
    "    let t_ob_top = ray_sphere(ro, rd, vec4<f32>(ob_top, u.box_obs.w));\n"
    "    let t_ob_mid = ray_sphere(ro, rd, vec4<f32>(0.5 * (ob_top + ob_bot), u.box_obs.w * 1.04));\n"
    "    let t_ob_bot = ray_sphere(ro, rd, vec4<f32>(ob_bot, u.box_obs.w));\n"
    "    var t_ob = 1000.0;\n"
    "    var ob_c = ob_top;\n"
    "    if (t_ob_top > 0.0 && t_ob_top < t_ob) { t_ob = t_ob_top; ob_c = ob_top; }\n"
    "    if (t_ob_mid > 0.0 && t_ob_mid < t_ob) { t_ob = t_ob_mid; ob_c = 0.5 * (ob_top + ob_bot); }\n"
    "    if (t_ob_bot > 0.0 && t_ob_bot < t_ob) { t_ob = t_ob_bot; ob_c = ob_bot; }\n"
    "    if (t_ob < best_t) {\n"
    "        best_t = t_ob;\n"
    "        let hp = ro + rd * t_ob;\n"
    "        let n = normalize(hp - ob_c);\n"
    "        let ndl = max(dot(n, sun_dir), 0.0);\n"
    "        let h_vec = normalize(sun_dir - rd);\n"
    "        let spec = pow(max(dot(n, h_vec), 0.0), 28.0);\n"
    "        let base = vec3<f32>(0.26, 0.30, 0.36);\n"
    "        col = base * (0.30 + 0.70 * ndl) + vec3<f32>(0.85, 0.90, 0.95) * spec * 0.35;\n"
    "    }\n"
    "\n"
    "    let hit_pos = ro + rd * best_t;\n"
    "    let eye_z = select(60.0, max(dot(hit_pos - ro, u.cam_fwd.xyz), 0.1), best_t < 99.0);\n"
    "    return vec4<f32>(col, eye_z);\n"
    "}\n";

static const char k_wgsl_ssfr_depth[] =
    "struct SsfrUniforms {\n"
    "    view_proj: mat4x4<f32>,\n"
    "    inv_view_proj: mat4x4<f32>,\n"
    "    cam_pos: vec4<f32>,\n"
    "    cam_fwd: vec4<f32>,\n"
    "    cam_right: vec4<f32>,\n"
    "    cam_up: vec4<f32>,\n"
    "    box_min: vec4<f32>,\n"
    "    box_max: vec4<f32>,\n"
    "    sphere0: vec4<f32>,\n"
    "    sphere1: vec4<f32>,\n"
    "    box_obs: vec4<f32>,\n"
    "    params0: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u: SsfrUniforms;\n"
    "@group(0) @binding(1) var<storage, read> positions: array<vec4<f32>>;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) quad_uv: vec2<f32>,\n"
    "    @location(1) eye_z: f32,\n"
    "    @location(2) foam: f32,\n"
    "    @location(3) radius: f32,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) vid: u32, @builtin(instance_index) iid: u32) -> VsOut {\n"
    "    var corners = array<vec2<f32>, 6>(\n"
    "        vec2<f32>(-1.0, -1.0),\n"
    "        vec2<f32>( 1.0, -1.0),\n"
    "        vec2<f32>( 1.0,  1.0),\n"
    "        vec2<f32>(-1.0, -1.0),\n"
    "        vec2<f32>( 1.0,  1.0),\n"
    "        vec2<f32>(-1.0,  1.0)\n"
    "    );\n"
    "    let p = positions[iid];\n"
    "    let q = corners[vid];\n"
    "    let rad = u.params0.x;\n"
    "    let world_p = p.xyz + (u.cam_right.xyz * q.x + u.cam_up.xyz * q.y) * rad;\n"
    "    let ez = dot(p.xyz - u.cam_pos.xyz, u.cam_fwd.xyz);\n"
    "    var out: VsOut;\n"
    "    out.pos = u.view_proj * vec4<f32>(world_p, 1.0);\n"
    "    out.quad_uv = q;\n"
    "    out.eye_z = ez;\n"
    "    out.foam = p.w;\n"
    "    out.radius = rad;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    let r2 = dot(in.quad_uv, in.quad_uv);\n"
    "    if (r2 > 1.0) {\n"
    "        discard;\n"
    "    }\n"
    "    let nz = sqrt(max(1.0 - r2, 0.0));\n"
    "    let z_surface = max(in.eye_z - nz * in.radius, 0.1);\n"
    "    let chord = 2.0 * nz * in.radius * 4.0;\n"
    "    return vec4<f32>(z_surface, in.foam, chord, 1.0);\n"
    "}\n";

static const char k_wgsl_ssfr_blur[] =
    "struct BlurUniforms {\n"
    "    step_dir: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> b: BlurUniforms;\n"
    "@group(0) @binding(1) var src_tex: texture_2d<f32>;\n"
    "@group(0) @binding(2) var src_smp: sampler;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) uv: vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {\n"
    "    var p = array<vec2<f32>, 3>(\n"
    "        vec2<f32>(-1.0, -1.0),\n"
    "        vec2<f32>( 3.0, -1.0),\n"
    "        vec2<f32>(-1.0,  3.0)\n"
    "    );\n"
    "    let xy = p[vid];\n"
    "    var out: VsOut;\n"
    "    out.pos = vec4<f32>(xy, 0.0, 1.0);\n"
    "    out.uv = vec2<f32>(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    let c0 = textureSampleLevel(src_tex, src_smp, in.uv, 0.0);\n"
    "    if (c0.a < 0.5 || c0.r > 50.0) {\n"
    "        return c0;\n"
    "    }\n"
    "    let z0 = c0.r;\n"
    "    let dir = b.step_dir.xy;\n"
    "    let depth_falloff = b.step_dir.z;\n"
    "    var sum_z = z0;\n"
    "    var sum_fg = c0.gb;\n"
    "    var w_sum = 1.0;\n"
    "    for (var k: i32 = -6; k <= 6; k = k + 1) {\n"
    "        if (k != 0) {\n"
    "            let fk = f32(k);\n"
    "            let uv_k = clamp(in.uv + dir * fk, vec2<f32>(0.001), vec2<f32>(0.999));\n"
    "            let ck = textureSampleLevel(src_tex, src_smp, uv_k, 0.0);\n"
    "            if (ck.a > 0.5 && ck.r < 50.0) {\n"
    "                let dz = ck.r - z0;\n"
    "                let w_spatial = exp(-0.045 * fk * fk);\n"
    "                let w_range = exp(-dz * dz * depth_falloff);\n"
    "                let w = w_spatial * w_range;\n"
    "                sum_z = sum_z + ck.r * w;\n"
    "                sum_fg = sum_fg + ck.gb * w;\n"
    "                w_sum = w_sum + w;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    let inv_w = 1.0 / w_sum;\n"
    "    return vec4<f32>(sum_z * inv_w, sum_fg.x * inv_w, sum_fg.y * inv_w, 1.0);\n"
    "}\n";

static const char k_wgsl_ssfr_comp[] =
    "struct SsfrUniforms {\n"
    "    view_proj: mat4x4<f32>,\n"
    "    inv_view_proj: mat4x4<f32>,\n"
    "    cam_pos: vec4<f32>,\n"
    "    cam_fwd: vec4<f32>,\n"
    "    cam_right: vec4<f32>,\n"
    "    cam_up: vec4<f32>,\n"
    "    box_min: vec4<f32>,\n"
    "    box_max: vec4<f32>,\n"
    "    sphere0: vec4<f32>,\n"
    "    sphere1: vec4<f32>,\n"
    "    box_obs: vec4<f32>,\n"
    "    params0: vec4<f32>,\n"
    "};\n"
    "@group(0) @binding(0) var<uniform> u: SsfrUniforms;\n"
    "@group(0) @binding(1) var bg_tex: texture_2d<f32>;\n"
    "@group(0) @binding(2) var fluid_tex: texture_2d<f32>;\n"
    "@group(0) @binding(3) var lin_smp: sampler;\n"
    "\n"
    "struct VsOut {\n"
    "    @builtin(position) pos: vec4<f32>,\n"
    "    @location(0) uv: vec2<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {\n"
    "    var p = array<vec2<f32>, 3>(\n"
    "        vec2<f32>(-1.0, -1.0),\n"
    "        vec2<f32>( 3.0, -1.0),\n"
    "        vec2<f32>(-1.0,  3.0)\n"
    "    );\n"
    "    let xy = p[vid];\n"
    "    var out: VsOut;\n"
    "    out.pos = vec4<f32>(xy, 0.0, 1.0);\n"
    "    out.uv = vec2<f32>(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);\n"
    "    return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VsOut) -> @location(0) vec4<f32> {\n"
    "    let bg = textureSampleLevel(bg_tex, lin_smp, in.uv, 0.0);\n"
    "    let fl = textureSampleLevel(fluid_tex, lin_smp, in.uv, 0.0);\n"
    "    var out_col = bg.rgb;\n"
    "\n"
    "    if (fl.a > 0.5 && fl.r < bg.a) {\n"
    "        let z_c = fl.r;\n"
    "        let du = u.params0.z;\n"
    "        let dv = u.params0.w;\n"
    "        let fl_r = textureSampleLevel(fluid_tex, lin_smp, in.uv + vec2<f32>(du, 0.0), 0.0);\n"
    "        let fl_l = textureSampleLevel(fluid_tex, lin_smp, in.uv - vec2<f32>(du, 0.0), 0.0);\n"
    "        let fl_d = textureSampleLevel(fluid_tex, lin_smp, in.uv + vec2<f32>(0.0, dv), 0.0);\n"
    "        let fl_u = textureSampleLevel(fluid_tex, lin_smp, in.uv - vec2<f32>(0.0, dv), 0.0);\n"
    "\n"
    "        var dz_dx = select(0.0, fl_r.r - z_c, fl_r.a > 0.5);\n"
    "        let dz_dx_l = select(0.0, z_c - fl_l.r, fl_l.a > 0.5);\n"
    "        if (abs(dz_dx_l) < abs(dz_dx) || fl_r.a < 0.5) { dz_dx = dz_dx_l; }\n"
    "\n"
    "        var dz_dy = select(0.0, fl_d.r - z_c, fl_d.a > 0.5);\n"
    "        let dz_dy_u = select(0.0, z_c - fl_u.r, fl_u.a > 0.5);\n"
    "        if (abs(dz_dy_u) < abs(dz_dy) || fl_d.a < 0.5) { dz_dy = dz_dy_u; }\n"
    "\n"
    "        let scale_xy = max(z_c * 0.0036, 0.0015);\n"
    "        let n_view = normalize(vec3<f32>(-dz_dx, dz_dy, scale_xy));\n"
    "        let n_world = normalize(\n"
    "            u.cam_right.xyz * n_view.x +\n"
    "            u.cam_up.xyz * n_view.y -\n"
    "            u.cam_fwd.xyz * n_view.z\n"
    "        );\n"
    "\n"
    "        let refr_off = vec2<f32>(n_view.x, -n_view.y) * (0.048 / max(z_c, 0.6));\n"
    "        let uv_refr = clamp(in.uv + refr_off, vec2<f32>(0.002), vec2<f32>(0.998));\n"
    "        let bg_r = textureSampleLevel(bg_tex, lin_smp, uv_refr, 0.0);\n"
    "        let refr_rgb = select(bg.rgb, bg_r.rgb, bg_r.a > z_c - 0.05);\n"
    "\n"
    "        let depth_diff = clamp((bg.a - z_c) * 0.85 + fl.b * 1.45, 0.10, 3.0);\n"
    "        let absorb = exp(-vec3<f32>(3.40, 0.72, 0.18) * depth_diff);\n"
    "        let shallow_tint = vec3<f32>(0.16, 0.76, 0.94);\n"
    "        let deep_water = vec3<f32>(0.015, 0.22, 0.48);\n"
    "        let refr_tinted = refr_rgb * mix(shallow_tint, vec3<f32>(1.0), 0.30) * absorb;\n"
    "        let body_col = mix(deep_water, refr_tinted, absorb.g * 0.72);\n"
    "\n"
    "        let sun_dir = normalize(vec3<f32>(0.52, 0.78, 0.34));\n"
    "        let ndv = clamp(n_view.z, 0.0, 1.0);\n"
    "        let fresnel = 0.04 + 0.90 * pow(1.0 - ndv, 4.0);\n"
    "        let refl_sky = mix(vec3<f32>(0.52, 0.76, 0.96), vec3<f32>(0.14, 0.42, 0.78), clamp(n_world.y * 0.5 + 0.5, 0.0, 1.0));\n"
    "        let h_vec = normalize(sun_dir - u.cam_fwd.xyz);\n"
    "        let spec = pow(max(dot(n_world, h_vec), 0.0), 84.0) * 1.25;\n"
    "        let sss = vec3<f32>(0.05, 0.52, 0.64) * clamp(1.0 - depth_diff * 0.45, 0.0, 1.0) * max(dot(n_world, sun_dir) + 0.4, 0.0) * 0.38;\n"
    "\n"
    "        let foam_amt = clamp((fl.g - 0.18) * 1.55, 0.0, 0.85);\n"
    "        let liquid_col = mix(body_col + sss, refl_sky, fresnel * 0.62) + vec3<f32>(1.0, 0.97, 0.90) * spec;\n"
    "        out_col = mix(liquid_col, vec3<f32>(0.92, 0.97, 1.0), foam_amt);\n"
    "    }\n"
    "\n"
    "    let mapped = out_col / (out_col + vec3<f32>(0.85)) * 1.38;\n"
    "    return vec4<f32>(clamp(mapped, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * GPU Üniform Paket Yapıları (std140 / 16B hizalı)
 * --------------------------------------------------------------------------- */

typedef struct {
    u32 particle_count;
    u32 _pad0;
    u32 _pad1;
    u32 _pad2;
    f32 init_origin[4];
    f32 init_vel[4];
} GpuFluidInitUniforms;

typedef struct {
    u32 count;
    u32 _pad0;
    u32 _pad1;
    u32 _pad2;
} GpuFluidCountUniforms;

typedef struct {
    u32 particle_count;
    u32 _pad0;
    u32 _pad1;
    u32 _pad2;
    f32 grid_params[4];     /* (grid_min.xyz, inv_cell_size) */
    f32 box_min[4];         /* (box_min.xyz, dt) */
    f32 box_max[4];         /* (box_max.xyz, gravity_y) */
    f32 kernel_params[4];   /* (h, h2, poly6_scale, spiky_scale) */
    f32 solver_params[4];   /* (eps_cfm, scorr_k, inv_dq_term, xsph_c) */
    f32 vort_params[4];     /* (vort_eps_dt, wall_damping, max_speed, self_density) */
    f32 sphere0[4];         /* (center.xyz, radius) */
    f32 sphere0_vel[4];     /* (vel.xyz, 0) */
    f32 sphere1[4];         /* (center.xyz, radius) */
    f32 box_obs[4];         /* (center.xyz, radius) */
} GpuFluidSimUniforms;

typedef struct {
    f32 view_proj[16];
    f32 inv_view_proj[16];
    f32 cam_pos[4];
    f32 cam_fwd[4];
    f32 cam_right[4];
    f32 cam_up[4];
    f32 box_min[4];
    f32 box_max[4];
    f32 sphere0[4];
    f32 sphere1[4];
    f32 box_obs[4];
    f32 params0[4]; /* (particle_radius, time_sec, inv_ssfr_w, inv_ssfr_h) */
} GpuFluidSsfrUniforms;

typedef struct {
    f32 step_dir[4]; /* (dx, dy, depth_falloff, 0) */
} GpuFluidBlurUniforms;

static RhiShader fluid_compile_mtk_cs(Rhi* rhi, Arena* scratch, const char* src, u32 src_len,
                                      const char* name) {
    usize mark = arena_mark(scratch);
    char* wgsl_buf = ARENA_PUSH_ARRAY(scratch, char, 131072u);
    MtkLayout layout;
    MtkDiag diag;
    MtkWgslOpts opts = { .frag_group = 0u, .entry_name = "main" };
    u32 wgsl_len = mtk_compile_wgsl(src, src_len, name, &opts, scratch,
                                    wgsl_buf, 131072u, &layout, &diag);
    RhiShader sh = RHI_INVALID_HANDLE;
    if (wgsl_len > 0u) {
        sh = rhi_create_shader(rhi, wgsl_buf, wgsl_len);
    } else {
        LOG("GPU Fluid compute WGSL hatasi (%s): %s", name, diag.text);
    }
    arena_pop_to(scratch, mark);
    return sh;
}

GpuFluidDesc gpu_fluid_default_desc(void) {
    GpuFluidDesc d;
    d.rest_density      = 1000.0f;
    d.smoothing_radius  = 0.065f;
    d.cell_size         = 0.050f;
    d.eps_cfm           = 600.0f;
    d.scorr_k           = -0.00008f;
    d.xsph_c            = 0.035f;
    d.vorticity_eps     = 0.35f;
    d.particle_radius   = 0.052f;
    d.solver_iterations = 2u;
    return d;
}

GpuFluidSceneParams gpu_fluid_default_scene(void) {
    GpuFluidSceneParams s;
    s.box_min     = vec3(-1.45f, 0.05f, -1.10f);
    s.box_max     = vec3( 1.45f, 2.85f,  1.10f);
    s.sphere0_pos = vec3( 0.42f, 0.36f,  0.12f);
    s.sphere0_rad = 0.32f;
    s.sphere0_vel = vec3( 0.0f,  0.0f,   0.0f);
    s.sphere1_pos = vec3(-0.35f, 0.28f, -0.38f);
    s.sphere1_rad = 0.24f;
    s.box_obs_pos = vec3( 0.02f, 0.05f,  0.18f);
    s.box_obs_rad = 0.22f;
    return s;
}

static void fluid_pack_sim_uniforms(const GpuFluidDesc* d, const GpuFluidSceneParams* sc,
                                    u32 particle_count, f32 dt, GpuFluidSimUniforms* u) {
    __builtin_memset(u, 0, sizeof(*u));
    u->particle_count = particle_count;

    f32 cs = d->cell_size > 0.005f ? d->cell_size : 0.050f;
    u->grid_params[0] = -1.60f;
    u->grid_params[1] =  0.00f;
    u->grid_params[2] = -1.60f;
    u->grid_params[3] = 1.0f / cs;

    f32 step_dt = m_clamp(dt, 0.004f, 0.016f);
    u->box_min[0] = sc->box_min.x;
    u->box_min[1] = sc->box_min.y;
    u->box_min[2] = sc->box_min.z;
    u->box_min[3] = step_dt;

    u->box_max[0] = sc->box_max.x;
    u->box_max[1] = sc->box_max.y;
    u->box_max[2] = sc->box_max.z;
    u->box_max[3] = -9.81f;

    f32 h  = d->smoothing_radius > 0.01f ? d->smoothing_radius : 0.065f;
    f32 h2 = h * h;
    f32 h4 = h2 * h2;
    f32 h6 = h4 * h2;
    f32 w0 = 0.18f;
    u->kernel_params[0] = h;
    u->kernel_params[1] = h2;
    u->kernel_params[2] = w0 / h6;
    u->kernel_params[3] = (-3.0f * w0) / h4;

    u->solver_params[0] = d->eps_cfm;
    u->solver_params[1] = d->scorr_k;
    u->solver_params[2] = 1.0f / (0.96f * h2);
    u->solver_params[3] = d->xsph_c;

    u->vort_params[0] = d->vorticity_eps * step_dt;
    u->vort_params[1] = 0.35f;
    u->vort_params[2] = 6.50f;
    u->vort_params[3] = w0;

    u->sphere0[0] = sc->sphere0_pos.x;
    u->sphere0[1] = sc->sphere0_pos.y;
    u->sphere0[2] = sc->sphere0_pos.z;
    u->sphere0[3] = sc->sphere0_rad;

    u->sphere0_vel[0] = sc->sphere0_vel.x;
    u->sphere0_vel[1] = sc->sphere0_vel.y;
    u->sphere0_vel[2] = sc->sphere0_vel.z;
    u->sphere0_vel[3] = 0.0f;

    u->sphere1[0] = sc->sphere1_pos.x;
    u->sphere1[1] = sc->sphere1_pos.y;
    u->sphere1[2] = sc->sphere1_pos.z;
    u->sphere1[3] = sc->sphere1_rad;

    u->box_obs[0] = sc->box_obs_pos.x;
    u->box_obs[1] = sc->box_obs_pos.y;
    u->box_obs[2] = sc->box_obs_pos.z;
    u->box_obs[3] = sc->box_obs_rad;
}

static void fluid_create_ssfr_targets(GpuFluid* fl, Rhi* rhi, u32 fb_w, u32 fb_h) {
    if (fb_w < 64u) fb_w = 640u;
    if (fb_h < 64u) fb_h = 360u;
    u32 sw = fb_w >> 1u;
    u32 sh = fb_h >> 1u;
    if (sw < 64u) sw = 64u;
    if (sh < 64u) sh = 64u;

    if (fl->scene_bg_tex != RHI_INVALID_HANDLE && fl->fb_w == fb_w && fl->fb_h == fb_h) {
        return;
    }

    if (fl->scene_bg_tex != RHI_INVALID_HANDLE)       rhi_destroy_resource(rhi, fl->scene_bg_tex);
    if (fl->fluid_depth_tex != RHI_INVALID_HANDLE)    rhi_destroy_resource(rhi, fl->fluid_depth_tex);
    if (fl->fluid_zbuf_tex != RHI_INVALID_HANDLE)     rhi_destroy_resource(rhi, fl->fluid_zbuf_tex);
    if (fl->fluid_blur_tmp_tex != RHI_INVALID_HANDLE) rhi_destroy_resource(rhi, fl->fluid_blur_tmp_tex);
    if (fl->fluid_smooth_tex != RHI_INVALID_HANDLE)   rhi_destroy_resource(rhi, fl->fluid_smooth_tex);
    if (fl->bg_pass_blur_h != RHI_INVALID_HANDLE)     rhi_destroy_resource(rhi, fl->bg_pass_blur_h);
    if (fl->bg_pass_blur_v != RHI_INVALID_HANDLE)     rhi_destroy_resource(rhi, fl->bg_pass_blur_v);
    if (fl->bg_pass_comp != RHI_INVALID_HANDLE)       rhi_destroy_resource(rhi, fl->bg_pass_comp);

    fl->fb_w   = fb_w;
    fl->fb_h   = fb_h;
    fl->ssfr_w = sw;
    fl->ssfr_h = sh;

    u32 rt_usage = RHI_TEX_SAMPLED | RHI_TEX_RENDER_ATTACHMENT;

    RhiTextureDesc td = {
        .width      = fb_w,
        .height     = fb_h,
        .mip_levels = 1u,
        .format     = RHI_FMT_RGBA16_FLOAT,
        .usage      = rt_usage
    };
    fl->scene_bg_tex = rhi_create_texture(rhi, &td);

    td.width  = sw;
    td.height = sh;
    fl->fluid_depth_tex    = rhi_create_texture(rhi, &td);
    fl->fluid_blur_tmp_tex = rhi_create_texture(rhi, &td);
    fl->fluid_smooth_tex   = rhi_create_texture(rhi, &td);

    td.format = RHI_FMT_DEPTH24_PLUS;
    td.usage  = RHI_TEX_RENDER_ATTACHMENT;
    fl->fluid_zbuf_tex = rhi_create_texture(rhi, &td);

    GpuFluidBlurUniforms bh = { { 1.85f / (f32)sw, 0.0f, 14.0f, 0.0f } };
    GpuFluidBlurUniforms bv = { { 0.0f, 1.85f / (f32)sh, 14.0f, 0.0f } };
    rhi_write_buffer(rhi, fl->blur_h_ubuf, 0u, &bh, (u32)sizeof(bh));
    rhi_write_buffer(rhi, fl->blur_v_ubuf, 0u, &bv, (u32)sizeof(bv));

    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_blur,
            .group_index = 0u,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = fl->blur_h_ubuf,     .size = (u32)sizeof(GpuFluidBlurUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = fl->fluid_depth_tex },
                { .binding = 2u, .kind = RHI_BIND_SAMPLER, .resource = fl->linear_sampler }
            },
            .entry_count = 3u
        };
        fl->bg_pass_blur_h = rhi_create_bind_group(rhi, &bg);

        bg.entries[0].resource = fl->blur_v_ubuf;
        bg.entries[1].resource = fl->fluid_blur_tmp_tex;
        fl->bg_pass_blur_v = rhi_create_bind_group(rhi, &bg);
    }

    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_comp,
            .group_index = 0u,
            .entries = {
                { .binding = 0u, .kind = RHI_BIND_BUFFER,  .resource = fl->ssfr_ubuf,        .size = (u32)sizeof(GpuFluidSsfrUniforms) },
                { .binding = 1u, .kind = RHI_BIND_TEXTURE, .resource = fl->scene_bg_tex },
                { .binding = 2u, .kind = RHI_BIND_TEXTURE, .resource = fl->fluid_smooth_tex },
                { .binding = 3u, .kind = RHI_BIND_SAMPLER, .resource = fl->linear_sampler }
            },
            .entry_count = 4u
        };
        fl->bg_pass_comp = rhi_create_bind_group(rhi, &bg);
    }
}

b32 gpu_fluid_init(GpuFluid* fl, Rhi* rhi, Arena* scratch, const GpuFluidDesc* desc) {
    if (!fl || !rhi || !scratch) return FALSE;
    __builtin_memset(fl, 0, sizeof(*fl));
    fl->desc  = desc ? *desc : gpu_fluid_default_desc();
    fl->scene = gpu_fluid_default_scene();

    /* 1. 9 Matoki compute çekirdeğini WGSL'e derle */
    fl->cs_init           = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_init, (u32)(sizeof(k_mtk_pbf_init) - 1u), "pbf_init.mtk");
    fl->cs_grid_clear     = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_grid_clear, (u32)(sizeof(k_mtk_pbf_grid_clear) - 1u), "pbf_grid_clear.mtk");
    fl->cs_predict_hash   = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_predict_hash, (u32)(sizeof(k_mtk_pbf_predict_hash) - 1u), "pbf_predict_hash.mtk");
    fl->cs_scan_block     = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_scan_block, (u32)(sizeof(k_mtk_pbf_scan_block) - 1u), "pbf_scan_block.mtk");
    fl->cs_scan_combine   = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_scan_combine, (u32)(sizeof(k_mtk_pbf_scan_combine) - 1u), "pbf_scan_combine.mtk");
    fl->cs_scatter        = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_scatter, (u32)(sizeof(k_mtk_pbf_scatter) - 1u), "pbf_scatter.mtk");
    fl->cs_lambda         = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_lambda, (u32)(sizeof(k_mtk_pbf_lambda) - 1u), "pbf_lambda.mtk");
    fl->cs_delta          = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_delta, (u32)(sizeof(k_mtk_pbf_delta) - 1u), "pbf_delta.mtk");
    fl->cs_vorticity_xsph = fluid_compile_mtk_cs(rhi, scratch, k_mtk_pbf_vorticity_xsph, (u32)(sizeof(k_mtk_pbf_vorticity_xsph) - 1u), "pbf_vorticity_xsph.mtk");

    if (!fl->cs_init || !fl->cs_grid_clear || !fl->cs_predict_hash || !fl->cs_scan_block ||
        !fl->cs_scan_combine || !fl->cs_scatter || !fl->cs_lambda || !fl->cs_delta || !fl->cs_vorticity_xsph) {
        return FALSE;
    }

    /* 2. Compute pipeline'ları */
    RhiComputePipelineDesc cpd = { .shader = fl->cs_init, .entry = "main" };
    fl->pipe_init           = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_grid_clear;     fl->pipe_grid_clear     = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_predict_hash;   fl->pipe_predict_hash   = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_scan_block;     fl->pipe_scan_block     = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_scan_combine;   fl->pipe_scan_combine   = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_scatter;        fl->pipe_scatter        = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_lambda;         fl->pipe_lambda         = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_delta;          fl->pipe_delta          = rhi_create_compute_pipeline(rhi, &cpd);
    cpd.shader = fl->cs_vorticity_xsph; fl->pipe_vorticity_xsph = rhi_create_compute_pipeline(rhi, &cpd);

    /* 3. GPU depolama (storage) tamponları (262.144 parçacık, 64^3 ızgara) */
    u32 st_usage   = RHI_BUF_STORAGE | RHI_BUF_COPY_DST | RHI_BUF_COPY_SRC;
    u32 vec4_bytes = GPU_FLUID_PARTICLES * 16u;
    u32 u32_bytes  = GPU_FLUID_GRID_CELLS * 4u;

    fl->pos_buf             = rhi_create_buffer(rhi, vec4_bytes, st_usage);
    fl->vel_buf             = rhi_create_buffer(rhi, vec4_bytes, st_usage);
    fl->pred_pos_buf        = rhi_create_buffer(rhi, vec4_bytes, st_usage);
    fl->sorted_pos_buf      = rhi_create_buffer(rhi, vec4_bytes, st_usage);
    fl->sorted_orig_pos_buf = rhi_create_buffer(rhi, vec4_bytes, st_usage);
    fl->lambda_buf          = rhi_create_buffer(rhi, vec4_bytes, st_usage);

    fl->cell_counts_buf   = rhi_create_buffer(rhi, u32_bytes, st_usage);
    fl->cell_offsets_buf  = rhi_create_buffer(rhi, u32_bytes, st_usage);
    fl->l1_sums_buf       = rhi_create_buffer(rhi, GPU_FLUID_L1_BLOCKS * 4u, st_usage);
    fl->l1_offsets_buf    = rhi_create_buffer(rhi, GPU_FLUID_L1_BLOCKS * 4u, st_usage);
    fl->l2_sums_buf       = rhi_create_buffer(rhi, GPU_FLUID_L2_BLOCKS * 4u, st_usage);
    fl->l2_offsets_buf    = rhi_create_buffer(rhi, GPU_FLUID_L2_BLOCKS * 4u, st_usage);
    fl->dummy_sum_buf     = rhi_create_buffer(rhi, 64u, st_usage);
    fl->particle_cell_buf = rhi_create_buffer(rhi, u32_bytes, st_usage);
    fl->particle_rank_buf = rhi_create_buffer(rhi, u32_bytes, st_usage);

    /* 4. Üniform tamponları */
    u32 ub_usage = RHI_BUF_UNIFORM | RHI_BUF_COPY_DST;
    fl->init_ubuf    = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidInitUniforms), ub_usage);
    fl->clear_ubuf   = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidCountUniforms), ub_usage);
    fl->scan_l0_ubuf = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidCountUniforms), ub_usage);
    fl->scan_l1_ubuf = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidCountUniforms), ub_usage);
    fl->scan_l2_ubuf = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidCountUniforms), ub_usage);
    fl->sim_ubuf     = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidSimUniforms), ub_usage);

    {
        GpuFluidCountUniforms cu = { GPU_FLUID_GRID_CELLS, 0u, 0u, 0u };
        rhi_write_buffer(rhi, fl->clear_ubuf, 0u, &cu, (u32)sizeof(cu));
        cu.count = GPU_FLUID_L1_BLOCKS;
        rhi_write_buffer(rhi, fl->scan_l0_ubuf, 0u, &cu, (u32)sizeof(cu));
        cu.count = GPU_FLUID_L2_BLOCKS;
        rhi_write_buffer(rhi, fl->scan_l1_ubuf, 0u, &cu, (u32)sizeof(cu));
        cu.count = 1u;
        rhi_write_buffer(rhi, fl->scan_l2_ubuf, 0u, &cu, (u32)sizeof(cu));
    }

    /* 5. Compute bind group'ları (hepsi <= 8 binding) */
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_init,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->init_ubuf, 0u, (u32)sizeof(GpuFluidInitUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->pos_buf,   0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->vel_buf,   0u, vec4_bytes }
            },
            .entry_count = 3u
        };
        fl->bg_init = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_grid_clear,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->clear_ubuf,      0u, (u32)sizeof(GpuFluidCountUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf, 0u, u32_bytes }
            },
            .entry_count = 2u
        };
        fl->bg_grid_clear = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_predict_hash,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->sim_ubuf,          0u, (u32)sizeof(GpuFluidSimUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->pos_buf,           0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->vel_buf,           0u, vec4_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->pred_pos_buf,      0u, vec4_bytes },
                { 4u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf,   0u, u32_bytes },
                { 5u, RHI_BIND_STORAGE_BUFFER, fl->particle_cell_buf, 0u, u32_bytes },
                { 6u, RHI_BIND_STORAGE_BUFFER, fl->particle_rank_buf, 0u, u32_bytes }
            },
            .entry_count = 7u
        };
        fl->bg_predict_hash = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_scan_block,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->scan_l0_ubuf,     0u, (u32)sizeof(GpuFluidCountUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf,  0u, u32_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf, 0u, u32_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->l1_sums_buf,      0u, GPU_FLUID_L1_BLOCKS * 4u }
            },
            .entry_count = 4u
        };
        fl->bg_scan_l0 = rhi_create_bind_group(rhi, &bg);

        bg.entries[0] = (RhiBindEntry){ 0u, RHI_BIND_BUFFER,         fl->scan_l1_ubuf,   0u, (u32)sizeof(GpuFluidCountUniforms) };
        bg.entries[1] = (RhiBindEntry){ 1u, RHI_BIND_STORAGE_BUFFER, fl->l1_sums_buf,    0u, GPU_FLUID_L1_BLOCKS * 4u };
        bg.entries[2] = (RhiBindEntry){ 2u, RHI_BIND_STORAGE_BUFFER, fl->l1_offsets_buf, 0u, GPU_FLUID_L1_BLOCKS * 4u };
        bg.entries[3] = (RhiBindEntry){ 3u, RHI_BIND_STORAGE_BUFFER, fl->l2_sums_buf,    0u, GPU_FLUID_L2_BLOCKS * 4u };
        fl->bg_scan_l1 = rhi_create_bind_group(rhi, &bg);

        bg.entries[0] = (RhiBindEntry){ 0u, RHI_BIND_BUFFER,         fl->scan_l2_ubuf,   0u, (u32)sizeof(GpuFluidCountUniforms) };
        bg.entries[1] = (RhiBindEntry){ 1u, RHI_BIND_STORAGE_BUFFER, fl->l2_sums_buf,    0u, GPU_FLUID_L2_BLOCKS * 4u };
        bg.entries[2] = (RhiBindEntry){ 2u, RHI_BIND_STORAGE_BUFFER, fl->l2_offsets_buf, 0u, GPU_FLUID_L2_BLOCKS * 4u };
        bg.entries[3] = (RhiBindEntry){ 3u, RHI_BIND_STORAGE_BUFFER, fl->dummy_sum_buf,  0u, 64u };
        fl->bg_scan_l2 = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_scan_combine,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->clear_ubuf,       0u, (u32)sizeof(GpuFluidCountUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf, 0u, u32_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->l1_offsets_buf,   0u, GPU_FLUID_L1_BLOCKS * 4u },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->l2_offsets_buf,   0u, GPU_FLUID_L2_BLOCKS * 4u }
            },
            .entry_count = 4u
        };
        fl->bg_scan_combine = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_scatter,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->sim_ubuf,            0u, (u32)sizeof(GpuFluidSimUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->pos_buf,             0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->pred_pos_buf,        0u, vec4_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf,    0u, u32_bytes },
                { 4u, RHI_BIND_STORAGE_BUFFER, fl->particle_cell_buf,   0u, u32_bytes },
                { 5u, RHI_BIND_STORAGE_BUFFER, fl->particle_rank_buf,   0u, u32_bytes },
                { 6u, RHI_BIND_STORAGE_BUFFER, fl->sorted_pos_buf,      0u, vec4_bytes },
                { 7u, RHI_BIND_STORAGE_BUFFER, fl->sorted_orig_pos_buf, 0u, vec4_bytes }
            },
            .entry_count = 8u
        };
        fl->bg_scatter = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_lambda,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->sim_ubuf,         0u, (u32)sizeof(GpuFluidSimUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->sorted_pos_buf,   0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf,  0u, u32_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf, 0u, u32_bytes },
                { 4u, RHI_BIND_STORAGE_BUFFER, fl->lambda_buf,       0u, vec4_bytes }
            },
            .entry_count = 5u
        };
        fl->bg_lambda_a = rhi_create_bind_group(rhi, &bg);

        bg.entries[1].resource = fl->pred_pos_buf;
        fl->bg_lambda_b = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_delta,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->sim_ubuf,         0u, (u32)sizeof(GpuFluidSimUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->sorted_pos_buf,   0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->lambda_buf,       0u, vec4_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf,  0u, u32_bytes },
                { 4u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf, 0u, u32_bytes },
                { 5u, RHI_BIND_STORAGE_BUFFER, fl->pred_pos_buf,     0u, vec4_bytes }
            },
            .entry_count = 6u
        };
        fl->bg_delta_a = rhi_create_bind_group(rhi, &bg);

        bg.entries[1].resource = fl->pred_pos_buf;
        bg.entries[5].resource = fl->sorted_pos_buf;
        fl->bg_delta_b = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_vorticity_xsph,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->sim_ubuf,            0u, (u32)sizeof(GpuFluidSimUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->pred_pos_buf,        0u, vec4_bytes },
                { 2u, RHI_BIND_STORAGE_BUFFER, fl->sorted_orig_pos_buf, 0u, vec4_bytes },
                { 3u, RHI_BIND_STORAGE_BUFFER, fl->cell_counts_buf,     0u, u32_bytes },
                { 4u, RHI_BIND_STORAGE_BUFFER, fl->cell_offsets_buf,    0u, u32_bytes },
                { 5u, RHI_BIND_STORAGE_BUFFER, fl->pos_buf,             0u, vec4_bytes },
                { 6u, RHI_BIND_STORAGE_BUFFER, fl->vel_buf,             0u, vec4_bytes }
            },
            .entry_count = 7u
        };
        fl->bg_vort_from_pred = rhi_create_bind_group(rhi, &bg);

        bg.entries[1].resource = fl->sorted_pos_buf;
        fl->bg_vort_from_sort = rhi_create_bind_group(rhi, &bg);
    }

    /* 6. SSFR grafik şeyderleri ve pipeline'ları */
    fl->sh_bg    = rhi_create_shader(rhi, k_wgsl_ssfr_bg,    (u32)(sizeof(k_wgsl_ssfr_bg) - 1u));
    fl->sh_depth = rhi_create_shader(rhi, k_wgsl_ssfr_depth, (u32)(sizeof(k_wgsl_ssfr_depth) - 1u));
    fl->sh_blur  = rhi_create_shader(rhi, k_wgsl_ssfr_blur,  (u32)(sizeof(k_wgsl_ssfr_blur) - 1u));
    fl->sh_comp  = rhi_create_shader(rhi, k_wgsl_ssfr_comp,  (u32)(sizeof(k_wgsl_ssfr_comp) - 1u));

    fl->ssfr_ubuf   = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidSsfrUniforms), ub_usage);
    fl->blur_h_ubuf = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidBlurUniforms), ub_usage);
    fl->blur_v_ubuf = rhi_create_buffer(rhi, (u32)sizeof(GpuFluidBlurUniforms), ub_usage);

    {
        RhiSamplerDesc smpd = {
            .address_u     = RHI_ADDR_CLAMP,
            .address_v     = RHI_ADDR_CLAMP,
            .mag_filter    = RHI_FILTER_LINEAR,
            .min_filter    = RHI_FILTER_LINEAR,
            .mipmap_filter = RHI_FILTER_NEAREST
        };
        fl->linear_sampler = rhi_create_sampler(rhi, &smpd);
    }

    {
        RhiPipelineDesc pd;
        __builtin_memset(&pd, 0, sizeof(pd));
        pd.vertex_shader   = fl->sh_bg;
        pd.fragment_shader = fl->sh_bg;
        pd.vs_entry        = "vs_main";
        pd.fs_entry        = "fs_main";
        pd.color_format    = RHI_FMT_RGBA16_FLOAT;
        pd.depth_format    = (RhiTextureFormat)0;
        pd.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
        pd.cull_mode       = RHI_CULL_NONE;
        fl->pipe_bg        = rhi_create_pipeline(rhi, &pd);
    }
    {
        RhiPipelineDesc pd;
        __builtin_memset(&pd, 0, sizeof(pd));
        pd.vertex_shader   = fl->sh_depth;
        pd.fragment_shader = fl->sh_depth;
        pd.vs_entry        = "vs_main";
        pd.fs_entry        = "fs_main";
        pd.color_format    = RHI_FMT_RGBA16_FLOAT;
        pd.depth_format    = RHI_FMT_DEPTH24_PLUS;
        pd.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
        pd.cull_mode       = RHI_CULL_NONE;
        pd.depth_test      = TRUE;
        pd.depth_write     = TRUE;
        pd.depth_compare   = RHI_CMP_LESS;
        fl->pipe_depth     = rhi_create_pipeline(rhi, &pd);
    }
    {
        RhiPipelineDesc pd;
        __builtin_memset(&pd, 0, sizeof(pd));
        pd.vertex_shader   = fl->sh_blur;
        pd.fragment_shader = fl->sh_blur;
        pd.vs_entry        = "vs_main";
        pd.fs_entry        = "fs_main";
        pd.color_format    = RHI_FMT_RGBA16_FLOAT;
        pd.depth_format    = (RhiTextureFormat)0;
        pd.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
        pd.cull_mode       = RHI_CULL_NONE;
        fl->pipe_blur      = rhi_create_pipeline(rhi, &pd);
    }
    {
        RhiPipelineDesc pd;
        __builtin_memset(&pd, 0, sizeof(pd));
        pd.vertex_shader   = fl->sh_comp;
        pd.fragment_shader = fl->sh_comp;
        pd.vs_entry        = "vs_main";
        pd.fs_entry        = "fs_main";
        pd.color_format    = RHI_FMT_SURFACE_DEFAULT;
        pd.depth_format    = (RhiTextureFormat)0;
        pd.topology        = RHI_TOPOLOGY_TRIANGLE_LIST;
        pd.cull_mode       = RHI_CULL_NONE;
        fl->pipe_comp      = rhi_create_pipeline(rhi, &pd);
    }

    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_bg,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER, fl->ssfr_ubuf, 0u, (u32)sizeof(GpuFluidSsfrUniforms) }
            },
            .entry_count = 1u
        };
        fl->bg_pass_bg = rhi_create_bind_group(rhi, &bg);
    }
    {
        RhiBindGroupDesc bg = {
            .pipeline    = fl->pipe_depth,
            .group_index = 0u,
            .entries = {
                { 0u, RHI_BIND_BUFFER,         fl->ssfr_ubuf, 0u, (u32)sizeof(GpuFluidSsfrUniforms) },
                { 1u, RHI_BIND_STORAGE_BUFFER, fl->pos_buf,   0u, vec4_bytes }
            },
            .entry_count = 2u
        };
        fl->bg_pass_depth = rhi_create_bind_group(rhi, &bg);
    }

    fluid_create_ssfr_targets(fl, rhi, 640u, 360u);
    fl->initialized      = TRUE;
    fl->particles_seeded = FALSE;
    return TRUE;
}

void gpu_fluid_reset_particles(GpuFluid* fl) {
    if (!fl) return;
    fl->particles_seeded = FALSE;
}

b32 gpu_fluid_encode(GpuFluid* fl, Rhi* rhi, const GpuFluidFrameParams* p, u32 fb_w, u32 fb_h) {
    if (!fl || !fl->initialized || !rhi || !p) return FALSE;

    fl->desc  = p->desc;
    fl->scene = p->scene;
    if (p->reset_particles) {
        fl->particles_seeded = FALSE;
    }

    fluid_create_ssfr_targets(fl, rhi, fb_w, fb_h);

    /* 1. PBF simülasyon üniformlarını güncelle */
    GpuFluidSimUniforms sim_u;
    fluid_pack_sim_uniforms(&fl->desc, &fl->scene, GPU_FLUID_PARTICLES, p->dt, &sim_u);
    rhi_write_buffer(rhi, fl->sim_ubuf, 0u, &sim_u, (u32)sizeof(sim_u));

    /* 2. Tek compute geçişinde tüm PBF aşamalarını yürüt */
    rhi_begin_compute_pass(rhi);

    if (!fl->particles_seeded) {
        GpuFluidInitUniforms iu;
        __builtin_memset(&iu, 0, sizeof(iu));
        iu.particle_count = GPU_FLUID_PARTICLES;
        iu.init_origin[0] = -1.38f;
        iu.init_origin[1] =  0.09f;
        iu.init_origin[2] = -0.78f;
        iu.init_origin[3] =  0.0245f;
        iu.init_vel[0]    =  1.65f;
        iu.init_vel[1]    = -0.35f;
        iu.init_vel[2]    =  0.00f;
        iu.init_vel[3]    =  0.14f;
        rhi_write_buffer(rhi, fl->init_ubuf, 0u, &iu, (u32)sizeof(iu));

        rhi_set_compute_pipeline(rhi, fl->pipe_init);
        rhi_set_bind_group(rhi, 0u, fl->bg_init);
        rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);
        fl->particles_seeded = TRUE;
    }

    /* Aşama 1: 64^3 ızgara sayaçlarını sıfırla */
    rhi_set_compute_pipeline(rhi, fl->pipe_grid_clear);
    rhi_set_bind_group(rhi, 0u, fl->bg_grid_clear);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    /* Aşama 2: Dış kuvvetler + konum tahmini + uzamsal hash + atomic_add */
    rhi_set_compute_pipeline(rhi, fl->pipe_predict_hash);
    rhi_set_bind_group(rhi, 0u, fl->bg_predict_hash);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    /* Aşama 3: 3-seviyeli hiyerarşik prefix sum (262.144 -> 4096 -> 64 -> 1) */
    rhi_set_compute_pipeline(rhi, fl->pipe_scan_block);
    rhi_set_bind_group(rhi, 0u, fl->bg_scan_l0);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_L1, 1u, 1u);

    rhi_set_bind_group(rhi, 0u, fl->bg_scan_l1);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_L2, 1u, 1u);

    rhi_set_bind_group(rhi, 0u, fl->bg_scan_l2);
    rhi_dispatch(rhi, 1u, 1u, 1u);

    rhi_set_compute_pipeline(rhi, fl->pipe_scan_combine);
    rhi_set_bind_group(rhi, 0u, fl->bg_scan_combine);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    /* Aşama 4: Counting-sort scatter */
    rhi_set_compute_pipeline(rhi, fl->pipe_scatter);
    rhi_set_bind_group(rhi, 0u, fl->bg_scatter);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    /* Aşama 5 & 6: PBF Yoğunluk kısıtı çözücü iterasyonları (lambda_i + delta_p_i + s_corr) */
    u32 iters = fl->desc.solver_iterations;
    if (iters < 1u) iters = 1u;
    if (iters > 2u) iters = 2u;

    rhi_set_compute_pipeline(rhi, fl->pipe_lambda);
    rhi_set_bind_group(rhi, 0u, fl->bg_lambda_a);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    rhi_set_compute_pipeline(rhi, fl->pipe_delta);
    rhi_set_bind_group(rhi, 0u, fl->bg_delta_a);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    if (iters >= 2u) {
        rhi_set_compute_pipeline(rhi, fl->pipe_lambda);
        rhi_set_bind_group(rhi, 0u, fl->bg_lambda_b);
        rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

        rhi_set_compute_pipeline(rhi, fl->pipe_delta);
        rhi_set_bind_group(rhi, 0u, fl->bg_delta_b);
        rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);
    }

    /* Aşama 7: Hız güncelleme + Girdap hapsi (vorticity confinement) + XSPH viskozite */
    rhi_set_compute_pipeline(rhi, fl->pipe_vorticity_xsph);
    rhi_set_bind_group(rhi, 0u, (iters >= 2u) ? fl->bg_vort_from_sort : fl->bg_vort_from_pred);
    rhi_dispatch(rhi, GPU_FLUID_GROUPS_PARTICLES, 1u, 1u);

    rhi_end_compute_pass(rhi);

    /* 3. SSFR (Screen-Space Fluid Rendering) üniformlarını hazırla */
    Mat4 vp  = mat4_mul(p->proj, p->view);
    Mat4 ivp = mat4_inverse(vp);
    {
        GpuFluidSsfrUniforms su;
        __builtin_memset(&su, 0, sizeof(su));
        for (u32 i = 0u; i < 16u; i++) {
            su.view_proj[i]     = vp.m[i];
            su.inv_view_proj[i] = ivp.m[i];
        }
        su.cam_pos[0] = p->cam_pos.x;
        su.cam_pos[1] = p->cam_pos.y;
        su.cam_pos[2] = p->cam_pos.z;
        su.cam_pos[3] = 1.0f;

        su.cam_right[0] = p->view.m[0];
        su.cam_right[1] = p->view.m[4];
        su.cam_right[2] = p->view.m[8];
        su.cam_up[0]    = p->view.m[1];
        su.cam_up[1]    = p->view.m[5];
        su.cam_up[2]    = p->view.m[9];
        su.cam_fwd[0]   = -p->view.m[2];
        su.cam_fwd[1]   = -p->view.m[6];
        su.cam_fwd[2]   = -p->view.m[10];

        su.box_min[0] = fl->scene.box_min.x;
        su.box_min[1] = fl->scene.box_min.y;
        su.box_min[2] = fl->scene.box_min.z;
        su.box_max[0] = fl->scene.box_max.x;
        su.box_max[1] = fl->scene.box_max.y;
        su.box_max[2] = fl->scene.box_max.z;

        su.sphere0[0] = fl->scene.sphere0_pos.x;
        su.sphere0[1] = fl->scene.sphere0_pos.y;
        su.sphere0[2] = fl->scene.sphere0_pos.z;
        su.sphere0[3] = fl->scene.sphere0_rad;

        su.sphere1[0] = fl->scene.sphere1_pos.x;
        su.sphere1[1] = fl->scene.sphere1_pos.y;
        su.sphere1[2] = fl->scene.sphere1_pos.z;
        su.sphere1[3] = fl->scene.sphere1_rad;

        su.box_obs[0] = fl->scene.box_obs_pos.x;
        su.box_obs[1] = fl->scene.box_obs_pos.y;
        su.box_obs[2] = fl->scene.box_obs_pos.z;
        su.box_obs[3] = fl->scene.box_obs_rad;

        su.params0[0] = fl->desc.particle_radius;
        su.params0[1] = p->time_sec;
        su.params0[2] = 1.0f / (f32)fl->ssfr_w;
        su.params0[3] = 1.0f / (f32)fl->ssfr_h;

        rhi_write_buffer(rhi, fl->ssfr_ubuf, 0u, &su, (u32)sizeof(su));
    }

    /* 4. SSFR Geçiş 1: Katı sahne + kova + katı cisimler -> scene_bg_tex */
    {
        RhiPassDesc pass = {
            .color_texture = fl->scene_bg_tex,
            .depth_texture = RHI_INVALID_HANDLE,
            .clear_color   = TRUE,
            .clear_r       = 0.20f,
            .clear_g       = 0.35f,
            .clear_b       = 0.55f,
            .clear_a       = 60.0f
        };
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, fl->pipe_bg);
        rhi_set_bind_group(rhi, 0u, fl->bg_pass_bg);
        rhi_draw(rhi, 3u, 1u, 0u, 0u);
        rhi_end_pass(rhi);
    }

    /* 5. SSFR Geçiş 2: 262.144 parçacık küre derinliği -> fluid_depth_tex */
    {
        RhiPassDesc pass = {
            .color_texture   = fl->fluid_depth_tex,
            .depth_texture   = fl->fluid_zbuf_tex,
            .clear_color     = TRUE,
            .clear_r         = 60.0f,
            .clear_g         = 0.0f,
            .clear_b         = 0.0f,
            .clear_a         = 0.0f,
            .clear_depth     = TRUE,
            .clear_depth_val = 1.0f
        };
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, fl->pipe_depth);
        rhi_set_bind_group(rhi, 0u, fl->bg_pass_depth);
        rhi_draw(rhi, 6u, GPU_FLUID_PARTICLES, 0u, 0u);
        rhi_end_pass(rhi);
    }

    /* 6. SSFR Geçiş 3a & 3b: Ayrılabilir Bilateral Derinlik Yumuşatma -> fluid_smooth_tex */
    {
        RhiPassDesc pass = {
            .color_texture = fl->fluid_blur_tmp_tex,
            .depth_texture = RHI_INVALID_HANDLE,
            .clear_color   = TRUE,
            .clear_r       = 60.0f,
            .clear_g       = 0.0f,
            .clear_b       = 0.0f,
            .clear_a       = 0.0f
        };
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, fl->pipe_blur);
        rhi_set_bind_group(rhi, 0u, fl->bg_pass_blur_h);
        rhi_draw(rhi, 3u, 1u, 0u, 0u);
        rhi_end_pass(rhi);

        pass.color_texture = fl->fluid_smooth_tex;
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, fl->pipe_blur);
        rhi_set_bind_group(rhi, 0u, fl->bg_pass_blur_v);
        rhi_draw(rhi, 3u, 1u, 0u, 0u);
        rhi_end_pass(rhi);
    }

    /* 7. SSFR Geçiş 4: Normal geri-çatımı + kırılma + Beer-Lambert + köpük -> Swapchain */
    {
        RhiPassDesc pass = {
            .color_texture = RHI_INVALID_HANDLE,
            .depth_texture = RHI_INVALID_HANDLE,
            .clear_color   = TRUE,
            .clear_r       = 0.12f,
            .clear_g       = 0.22f,
            .clear_b       = 0.38f,
            .clear_a       = 1.0f
        };
        rhi_begin_pass(rhi, &pass);
        rhi_set_pipeline(rhi, fl->pipe_comp);
        rhi_set_bind_group(rhi, 0u, fl->bg_pass_comp);
        rhi_draw(rhi, 3u, 1u, 0u, 0u);
        rhi_end_pass(rhi);
    }

    fl->frame_index++;
    return !rhi->overflow && rhi->dropped_cmds == 0u;
}

/* ---------------------------------------------------------------------------
 * Tam Çözünürlüklü GPU Akışkan Sahne Çizimi (kendi Rhi + Arena'sı ile)
 * --------------------------------------------------------------------------- */

typedef struct {
    Arena    arena;
    Rhi      rhi;
    GpuFluid fluid;
    b32      inited;
} GpuFluidRuntime;

static u8              g_gpu_fluid_mem[KB(512)] __attribute__((aligned(16)));
static GpuFluidRuntime g_gpu_fluid_rt;

b32 gpu_fluid_render(Arena* scratch, const GpuFluidFrameParams* p) {
    if (!scratch || !p) return FALSE;
    GpuFluidRuntime* rt = &g_gpu_fluid_rt;

    if (!rt->inited) {
        arena_init(&rt->arena, g_gpu_fluid_mem, sizeof(g_gpu_fluid_mem));
        if (!rhi_init(&rt->rhi, &rt->arena, 256u, KB(384))) {
            return FALSE;
        }
        rhi_set_active(&rt->rhi);
        if (!gpu_fluid_init(&rt->fluid, &rt->rhi, scratch, &p->desc)) {
            return FALSE;
        }
        rt->inited = TRUE;
    } else {
        rhi_set_active(&rt->rhi);
        rhi_begin_frame(&rt->rhi);
    }

    rhi_poll_errors(&rt->rhi);

    u32 vp_w = engine_gpu_viewport_width();
    u32 vp_h = engine_gpu_viewport_height();
    if (vp_w == 0u) vp_w = 1280u;
    if (vp_h == 0u) vp_h = 720u;

    if (!gpu_fluid_encode(&rt->fluid, &rt->rhi, p, vp_w, vp_h)) {
        return FALSE;
    }
    rhi_present(&rt->rhi);
    return !rt->rhi.overflow && rt->rhi.dropped_cmds == 0u;
}

/* ---------------------------------------------------------------------------
 * Test Yardımcısı: Matoki @compute IR Yorumlayıcısı Üzerinde Tam PBF Adımı
 * --------------------------------------------------------------------------- */

static b32 fluid_build_ir_fn(Arena* scratch, const char* src, u32 src_len,
                             const char* name, IrFunc* out_fn) {
    MtkLayout lay;
    MtkDiag diag;
    __builtin_memset(out_fn, 0, sizeof(*out_fn));
    __builtin_memset(&lay, 0, sizeof(lay));
    mtk_diag_init(&diag);
    if (!mtk_compile_ir(src, src_len, name, scratch, out_fn, &lay, &diag)) {
        LOG("PBF IR derleme hatasi (%s): %s", name, diag.text);
        return FALSE;
    }
    ir_optimize(out_fn);
    return TRUE;
}

b32 gpu_fluid_test_step_cpu(Arena* scratch, Vec4* positions, Vec4* velocities, u32 count,
                            const GpuFluidSceneParams* scene, f32 dt,
                            f32* out_max_density_err, u32* out_sorted_total) {
    if (!scratch || !positions || !velocities || count == 0u || !scene) return FALSE;
    usize mark = arena_mark(scratch);

    IrFunc fn_clear, fn_predict, fn_scan, fn_combine, fn_scatter, fn_lambda, fn_delta, fn_vort;
    if (!fluid_build_ir_fn(scratch, k_mtk_pbf_grid_clear, (u32)(sizeof(k_mtk_pbf_grid_clear) - 1u), "pbf_grid_clear.mtk", &fn_clear) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_predict_hash, (u32)(sizeof(k_mtk_pbf_predict_hash) - 1u), "pbf_predict_hash.mtk", &fn_predict) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_scan_block, (u32)(sizeof(k_mtk_pbf_scan_block) - 1u), "pbf_scan_block.mtk", &fn_scan) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_scan_combine, (u32)(sizeof(k_mtk_pbf_scan_combine) - 1u), "pbf_scan_combine.mtk", &fn_combine) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_scatter, (u32)(sizeof(k_mtk_pbf_scatter) - 1u), "pbf_scatter.mtk", &fn_scatter) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_lambda, (u32)(sizeof(k_mtk_pbf_lambda) - 1u), "pbf_lambda.mtk", &fn_lambda) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_delta, (u32)(sizeof(k_mtk_pbf_delta) - 1u), "pbf_delta.mtk", &fn_delta) ||
        !fluid_build_ir_fn(scratch, k_mtk_pbf_vorticity_xsph, (u32)(sizeof(k_mtk_pbf_vorticity_xsph) - 1u), "pbf_vorticity_xsph.mtk", &fn_vort)) {
        arena_pop_to(scratch, mark);
        return FALSE;
    }

    GpuFluidDesc d = gpu_fluid_default_desc();
    GpuFluidSimUniforms sim_u;
    fluid_pack_sim_uniforms(&d, scene, count, dt, &sim_u);

    Vec4* pred_pos        = ARENA_PUSH_ARRAY(scratch, Vec4, count);
    Vec4* sorted_pos      = ARENA_PUSH_ARRAY(scratch, Vec4, count);
    Vec4* sorted_orig_pos = ARENA_PUSH_ARRAY(scratch, Vec4, count);
    Vec4* lambda_buf      = ARENA_PUSH_ARRAY(scratch, Vec4, count);
    u32*  cell_counts     = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_GRID_CELLS);
    u32*  cell_offsets    = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_GRID_CELLS);
    u32*  l1_sums         = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_L1_BLOCKS);
    u32*  l1_offsets      = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_L1_BLOCKS);
    u32*  l2_sums         = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_L2_BLOCKS);
    u32*  l2_offsets      = ARENA_PUSH_ARRAY(scratch, u32, GPU_FLUID_L2_BLOCKS);
    u32*  dummy_sum       = ARENA_PUSH_ARRAY(scratch, u32, 4u);
    u32*  particle_cell   = ARENA_PUSH_ARRAY(scratch, u32, count);
    u32*  particle_rank   = ARENA_PUSH_ARRAY(scratch, u32, count);

    __builtin_memset(cell_counts, 0, GPU_FLUID_GRID_CELLS * sizeof(u32));

    /* 1. pbf_predict_hash (ir_eval_compute) */
    {
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = count;
        b.total_count  = count;
        b.buffer_count = 6u;
        b.uniforms     = (const u8*)&sim_u;
        b.buffers[0]   = positions;
        b.buffers[1]   = velocities;
        b.buffers[2]   = pred_pos;
        b.buffers[3]   = cell_counts;
        b.buffers[4]   = particle_cell;
        b.buffers[5]   = particle_rank;
        ir_eval_compute(&fn_predict, &b);
    }

    /* 2. 3-seviye pbf_scan_block + pbf_scan_combine (ir_eval_compute) */
    {
        GpuFluidCountUniforms cu = { GPU_FLUID_L1_BLOCKS, 0u, 0u, 0u };
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = GPU_FLUID_L1_BLOCKS;
        b.total_count  = GPU_FLUID_L1_BLOCKS;
        b.buffer_count = 3u;
        b.uniforms     = (const u8*)&cu;
        b.buffers[0]   = cell_counts;
        b.buffers[1]   = cell_offsets;
        b.buffers[2]   = l1_sums;
        ir_eval_compute(&fn_scan, &b);

        cu.count      = GPU_FLUID_L2_BLOCKS;
        b.count       = GPU_FLUID_L2_BLOCKS;
        b.total_count = GPU_FLUID_L2_BLOCKS;
        b.buffers[0]  = l1_sums;
        b.buffers[1]  = l1_offsets;
        b.buffers[2]  = l2_sums;
        ir_eval_compute(&fn_scan, &b);

        cu.count      = 1u;
        b.count       = 1u;
        b.total_count = 1u;
        b.buffers[0]  = l2_sums;
        b.buffers[1]  = l2_offsets;
        b.buffers[2]  = dummy_sum;
        ir_eval_compute(&fn_scan, &b);

        cu.count      = GPU_FLUID_GRID_CELLS;
        b.count       = GPU_FLUID_GRID_CELLS;
        b.total_count = GPU_FLUID_GRID_CELLS;
        b.buffers[0]  = cell_offsets;
        b.buffers[1]  = l1_offsets;
        b.buffers[2]  = l2_offsets;
        ir_eval_compute(&fn_combine, &b);

        if (out_sorted_total) *out_sorted_total = dummy_sum[0];
    }

    /* 3. pbf_scatter (ir_eval_compute) */
    {
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = count;
        b.total_count  = count;
        b.buffer_count = 7u;
        b.uniforms     = (const u8*)&sim_u;
        b.buffers[0]   = positions;
        b.buffers[1]   = pred_pos;
        b.buffers[2]   = cell_offsets;
        b.buffers[3]   = particle_cell;
        b.buffers[4]   = particle_rank;
        b.buffers[5]   = sorted_pos;
        b.buffers[6]   = sorted_orig_pos;
        ir_eval_compute(&fn_scatter, &b);
    }

    /* 4. pbf_lambda (ir_eval_compute) */
    {
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = count;
        b.total_count  = count;
        b.buffer_count = 4u;
        b.uniforms     = (const u8*)&sim_u;
        b.buffers[0]   = sorted_pos;
        b.buffers[1]   = cell_counts;
        b.buffers[2]   = cell_offsets;
        b.buffers[3]   = lambda_buf;
        ir_eval_compute(&fn_lambda, &b);
    }

    if (out_max_density_err) {
        f32 max_err = 0.0f;
        for (u32 i = 0u; i < count; i++) {
            f32 c_i = lambda_buf[i].z;
            if (c_i > max_err) max_err = c_i;
        }
        *out_max_density_err = max_err;
    }

    /* 5. pbf_delta (ir_eval_compute) */
    {
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = count;
        b.total_count  = count;
        b.buffer_count = 5u;
        b.uniforms     = (const u8*)&sim_u;
        b.buffers[0]   = sorted_pos;
        b.buffers[1]   = lambda_buf;
        b.buffers[2]   = cell_counts;
        b.buffers[3]   = cell_offsets;
        b.buffers[4]   = pred_pos;
        ir_eval_compute(&fn_delta, &b);
    }

    /* 6. pbf_vorticity_xsph (ir_eval_compute) */
    {
        MtkComputeBatch b;
        __builtin_memset(&b, 0, sizeof(b));
        b.start        = 0u;
        b.count        = count;
        b.total_count  = count;
        b.buffer_count = 6u;
        b.uniforms     = (const u8*)&sim_u;
        b.buffers[0]   = pred_pos;
        b.buffers[1]   = sorted_orig_pos;
        b.buffers[2]   = cell_counts;
        b.buffers[3]   = cell_offsets;
        b.buffers[4]   = positions;
        b.buffers[5]   = velocities;
        ir_eval_compute(&fn_vort, &b);
    }

    arena_pop_to(scratch, mark);
    return TRUE;
}
