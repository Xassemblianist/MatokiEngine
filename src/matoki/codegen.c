/* matoki/codegen.c — Matoki IR -> WebAssembly SIMD ikili modül üreticisi (D hattı).
 * mbin kullanarak (param $batch i32) -> () imzalı "shade" fonksiyonunu üretir.
 * Döngüden bağımsız (uniform/sabit/doku) hesapları ve splat'leri döngü önüne (preheader) taşır,
 * tek kullanımlı ifadeleri özyinelemeli ifade ağacı (tree-ification) ile yığında birleştirir,
 * ve isteğe bağlı olarak blend aşamasını (REPLACE/ALPHA/ADD) doğrudan shader sonuna kaynaştırır.
 */
#include "matoki/codegen.h"
#include "matoki/variant.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"
#include "core/log.h"

#define CG_MAX_INSTS 4096
#define CG_MAX_POOL  512
#define CG_MAX_IF    64

static inline u32 cg_f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

typedef struct {
    const IrFunc*         f;
    const MtkCodegenOpts* opts;
    MbFunc*               mf;

    /* Sabit görevli wasm local indeksleri */
    u32 loc_batch;           /* param 0: ShadeBatch* */
    u32 loc_count;           /* i32: batch->count */
    u32 loc_quads;           /* i32: batch->quads */
    u32 loc_varyings;        /* i32: batch->varyings */
    u32 loc_uniforms;        /* i32: batch->uniforms */
    u32 loc_out_rgba;        /* i32: batch->out_rgba */
    u32 loc_textures;        /* i32: batch->textures */
    u32 loc_q;               /* i32: döngü sayacı q */
    u32 loc_q16;             /* i32: q * 16 (varying / output byte ofseti) */
    u32 loc_qhdr;            /* i32: &quads[q] */
    u32 loc_tmp_i32;         /* i32: geçici skaler 0 */
    u32 loc_tmp_i32_b;       /* i32: geçici skaler 1 */
    u32 loc_fb_px;           /* i32: batch->fb_px (fuse_blend != 0) */
    u32 loc_fb_stride;       /* i32: batch->fb_stride (fuse_blend != 0) */
    u32 loc_fb_stride_bytes; /* i32: batch->fb_stride * 4 (fuse_blend != 0) */
    u32 loc_fb_addr0;        /* i32: quad satır 0 fb adresi */
    u32 loc_fb_addr1;        /* i32: quad satır 1 fb adresi */

    /* Döngü önü (preheader) taban adresleri */
    u32 loc_var_base[SHADE_MAX_VARYINGS];
    u32 loc_out_base[4];
    u32 loc_tex_px[SHADE_MAX_TEXTURES];
    u32 loc_tex_px_v[SHADE_MAX_TEXTURES];

    /* Sabit görevli v128 local'ler */
    u32 loc_active;          /* v128: aktif lane maskesi (varying IF / discard için) */
    u32 loc_blend_mask;      /* v128: quad görünürlük maskesi (fuse_blend için) */
    u32 loc_tmp_v0;          /* v128: geçici vektör 0 */
    u32 loc_tmp_v1;          /* v128: geçici vektör 1 */
    u32 loc_v_mask_bits;     /* v128: i32x4(1, 2, 4, 8) */
    u32 loc_v_sign_mask;     /* v128: f32x4(-0.0, -0.0, -0.0, -0.0) */
    u32 loc_v_frag_dx;       /* v128: f32x4(0.5, 1.5, 0.5, 1.5) */
    u32 loc_v_frag_dy;       /* v128: f32x4(0.5, 0.5, 1.5, 1.5) */
    u32 loc_v_zero;          /* v128: f32x4(0, 0, 0, 0) */
    u32 loc_v_one;           /* v128: f32x4(1, 1, 1, 1) */
    u32 loc_v_255;           /* v128: f32x4(255, 255, 255, 255) */
    u32 loc_v_half;          /* v128: f32x4(0.5, 0.5, 0.5, 0.5) */
    u32 loc_v_inv255;        /* v128: f32x4(1/255, ...) */
    u32 loc_v_ff;            /* v128: i32x4(0xFF, ...) */
    u32 loc_fb_dst;          /* v128: hedef 4 piksel RGBA8 */
    u32 loc_out_ch[4];       /* v128: kaynaştırılmış OUTPUT kanalları 0..3 */

    /* Özellik kullanım bayrakları */
    b32 used_varying[SHADE_MAX_VARYINGS];
    b32 used_tex_info[SHADE_MAX_TEXTURES];
    b32 used_tex_load_u[SHADE_MAX_TEXTURES];
    b32 used_tex_load_v[SHADE_MAX_TEXTURES];
    b32 has_any_varying;
    b32 has_any_uniform;
    b32 has_any_tex;
    b32 has_frag_x;
    b32 has_frag_y;
    b32 has_discard;
    b32 has_discard_in_if;
    b32 has_deriv;           /* DERIV_X/Y var: yardımcı (kapsanmayan) lane'ler de bölgeleri çalıştırır */
    b32 need_qhdr;
    b32 need_active_mask;
    b32 need_q16;
    b32 need_sign_mask;

    /* Her IR komutu için analiz ve tahsis tabloları */
    u32 use_count[CG_MAX_INSTS];
    u32 scalar_use_count[CG_MAX_INSTS];
    u32 splat_use_count[CG_MAX_INSTS];
    u32 last_use[CG_MAX_INSTS];
    u32 single_user[CG_MAX_INSTS];
    u32 inst_region[CG_MAX_INSTS];
    b32 is_preheader[CG_MAX_INSTS];
    b32 used_in_loop[CG_MAX_INSTS];
    b32 need_splat[CG_MAX_INSTS];
    b32 inlined[CG_MAX_INSTS];
    u8  select_fneg_mode[CG_MAX_INSTS]; /* 0: normal, 1: select(c, -x, x), 2: select(c, x, -x) */
    u32 val_loc[CG_MAX_INSTS];
    u32 splat_loc[CG_MAX_INSTS];

    /* Varying IF blokları için durum yığını */
    struct {
        b32 is_varying;
        u32 saved_mask_loc;
        u32 cond_mask_loc;
        b32 has_else;
    } if_stk[CG_MAX_IF];
    u32 if_depth;
    u32 max_var_if_depth;
    u32 if_saved_locs[CG_MAX_IF];
    u32 if_cond_locs[CG_MAX_IF];

    /* İstatistikler */
    u32 stat_raw_locals;
    u32 stat_pooled_locals;
    u32 stat_inlined_ops;
} CgCtx;

static b32 cg_fail(char* err, u32 err_cap, const char* fmt, ...) {
    if (err && err_cap > 0) {
        VaList ap;
        VA_START(ap, fmt);
        fmt_vformat(err, err_cap, fmt, ap);
        VA_END(ap);
    }
    return FALSE;
}

static u8 ir_to_wasm_valtype(const IrInst* in) {
    if (in->flags & IR_FLAG_VARYING) {
        return MB_VAL_V128;
    }
    if (in->type == IR_T_F32) {
        return MB_VAL_F32;
    }
    return MB_VAL_I32;
}

static b32 is_const_op(IrOp op) {
    return op == IR_CONST_F32 || op == IR_CONST_I32 || op == IR_CONST_BOOL;
}

static b32 is_all_zero_const(const IrInst* in) {
    if (in->op == IR_CONST_I32 || in->op == IR_CONST_BOOL) {
        return in->imm.i == 0;
    }
    if (in->op == IR_CONST_F32) {
        return cg_f32_bits(in->imm.f) == 0u;
    }
    return FALSE;
}

static b32 is_all_ones_const(const IrInst* in) {
    if (in->op == IR_CONST_I32) {
        return in->imm.i == -1;
    }
    if (in->op == IR_CONST_BOOL) {
        return in->imm.i != 0;
    }
    if (in->op == IR_CONST_F32) {
        return cg_f32_bits(in->imm.f) == 0xFFFFFFFFu;
    }
    return FALSE;
}

static b32 can_inline_inst(const IrFunc* f, u32 idx) {
    const IrInst* in = &f->insts[idx];
    if (in->op == IR_NOP || in->type == IR_T_VOID) return FALSE;
    return TRUE;
}

/* Bir ifade ağacının (root) altında son kullanımı <= root olan farklı döngü-içi
 * local'lerin (inlined olmayan) sayısını hesaplar. */
static u32 count_dying_loop_locals(const CgCtx* ctx, u32 node, u32 root,
                                   u32* seen, u32* seen_cnt) {
    const IrInst* in = &ctx->f->insts[node];
    u32 ops[3] = { in->a, in->b, in->c };
    if (in->op == IR_SELECT && ctx->select_fneg_mode[node] == 3) {
        ops[0] = ctx->f->insts[ctx->f->insts[in->a].a].a;
        ops[1] = IR_NONE;
    }
    for (u32 k = 0; k < 3; k++) {
        u32 op_id = ops[k];
        if (op_id == IR_NONE) continue;
        if (ctx->inlined[op_id]) {
            count_dying_loop_locals(ctx, op_id, root, seen, seen_cnt);
        } else if (!ctx->is_preheader[op_id] && ctx->last_use[op_id] <= root) {
            b32 dup = FALSE;
            for (u32 s = 0; s < *seen_cnt; s++) {
                if (seen[s] == op_id) { dup = TRUE; break; }
            }
            if (!dup && *seen_cnt < 16) {
                seen[(*seen_cnt)++] = op_id;
            }
        }
    }
    return *seen_cnt;
}

