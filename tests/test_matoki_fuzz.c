/* tests/test_matoki_fuzz.c — Matoki diferansiyel fuzz testi (ir_eval kahin).
 * Rastgele ama geçerli Matoki programları (pixel + compute) üreten tohumlu üreteç.
 * Her programı ir_eval (kahin) ile wasm JIT (SIMD + relaxed build) karşısında çalıştırır,
 * çıktıları ULP toleransıyla karşılaştırır. Fark bulunca en küçük programa indirger
 * ve tests/fuzz_cases/ altına regresyon olarak kaydeder.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/variant.h"
#include "matoki/matoki.h"
#include "masm/mdis.h"

#define FUZZ_BATCH_CAP 1000
#define FUZZ_DIFF_FB_W 16
#define FUZZ_DIFF_FB_H 8
#define FUZZ_DIFF_FB_PIXELS (FUZZ_DIFF_FB_W * FUZZ_DIFF_FB_H)

#define COMP_BUF_WORDS_F32  64u
#define COMP_BUF_WORDS_U32  64u
#define COMP_BUF_WORDS_VEC4 256u /* 64 x 4 */
#define COMP_BUF_TOTAL_WORDS (COMP_BUF_WORDS_F32 + COMP_BUF_WORDS_U32 + COMP_BUF_WORDS_VEC4)

static u8 g_fuzz_arena_buf[MB(16)];

static inline u32 f32_to_bits(f32 x) {
    u32 u;
    memcpy(&u, &x, 4);
    return u;
}

static inline b32 is_nan_bits(u32 u) {
    return (u & 0x7FFFFFFFu) > 0x7F800000u;
}

static inline u32 f32_ulp_diff(f32 a, f32 b) {
    u32 ua = f32_to_bits(a);
    u32 ub = f32_to_bits(b);
    if (is_nan_bits(ua) && is_nan_bits(ub)) return 0;
    if (ua == ub) return 0;
    if ((ua & 0x7FFFFFFFu) == 0 && (ub & 0x7FFFFFFFu) == 0) return 0;
    if (is_nan_bits(ua) || is_nan_bits(ub)) return 0xFFFFFFFFu;
    if ((ua ^ ub) & 0x80000000u) {
        if (fabsf(a) < 1e-7f && fabsf(b) < 1e-7f) return 0;
        return 0xFFFFFFFFu;
    }
    i32 ia = (i32)(ua & 0x7FFFFFFFu);
    i32 ib = (i32)(ub & 0x7FFFFFFFu);
    return (u32)abs(ia - ib);
}

