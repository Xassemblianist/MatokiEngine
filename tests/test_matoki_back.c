/* tests/test_matoki_back.c — Matoki arka uç (D hattı) birim ve diferansiyel testleri.
 * ir_verify (olumlu/olumsuz), ir_print altın çıktı, constfold/CSE/DCE/specialize
 * optimizasyonları ve 500 rastgele IR programı x rastgele ShadeBatch üzerinde
 * ir_eval vs SIMD WebAssembly (node + wasm-validate) bit-bit diferansiyel testi içerir.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/log.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "matoki/variant.h"
#include "matoki/matoki.h"
#include "masm/mdis.h"

static void ensure_matoki_test_dir(void) {
    mkdir("build", 0755);
    mkdir("build/test_matoki", 0755);
}

static inline u32 f32_to_u32(f32 x) {
    u32 u;
    memcpy(&u, &x, 4);
    return u;
}

static inline b32 is_nan_bits(u32 u) {
    return (u & 0x7FFFFFFFu) > 0x7F800000u;
}

static inline b32 f32_bit_eq_or_nan(f32 a, f32 b) {
    u32 ua = f32_to_u32(a);
    u32 ub = f32_to_u32(b);
    if (is_nan_bits(ua) && is_nan_bits(ub)) return TRUE;
    return ua == ub;
}

/* Yardımcı: geçerli en basit 4 kanallı OUTPUT ekler */
static void emit_rgba_outputs(IrFunc* f, u32 r, u32 g, u32 b, u32 a) {
    ir_output(f, 0, r);
    ir_output(f, 1, g);
    ir_output(f, 2, b);
    ir_output(f, 3, a);
}

TEST(matoki_ir_verify_and_print) {
    static u8 arena_buf[65536];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    IrFunc f;
    ir_init(&f, &arena, 128);

    u32 u_tint = ir_uniform(&f, IR_T_F32, 0);
    u32 v_uv   = ir_varying(&f, 0);
    u32 c_half = ir_const_f32(&f, 0.5f);
    u32 prod   = ir_op2(&f, IR_FMUL, v_uv, u_tint);
    u32 cond   = ir_op2(&f, IR_FLT, prod, c_half);

    ir_if_begin(&f, cond);
    u32 br_t = ir_op2(&f, IR_FADD, prod, c_half);
    ir_else(&f);
    u32 br_f = ir_op2(&f, IR_FSUB, prod, c_half);
    ir_if_end(&f);

    u32 res = ir_select(&f, cond, br_t, br_f);
    emit_rgba_outputs(&f, res, prod, c_half, u_tint);

    char err[256];
    CHECK(ir_verify(&f, err, sizeof(err)));

    char printed[2048];
    u32 plen = ir_print(&f, printed, sizeof(printed));
    CHECK(plen > 0);

    const char* expected =
        "%0 = uniform.f32.u 0\n"
        "%1 = varying.v 0\n"
        "%2 = const.f32.u 0.500000\n"
        "%3 = fmul.v %1, %0\n"
        "%4 = flt.v %3, %2\n"
        "if.v %4\n"
        "  %6 = fadd.v %3, %2\n"
        "else\n"
        "  %8 = fsub.v %3, %2\n"
        "endif\n"
        "%10 = select.v %4, %6, %8\n"
        "output 0, %10\n"
        "output 1, %3\n"
        "output 2, %2\n"
        "output 3, %0\n";
    CHECK(strcmp(printed, expected) == 0);

    /* Olumsuz durum 1: Eksik OUTPUT kanalı */
    IrFunc bad;
    ir_init(&bad, &arena, 32);
    u32 c0 = ir_const_f32(&bad, 1.0f);
    ir_output(&bad, 0, c0);
    ir_output(&bad, 1, c0);
    ir_output(&bad, 2, c0);
    CHECK(!ir_verify(&bad, err, sizeof(err)));
    CHECK(strstr(err, "incomplete OUTPUT") != NULL);

    /* Olumsuz durum 2: Dengesiz IF_BEGIN (IF_END eksik) */
    ir_init(&bad, &arena, 32);
    c0 = ir_const_f32(&bad, 1.0f);
    u32 cb = ir_const_bool(&bad, TRUE);
    ir_if_begin(&bad, cb);
    emit_rgba_outputs(&bad, c0, c0, c0, c0);
    CHECK(!ir_verify(&bad, err, sizeof(err)));

    /* Olumsuz durum 3: Tanımdan önce kullanım (ileri referans) */
    ir_init(&bad, &arena, 32);
    c0 = ir_const_f32(&bad, 1.0f);
    u32 add = ir_op2(&bad, IR_FADD, c0, c0);
    emit_rgba_outputs(&bad, add, c0, c0, c0);
    bad.insts[1].a = 2; /* %1 komutu %2'yi kullanmaya çalışsın */
    CHECK(!ir_verify(&bad, err, sizeof(err)));
    CHECK(strstr(err, "not defined before use") != NULL);

    /* Olumsuz durum 4: Tip uyumsuzluğu */
    ir_init(&bad, &arena, 32);
    c0 = ir_const_f32(&bad, 1.0f);
    u32 ci = ir_const_i32(&bad, 5);
    add = ir_op2(&bad, IR_FADD, c0, c0);
    emit_rgba_outputs(&bad, add, c0, c0, c0);
    bad.insts[2].b = ci; /* fadd'a i32 operand ver */
    CHECK(!ir_verify(&bad, err, sizeof(err)));
    CHECK(strstr(err, "type") != NULL);
}