static void analyze_liveness_and_trees(CgCtx* ctx) {
    const IrFunc* f = ctx->f;
    u32 n = f->count;

    for (u32 i = 0; i < n; i++) {
        ctx->use_count[i]        = 0;
        ctx->scalar_use_count[i] = 0;
        ctx->splat_use_count[i]  = 0;
        ctx->last_use[i]         = i;
        ctx->single_user[i]      = IR_NONE;
        ctx->inst_region[i]      = 0;
        ctx->is_preheader[i]     = FALSE;
        ctx->used_in_loop[i]     = FALSE;
        ctx->need_splat[i]       = FALSE;
        ctx->inlined[i]          = FALSE;
        ctx->select_fneg_mode[i] = 0;
        ctx->val_loc[i]          = IR_NONE;
        ctx->splat_loc[i]        = IR_NONE;
    }

    /* 1. Bölge (IF/ELSE), varying IF derinliği, kullanılan özellikler ve preheader uygunluğunu belirle */
    u32 cur_reg = 0;
    u32 next_reg = 1;
    u32 reg_stk[CG_MAX_IF];
    b32 var_if_stk[CG_MAX_IF];
    u32 stk_d = 0;
    u32 cur_var_if = 0;
    u32 max_var_if = 0;

    for (u32 i = 0; i < n; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        if (op == IR_IF_BEGIN) {
            ctx->inst_region[i] = cur_reg;
            reg_stk[stk_d] = cur_reg;
            b32 is_v = (in->flags & IR_FLAG_VARYING) != 0;
            var_if_stk[stk_d] = is_v;
            stk_d++;
            if (is_v) {
                cur_var_if++;
                if (cur_var_if > max_var_if) max_var_if = cur_var_if;
            }
            cur_reg = next_reg++;
            continue;
        }
        if (op == IR_ELSE) {
            ctx->inst_region[i] = (stk_d > 0) ? reg_stk[stk_d - 1] : 0;
            cur_reg = next_reg++;
            continue;
        }
        if (op == IR_IF_END) {
            if (stk_d > 0) {
                stk_d--;
                if (var_if_stk[stk_d]) cur_var_if--;
                cur_reg = reg_stk[stk_d];
            }
            ctx->inst_region[i] = cur_reg;
            continue;
        }

        ctx->inst_region[i] = cur_reg;

        /* Özellik taraması */
        if (op == IR_VARYING && in->imm.u < SHADE_MAX_VARYINGS) {
            ctx->used_varying[in->imm.u] = TRUE;
            ctx->has_any_varying = TRUE;
        } else if (op == IR_UNIFORM_F32 || op == IR_UNIFORM_I32) {
            ctx->has_any_uniform = TRUE;
        } else if (op == IR_TEX_W || op == IR_TEX_H || op == IR_TEX_FLAGS || op == IR_TEX_LEVELS) {
            if (in->imm.u < SHADE_MAX_TEXTURES) ctx->used_tex_info[in->imm.u] = TRUE;
            ctx->has_any_tex = TRUE;
        } else if (op == IR_TEXEL_LOAD) {
            if (in->imm.u < SHADE_MAX_TEXTURES) {
                if (in->flags & IR_FLAG_VARYING) ctx->used_tex_load_v[in->imm.u] = TRUE;
                else                             ctx->used_tex_load_u[in->imm.u] = TRUE;
            }
            ctx->has_any_tex = TRUE;
        } else if (op == IR_FRAG_X) {
            ctx->has_frag_x = TRUE;
        } else if (op == IR_FRAG_Y) {
            ctx->has_frag_y = TRUE;
        } else if (op == IR_DERIV_X || op == IR_DERIV_Y) {
            ctx->has_deriv = TRUE;
        } else if (op == IR_DISCARD) {
            ctx->has_discard = TRUE;
            if (stk_d > 0) ctx->has_discard_in_if = TRUE;
        }

        /* Preheader uygunluğu:
         * Sabit, uniform ve doku boyutu/bayrağı okumaları her zaman döngü önüne alınabilir;
         * ayrıca üst seviyede (stk_d == 0) olup tüm operandları preheader'da olan
         * saf uniform komutlar da döngü önünde bir kez hesaplanır. */
        if (!(in->flags & IR_FLAG_VARYING) && in->type != IR_T_VOID) {
            if (is_const_op(op) || op == IR_UNIFORM_F32 || op == IR_UNIFORM_I32 ||
                op == IR_TEX_W || op == IR_TEX_H || op == IR_TEX_FLAGS || op == IR_TEX_LEVELS) {
                ctx->is_preheader[i] = TRUE;
            } else if ((stk_d == 0 || (in->flags & IR_FLAG_HOISTED)) &&
                       (op != IR_TEXEL_LOAD || stk_d == 0)) {
                b32 ok = TRUE;
                if (in->a != IR_NONE && !ctx->is_preheader[in->a]) ok = FALSE;
                if (in->b != IR_NONE && !ctx->is_preheader[in->b]) ok = FALSE;
                if (in->c != IR_NONE && !ctx->is_preheader[in->c]) ok = FALSE;
                ctx->is_preheader[i] = ok;
            }
        }
    }

    ctx->max_var_if_depth = max_var_if;
    ctx->need_active_mask = (ctx->max_var_if_depth > 0) || ctx->has_discard_in_if;
    ctx->need_qhdr        = ctx->need_active_mask || ctx->has_frag_x || ctx->has_frag_y ||
                            ctx->has_discard || (ctx->opts->fuse_blend != MTK_FUSE_NONE);
    ctx->need_q16         = ctx->has_any_varying || (ctx->opts->fuse_blend == MTK_FUSE_NONE);

    /* 2. Geriye doğru geçişle yalnızca canlı komutların kullanım sayılarını,
     * son kullanımlarını ve splat ihtiyaçlarını hesapla */
    for (u32 idx = n; idx > 0; idx--) {
        u32 i = idx - 1;
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;
        if (in->type != IR_T_VOID && ctx->use_count[i] == 0) {
            continue;
        }

        b32 consumer_is_var = (in->flags & IR_FLAG_VARYING) != 0;
        if (op == IR_DISCARD || op == IR_OUTPUT) {
            consumer_is_var = TRUE;
        }

        u32 ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < 3; k++) {
            u32 op_id = ops[k];
            if (op_id == IR_NONE) continue;

            /* Varying SELECT'te bir kolu tüm-0 veya tüm-1 sabiti ise o kol v128.and/andnot/or'a
             * dönüşeceğinden sabitin splat edilmesine gerek yoktur */
            if (op == IR_SELECT && consumer_is_var && (k == 1 || k == 2)) {
                const IrInst* ib_in = &f->insts[in->b];
                const IrInst* ic_in = &f->insts[in->c];
                b32 z_c = is_all_zero_const(ic_in);
                b32 z_b = is_all_zero_const(ib_in);
                b32 o_b = is_all_ones_const(ib_in);
                b32 o_c = is_all_ones_const(ic_in);
                if (k == 2 && (z_c || (!z_b && !o_b && o_c))) {
                    continue;
                }
                if (k == 1 && (!z_c && (z_b || o_b))) {
                    continue;
                }
            }

            ctx->use_count[op_id]++;
            ctx->single_user[op_id] = i;
            if (i > ctx->last_use[op_id]) {
                ctx->last_use[op_id] = i;
            }
            if (!ctx->is_preheader[i]) {
                ctx->used_in_loop[op_id] = TRUE;
            }

            b32 want_var = consumer_is_var;
            if (k == 1 && consumer_is_var && (op == IR_ISHL || op == IR_ISHR_S || op == IR_ISHR_U)) {
                if (!(f->insts[op_id].flags & IR_FLAG_VARYING)) {
                    want_var = FALSE;
                }
            }

            if (want_var && !(f->insts[op_id].flags & IR_FLAG_VARYING)) {
                ctx->need_splat[op_id] = TRUE;
                ctx->splat_use_count[op_id]++;
            } else {
                ctx->scalar_use_count[op_id]++;
            }
        }
    }

    /* 3. Varying SELECT(cond, fneg(x), x) ve SELECT(cond, x, fneg(x)) kalıbını tanı:
     * sin/cos polinomlarında üretilen bu kalıp bitselect yerine doğrudan işaret biti XOR'una
     * (v128.xor(x, v128.and(cond, -0.0f))) dönüşür; böylece fneg komutu katlanır ve x'in
     * kullanım sayısı 2'den 1'e düşerek x'in de yığında inline edilmesini sağlar!
     * Ayrıca cond == ((q & 2) != 0) ise (q << 30) & -0.0f ile 2 SIMD komutu ve 2 splat daha elenir. */
    for (u32 i = 0; i < n; i++) {
        const IrInst* in = &f->insts[i];
        if (ctx->use_count[i] == 0) continue;
        if (in->op != IR_SELECT || !(in->flags & IR_FLAG_VARYING)) continue;
        if (in->type != IR_T_F32) continue;

        u32 a = in->a;
        u32 b = in->b;
        u32 c = in->c;
        if (f->insts[b].op == IR_FNEG && f->insts[b].a == c &&
            (f->insts[b].flags & IR_FLAG_VARYING) && (f->insts[c].flags & IR_FLAG_VARYING) &&
            ctx->use_count[b] == 1 && ctx->inst_region[b] == ctx->inst_region[i]) {
            ctx->select_fneg_mode[i] = 1;
            ctx->use_count[b]        = 0;
            ctx->stat_inlined_ops++;
            ctx->need_sign_mask      = TRUE;
            if (ctx->use_count[c] == 2) {
                ctx->use_count[c]    = 1;
                ctx->single_user[c]  = i;
            }
            /* cond = (q & 2) != 0 kalıbını kontrol et */
            const IrInst* cond_in = &f->insts[a];
            if (cond_in->op == IR_INE && ctx->use_count[a] == 1 &&
                f->insts[cond_in->b].op == IR_CONST_I32 && f->insts[cond_in->b].imm.i == 0) {
                u32 and_id = cond_in->a;
                const IrInst* and_in = &f->insts[and_id];
                if (and_in->op == IR_IAND && ctx->use_count[and_id] == 1 &&
                    f->insts[and_in->b].op == IR_CONST_I32 && f->insts[and_in->b].imm.i == 2 &&
                    (f->insts[and_in->a].flags & IR_FLAG_VARYING)) {
                    ctx->select_fneg_mode[i] = 3;
                    ctx->use_count[a]        = 0;
                    ctx->use_count[and_id]   = 0;
                    ctx->stat_inlined_ops   += 2;
                    /* Sabit 0 ve 2'nin splat kullanım sayılarını düşür */
                    u32 c0 = cond_in->b;
                    u32 c2 = and_in->b;
                    if (ctx->use_count[c0] > 0) ctx->use_count[c0]--;
                    if (ctx->splat_use_count[c0] > 0) {
                        ctx->splat_use_count[c0]--;
                        if (ctx->splat_use_count[c0] == 0) ctx->need_splat[c0] = FALSE;
                    }
                    if (ctx->use_count[c2] > 0) ctx->use_count[c2]--;
                    if (ctx->splat_use_count[c2] > 0) {
                        ctx->splat_use_count[c2]--;
                        if (ctx->splat_use_count[c2] == 0) ctx->need_splat[c2] = FALSE;
                    }
                    if (ctx->use_count[and_in->a] == 1) {
                        ctx->single_user[and_in->a] = i;
                    }
                }
            }
        } else if (f->insts[c].op == IR_FNEG && f->insts[c].a == b &&
                   (f->insts[c].flags & IR_FLAG_VARYING) && (f->insts[b].flags & IR_FLAG_VARYING) &&
                   ctx->use_count[c] == 1 && ctx->inst_region[c] == ctx->inst_region[i]) {
            ctx->select_fneg_mode[i] = 2;
            ctx->use_count[c]        = 0;
            ctx->stat_inlined_ops++;
            ctx->need_sign_mask      = TRUE;
            if (ctx->use_count[b] == 2) {
                ctx->use_count[b]    = 1;
                ctx->single_user[b]  = i;
            }
        }
    }

    /* 4. Genelleştirilmiş özyinelemeli ifade ağacı birleştirme (expression-tree inlining):
     * Tek kullanımlı (use_count[u] == 1) her saf değer komutu, tüketicisi ile aynı IF bölgesinde
     * ve aynı aşamada (preheader vs loop) ise local.set/local.get çifti olmadan doğrudan
     * tüketicisinin operandı değerlendirilirken yığında üretilir. */
    for (u32 u = 0; u < n; u++) {
        if (ctx->inlined[u]) continue;
        if (!can_inline_inst(f, u)) continue;
        const IrInst* prod = &f->insts[u];
        /* Sabitler skaler kullanımda doğrudan const olarak basılır, varying kullanımda ise
         * preheader'da splat edilir; bu yüzden sabitler için ayrı local ayırmaya gerek yoktur */
        if (is_const_op((IrOp)prod->op)) continue;

        if (ctx->use_count[u] != 1) continue;
        u32 cons_idx = ctx->single_user[u];
        if (cons_idx == IR_NONE || cons_idx <= u) continue;

        const IrInst* cons = &f->insts[cons_idx];
        if (cons->op == IR_NOP) continue;

        /* Preheader'daki bir komutu asla döngü içindeki bir komuta inline etme! */
        if (ctx->is_preheader[u] != ctx->is_preheader[cons_idx]) continue;

        /* Farklı IF/ELSE bölgeleri arasında inline yapma */
        if (ctx->inst_region[u] != ctx->inst_region[cons_idx]) continue;

        /* Üretici uniform iken tüketici varying splat istiyorsa splat local'i kullan */
        if (ctx->need_splat[u]) continue;

        /* Eğer u ile tüketicisi cons_idx arasında çok-kullanımlı başka döngü komutları
         * (kendi adımında local'e yazılacak ifadeler) varsa ve u'nun alt ağacı >= 2 farklı
         * döngü local'inin son kullanımıysa, u'yu cons_idx'e ertelemek o >= 2 yazmacı aradaki
         * ifadeler boyunca canlı tutar; u'yu kendi adımında tek yazmaca yazıp onları bırak. */
        if (!ctx->is_preheader[u] && cons_idx > u + 1) {
            u32 multi_between = 0;
            for (u32 k = u + 1; k < cons_idx; k++) {
                if (!ctx->is_preheader[k] && ctx->use_count[k] > 1) {
                    multi_between++;
                }
            }
            if (multi_between >= 2) {
                u32 seen[16];
                u32 seen_cnt = 0;
                if (count_dying_loop_locals(ctx, u, u, seen, &seen_cnt) >= 2) {
                    continue;
                }
            }
        }

        ctx->inlined[u] = TRUE;
        ctx->stat_inlined_ops++;
    }

    /* 5. Inline edilen komutlar kendi indekslerinde değil, kök tüketicilerinin indeksinde
     * üretildiğinden, kullandıkları operandların son kullanım adımını (last_use) kök
     * tüketicinin adımına uzat ki LocalPool o local'i erken yeniden kullanmasın! */
    u32 emit_step[CG_MAX_INSTS];
    for (u32 idx = n; idx > 0; idx--) {
        u32 i = idx - 1;
        if (ctx->inlined[i] && ctx->single_user[i] != IR_NONE) {
            emit_step[i] = emit_step[ctx->single_user[i]];
        } else {
            emit_step[i] = i;
        }
        const IrInst* in = &f->insts[i];
        if (in->op == IR_NOP) continue;
        if (in->type != IR_T_VOID && ctx->use_count[i] == 0) continue;
        u32 ops[3] = { in->a, in->b, in->c };
        if (in->op == IR_SELECT && ctx->select_fneg_mode[i] == 3) {
            ops[0] = f->insts[f->insts[in->a].a].a;
            ops[1] = IR_NONE;
        }
        for (u32 k = 0; k < 3; k++) {
            u32 op_id = ops[k];
            if (op_id != IR_NONE && emit_step[i] > ctx->last_use[op_id]) {
                ctx->last_use[op_id] = emit_step[i];
            }
        }
    }
}