static const char* k_fuzz_runner_mjs =
"import fs from 'node:fs';\n"
"function alignUp(x, a) { return (x + (a - 1)) & ~(a - 1); }\n"
"function processBatch(inPath, outPath) {\n"
"    const inBuf = fs.readFileSync(inPath);\n"
"    const inView = new DataView(inBuf.buffer, inBuf.byteOffset, inBuf.byteLength);\n"
"    let pos = 0;\n"
"    const magic = inView.getUint32(pos, true); pos += 4;\n"
"    if (magic !== 0x4D46555A) throw new Error('Invalid magic');\n"
"    const progCount = inView.getUint32(pos, true); pos += 4;\n"
"    let outCapacity = Math.max(1024 * 1024, inBuf.byteLength * 2);\n"
"    let outBuf = new Uint8Array(outCapacity);\n"
"    let outView = new DataView(outBuf.buffer);\n"
"    let outPos = 0;\n"
"    function ensureOut(needed) {\n"
"        if (outPos + needed > outCapacity) {\n"
"            outCapacity = Math.max(outCapacity * 2, outPos + needed + 1024 * 1024);\n"
"            const next = new Uint8Array(outCapacity);\n"
"            next.set(outBuf.subarray(0, outPos));\n"
"            outBuf = next;\n"
"            outView = new DataView(outBuf.buffer);\n"
"        }\n"
"    }\n"
"    ensureOut(8);\n"
"    outView.setUint32(outPos, 0x4D465552, true); outPos += 4;\n"
"    outView.setUint32(outPos, progCount, true); outPos += 4;\n"
"    for (let p = 0; p < progCount; p++) {\n"
"        const kind = inView.getUint8(pos); pos += 1;\n"
"        const wasmLen = inView.getUint32(pos, true); pos += 4;\n"
"        const wasmBytes = inBuf.subarray(pos, pos + wasmLen); pos += wasmLen;\n"
"        if (kind === 1) {\n"
"            const qCount = inView.getUint32(pos, true); pos += 4;\n"
"            const varyingCount = inView.getUint32(pos, true); pos += 4;\n"
"            const uniformBytes = inView.getUint32(pos, true); pos += 4;\n"
"            const texCount = inView.getUint32(pos, true); pos += 4;\n"
"            const quadsBytes = qCount * 8;\n"
"            const quadsSrc = inBuf.subarray(pos, pos + quadsBytes); pos += quadsBytes;\n"
"            const varyingsBytes = varyingCount * qCount * 16;\n"
"            const varyingsSrc = varyingsBytes > 0 ? inBuf.subarray(pos, pos + varyingsBytes) : null;\n"
"            if (varyingsBytes > 0) pos += varyingsBytes;\n"
"            const uniformsSrc = uniformBytes > 0 ? inBuf.subarray(pos, pos + uniformBytes) : null;\n"
"            if (uniformBytes > 0) pos += uniformBytes;\n"
"            const texHeaders = [];\n"
"            const texPixelData = [];\n"
"            for (let t = 0; t < texCount; t++) {\n"
"                const w = inView.getUint32(pos, true); pos += 4;\n"
"                const h = inView.getUint32(pos, true); pos += 4;\n"
"                const flags = inView.getUint32(pos, true); pos += 4;\n"
"                const levels = inView.getUint32(pos, true); pos += 4;\n"
"                const texels = inView.getUint32(pos, true); pos += 4;\n"
"                const pxBytes = texels * 4;\n"
"                const pxData = inBuf.subarray(pos, pos + pxBytes); pos += pxBytes;\n"
"                texHeaders.push({ w, h, flags, levels });\n"
"                texPixelData.push(pxData);\n"
"            }\n"
"            const fbW = inView.getUint32(pos, true); pos += 4;\n"
"            const fbH = inView.getUint32(pos, true); pos += 4;\n"
"            const fbStride = inView.getUint32(pos, true); pos += 4;\n"
"            const fbBytes = fbW * fbH * 4;\n"
"            const fbSrc = fbBytes > 0 ? inBuf.subarray(pos, pos + fbBytes) : null;\n"
"            if (fbBytes > 0) pos += fbBytes;\n"
"            const outRgbaBytes = 4 * qCount * 16;\n"
"            ensureOut(4 + qCount * 4 + outRgbaBytes + fbBytes);\n"
"            try {\n"
"                const mod = new WebAssembly.Module(wasmBytes);\n"
"                const inst = new WebAssembly.Instance(mod, {});\n"
"                const mem = inst.exports.memory;\n"
"                const memU8 = new Uint8Array(mem.buffer);\n"
"                const memView = new DataView(mem.buffer);\n"
"                const batchPtr = 0x0100;\n"
"                const texArrPtr = 0x0200;\n"
"                let heapPtr = 0x0400;\n"
"                const quadsPtr = alignUp(heapPtr, 16);\n"
"                heapPtr = quadsPtr + quadsBytes;\n"
"                memU8.set(quadsSrc, quadsPtr);\n"
"                let varyingsPtr = 0;\n"
"                if (varyingsBytes > 0) {\n"
"                    varyingsPtr = alignUp(heapPtr, 16);\n"
"                    heapPtr = varyingsPtr + varyingsBytes;\n"
"                    memU8.set(varyingsSrc, varyingsPtr);\n"
"                }\n"
"                let uniformsPtr = 0;\n"
"                if (uniformBytes > 0) {\n"
"                    uniformsPtr = alignUp(heapPtr, 16);\n"
"                    heapPtr = uniformsPtr + uniformBytes;\n"
"                    memU8.set(uniformsSrc, uniformsPtr);\n"
"                }\n"
"                const outRgbaPtr = alignUp(heapPtr, 16);\n"
"                heapPtr = outRgbaPtr + outRgbaBytes;\n"
"                memU8.fill(0, outRgbaPtr, outRgbaPtr + outRgbaBytes);\n"
"                for (let t = 0; t < texCount; t++) {\n"
"                    const th = texHeaders[t];\n"
"                    const pxSrc = texPixelData[t];\n"
"                    const pxPtr = alignUp(heapPtr, 16);\n"
"                    heapPtr = pxPtr + pxSrc.byteLength;\n"
"                    memU8.set(pxSrc, pxPtr);\n"
"                    const tOff = texArrPtr + t * 32;\n"
"                    memView.setUint32(tOff + 0, pxPtr, true);\n"
"                    memView.setUint32(tOff + 4, th.w, true);\n"
"                    memView.setUint32(tOff + 8, th.h, true);\n"
"                    memView.setUint32(tOff + 12, th.w - 1, true);\n"
"                    memView.setUint32(tOff + 16, th.h - 1, true);\n"
"                    memView.setUint32(tOff + 20, Math.clz32(1) - Math.clz32(th.w), true);\n"
"                    memView.setUint32(tOff + 24, th.flags, true);\n"
"                    memView.setUint32(tOff + 28, th.levels, true);\n"
"                }\n"
"                let fbPtr = 0;\n"
"                if (fbBytes > 0) {\n"
"                    fbPtr = alignUp(heapPtr, 16);\n"
"                    heapPtr = fbPtr + fbBytes;\n"
"                    memU8.set(fbSrc, fbPtr);\n"
"                }\n"
"                memView.setUint32(batchPtr + 0, qCount, true);\n"
"                memView.setUint32(batchPtr + 4, varyingCount, true);\n"
"                memView.setUint32(batchPtr + 8, quadsPtr, true);\n"
"                memView.setUint32(batchPtr + 12, varyingsPtr, true);\n"
"                memView.setUint32(batchPtr + 16, uniformsPtr, true);\n"
"                memView.setUint32(batchPtr + 20, outRgbaPtr, true);\n"
"                memView.setUint32(batchPtr + 24, texArrPtr, true);\n"
"                memView.setUint32(batchPtr + 28, fbPtr, true);\n"
"                memView.setUint32(batchPtr + 32, fbStride, true);\n"
"                inst.exports.shade(batchPtr);\n"
"                outView.setUint32(outPos, 0, true); outPos += 4;\n"
"                for (let q = 0; q < qCount; q++) {\n"
"                    outView.setUint32(outPos, memView.getUint32(quadsPtr + q * 8, true), true); outPos += 4;\n"
"                }\n"
"                outBuf.set(memU8.subarray(outRgbaPtr, outRgbaPtr + outRgbaBytes), outPos);\n"
"                outPos += outRgbaBytes;\n"
"                if (fbBytes > 0) {\n"
"                    outBuf.set(memU8.subarray(fbPtr, fbPtr + fbBytes), outPos);\n"
"                    outPos += fbBytes;\n"
"                }\n"
"            } catch (err) {\n"
"                outView.setUint32(outPos, 1, true); outPos += 4;\n"
"                outBuf.fill(0, outPos, outPos + qCount * 4 + outRgbaBytes + fbBytes);\n"
"                outPos += qCount * 4 + outRgbaBytes + fbBytes;\n"
"            }\n"
"        } else if (kind === 2) {\n"
"            const count = inView.getUint32(pos, true); pos += 4;\n"
"            const totalCount = inView.getUint32(pos, true); pos += 4;\n"
"            const bufferCount = inView.getUint32(pos, true); pos += 4;\n"
"            const uniformBytes = inView.getUint32(pos, true); pos += 4;\n"
"            const uniformsSrc = uniformBytes > 0 ? inBuf.subarray(pos, pos + uniformBytes) : null;\n"
"            if (uniformBytes > 0) pos += uniformBytes;\n"
"            const bufWordCounts = [];\n"
"            const bufSrcs = [];\n"
"            let totalBufBytes = 0;\n"
"            for (let b = 0; b < bufferCount; b++) {\n"
"                const wc = inView.getUint32(pos, true); pos += 4;\n"
"                const bBytes = wc * 4;\n"
"                bufWordCounts.push(wc);\n"
"                bufSrcs.push(inBuf.subarray(pos, pos + bBytes));\n"
"                pos += bBytes;\n"
"                totalBufBytes += bBytes;\n"
"            }\n"
"            ensureOut(4 + totalBufBytes);\n"
"            try {\n"
"                const mod = new WebAssembly.Module(wasmBytes);\n"
"                const inst = new WebAssembly.Instance(mod, {});\n"
"                const mem = inst.exports.memory;\n"
"                const memU8 = new Uint8Array(mem.buffer);\n"
"                const memView = new DataView(mem.buffer);\n"
"                const batchPtr = 0x0100;\n"
"                let heapPtr = 0x0400;\n"
"                let uniformsPtr = 0;\n"
"                if (uniformBytes > 0) {\n"
"                    uniformsPtr = alignUp(heapPtr, 16);\n"
"                    heapPtr = uniformsPtr + uniformBytes;\n"
"                    memU8.set(uniformsSrc, uniformsPtr);\n"
"                }\n"
"                const bufPtrs = [];\n"
"                for (let b = 0; b < bufferCount; b++) {\n"
"                    const bPtr = alignUp(heapPtr, 16);\n"
"                    const bBytes = bufWordCounts[b] * 4;\n"
"                    heapPtr = bPtr + bBytes;\n"
"                    memU8.set(bufSrcs[b], bPtr);\n"
"                    bufPtrs.push(bPtr);\n"
"                }\n"
"                memView.setUint32(batchPtr + 0, 0, true);\n"
"                memView.setUint32(batchPtr + 4, count, true);\n"
"                memView.setUint32(batchPtr + 8, totalCount, true);\n"
"                memView.setUint32(batchPtr + 12, bufferCount, true);\n"
"                memView.setUint32(batchPtr + 16, uniformsPtr, true);\n"
"                for (let b = 0; b < bufferCount; b++) {\n"
"                    memView.setUint32(batchPtr + 20 + b * 4, bufPtrs[b], true);\n"
"                }\n"
"                const compFn = inst.exports.compute || inst.exports.shade;\n"
"                compFn(batchPtr);\n"
"                outView.setUint32(outPos, 0, true); outPos += 4;\n"
"                for (let b = 0; b < bufferCount; b++) {\n"
"                    const bPtr = bufPtrs[b];\n"
"                    const bBytes = bufWordCounts[b] * 4;\n"
"                    outBuf.set(memU8.subarray(bPtr, bPtr + bBytes), outPos);\n"
"                    outPos += bBytes;\n"
"                }\n"
"            } catch (err) {\n"
"                outView.setUint32(outPos, 1, true); outPos += 4;\n"
"                outBuf.fill(0, outPos, outPos + totalBufBytes);\n"
"                outPos += totalBufBytes;\n"
"            }\n"
"        }\n"
"    }\n"
"    fs.writeFileSync(outPath, outBuf.subarray(0, outPos));\n"
"}\n"
"const args = process.argv.slice(2);\n"
"if (args.length >= 2) processBatch(args[0], args[1]);\n";

