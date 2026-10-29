/* matoki/ir_eval.c — Matoki IR referans yorumlayıcısı.
 * IR komutlarını skaler C ile, 2x2 quad'ın 4 lane'i üzerinde çalıştırır.
 * Tüm f32 ve i32 işlemleri WebAssembly (wasm32 + SIMD) semantiği ile bit-bit
 * uyumludur (NaN yayılımı, -0.0 işaretli min/max, round-to-nearest-even, trunc_sat_f32_s).
 */
#include "matoki/codegen.h"
#include "matoki/variant.h"
#include "core/log.h"

typedef union {
    f32 f[4];
    i32 i[4];
    u32 u[4];
} EvalVal;

static inline u32 f32_to_bits(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

static inline f32 bits_to_f32(u32 u) {
    f32 x;
    __builtin_memcpy(&x, &u, 4);
    return x;
}

static inline b32 f32_is_nan(f32 x) {
    return (f32_to_bits(x) & 0x7FFFFFFFu) > 0x7F800000u;
}

static inline f32 f32_canon_nan(void) {
    return bits_to_f32(0x7FC00000u);
}

static inline f32 wasm_fadd(f32 a, f32 b) {
    f32 r = a + b;
    return f32_is_nan(r) ? f32_canon_nan() : r;
}

static inline f32 wasm_fsub(f32 a, f32 b) {
    f32 r = a - b;
    return f32_is_nan(r) ? f32_canon_nan() : r;
}

static inline f32 wasm_fmul(f32 a, f32 b) {
    f32 r = a * b;
    return f32_is_nan(r) ? f32_canon_nan() : r;
}

static inline f32 wasm_fdiv(f32 a, f32 b) {
    f32 r = a / b;
    return f32_is_nan(r) ? f32_canon_nan() : r;
}

static inline f32 wasm_fmin(f32 a, f32 b) {
    if (f32_is_nan(a) || f32_is_nan(b)) return f32_canon_nan();
    if (a == 0.0f && b == 0.0f) {
        return bits_to_f32(f32_to_bits(a) | f32_to_bits(b));
    }
    return a < b ? a : b;
}

static inline f32 wasm_fmax(f32 a, f32 b) {
    if (f32_is_nan(a) || f32_is_nan(b)) return f32_canon_nan();
    if (a == 0.0f && b == 0.0f) {
        return bits_to_f32(f32_to_bits(a) & f32_to_bits(b));
    }
    return a > b ? a : b;
}

static inline f32 wasm_fneg(f32 a) {
    return bits_to_f32(f32_to_bits(a) ^ 0x80000000u);
}

static inline f32 wasm_fabs(f32 a) {
    return bits_to_f32(f32_to_bits(a) & 0x7FFFFFFFu);
}

static inline f32 wasm_fsqrt(f32 a) {
    if (f32_is_nan(a)) return f32_canon_nan();
    if (a < 0.0f) return f32_canon_nan();
    f32 r = __builtin_sqrtf(a);
    return f32_is_nan(r) ? f32_canon_nan() : r;
}

static inline f32 wasm_ftrunc(f32 x) {
    if (f32_is_nan(x)) return f32_canon_nan();
    u32 bits = f32_to_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits >= 0x4B000000u) return x; /* |x| >= 2^23 veya Inf */
    if (abs_bits < 0x3F800000u) return bits_to_f32(sign); /* |x| < 1.0 -> +-0.0 */
    i32 i = (i32)x;
    f32 r = (f32)i;
    return (r == 0.0f) ? bits_to_f32(sign) : r;
}

static inline f32 wasm_ffloor(f32 x) {
    if (f32_is_nan(x)) return f32_canon_nan();
    u32 bits = f32_to_bits(x);
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits == 0u || abs_bits >= 0x4B000000u) return x;
    f32 t = wasm_ftrunc(x);
    if (x < t) return t - 1.0f;
    return t;
}