typedef struct {
    u32 slot_idx[CG_MAX_POOL];
    u32 free_after[CG_MAX_POOL];
    u8  valtype[CG_MAX_POOL];
    u32 count;
} LocalPool;

static u32 pool_acquire_slot(LocalPool* p, u8 vt, u32 cur_step, u32 last_use_step) {
    for (u32 i = 0; i < p->count; i++) {
        if (p->valtype[i] == vt && p->free_after[i] < cur_step) {
            p->free_after[i] = last_use_step;
            return i;
        }
    }
    ASSERT(p->count < CG_MAX_POOL);
    u32 idx = p->count++;
    p->slot_idx[idx]   = 0;
    p->free_after[idx] = last_use_step;
    p->valtype[idx]    = vt;
    return idx;
}

static void allocate_locals(CgCtx* ctx) {
    MbFunc* mf = ctx->mf;
    const IrFunc* f = ctx->f;

    ctx->loc_batch = 0; /* parametre 0 */

    b32 inside_if[CG_MAX_INSTS];
    u32 outer_if_end[CG_MAX_INSTS];
    u32 if_open_stk[CG_MAX_IF];
    u32 d_cur = 0;

    for (u32 i = 0; i < f->count; i++) {
        outer_if_end[i] = i;
        IrOp op = (IrOp)f->insts[i].op;
        if (op == IR_IF_BEGIN) {
            if_open_stk[d_cur++] = i;
            inside_if[i] = TRUE;
        } else if (op == IR_IF_END) {
            inside_if[i] = TRUE;
            if (d_cur > 0) {
                u32 open_idx = if_open_stk[--d_cur];
                if (d_cur == 0) {
                    for (u32 k = open_idx; k <= i; k++) {
                        outer_if_end[k] = i;
                    }
                }
            }
        } else {
            inside_if[i] = (d_cur > 0);
        }
    }

    LocalPool pool;
    __builtin_memset(&pool, 0, sizeof(pool));

    /* Aynı sabit değerin birden fazla yerde splat edilmesini önlemek için
     * preheader sabitlerinin splat slotlarını deduplicate et */
    u32 const_splat_inst[CG_MAX_INSTS];
    u32 const_splat_count = 0;

    u32 raw_count = 0;
    for (u32 pass = 0; pass < 2; pass++) {
        b32 want_pre = (pass == 0);
        if (pass == 1) {
            for (u32 s = 0; s < pool.count; s++) {
                if (pool.free_after[s] <= f->count) {
                    pool.free_after[s] = 0;
                }
            }
        }
        for (u32 i = 0; i < f->count; i++) {
            if (ctx->is_preheader[i] != want_pre) continue;
            const IrInst* in = &f->insts[i];
            IrOp op = (IrOp)in->op;
            if (op == IR_NOP || in->type == IR_T_VOID) continue;
            raw_count++;
            if (ctx->need_splat[i]) raw_count++;

            if (ctx->inlined[i] || ctx->use_count[i] == 0) {
                continue;
            }

            u8 vt = ir_to_wasm_valtype(in);
            u32 cur_step = i;
            u32 last = ctx->last_use[i];
            if (last < i) last = i;
            if (outer_if_end[last] > last) {
                last = outer_if_end[last];
            }
            /* Preheader'da hesaplanıp döngü içinde kullanılan değerler tüm döngü boyunca canlıdır */
            if (ctx->is_preheader[i] && ctx->used_in_loop[i]) {
                last = f->count + 1;
            }

            /* Sabitler skaler kullanımda doğrudan wasm sabiti olarak basıldığından val_loc gerekmez;
             * yalnızca varying splat gerekiyorsa splat_loc ayrılır (ve aynı sabitler paylaşılır). */
            if (is_const_op(op)) {
                if (ctx->need_splat[i]) {
                    u32 shared_slot = IR_NONE;
                    for (u32 c = 0; c < const_splat_count; c++) {
                        u32 prev_i = const_splat_inst[c];
                        const IrInst* prev = &f->insts[prev_i];
                        if (prev->op == in->op &&
                            ((op == IR_CONST_F32 && cg_f32_bits(prev->imm.f) == cg_f32_bits(in->imm.f)) ||
                             (op != IR_CONST_F32 && prev->imm.i == in->imm.i))) {
                            shared_slot = ctx->splat_loc[prev_i];
                            if (last > pool.free_after[shared_slot]) {
                                pool.free_after[shared_slot] = last;
                            }
                            break;
                        }
                    }
                    if (shared_slot != IR_NONE) {
                        ctx->splat_loc[i] = shared_slot;
                    } else {
                        ctx->splat_loc[i] = pool_acquire_slot(&pool, MB_VAL_V128, cur_step, last);
                        const_splat_inst[const_splat_count++] = i;
                    }
                }
                continue;
            }

            if (!want_pre && inside_if[i]) {
                if (ctx->scalar_use_count[i] > 0 || (in->flags & IR_FLAG_VARYING)) {
                    u32 idx = pool.count++;
                    ASSERT(idx < CG_MAX_POOL);
                    pool.slot_idx[idx]   = 0;
                    pool.free_after[idx] = f->count + 1;
                    pool.valtype[idx]    = vt;
                    ctx->val_loc[i]      = idx;
                }
                if (ctx->need_splat[i]) {
                    u32 sidx = pool.count++;
                    ASSERT(sidx < CG_MAX_POOL);
                    pool.slot_idx[sidx]   = 0;
                    pool.free_after[sidx] = f->count + 1;
                    pool.valtype[sidx]    = MB_VAL_V128;
                    ctx->splat_loc[i]     = sidx;
                }
            } else {
                if (ctx->scalar_use_count[i] > 0 || (in->flags & IR_FLAG_VARYING)) {
                    ctx->val_loc[i] = pool_acquire_slot(&pool, vt, cur_step, last);
                }
                if (ctx->need_splat[i]) {
                    ctx->splat_loc[i] = pool_acquire_slot(&pool, MB_VAL_V128, cur_step, last);
                }
            }
        }
    }

    u32 pool_i32 = 0, pool_f32 = 0, pool_v128 = 0;
    for (u32 s = 0; s < pool.count; s++) {
        if (pool.valtype[s] == MB_VAL_I32)       pool_i32++;
        else if (pool.valtype[s] == MB_VAL_F32)  pool_f32++;
        else if (pool.valtype[s] == MB_VAL_V128) pool_v128++;
    }

    /* Sabit görevli i32 local sayısını belirle */
    u32 fixed_i32 = 11; /* count..tmp_i32_b */
    if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
        fixed_i32 += 5; /* fb_px, fb_stride, fb_stride_bytes, fb_addr0, fb_addr1 */
    } else {
        fixed_i32 += 4; /* loc_out_base[0..3] */
    }
    for (u32 v = 0; v < SHADE_MAX_VARYINGS; v++) {
        if (ctx->used_varying[v]) fixed_i32++;
    }
    for (u32 t = 0; t < SHADE_MAX_TEXTURES; t++) {
        if (ctx->used_tex_load_u[t]) fixed_i32++;
    }

    u32 base_i32 = mb_local(mf, MB_VAL_I32, fixed_i32 + pool_i32);
    u32 cur_i32  = base_i32;
    ctx->loc_count     = cur_i32++;
    ctx->loc_quads     = cur_i32++;
    ctx->loc_varyings  = cur_i32++;
    ctx->loc_uniforms  = cur_i32++;
    ctx->loc_out_rgba  = cur_i32++;
    ctx->loc_textures  = cur_i32++;
    ctx->loc_q         = cur_i32++;
    ctx->loc_q16       = cur_i32++;
    ctx->loc_qhdr      = cur_i32++;
    ctx->loc_tmp_i32   = cur_i32++;
    ctx->loc_tmp_i32_b = cur_i32++;

    if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
        ctx->loc_fb_px           = cur_i32++;
        ctx->loc_fb_stride       = cur_i32++;
        ctx->loc_fb_stride_bytes = cur_i32++;
        ctx->loc_fb_addr0        = cur_i32++;
        ctx->loc_fb_addr1        = cur_i32++;
    } else {
        for (u32 c = 0; c < 4; c++) {
            ctx->loc_out_base[c] = cur_i32++;
        }
    }
    for (u32 v = 0; v < SHADE_MAX_VARYINGS; v++) {
        ctx->loc_var_base[v] = ctx->used_varying[v] ? cur_i32++ : IR_NONE;
    }
    for (u32 t = 0; t < SHADE_MAX_TEXTURES; t++) {
        ctx->loc_tex_px[t] = ctx->used_tex_load_u[t] ? cur_i32++ : IR_NONE;
    }
    u32 next_i32 = cur_i32;

    /* 2. Grup: tüm f32 local'ler */
    u32 next_f32 = mb_local(mf, MB_VAL_F32, pool_f32);

    /* 3. Grup: tüm v128 local'ler */
    u32 fixed_v128 = 4 + ctx->max_var_if_depth * 2; /* active, blend_mask, tmp_v0, tmp_v1 + IF yığını */
    if (ctx->need_active_mask || ctx->opts->fuse_blend != MTK_FUSE_NONE) fixed_v128++; /* v_mask_bits */
    if (ctx->need_sign_mask) fixed_v128++; /* v_sign_mask */
    if (ctx->has_frag_x) fixed_v128++;
    if (ctx->has_frag_y) fixed_v128++;
    for (u32 t = 0; t < SHADE_MAX_TEXTURES; t++) {
        if (ctx->used_tex_load_v[t]) fixed_v128++;
    }
    if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
        fixed_v128 += 9; /* zero, one, 255, half, fb_dst, out_ch[0..3] */
        if (ctx->opts->fuse_blend == MTK_FUSE_ALPHA || ctx->opts->fuse_blend == MTK_FUSE_ADD) {
            fixed_v128 += 2; /* inv255, ff */
        }
    }

    u32 base_v128 = mb_local(mf, MB_VAL_V128, fixed_v128 + pool_v128);
    u32 cur_v128  = base_v128;
    ctx->loc_active     = cur_v128++;
    ctx->loc_blend_mask = cur_v128++;
    ctx->loc_tmp_v0     = cur_v128++;
    ctx->loc_tmp_v1     = cur_v128++;
    for (u32 d = 0; d < ctx->max_var_if_depth; d++) {
        ctx->if_saved_locs[d] = cur_v128++;
        ctx->if_cond_locs[d]  = cur_v128++;
    }
    if (ctx->need_active_mask || ctx->opts->fuse_blend != MTK_FUSE_NONE) {
        ctx->loc_v_mask_bits = cur_v128++;
    }
    if (ctx->need_sign_mask) {
        ctx->loc_v_sign_mask = cur_v128++;
    }
    if (ctx->has_frag_x) {
        ctx->loc_v_frag_dx = cur_v128++;
    }
    if (ctx->has_frag_y) {
        ctx->loc_v_frag_dy = cur_v128++;
    }
    for (u32 t = 0; t < SHADE_MAX_TEXTURES; t++) {
        ctx->loc_tex_px_v[t] = ctx->used_tex_load_v[t] ? cur_v128++ : IR_NONE;
    }
    if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
        ctx->loc_v_zero = cur_v128++;
        ctx->loc_v_one  = cur_v128++;
        ctx->loc_v_255  = cur_v128++;
        ctx->loc_v_half = cur_v128++;
        ctx->loc_fb_dst = cur_v128++;
        for (u32 c = 0; c < 4; c++) {
            ctx->loc_out_ch[c] = cur_v128++;
        }
        if (ctx->opts->fuse_blend == MTK_FUSE_ALPHA || ctx->opts->fuse_blend == MTK_FUSE_ADD) {
            ctx->loc_v_inv255 = cur_v128++;
            ctx->loc_v_ff     = cur_v128++;
        }
    }
    u32 next_v128 = cur_v128;

    /* Havuz slotlarına gerçek wasm local indekslerini ata */
    for (u32 s = 0; s < pool.count; s++) {
        if (pool.valtype[s] == MB_VAL_I32)       pool.slot_idx[s] = next_i32++;
        else if (pool.valtype[s] == MB_VAL_F32)  pool.slot_idx[s] = next_f32++;
        else if (pool.valtype[s] == MB_VAL_V128) pool.slot_idx[s] = next_v128++;
    }

    for (u32 i = 0; i < f->count; i++) {
        if (ctx->val_loc[i] != IR_NONE) {
            ctx->val_loc[i] = pool.slot_idx[ctx->val_loc[i]];
        }
        if (ctx->splat_loc[i] != IR_NONE) {
            ctx->splat_loc[i] = pool.slot_idx[ctx->splat_loc[i]];
        }
    }

    ctx->stat_raw_locals    = raw_count;
    ctx->stat_pooled_locals = pool.count;
}

