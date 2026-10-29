/* tests/test_matoki_compute.c — Matoki compute shader (F0_A) birim ve diferansiyel testleri.
 * Sözdizimi/sema hata denetimlerini, 3 örnek compute çekirdeğini (parçacık güncelleme,
 * iki geçişli prefix-sum, köşe × eklem matrisi skinning) ve u32 atomic_add desteğini
 * C referansı, ir_eval_compute (job_parallel_for 1T/8T dilimleme), WebAssembly SIMD (V8)
 * ve WGSL (@compute @workgroup_size(64), build/wgsl/compute_*.wgsl) hedeflerinde doğrular.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"
#include "thread/thread.h"
#include "thread/job.h"
#include "matoki/matoki.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/wgsl.h"

static u8 g_comp_arena_buf[MB(16)];

static void ensure_compute_dirs(void) {
    mkdir("build", 0755);
    mkdir("build/wgsl", 0755);
    mkdir("build/test_compute", 0755);
}

static char* read_file_text(const char* path, u32* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = (u32)rd;
    return buf;
}

static b32 write_file_bytes(const char* path, const void* data, u32 len) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;
    size_t wr = fwrite(data, 1, len, fp);
    fclose(fp);
    return wr == (size_t)len;
}

/* ============================================================================
 * 1. C Referans Fonksiyonları (3 Çekirdek + Atomic Histogram)
 * ============================================================================ */

typedef struct {
    f32 dt;        f32 _pad0[3];
    f32 gravity_y; f32 _pad1[3];
    f32 damping;   f32 _pad2[3];
    f32 floor_y;   f32 _pad3[3];
    f32 bounce;    f32 _pad4[3];
} ParticleUniforms;

static void ref_particle_update(u32 count, const ParticleUniforms* u, f32* pos4, f32* vel4) {
    for (u32 i = 0; i < count; i++) {
        f32 px = pos4[i * 4 + 0];
        f32 py = pos4[i * 4 + 1];
        f32 pz = pos4[i * 4 + 2];
        f32 pw = pos4[i * 4 + 3];

        f32 vx = vel4[i * 4 + 0];
        f32 vy = vel4[i * 4 + 1];
        f32 vz = vel4[i * 4 + 2];
        f32 vw = vel4[i * 4 + 3];

        vx = vx * u->damping;
        vy = (vy + u->gravity_y * u->dt) * u->damping;
        vz = vz * u->damping;

        px = px + vx * u->dt;
        py = py + vy * u->dt;
        pz = pz + vz * u->dt;

        if (py < u->floor_y) {
            py = u->floor_y;
            vy = (vy < 0.0f ? -vy : vy) * u->bounce;
        }

        f32 life = pw - u->dt;
        if (life < 0.0f) life = 0.0f;

        pos4[i * 4 + 0] = px;
        pos4[i * 4 + 1] = py;
        pos4[i * 4 + 2] = pz;
        pos4[i * 4 + 3] = life;

        vel4[i * 4 + 0] = vx;
        vel4[i * 4 + 1] = vy;
        vel4[i * 4 + 2] = vz;
        vel4[i * 4 + 3] = vw;
    }
}

static void ref_prefix_sum(u32 count, const u32* in_data, u32* out_data) {
    u32 acc = 0;
    for (u32 i = 0; i < count; i++) {
        acc += in_data[i];
        out_data[i] = acc;
    }
}

static void ref_skinning(u32 count, const f32* in_pos, const f32* in_norm,
                         const u32* in_joints, const f32* in_weights,
                         const f32* bone_mats, f32* out_pos, f32* out_norm) {
    for (u32 i = 0; i < count; i++) {
        const f32* p = &in_pos[i * 4];
        const f32* n = &in_norm[i * 4];
        const f32* w = &in_weights[i * 4];
        u32 pj = in_joints[i];

        u32 j0 = ((pj >>  0) & 255u) * 3u;
        u32 j1 = ((pj >>  8) & 255u) * 3u;
        u32 j2 = ((pj >> 16) & 255u) * 3u;
        u32 j3 = ((pj >> 24) & 255u) * 3u;

        f32 r0[4], r1[4], r2[4];
        for (u32 c = 0; c < 4; c++) {
            r0[c] = bone_mats[j0 * 4 + c] * w[0] +
                    bone_mats[j1 * 4 + c] * w[1] +
                    bone_mats[j2 * 4 + c] * w[2] +
                    bone_mats[j3 * 4 + c] * w[3];
            r1[c] = bone_mats[(j0 + 1) * 4 + c] * w[0] +
                    bone_mats[(j1 + 1) * 4 + c] * w[1] +
                    bone_mats[(j2 + 1) * 4 + c] * w[2] +
                    bone_mats[(j3 + 1) * 4 + c] * w[3];
            r2[c] = bone_mats[(j0 + 2) * 4 + c] * w[0] +
                    bone_mats[(j1 + 2) * 4 + c] * w[1] +
                    bone_mats[(j2 + 2) * 4 + c] * w[2] +
                    bone_mats[(j3 + 2) * 4 + c] * w[3];
        }

        out_pos[i * 4 + 0] = r0[0] * p[0] + r0[1] * p[1] + r0[2] * p[2] + r0[3];
        out_pos[i * 4 + 1] = r1[0] * p[0] + r1[1] * p[1] + r1[2] * p[2] + r1[3];
        out_pos[i * 4 + 2] = r2[0] * p[0] + r2[1] * p[1] + r2[2] * p[2] + r2[3];
        out_pos[i * 4 + 3] = 1.0f;

        out_norm[i * 4 + 0] = r0[0] * n[0] + r0[1] * n[1] + r0[2] * n[2];
        out_norm[i * 4 + 1] = r1[0] * n[0] + r1[1] * n[1] + r1[2] * n[2];
        out_norm[i * 4 + 2] = r2[0] * n[0] + r2[1] * n[1] + r2[2] * n[2];
        out_norm[i * 4 + 3] = 0.0f;
    }
}