static inline f32 wasm_fceil(f32 x) {
    if (f32_is_nan(x)) return f32_canon_nan();
    u32 bits = f32_to_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits == 0u || abs_bits >= 0x4B000000u) return x;
    f32 t = wasm_ftrunc(x);
    if (x > t) {
        f32 r = t + 1.0f;
        return (r == 0.0f) ? bits_to_f32(sign) : r;
    }
    return t;
}

static inline f32 wasm_fnearest(f32 x) {
    if (f32_is_nan(x)) return f32_canon_nan();
    u32 bits = f32_to_bits(x);
    u32 sign = bits & 0x80000000u;
    u32 abs_bits = bits & 0x7FFFFFFFu;
    if (abs_bits >= 0x4B000000u) return x;
    if (abs_bits <= 0x3F000000u) return bits_to_f32(sign); /* |x| <= 0.5 -> +-0.0 */
    f32 ax = bits_to_f32(abs_bits);
    i32 fl = (i32)ax;
    f32 ffl = (f32)fl;
    f32 diff = ax - ffl;
    i32 res_i = fl;
    if (diff > 0.5f || (diff == 0.5f && (fl & 1))) {
        res_i = fl + 1;
    }
    f32 r = (f32)res_i;
    return bits_to_f32(f32_to_bits(r) | sign);
}

static inline i32 wasm_trunc_sat_f32_s(f32 x) {
    if (f32_is_nan(x)) return 0;
    if (x >= 2147483648.0f) return I32_MAX;
    if (x < -2147483648.0f) return I32_MIN;
    return (i32)x;
}

static inline i32 wasm_iadd(i32 a, i32 b) { return (i32)((u32)a + (u32)b); }
static inline i32 wasm_isub(i32 a, i32 b) { return (i32)((u32)a - (u32)b); }
static inline i32 wasm_imul(i32 a, i32 b) { return (i32)((u32)a * (u32)b); }
static inline i32 wasm_ishl(i32 a, i32 b) { return (i32)((u32)a << ((u32)b & 31u)); }
static inline i32 wasm_ishr_s(i32 a, i32 b) { return a >> ((u32)b & 31u); }
static inline i32 wasm_ishr_u(i32 a, i32 b) { return (i32)((u32)a >> ((u32)b & 31u)); }

static inline f32 eval_clamp01(f32 x) {
    f32 c = (x > 0.0f) ? x : 0.0f;
    return (c < 1.0f) ? c : 1.0f;
}

static inline u8 eval_f32_to_u8(f32 v) {
    f32 c = eval_clamp01(v);
    return (u8)wasm_trunc_sat_f32_s(wasm_fadd(wasm_fmul(c, 255.0f), 0.5f));
}

static inline u32 eval_pack_rgba(f32 r, f32 g, f32 b, f32 a) {
    return (u32)eval_f32_to_u8(r) |
           ((u32)eval_f32_to_u8(g) << 8) |
           ((u32)eval_f32_to_u8(b) << 16) |
           ((u32)eval_f32_to_u8(a) << 24);
}

#define EVAL_STACK_CAP 4096