TEST(matoki_opt_passes_and_specialize) {
    static u8 arena_buf[65536];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    /* 1. Uniform özelleştirme: tint = (1, 1, 1, 1) olduğunda c * tint çarpımlarının silinmesi */
    IrFunc f;
    ir_init(&f, &arena, 256);

    u32 u_r = ir_uniform(&f, IR_T_F32, 0);
    u32 u_g = ir_uniform(&f, IR_T_F32, 4);
    u32 u_b = ir_uniform(&f, IR_T_F32, 8);
    u32 u_a = ir_uniform(&f, IR_T_F32, 12);

    u32 v_r = ir_varying(&f, 0);
    u32 v_g = ir_varying(&f, 1);
    u32 v_b = ir_varying(&f, 2);
    u32 v_a = ir_varying(&f, 3);

    u32 m_r = ir_op2(&f, IR_FMUL, v_r, u_r);
    u32 m_g = ir_op2(&f, IR_FMUL, v_g, u_g);
    u32 m_b = ir_op2(&f, IR_FMUL, v_b, u_b);
    u32 m_a = ir_op2(&f, IR_FMUL, v_a, u_a);

    /* Ayrıca CSE, constfold ve ölü kod içeren ifadeler ekleyelim */
    u32 c2   = ir_const_f32(&f, 2.0f);
    u32 c3   = ir_const_f32(&f, 3.0f);
    u32 cf   = ir_op2(&f, IR_FADD, c2, c3); /* -> 5.0 */
    u32 cse1 = ir_op2(&f, IR_FADD, m_r, cf);
    u32 cse2 = ir_op2(&f, IR_FADD, cf, m_r); /* değişmeli CSE: cse1 ile aynı! */
    u32 sub0 = ir_op2(&f, IR_FSUB, cse1, cse2); /* x - x (varying f32 NaN olabilir diye silinmez ama CSE sonrası aynı operand olur) */
    UNUSED(sub0); /* sub0 hiç kullanılmıyor -> DCE silmeli! */

    /* Boş IF bölgesi -> DCE silmeli */
    u32 cond_v = ir_op2(&f, IR_FLT, v_a, c2);
    ir_if_begin(&f, cond_v);
    u32 dead_in_if = ir_op2(&f, IR_FMUL, v_r, c3);
    UNUSED(dead_in_if);
    ir_if_end(&f);

    emit_rgba_outputs(&f, m_r, m_g, m_b, m_a);

    /* tint = (1, 1, 1, 1) olarak özelleştir */
    ir_specialize_uniform_f32(&f, 0,  1.0f);
    ir_specialize_uniform_f32(&f, 4,  1.0f);
    ir_specialize_uniform_f32(&f, 8,  1.0f);
    ir_specialize_uniform_f32(&f, 12, 1.0f);

    ir_optimize(&f);

    char err[256];
    CHECK(ir_verify(&f, err, sizeof(err)));

    /* Özelleştirme ve optimizasyon sonrası:
     * 4 varying + 4 output = 8 komut kalmalı! Hiçbir IR_FMUL veya IR_UNIFORM_F32 kalmamalı. */
    CHECK(f.count == 8);
    for (u32 i = 0; i < f.count; i++) {
        CHECK(f.insts[i].op != IR_FMUL);
        CHECK(f.insts[i].op != IR_UNIFORM_F32);
        CHECK(f.insts[i].op != IR_IF_BEGIN);
    }

    /* 2. Bölge-farkında CSE: IF dalı içindeki ifade, dal dışındaki ifadeyle birleştirilemez;
     * ancak üst bölgedeki ifade alt bölgede yeniden kullanılabilir. */
    ir_init(&f, &arena, 128);
    u32 vx = ir_varying(&f, 0);
    u32 vy = ir_varying(&f, 1);
    u32 c0 = ir_const_f32(&f, 0.0f);
    u32 cv = ir_op2(&f, IR_FGT, vx, c0);

    ir_if_begin(&f, cv);
    u32 in_branch = ir_op2(&f, IR_FADD, vx, vy);
    ir_else(&f);
    u32 in_else   = ir_op2(&f, IR_FSUB, vx, vy);
    ir_if_end(&f);

    u32 sel = ir_select(&f, cv, in_branch, in_else);
    /* IF dışında vx + vy tekrar hesaplanıyor: in_branch yalnızca THEN bölgesinde tanımlı olduğu için
     * dışarıdaki kullanım in_branch ile birleştirilmemeli! */
    u32 out_sum = ir_op2(&f, IR_FADD, vx, vy);
    emit_rgba_outputs(&f, sel, out_sum, vx, vy);

    ir_optimize(&f);
    CHECK(ir_verify(&f, err, sizeof(err)));
    /* out_sum kendi IR_FADD komutu olarak kalmalı (toplam 2 adet IR_FADD olmalı) */
    u32 fadd_cnt = 0;
    for (u32 i = 0; i < f.count; i++) {
        if (f.insts[i].op == IR_FADD) fadd_cnt++;
    }
    CHECK(fadd_cnt == 2);
}

/* ShadeBatch'i run_shade.mjs'nin beklediği ikili formata serileştirir */
static b32 write_batch_bin(const char* path, const ShadeBatch* b, u32 uniform_bytes, u32 tex_count,
                           u32 fb_w, u32 fb_h, const u32* fb_px) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;

    fwrite(&b->count, 4, 1, fp);
    fwrite(&b->varying_count, 4, 1, fp);
    fwrite(&uniform_bytes, 4, 1, fp);
    fwrite(&tex_count, 4, 1, fp);

    fwrite(b->quads, sizeof(QuadHeader), b->count, fp);
    if (b->varying_count > 0) {
        fwrite(b->varyings, sizeof(f32), b->varying_count * b->count * 4, fp);
    }
    if (uniform_bytes > 0) {
        fwrite(b->uniforms, 1, uniform_bytes, fp);
    }
    for (u32 t = 0; t < tex_count; t++) {
        const Texture* tx = &b->textures[t];
        fwrite(&tx->w, 4, 1, fp);
        fwrite(&tx->h, 4, 1, fp);
        fwrite(&tx->flags, 4, 1, fp);
        fwrite(&tx->levels, 4, 1, fp);
        u32 texels = 0;
        for (u32 k = 0; k < (tx->levels ? tx->levels : 1u); k++) texels += (tx->w >> k) * (tx->h >> k);
        fwrite(tx->px, sizeof(u32), texels, fp);
    }
    if (fb_w > 0 && fb_h > 0 && fb_px) {
        fwrite(&fb_w, 4, 1, fp);
        fwrite(&fb_h, 4, 1, fp);
        fwrite(fb_px, sizeof(u32), fb_w * fb_h, fp);
    }

    fclose(fp);
    return TRUE;
}