/* ============================================================================
 * 2. Sema ve Ön Uç Hata Testleri
 * ============================================================================ */

static b32 expect_compile_error(Arena* a, const char* src, const char* substr) {
    usize mark = arena_mark(a);
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;
    b32 ok = mtk_compile_ir(src, (u32)strlen(src), "err_compute.mtk", a, &ir, &layout, &diag);
    arena_pop_to(a, mark);
    if (ok || diag.count == 0) return FALSE;
    return strstr(diag.text, substr) != NULL;
}

TEST(matoki_compute_sema_and_errors) {
    Arena a;
    arena_init(&a, g_comp_arena_buf, sizeof(g_comp_arena_buf));

    /* 1. global_id yalnızca fn compute() içinde kullanılabilir */
    CHECK(expect_compile_error(&a,
        "fn frag() -> vec4 {\n"
        "    let i: u32 = global_id;\n"
        "    return vec4(f32(i), 0.0, 0.0, 1.0);\n"
        "}\n",
        "global_id"));

    /* 2. discard fn compute() içinde kullanılamaz */
    CHECK(expect_compile_error(&a,
        "buffer b: buffer<f32>;\n"
        "fn compute() {\n"
        "    discard;\n"
        "}\n",
        "discard"));

    /* 3. fn compute() dönüş tipi alamaz */
    CHECK(expect_compile_error(&a,
        "buffer b: buffer<f32>;\n"
        "fn compute() -> vec4 {\n"
        "    return vec4(0.0);\n"
        "}\n",
        "compute"));

    /* 4. fn compute() içinde değer döndüren return kullanılamaz */
    CHECK(expect_compile_error(&a,
        "buffer b: buffer<f32>;\n"
        "fn compute() {\n"
        "    return 1.0;\n"
        "}\n",
        "return"));

    /* 5. Buffer olmayan değişkene indeksleme yapılamaz */
    CHECK(expect_compile_error(&a,
        "uniform x: f32;\n"
        "fn compute() {\n"
        "    let y: f32 = x[0u];\n"
        "}\n",
        "buffer"));

    /* 6. Buffer indeksi u32/i32 olmalıdır */
    CHECK(expect_compile_error(&a,
        "buffer b: buffer<f32>;\n"
        "fn compute() {\n"
        "    b[1.5] = 2.0;\n"
        "}\n",
        "indeks"));

    /* 7. atomic_add yalnızca buffer<u32> kabul eder */
    CHECK(expect_compile_error(&a,
        "buffer b: buffer<f32>;\n"
        "fn compute() {\n"
        "    let old: u32 = atomic_add(b, global_id, 1u);\n"
        "}\n",
        "atomic_add"));
}

/* ============================================================================
 * 3. IR Üretimi, Erken Return Maskeleme, Swizzle Atama ve Atomic Add Testi
 * ============================================================================ */

static const IrFunc* g_eval_ir = NULL;

static void eval_dispatch_cb(MtkComputeBatch* slice) {
    ir_eval_compute(g_eval_ir, slice);
}