void ir_eval_ex(const IrFunc* f, ShadeBatch* batch, u32 fuse_blend) {
    ASSERT(f != NULL && batch != NULL);
    if (f->count > EVAL_STACK_CAP) {
        PANIC("ir_eval: instruction count %u exceeds EVAL_STACK_CAP (%u)",
              f->count, (u32)EVAL_STACK_CAP);
    }

    EvalVal vals[EVAL_STACK_CAP];
    __builtin_memset(vals, 0, sizeof(EvalVal) * f->count);

    b32 has_deriv = FALSE;
    for (u32 i = 0; i < f->count; i++) {
        if (f->insts[i].op == IR_DERIV_X || f->insts[i].op == IR_DERIV_Y) has_deriv = TRUE;
    }

    u32 count = batch->count;
    for (u32 q = 0; q < count; q++) {
        __builtin_memset(vals, 0, sizeof(EvalVal) * f->count);
        EvalVal out_ch[4];
        __builtin_memset(out_ch, 0, sizeof(out_ch));
        /* Kontrol yığını: her IF seviyesi için
         *   is_uniform: koşul uniform mu
         *   u_cond:     uniform koşul değeri (0 veya 1)
         *   u_in_else:  ELSE dalında mıyız
         *   v_saved:    IF'e girerkenki aktif lane maskesi (4 bit)
         *   v_cond:     IF koşulunun lane maskesi (4 bit)
         */
        struct {
            b32 is_uniform;
            b32 u_cond;
            b32 u_in_else;
            u32 v_saved;
            u32 v_cond;
            u32 parent_active;
        } if_stk[64];
        u32 if_depth = 0;

        /* u_active: tüm kapsayıcı uniform IF dalları aktif mi?
         * v_mask:   varying kontrol akışı için aktif lane maskesi (başta quad.mask & 0xF) */
        b32 u_active = TRUE;
        u32 v_mask   = batch->quads ? (batch->quads[q].mask & 0xFu) : 0xFu;
        if (has_deriv) v_mask = 0xFu;   /* yardımcı lane'ler de kontrol akışına katılır (codegen ile aynı) */

        for (u32 pc = 0; pc < f->count; pc++) {
            const IrInst* in = &f->insts[pc];
            IrOp op = (IrOp)in->op;
            if (op == IR_NOP) continue;

            /* Kontrol akışı komutları aktiflik durumunu günceller */
            if (op == IR_IF_BEGIN) {
                ASSERT(if_depth < ARRAY_LEN(if_stk));
                b32 is_var = (f->insts[in->a].flags & IR_FLAG_VARYING) != 0;
                if_stk[if_depth].is_uniform    = !is_var;
                if_stk[if_depth].u_in_else     = FALSE;
                if_stk[if_depth].parent_active = u_active;
                if_stk[if_depth].v_saved       = v_mask;

                if (!is_var) {
                    b32 c = vals[in->a].u[0] != 0;
                    if_stk[if_depth].u_cond = c;
                    if_stk[if_depth].v_cond = c ? 0xFu : 0u;
                    u_active = u_active && c;
                } else {
                    u32 cm = 0;
                    for (u32 l = 0; l < 4; l++) {
                        if (vals[in->a].u[l] != 0) cm |= (1u << l);
                    }
                    if_stk[if_depth].u_cond = TRUE;
                    if_stk[if_depth].v_cond = cm;
                    v_mask = v_mask & cm;
                }
                if_depth++;
                continue;
            }
            if (op == IR_ELSE) {
                ASSERT(if_depth > 0);
                u32 top = if_depth - 1;
                if_stk[top].u_in_else = TRUE;
                if (if_stk[top].is_uniform) {
                    u_active = if_stk[top].parent_active && !if_stk[top].u_cond;
                } else {
                    v_mask = if_stk[top].v_saved & (~if_stk[top].v_cond & 0xFu);
                }
                continue;
            }
            if (op == IR_IF_END) {
                ASSERT(if_depth > 0);
                if_depth--;
                u_active = if_stk[if_depth].parent_active;
                v_mask   = if_stk[if_depth].v_saved;
                continue;
            }

            /* Eğer kapsayıcı bir uniform IF dalı devre dışıysa veya varying IF'te
             * hiçbir aktif lane yoksa (any_true(v_mask) == 0), bu bölge atlanır! */
            if (!u_active || v_mask == 0u) {
                continue;
            }

            EvalVal r = {{0, 0, 0, 0}};
            const EvalVal* va = (in->a != IR_NONE) ? &vals[in->a] : NULL;
            const EvalVal* vb = (in->b != IR_NONE) ? &vals[in->b] : NULL;
            const EvalVal* vc = (in->c != IR_NONE) ? &vals[in->c] : NULL;

            switch (op) {
            case IR_CONST_F32:
                for (u32 l = 0; l < 4; l++) r.f[l] = in->imm.f;
                break;
            case IR_CONST_I32:
                for (u32 l = 0; l < 4; l++) r.i[l] = in->imm.i;
                break;
            case IR_CONST_BOOL:
                for (u32 l = 0; l < 4; l++) r.u[l] = in->imm.i ? 0xFFFFFFFFu : 0u;
                break;
            case IR_UNIFORM_F32: {
                f32 v = 0.0f;
                if (batch->uniforms) {
                    __builtin_memcpy(&v, batch->uniforms + in->imm.u, 4);
                }
                for (u32 l = 0; l < 4; l++) r.f[l] = v;
            } break;
            case IR_UNIFORM_I32: {
                i32 v = 0;
                if (batch->uniforms) {
                    __builtin_memcpy(&v, batch->uniforms + in->imm.u, 4);
                }
                for (u32 l = 0; l < 4; l++) r.i[l] = v;
            } break;
            case IR_VARYING: {
                u32 v_idx = in->imm.u;
                const f32* base = batch->varyings + (v_idx * count + q) * 4;
                for (u32 l = 0; l < 4; l++) r.f[l] = base[l];
            } break;
            case IR_FRAG_X: {
                f32 qx = batch->quads ? (f32)batch->quads[q].x : 0.0f;
                r.f[0] = qx + 0.5f;
                r.f[1] = qx + 1.5f;
                r.f[2] = qx + 0.5f;
                r.f[3] = qx + 1.5f;
            } break;
            case IR_FRAG_Y: {
                f32 qy = batch->quads ? (f32)batch->quads[q].y : 0.0f;
                r.f[0] = qy + 0.5f;
                r.f[1] = qy + 0.5f;
                r.f[2] = qy + 1.5f;
                r.f[3] = qy + 1.5f;
            } break;

            case IR_FADD:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fadd(va->f[l], vb->f[l]);
                break;
            case IR_FSUB:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fsub(va->f[l], vb->f[l]);
                break;
            case IR_FMUL:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmul(va->f[l], vb->f[l]);
                break;
            case IR_FDIV:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fdiv(va->f[l], vb->f[l]);
                break;
            case IR_FMIN:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmin(va->f[l], vb->f[l]);
                break;
            case IR_FMAX:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmax(va->f[l], vb->f[l]);
                break;
            case IR_FNEG:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fneg(va->f[l]);
                break;
            case IR_FABS:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fabs(va->f[l]);
                break;
            case IR_FSQRT:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fsqrt(va->f[l]);
                break;
            case IR_FFLOOR:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_ffloor(va->f[l]);
                break;
            case IR_FCEIL:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fceil(va->f[l]);
                break;
            case IR_FTRUNC:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_ftrunc(va->f[l]);
                break;
            case IR_FNEAREST:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fnearest(va->f[l]);
                break;

            case IR_IADD:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_iadd(va->i[l], vb->i[l]);
                break;
            case IR_ISUB:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_isub(va->i[l], vb->i[l]);
                break;
            case IR_IMUL:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_imul(va->i[l], vb->i[l]);
                break;
            case IR_IAND:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] & vb->u[l];
                break;
            case IR_IOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] | vb->u[l];
                break;
            case IR_IXOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] ^ vb->u[l];
                break;
            case IR_ISHL:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishl(va->i[l], vb->i[l]);
                break;
            case IR_ISHR_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishr_s(va->i[l], vb->i[l]);
                break;
            case IR_ISHR_U:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishr_u(va->i[l], vb->i[l]);
                break;
            case IR_IMIN_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = va->i[l] < vb->i[l] ? va->i[l] : vb->i[l];
                break;
            case IR_IMAX_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = va->i[l] > vb->i[l] ? va->i[l] : vb->i[l];
                break;

            case IR_FEQ:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] == vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FNE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] != vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FLT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] < vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FLE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] <= vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FGT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] > vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FGE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] >= vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;

            case IR_IEQ:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] == vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_INE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] != vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_ILT_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] < vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_ILE_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] <= vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_IGT_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] > vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_IGE_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] >= vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;

            case IR_BAND:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->u[l] && vb->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_BOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->u[l] || vb->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_BNOT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (!va->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_SELECT:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] ? vb->u[l] : vc->u[l];
                break;

            case IR_F2I:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_trunc_sat_f32_s(va->f[l]);
                break;
            case IR_I2F:
                for (u32 l = 0; l < 4; l++) r.f[l] = (f32)va->i[l];
                break;
            case IR_BITCAST_F2I:
            case IR_BITCAST_I2F:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l];
                break;

            case IR_TEX_W: {
                i32 w = batch->textures ? (i32)batch->textures[in->imm.u].w : 0;
                for (u32 l = 0; l < 4; l++) r.i[l] = w;
            } break;
            case IR_TEX_H: {
                i32 h = batch->textures ? (i32)batch->textures[in->imm.u].h : 0;
                for (u32 l = 0; l < 4; l++) r.i[l] = h;
            } break;
            case IR_TEX_FLAGS: {
                i32 fl = batch->textures ? (i32)batch->textures[in->imm.u].flags : 0;
                for (u32 l = 0; l < 4; l++) r.i[l] = fl;
            } break;
            case IR_TEX_LEVELS: {
                i32 lv = batch->textures ? (i32)batch->textures[in->imm.u].levels : 0;
                for (u32 l = 0; l < 4; l++) r.i[l] = lv;
            } break;
            case IR_DERIV_X:
            case IR_DERIV_Y: {
                /* kaba türev: lane1 (x) veya lane2 (y) eksi lane0; uniform → 0 (codegen ile aynı) */
                u32 o = (op == IR_DERIV_X) ? 1u : 2u;
                f32 d = (in->flags & IR_FLAG_VARYING) ? wasm_fsub(va->f[o], va->f[0]) : 0.0f;
                for (u32 l = 0; l < 4; l++) r.f[l] = d;
            } break;
            case IR_TEXEL_LOAD: {
                const Texture* tex = &batch->textures[in->imm.u];
                for (u32 l = 0; l < 4; l++) {
                    u32 idx = va->u[l];
                    r.u[l] = tex->px[idx];
                }
            } break;

            case IR_DISCARD: {
                u32 disc = 0;
                for (u32 l = 0; l < 4; l++) {
                    if (va->u[l] != 0) disc |= (1u << l);
                }
                /* Yalnızca şu an aktif olan lane'ler discard yapabilir */
                disc &= v_mask;
                if (batch->quads) {
                    batch->quads[q].mask &= ~disc;
                }
            } break;

            case IR_OUTPUT: {
                u32 ch = in->imm.u;
                ASSERT(ch < 4u);
                out_ch[ch] = *va;
                if (fuse_blend == MTK_FUSE_NONE && batch->out_rgba) {
                    f32* dst = batch->out_rgba + (ch * count + q) * 4;
                    for (u32 l = 0; l < 4; l++) {
                        dst[l] = va->f[l];
                    }
                }
            } break;

            default:
                PANIC("ir_eval: unhandled opcode %u", (u32)op);
            }

            vals[pc] = r;
        }

        if (fuse_blend != MTK_FUSE_NONE && batch->fb_px && batch->quads) {
            u32 mask = batch->quads[q].mask & 0xFu;
            u32 qx   = batch->quads[q].x;
            u32 qy   = batch->quads[q].y;
            u32 fb_w = batch->fb_stride;
            const f32 inv255 = 1.0f / 255.0f;

            for (u32 l = 0; l < 4; l++) {
                if ((mask & (1u << l)) == 0) continue;
                u32* dst_ptr = batch->fb_px + (qy + (l >> 1u)) * fb_w + (qx + (l & 1u));
                u32 dst = *dst_ptr;

                f32 sr = eval_clamp01(out_ch[0].f[l]);
                f32 sg = eval_clamp01(out_ch[1].f[l]);
                f32 sb = eval_clamp01(out_ch[2].f[l]);
                f32 sa = eval_clamp01(out_ch[3].f[l]);

                if (fuse_blend == MTK_FUSE_REPLACE) {
                    *dst_ptr = eval_pack_rgba(sr, sg, sb, sa);
                } else if (fuse_blend == MTK_FUSE_ALPHA) {
                    f32 inv_sa = wasm_fsub(1.0f, sa);
                    f32 dr = wasm_fmul((f32)(dst & 0xFFu), inv255);
                    f32 dg = wasm_fmul((f32)((dst >> 8) & 0xFFu), inv255);
                    f32 db = wasm_fmul((f32)((dst >> 16) & 0xFFu), inv255);
                    f32 da = wasm_fmul((f32)((dst >> 24) & 0xFFu), inv255);

                    f32 rr = wasm_fadd(wasm_fmul(sr, sa), wasm_fmul(dr, inv_sa));
                    f32 rg = wasm_fadd(wasm_fmul(sg, sa), wasm_fmul(dg, inv_sa));
                    f32 rb = wasm_fadd(wasm_fmul(sb, sa), wasm_fmul(db, inv_sa));
                    f32 ra = wasm_fadd(sa, wasm_fmul(da, inv_sa));
                    *dst_ptr = eval_pack_rgba(rr, rg, rb, ra);
                } else if (fuse_blend == MTK_FUSE_ADD) {
                    f32 dr = wasm_fmul((f32)(dst & 0xFFu), inv255);
                    f32 dg = wasm_fmul((f32)((dst >> 8) & 0xFFu), inv255);
                    f32 db = wasm_fmul((f32)((dst >> 16) & 0xFFu), inv255);
                    f32 da = wasm_fmul((f32)((dst >> 24) & 0xFFu), inv255);

                    f32 rr = wasm_fadd(sr, dr);
                    f32 rg = wasm_fadd(sg, dg);
                    f32 rb = wasm_fadd(sb, db);
                    f32 ra = wasm_fadd(sa, da);
                    *dst_ptr = eval_pack_rgba(rr, rg, rb, ra);
                }
            }
        }
    }
}

