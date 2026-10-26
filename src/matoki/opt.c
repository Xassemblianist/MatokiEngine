/* matoki/opt.c — Matoki IR optimizasyon geçişleri (D hattı).
 * Uniform özelleştirme, sabit katlama ve NaN-güvenli cebirsel sadeleştirme (constfold),
 * döngüden bağımsız kod taşıma (LICM), bölge-farkında ortak alt-ifade eleme (CSE),
 * ölü kod ve boş IF eleme (DCE) ile NOP sıkıştırma (compact) geçişlerini içerir.
 */
#include "matoki/ir.h"
#include "matoki/variant.h"
#include "core/log.h"

#define OPT_MAX_INSTS 4096

static inline u32 f32_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

static inline f32 bits_f32(u32 u) {
    f32 x;
    __builtin_memcpy(&x, &u, 4);
    return x;
}

static inline b32 is_nan_f32(f32 x) {
    return (f32_bits(x) & 0x7FFFFFFFu) > 0x7F800000u;
}

static inline f32 canon_nan_f32(void) {
    return bits_f32(0x7FC00000u);
}

static inline b32 is_pos_zero_f32(f32 x) {
    return f32_bits(x) == 0x00000000u;
}

static inline b32 is_neg_zero_f32(f32 x) {
    return f32_bits(x) == 0x80000000u;
}

static inline b32 is_pos_one_f32(f32 x) {
    return f32_bits(x) == 0x3F800000u;
}

static inline f32 cf_fadd(f32 a, f32 b) {
    f32 r = a + b;
    return is_nan_f32(r) ? canon_nan_f32() : r;
}

static inline f32 cf_fsub(f32 a, f32 b) {
    f32 r = a - b;
    return is_nan_f32(r) ? canon_nan_f32() : r;
}

static inline f32 cf_fmul(f32 a, f32 b) {
    f32 r = a * b;
    return is_nan_f32(r) ? canon_nan_f32() : r;
}

static inline f32 cf_fdiv(f32 a, f32 b) {
    f32 r = a / b;
    return is_nan_f32(r) ? canon_nan_f32() : r;
}

static inline f32 cf_fmin(f32 a, f32 b) {
    if (is_nan_f32(a) || is_nan_f32(b)) return canon_nan_f32();
    if (a == 0.0f && b == 0.0f) return bits_f32(f32_bits(a) | f32_bits(b));
    return a < b ? a : b;
}

static inline f32 cf_fmax(f32 a, f32 b) {
    if (is_nan_f32(a) || is_nan_f32(b)) return canon_nan_f32();
    if (a == 0.0f && b == 0.0f) return bits_f32(f32_bits(a) & f32_bits(b));
    return a > b ? a : b;
}

static inline f32 cf_fneg(f32 a) {
    return bits_f32(f32_bits(a) ^ 0x80000000u);
}

static inline f32 cf_fabs(f32 a) {
    return bits_f32(f32_bits(a) & 0x7FFFFFFFu);
}

static inline f32 cf_fsqrt(f32 a) {
    if (is_nan_f32(a) || a < 0.0f) return canon_nan_f32();
    f32 r = __builtin_sqrtf(a);
    return is_nan_f32(r) ? canon_nan_f32() : r;
}

static inline f32 cf_ftrunc(f32 x) {
    if (is_nan_f32(x)) return canon_nan_f32();
    u32 bits = f32_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits >= 0x4B000000u) return x;
    if (abs_bits < 0x3F800000u) return bits_f32(sign);
    i32 i = (i32)x;
    f32 r = (f32)i;
    return (r == 0.0f) ? bits_f32(sign) : r;
}

static inline f32 cf_ffloor(f32 x) {
    if (is_nan_f32(x)) return canon_nan_f32();
    u32 bits = f32_bits(x);
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits == 0u || abs_bits >= 0x4B000000u) return x;
    f32 t = cf_ftrunc(x);
    if (x < t) return t - 1.0f;
    return t;
}

static inline f32 cf_fceil(f32 x) {
    if (is_nan_f32(x)) return canon_nan_f32();
    u32 bits = f32_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits == 0u || abs_bits >= 0x4B000000u) return x;
    f32 t = cf_ftrunc(x);
    if (x > t) {
        f32 r = t + 1.0f;
        return (r == 0.0f) ? bits_f32(sign) : r;
    }
    return t;
}

static inline f32 cf_fnearest(f32 x) {
    if (is_nan_f32(x)) return canon_nan_f32();
    u32 bits = f32_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits >= 0x4B000000u) return x;
    if (abs_bits <= 0x3F000000u) return bits_f32(sign);
    f32 ax = bits_f32(abs_bits);
    i32 fl = (i32)ax;
    f32 ffl = (f32)fl;
    f32 diff = ax - ffl;
    i32 res_i = fl;
    if (diff > 0.5f || (diff == 0.5f && (fl & 1))) {
        res_i = fl + 1;
    }
    f32 r = (f32)res_i;
    return bits_f32(f32_bits(r) | sign);
}