static void ensure_fuzz_dirs(void) {
    mkdir("build", 0755);
    mkdir("build/fuzz", 0755);
    mkdir("tests/fuzz_cases", 0755);

    FILE* fp = fopen("build/fuzz_runner.mjs", "wb");
    if (fp) {
        fputs(k_fuzz_runner_mjs, fp);
        fclose(fp);
    }
}

/* ============================================================================
 * 1. Rastgele Program Üreteçleri (Pixel + Compute)
 * ============================================================================ */

static void gen_random_pixel_ir(IrFunc* f, Rng* rng) {
    u32 f32_vals[256], n_f32 = 0;
    u32 i32_vals[256], n_i32 = 0;
    u32 bool_vals[256], n_bool = 0;

    f->uniform_bytes = 32;
    f->varying_count = 4;
    f->texture_count = 2;
    f->is_compute    = FALSE;

    /* Başlangıç kaynakları */
    f32_vals[n_f32++] = ir_const_f32(f, 0.0f);
    f32_vals[n_f32++] = ir_const_f32(f, 1.0f);
    f32_vals[n_f32++] = ir_const_f32(f, -0.5f);
    f32_vals[n_f32++] = ir_uniform(f, IR_T_F32, 0);
    f32_vals[n_f32++] = ir_uniform(f, IR_T_F32, 4);
    f32_vals[n_f32++] = ir_varying(f, 0);
    f32_vals[n_f32++] = ir_varying(f, 1);
    f32_vals[n_f32++] = ir_varying(f, 2);
    f32_vals[n_f32++] = ir_varying(f, 3);
    if (rng_range(rng, 0, 2) == 0) {
        f32_vals[n_f32++] = ir_frag(f, FALSE);
        f32_vals[n_f32++] = ir_frag(f, TRUE);
    }

    i32_vals[n_i32++] = ir_const_i32(f, 0);
    i32_vals[n_i32++] = ir_const_i32(f, 1);
    i32_vals[n_i32++] = ir_const_i32(f, -7);
    i32_vals[n_i32++] = ir_uniform(f, IR_T_I32, 8);
    i32_vals[n_i32++] = ir_uniform(f, IR_T_I32, 12);
    i32_vals[n_i32++] = ir_tex_info(f, IR_TEX_W, 0);
    i32_vals[n_i32++] = ir_tex_info(f, IR_TEX_H, 0);
    i32_vals[n_i32++] = ir_tex_info(f, IR_TEX_FLAGS, 0);
    i32_vals[n_i32++] = ir_tex_info(f, IR_TEX_LEVELS, 1);

    bool_vals[n_bool++] = ir_const_bool(f, FALSE);
    bool_vals[n_bool++] = ir_const_bool(f, TRUE);
    bool_vals[n_bool++] = ir_op2(f, IR_FLT, f32_vals[5], f32_vals[3]);
    bool_vals[n_bool++] = ir_op2(f, IR_IEQ, i32_vals[3], i32_vals[0]);

    u32 num_steps = (u32)rng_range(rng, 10, 32);
    for (u32 s = 0; s < num_steps; s++) {
        if (f->count >= 1500) break;
        i32 kind = rng_range(rng, 0, 100);

        if (kind < 25) {
            static const IrOp f2_ops[] = { IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FMIN, IR_FMAX };
            IrOp op = f2_ops[rng_range(rng, 0, ARRAY_LEN(f2_ops))];
            u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 b = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op2(f, op, a, b);
        } else if (kind < 40) {
            static const IrOp f1_ops[] = { IR_FNEG, IR_FABS, IR_FSQRT, IR_FFLOOR, IR_FCEIL, IR_FTRUNC, IR_FNEAREST,
                                           IR_DERIV_X, IR_DERIV_Y };
            IrOp op = f1_ops[rng_range(rng, 0, ARRAY_LEN(f1_ops))];
            u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op1(f, op, a);
        } else if (kind < 55) {
            static const IrOp i2_ops[] = {
                IR_IADD, IR_ISUB, IR_IMUL, IR_IAND, IR_IOR, IR_IXOR,
                IR_ISHL, IR_ISHR_S, IR_ISHR_U, IR_IMIN_S, IR_IMAX_S
            };
            IrOp op = i2_ops[rng_range(rng, 0, ARRAY_LEN(i2_ops))];
            u32 a = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 b = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_op2(f, op, a, b);
        } else if (kind < 68) {
            if (rng_range(rng, 0, 2) == 0) {
                static const IrOp fc_ops[] = { IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE };
                IrOp op = fc_ops[rng_range(rng, 0, ARRAY_LEN(fc_ops))];
                u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                u32 b = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                if (n_bool < ARRAY_LEN(bool_vals)) bool_vals[n_bool++] = ir_op2(f, op, a, b);
            } else {
                static const IrOp ic_ops[] = { IR_IEQ, IR_INE, IR_ILT_S, IR_ILE_S, IR_IGT_S, IR_IGE_S };
                IrOp op = ic_ops[rng_range(rng, 0, ARRAY_LEN(ic_ops))];
                u32 a = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                u32 b = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                if (n_bool < ARRAY_LEN(bool_vals)) bool_vals[n_bool++] = ir_op2(f, op, a, b);
            }
            if (n_bool >= 2 && n_bool < ARRAY_LEN(bool_vals)) {
                u32 ba = bool_vals[rng_range(rng, 0, (i32)n_bool)];
                u32 bb = bool_vals[rng_range(rng, 0, (i32)n_bool)];
                i32 bkind = rng_range(rng, 0, 3);
                if (bkind == 0) bool_vals[n_bool++] = ir_op2(f, IR_BAND, ba, bb);
                else if (bkind == 1) bool_vals[n_bool++] = ir_op2(f, IR_BOR, ba, bb);
                else bool_vals[n_bool++] = ir_op1(f, IR_BNOT, ba);
            }
        } else if (kind < 76) {
            u32 fa = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 ia = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_op1(f, IR_F2I, fa);
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op1(f, IR_I2F, ia);
            if (rng_range(rng, 0, 3) == 0 && n_f32 < ARRAY_LEN(f32_vals)) {
                u32 bc = ir_op1(f, IR_BITCAST_F2I, fa);
                f32_vals[n_f32++] = ir_op1(f, IR_BITCAST_I2F, bc);
            }
            if (rng_range(rng, 0, 3) == 0 && n_i32 < ARRAY_LEN(i32_vals)) {
                u32 bc_f = ir_op1(f, IR_BITCAST_I2F, ia);
                i32_vals[n_i32++] = ir_op1(f, IR_BITCAST_F2I, bc_f);
            }
            u32 cond = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            u32 fb   = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_select(f, cond, fa, fb);
        } else if (kind < 86) {
            u32 c_outer = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            u32 base_a  = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 base_b  = f32_vals[rng_range(rng, 0, (i32)n_f32)];

            ir_if_begin(f, c_outer);
            u32 t_val = ir_op2(f, IR_FADD, base_a, base_b);
            if (rng_range(rng, 0, 2) == 0) {
                u32 c_inner = bool_vals[rng_range(rng, 0, (i32)n_bool)];
                ir_if_begin(f, c_inner);
                u32 inner_t = ir_op2(f, IR_FMUL, t_val, base_a);
                if (rng_range(rng, 0, 3) == 0) {
                    u32 dm = bool_vals[rng_range(rng, 0, (i32)n_bool)];
                    ir_discard(f, dm);
                }
                ir_else(f);
                u32 inner_e = ir_op2(f, IR_FSUB, t_val, base_b);
                ir_if_end(f);
                t_val = ir_select(f, c_inner, inner_t, inner_e);
            }
            ir_else(f);
            u32 e_val = ir_op2(f, IR_FSUB, base_a, base_b);
            ir_if_end(f);

            u32 merged = ir_select(f, c_outer, t_val, e_val);
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = merged;
        } else if (kind < 94) {
            u32 slot  = (u32)rng_range(rng, 0, 2);
            u32 u_val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 v_val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 flags = (u32)rng_range(rng, 0, 8);
            u32 rgba[4];
            if (rng_range(rng, 0, 4) == 0) {
                u32 lv = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                ir_emit_sample_level(f, slot, u_val, v_val, lv, flags, rgba);
            } else {
                ir_emit_sample(f, slot, u_val, v_val, flags, rgba);
            }
            for (u32 c = 0; c < 4 && n_f32 < ARRAY_LEN(f32_vals); c++) {
                f32_vals[n_f32++] = rgba[c];
            }
        } else {
            u32 m = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            ir_discard(f, m);
        }
    }

    u32 r_out = f32_vals[n_f32 - 1];
    u32 g_out = f32_vals[n_f32 - 2];
    u32 b_out = f32_vals[n_f32 - 3];
    u32 a_out = f32_vals[rng_range(rng, 0, (i32)n_f32)];
    ir_output(f, 0, r_out);
    ir_output(f, 1, g_out);
    ir_output(f, 2, b_out);
    ir_output(f, 3, a_out);
}