void ir_eval(const IrFunc* f, ShadeBatch* batch) {
    ir_eval_ex(f, batch, MTK_FUSE_NONE);
}

void ir_eval_compute(const IrFunc* f, MtkComputeBatch* batch) {
    ASSERT(f != NULL && batch != NULL);
    if (f->count > EVAL_STACK_CAP) {
        PANIC("ir_eval_compute: instruction count %u exceeds EVAL_STACK_CAP (%u)",
              f->count, (u32)EVAL_STACK_CAP);
    }

    EvalVal vals[EVAL_STACK_CAP];
    u32 start = batch->start;
    u32 end   = batch->start + batch->count;
    u32 limit = (batch->total_count > 0) ? batch->total_count : end;
    if (end > limit) end = limit;

    for (u32 base_gid = start; base_gid < end; base_gid += 4u) {
        __builtin_memset(vals, 0, sizeof(EvalVal) * f->count);

        struct {
            b32 is_uniform;
            b32 u_cond;
            b32 u_in_else;
            u32 v_saved;
            u32 v_cond;
            u32 parent_active;
        } if_stk[64];
        u32 if_depth = 0;

        b32 u_active = TRUE;
        u32 init_mask = 0;
        for (u32 l = 0; l < 4; l++) {
            if (base_gid + l < end) init_mask |= (1u << l);
        }
        u32 v_mask = init_mask;

        for (u32 pc = 0; pc < f->count; pc++) {
            const IrInst* in = &f->insts[pc];
            IrOp op = (IrOp)in->op;
            if (op == IR_NOP) continue;

            if (op == IR_IF_BEGIN) {
                ASSERT(if_depth < ARRAY_LEN(if_stk));
                b32 is_var = (f->insts[in->a].flags & IR_FLAG_VARYING) != 0;
                if_stk[if_depth].is_uniform    = !is_var;
                if_stk[if_depth].u_in_else     = FALSE;
                if_stk[if_depth].parent_active = u_active;
                if_stk[if_depth].v_saved       = v_mask;

                if (!is_var) {
                    b32 c = vals[in->a].u[0] != 0;
                    if_stk[if_depth].u_cond = c;
                    if_stk[if_depth].v_cond = c ? 0xFu : 0u;
                    u_active = u_active && c;
                } else {
                    u32 cm = 0;
                    for (u32 l = 0; l < 4; l++) {
                        if (vals[in->a].u[l] != 0) cm |= (1u << l);
                    }
                    if_stk[if_depth].u_cond = TRUE;
                    if_stk[if_depth].v_cond = cm;
                    v_mask = v_mask & cm;
                }
                if_depth++;
                continue;
            }
            if (op == IR_ELSE) {
                ASSERT(if_depth > 0);
                u32 top = if_depth - 1;
                if_stk[top].u_in_else = TRUE;
                if (if_stk[top].is_uniform) {
                    u_active = if_stk[top].parent_active && !if_stk[top].u_cond;
                } else {
                    v_mask = if_stk[top].v_saved & (~if_stk[top].v_cond & 0xFu);
                }
                continue;
            }
            if (op == IR_IF_END) {
                ASSERT(if_depth > 0);
                if_depth--;
                u_active = if_stk[if_depth].parent_active;
                v_mask   = if_stk[if_depth].v_saved;
                continue;
            }

            if (!u_active || v_mask == 0u) {
                continue;
            }

            EvalVal r = {{0, 0, 0, 0}};
            const EvalVal* va = (in->a != IR_NONE) ? &vals[in->a] : NULL;
            const EvalVal* vb = (in->b != IR_NONE) ? &vals[in->b] : NULL;
            const EvalVal* vc = (in->c != IR_NONE) ? &vals[in->c] : NULL;

            switch (op) {
            case IR_CONST_F32:
                for (u32 l = 0; l < 4; l++) r.f[l] = in->imm.f;
                break;
            case IR_CONST_I32:
                for (u32 l = 0; l < 4; l++) r.i[l] = in->imm.i;
                break;
            case IR_CONST_BOOL:
                for (u32 l = 0; l < 4; l++) r.u[l] = in->imm.i ? 0xFFFFFFFFu : 0u;
                break;
            case IR_UNIFORM_F32: {
                f32 v = 0.0f;
                if (batch->uniforms) {
                    __builtin_memcpy(&v, batch->uniforms + in->imm.u, 4);
                }
                for (u32 l = 0; l < 4; l++) r.f[l] = v;
            } break;
            case IR_UNIFORM_I32: {
                i32 v = 0;
                if (batch->uniforms) {
                    __builtin_memcpy(&v, batch->uniforms + in->imm.u, 4);
                }
                for (u32 l = 0; l < 4; l++) r.i[l] = v;
            } break;
            case IR_GLOBAL_ID:
                for (u32 l = 0; l < 4; l++) r.u[l] = base_gid + l;
                break;

            case IR_BUF_LOAD: {
                u32 slot    = IR_BUF_SLOT(in->imm.u);
                u32 comp    = IR_BUF_COMP(in->imm.u);
                b32 is_vec4 = IR_BUF_IS_VEC4(in->imm.u);
                const u32* buf = (slot < COMPUTE_MAX_BUFFERS) ? (const u32*)batch->buffers[slot] : NULL;
                for (u32 l = 0; l < 4; l++) {
                    if ((v_mask & (1u << l)) && buf) {
                        u32 elem_idx = va->u[l];
                        u32 word_idx = is_vec4 ? (elem_idx * 4u + comp) : elem_idx;
                        r.u[l] = buf[word_idx];
                    }
                }
            } break;

            case IR_BUF_STORE: {
                u32 slot    = IR_BUF_SLOT(in->imm.u);
                u32 comp    = IR_BUF_COMP(in->imm.u);
                b32 is_vec4 = IR_BUF_IS_VEC4(in->imm.u);
                u32* buf = (slot < COMPUTE_MAX_BUFFERS) ? (u32*)batch->buffers[slot] : NULL;
                for (u32 l = 0; l < 4; l++) {
                    b32 ok = ((v_mask & (1u << l)) != 0) && (vc == NULL || vc->u[l] != 0);
                    if (ok && buf) {
                        u32 elem_idx = va->u[l];
                        u32 word_idx = is_vec4 ? (elem_idx * 4u + comp) : elem_idx;
                        buf[word_idx] = vb->u[l];
                    }
                }
            } break;

            case IR_ATOMIC_ADD: {
                u32 slot = in->imm.u;
                u32* buf = (slot < COMPUTE_MAX_BUFFERS) ? (u32*)batch->buffers[slot] : NULL;
                for (u32 l = 0; l < 4; l++) {
                    b32 ok = ((v_mask & (1u << l)) != 0) && (vc == NULL || vc->u[l] != 0);
                    if (ok && buf) {
                        r.u[l] = __atomic_fetch_add(buf + va->u[l], vb->u[l], __ATOMIC_SEQ_CST);
                    }
                }
            } break;

            case IR_FADD:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fadd(va->f[l], vb->f[l]);
                break;
            case IR_FSUB:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fsub(va->f[l], vb->f[l]);
                break;
            case IR_FMUL:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmul(va->f[l], vb->f[l]);
                break;
            case IR_FDIV:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fdiv(va->f[l], vb->f[l]);
                break;
            case IR_FMIN:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmin(va->f[l], vb->f[l]);
                break;
            case IR_FMAX:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fmax(va->f[l], vb->f[l]);
                break;
            case IR_FNEG:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fneg(va->f[l]);
                break;
            case IR_FABS:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fabs(va->f[l]);
                break;
            case IR_FSQRT:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fsqrt(va->f[l]);
                break;
            case IR_FFLOOR:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_ffloor(va->f[l]);
                break;
            case IR_FCEIL:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fceil(va->f[l]);
                break;
            case IR_FTRUNC:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_ftrunc(va->f[l]);
                break;
            case IR_FNEAREST:
                for (u32 l = 0; l < 4; l++) r.f[l] = wasm_fnearest(va->f[l]);
                break;

            case IR_IADD:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_iadd(va->i[l], vb->i[l]);
                break;
            case IR_ISUB:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_isub(va->i[l], vb->i[l]);
                break;
            case IR_IMUL:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_imul(va->i[l], vb->i[l]);
                break;
            case IR_IAND:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] & vb->u[l];
                break;
            case IR_IOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] | vb->u[l];
                break;
            case IR_IXOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] ^ vb->u[l];
                break;
            case IR_ISHL:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishl(va->i[l], vb->i[l]);
                break;
            case IR_ISHR_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishr_s(va->i[l], vb->i[l]);
                break;
            case IR_ISHR_U:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_ishr_u(va->i[l], vb->i[l]);
                break;
            case IR_IMIN_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = va->i[l] < vb->i[l] ? va->i[l] : vb->i[l];
                break;
            case IR_IMAX_S:
                for (u32 l = 0; l < 4; l++) r.i[l] = va->i[l] > vb->i[l] ? va->i[l] : vb->i[l];
                break;

            case IR_FEQ:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] == vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FNE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] != vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FLT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] < vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FLE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] <= vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FGT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] > vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_FGE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->f[l] >= vb->f[l]) ? 0xFFFFFFFFu : 0u;
                break;

            case IR_IEQ:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] == vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_INE:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] != vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_ILT_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] < vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_ILE_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] <= vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_IGT_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] > vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_IGE_S:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->i[l] >= vb->i[l]) ? 0xFFFFFFFFu : 0u;
                break;

            case IR_BAND:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->u[l] && vb->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_BOR:
                for (u32 l = 0; l < 4; l++) r.u[l] = (va->u[l] || vb->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_BNOT:
                for (u32 l = 0; l < 4; l++) r.u[l] = (!va->u[l]) ? 0xFFFFFFFFu : 0u;
                break;
            case IR_SELECT:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l] ? vb->u[l] : vc->u[l];
                break;

            case IR_F2I:
                for (u32 l = 0; l < 4; l++) r.i[l] = wasm_trunc_sat_f32_s(va->f[l]);
                break;
            case IR_I2F:
                for (u32 l = 0; l < 4; l++) r.f[l] = (f32)va->i[l];
                break;
            case IR_BITCAST_F2I:
            case IR_BITCAST_I2F:
                for (u32 l = 0; l < 4; l++) r.u[l] = va->u[l];
                break;

            default:
                PANIC("ir_eval_compute: unhandled opcode %u", (u32)op);
            }

            vals[pc] = r;
        }
    }
}