static inline i32 cf_trunc_sat_f32_s(f32 x) {
    if (is_nan_f32(x)) return 0;
    if (x >= 2147483648.0f) return I32_MAX;
    if (x < -2147483648.0f) return I32_MIN;
    return (i32)x;
}

/* Yardımcı: tek bir komutun kullandığı operand kimliklerini remap tablosuna göre günceller */
static void remap_inst_operands(const IrFunc* f, IrInst* in, const u32* remap) {
    if (in->a != IR_NONE) in->a = remap[in->a];
    if (in->b != IR_NONE) in->b = remap[in->b];
    if (in->c != IR_NONE) in->c = remap[in->c];

    /* Operandların varying bayrakları sadeleşme sonucu uniform'a düşmüş olabilir */
    IrOp op = (IrOp)in->op;
    switch (op) {
    case IR_CONST_F32:
    case IR_CONST_I32:
    case IR_CONST_BOOL:
    case IR_UNIFORM_F32:
    case IR_UNIFORM_I32:
    case IR_TEX_W:
    case IR_TEX_H:
    case IR_TEX_FLAGS:
    case IR_ELSE:
    case IR_IF_END:
    case IR_NOP:
        in->flags &= ~IR_FLAG_VARYING;
        break;
    case IR_VARYING:
    case IR_FRAG_X:
    case IR_FRAG_Y:
        in->flags |= IR_FLAG_VARYING;
        break;
    case IR_SELECT: {
        u8 v = (f->insts[in->a].flags | f->insts[in->b].flags | f->insts[in->c].flags) & IR_FLAG_VARYING;
        in->flags = (in->flags & ~IR_FLAG_VARYING) | v;
    } break;
    default: {
        u8 v = 0;
        if (in->a != IR_NONE) v |= f->insts[in->a].flags & IR_FLAG_VARYING;
        if (in->b != IR_NONE) v |= f->insts[in->b].flags & IR_FLAG_VARYING;
        in->flags = (in->flags & ~IR_FLAG_VARYING) | v;
    } break;
    }
}

static void make_const_f32(IrInst* in, f32 v) {
    in->op    = IR_CONST_F32;
    in->type  = IR_T_F32;
    in->flags &= IR_FLAG_HOISTED;
    in->a     = IR_NONE;
    in->b     = IR_NONE;
    in->c     = IR_NONE;
    in->imm.f = v;
}

static void make_const_i32(IrInst* in, i32 v) {
    in->op    = IR_CONST_I32;
    in->type  = IR_T_I32;
    in->flags &= IR_FLAG_HOISTED;
    in->a     = IR_NONE;
    in->b     = IR_NONE;
    in->c     = IR_NONE;
    in->imm.i = v;
}

static void make_const_bool(IrInst* in, b32 v) {
    in->op    = IR_CONST_BOOL;
    in->type  = IR_T_BOOL;
    in->flags &= IR_FLAG_HOISTED;
    in->a     = IR_NONE;
    in->b     = IR_NONE;
    in->c     = IR_NONE;
    in->imm.i = v ? 1 : 0;
}

void ir_specialize_uniform_f32(IrFunc* f, u32 byte_offset, f32 value) {
    ASSERT(f != NULL);
    for (u32 i = 0; i < f->count; i++) {
        IrInst* in = &f->insts[i];
        if (in->op == IR_UNIFORM_F32 && in->imm.u == byte_offset) {
            make_const_f32(in, value);
        }
    }
}

/* Boş IF/ELSE/IF_END bölgelerini (içinde sadece NOP olan) temizler ve
 * tek gövde komutu DISCARD olan ELSE'siz IF bloklarını düzleştirir */