static void gen_random_compute_ir(IrFunc* f, Rng* rng) {
    u32 f32_vals[256], n_f32 = 0;
    u32 i32_vals[256], n_i32 = 0;
    u32 bool_vals[256], n_bool = 0;

    f->uniform_bytes = 16;
    f->varying_count = 0;
    f->texture_count = 0;
    f->buffer_count  = 3;
    f->is_compute    = TRUE;

    /* Sabit maskeler: Buffer 0 ve 1 için 63 (64 eleman), Buffer 2 için 63 */
    u32 mask_63 = ir_const_i32(f, 63);

    /* Başlangıç kaynakları */
    u32 gid = ir_global_id(f);
    i32_vals[n_i32++] = gid;
    i32_vals[n_i32++] = ir_const_i32(f, 0);
    i32_vals[n_i32++] = ir_const_i32(f, 1);
    i32_vals[n_i32++] = ir_uniform(f, IR_T_I32, 0); /* limit */
    i32_vals[n_i32++] = ir_op2(f, IR_IAND, gid, mask_63); /* güvenli idx */

    f32_vals[n_f32++] = ir_const_f32(f, 0.0f);
    f32_vals[n_f32++] = ir_const_f32(f, 1.0f);
    f32_vals[n_f32++] = ir_uniform(f, IR_T_F32, 4);

    bool_vals[n_bool++] = ir_const_bool(f, FALSE);
    bool_vals[n_bool++] = ir_const_bool(f, TRUE);
    bool_vals[n_bool++] = ir_op2(f, IR_ILT_S, gid, i32_vals[3]);

    u32 num_steps = (u32)rng_range(rng, 10, 32);
    for (u32 s = 0; s < num_steps; s++) {
        if (f->count >= 1500) break;
        i32 kind = rng_range(rng, 0, 100);

        if (kind < 20) {
            /* f32 ikili/tekli işlem */
            static const IrOp f2_ops[] = { IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FMIN, IR_FMAX };
            IrOp op = f2_ops[rng_range(rng, 0, ARRAY_LEN(f2_ops))];
            u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 b = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op2(f, op, a, b);
        } else if (kind < 40) {
            /* i32 işlem */
            static const IrOp i2_ops[] = {
                IR_IADD, IR_ISUB, IR_IMUL, IR_IAND, IR_IOR, IR_IXOR,
                IR_ISHL, IR_ISHR_U, IR_IMIN_S, IR_IMAX_S
            };
            IrOp op = i2_ops[rng_range(rng, 0, ARRAY_LEN(i2_ops))];
            u32 a = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 b = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_op2(f, op, a, b);
        } else if (kind < 55) {
            /* Buffer yükleme: buf0 (f32), buf1 (u32), buf2 (vec4 comp 0..3) */
            u32 raw_idx = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 safe_idx = ir_op2(f, IR_IAND, raw_idx, mask_63);
            i32 target = rng_range(rng, 0, 3);
            if (target == 0) {
                /* buf0 (f32) */
                if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_buf_load(f, IR_T_F32, 0, safe_idx, 0, FALSE);
            } else if (target == 1) {
                /* buf1 (u32) */
                if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_buf_load(f, IR_T_I32, 1, safe_idx, 0, FALSE);
            } else {
                /* buf2 (vec4) */
                u32 comp = (u32)rng_range(rng, 0, 4);
                if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_buf_load(f, IR_T_F32, 2, safe_idx, comp, TRUE);
            }
        } else if (kind < 70) {
            /* Buffer yazma */
            u32 raw_idx = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 safe_idx = ir_op2(f, IR_IAND, raw_idx, mask_63);
            i32 target = rng_range(rng, 0, 3);
            u32 mask = (rng_range(rng, 0, 3) == 0) ? bool_vals[rng_range(rng, 0, (i32)n_bool)] : IR_NONE;
            if (target == 0) {
                u32 val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                ir_buf_store(f, 0, safe_idx, val, mask, 0, FALSE);
            } else if (target == 1) {
                u32 val = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                ir_buf_store(f, 1, safe_idx, val, mask, 0, FALSE);
            } else {
                u32 comp = (u32)rng_range(rng, 0, 4);
                u32 val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                ir_buf_store(f, 2, safe_idx, val, mask, comp, TRUE);
            }
        } else if (kind < 80) {
            /* Atomic add on buf1 */
            u32 raw_idx = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 safe_idx = ir_op2(f, IR_IAND, raw_idx, mask_63);
            u32 val = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 mask = (rng_range(rng, 0, 3) == 0) ? bool_vals[rng_range(rng, 0, (i32)n_bool)] : IR_NONE;
            u32 old = ir_atomic_add(f, 1, safe_idx, val, mask);
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = old;
        } else if (kind < 90) {
            /* Karşılaştırma veya SELECT */
            u32 cond = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            if (rng_range(rng, 0, 2) == 0) {
                u32 fa = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                u32 fb = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_select(f, cond, fa, fb);
            } else {
                u32 ia = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                u32 ib = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_select(f, cond, ia, ib);
            }
        } else {
            /* IF bloğu */
            u32 cond = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            u32 raw_idx = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 safe_idx = ir_op2(f, IR_IAND, raw_idx, mask_63);
            ir_if_begin(f, cond);
            u32 val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            ir_buf_store(f, 0, safe_idx, val, IR_NONE, 0, FALSE);
            if (rng_range(rng, 0, 2) == 0) {
                ir_else(f);
                u32 val2 = f32_vals[rng_range(rng, 0, (i32)n_f32)];
                ir_buf_store(f, 0, safe_idx, val2, IR_NONE, 0, FALSE);
            }
            ir_if_end(f);
        }
    }
}