TEST(matoki_compute_atomic_and_early_return) {
    ensure_compute_dirs();
    job_system_init();

    Arena a;
    arena_init(&a, g_comp_arena_buf, sizeof(g_comp_arena_buf));

    const char* src =
        "uniform limit: u32;\n"
        "buffer vals: buffer<u32>;\n"
        "buffer hist: buffer<u32>;\n"
        "buffer out_v: buffer<vec4>;\n"
        "fn compute() {\n"
        "    let i: u32 = global_id;\n"
        "    if (i >= limit) {\n"
        "        return;\n"
        "    }\n"
        "    let v: u32 = vals[i];\n"
        "    let bin: u32 = v & 15u;\n"
        "    let prev: u32 = atomic_add(hist, bin, 1u);\n"
        "    vals[i] = v + prev * 0u + 10u;\n"
        "    out_v[i] = vec4(1.0, 2.0, 3.0, 4.0);\n"
        "    out_v[i].yz = vec2(f32(bin), f32(v));\n"
        "}\n";

    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;
    CHECK(mtk_compile_ir(src, (u32)strlen(src), "atomic_test.mtk", &a, &ir, &layout, &diag));
    CHECK(diag.count == 0);
    CHECK(ir.is_compute == TRUE);
    CHECK(layout.is_compute == TRUE);
    CHECK(layout.buffer_count == 3u);

    ir_optimize(&ir);
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    /* WGSL çıktısını üret ve kaydet */
    char wgsl_buf[16384];
    u32 wlen = mtk_wgsl_compute(&ir, &layout, NULL, wgsl_buf, sizeof(wgsl_buf), err, sizeof(err));
    CHECK(wlen > 0);
    CHECK(strstr(wgsl_buf, "@compute @workgroup_size(64)") != NULL);
    CHECK(strstr(wgsl_buf, "array<atomic<u32>>") != NULL);
    CHECK(strstr(wgsl_buf, "atomicAdd(") != NULL);
    CHECK(write_file_bytes("build/wgsl/compute_atomic.wgsl", wgsl_buf, wlen));

    /* Wasm çıktısını üret ve doğrula */
    u8 wasm_buf[65536];
    MtkCodegenOpts opts = { TRUE, FALSE, MTK_FUSE_NONE };
    u32 wasm_len = mtk_codegen_compute(&ir, &opts, &a, wasm_buf, sizeof(wasm_buf), err, sizeof(err));
    CHECK(wasm_len > 0);
    CHECK(write_file_bytes("build/test_compute/atomic.wasm", wasm_buf, wasm_len));
    CHECK(system("wasm-validate --enable-all build/test_compute/atomic.wasm") == 0);

    /* 260 elemanla ama limit = 253 ile (erken return + 4'ün katı olmayan sınır) test et */
    enum { N_DISP = 260, LIMIT = 253 };
    u32 vals_ref[N_DISP], vals_eval[N_DISP];
    u32 hist_ref[16], hist_eval[16];
    f32 outv_eval[N_DISP * 4];

    memset(hist_ref, 0, sizeof(hist_ref));
    memset(hist_eval, 0, sizeof(hist_eval));
    memset(outv_eval, 0, sizeof(outv_eval));

    for (u32 i = 0; i < N_DISP; i++) {
        vals_ref[i]  = (i * 37u + 11u) & 255u;
        vals_eval[i] = vals_ref[i];
        if (i < LIMIT) {
            u32 bin = vals_ref[i] & 15u;
            hist_ref[bin]++;
            vals_ref[i] += 10u;
        }
    }

    u32 u_limit = LIMIT;
    MtkComputeBatch batch = {0};
    batch.count        = N_DISP;
    batch.start        = 0;
    batch.total_count  = N_DISP;
    batch.buffer_count = 3u;
    batch.uniforms     = (const u8*)&u_limit;
    batch.buffers[0]   = vals_eval;
    batch.buffers[1]   = hist_eval;
    batch.buffers[2]   = outv_eval;

    g_eval_ir = &ir;
    engine_set_worker_count(7u);
    mtk_compute_dispatch(eval_dispatch_cb, &batch, N_DISP, 16u);
    engine_set_worker_count(0u);
    g_eval_ir = NULL;

    for (u32 b = 0; b < 16; b++) {
        CHECK(hist_eval[b] == hist_ref[b]);
    }
    for (u32 i = 0; i < N_DISP; i++) {
        CHECK(vals_eval[i] == vals_ref[i]);
        if (i < LIMIT) {
            u32 orig_v = (i * 37u + 11u) & 255u;
            CHECK_NEAR(outv_eval[i * 4 + 0], 1.0f, 1e-6f);
            CHECK_NEAR(outv_eval[i * 4 + 1], (f32)(orig_v & 15u), 1e-6f);
            CHECK_NEAR(outv_eval[i * 4 + 2], (f32)orig_v, 1e-6f);
            CHECK_NEAR(outv_eval[i * 4 + 3], 4.0f, 1e-6f);
        } else {
            /* limit üstü elemanlara erken return nedeniyle dokunulmamış olmalı */
            CHECK_NEAR(outv_eval[i * 4 + 0], 0.0f, 1e-6f);
            CHECK_NEAR(outv_eval[i * 4 + 3], 0.0f, 1e-6f);
        }
    }

    /* Wasm SIMD atomic + erken return + swizzled vec4 store doğrulaması */
    const char* atomic_js =
        "const fs = require('fs');\n"
        "const mem = new WebAssembly.Memory({ initial: 4, maximum: 64, shared: true });\n"
        "const u8  = new Uint8Array(mem.buffer);\n"
        "const u32 = new Uint32Array(mem.buffer);\n"
        "const f32 = new Float32Array(mem.buffer);\n"
        "const mod = new WebAssembly.Module(fs.readFileSync('build/test_compute/atomic.wasm'));\n"
        "const ex  = new WebAssembly.Instance(mod, { env: { memory: mem } }).exports;\n"
        "const N = 260, LIMIT = 253;\n"
        "const offU = 256, offVals = 512, offHist = 2048, offOutV = 4096;\n"
        "u32[offU >> 2] = LIMIT;\n"
        "for (let i = 0; i < N; i++) u32[(offVals >> 2) + i] = ((i * 37 + 11) & 255) >>> 0;\n"
        "for (let s = 0; s < N; s += 16) {\n"
        "  const c = Math.min(16, N - s);\n"
        "  u32[0] = s; u32[1] = c; u32[2] = N; u32[3] = 3; u32[4] = offU;\n"
        "  u32[5] = offVals; u32[6] = offHist; u32[7] = offOutV;\n"
        "  ex.compute(0);\n"
        "}\n"
        "const out = Buffer.alloc(16 * 4 + N * 4 + N * 16);\n"
        "out.set(u8.subarray(offHist, offHist + 16 * 4), 0);\n"
        "out.set(u8.subarray(offVals, offVals + N * 4), 16 * 4);\n"
        "out.set(u8.subarray(offOutV, offOutV + N * 16), 16 * 4 + N * 4);\n"
        "fs.writeFileSync('build/test_compute/atomic_out.bin', out);\n";
    CHECK(write_file_bytes("build/test_compute/run_atomic.js", atomic_js, (u32)strlen(atomic_js)));
    CHECK(system("node build/test_compute/run_atomic.js") == 0);

    struct {
        u32 hist[16];
        u32 vals[N_DISP];
        f32 outv[N_DISP * 4];
    } wasm_at;
    FILE* fp = fopen("build/test_compute/atomic_out.bin", "rb");
    CHECK(fp != NULL);
    CHECK(fread(&wasm_at, 1, sizeof(wasm_at), fp) == sizeof(wasm_at));
    fclose(fp);
    for (u32 b = 0; b < 16; b++) {
        CHECK(wasm_at.hist[b] == hist_ref[b]);
    }
    for (u32 i = 0; i < N_DISP; i++) {
        CHECK(wasm_at.vals[i] == vals_ref[i]);
        for (u32 c = 0; c < 4; c++) {
            CHECK_NEAR(wasm_at.outv[i * 4 + c], outv_eval[i * 4 + c], 1e-6f);
        }
    }
}