static void remove_empty_ifs(IrFunc* f) {
    struct {
        u32 if_idx;
        u32 else_idx;
        u32 body_count;
        u32 single_idx;
    } stk[64];
    u32 depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        IrOp op = (IrOp)f->insts[i].op;
        if (op == IR_NOP) continue;

        if (op == IR_IF_BEGIN) {
            ASSERT(depth < ARRAY_LEN(stk));
            stk[depth].if_idx     = i;
            stk[depth].else_idx   = IR_NONE;
            stk[depth].body_count = 0;
            stk[depth].single_idx = IR_NONE;
            depth++;
        } else if (op == IR_ELSE) {
            ASSERT(depth > 0);
            stk[depth - 1].else_idx = i;
        } else if (op == IR_IF_END) {
            ASSERT(depth > 0);
            depth--;
            u32 if_idx = stk[depth].if_idx;
            if (stk[depth].body_count == 0) {
                f->insts[if_idx].op = IR_NOP;
                f->insts[if_idx].type = IR_T_VOID;
                if (stk[depth].else_idx != IR_NONE) {
                    f->insts[stk[depth].else_idx].op = IR_NOP;
                    f->insts[stk[depth].else_idx].type = IR_T_VOID;
                }
                f->insts[i].op = IR_NOP;
                f->insts[i].type = IR_T_VOID;
            } else if (stk[depth].body_count == 1 &&
                       stk[depth].else_idx == IR_NONE &&
                       stk[depth].single_idx != IR_NONE &&
                       f->insts[stk[depth].single_idx].op == IR_DISCARD) {
                u32 disc_idx = stk[depth].single_idx;
                u32 c = f->insts[if_idx].a;
                u32 m = f->insts[disc_idx].a;
                if (m == c || (f->insts[m].op == IR_CONST_BOOL && f->insts[m].imm.i != 0)) {
                    f->insts[if_idx].op     = IR_NOP;
                    f->insts[if_idx].type   = IR_T_VOID;
                    f->insts[disc_idx].a     = c;
                    f->insts[disc_idx].flags = f->insts[c].flags & IR_FLAG_VARYING;
                    f->insts[i].op          = IR_NOP;
                    f->insts[i].type        = IR_T_VOID;
                } else if (m < if_idx) {
                    f->insts[if_idx].op     = IR_BAND;
                    f->insts[if_idx].type   = IR_T_BOOL;
                    f->insts[if_idx].flags  = (f->insts[c].flags | f->insts[m].flags) & IR_FLAG_VARYING;
                    f->insts[if_idx].a      = c;
                    f->insts[if_idx].b      = m;
                    f->insts[if_idx].c      = IR_NONE;
                    f->insts[disc_idx].a     = if_idx;
                    f->insts[disc_idx].flags = f->insts[if_idx].flags & IR_FLAG_VARYING;
                    f->insts[i].op          = IR_NOP;
                    f->insts[i].type        = IR_T_VOID;
                }
                if (depth > 0) {
                    stk[depth - 1].body_count += 2;
                }
            } else if (depth > 0) {
                stk[depth - 1].body_count += 2;
            }
        } else {
            if (depth > 0) {
                stk[depth - 1].body_count++;
                stk[depth - 1].single_idx = i;
            }
        }
    }
}