static void emit_inst_expr(CgCtx* ctx, u32 idx);

/* Bir operandın değerini yığına iter.
 * Eğer operand tek kullanımlı ve inline edilmişse ifadesini özyinelemeli olarak doğrudan yığında üretir;
 * sabitse ve skaler isteniyorsa doğrudan const basar; aksi halde ilgili local'den okur. */
static void emit_push_operand(CgCtx* ctx, u32 op_id, b32 want_varying) {
    if (ctx->inlined[op_id]) {
        emit_inst_expr(ctx, op_id);
        return;
    }

    const IrInst* def = &ctx->f->insts[op_id];
    IrOp dop = (IrOp)def->op;
    b32 def_is_var = (def->flags & IR_FLAG_VARYING) != 0;

    if (!want_varying && is_const_op(dop)) {
        if (dop == IR_CONST_F32)      mb_f32_const(ctx->mf, def->imm.f);
        else if (dop == IR_CONST_I32) mb_i32_const(ctx->mf, def->imm.i);
        else                          mb_i32_const(ctx->mf, def->imm.i ? 1 : 0);
        return;
    }

    if (want_varying && !def_is_var) {
        ASSERT(ctx->splat_loc[op_id] != IR_NONE);
        mb_local_get(ctx->mf, ctx->splat_loc[op_id]);
    } else {
        ASSERT(ctx->val_loc[op_id] != IR_NONE);
        mb_local_get(ctx->mf, ctx->val_loc[op_id]);
    }
}