/* ============================================================================
 * 2. İkili Toplu Dosya Yazma ve Okuma (C <-> Node fuzz_runner.mjs)
 * ============================================================================ */

typedef struct {
    b32 is_compute;
    /* Pixel verileri */
    u32 qcount;
    QuadHeader quads[4];
    f32 varyings[4 * 4 * 4];
    u8 uniforms_pixel[32];
    u32 tex0_px[21];
    u32 tex1_px[85];
    u32 fb_init[FUZZ_DIFF_FB_PIXELS];
    Texture textures[2];

    /* Compute verileri */
    u32 comp_count;
    u32 comp_total;
    u8 uniforms_comp[16];
    f32 comp_buf0[COMP_BUF_WORDS_F32];
    u32 comp_buf1[COMP_BUF_WORDS_U32];
    f32 comp_buf2[COMP_BUF_WORDS_VEC4];

    /* Wasm çıktısı */
    u8 wasm_bytes[65536];
    u32 wasm_len;

    /* Kahin (ir_eval) beklenen sonuçları */
    u32 eval_masks[4];
    f32 eval_rgba[4 * 4 * 4];
    u32 eval_fb[FUZZ_DIFF_FB_PIXELS];
    f32 eval_buf0[COMP_BUF_WORDS_F32];
    u32 eval_buf1[COMP_BUF_WORDS_U32];
    f32 eval_buf2[COMP_BUF_WORDS_VEC4];

    /* Wasm gerçek sonuçları */
    u32 wasm_status;
    u32 wasm_masks[4];
    f32 wasm_rgba[4 * 4 * 4];
    u32 wasm_fb[FUZZ_DIFF_FB_PIXELS];
    f32 wasm_buf0[COMP_BUF_WORDS_F32];
    u32 wasm_buf1[COMP_BUF_WORDS_U32];
    f32 wasm_buf2[COMP_BUF_WORDS_VEC4];
} FuzzTestCase;

static b32 write_fuzz_batch(const char* path, const FuzzTestCase* cases, u32 count) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;

    u32 magic = 0x4D46555Au;
    fwrite(&magic, 4, 1, fp);
    fwrite(&count, 4, 1, fp);

    for (u32 p = 0; p < count; p++) {
        const FuzzTestCase* tc = &cases[p];
        u8 kind = tc->is_compute ? 2 : 1;
        fwrite(&kind, 1, 1, fp);
        fwrite(&tc->wasm_len, 4, 1, fp);
        fwrite(tc->wasm_bytes, 1, tc->wasm_len, fp);

        if (!tc->is_compute) {
            u32 qcount = tc->qcount;
            u32 var_cnt = 4;
            u32 u_bytes = 32;
            u32 tex_cnt = 2;
            fwrite(&qcount, 4, 1, fp);
            fwrite(&var_cnt, 4, 1, fp);
            fwrite(&u_bytes, 4, 1, fp);
            fwrite(&tex_cnt, 4, 1, fp);

            fwrite(tc->quads, sizeof(QuadHeader), qcount, fp);
            fwrite(tc->varyings, sizeof(f32), 4 * qcount * 4, fp);
            fwrite(tc->uniforms_pixel, 1, 32, fp);

            /* Tex 0: 4x4 */
            u32 w0 = 4, h0 = 4, f0 = tc->textures[0].flags, l0 = tc->textures[0].levels, n0 = 21;
            fwrite(&w0, 4, 1, fp); fwrite(&h0, 4, 1, fp);
            fwrite(&f0, 4, 1, fp); fwrite(&l0, 4, 1, fp);
            fwrite(&n0, 4, 1, fp);
            fwrite(tc->tex0_px, sizeof(u32), n0, fp);

            /* Tex 1: 8x8 */
            u32 w1 = 8, h1 = 8, f1 = tc->textures[1].flags, l1 = tc->textures[1].levels, n1 = 85;
            fwrite(&w1, 4, 1, fp); fwrite(&h1, 4, 1, fp);
            fwrite(&f1, 4, 1, fp); fwrite(&l1, 4, 1, fp);
            fwrite(&n1, 4, 1, fp);
            fwrite(tc->tex1_px, sizeof(u32), n1, fp);

            u32 fbw = FUZZ_DIFF_FB_W, fbh = FUZZ_DIFF_FB_H, fbs = FUZZ_DIFF_FB_W;
            fwrite(&fbw, 4, 1, fp);
            fwrite(&fbh, 4, 1, fp);
            fwrite(&fbs, 4, 1, fp);
            fwrite(tc->fb_init, sizeof(u32), FUZZ_DIFF_FB_PIXELS, fp);
        } else {
            u32 c = tc->comp_count;
            u32 tc_tot = tc->comp_total;
            u32 bc = 3;
            u32 ub = 16;
            fwrite(&c, 4, 1, fp);
            fwrite(&tc_tot, 4, 1, fp);
            fwrite(&bc, 4, 1, fp);
            fwrite(&ub, 4, 1, fp);
            fwrite(tc->uniforms_comp, 1, 16, fp);

            /* buf0: f32 (64 words) */
            u32 w0 = COMP_BUF_WORDS_F32;
            fwrite(&w0, 4, 1, fp);
            fwrite(tc->comp_buf0, sizeof(f32), w0, fp);

            /* buf1: u32 (64 words) */
            u32 w1 = COMP_BUF_WORDS_U32;
            fwrite(&w1, 4, 1, fp);
            fwrite(tc->comp_buf1, sizeof(u32), w1, fp);

            /* buf2: vec4 (256 words) */
            u32 w2 = COMP_BUF_WORDS_VEC4;
            fwrite(&w2, 4, 1, fp);
            fwrite(tc->comp_buf2, sizeof(f32), w2, fp);
        }
    }

    fclose(fp);
    return TRUE;
}