void ir_opt_constfold(IrFunc* f) {
    ASSERT(f != NULL);
    if (f->count > OPT_MAX_INSTS) {
        PANIC("ir_opt_constfold: count %u > OPT_MAX_INSTS", f->count);
    }

    u32 remap[OPT_MAX_INSTS];
    for (u32 i = 0; i < f->count; i++) remap[i] = i;

    /* Sabit koşula sahip IF bloklarının ölü dallarını silmek için durum yığını */
    struct {
        b32 is_const;
        b32 cond_val;
        b32 in_else;
        u32 if_idx;
        u32 else_idx;
        u32 cond_id;
        b32 parent_dead;
    } if_stk[64];
    u32 if_depth = 0;
    b32 dead_branch = FALSE;

    for (u32 i = 0; i < f->count; i++) {
        IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        remap_inst_operands(f, in, remap);

        if (op == IR_IF_BEGIN) {
            ASSERT(if_depth < ARRAY_LEN(if_stk));
            if_stk[if_depth].parent_dead = dead_branch;
            if_stk[if_depth].in_else     = FALSE;
            if_stk[if_depth].if_idx      = i;
            if_stk[if_depth].else_idx    = IR_NONE;
            if_stk[if_depth].cond_id     = in->a;
            if (!dead_branch && f->insts[in->a].op == IR_CONST_BOOL) {
                b32 c = f->insts[in->a].imm.i != 0;
                if_stk[if_depth].is_const = TRUE;
                if_stk[if_depth].cond_val = c;
                dead_branch = !c;
                /* IF_BEGIN'i NOP yap: sabit koşulda gerçek dala gerek yok */
                in->op = IR_NOP;
                in->type = IR_T_VOID;
            } else {
                if_stk[if_depth].is_const = FALSE;
                if_stk[if_depth].cond_val = FALSE;
                if (dead_branch) {
                    in->op = IR_NOP;
                    in->type = IR_T_VOID;
                }
            }
            if_depth++;
            continue;
        }
        if (op == IR_ELSE) {
            ASSERT(if_depth > 0);
            u32 top = if_depth - 1;
            if_stk[top].in_else  = TRUE;
            if_stk[top].else_idx = i;
            if (if_stk[top].parent_dead) {
                in->op = IR_NOP;
                in->type = IR_T_VOID;
            } else if (if_stk[top].is_const) {
                dead_branch = if_stk[top].cond_val;
                in->op = IR_NOP;
                in->type = IR_T_VOID;
            }
            continue;
        }
        if (op == IR_IF_END) {
            ASSERT(if_depth > 0);
            if_depth--;
            if (if_stk[if_depth].parent_dead || if_stk[if_depth].is_const) {
                in->op = IR_NOP;
                in->type = IR_T_VOID;
            }
            dead_branch = if_stk[if_depth].parent_dead;
            continue;
        }

        if (dead_branch) {
            in->op = IR_NOP;
            in->type = IR_T_VOID;
            continue;
        }

        const IrInst* ia = (in->a != IR_NONE) ? &f->insts[in->a] : NULL;
        const IrInst* ib = (in->b != IR_NONE) ? &f->insts[in->b] : NULL;
        const IrInst* ic = (in->c != IR_NONE) ? &f->insts[in->c] : NULL;

        b32 ca_f = ia && ia->op == IR_CONST_F32;
        b32 cb_f = ib && ib->op == IR_CONST_F32;
        b32 ca_i = ia && ia->op == IR_CONST_I32;
        b32 cb_i = ib && ib->op == IR_CONST_I32;
        b32 ca_b = ia && ia->op == IR_CONST_BOOL;
        b32 cb_b = ib && ib->op == IR_CONST_BOOL;
        b32 cc_b = ic && ic->op == IR_CONST_BOOL;

        switch (op) {
        case IR_FNEG:
            if (ca_f) make_const_f32(in, cf_fneg(ia->imm.f));
            else if (ia->op == IR_FNEG) remap[i] = ia->a;
            break;
        case IR_FABS:
            if (ca_f) make_const_f32(in, cf_fabs(ia->imm.f));
            else if (ia->op == IR_FABS || ia->op == IR_FNEG) {
                if (ia->op == IR_FABS) remap[i] = in->a;
                else in->a = ia->a;
            }
            break;
        case IR_FSQRT:
            if (ca_f) make_const_f32(in, cf_fsqrt(ia->imm.f));
            break;
        case IR_FFLOOR:
            if (ca_f) make_const_f32(in, cf_ffloor(ia->imm.f));
            break;
        case IR_FCEIL:
            if (ca_f) make_const_f32(in, cf_fceil(ia->imm.f));
            break;
        case IR_FTRUNC:
            if (ca_f) make_const_f32(in, cf_ftrunc(ia->imm.f));
            break;
        case IR_FNEAREST:
            if (ca_f) make_const_f32(in, cf_fnearest(ia->imm.f));
            break;

        case IR_FADD:
            if (ca_f && cb_f) {
                make_const_f32(in, cf_fadd(ia->imm.f, ib->imm.f));
            } else if (cb_f && is_neg_zero_f32(ib->imm.f)) {
                /* IEEE 754: x + (-0.0) == x (-0.0 ve sNaN->qNaN korunumu için yalnızca -0.0 güvenli) */
                remap[i] = in->a;
            } else if (ca_f && is_neg_zero_f32(ia->imm.f)) {
                remap[i] = in->b;
            }
            break;

        case IR_FSUB:
            if (ca_f && cb_f) {
                make_const_f32(in, cf_fsub(ia->imm.f, ib->imm.f));
            } else if (cb_f && is_pos_zero_f32(ib->imm.f)) {
                /* IEEE 754: x - (+0.0) == x */
                remap[i] = in->a;
            }
            break;

        case IR_FMUL:
            if (ca_f && cb_f) {
                make_const_f32(in, cf_fmul(ia->imm.f, ib->imm.f));
            } else if (cb_f && is_pos_one_f32(ib->imm.f)) {
                /* x * 1.0 == x (NaN, -0.0 ve Inf dahil) */
                remap[i] = in->a;
            } else if (ca_f && is_pos_one_f32(ia->imm.f)) {
                remap[i] = in->b;
            }
            break;

        case IR_FDIV:
            if (ca_f && cb_f) {
                make_const_f32(in, cf_fdiv(ia->imm.f, ib->imm.f));
            } else if (cb_f && is_pos_one_f32(ib->imm.f)) {
                /* x / 1.0 == x */
                remap[i] = in->a;
            }
            break;

        case IR_FMIN:
            if (ca_f && cb_f) make_const_f32(in, cf_fmin(ia->imm.f, ib->imm.f));
            else if (in->a == in->b) remap[i] = in->a;
            break;

        case IR_FMAX:
            if (ca_f && cb_f) make_const_f32(in, cf_fmax(ia->imm.f, ib->imm.f));
            else if (in->a == in->b) remap[i] = in->a;
            break;

        case IR_IADD:
            if (ca_i && cb_i) make_const_i32(in, (i32)((u32)ia->imm.i + (u32)ib->imm.i));
            else if (cb_i && ib->imm.i == 0) remap[i] = in->a;
            else if (ca_i && ia->imm.i == 0) remap[i] = in->b;
            break;

        case IR_ISUB:
            if (ca_i && cb_i) make_const_i32(in, (i32)((u32)ia->imm.i - (u32)ib->imm.i));
            else if (cb_i && ib->imm.i == 0) remap[i] = in->a;
            else if (in->a == in->b) make_const_i32(in, 0);
            break;

        case IR_IMUL:
            if (ca_i && cb_i) make_const_i32(in, (i32)((u32)ia->imm.i * (u32)ib->imm.i));
            else if (cb_i && ib->imm.i == 1) remap[i] = in->a;
            else if (ca_i && ia->imm.i == 1) remap[i] = in->b;
            else if ((cb_i && ib->imm.i == 0) || (ca_i && ia->imm.i == 0)) make_const_i32(in, 0);
            break;

        case IR_IAND:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i & ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            else if ((cb_i && ib->imm.i == 0) || (ca_i && ia->imm.i == 0)) make_const_i32(in, 0);
            else if (cb_i && ib->imm.i == -1) remap[i] = in->a;
            else if (ca_i && ia->imm.i == -1) remap[i] = in->b;
            break;

        case IR_IOR:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i | ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            else if (cb_i && ib->imm.i == 0) remap[i] = in->a;
            else if (ca_i && ia->imm.i == 0) remap[i] = in->b;
            else if ((cb_i && ib->imm.i == -1) || (ca_i && ia->imm.i == -1)) make_const_i32(in, -1);
            break;

        case IR_IXOR:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i ^ ib->imm.i);
            else if (in->a == in->b) make_const_i32(in, 0);
            else if (cb_i && ib->imm.i == 0) remap[i] = in->a;
            else if (ca_i && ia->imm.i == 0) remap[i] = in->b;
            break;

        case IR_ISHL:
            if (ca_i && cb_i) make_const_i32(in, (i32)((u32)ia->imm.i << ((u32)ib->imm.i & 31u)));
            else if (cb_i && ((u32)ib->imm.i & 31u) == 0) remap[i] = in->a;
            break;

        case IR_ISHR_S:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i >> ((u32)ib->imm.i & 31u));
            else if (cb_i && ((u32)ib->imm.i & 31u) == 0) remap[i] = in->a;
            break;

        case IR_ISHR_U:
            if (ca_i && cb_i) make_const_i32(in, (i32)((u32)ia->imm.i >> ((u32)ib->imm.i & 31u)));
            else if (cb_i && ((u32)ib->imm.i & 31u) == 0) remap[i] = in->a;
            break;

        case IR_IMIN_S:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i < ib->imm.i ? ia->imm.i : ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            break;

        case IR_IMAX_S:
            if (ca_i && cb_i) make_const_i32(in, ia->imm.i > ib->imm.i ? ia->imm.i : ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            break;

        case IR_FEQ:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f == ib->imm.f);
            break;
        case IR_FNE:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f != ib->imm.f);
            break;
        case IR_FLT:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f < ib->imm.f);
            break;
        case IR_FLE:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f <= ib->imm.f);
            break;
        case IR_FGT:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f > ib->imm.f);
            break;
        case IR_FGE:
            if (ca_f && cb_f) make_const_bool(in, ia->imm.f >= ib->imm.f);
            break;

        case IR_IEQ:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i == ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, TRUE);
            break;
        case IR_INE:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i != ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, FALSE);
            break;
        case IR_ILT_S:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i < ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, FALSE);
            break;
        case IR_ILE_S:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i <= ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, TRUE);
            break;
        case IR_IGT_S:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i > ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, FALSE);
            break;
        case IR_IGE_S:
            if (ca_i && cb_i) make_const_bool(in, ia->imm.i >= ib->imm.i);
            else if (in->a == in->b) make_const_bool(in, TRUE);
            break;

        case IR_BAND:
            if (ca_b && cb_b) make_const_bool(in, ia->imm.i && ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            else if ((cb_b && !ib->imm.i) || (ca_b && !ia->imm.i)) make_const_bool(in, FALSE);
            else if (cb_b && ib->imm.i) remap[i] = in->a;
            else if (ca_b && ia->imm.i) remap[i] = in->b;
            break;

        case IR_BOR:
            if (ca_b && cb_b) make_const_bool(in, ia->imm.i || ib->imm.i);
            else if (in->a == in->b) remap[i] = in->a;
            else if ((cb_b && ib->imm.i) || (ca_b && ia->imm.i)) make_const_bool(in, TRUE);
            else if (cb_b && !ib->imm.i) remap[i] = in->a;
            else if (ca_b && !ia->imm.i) remap[i] = in->b;
            break;

        case IR_BNOT:
            if (ca_b) make_const_bool(in, !ia->imm.i);
            else if (ia->op == IR_BNOT) remap[i] = ia->a;
            break;

        case IR_SELECT: {
            b32 resolved = FALSE;
            for (u32 d = if_depth; d > 0; d--) {
                u32 idx_d = d - 1;
                if (!if_stk[idx_d].is_const && if_stk[idx_d].cond_id == in->a) {
                    remap[i] = if_stk[idx_d].in_else ? in->c : in->b;
                    resolved = TRUE;
                    break;
                }
            }
            if (resolved) break;

            if (ca_b) {
                remap[i] = ia->imm.i ? in->b : in->c;
            } else if (in->b == in->c) {
                remap[i] = in->b;
            } else if (cb_b && cc_b) {
                if (ib->imm.i && !ic->imm.i) {
                    remap[i] = in->a;
                } else if (!ib->imm.i && ic->imm.i) {
                    in->op    = IR_BNOT;
                    in->flags = (in->flags & IR_FLAG_HOISTED) | (ia->flags & IR_FLAG_VARYING);
                    in->b     = IR_NONE;
                    in->c     = IR_NONE;
                }
            } else if (cb_b && ib->imm.i) {
                in->op    = IR_BOR;
                in->flags = (in->flags & IR_FLAG_HOISTED) | ((ia->flags | ic->flags) & IR_FLAG_VARYING);
                in->b     = in->c;
                in->c     = IR_NONE;
            } else if (cc_b && !ic->imm.i) {
                in->op    = IR_BAND;
                in->flags = (in->flags & IR_FLAG_HOISTED) | ((ia->flags | ib->flags) & IR_FLAG_VARYING);
                in->c     = IR_NONE;
            } else if (ia->op == IR_BNOT) {
                u32 tmp = in->b;
                in->a = ia->a;
                in->b = in->c;
                in->c = tmp;
            }
        } break;

        case IR_F2I:
            if (ca_f) make_const_i32(in, cf_trunc_sat_f32_s(ia->imm.f));
            break;
        case IR_I2F:
            if (ca_i) make_const_f32(in, (f32)ia->imm.i);
            break;
        case IR_BITCAST_F2I:
            if (ca_f) make_const_i32(in, (i32)f32_bits(ia->imm.f));
            else if (ia->op == IR_BITCAST_I2F) remap[i] = ia->a;
            break;
        case IR_BITCAST_I2F:
            if (ca_i) make_const_f32(in, bits_f32((u32)ia->imm.i));
            else if (ia->op == IR_BITCAST_F2I) remap[i] = ia->a;
            break;

        case IR_DISCARD:
            if (ca_b && !ia->imm.i) {
                /* discard(false) etkisizdir */
                in->op = IR_NOP;
                in->type = IR_T_VOID;
            }
            break;

        default:
            break;
        }
    }

    remove_empty_ifs(f);
}