/* Değer üreten bir IR komutunun ifadesini değerlendirip sonucunu yığının tepesinde bırakır */
static void emit_inst_expr(CgCtx* ctx, u32 idx) {
    MbFunc* mf = ctx->mf;
    const IrFunc* f = ctx->f;
    const IrInst* in = &f->insts[idx];
    IrOp op = (IrOp)in->op;
    b32 is_var = (in->flags & IR_FLAG_VARYING) != 0;

    switch (op) {
    case IR_CONST_F32:
        mb_f32_const(mf, in->imm.f);
        break;
    case IR_CONST_I32:
        mb_i32_const(mf, in->imm.i);
        break;
    case IR_CONST_BOOL:
        mb_i32_const(mf, in->imm.i ? 1 : 0);
        break;
    case IR_UNIFORM_F32:
        mb_local_get(mf, ctx->loc_uniforms);
        mb_f32_load(mf, 2, in->imm.u);
        break;
    case IR_UNIFORM_I32:
        mb_local_get(mf, ctx->loc_uniforms);
        mb_i32_load(mf, 2, in->imm.u);
        break;
    case IR_VARYING: {
        u32 v_idx = in->imm.u;
        mb_local_get(mf, ctx->loc_var_base[v_idx]);
        mb_local_get(mf, ctx->loc_q16);
        mb_i32_add(mf);
        mb_v128_load(mf, 2, 0);
    } break;
    case IR_FRAG_X:
        mb_local_get(mf, ctx->loc_qhdr);
        mb_i32_load16_u(mf, 1, QUAD_OFF_X);
        mb_f32_convert_i32_u(mf);
        mb_f32x4_splat(mf);
        mb_local_get(mf, ctx->loc_v_frag_dx);
        mb_f32x4_add(mf);
        break;
    case IR_FRAG_Y:
        mb_local_get(mf, ctx->loc_qhdr);
        mb_i32_load16_u(mf, 1, QUAD_OFF_Y);
        mb_f32_convert_i32_u(mf);
        mb_f32x4_splat(mf);
        mb_local_get(mf, ctx->loc_v_frag_dy);
        mb_f32x4_add(mf);
        break;

    /* Tekli (unary) işlemler */
    case IR_FNEG:
    case IR_FABS:
    case IR_FSQRT:
    case IR_FFLOOR:
    case IR_FCEIL:
    case IR_FTRUNC:
    case IR_FNEAREST:
    case IR_BNOT:
    case IR_F2I:
    case IR_I2F:
    case IR_BITCAST_F2I:
    case IR_BITCAST_I2F:
        emit_push_operand(ctx, in->a, is_var);
        if (is_var) {
            switch (op) {
            case IR_FNEG:        mb_f32x4_neg(mf); break;
            case IR_FABS:        mb_f32x4_abs(mf); break;
            case IR_FSQRT:       mb_f32x4_sqrt(mf); break;
            case IR_FFLOOR:      mb_f32x4_floor(mf); break;
            case IR_FCEIL:       mb_f32x4_ceil(mf); break;
            case IR_FTRUNC:      mb_f32x4_trunc(mf); break;
            case IR_FNEAREST:    mb_f32x4_nearest(mf); break;
            case IR_BNOT:        mb_v128_not(mf); break;
            case IR_F2I:         mb_i32x4_trunc_sat_f32x4_s(mf); break;
            case IR_I2F:         mb_f32x4_convert_i32x4_s(mf); break;
            case IR_BITCAST_F2I: break;
            case IR_BITCAST_I2F: break;
            default: break;
            }
        } else {
            switch (op) {
            case IR_FNEG:        mb_f32_neg(mf); break;
            case IR_FABS:        mb_f32_abs(mf); break;
            case IR_FSQRT:       mb_f32_sqrt(mf); break;
            case IR_FFLOOR:      mb_f32_floor(mf); break;
            case IR_FCEIL:       mb_f32_ceil(mf); break;
            case IR_FTRUNC:      mb_f32_trunc(mf); break;
            case IR_FNEAREST:    mb_f32_nearest(mf); break;
            case IR_BNOT:        mb_i32_eqz(mf); break;
            case IR_F2I:         mb_i32_trunc_sat_f32_s(mf); break;
            case IR_I2F:         mb_f32_convert_i32_s(mf); break;
            case IR_BITCAST_F2I: mb_i32_reinterpret_f32(mf); break;
            case IR_BITCAST_I2F: mb_f32_reinterpret_i32(mf); break;
            default: break;
            }
        }
        break;

    /* İkili (binary) işlemler */
    case IR_FADD:
    case IR_FSUB:
    case IR_FMUL:
    case IR_FDIV:
    case IR_FMIN:
    case IR_FMAX:
    case IR_IADD:
    case IR_ISUB:
    case IR_IMUL:
    case IR_IAND:
    case IR_IOR:
    case IR_IXOR:
    case IR_ISHL:
    case IR_ISHR_S:
    case IR_ISHR_U:
    case IR_IMIN_S:
    case IR_IMAX_S:
    case IR_FEQ:
    case IR_FNE:
    case IR_FLT:
    case IR_FLE:
    case IR_FGT:
    case IR_FGE:
    case IR_IEQ:
    case IR_INE:
    case IR_ILT_S:
    case IR_ILE_S:
    case IR_IGT_S:
    case IR_IGE_S:
    case IR_BAND:
    case IR_BOR:
        if (is_var && (op == IR_ISHL || op == IR_ISHR_S || op == IR_ISHR_U)) {
            if (!(f->insts[in->b].flags & IR_FLAG_VARYING)) {
                emit_push_operand(ctx, in->a, TRUE);
                emit_push_operand(ctx, in->b, FALSE);
                if (op == IR_ISHL)        mb_i32x4_shl(mf);
                else if (op == IR_ISHR_S) mb_i32x4_shr_s(mf);
                else                      mb_i32x4_shr_u(mf);
            } else {
                /* Alt ağaçlar loc_tmp_v0/v1 kullanabileceğinden önce her iki operandı yığına it */
                emit_push_operand(ctx, in->a, TRUE);
                emit_push_operand(ctx, in->b, TRUE);
                mb_local_set(mf, ctx->loc_tmp_v1);
                mb_local_set(mf, ctx->loc_tmp_v0);

                mb_i32x4_const(mf, 0, 0, 0, 0);
                for (u8 lane = 0; lane < 4; lane++) {
                    mb_local_get(mf, ctx->loc_tmp_v0);
                    mb_i32x4_extract_lane(mf, lane);
                    mb_local_get(mf, ctx->loc_tmp_v1);
                    mb_i32x4_extract_lane(mf, lane);
                    if (op == IR_ISHL)        mb_i32_shl(mf);
                    else if (op == IR_ISHR_S) mb_i32_shr_s(mf);
                    else                      mb_i32_shr_u(mf);
                    mb_i32x4_replace_lane(mf, lane);
                }
            }
            break;
        }

        if (!is_var && (op == IR_IMIN_S || op == IR_IMAX_S)) {
            emit_push_operand(ctx, in->a, FALSE);
            emit_push_operand(ctx, in->b, FALSE);
            mb_local_set(mf, ctx->loc_tmp_i32_b);
            mb_local_tee(mf, ctx->loc_tmp_i32);
            mb_local_get(mf, ctx->loc_tmp_i32_b);
            mb_local_get(mf, ctx->loc_tmp_i32);
            mb_local_get(mf, ctx->loc_tmp_i32_b);
            if (op == IR_IMIN_S) mb_i32_lt_s(mf);
            else                 mb_i32_gt_s(mf);
            mb_select(mf);
            break;
        }

        /* Değişme özelliğine sahip (commutative) işlemlerde sağ operand inline edilmiş bir alt ağaç
         * ve sol operand yaprak (local/splat) ise önce sağ alt ağacı değerlendir ki Horner zincirleri
         * yığında derinleşmeden yalnız 2 yazmaçla düz (flat) olarak hesaplansın! */
        if ((op == IR_FADD || op == IR_FMUL || op == IR_IADD || op == IR_IMUL ||
             op == IR_IAND || op == IR_IOR  || op == IR_IXOR || op == IR_IMIN_S ||
             op == IR_IMAX_S || op == IR_FEQ || op == IR_FNE || op == IR_IEQ ||
             op == IR_INE  || op == IR_BAND || op == IR_BOR) &&
            ctx->inlined[in->b] && !ctx->inlined[in->a]) {
            emit_push_operand(ctx, in->b, is_var);
            emit_push_operand(ctx, in->a, is_var);
        } else {
            emit_push_operand(ctx, in->a, is_var);
            emit_push_operand(ctx, in->b, is_var);
        }

        if (is_var) {
            switch (op) {
            case IR_FADD:   mb_f32x4_add(mf); break;
            case IR_FSUB:   mb_f32x4_sub(mf); break;
            case IR_FMUL:   mb_f32x4_mul(mf); break;
            case IR_FDIV:   mb_f32x4_div(mf); break;
            case IR_FMIN:   mb_f32x4_min(mf); break;
            case IR_FMAX:   mb_f32x4_max(mf); break;
            case IR_IADD:   mb_i32x4_add(mf); break;
            case IR_ISUB:   mb_i32x4_sub(mf); break;
            case IR_IMUL:   mb_i32x4_mul(mf); break;
            case IR_IAND:   mb_v128_and(mf); break;
            case IR_IOR:    mb_v128_or(mf); break;
            case IR_IXOR:   mb_v128_xor(mf); break;
            case IR_IMIN_S: mb_i32x4_min_s(mf); break;
            case IR_IMAX_S: mb_i32x4_max_s(mf); break;
            case IR_FEQ:    mb_f32x4_eq(mf); break;
            case IR_FNE:    mb_f32x4_ne(mf); break;
            case IR_FLT:    mb_f32x4_lt(mf); break;
            case IR_FLE:    mb_f32x4_le(mf); break;
            case IR_FGT:    mb_f32x4_gt(mf); break;
            case IR_FGE:    mb_f32x4_ge(mf); break;
            case IR_IEQ:    mb_i32x4_eq(mf); break;
            case IR_INE:    mb_i32x4_ne(mf); break;
            case IR_ILT_S:  mb_i32x4_lt_s(mf); break;
            case IR_ILE_S:  mb_i32x4_le_s(mf); break;
            case IR_IGT_S:  mb_i32x4_gt_s(mf); break;
            case IR_IGE_S:  mb_i32x4_ge_s(mf); break;
            case IR_BAND:   mb_v128_and(mf); break;
            case IR_BOR:    mb_v128_or(mf); break;
            default: break;
            }
        } else {
            switch (op) {
            case IR_FADD:   mb_f32_add(mf); break;
            case IR_FSUB:   mb_f32_sub(mf); break;
            case IR_FMUL:   mb_f32_mul(mf); break;
            case IR_FDIV:   mb_f32_div(mf); break;
            case IR_FMIN:   mb_f32_min(mf); break;
            case IR_FMAX:   mb_f32_max(mf); break;
            case IR_IADD:   mb_i32_add(mf); break;
            case IR_ISUB:   mb_i32_sub(mf); break;
            case IR_IMUL:   mb_i32_mul(mf); break;
            case IR_IAND:   mb_i32_and(mf); break;
            case IR_IOR:    mb_i32_or(mf); break;
            case IR_IXOR:   mb_i32_xor(mf); break;
            case IR_ISHL:   mb_i32_shl(mf); break;
            case IR_ISHR_S: mb_i32_shr_s(mf); break;
            case IR_ISHR_U: mb_i32_shr_u(mf); break;
            case IR_FEQ:    mb_f32_eq(mf); break;
            case IR_FNE:    mb_f32_ne(mf); break;
            case IR_FLT:    mb_f32_lt(mf); break;
            case IR_FLE:    mb_f32_le(mf); break;
            case IR_FGT:    mb_f32_gt(mf); break;
            case IR_FGE:    mb_f32_ge(mf); break;
            case IR_IEQ:    mb_i32_eq(mf); break;
            case IR_INE:    mb_i32_ne(mf); break;
            case IR_ILT_S:  mb_i32_lt_s(mf); break;
            case IR_ILE_S:  mb_i32_le_s(mf); break;
            case IR_IGT_S:  mb_i32_gt_s(mf); break;
            case IR_IGE_S:  mb_i32_ge_s(mf); break;
            case IR_BAND:   mb_i32_and(mf); break;
            case IR_BOR:    mb_i32_or(mf); break;
            default: break;
            }
        }
        break;

    case IR_SELECT:
        if (is_var) {
            if (ctx->select_fneg_mode[idx] == 3) {
                /* select((q & 2) != 0, -x, x) -> v128.xor(x, v128.and(q << 30, -0.0f)) */
                u32 q_id = f->insts[f->insts[in->a].a].a;
                emit_push_operand(ctx, in->c, TRUE);
                emit_push_operand(ctx, q_id, TRUE);
                mb_i32_const(mf, 30);
                mb_i32x4_shl(mf);
                mb_local_get(mf, ctx->loc_v_sign_mask);
                mb_v128_and(mf);
                mb_v128_xor(mf);
                break;
            }
            if (ctx->select_fneg_mode[idx] == 1) {
                /* select(cond, -x, x) -> v128.xor(x, v128.and(cond, -0.0f)) */
                emit_push_operand(ctx, in->c, TRUE);
                emit_push_operand(ctx, in->a, TRUE);
                mb_local_get(mf, ctx->loc_v_sign_mask);
                mb_v128_and(mf);
                mb_v128_xor(mf);
                break;
            }
            if (ctx->select_fneg_mode[idx] == 2) {
                /* select(cond, x, -x) -> v128.xor(x, v128.andnot(-0.0f, cond)) */
                emit_push_operand(ctx, in->b, TRUE);
                mb_local_get(mf, ctx->loc_v_sign_mask);
                emit_push_operand(ctx, in->a, TRUE);
                mb_v128_andnot(mf);
                mb_v128_xor(mf);
                break;
            }
            /* Bir kolu tüm-0 veya tüm-1 olan varying select'leri bitselect yerine and/andnot/or'a düşür */
            if (is_all_zero_const(&f->insts[in->c])) {
                emit_push_operand(ctx, in->b, TRUE);
                emit_push_operand(ctx, in->a, TRUE);
                mb_v128_and(mf);
                break;
            }
            if (is_all_zero_const(&f->insts[in->b])) {
                emit_push_operand(ctx, in->c, TRUE);
                emit_push_operand(ctx, in->a, TRUE);
                mb_v128_andnot(mf);
                break;
            }
            if (is_all_ones_const(&f->insts[in->b])) {
                emit_push_operand(ctx, in->c, TRUE);
                emit_push_operand(ctx, in->a, TRUE);
                mb_v128_or(mf);
                break;
            }
            if (is_all_ones_const(&f->insts[in->c])) {
                emit_push_operand(ctx, in->a, TRUE);
                emit_push_operand(ctx, in->b, TRUE);
                mb_v128_andnot(mf);
                mb_v128_not(mf);
                break;
            }
            emit_push_operand(ctx, in->b, TRUE);
            emit_push_operand(ctx, in->c, TRUE);
            emit_push_operand(ctx, in->a, TRUE);
            mb_v128_bitselect(mf);
        } else {
            emit_push_operand(ctx, in->b, FALSE);
            emit_push_operand(ctx, in->c, FALSE);
            emit_push_operand(ctx, in->a, FALSE);
            mb_select(mf);
        }
        break;

    case IR_TEX_W:
        mb_local_get(mf, ctx->loc_textures);
        mb_i32_load(mf, 2, in->imm.u * TEX_SIZE + TEX_OFF_W);
        break;
    case IR_TEX_H:
        mb_local_get(mf, ctx->loc_textures);
        mb_i32_load(mf, 2, in->imm.u * TEX_SIZE + TEX_OFF_H);
        break;
    case IR_TEX_FLAGS:
        mb_local_get(mf, ctx->loc_textures);
        mb_i32_load(mf, 2, in->imm.u * TEX_SIZE + TEX_OFF_FLAGS);
        break;
    case IR_TEX_LEVELS:
        mb_local_get(mf, ctx->loc_textures);
        mb_i32_load(mf, 2, in->imm.u * TEX_SIZE + TEX_OFF_LEVELS);
        break;

    case IR_DERIV_X:
    case IR_DERIV_Y: {
        if (!is_var) {
            mb_f32_const(mf, 0.0f);   /* uniform'un türevi (ir_eval ile aynı) */
            break;
        }
        /* lane yayma: shuffle(v, v, [L L L L]) - shuffle(v, v, [0 0 0 0]) */
        u8 src = (op == IR_DERIV_X) ? 1u : 2u;
        u8 lanes_hi[16], lanes_lo[16];
        for (u8 k = 0; k < 16; k++) {
            lanes_hi[k] = (u8)(src * 4u + (k & 3u));
            lanes_lo[k] = (u8)(k & 3u);
        }
        emit_push_operand(ctx, in->a, TRUE);
        mb_local_tee(mf, ctx->loc_tmp_v0);
        mb_local_get(mf, ctx->loc_tmp_v0);
        mb_i8x16_shuffle(mf, lanes_hi);
        mb_local_get(mf, ctx->loc_tmp_v0);
        mb_local_get(mf, ctx->loc_tmp_v0);
        mb_i8x16_shuffle(mf, lanes_lo);
        mb_f32x4_sub(mf);
    } break;

    case IR_TEXEL_LOAD: {
        u32 slot = in->imm.u;
        if (!is_var) {
            mb_local_get(mf, ctx->loc_tex_px[slot]);
            emit_push_operand(ctx, in->a, FALSE);
            mb_i32_const(mf, 2);
            mb_i32_shl(mf);
            mb_i32_add(mf);
            mb_i32_load(mf, 2, 0);
        } else {
            /* 4 lane gather ortak adres hesabı:
             * addr_vec = loc_tex_px_v[slot] + (idx << 2) işlemini tek seferde SIMD'de yap,
             * lane 0'ı v128.load32_zero ile yükle ve lane 1..3'ü i32.load + replace_lane ile birleştir. */
            emit_push_operand(ctx, in->a, TRUE);
            mb_i32_const(mf, 2);
            mb_i32x4_shl(mf);
            mb_local_get(mf, ctx->loc_tex_px_v[slot]);
            mb_i32x4_add(mf);
            mb_local_tee(mf, ctx->loc_tmp_v0);

            mb_i32x4_extract_lane(mf, 0);
            mb_v128_load32_zero(mf, 2, 0);

            for (u8 lane = 1; lane < 4; lane++) {
                mb_local_get(mf, ctx->loc_tmp_v0);
                mb_i32x4_extract_lane(mf, lane);
                mb_i32_load(mf, 2, 0);
                mb_i32x4_replace_lane(mf, lane);
            }
        }
    } break;

    default:
        PANIC("emit_inst_expr: unhandled op %u", (u32)op);
    }
}