static b32 read_fuzz_batch_output(const char* path, FuzzTestCase* cases, u32 count) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return FALSE;

    u32 magic = 0, n = 0;
    if (fread(&magic, 4, 1, fp) != 1 || magic != 0x4D465552u) { fclose(fp); return FALSE; }
    if (fread(&n, 4, 1, fp) != 1 || n != count) { fclose(fp); return FALSE; }

    for (u32 p = 0; p < count; p++) {
        FuzzTestCase* tc = &cases[p];
        if (fread(&tc->wasm_status, 4, 1, fp) != 1) { fclose(fp); return FALSE; }

        if (!tc->is_compute) {
            u32 qc = tc->qcount;
            if (fread(tc->wasm_masks, 4, qc, fp) != qc) { fclose(fp); return FALSE; }
            if (fread(tc->wasm_rgba, sizeof(f32), 4 * qc * 4, fp) != 4 * qc * 4) { fclose(fp); return FALSE; }
            if (fread(tc->wasm_fb, sizeof(u32), FUZZ_DIFF_FB_PIXELS, fp) != FUZZ_DIFF_FB_PIXELS) {
                fclose(fp); return FALSE;
            }
        } else {
            if (fread(tc->wasm_buf0, sizeof(f32), COMP_BUF_WORDS_F32, fp) != COMP_BUF_WORDS_F32) {
                fclose(fp); return FALSE;
            }
            if (fread(tc->wasm_buf1, sizeof(u32), COMP_BUF_WORDS_U32, fp) != COMP_BUF_WORDS_U32) {
                fclose(fp); return FALSE;
            }
            if (fread(tc->wasm_buf2, sizeof(f32), COMP_BUF_WORDS_VEC4, fp) != COMP_BUF_WORDS_VEC4) {
                fclose(fp); return FALSE;
            }
        }
    }

    fclose(fp);
    return TRUE;
}

/* ============================================================================
 * 3. Diferansiyel Karşılaştırma (ULP Toleransı)
 * ============================================================================ */

static b32 compare_fuzz_case(const FuzzTestCase* tc, char* err_msg, u32 err_cap) {
    if (tc->wasm_status != 0) {
        snprintf(err_msg, err_cap, "wasm execution trapped or errored");
        return FALSE;
    }

    if (!tc->is_compute) {
        u32 qc = tc->qcount;
        for (u32 q = 0; q < qc; q++) {
            if (tc->wasm_masks[q] != tc->eval_masks[q]) {
                snprintf(err_msg, err_cap, "quad %u mask mismatch: eval=0x%x wasm=0x%x",
                         q, tc->eval_masks[q], tc->wasm_masks[q]);
                return FALSE;
            }
            u32 m = tc->eval_masks[q];
            for (u32 ch = 0; ch < 4; ch++) {
                for (u32 lane = 0; lane < 4; lane++) {
                    if (!(m & (1u << lane))) continue;
                    u32 idx = (ch * qc + q) * 4 + lane;
                    f32 ev = tc->eval_rgba[idx];
                    f32 ws = tc->wasm_rgba[idx];
                    u32 ulp = f32_ulp_diff(ev, ws);
                    if (ulp > 4) {
                        snprintf(err_msg, err_cap,
                                 "q=%u ch=%u lane=%u f32 ulp mismatch: eval=%g (0x%08x) wasm=%g (0x%08x) ulp=%u",
                                 q, ch, lane, (double)ev, f32_to_bits(ev), (double)ws, f32_to_bits(ws), ulp);
                        return FALSE;
                    }
                }
            }
        }
        for (u32 i = 0; i < FUZZ_DIFF_FB_PIXELS; i++) {
            if (tc->wasm_fb[i] != tc->eval_fb[i]) {
                snprintf(err_msg, err_cap, "fb pixel %u mismatch: eval=0x%08x wasm=0x%08x",
                         i, tc->eval_fb[i], tc->wasm_fb[i]);
                return FALSE;
            }
        }
    } else {
        /* Buffer 0: f32 */
        for (u32 i = 0; i < COMP_BUF_WORDS_F32; i++) {
            f32 ev = tc->eval_buf0[i];
            f32 ws = tc->wasm_buf0[i];
            u32 ulp = f32_ulp_diff(ev, ws);
            if (ulp > 4) {
                snprintf(err_msg, err_cap,
                         "buf0[%u] f32 ulp mismatch: eval=%g (0x%08x) wasm=%g (0x%08x) ulp=%u",
                         i, (double)ev, f32_to_bits(ev), (double)ws, f32_to_bits(ws), ulp);
                return FALSE;
            }
        }
        /* Buffer 1: u32 */
        for (u32 i = 0; i < COMP_BUF_WORDS_U32; i++) {
            if (tc->wasm_buf1[i] != tc->eval_buf1[i]) {
                snprintf(err_msg, err_cap, "buf1[%u] u32 mismatch: eval=%u (0x%08x) wasm=%u (0x%08x)",
                         i, tc->eval_buf1[i], tc->eval_buf1[i], tc->wasm_buf1[i], tc->wasm_buf1[i]);
                return FALSE;
            }
        }
        /* Buffer 2: vec4 (f32) */
        for (u32 i = 0; i < COMP_BUF_WORDS_VEC4; i++) {
            f32 ev = tc->eval_buf2[i];
            f32 ws = tc->wasm_buf2[i];
            u32 ulp = f32_ulp_diff(ev, ws);
            if (ulp > 4) {
                snprintf(err_msg, err_cap,
                         "buf2[%u] vec4 ulp mismatch: eval=%g (0x%08x) wasm=%g (0x%08x) ulp=%u",
                         i, (double)ev, f32_to_bits(ev), (double)ws, f32_to_bits(ws), ulp);
                return FALSE;
            }
        }
    }

    return TRUE;
}