static b32 is_commutative(IrOp op) {
    switch (op) {
    case IR_FADD:
    case IR_FMUL:
    case IR_FMIN:
    case IR_FMAX:
    case IR_IADD:
    case IR_IMUL:
    case IR_IAND:
    case IR_IOR:
    case IR_IXOR:
    case IR_IMIN_S:
    case IR_IMAX_S:
    case IR_FEQ:
    case IR_FNE:
    case IR_IEQ:
    case IR_INE:
    case IR_BAND:
    case IR_BOR:
        return TRUE;
    default:
        return FALSE;
    }
}

static b32 is_pure_value_op(IrOp op) {
    switch (op) {
    case IR_NOP:
    case IR_IF_BEGIN:
    case IR_ELSE:
    case IR_IF_END:
    case IR_DISCARD:
    case IR_OUTPUT:
        return FALSE;
    default:
        return TRUE;
    }
}

static b32 insts_equal(const IrInst* x, const IrInst* y) {
    if (x->op != y->op || x->type != y->type) return FALSE;
    if ((x->flags & IR_FLAG_VARYING) != (y->flags & IR_FLAG_VARYING)) return FALSE;

    IrOp op = (IrOp)x->op;
    switch (op) {
    case IR_CONST_F32:
        /* +0.0 ve -0.0, veya farklı bitli sabitler birleştirilmez */
        return f32_bits(x->imm.f) == f32_bits(y->imm.f);
    case IR_CONST_I32:
    case IR_CONST_BOOL:
        return x->imm.i == y->imm.i;
    case IR_UNIFORM_F32:
    case IR_UNIFORM_I32:
    case IR_VARYING:
    case IR_TEX_W:
    case IR_TEX_H:
    case IR_TEX_FLAGS:
        return x->imm.u == y->imm.u;
    case IR_FRAG_X:
    case IR_FRAG_Y:
        return TRUE;
    case IR_TEXEL_LOAD:
        return x->imm.u == y->imm.u && x->a == y->a;
    case IR_SELECT:
        return x->a == y->a && x->b == y->b && x->c == y->c;
    default:
        return x->a == y->a && x->b == y->b;
    }
}