static void emit_store_result(CgCtx* ctx, u32 idx) {
    MbFunc* mf = ctx->mf;
    const IrInst* in = &ctx->f->insts[idx];

    if (ctx->inlined[idx]) {
        return;
    }

    if (ctx->use_count[idx] == 0) {
        mb_drop(mf);
        return;
    }

    if (ctx->need_splat[idx]) {
        if (ctx->val_loc[idx] != IR_NONE) {
            mb_local_tee(mf, ctx->val_loc[idx]);
        }
        if (in->type == IR_T_F32) {
            mb_f32x4_splat(mf);
        } else if (in->type == IR_T_BOOL) {
            mb_i32_const(mf, -1);
            mb_i32_mul(mf);
            mb_i32x4_splat(mf);
        } else {
            mb_i32x4_splat(mf);
        }
        mb_local_set(mf, ctx->splat_loc[idx]);
    } else if (ctx->val_loc[idx] != IR_NONE) {
        mb_local_set(mf, ctx->val_loc[idx]);
    } else {
        mb_drop(mf);
    }
}

/* Yığındaki [0.0f, 1.0f] aralığındaki f32x4 değerini [0..255] aralığında i32x4'e dönüştürür */
static void emit_f32x4_to_u8_i32x4(CgCtx* ctx) {
    MbFunc* mf = ctx->mf;
    mb_local_get(mf, ctx->loc_v_255);
    mb_f32x4_mul(mf);
    mb_local_get(mf, ctx->loc_v_half);
    mb_f32x4_add(mf);
    mb_i32x4_trunc_sat_f32x4_s(mf);
}

static void emit_inst(CgCtx* ctx, u32 idx) {
    MbFunc* mf = ctx->mf;
    const IrFunc* f = ctx->f;
    const IrInst* in = &f->insts[idx];
    IrOp op = (IrOp)in->op;
    b32 is_var = (in->flags & IR_FLAG_VARYING) != 0;

    if (op == IR_NOP) return;
    if (ctx->inlined[idx]) return;

    if (op == IR_IF_BEGIN) {
        ASSERT(ctx->if_depth < CG_MAX_IF);
        u32 d = ctx->if_depth++;
        ctx->if_stk[d].is_varying = is_var;
        ctx->if_stk[d].has_else   = FALSE;

        if (!is_var) {
            emit_push_operand(ctx, in->a, FALSE);
            mb_if(mf, MB_BLOCK_VOID);
        } else {
            u32 vd = 0;
            for (u32 k = 0; k < d; k++) {
                if (ctx->if_stk[k].is_varying) vd++;
            }
            u32 saved_loc = ctx->if_saved_locs[vd];
            u32 cond_loc  = ctx->if_cond_locs[vd];
            ctx->if_stk[d].saved_mask_loc = saved_loc;
            ctx->if_stk[d].cond_mask_loc  = cond_loc;

            mb_local_get(mf, ctx->loc_active);
            mb_local_set(mf, saved_loc);

            emit_push_operand(ctx, in->a, TRUE);
            mb_local_tee(mf, cond_loc);

            mb_local_get(mf, saved_loc);
            mb_v128_and(mf);
            mb_local_tee(mf, ctx->loc_active);

            mb_v128_any_true(mf);
            mb_if(mf, MB_BLOCK_VOID);
        }
        return;
    }

    if (op == IR_ELSE) {
        ASSERT(ctx->if_depth > 0);
        u32 d = ctx->if_depth - 1;
        ctx->if_stk[d].has_else = TRUE;

        if (!ctx->if_stk[d].is_varying) {
            mb_else(mf);
        } else {
            mb_end(mf);

            u32 saved_loc = ctx->if_stk[d].saved_mask_loc;
            u32 cond_loc  = ctx->if_stk[d].cond_mask_loc;

            mb_local_get(mf, saved_loc);
            mb_local_get(mf, cond_loc);
            mb_v128_andnot(mf);
            mb_local_tee(mf, ctx->loc_active);

            mb_v128_any_true(mf);
            mb_if(mf, MB_BLOCK_VOID);
        }
        return;
    }

    if (op == IR_IF_END) {
        ASSERT(ctx->if_depth > 0);
        u32 d = --ctx->if_depth;
        mb_end(mf);
        if (ctx->if_stk[d].is_varying) {
            mb_local_get(mf, ctx->if_stk[d].saved_mask_loc);
            mb_local_set(mf, ctx->loc_active);
        }
        return;
    }

    if (op == IR_DISCARD) {
        if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
            /* Blend kaynaştırma aktifken loc_blend_mask'i güncelle ve qhdr->mask'e yaz */
            mb_local_get(mf, ctx->loc_qhdr);
            mb_local_get(mf, ctx->loc_blend_mask);
            emit_push_operand(ctx, in->a, TRUE);
            if (ctx->if_depth > 0) {
                mb_local_get(mf, ctx->loc_active);
                mb_v128_and(mf);
            }
            mb_v128_andnot(mf);
            mb_local_tee(mf, ctx->loc_blend_mask);
            mb_i32x4_bitmask(mf);
            mb_i32_store(mf, 2, QUAD_OFF_MASK);
        } else {
            mb_local_get(mf, ctx->loc_qhdr);
            mb_local_get(mf, ctx->loc_qhdr);
            mb_i32_load(mf, 2, QUAD_OFF_MASK);

            emit_push_operand(ctx, in->a, TRUE);
            if (ctx->if_depth > 0) {
                mb_local_get(mf, ctx->loc_active);
                mb_v128_and(mf);
            }
            mb_i32x4_bitmask(mf);
            mb_i32_const(mf, -1);
            mb_i32_xor(mf);
            mb_i32_and(mf);

            mb_i32_store(mf, 2, QUAD_OFF_MASK);
        }
        return;
    }

    if (op == IR_OUTPUT) {
        u32 ch = in->imm.u;
        if (ctx->opts->fuse_blend != MTK_FUSE_NONE) {
            /* pmin(1.0f, pmax(0.0f, x)): NaN ve <=0 değerleri +0.0f'a, >1 değerleri 1.0f'a tek komutla sıkıştırır */
            mb_local_get(mf, ctx->loc_v_one);
            mb_local_get(mf, ctx->loc_v_zero);
            emit_push_operand(ctx, in->a, TRUE);
            mb_f32x4_pmax(mf);
            mb_f32x4_pmin(mf);
            if (ctx->opts->fuse_blend == MTK_FUSE_REPLACE) {
                emit_f32x4_to_u8_i32x4(ctx);
            }
            mb_local_set(mf, ctx->loc_out_ch[ch]);
        } else {
            mb_local_get(mf, ctx->loc_out_base[ch]);
            mb_local_get(mf, ctx->loc_q16);
            mb_i32_add(mf);
            emit_push_operand(ctx, in->a, TRUE);
            mb_v128_store(mf, 2, 0);
        }
        return;
    }

    if (ctx->use_count[idx] == 0) {
        return;
    }

    emit_inst_expr(ctx, idx);
    emit_store_result(ctx, idx);
}