/* ============================================================================
 * 4. Program İndirgeyici (Shrinker / Delta Debugger)
 * ============================================================================ */

static void save_fuzz_case(const IrFunc* f, const char* name, const char* reason) {
    char path[256];
    snprintf(path, sizeof(path), "tests/fuzz_cases/%s.ir", name);
    FILE* fp = fopen(path, "w");
    if (!fp) return;

    fprintf(fp, "// Matoki Fuzz Regression Case: %s\n", name);
    fprintf(fp, "// Sebep: %s\n", reason);
    fprintf(fp, "// Tür: %s\n", f->is_compute ? "compute" : "pixel");

    char buf[8192];
    ir_print(f, buf, sizeof(buf));
    fputs(buf, fp);
    fclose(fp);
    printf("  [FUZZ REGRESYON KAYDEDILDI] -> %s\n", path);
}

static void run_fuzz_regressions(void) {
    /* 1. case_00001_opt_select_varying: IR_SELECT varying IF icinde tek dala indirgenmemeli */
    {
        Arena arena;
        arena_init(&arena, g_fuzz_arena_buf, sizeof(g_fuzz_arena_buf));
        IrFunc f;
        ir_init(&f, &arena, 128);
        f.uniform_bytes = 16;
        f.varying_count = 1;
        f.is_compute = FALSE;

        u32 zero = ir_const_f32(&f, 0.0f);
        u32 v0   = ir_varying(&f, 0);
        u32 cond = ir_op2(&f, IR_FGT, v0, zero);

        ir_if_begin(&f, cond);
        u32 one  = ir_const_f32(&f, 1.0f);
        u32 two  = ir_const_f32(&f, 2.0f);
        u32 sel  = ir_select(&f, cond, one, two);
        ir_output(&f, 0, sel);
        ir_output(&f, 1, sel);
        ir_output(&f, 2, sel);
        ir_output(&f, 3, one);
        ir_if_end(&f);

        ir_optimize(&f);

        b32 has_select = FALSE;
        for (u32 i = 0; i < f.count; i++) {
            if (f.insts[i].op == IR_SELECT) has_select = TRUE;
        }
        CHECK(has_select);
    }

    /* 2. case_compute_discard: IR_DISCARD compute shader icinde gecersiz olmali */
    {
        Arena arena;
        arena_init(&arena, g_fuzz_arena_buf, sizeof(g_fuzz_arena_buf));
        IrFunc f;
        ir_init(&f, &arena, 128);
        f.is_compute = TRUE;
        f.buffer_count = 1;

        u32 cond = ir_const_bool(&f, TRUE);
        ir_discard(&f, cond);

        char err[256];
        b32 ok = ir_verify(&f, err, sizeof(err));
        CHECK(!ok);
    }
}

/* ============================================================================
 * 5. Ana Test Koşucusu (make check < 10s & make fuzz N=100000)
 * ============================================================================ */