/* Bölge kapsama kontrolü: r_def bölgesi r_use bölgesinin atası (veya kendisi) mi? */
static b32 region_dominates(u32 r_def, u32 r_use, const u32* reg_parent) {
    u32 cur = r_use;
    while (cur != r_def) {
        if (cur == 0) return FALSE;
        cur = reg_parent[cur];
    }
    return TRUE;
}

void ir_opt_cse(IrFunc* f) {
    ASSERT(f != NULL);
    if (f->count > OPT_MAX_INSTS) {
        PANIC("ir_opt_cse: count %u > OPT_MAX_INSTS", f->count);
    }

    u32 remap[OPT_MAX_INSTS];
    u32 inst_reg[OPT_MAX_INSTS];
    u32 reg_parent[OPT_MAX_INSTS];
    u32 if_reg_stk[64];
    u32 if_depth = 0;

    u32 cur_reg = 0;
    u32 next_reg = 1;
    reg_parent[0] = 0;

    for (u32 i = 0; i < f->count; i++) {
        remap[i] = i;
        IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) {
            inst_reg[i] = cur_reg;
            continue;
        }

        remap_inst_operands(f, in, remap);

        if (op == IR_IF_BEGIN) {
            inst_reg[i] = cur_reg;
            ASSERT(if_depth < ARRAY_LEN(if_reg_stk));
            if_reg_stk[if_depth++] = cur_reg;
            u32 new_r = next_reg++;
            reg_parent[new_r] = cur_reg;
            cur_reg = new_r;
            continue;
        }
        if (op == IR_ELSE) {
            ASSERT(if_depth > 0);
            u32 parent_r = if_reg_stk[if_depth - 1];
            inst_reg[i] = parent_r;
            u32 new_r = next_reg++;
            reg_parent[new_r] = parent_r;
            cur_reg = new_r;
            continue;
        }
        if (op == IR_IF_END) {
            ASSERT(if_depth > 0);
            cur_reg = if_reg_stk[--if_depth];
            inst_reg[i] = cur_reg;
            continue;
        }

        inst_reg[i] = cur_reg;
        if (!is_pure_value_op(op)) continue;

        /* Değişmeli (commutative) ikili işlemlerde operandları kanonik sıraya sok:
         * varying operand her zaman ilk sırada (in->a) olsun ki codegen'de yığın
         * zincirlemesi (tree-ification) kesintisiz çalışsın; eşitse küçük indeks önce. */
        if (is_commutative(op)) {
            b32 va = (f->insts[in->a].flags & IR_FLAG_VARYING) != 0;
            b32 vb = (f->insts[in->b].flags & IR_FLAG_VARYING) != 0;
            if ((!va && vb) || (va == vb && in->a > in->b)) {
                u32 tmp = in->a;
                in->a = in->b;
                in->b = tmp;
            }
        }

        for (u32 j = 0; j < i; j++) {
            const IrInst* prev = &f->insts[j];
            if (prev->op == IR_NOP) continue;
            if (remap[j] != j) continue;
            if (insts_equal(in, prev) && region_dominates(inst_reg[j], cur_reg, reg_parent)) {
                remap[i] = j;
                break;
            }
        }
    }
}