/* ============================================================================
 * 4. Üç Örnek Compute Çekirdeği: C Referansı vs ir_eval vs Wasm SIMD + WGSL
 * ============================================================================ */

static void compile_compute_shader_file(Arena* a, const char* mtk_path,
                                        const char* wgsl_path, const char* wasm_path,
                                        IrFunc* out_ir, MtkLayout* out_layout) {
    u32 src_len = 0;
    char* src = read_file_text(mtk_path, &src_len);
    CHECK(src != NULL);

    MtkDiag diag;
    b32 ok = mtk_compile_ir(src, src_len, mtk_path, a, out_ir, out_layout, &diag);
    if (!ok || diag.count > 0) {
        fprintf(stderr, "mtk_compile_ir FAILED on %s: %s\n", mtk_path, diag.text);
    }
    CHECK(ok && diag.count == 0);
    CHECK(out_ir->is_compute == TRUE);
    CHECK(out_layout->is_compute == TRUE);

    ir_optimize(out_ir);
    char err[256];
    CHECK(ir_verify(out_ir, err, sizeof(err)));

    char wgsl_buf[32768];
    u32 wlen = mtk_compile_wgsl(src, src_len, mtk_path, NULL, a, wgsl_buf, sizeof(wgsl_buf), NULL, &diag);
    if (wlen == 0) {
        fprintf(stderr, "mtk_compile_wgsl FAILED on %s: %s\n", mtk_path, diag.text);
    }
    CHECK(wlen > 0);
    CHECK(strstr(wgsl_buf, "@compute @workgroup_size(64)") != NULL);
    CHECK(write_file_bytes(wgsl_path, wgsl_buf, wlen));

    u8 wasm_buf[65536];
    MtkCodegenOpts opts = { TRUE, FALSE, MTK_FUSE_NONE };
    u32 wasm_len = mtk_compile_wasm(src, src_len, mtk_path, &opts, a, wasm_buf, sizeof(wasm_buf), NULL, &diag);
    if (wasm_len == 0) {
        fprintf(stderr, "mtk_compile_wasm FAILED on %s: %s\n", mtk_path, diag.text);
    }
    CHECK(wasm_len > 0);
    CHECK(write_file_bytes(wasm_path, wasm_buf, wasm_len));

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wasm-validate --enable-all %s", wasm_path);
    CHECK(system(cmd) == 0);

    free(src);
}