/* Preheader'da sabit splat'leri ve saf uniform hesapları tek seferde üretir */
static void emit_preheader_insts(CgCtx* ctx) {
    MbFunc* mf = ctx->mf;
    const IrFunc* f = ctx->f;

    /* Paylaşılan sabit splat slotlarının yalnızca bir kez doldurulması için takip et */
    u32 filled_splat_locs[CG_MAX_POOL];
    u32 filled_count = 0;

    for (u32 i = 0; i < f->count; i++) {
        if (!ctx->is_preheader[i]) continue;
        if (ctx->inlined[i] || ctx->use_count[i] == 0) continue;

        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;

        if (is_const_op(op)) {
            if (!ctx->need_splat[i]) continue;
            u32 sloc = ctx->splat_loc[i];
            b32 already = FALSE;
            for (u32 k = 0; k < filled_count; k++) {
                if (filled_splat_locs[k] == sloc) {
                    already = TRUE;
                    break;
                }
            }
            if (already) continue;
            filled_splat_locs[filled_count++] = sloc;

            if (op == IR_CONST_F32) {
                mb_f32x4_const(mf, in->imm.f, in->imm.f, in->imm.f, in->imm.f);
            } else if (op == IR_CONST_I32) {
                mb_i32x4_const(mf, in->imm.i, in->imm.i, in->imm.i, in->imm.i);
            } else {
                i32 m = in->imm.i ? -1 : 0;
                mb_i32x4_const(mf, m, m, m, m);
            }
            mb_local_set(mf, sloc);
            continue;
        }

        emit_inst_expr(ctx, i);
        emit_store_result(ctx, i);
    }
}

/* Blend kaynaştırma (Blend Fusion) son aşaması:
 * loc_out_ch[0..3] içindeki 4 kanalı batch->fb_px'ten yüklenen 4 hedef pikselle
 * REPLACE / ALPHA / ADD modunda birleştirir, loc_blend_mask ile bitselect uygular ve geri yazar. */
static void emit_fused_blend_epilogue(CgCtx* ctx) {
    MbFunc* mf = ctx->mf;
    u32 mode = ctx->opts->fuse_blend;

    /* addr0 = fb_px + ((qy * fb_stride + qx) << 2)
     * addr1 = addr0 + fb_stride_bytes */
    mb_local_get(mf, ctx->loc_fb_px);
    mb_local_get(mf, ctx->loc_qhdr);
    mb_i32_load16_u(mf, 1, QUAD_OFF_Y);
    mb_local_get(mf, ctx->loc_fb_stride);
    mb_i32_mul(mf);
    mb_local_get(mf, ctx->loc_qhdr);
    mb_i32_load16_u(mf, 1, QUAD_OFF_X);
    mb_i32_add(mf);
    mb_i32_const(mf, 2);
    mb_i32_shl(mf);
    mb_i32_add(mf);
    mb_local_tee(mf, ctx->loc_fb_addr0);
    mb_local_get(mf, ctx->loc_fb_stride_bytes);
    mb_i32_add(mf);
    mb_local_tee(mf, ctx->loc_fb_addr1);

    /* Satır 0 (2 piksel = 64 bit) ve satır 1 (2 piksel = 64 bit) -> loc_fb_dst */
    mb_local_get(mf, ctx->loc_fb_addr0);
    mb_v128_load64_zero(mf, 3, 0);
    mb_v128_load64_lane(mf, 3, 0, 1);
    mb_local_set(mf, ctx->loc_fb_dst);

    if (mode == MTK_FUSE_ALPHA) {
        /* tmp_v1 = inv_sa = 1.0f - sa */
        mb_local_get(mf, ctx->loc_v_one);
        mb_local_get(mf, ctx->loc_out_ch[3]);
        mb_f32x4_sub(mf);
        mb_local_set(mf, ctx->loc_tmp_v1);

        /* Kanal 0..2 (R, G, B): out_ch[c] = u8(pmin(1.0f, sr * sa + dr * inv_sa)) */
        for (u32 c = 0; c < 3; c++) {
            mb_local_get(mf, ctx->loc_v_one);
            mb_local_get(mf, ctx->loc_out_ch[c]);
            mb_local_get(mf, ctx->loc_out_ch[3]);
            mb_f32x4_mul(mf);

            mb_local_get(mf, ctx->loc_fb_dst);
            if (c > 0) {
                mb_i32_const(mf, (i32)(c * 8));
                mb_i32x4_shr_u(mf);
            }
            mb_local_get(mf, ctx->loc_v_ff);
            mb_v128_and(mf);
            mb_f32x4_convert_i32x4_u(mf);
            mb_local_get(mf, ctx->loc_v_inv255);
            mb_f32x4_mul(mf);
            mb_local_get(mf, ctx->loc_tmp_v1);
            mb_f32x4_mul(mf);

            mb_f32x4_add(mf);
            mb_f32x4_pmin(mf);
            emit_f32x4_to_u8_i32x4(ctx);
            mb_local_set(mf, ctx->loc_out_ch[c]);
        }

        /* Kanal 3 (A): out_ch[3] = u8(pmin(1.0f, sa + da * inv_sa)) */
        mb_local_get(mf, ctx->loc_v_one);
        mb_local_get(mf, ctx->loc_out_ch[3]);
        mb_local_get(mf, ctx->loc_fb_dst);
        mb_i32_const(mf, 24);
        mb_i32x4_shr_u(mf);
        mb_f32x4_convert_i32x4_u(mf);
        mb_local_get(mf, ctx->loc_v_inv255);
        mb_f32x4_mul(mf);
        mb_local_get(mf, ctx->loc_tmp_v1);
        mb_f32x4_mul(mf);
        mb_f32x4_add(mf);
        mb_f32x4_pmin(mf);
        emit_f32x4_to_u8_i32x4(ctx);
        mb_local_set(mf, ctx->loc_out_ch[3]);
    } else if (mode == MTK_FUSE_ADD) {
        /* Kanal 0..3 (R, G, B, A): out_ch[c] = u8(pmin(1.0f, sc + dc)) */
        for (u32 c = 0; c < 4; c++) {
            mb_local_get(mf, ctx->loc_v_one);
            mb_local_get(mf, ctx->loc_out_ch[c]);

            mb_local_get(mf, ctx->loc_fb_dst);
            if (c > 0) {
                mb_i32_const(mf, (i32)(c * 8));
                mb_i32x4_shr_u(mf);
            }
            if (c < 3) {
                mb_local_get(mf, ctx->loc_v_ff);
                mb_v128_and(mf);
            }
            mb_f32x4_convert_i32x4_u(mf);
            mb_local_get(mf, ctx->loc_v_inv255);
            mb_f32x4_mul(mf);

            mb_f32x4_add(mf);
            mb_f32x4_pmin(mf);
            emit_f32x4_to_u8_i32x4(ctx);
            mb_local_set(mf, ctx->loc_out_ch[c]);
        }
    }

    /* 4 x i32x4 (R, G, B, A) -> narrow_u -> narrow_u -> shuffle ile 4 x RGBA8 paketle */
    mb_local_get(mf, ctx->loc_out_ch[0]);
    mb_local_get(mf, ctx->loc_out_ch[1]);
    mb_i16x8_narrow_i32x4_u(mf);

    mb_local_get(mf, ctx->loc_out_ch[2]);
    mb_local_get(mf, ctx->loc_out_ch[3]);
    mb_i16x8_narrow_i32x4_u(mf);

    mb_i8x16_narrow_i16x8_u(mf);
    mb_local_tee(mf, ctx->loc_tmp_v0);
    mb_local_get(mf, ctx->loc_tmp_v0);
    static const u8 rgba_shuf[16] = {
        0, 4, 8, 12,
        1, 5, 9, 13,
        2, 6, 10, 14,
        3, 7, 11, 15
    };
    mb_i8x16_shuffle(mf, rgba_shuf);

    /* Aktif lane maskesiyle (loc_blend_mask) yeni pikselleri ve eski hedef pikselleri seç */
    mb_local_get(mf, ctx->loc_fb_dst);
    mb_local_get(mf, ctx->loc_blend_mask);
    mb_v128_bitselect(mf);
    mb_local_tee(mf, ctx->loc_tmp_v0);

    /* Satır 0 ve Satır 1'e 64-bit lane store ile geri yaz (yığında tmp_v0 var, önce addr0 gerekir) */
    mb_drop(mf);
    mb_local_get(mf, ctx->loc_fb_addr0);
    mb_local_get(mf, ctx->loc_tmp_v0);
    mb_v128_store64_lane(mf, 3, 0, 0);

    mb_local_get(mf, ctx->loc_fb_addr1);
    mb_local_get(mf, ctx->loc_tmp_v0);
    mb_v128_store64_lane(mf, 3, 0, 1);
}