void ir_opt_dce(IrFunc* f) {
    ASSERT(f != NULL);
    if (f->count > OPT_MAX_INSTS) {
        PANIC("ir_opt_dce: count %u > OPT_MAX_INSTS", f->count);
    }

    for (u32 i = 0; i < f->count; i++) {
        f->insts[i].flags &= ~IR_FLAG_LIVE;
    }

    /* DISCARD bir IF bloğu içindeyse o IF bloğunun koşulu da canlıdır.
     * Bunu takip etmek için her komutun doğrudan hangi IF_BEGIN altında olduğunu bulalım. */
    u32 enclosing_if[OPT_MAX_INSTS];
    u32 if_stk[64];
    u32 depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        IrOp op = (IrOp)f->insts[i].op;
        enclosing_if[i] = (depth > 0) ? if_stk[depth - 1] : IR_NONE;
        if (op == IR_IF_BEGIN) {
            ASSERT(depth < ARRAY_LEN(if_stk));
            if_stk[depth++] = i;
        } else if (op == IR_IF_END) {
            ASSERT(depth > 0);
            depth--;
            enclosing_if[i] = (depth > 0) ? if_stk[depth - 1] : IR_NONE;
        }
    }

    /* Kökleri işaretle: OUTPUT ve DISCARD */
    for (u32 i = 0; i < f->count; i++) {
        IrOp op = (IrOp)f->insts[i].op;
        if (op == IR_OUTPUT || op == IR_DISCARD) {
            f->insts[i].flags |= IR_FLAG_LIVE;
        }
    }

    /* Geriye doğru canlılık yayılımı (SSA'da tanımlar her zaman kullanımdan öncedir) */
    for (u32 i = f->count; i > 0; i--) {
        u32 idx = i - 1;
        IrInst* in = &f->insts[idx];
        if (in->op == IR_NOP) continue;
        if (!(in->flags & IR_FLAG_LIVE)) continue;

        if (in->a != IR_NONE) f->insts[in->a].flags |= IR_FLAG_LIVE;
        if (in->b != IR_NONE) f->insts[in->b].flags |= IR_FLAG_LIVE;
        if (in->c != IR_NONE) f->insts[in->c].flags |= IR_FLAG_LIVE;

        /* Bu canlı komut bir IF bölgesinin içindeyse, kapsayıcı IF_BEGIN de canlıdır
         * (çünkü atlama optimizasyonu için IF_BEGIN'in koşuluna ihtiyaç vardır). */
        u32 p_if = enclosing_if[idx];
        if (p_if != IR_NONE) {
            f->insts[p_if].flags |= IR_FLAG_LIVE;
        }
    }

    /* Canlı olmayan değer komutlarını NOP'a çevir (IF_BEGIN/ELSE/IF_END aşağıda temizlenir) */
    for (u32 i = 0; i < f->count; i++) {
        IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP || op == IR_IF_BEGIN || op == IR_ELSE || op == IR_IF_END) {
            in->flags &= ~IR_FLAG_LIVE;
            continue;
        }
        if (!(in->flags & IR_FLAG_LIVE)) {
            in->op    = IR_NOP;
            in->type  = IR_T_VOID;
            in->flags = 0;
            in->a     = IR_NONE;
            in->b     = IR_NONE;
            in->c     = IR_NONE;
        } else {
            in->flags &= ~IR_FLAG_LIVE;
        }
    }

    /* Boş kalan IF bölgelerini temizle */
    remove_empty_ifs(f);
}