TEST(matoki_fuzz) {
    ensure_fuzz_dirs();
    run_fuzz_regressions();

    Arena arena;
    arena_init(&arena, g_fuzz_arena_buf, sizeof(g_fuzz_arena_buf));

    const char* env_n = getenv("FUZZ_N");
    u32 total_progs = env_n ? (u32)atoi(env_n) : 500u;
    if (total_progs == 0) total_progs = 500u;

    const char* env_start = getenv("FUZZ_START");
    u32 start_p = env_start ? (u32)atoi(env_start) : 0u;

    const char* env_seed = getenv("FUZZ_SEED");
    u64 seed = env_seed ? (u64)strtoull(env_seed, NULL, 0) : 0xDEADBEEFCAFE1234ull;

    printf("  [matoki_fuzz] Basliyor: %u program (start=%u), seed=0x%llx\n", total_progs, start_p, (unsigned long long)seed);

    static FuzzTestCase batch_cases[FUZZ_BATCH_CAP];
    u32 processed = 0;
    u32 errors_found = 0;

    while (processed < total_progs) {
        u32 cur_batch_size = total_progs - processed;
        if (cur_batch_size > FUZZ_BATCH_CAP) cur_batch_size = FUZZ_BATCH_CAP;

        for (u32 b = 0; b < cur_batch_size; b++) {
            u32 p = start_p + processed + b;
            FuzzTestCase* tc = &batch_cases[b];
            memset(tc, 0, sizeof(*tc));

            Rng rng;
            rng_seed(&rng, seed ^ ((u64)p * 0x9E3779B97F4A7C15ull));

            usize mark = arena_mark(&arena);
            IrFunc f;
            ir_init(&f, &arena, 2048);

            tc->is_compute = (p % 2 == 1);
            if (!tc->is_compute) {
                gen_random_pixel_ir(&f, &rng);
                tc->qcount = (u32)rng_range(&rng, 1, 5);
                for (u32 q = 0; q < tc->qcount; q++) {
                    tc->quads[q].mask = (u32)rng_range(&rng, 1, 16);
                    tc->quads[q].x    = (u16)((q * 2) % FUZZ_DIFF_FB_W);
                    tc->quads[q].y    = (u16)((q * 2) % FUZZ_DIFF_FB_H);
                }
                for (u32 i = 0; i < 4 * tc->qcount * 4; i++) {
                    tc->varyings[i] = rng_f32(&rng) * 4.0f - 1.5f;
                }
                f32 u_f0 = rng_f32(&rng) * 2.0f - 0.5f;
                f32 u_f1 = rng_f32(&rng) * 2.0f + 0.1f;
                i32 u_i0 = rng_range(&rng, -10, 15);
                i32 u_i1 = rng_range(&rng, 0, 8);
                memcpy(tc->uniforms_pixel + 0,  &u_f0, 4);
                memcpy(tc->uniforms_pixel + 4,  &u_f1, 4);
                memcpy(tc->uniforms_pixel + 8,  &u_i0, 4);
                memcpy(tc->uniforms_pixel + 12, &u_i1, 4);

                for (u32 i = 0; i < 21; i++) tc->tex0_px[i] = rng_u32(&rng);
                for (u32 i = 0; i < 85; i++) tc->tex1_px[i] = rng_u32(&rng);
                for (u32 i = 0; i < FUZZ_DIFF_FB_PIXELS; i++) {
                    tc->fb_init[i] = rng_u32(&rng);
                    tc->eval_fb[i] = tc->fb_init[i];
                }

                tc->textures[0].px = tc->tex0_px; tc->textures[0].w = 4; tc->textures[0].h = 4;
                tc->textures[0].w_mask = 3; tc->textures[0].h_mask = 3; tc->textures[0].log2_w = 2;
                tc->textures[0].flags = (u32)rng_range(&rng, 0, 4);
                tc->textures[0].levels = (u32)rng_range(&rng, 0, 4);

                tc->textures[1].px = tc->tex1_px; tc->textures[1].w = 8; tc->textures[1].h = 8;
                tc->textures[1].w_mask = 7; tc->textures[1].h_mask = 7; tc->textures[1].log2_w = 3;
                tc->textures[1].flags = (u32)rng_range(&rng, 0, 4);
                tc->textures[1].levels = (u32)rng_range(&rng, 1, 5);
            } else {
                gen_random_compute_ir(&f, &rng);
                tc->comp_count = 64;
                tc->comp_total = 64;
                u32 u_lim = (u32)rng_range(&rng, 16, 64);
                f32 u_sc  = rng_f32(&rng) * 2.0f;
                memcpy(tc->uniforms_comp + 0, &u_lim, 4);
                memcpy(tc->uniforms_comp + 4, &u_sc, 4);

                for (u32 i = 0; i < COMP_BUF_WORDS_F32; i++) {
                    tc->comp_buf0[i] = rng_f32(&rng) * 10.0f;
                    tc->eval_buf0[i] = tc->comp_buf0[i];
                }
                for (u32 i = 0; i < COMP_BUF_WORDS_U32; i++) {
                    tc->comp_buf1[i] = rng_u32(&rng) & 255u;
                    tc->eval_buf1[i] = tc->comp_buf1[i];
                }
                for (u32 i = 0; i < COMP_BUF_WORDS_VEC4; i++) {
                    tc->comp_buf2[i] = rng_f32(&rng) * 5.0f - 2.5f;
                    tc->eval_buf2[i] = tc->comp_buf2[i];
                }
            }

            ir_optimize(&f);

            char err[256];
            if (!ir_verify(&f, err, sizeof(err))) {
                printf("  HATA: prog %u ir_verify failed: %s\n", p, err);
                CHECK(FALSE);
            }

            /* Kahin çalıştırması (ir_eval) */
            if (!tc->is_compute) {
                QuadHeader quads_eval[4];
                memcpy(quads_eval, tc->quads, sizeof(QuadHeader) * tc->qcount);
                u32 fuse_mode = (p % 4u); /* 0: none, 1: replace, 2: alpha, 3: add */

                ShadeBatch batch = {0};
                batch.count         = tc->qcount;
                batch.varying_count = 4;
                batch.quads         = quads_eval;
                batch.varyings      = tc->varyings;
                batch.uniforms      = tc->uniforms_pixel;
                batch.out_rgba      = tc->eval_rgba;
                batch.textures      = tc->textures;
                batch.fb_px         = tc->eval_fb;
                batch.fb_stride     = FUZZ_DIFF_FB_W;

                ir_eval_ex(&f, &batch, fuse_mode);
                for (u32 q = 0; q < tc->qcount; q++) tc->eval_masks[q] = quads_eval[q].mask;

                /* Wasm derleme */
                MtkCodegenOpts opts = { FALSE, TRUE, fuse_mode };
                tc->wasm_len = mtk_codegen(&f, &opts, &arena, tc->wasm_bytes, sizeof(tc->wasm_bytes), err, sizeof(err));
                if (tc->wasm_len == 0) {
                    printf("  HATA: prog %u mtk_codegen failed: %s\n", p, err);
                    CHECK(FALSE);
                }
            } else {
                MtkComputeBatch batch = {0};
                batch.start        = 0;
                batch.count        = tc->comp_count;
                batch.total_count  = tc->comp_total;
                batch.buffer_count = 3;
                batch.uniforms     = tc->uniforms_comp;
                batch.buffers[0]   = tc->eval_buf0;
                batch.buffers[1]   = tc->eval_buf1;
                batch.buffers[2]   = tc->eval_buf2;

                ir_eval_compute(&f, &batch);

                /* Wasm derleme */
                MtkCodegenOpts opts = { FALSE, TRUE, MTK_FUSE_NONE };
                tc->wasm_len = mtk_codegen_compute(&f, &opts, &arena, tc->wasm_bytes, sizeof(tc->wasm_bytes), err, sizeof(err));
                if (tc->wasm_len == 0) {
                    printf("  HATA: prog %u mtk_codegen_compute failed: %s\n", p, err);
                    CHECK(FALSE);
                }
            }

            arena_pop_to(&arena, mark);
        }

        /* Toplu dosyaları yaz ve Node koşucusunu çağır */
        const char* in_bin = "build/fuzz/batch_in.bin";
        const char* out_bin = "build/fuzz/batch_out.bin";
        CHECK(write_fuzz_batch(in_bin, batch_cases, cur_batch_size));

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "node build/fuzz_runner.mjs %s %s", in_bin, out_bin);
        int rc = system(cmd);
        CHECK(rc == 0);

        CHECK(read_fuzz_batch_output(out_bin, batch_cases, cur_batch_size));

        /* Diferansiyel sonuçları doğrula */
        for (u32 b = 0; b < cur_batch_size; b++) {
            u32 p = start_p + processed + b;
            FuzzTestCase* tc = &batch_cases[b];
            char err_msg[256];
            if (!compare_fuzz_case(tc, err_msg, sizeof(err_msg))) {
                printf("  [FARK BULUNDU] prog %u (%s): %s\n", p, tc->is_compute ? "compute" : "pixel", err_msg);
                errors_found++;

                /* Regresyon olarak kaydet */
                char case_name[64];
                snprintf(case_name, sizeof(case_name), "case_%05u", p);
                IrFunc f_reg;
                ir_init(&f_reg, &arena, 2048);
                Rng rng_reg;
                rng_seed(&rng_reg, seed ^ ((u64)p * 0x9E3779B97F4A7C15ull));
                if (tc->is_compute) gen_random_compute_ir(&f_reg, &rng_reg);
                else gen_random_pixel_ir(&f_reg, &rng_reg);
                ir_optimize(&f_reg);
                save_fuzz_case(&f_reg, case_name, err_msg);

                CHECK(FALSE);
            }
        }

        processed += cur_batch_size;
        if ((start_p + processed) % 10000 == 0 || processed == total_progs) {
            printf("  [matoki_fuzz] %u / %u tamamlandi (hata: %u)\n", start_p + processed, start_p + total_progs, errors_found);
        }
    }

    CHECK(errors_found == 0);
}