u32 mtk_codegen(const IrFunc* f, const MtkCodegenOpts* opts, Arena* scratch,
                u8* out, u32 out_cap, char* err, u32 err_cap) {
    if (err && err_cap > 0) err[0] = '\0';
    if (!f || !scratch || !out || out_cap == 0) {
        cg_fail(err, err_cap, "invalid arguments to mtk_codegen");
        return 0;
    }
    if (f->count > CG_MAX_INSTS) {
        cg_fail(err, err_cap, "IR instruction count %u exceeds maximum %u", f->count, (u32)CG_MAX_INSTS);
        return 0;
    }
    if (!ir_verify(f, err, err_cap)) {
        return 0;
    }

    MtkCodegenOpts default_opts = { FALSE, FALSE, MTK_FUSE_NONE };
    if (!opts) opts = &default_opts;

    usize mark = arena_mark(scratch);
    CgCtx* ctx = ARENA_PUSH_STRUCT(scratch, CgCtx);
    ctx->f    = f;
    ctx->opts = opts;

    usize rem = scratch->cap - scratch->used;
    if (rem < 65536) {
        arena_pop_to(scratch, mark);
        cg_fail(err, err_cap, "insufficient scratch arena space (%zu bytes)", rem);
        return 0;
    }
    u32 work_cap = (rem > (usize)MB(4)) ? (u32)MB(4) : (u32)(rem & ~15u);
    u8* work_mem = ARENA_PUSH_ARRAY(scratch, u8, work_cap);

    MbModule mod;
    mb_module_init(&mod, work_mem, work_cap);

    if (opts->own_memory) {
        mb_memory(&mod, 64, 64, FALSE);
    } else {
        u32 max_pages = opts->shared_memory ? 16384u : 0u;
        mb_import_memory(&mod, "env", "memory", 1, max_pages, opts->shared_memory);
    }

    const u8 shade_params[1] = { MB_VAL_I32 };
    u32 t_shade = mb_type(&mod, shade_params, 1, NULL, 0);
    MbFunc* mf = mb_func_begin(&mod, t_shade);
    ctx->mf = mf;

    analyze_liveness_and_trees(ctx);
    allocate_locals(ctx);

    /* 1. count = batch->count; if (count == 0) return; */
    mb_local_get(mf, ctx->loc_batch);
    mb_i32_load(mf, 2, SHADE_OFF_COUNT);
    mb_local_tee(mf, ctx->loc_count);
    mb_i32_eqz(mf);
    mb_if(mf, MB_BLOCK_VOID);
    mb_return(mf);
    mb_end(mf);

    /* 2. Yalnızca gerçekten kullanılan ShadeBatch alanlarını ve döngü önü taban adreslerini yükle */
    if (ctx->need_qhdr) {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_QUADS);
        mb_local_set(mf, ctx->loc_quads);
    }
    if (ctx->has_any_uniform) {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_UNIFORMS);
        mb_local_set(mf, ctx->loc_uniforms);
    }
    if (ctx->has_any_tex) {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_TEXTURES);
        mb_local_set(mf, ctx->loc_textures);

        for (u32 t = 0; t < SHADE_MAX_TEXTURES; t++) {
            if (ctx->used_tex_load_u[t] || ctx->used_tex_load_v[t]) {
                mb_local_get(mf, ctx->loc_textures);
                mb_i32_load(mf, 2, t * TEX_SIZE + TEX_OFF_PX);
                if (ctx->used_tex_load_u[t] && ctx->used_tex_load_v[t]) {
                    mb_local_tee(mf, ctx->loc_tex_px[t]);
                    mb_i32x4_splat(mf);
                    mb_local_set(mf, ctx->loc_tex_px_v[t]);
                } else if (ctx->used_tex_load_u[t]) {
                    mb_local_set(mf, ctx->loc_tex_px[t]);
                } else {
                    mb_i32x4_splat(mf);
                    mb_local_set(mf, ctx->loc_tex_px_v[t]);
                }
            }
        }
    }

    /* count * 16 byte kanal adımı (loc_tmp_i32 içinde) */
    if (ctx->has_any_varying || opts->fuse_blend == MTK_FUSE_NONE) {
        mb_local_get(mf, ctx->loc_count);
        mb_i32_const(mf, 4);
        mb_i32_shl(mf);
        mb_local_set(mf, ctx->loc_tmp_i32);
    }

    if (ctx->has_any_varying) {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_VARYINGS);
        mb_local_set(mf, ctx->loc_varyings);

        for (u32 v = 0; v < SHADE_MAX_VARYINGS; v++) {
            if (!ctx->used_varying[v]) continue;
            mb_local_get(mf, ctx->loc_varyings);
            if (v > 0) {
                mb_local_get(mf, ctx->loc_tmp_i32);
                if (v > 1) {
                    mb_i32_const(mf, (i32)v);
                    mb_i32_mul(mf);
                }
                mb_i32_add(mf);
            }
            mb_local_set(mf, ctx->loc_var_base[v]);
        }
    }

    if (opts->fuse_blend == MTK_FUSE_NONE) {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_OUT_RGBA);
        mb_local_tee(mf, ctx->loc_out_base[0]);
        mb_local_get(mf, ctx->loc_tmp_i32);
        mb_i32_add(mf);
        mb_local_tee(mf, ctx->loc_out_base[1]);
        mb_local_get(mf, ctx->loc_tmp_i32);
        mb_i32_add(mf);
        mb_local_tee(mf, ctx->loc_out_base[2]);
        mb_local_get(mf, ctx->loc_tmp_i32);
        mb_i32_add(mf);
        mb_local_set(mf, ctx->loc_out_base[3]);
    } else {
        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_FB_PX);
        mb_local_set(mf, ctx->loc_fb_px);

        mb_local_get(mf, ctx->loc_batch);
        mb_i32_load(mf, 2, SHADE_OFF_FB_STRIDE);
        mb_local_tee(mf, ctx->loc_fb_stride);
        mb_i32_const(mf, 2);
        mb_i32_shl(mf);
        mb_local_set(mf, ctx->loc_fb_stride_bytes);
    }

    /* 3. Döngü önü sabit v128 vektörlerini ve LICM ile taşınan uniform hesapları hazırla */
    if (ctx->need_active_mask || opts->fuse_blend != MTK_FUSE_NONE) {
        mb_i32x4_const(mf, 1, 2, 4, 8);
        mb_local_set(mf, ctx->loc_v_mask_bits);
    }
    if (ctx->need_sign_mask) {
        mb_f32x4_const(mf, -0.0f, -0.0f, -0.0f, -0.0f);
        mb_local_set(mf, ctx->loc_v_sign_mask);
    }
    if (ctx->has_frag_x) {
        mb_f32x4_const(mf, 0.5f, 1.5f, 0.5f, 1.5f);
        mb_local_set(mf, ctx->loc_v_frag_dx);
    }
    if (ctx->has_frag_y) {
        mb_f32x4_const(mf, 0.5f, 0.5f, 1.5f, 1.5f);
        mb_local_set(mf, ctx->loc_v_frag_dy);
    }
    if (opts->fuse_blend != MTK_FUSE_NONE) {
        mb_f32x4_const(mf, 0.0f, 0.0f, 0.0f, 0.0f);
        mb_local_set(mf, ctx->loc_v_zero);
        mb_f32x4_const(mf, 1.0f, 1.0f, 1.0f, 1.0f);
        mb_local_set(mf, ctx->loc_v_one);
        mb_f32x4_const(mf, 255.0f, 255.0f, 255.0f, 255.0f);
        mb_local_set(mf, ctx->loc_v_255);
        mb_f32x4_const(mf, 0.5f, 0.5f, 0.5f, 0.5f);
        mb_local_set(mf, ctx->loc_v_half);
        if (opts->fuse_blend == MTK_FUSE_ALPHA || opts->fuse_blend == MTK_FUSE_ADD) {
            const f32 inv255 = 1.0f / 255.0f;
            mb_f32x4_const(mf, inv255, inv255, inv255, inv255);
            mb_local_set(mf, ctx->loc_v_inv255);
            mb_i32x4_const(mf, 0xFF, 0xFF, 0xFF, 0xFF);
            mb_local_set(mf, ctx->loc_v_ff);
        }
    }

    emit_preheader_insts(ctx);

    /* 4. Alt-testli (do-while) q = 0..count-1 döngüsü:
     * Her iterasyonda q<<4 ve quads+(q<<3) hesaplamak yerine loc_q16 (+16) ve loc_qhdr (+8)
     * indüksiyon değişkenlerini doğrudan ilerlet! */
    if (ctx->need_q16) {
        mb_local_get(mf, ctx->loc_count);
        mb_i32_const(mf, 4);
        mb_i32_shl(mf);
        mb_local_set(mf, ctx->loc_count);
        mb_i32_const(mf, 0);
        mb_local_set(mf, ctx->loc_q16);
        if (ctx->need_qhdr) {
            mb_local_get(mf, ctx->loc_quads);
            mb_local_set(mf, ctx->loc_qhdr);
        }
    } else if (ctx->need_qhdr) {
        mb_local_get(mf, ctx->loc_quads);
        mb_local_tee(mf, ctx->loc_qhdr);
        mb_local_get(mf, ctx->loc_count);
        mb_i32_const(mf, 3);
        mb_i32_shl(mf);
        mb_i32_add(mf);
        mb_local_set(mf, ctx->loc_count);
    }

    mb_loop(mf, MB_BLOCK_VOID);

    if (ctx->has_deriv && ctx->need_active_mask) {
        /* Türev kullanan shader: kontrol akışı 4 lane'in hepsiyle (yardımcı lane'ler dahil) yürür;
         * kapsama maskesi yalnızca yazımı (discard / füzyonlu blend) belirler. */
        if (opts->fuse_blend != MTK_FUSE_NONE) {
            mb_local_get(mf, ctx->loc_qhdr);
            mb_i32_load(mf, 2, QUAD_OFF_MASK);
            mb_i32x4_splat(mf);
            mb_local_get(mf, ctx->loc_v_mask_bits);
            mb_v128_and(mf);
            mb_local_get(mf, ctx->loc_v_mask_bits);
            mb_i32x4_eq(mf);
            mb_local_set(mf, ctx->loc_blend_mask);
        }
        mb_i32x4_const(mf, -1, -1, -1, -1);
        mb_local_set(mf, ctx->loc_active);
    } else if (ctx->need_active_mask || opts->fuse_blend != MTK_FUSE_NONE) {
        mb_local_get(mf, ctx->loc_qhdr);
        mb_i32_load(mf, 2, QUAD_OFF_MASK);
        mb_i32x4_splat(mf);
        mb_local_get(mf, ctx->loc_v_mask_bits);
        mb_v128_and(mf);
        mb_local_get(mf, ctx->loc_v_mask_bits);
        mb_i32x4_eq(mf);
        if (ctx->need_active_mask && opts->fuse_blend != MTK_FUSE_NONE) {
            mb_local_tee(mf, ctx->loc_blend_mask);
            mb_local_set(mf, ctx->loc_active);
        } else if (opts->fuse_blend != MTK_FUSE_NONE) {
            mb_local_set(mf, ctx->loc_blend_mask);
        } else {
            mb_local_set(mf, ctx->loc_active);
        }
    }

    /* 5. Döngü içi IR komutlarını üret */
    for (u32 i = 0; i < f->count; i++) {
        if (ctx->is_preheader[i]) continue;
        emit_inst(ctx, i);
    }

    /* 6. Kaynaştırılmış blend varsa uygula */
    if (opts->fuse_blend != MTK_FUSE_NONE) {
        emit_fused_blend_epilogue(ctx);
    }

    /* İndüksiyon değişkenlerini ilerlet ve döngü sonunu kontrol et */
    if (ctx->need_q16) {
        if (ctx->need_qhdr) {
            mb_local_get(mf, ctx->loc_qhdr);
            mb_i32_const(mf, QUAD_SIZE);
            mb_i32_add(mf);
            mb_local_set(mf, ctx->loc_qhdr);
        }
        mb_local_get(mf, ctx->loc_q16);
        mb_i32_const(mf, 16);
        mb_i32_add(mf);
        mb_local_tee(mf, ctx->loc_q16);
        mb_local_get(mf, ctx->loc_count);
        mb_i32_ne(mf);
        mb_br_if(mf, 0);
    } else {
        mb_local_get(mf, ctx->loc_qhdr);
        mb_i32_const(mf, QUAD_SIZE);
        mb_i32_add(mf);
        mb_local_tee(mf, ctx->loc_qhdr);
        mb_local_get(mf, ctx->loc_count);
        mb_i32_ne(mf);
        mb_br_if(mf, 0);
    }

    mb_end(mf); /* loop sonu */

    mb_func_end(mf);
    mb_export_func(&mod, "shade", mf->func_idx);

    if (opts->own_memory) {
        MbExport* exp = &mod.exports[mod.export_count++];
        exp->name[0] = 'm'; exp->name[1] = 'e'; exp->name[2] = 'm';
        exp->name[3] = 'o'; exp->name[4] = 'r'; exp->name[5] = 'y';
        exp->name[6] = '\0';
        exp->name_len = 6;
        exp->kind     = MB_EXT_MEMORY;
        exp->index    = 0;
    }

    MbBuf out_buf;
    mb_buf_init(&out_buf, out, out_cap);
    u32 total_bytes = mb_module_finish(&mod, &out_buf);

    arena_pop_to(scratch, mark);
    return total_bytes;
}