void ir_compact(IrFunc* f) {
    ASSERT(f != NULL);
    if (f->count > OPT_MAX_INSTS) {
        PANIC("ir_compact: count %u > OPT_MAX_INSTS", f->count);
    }

    u32 new_id[OPT_MAX_INSTS];
    u32 dst = 0;

    for (u32 i = 0; i < f->count; i++) {
        IrInst in = f->insts[i];
        if (in.op == IR_NOP) {
            new_id[i] = IR_NONE;
            continue;
        }
        if (in.a != IR_NONE) {
            ASSERT(in.a < i && new_id[in.a] != IR_NONE);
            in.a = new_id[in.a];
        }
        if (in.b != IR_NONE) {
            ASSERT(in.b < i && new_id[in.b] != IR_NONE);
            in.b = new_id[in.b];
        }
        if (in.c != IR_NONE) {
            ASSERT(in.c < i && new_id[in.c] != IR_NONE);
            in.c = new_id[in.c];
        }
        new_id[i] = dst;
        f->insts[dst++] = in;
    }
    f->count = dst;
}

void ir_opt_licm(IrFunc* f) {
    ASSERT(f != NULL);
    if (f->count > OPT_MAX_INSTS) {
        PANIC("ir_opt_licm: count %u > OPT_MAX_INSTS", f->count);
    }

    b32 can_hoist[OPT_MAX_INSTS];
    u32 if_depth = 0;

    for (u32 i = 0; i < f->count; i++) {
        IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        can_hoist[i] = FALSE;

        if (op == IR_NOP) continue;
        if (op == IR_IF_BEGIN) {
            if_depth++;
            continue;
        }
        if (op == IR_IF_END) {
            ASSERT(if_depth > 0);
            if_depth--;
            continue;
        }
        if (op == IR_ELSE || !is_pure_value_op(op)) {
            continue;
        }
        if (in->flags & IR_FLAG_VARYING) {
            continue;
        }
        /* Koşullu dallar içindeki bellek okumaları (IR_TEXEL_LOAD) sınır kontrolüne
         * bağlı olabileceğinden yalnızca üst seviyede (if_depth == 0) dışarı taşınır */
        if (op == IR_TEXEL_LOAD && if_depth > 0) {
            continue;
        }
        if (in->a != IR_NONE && !can_hoist[in->a]) continue;
        if (in->b != IR_NONE && !can_hoist[in->b]) continue;
        if (in->c != IR_NONE && !can_hoist[in->c]) continue;

        can_hoist[i] = TRUE;
    }

    IrInst tmp[OPT_MAX_INSTS];
    u32 new_id[OPT_MAX_INSTS];
    u32 dst = 0;

    /* 1. Taşınabilen tüm döngüden bağımsız (uniform) komutları ön-başlığa (preheader) al */
    for (u32 i = 0; i < f->count; i++) {
        if (!can_hoist[i]) continue;
        IrInst in = f->insts[i];
        if (in.a != IR_NONE) in.a = new_id[in.a];
        if (in.b != IR_NONE) in.b = new_id[in.b];
        if (in.c != IR_NONE) in.c = new_id[in.c];
        in.flags |= IR_FLAG_HOISTED;
        new_id[i] = dst;
        tmp[dst++] = in;
    }

    /* 2. Döngü içinde kalan komutları sırasını koruyarak yerleştir */
    for (u32 i = 0; i < f->count; i++) {
        if (can_hoist[i]) continue;
        IrInst in = f->insts[i];
        if (in.op != IR_NOP) {
            if (in.a != IR_NONE) in.a = new_id[in.a];
            if (in.b != IR_NONE) in.b = new_id[in.b];
            if (in.c != IR_NONE) in.c = new_id[in.c];
            in.flags &= ~IR_FLAG_HOISTED;
        }
        new_id[i] = dst;
        tmp[dst++] = in;
    }

    for (u32 i = 0; i < dst; i++) {
        f->insts[i] = tmp[i];
    }
    f->count = dst;

    remove_empty_ifs(f);
    ir_opt_dce(f);
    ir_compact(f);
}

u32 ir_count_loop_insts(const IrFunc* f) {
    ASSERT(f != NULL);
    u32 loop_count = 0;
    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        if (in->op == IR_NOP) continue;
        if (!(in->flags & IR_FLAG_HOISTED)) {
            loop_count++;
        }
    }
    return loop_count;
}

void ir_optimize(IrFunc* f) {
    ASSERT(f != NULL);
    for (u32 pass = 0; pass < 4; pass++) {
        u32 prev_count = f->count;
        ir_opt_constfold(f);
        ir_opt_cse(f);
        ir_opt_dce(f);
        ir_compact(f);
        ir_opt_licm(f);
        if (f->count == prev_count && pass > 0) {
            break;
        }
    }
}