TEST(matoki_compute_three_kernels_and_wasm_diff) {
    ensure_compute_dirs();
    job_system_init();

    Arena a;
    arena_init(&a, g_comp_arena_buf, sizeof(g_comp_arena_buf));

    IrFunc ir_part, ir_scan, ir_add, ir_skin;
    MtkLayout lay_part, lay_scan, lay_add, lay_skin;

    compile_compute_shader_file(&a,
        "shaders/compute/particle.mtk",
        "build/wgsl/compute_particle.wgsl",
        "build/test_compute/particle.wasm",
        &ir_part, &lay_part);

    compile_compute_shader_file(&a,
        "shaders/compute/prefix_sum_scan.mtk",
        "build/wgsl/compute_prefix_sum_scan.wgsl",
        "build/test_compute/prefix_sum_scan.wasm",
        &ir_scan, &lay_scan);

    compile_compute_shader_file(&a,
        "shaders/compute/prefix_sum_add.mtk",
        "build/wgsl/compute_prefix_sum_add.wgsl",
        "build/test_compute/prefix_sum_add.wasm",
        &ir_add, &lay_add);

    compile_compute_shader_file(&a,
        "shaders/compute/skinning.mtk",
        "build/wgsl/compute_skinning.wgsl",
        "build/test_compute/skinning.wasm",
        &ir_skin, &lay_skin);

    Rng rng;
    rng_seed(&rng, 0x123456789ABCDEF0ull);

    /* --- Çekirdek 1: Parçacık Güncelleme (N = 1027, 4'ün katı olmayan kuyruk dahil) --- */
    enum { N_PART = 1027 };
    static f32 p_ref[N_PART * 4], v_ref[N_PART * 4];
    static f32 p_eval[N_PART * 4], v_eval[N_PART * 4];

    for (u32 i = 0; i < N_PART * 4; i += 4) {
        p_ref[i + 0] = rng_f32(&rng) * 20.0f - 10.0f;
        p_ref[i + 1] = rng_f32(&rng) * 4.0f - 1.5f; /* bazıları zeminin altına düşecek */
        p_ref[i + 2] = rng_f32(&rng) * 20.0f - 10.0f;
        p_ref[i + 3] = rng_f32(&rng) * 2.0f;

        v_ref[i + 0] = rng_f32(&rng) * 6.0f - 3.0f;
        v_ref[i + 1] = rng_f32(&rng) * 6.0f - 5.0f;
        v_ref[i + 2] = rng_f32(&rng) * 6.0f - 3.0f;
        v_ref[i + 3] = 1.0f;
    }
    memcpy(p_eval, p_ref, sizeof(p_ref));
    memcpy(v_eval, v_ref, sizeof(v_ref));

    ParticleUniforms pu = {
        0.016f,  {0, 0, 0},
        -9.81f,  {0, 0, 0},
        0.99f,   {0, 0, 0},
        0.0f,    {0, 0, 0},
        0.75f,   {0, 0, 0}
    };
    CHECK(lay_part.uniform_bytes == sizeof(ParticleUniforms));
    ref_particle_update(N_PART, &pu, p_ref, v_ref);

    MtkComputeBatch b_part = {0};
    b_part.count        = N_PART;
    b_part.start        = 0;
    b_part.total_count  = N_PART;
    b_part.buffer_count = 2u;
    b_part.uniforms     = (const u8*)&pu;
    b_part.buffers[0]   = p_eval;
    b_part.buffers[1]   = v_eval;

    g_eval_ir = &ir_part;
    engine_set_worker_count(7u);
    mtk_compute_dispatch(eval_dispatch_cb, &b_part, N_PART, 64u);
    engine_set_worker_count(0u);
    g_eval_ir = NULL;

    for (u32 i = 0; i < N_PART * 4; i++) {
        CHECK_NEAR(p_eval[i], p_ref[i], 1e-5f);
        CHECK_NEAR(v_eval[i], v_ref[i], 1e-5f);
    }

    /* --- Çekirdek 2: İki Geçişli Prefix-Sum (N = 1024 = 256 blok × 4) --- */
    enum { N_SCAN = 1024, N_BLKS = N_SCAN / 4 };
    static u32 scan_in[N_SCAN], scan_ref[N_SCAN], scan_eval[N_SCAN];
    static u32 blk_sums[N_BLKS], blk_offs[N_BLKS];

    for (u32 i = 0; i < N_SCAN; i++) {
        scan_in[i] = (rng_u32(&rng) & 1023u) + 1u;
    }
    ref_prefix_sum(N_SCAN, scan_in, scan_ref);

    /* 1. Geçiş: blok içi tarama */
    MtkComputeBatch b_scan1 = {0};
    b_scan1.count        = N_BLKS;
    b_scan1.total_count  = N_BLKS;
    b_scan1.buffer_count = 3u;
    b_scan1.buffers[0]   = scan_in;
    b_scan1.buffers[1]   = scan_eval;
    b_scan1.buffers[2]   = blk_sums;
    g_eval_ir = &ir_scan;
    engine_set_worker_count(7u);
    mtk_compute_dispatch(eval_dispatch_cb, &b_scan1, N_BLKS, 32u);

    /* Blok toplamlarının dışlayıcı (exclusive) önek toplamı */
    u32 running = 0;
    for (u32 b = 0; b < N_BLKS; b++) {
        blk_offs[b] = running;
        running += blk_sums[b];
    }

    /* 2. Geçiş: blok tabanlarını elemanlara ekleme */
    MtkComputeBatch b_scan2 = {0};
    b_scan2.count        = N_SCAN;
    b_scan2.total_count  = N_SCAN;
    b_scan2.buffer_count = 2u;
    b_scan2.buffers[0]   = scan_eval;
    b_scan2.buffers[1]   = blk_offs;
    g_eval_ir = &ir_add;
    mtk_compute_dispatch(eval_dispatch_cb, &b_scan2, N_SCAN, 64u);
    engine_set_worker_count(0u);
    g_eval_ir = NULL;

    for (u32 i = 0; i < N_SCAN; i++) {
        CHECK(scan_eval[i] == scan_ref[i]);
    }

    /* --- Çekirdek 3: Skinning (N = 515 köşe, 32 eklem) --- */
    enum { N_SKIN = 515, N_BONES = 32 };
    static f32 sk_in_pos[N_SKIN * 4], sk_in_norm[N_SKIN * 4], sk_weights[N_SKIN * 4];
    static u32 sk_joints[N_SKIN];
    static f32 sk_bones[N_BONES * 3 * 4];
    static f32 sk_pos_ref[N_SKIN * 4], sk_norm_ref[N_SKIN * 4];
    static f32 sk_pos_eval[N_SKIN * 4], sk_norm_eval[N_SKIN * 4];

    for (u32 b = 0; b < N_BONES * 3; b++) {
        sk_bones[b * 4 + 0] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_bones[b * 4 + 1] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_bones[b * 4 + 2] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_bones[b * 4 + 3] = rng_f32(&rng) * 4.0f - 2.0f;
    }
    for (u32 i = 0; i < N_SKIN; i++) {
        sk_in_pos[i * 4 + 0]  = rng_f32(&rng) * 10.0f - 5.0f;
        sk_in_pos[i * 4 + 1]  = rng_f32(&rng) * 10.0f - 5.0f;
        sk_in_pos[i * 4 + 2]  = rng_f32(&rng) * 10.0f - 5.0f;
        sk_in_pos[i * 4 + 3]  = 1.0f;

        sk_in_norm[i * 4 + 0] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_in_norm[i * 4 + 1] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_in_norm[i * 4 + 2] = rng_f32(&rng) * 2.0f - 1.0f;
        sk_in_norm[i * 4 + 3] = 0.0f;

        u32 b0 = rng_u32(&rng) % N_BONES;
        u32 b1 = rng_u32(&rng) % N_BONES;
        u32 b2 = rng_u32(&rng) % N_BONES;
        u32 b3 = rng_u32(&rng) % N_BONES;
        sk_joints[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

        f32 w0 = rng_f32(&rng) + 0.05f;
        f32 w1 = rng_f32(&rng) + 0.05f;
        f32 w2 = rng_f32(&rng) + 0.05f;
        f32 w3 = rng_f32(&rng) + 0.05f;
        f32 inv_w = 1.0f / (w0 + w1 + w2 + w3);
        sk_weights[i * 4 + 0] = w0 * inv_w;
        sk_weights[i * 4 + 1] = w1 * inv_w;
        sk_weights[i * 4 + 2] = w2 * inv_w;
        sk_weights[i * 4 + 3] = w3 * inv_w;
    }

    ref_skinning(N_SKIN, sk_in_pos, sk_in_norm, sk_joints, sk_weights, sk_bones,
                 sk_pos_ref, sk_norm_ref);

    MtkComputeBatch b_skin = {0};
    b_skin.count        = N_SKIN;
    b_skin.total_count  = N_SKIN;
    b_skin.buffer_count = 7u;
    b_skin.buffers[0]   = sk_in_pos;
    b_skin.buffers[1]   = sk_in_norm;
    b_skin.buffers[2]   = sk_joints;
    b_skin.buffers[3]   = sk_weights;
    b_skin.buffers[4]   = sk_bones;
    b_skin.buffers[5]   = sk_pos_eval;
    b_skin.buffers[6]   = sk_norm_eval;

    g_eval_ir = &ir_skin;
    engine_set_worker_count(7u);
    mtk_compute_dispatch(eval_dispatch_cb, &b_skin, N_SKIN, 64u);
    engine_set_worker_count(0u);
    g_eval_ir = NULL;

    for (u32 i = 0; i < N_SKIN * 4; i++) {
        CHECK_NEAR(sk_pos_eval[i],  sk_pos_ref[i],  1e-5f);
        CHECK_NEAR(sk_norm_eval[i], sk_norm_ref[i], 1e-5f);
    }

    /* --- Node.js üzerinden üretilen Wasm SIMD modüllerini çalıştır ve C referansıyla karşılaştır --- */
    const char* js_runner =
        "const fs = require('fs');\n"
        "const mem = new WebAssembly.Memory({ initial: 16, maximum: 16384, shared: true });\n"
        "const u8  = new Uint8Array(mem.buffer);\n"
        "const u32 = new Uint32Array(mem.buffer);\n"
        "function loadMod(path) {\n"
        "  const bytes = fs.readFileSync(path);\n"
        "  const mod = new WebAssembly.Module(bytes);\n"
        "  return new WebAssembly.Instance(mod, { env: { memory: mem } }).exports;\n"
        "}\n"
        "function runChunked(fn, count, chunk, offU, bufs) {\n"
        "  for (let s = 0; s < count; s += chunk) {\n"
        "    const c = Math.min(chunk, count - s);\n"
        "    u32[0] = s;\n"
        "    u32[1] = c;\n"
        "    u32[2] = count;\n"
        "    u32[3] = bufs.length;\n"
        "    u32[4] = offU;\n"
        "    for (let b = 0; b < bufs.length; b++) u32[5 + b] = bufs[b];\n"
        "    fn(0);\n"
        "  }\n"
        "}\n"
        "// 1. Particle test\n"
        "{\n"
        "  const ex = loadMod('build/test_compute/particle.wasm');\n"
        "  const raw = fs.readFileSync('build/test_compute/part_in.bin');\n"
        "  u8.set(raw, 1024);\n"
        "  const N = 1027;\n"
        "  const offU = 1024;\n"
        "  const offP = 1024 + 80;\n"
        "  const offV = offP + N * 16;\n"
        "  runChunked(ex.compute, N, 64, offU, [offP, offV]);\n"
        "  fs.writeFileSync('build/test_compute/part_out.bin', u8.subarray(offP, offV + N * 16));\n"
        "}\n"
        "// 2. Prefix sum test\n"
        "{\n"
        "  const exScan = loadMod('build/test_compute/prefix_sum_scan.wasm');\n"
        "  const exAdd  = loadMod('build/test_compute/prefix_sum_add.wasm');\n"
        "  const raw = fs.readFileSync('build/test_compute/scan_in.bin');\n"
        "  const N = 1024, B = 256;\n"
        "  const offIn = 1024, offOut = offIn + N * 4, offSums = offOut + N * 4, offOffs = offSums + B * 4;\n"
        "  u8.set(raw, offIn);\n"
        "  runChunked(exScan.compute, B, 32, 0, [offIn, offOut, offSums]);\n"
        "  let acc = 0;\n"
        "  for (let b = 0; b < B; b++) {\n"
        "    u32[(offOffs >> 2) + b] = acc;\n"
        "    acc = (acc + u32[(offSums >> 2) + b]) >>> 0;\n"
        "  }\n"
        "  runChunked(exAdd.compute, N, 64, 0, [offOut, offOffs]);\n"
        "  fs.writeFileSync('build/test_compute/scan_out.bin', u8.subarray(offOut, offOut + N * 4));\n"
        "}\n"
        "// 3. Skinning test\n"
        "{\n"
        "  const ex = loadMod('build/test_compute/skinning.wasm');\n"
        "  const raw = fs.readFileSync('build/test_compute/skin_in.bin');\n"
        "  const N = 515, BONES = 32;\n"
        "  const offPos = 1024;\n"
        "  const offNorm = offPos + N * 16;\n"
        "  const offJoints = offNorm + N * 16;\n"
        "  const offWeights = offJoints + N * 4;\n"
        "  const offBones = offWeights + N * 16;\n"
        "  const offOutPos = offBones + BONES * 3 * 16;\n"
        "  const offOutNorm = offOutPos + N * 16;\n"
        "  u8.set(raw, offPos);\n"
        "  runChunked(ex.compute, N, 64, 0, [offPos, offNorm, offJoints, offWeights, offBones, offOutPos, offOutNorm]);\n"
        "  fs.writeFileSync('build/test_compute/skin_out.bin', u8.subarray(offOutPos, offOutNorm + N * 16));\n"
        "}\n";

    CHECK(write_file_bytes("build/test_compute/run_wasm.js", js_runner, (u32)strlen(js_runner)));

    /* Particle girdi paketini yaz (tekrar başlangıç değerlerini üret) */
    rng_seed(&rng, 0x123456789ABCDEF0ull);
    static u8 part_in_blob[80 + N_PART * 32];
    memset(part_in_blob, 0, sizeof(part_in_blob));
    memcpy(part_in_blob, &pu, sizeof(pu));
    f32* p_init = (f32*)(part_in_blob + 80);
    f32* v_init = (f32*)(part_in_blob + 80 + N_PART * 16);
    for (u32 i = 0; i < N_PART * 4; i += 4) {
        p_init[i + 0] = rng_f32(&rng) * 20.0f - 10.0f;
        p_init[i + 1] = rng_f32(&rng) * 4.0f - 1.5f;
        p_init[i + 2] = rng_f32(&rng) * 20.0f - 10.0f;
        p_init[i + 3] = rng_f32(&rng) * 2.0f;
        v_init[i + 0] = rng_f32(&rng) * 6.0f - 3.0f;
        v_init[i + 1] = rng_f32(&rng) * 6.0f - 5.0f;
        v_init[i + 2] = rng_f32(&rng) * 6.0f - 3.0f;
        v_init[i + 3] = 1.0f;
    }
    CHECK(write_file_bytes("build/test_compute/part_in.bin", part_in_blob, sizeof(part_in_blob)));

    /* Prefix-sum girdi paketini yaz */
    CHECK(write_file_bytes("build/test_compute/scan_in.bin", scan_in, sizeof(scan_in)));

    /* Skinning girdi paketini yaz */
    enum { SKIN_IN_BYTES = N_SKIN * 16 + N_SKIN * 16 + N_SKIN * 4 + N_SKIN * 16 + N_BONES * 3 * 16 };
    static u8 skin_in_blob[SKIN_IN_BYTES];
    u32 cur = 0;
    memcpy(skin_in_blob + cur, sk_in_pos,  N_SKIN * 16); cur += N_SKIN * 16;
    memcpy(skin_in_blob + cur, sk_in_norm, N_SKIN * 16); cur += N_SKIN * 16;
    memcpy(skin_in_blob + cur, sk_joints,  N_SKIN * 4);  cur += N_SKIN * 4;
    memcpy(skin_in_blob + cur, sk_weights, N_SKIN * 16); cur += N_SKIN * 16;
    memcpy(skin_in_blob + cur, sk_bones,   N_BONES * 3 * 16);
    CHECK(write_file_bytes("build/test_compute/skin_in.bin", skin_in_blob, sizeof(skin_in_blob)));

    /* Node.js ile Wasm SIMD çekirdeklerini çalıştır */
    CHECK(system("node build/test_compute/run_wasm.js") == 0);

    /* Particle Wasm çıktısını doğrula */
    static f32 wasm_part_out[N_PART * 8];
    FILE* fp = fopen("build/test_compute/part_out.bin", "rb");
    CHECK(fp != NULL);
    CHECK(fread(wasm_part_out, 1, sizeof(wasm_part_out), fp) == sizeof(wasm_part_out));
    fclose(fp);
    for (u32 i = 0; i < N_PART * 4; i++) {
        CHECK_NEAR(wasm_part_out[i],              p_ref[i], 1e-5f);
        CHECK_NEAR(wasm_part_out[N_PART * 4 + i], v_ref[i], 1e-5f);
    }

    /* Prefix-sum Wasm çıktısını doğrula */
    static u32 wasm_scan_out[N_SCAN];
    fp = fopen("build/test_compute/scan_out.bin", "rb");
    CHECK(fp != NULL);
    CHECK(fread(wasm_scan_out, 1, sizeof(wasm_scan_out), fp) == sizeof(wasm_scan_out));
    fclose(fp);
    for (u32 i = 0; i < N_SCAN; i++) {
        CHECK(wasm_scan_out[i] == scan_ref[i]);
    }

    /* Skinning Wasm çıktısını doğrula */
    static f32 wasm_skin_out[N_SKIN * 8];
    fp = fopen("build/test_compute/skin_out.bin", "rb");
    CHECK(fp != NULL);
    CHECK(fread(wasm_skin_out, 1, sizeof(wasm_skin_out), fp) == sizeof(wasm_skin_out));
    fclose(fp);
    for (u32 i = 0; i < N_SKIN * 4; i++) {
        CHECK_NEAR(wasm_skin_out[i],              sk_pos_ref[i],  1e-5f);
        CHECK_NEAR(wasm_skin_out[N_SKIN * 4 + i], sk_norm_ref[i], 1e-5f);
    }
}