static b32 read_output_bin(const char* path, u32 count, f32* out_rgba, u32* out_masks,
                           u32 fb_pixels, u32* out_fb) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return FALSE;
    size_t nr1 = fread(out_rgba, sizeof(f32), 4 * count * 4, fp);
    size_t nr2 = fread(out_masks, sizeof(u32), count, fp);
    size_t nr3 = 0;
    if (fb_pixels > 0 && out_fb) {
        nr3 = fread(out_fb, sizeof(u32), fb_pixels, fp);
    }
    fclose(fp);
    if (fb_pixels > 0 && out_fb && nr3 != (size_t)fb_pixels) return FALSE;
    return nr1 == (size_t)(4 * count * 4) && nr2 == (size_t)count;
}

/* Rastgele, tip-doğru, iç içe IF (uniform & varying), SELECT, DISCARD ve SAMPLE içeren
 * IR programı üreteci */
static void gen_random_ir(IrFunc* f, Rng* rng, u32 prog_idx) {
    /* Havuz: üst seviyede tanımlı olan değerleri (bölge dışı kullanım hatası olmaması için)
     * ve aktif bölgede tanımlı olan değerleri takip ederiz. */
    u32 f32_vals[256], n_f32 = 0;
    u32 i32_vals[256], n_i32 = 0;
    u32 bool_vals[256], n_bool = 0;

    f->uniform_bytes = 32;
    f->varying_count = 4;
    f->texture_count = 2;

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

    u32 num_steps = (u32)rng_range(rng, 8, 28);
    for (u32 s = 0; s < num_steps; s++) {
        i32 kind = rng_range(rng, 0, 100);

        if (kind < 25) {
            /* f32 ikili işlem */
            static const IrOp f2_ops[] = { IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FMIN, IR_FMAX };
            IrOp op = f2_ops[rng_range(rng, 0, ARRAY_LEN(f2_ops))];
            u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 b = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op2(f, op, a, b);
        } else if (kind < 40) {
            /* f32 tekli işlem */
            static const IrOp f1_ops[] = { IR_FNEG, IR_FABS, IR_FSQRT, IR_FFLOOR, IR_FCEIL, IR_FTRUNC, IR_FNEAREST,
                                           IR_DERIV_X, IR_DERIV_Y };
            IrOp op = f1_ops[rng_range(rng, 0, ARRAY_LEN(f1_ops))];
            u32 a = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op1(f, op, a);
        } else if (kind < 55) {
            /* i32 ikili işlem */
            static const IrOp i2_ops[] = {
                IR_IADD, IR_ISUB, IR_IMUL, IR_IAND, IR_IOR, IR_IXOR,
                IR_ISHL, IR_ISHR_S, IR_ISHR_U, IR_IMIN_S, IR_IMAX_S
            };
            IrOp op = i2_ops[rng_range(rng, 0, ARRAY_LEN(i2_ops))];
            u32 a = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            u32 b = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_op2(f, op, a, b);
        } else if (kind < 68) {
            /* Karşılaştırma ve bool işlemleri */
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
            /* Tip dönüşümleri ve SELECT */
            u32 fa = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 ia = i32_vals[rng_range(rng, 0, (i32)n_i32)];
            if (n_i32 < ARRAY_LEN(i32_vals)) i32_vals[n_i32++] = ir_op1(f, IR_F2I, fa);
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_op1(f, IR_I2F, ia);
            if (rng_range(rng, 0, 3) == 0 && n_i32 < ARRAY_LEN(i32_vals) && n_f32 < ARRAY_LEN(f32_vals)) {
                u32 bc = ir_op1(f, IR_BITCAST_F2I, fa);
                i32_vals[n_i32++] = bc;
                f32_vals[n_f32++] = ir_op1(f, IR_BITCAST_I2F, bc);
            }
            u32 cond = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            u32 fb   = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = ir_select(f, cond, fa, fb);
        } else if (kind < 86) {
            /* Yapısal IF / ELSE / IF_END (isteğe bağlı iç içe IF + SELECT birleştirmesi) */
            u32 c_outer = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            u32 base_a  = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 base_b  = f32_vals[rng_range(rng, 0, (i32)n_f32)];

            ir_if_begin(f, c_outer);
            u32 t_val = ir_op2(f, IR_FADD, base_a, base_b);
            if (rng_range(rng, 0, 2) == 0) {
                /* İç içe IF */
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

            /* Anlambilim kuralı: IF dallarında üretilen değerler dışarıda c_outer SELECT ile birleşir */
            u32 merged = ir_select(f, c_outer, t_val, e_val);
            if (n_f32 < ARRAY_LEN(f32_vals)) f32_vals[n_f32++] = merged;
        } else if (kind < 94) {
            /* Doku örnekleme (nearest veya bilinear, wrap veya clamp) */
            u32 slot  = (u32)rng_range(rng, 0, 2);
            u32 u_val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 v_val = f32_vals[rng_range(rng, 0, (i32)n_f32)];
            u32 flags = (u32)rng_range(rng, 0, 8); /* wrap/clamp x nearest/bilinear x mipmap */
            u32 rgba[4];
            if (rng_range(rng, 0, 4) == 0) {
                /* açık seviye: rastgele i32 (kırpma yolunu da sınar) */
                u32 lv = i32_vals[rng_range(rng, 0, (i32)n_i32)];
                ir_emit_sample_level(f, slot, u_val, v_val, lv, flags, rgba);
            } else {
                ir_emit_sample(f, slot, u_val, v_val, flags, rgba);
            }
            for (u32 c = 0; c < 4 && n_f32 < ARRAY_LEN(f32_vals); c++) {
                f32_vals[n_f32++] = rgba[c];
            }
        } else {
            /* Doğrudan DISCARD */
            u32 m = bool_vals[rng_range(rng, 0, (i32)n_bool)];
            ir_discard(f, m);
        }
    }

    UNUSED(prog_idx);
    u32 r_out = f32_vals[n_f32 - 1];
    u32 g_out = f32_vals[n_f32 - 2];
    u32 b_out = f32_vals[n_f32 - 3];
    u32 a_out = f32_vals[rng_range(rng, 0, (i32)n_f32)];
    emit_rgba_outputs(f, r_out, g_out, b_out, a_out);
}

#define DIFF_FB_W 16
#define DIFF_FB_H 8
#define DIFF_FB_PIXELS (DIFF_FB_W * DIFF_FB_H)

TEST(matoki_differential_500) {
    ensure_matoki_test_dir();

    static u8 arena_buf[MB(8)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    Rng rng;
    rng_seed(&rng, 0xCAFEBABE12345678ull);

    const u32 NUM_PROGS = 500;
    FILE* list_fp = fopen("build/test_matoki/batch_list.txt", "w");
    CHECK(list_fp != NULL);

    /* Her program için beklenen ir_eval sonuçlarını bellekte saklayalım */
    typedef struct {
        u32 count;
        f32 eval_rgba[16 * 4]; /* max 4 quad * 4 ch * 4 lane */
        u32 eval_masks[4];
        u32 eval_fb[DIFF_FB_PIXELS];
    } ExpectedResult;

    static ExpectedResult exp_unopt[500];
    static ExpectedResult exp_opt[500];

    for (u32 p = 0; p < NUM_PROGS; p++) {
        usize mark = arena_mark(&arena);

        IrFunc f_unopt;
        ir_init(&f_unopt, &arena, 1024);
        gen_random_ir(&f_unopt, &rng, p);

        char err[256];
        if (!ir_verify(&f_unopt, err, sizeof(err))) {
            printf("ir_verify failed on unopt prog %u: %s\n", p, err);
            CHECK(FALSE);
        }

        /* Kopyasını oluştur ve optimize et (LICM dahil tüm paslar) */
        IrFunc f_opt = f_unopt;
        f_opt.insts = ARENA_PUSH_ARRAY(&arena, IrInst, f_unopt.cap);
        memcpy(f_opt.insts, f_unopt.insts, sizeof(IrInst) * f_unopt.count);
        ir_optimize(&f_opt);
        if (!ir_verify(&f_opt, err, sizeof(err))) {
            printf("ir_verify failed on opt prog %u: %s\n", p, err);
            CHECK(FALSE);
        }

        /* Rastgele ShadeBatch oluştur (1..4 quad, DIFF_FB_W x DIFF_FB_H içinde ayrık koordinatlar) */
        u32 qcount = (u32)rng_range(&rng, 1, 5);
        QuadHeader quads_u[4], quads_o[4], quads_f[4];
        for (u32 q = 0; q < qcount; q++) {
            quads_u[q].mask = (u32)rng_range(&rng, 1, 16);
            quads_u[q].x    = (u16)((q * 2 + (u32)rng_range(&rng, 0, 2) * 8) % DIFF_FB_W);
            quads_u[q].y    = (u16)(((q / 2) * 2 + (u32)rng_range(&rng, 0, 2) * 4) % DIFF_FB_H);
            quads_o[q]      = quads_u[q];
            quads_f[q]      = quads_u[q];
        }

        f32 varyings[4 * 4 * 4];
        for (u32 i = 0; i < 4 * qcount * 4; i++) {
            varyings[i] = rng_f32(&rng) * 4.0f - 1.5f;
        }

        u8 uniforms[32];
        f32 u_f0 = rng_f32(&rng) * 2.0f - 0.5f;
        f32 u_f1 = rng_f32(&rng) * 2.0f + 0.1f;
        i32 u_i0 = rng_range(&rng, -10, 15);
        i32 u_i1 = rng_range(&rng, 0, 8);
        memset(uniforms, 0, sizeof(uniforms));
        memcpy(uniforms + 0,  &u_f0, 4);
        memcpy(uniforms + 4,  &u_f1, 4);
        memcpy(uniforms + 8,  &u_i0, 4);
        memcpy(uniforms + 12, &u_i1, 4);

        /* mip zincirleri: 4x4 (16+4+1) ve 8x8 (64+16+4+1) */
        u32 tex0_px[21], tex1_px[85];
        for (u32 i = 0; i < 21; i++) tex0_px[i] = rng_u32(&rng);
        for (u32 i = 0; i < 85; i++) tex1_px[i] = rng_u32(&rng);

        u32 fb_init[DIFF_FB_PIXELS];
        for (u32 i = 0; i < DIFF_FB_PIXELS; i++) {
            fb_init[i] = rng_u32(&rng);
            exp_opt[p].eval_fb[i] = fb_init[i];
        }

        Texture textures[2];
        memset(textures, 0, sizeof(textures));
        textures[0].px = tex0_px; textures[0].w = 4; textures[0].h = 4;
        textures[0].w_mask = 3; textures[0].h_mask = 3; textures[0].log2_w = 2;
        textures[0].flags = (u32)rng_range(&rng, 0, 4);
        textures[0].levels = (u32)rng_range(&rng, 0, 4);   /* 0 → 1 kabul edilir */

        textures[1].px = tex1_px; textures[1].w = 8; textures[1].h = 8;
        textures[1].w_mask = 7; textures[1].h_mask = 7; textures[1].log2_w = 3;
        textures[1].flags = (u32)rng_range(&rng, 0, 4);
        textures[1].levels = (u32)rng_range(&rng, 1, 5);

        ShadeBatch batch_u = {0};
        batch_u.count         = qcount;
        batch_u.varying_count = 4;
        batch_u.quads         = quads_u;
        batch_u.varyings      = varyings;
        batch_u.uniforms      = uniforms;
        batch_u.out_rgba      = exp_unopt[p].eval_rgba;
        batch_u.textures      = textures;
        batch_u.fb_px         = exp_opt[p].eval_fb;
        batch_u.fb_stride     = DIFF_FB_W;

        char in_path[128], wasm_u_path[128], wasm_o_path[128], wasm_f_path[128];
        char out_u_path[128], out_o_path[128], out_f_path[128];
        snprintf(in_path,     sizeof(in_path),     "build/test_matoki/in_%03u.bin", p);
        snprintf(wasm_u_path, sizeof(wasm_u_path), "build/test_matoki/mod_u_%03u.wasm", p);
        snprintf(wasm_o_path, sizeof(wasm_o_path), "build/test_matoki/mod_o_%03u.wasm", p);
        snprintf(wasm_f_path, sizeof(wasm_f_path), "build/test_matoki/mod_f_%03u.wasm", p);
        snprintf(out_u_path,  sizeof(out_u_path),  "build/test_matoki/out_u_%03u.bin", p);
        snprintf(out_o_path,  sizeof(out_o_path),  "build/test_matoki/out_o_%03u.bin", p);
        snprintf(out_f_path,  sizeof(out_f_path),  "build/test_matoki/out_f_%03u.bin", p);

        /* Girdi ikilisini kaydet (ir_eval maskeleri ve fb_px'i değiştirmeden önce) */
        CHECK(write_batch_bin(in_path, &batch_u, 32, 2, DIFF_FB_W, DIFF_FB_H, fb_init));

        /* Referans yorumlayıcıyı unopt, opt ve fused (REPLACE/ALPHA/ADD) üzerinde çalıştır */
        u32 fuse_mode = (p % 3u) + 1u; /* 1: REPLACE, 2: ALPHA, 3: ADD */
        ir_eval(&f_unopt, &batch_u);
        exp_unopt[p].count = qcount;
        for (u32 q = 0; q < qcount; q++) exp_unopt[p].eval_masks[q] = quads_u[q].mask;

        ShadeBatch batch_o = batch_u;
        batch_o.quads    = quads_o;
        batch_o.out_rgba = exp_opt[p].eval_rgba;
        ir_eval(&f_opt, &batch_o);
        exp_opt[p].count = qcount;
        for (u32 q = 0; q < qcount; q++) exp_opt[p].eval_masks[q] = quads_o[q].mask;

        f32 dummy_rgba[64];
        ShadeBatch batch_f = batch_u;
        batch_f.quads    = quads_f;
        batch_f.out_rgba = dummy_rgba;
        batch_f.fb_px    = exp_opt[p].eval_fb;
        batch_f.fb_stride = DIFF_FB_W;
        ir_eval_ex(&f_opt, &batch_f, fuse_mode);

        /* Optimizasyon öncesi ve sonrası ir_eval sonuçları aktif lane'lerde birebir eşleşmeli */
        for (u32 q = 0; q < qcount; q++) {
            CHECK(exp_unopt[p].eval_masks[q] == exp_opt[p].eval_masks[q]);
            CHECK(quads_f[q].mask == exp_opt[p].eval_masks[q]);
            u32 m = exp_unopt[p].eval_masks[q];
            for (u32 ch = 0; ch < 4; ch++) {
                for (u32 lane = 0; lane < 4; lane++) {
                    if (!(m & (1u << lane))) continue;
                    u32 idx = (ch * qcount + q) * 4 + lane;
                    CHECK(f32_bit_eq_or_nan(exp_unopt[p].eval_rgba[idx], exp_opt[p].eval_rgba[idx]));
                }
            }
        }

        /* Codegen (unopt, opt ve fused) */
        MtkCodegenOpts cg_opts = { FALSE, TRUE, MTK_FUSE_NONE };
        MtkCodegenOpts cg_opts_f = { FALSE, TRUE, fuse_mode };
        u8 wasm_buf[65536];

        u32 len_u = mtk_codegen(&f_unopt, &cg_opts, &arena, wasm_buf, sizeof(wasm_buf), err, sizeof(err));
        if (len_u == 0) {
            printf("mtk_codegen unopt failed on prog %u: %s\n", p, err);
            CHECK(FALSE);
        }
        FILE* fpu = fopen(wasm_u_path, "wb");
        CHECK(fpu != NULL);
        fwrite(wasm_buf, 1, len_u, fpu);
        fclose(fpu);

        u32 len_o = mtk_codegen(&f_opt, &cg_opts, &arena, wasm_buf, sizeof(wasm_buf), err, sizeof(err));
        if (len_o == 0) {
            printf("mtk_codegen opt failed on prog %u: %s\n", p, err);
            CHECK(FALSE);
        }
        FILE* fpo = fopen(wasm_o_path, "wb");
        CHECK(fpo != NULL);
        fwrite(wasm_buf, 1, len_o, fpo);
        fclose(fpo);

        u32 len_f = mtk_codegen(&f_opt, &cg_opts_f, &arena, wasm_buf, sizeof(wasm_buf), err, sizeof(err));
        if (len_f == 0) {
            printf("mtk_codegen fused failed on prog %u: %s\n", p, err);
            CHECK(FALSE);
        }
        FILE* fpf = fopen(wasm_f_path, "wb");
        CHECK(fpf != NULL);
        fwrite(wasm_buf, 1, len_f, fpf);
        fclose(fpf);

        fprintf(list_fp, "%s %s %s\n", wasm_u_path, in_path, out_u_path);
        fprintf(list_fp, "%s %s %s\n", wasm_o_path, in_path, out_o_path);
        fprintf(list_fp, "%s %s %s\n", wasm_f_path, in_path, out_f_path);

        arena_pop_to(&arena, mark);
    }

    fclose(list_fp);

    /* Tüm üretilen modülleri wasm-validate --enable-all ile doğrula */
    int rc_val = system("sh -c 'for f in build/test_matoki/mod_*.wasm; do wasm-validate --enable-all \"$f\" || exit 1; done'");
    CHECK(rc_val == 0);

    /* Tüm 1500 koşumu (500 unopt + 500 opt + 500 fused) node üzerinden çalıştır */
    int rc_node = system("node tools/host/run_shade.mjs --batch build/test_matoki/batch_list.txt");
    CHECK(rc_node == 0);

    /* Sonuçları bit-bit karşılaştır (aktif lane'ler, quad maskeleri ve kaynaştırılmış fb_px) */
    u32 mismatches = 0;
    for (u32 p = 0; p < NUM_PROGS; p++) {
        char out_u_path[128], out_o_path[128], out_f_path[128];
        snprintf(out_u_path, sizeof(out_u_path), "build/test_matoki/out_u_%03u.bin", p);
        snprintf(out_o_path, sizeof(out_o_path), "build/test_matoki/out_o_%03u.bin", p);
        snprintf(out_f_path, sizeof(out_f_path), "build/test_matoki/out_f_%03u.bin", p);

        f32 wasm_rgba_u[64], wasm_rgba_o[64], wasm_rgba_f[64];
        u32 wasm_masks_u[4], wasm_masks_o[4], wasm_masks_f[4];
        u32 wasm_fb_u[DIFF_FB_PIXELS], wasm_fb_o[DIFF_FB_PIXELS], wasm_fb_f[DIFF_FB_PIXELS];
        u32 qc = exp_unopt[p].count;

        CHECK(read_output_bin(out_u_path, qc, wasm_rgba_u, wasm_masks_u, DIFF_FB_PIXELS, wasm_fb_u));
        CHECK(read_output_bin(out_o_path, qc, wasm_rgba_o, wasm_masks_o, DIFF_FB_PIXELS, wasm_fb_o));
        CHECK(read_output_bin(out_f_path, qc, wasm_rgba_f, wasm_masks_f, DIFF_FB_PIXELS, wasm_fb_f));

        for (u32 q = 0; q < qc; q++) {
            u32 m = exp_unopt[p].eval_masks[q];
            if (wasm_masks_u[q] != m || wasm_masks_o[q] != exp_opt[p].eval_masks[q] ||
                wasm_masks_f[q] != exp_opt[p].eval_masks[q]) {
                printf("prog %u quad %u mask mismatch: eval=0x%x wasm_u=0x%x wasm_o=0x%x wasm_f=0x%x\n",
                       p, q, m, wasm_masks_u[q], wasm_masks_o[q], wasm_masks_f[q]);
                mismatches++;
            }
            for (u32 ch = 0; ch < 4; ch++) {
                for (u32 lane = 0; lane < 4; lane++) {
                    if (!(m & (1u << lane))) continue;
                    u32 i = (ch * qc + q) * 4 + lane;
                    if (!f32_bit_eq_or_nan(wasm_rgba_u[i], exp_unopt[p].eval_rgba[i]) ||
                        !f32_bit_eq_or_nan(wasm_rgba_o[i], exp_opt[p].eval_rgba[i])) {
                        printf("prog %u q=%u ch=%u lane=%u f32 mismatch: eval=%f (0x%08x) wasm_u=%f (0x%08x) wasm_o=%f (0x%08x)\n",
                               p, q, ch, lane,
                               (double)exp_unopt[p].eval_rgba[i], f32_to_u32(exp_unopt[p].eval_rgba[i]),
                               (double)wasm_rgba_u[i], f32_to_u32(wasm_rgba_u[i]),
                               (double)wasm_rgba_o[i], f32_to_u32(wasm_rgba_o[i]));
                        mismatches++;
                    }
                }
            }
        }

        for (u32 px = 0; px < DIFF_FB_PIXELS; px++) {
            if (wasm_fb_f[px] != exp_opt[p].eval_fb[px]) {
                printf("prog %u (fuse_mode=%u) fb_px[%u] mismatch: eval=0x%08x wasm_f=0x%08x\n",
                       p, (p % 3u) + 1u, px, exp_opt[p].eval_fb[px], wasm_fb_f[px]);
                mismatches++;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST(matoki_licm_unit) {
    static u8 arena_buf[MB(1)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    /* Elle kurulan IR:
     * Döngüden bağımsız (uniform/sabit/doku boyutu) hesaplar hem üst seviyede hem de
     * varying IF bloğunun içinde yer alır; ir_opt_licm bunların hepsini döngü önüne (preheader)
     * taşımalı ve döngü içinde yalnızca varying + kontrol akışı + output komutlarını bırakmalıdır. */
    IrFunc f;
    ir_init(&f, &arena, 128);
    f.uniform_bytes = 12;
    f.varying_count = 2;
    f.texture_count = 1;

    u32 v0     = ir_varying(&f, 0);
    u32 v1     = ir_varying(&f, 1);
    u32 u0     = ir_uniform(&f, IR_T_F32, 0);
    u32 u1     = ir_uniform(&f, IR_T_F32, 4);
    u32 c_two  = ir_const_f32(&f, 2.0f);
    u32 c_half = ir_const_f32(&f, 0.5f);
    u32 tw     = ir_tex_info(&f, IR_TEX_W, 0);

    /* Üst seviye uniform hesap: u_scale = (u0 + u1) * 2.0 */
    u32 u_sum   = ir_op2(&f, IR_FADD, u0, u1);
    u32 u_scale = ir_op2(&f, IR_FMUL, u_sum, c_two);

    /* Varying IF bloğu içinde saf uniform hesap */
    u32 cond_v = ir_op2(&f, IR_FGT, v0, c_half);
    ir_if_begin(&f, cond_v);
    u32 u_diff   = ir_op2(&f, IR_FSUB, u0, u1);
    u32 tw_f     = ir_op1(&f, IR_I2F, tw);
    u32 u_in_if  = ir_op2(&f, IR_FADD, u_diff, tw_f);
    u32 v_branch = ir_op2(&f, IR_FMUL, v0, u_in_if);
    ir_if_end(&f);

    u32 v_sel = ir_select(&f, cond_v, v_branch, v1);
    u32 out_r = ir_op2(&f, IR_FADD, v_sel, u_scale);
    emit_rgba_outputs(&f, out_r, v0, v1, c_half);

    /* LICM öncesi: tüm 22 komut döngü içindedir (IR_FLAG_HOISTED yok) */
    CHECK(ir_count_loop_insts(&f) == 22);

    ir_opt_licm(&f);

    /* LICM sonrası döngüde kalan komut sayısı tam olarak 12 olmalı:
     * v0, v1, cond_v, if_begin, v_branch, if_end, v_sel, out_r, output0..3
     * (Taşınan 10 komut: u0, u1, c_two, c_half, tw, u_sum, u_scale, u_diff, tw_f, u_in_if) */
    CHECK(ir_count_loop_insts(&f) == 12);

    /* İlk 10 komutun tamamı IR_FLAG_HOISTED bayrağına sahip olmalı */
    for (u32 i = 0; i < 10; i++) {
        CHECK((f.insts[i].flags & IR_FLAG_HOISTED) != 0);
        CHECK((f.insts[i].flags & IR_FLAG_VARYING) == 0);
    }

    char err[256];
    CHECK(ir_verify(&f, err, sizeof(err)));
}

TEST(matoki_variant_cache_unit) {
    static u8 arena_buf[MB(2)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    MtkVariantCache cache;
    mtk_variant_cache_init(&cache, &arena);

    const char* sprite_src =
        "uniform tint: vec4;\n"
        "uniform tex:  texture2d;\n"
        "in uv: vec2;\n"
        "fn frag() -> rgba {\n"
        "    let c = sample(tex, uv);\n"
        "    if c.a < 0.01 { discard; }\n"
        "    return c * tint;\n"
        "}\n";
    u32 src_len = (u32)strlen(sprite_src);

    MtkVariantKey key_base;
    memset(&key_base, 0, sizeof(key_base));
    key_base.src     = sprite_src;
    key_base.src_len = src_len;
    key_base.src_hash = mtk_hash_source(sprite_src, src_len);

    MtkDiag diag;
    const MtkVariant* v1 = mtk_variant_get(&cache, &key_base, &arena, &diag);
    CHECK(v1 != NULL);
    CHECK(v1->wasm_len > 0);
    CHECK(cache.count == 1);
    CHECK(cache.hits == 0);
    CHECK(cache.misses == 1);

    /* Aynı anahtarla ikinci çağrı: önbellekten (hit) birebir aynı modülü döndürmeli */
    const MtkVariant* v2 = mtk_variant_get(&cache, &key_base, &arena, &diag);
    CHECK(v2 == v1);
    CHECK(v2->wasm == v1->wasm);
    CHECK(v2->wasm_len == v1->wasm_len);
    CHECK(cache.count == 1);
    CHECK(cache.hits == 1);
    CHECK(cache.misses == 1);

    /* Farklı anahtar: tint = (1, 1, 1, 1) olarak özelleştirildiğinde c * tint çarpımları
     * ve tint splat'leri elenmeli; daha kısa wasm modülü ve daha az döngü komutu üretilmeli! */
    MtkVariantKey key_tint1 = key_base;
    key_tint1.spec_count = 4;
    for (u32 c = 0; c < 4; c++) {
        key_tint1.specs[c].byte_offset = c * 4;
        key_tint1.specs[c].value       = 1.0f;
    }

    const MtkVariant* v_spec = mtk_variant_get(&cache, &key_tint1, &arena, &diag);
    CHECK(v_spec != NULL);
    CHECK(v_spec != v1);
    CHECK(cache.count == 2);
    CHECK(cache.misses == 2);
    CHECK(v_spec->wasm_len < v1->wasm_len);
    CHECK(v_spec->loop_inst_count <= v1->loop_inst_count);
    CHECK(v_spec->wasm_loop_ops <= v1->wasm_loop_ops);
}

TEST(matoki_sprite_shader_codegen) {
    ensure_matoki_test_dir();

    static u8 arena_buf[MB(2)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    /* docs/MATOKI.md §2'deki örnek sprite shader:
     *   uniform tint: vec4;
     *   uniform tex:  texture2d;
     *   in uv: vec2;
     *   fn frag() -> rgba {
     *       let c = sample(tex, uv);
     *       if c.a < 0.01 { discard; }
     *       return c * tint;
     *   }
     */
    IrFunc f;
    ir_init(&f, &arena, 256);
    f.uniform_bytes = 16;
    f.varying_count = 2;
    f.texture_count = 1;

    u32 tint_r = ir_uniform(&f, IR_T_F32, 0);
    u32 tint_g = ir_uniform(&f, IR_T_F32, 4);
    u32 tint_b = ir_uniform(&f, IR_T_F32, 8);
    u32 tint_a = ir_uniform(&f, IR_T_F32, 12);

    u32 uv_u = ir_varying(&f, 0);
    u32 uv_v = ir_varying(&f, 1);

    u32 c_rgba[4];
    ir_emit_sample(&f, 0, uv_u, uv_v, 0 /* nearest + wrap */, c_rgba);

    u32 thresh = ir_const_f32(&f, 0.01f);
    u32 cond   = ir_op2(&f, IR_FLT, c_rgba[3], thresh);
    ir_if_begin(&f, cond);
    ir_discard(&f, cond);
    ir_if_end(&f);

    u32 out_r = ir_op2(&f, IR_FMUL, c_rgba[0], tint_r);
    u32 out_g = ir_op2(&f, IR_FMUL, c_rgba[1], tint_g);
    u32 out_b = ir_op2(&f, IR_FMUL, c_rgba[2], tint_b);
    u32 out_a = ir_op2(&f, IR_FMUL, c_rgba[3], tint_a);
    emit_rgba_outputs(&f, out_r, out_g, out_b, out_a);

    ir_optimize(&f);

    char err[256];
    CHECK(ir_verify(&f, err, sizeof(err)));

    MtkCodegenOpts opts = { FALSE, FALSE, MTK_FUSE_NONE };
    u8 wasm_buf[16384];
    u32 wasm_len = mtk_codegen(&f, &opts, &arena, wasm_buf, sizeof(wasm_buf), err, sizeof(err));
    CHECK(wasm_len > 0);

    FILE* fp = fopen("build/test_matoki/sprite.wasm", "wb");
    CHECK(fp != NULL);
    fwrite(wasm_buf, 1, wasm_len, fp);
    fclose(fp);

    mkdir("build/test_out", 0755);
    FILE* fp2 = fopen("build/test_out/sprite.wasm", "wb");
    CHECK(fp2 != NULL);
    fwrite(wasm_buf, 1, wasm_len, fp2);
    fclose(fp2);

    CHECK(system("wasm-validate --enable-all build/test_matoki/sprite.wasm") == 0);
}

/* P4: Basit, discard'sız çekirdeklerde v128.load32_splat ve 4x döngü açmanın (offset=0/16/32/48)
 * üretildiğini doğrular. */
TEST(matoki_unroll4x_and_load32_splat) {
    ensure_matoki_test_dir();

    static u8 arena_buf[MB(2)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    const char* src =
        "uniform tint: vec4;\n"
        "in v_color: vec4;\n"
        "fn frag() -> rgba { return v_color * tint; }\n";

    u8 wasm_buf[16384];
    MtkLayout layout;
    MtkDiag diag;
    u32 wasm_len = mtk_compile_wasm(src, (u32)strlen(src), "vcolor_p4.mtk", NULL, &arena,
                                    wasm_buf, sizeof(wasm_buf), &layout, &diag);
    CHECK(wasm_len > 0);
    CHECK(diag.count == 0);

    MdisResult dr = mdis_disassemble(&arena, wasm_buf, wasm_len);
    CHECK(dr.ok);

    /* 4 adet uniform bileşeni v128.load32_splat ile yüklenmeli */
    u32 splat_cnt = 0;
    const char* p = dr.wat;
    while ((p = strstr(p, "v128.load32_splat")) != NULL) {
        splat_cnt++;
        p += 17;
    }
    CHECK(splat_cnt == 4);

    /* 4x açılmış döngüde offset=16, offset=32, offset=48 bellek erişimleri bulunmalı */
    CHECK(strstr(dr.wat, "offset=16") != NULL);
    CHECK(strstr(dr.wat, "offset=32") != NULL);
    CHECK(strstr(dr.wat, "offset=48") != NULL);
}

/* P5: Sabit alfa eşiği karşılaştırmasının (c.a < 0.01) tamsayı alanında (i32x4.lt_s, eşik = 3)
 * yapıldığını ve gereksiz f32x4.lt / f32x4.mul komutlarının elendiğini doğrular. */
TEST(matoki_alpha_cmp_int_fold) {
    ensure_matoki_test_dir();

    static u8 arena_buf[MB(2)];
    Arena arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf));

    const char* src =
        "uniform tint: vec4;\n"
        "uniform tex: texture2d;\n"
        "in uv: vec2;\n"
        "fn frag() -> rgba {\n"
        "    let c: vec4 = sample(tex, uv);\n"
        "    if (c.a < 0.01) { discard; }\n"
        "    return c * tint;\n"
        "}\n";

    u8 wasm_buf[16384];
    MtkLayout layout;
    MtkDiag diag;
    u32 wasm_len = mtk_compile_wasm(src, (u32)strlen(src), "sprite_p5.mtk", NULL, &arena,
                                    wasm_buf, sizeof(wasm_buf), &layout, &diag);
    CHECK(wasm_len > 0);
    CHECK(diag.count == 0);

    MdisResult dr = mdis_disassemble(&arena, wasm_buf, wasm_len);
    CHECK(dr.ok);

    /* Alfa karşılaştırması f32x4.lt yerine i32x4.lt_s ve i32x4 3 3 3 3 sabitiyle yapılmalı */
    CHECK(strstr(dr.wat, "f32x4.lt") == NULL);
    CHECK(strstr(dr.wat, "i32x4.lt_s") != NULL);
    CHECK(strstr(dr.wat, "v128.const i32x4 3 3 3 3") != NULL);
}
