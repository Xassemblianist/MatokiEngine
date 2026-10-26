/* matoki/ir.c — Matoki ara dili (IR) kurucuları, doğrulayıcı ve yazdırıcı.
 * Tüm kurucular tip ve tekdüzelik (uniform/varying) çıkarımını otomatik yapar.
 * ir_emit_sample doku örneklemesini (nearest ve bilinear, wrap ve clamp) temel IR
 * komutlarına açar. ir_verify SSA, tip ve kontrol akışı kurallarını denetler.
 */
#include "matoki/ir.h"
#include "raster/shade_abi.h"
#include "core/log.h"

static u32 ir_push(IrFunc* f, IrInst inst) {
    if (f->count >= f->cap) {
        PANIC("ir_push: capacity overflow (%u >= %u)", f->count, f->cap);
    }
    u32 id = f->count++;
    f->insts[id] = inst;
    return id;
}

static const IrInst* ir_get(const IrFunc* f, u32 id) {
    if (id >= f->count) {
        PANIC("ir_get: invalid operand id %u (count=%u)", id, f->count);
    }
    const IrInst* inst = &f->insts[id];
    if (inst->op == IR_NOP || inst->type == IR_T_VOID) {
        PANIC("ir_get: operand %%%u is not a value instruction (op=%u, type=%u)",
              id, (u32)inst->op, (u32)inst->type);
    }
    return inst;
}

void ir_init(IrFunc* f, Arena* arena, u32 cap) {
    ASSERT(f != NULL && arena != NULL && cap > 0);
    f->insts         = ARENA_PUSH_ARRAY(arena, IrInst, cap);
    f->count         = 0;
    f->cap           = cap;
    f->uniform_bytes = 0;
    f->varying_count = 0;
    f->texture_count = 0;
}

u32 ir_const_f32(IrFunc* f, f32 v) {
    IrInst inst = {0};
    inst.op    = IR_CONST_F32;
    inst.type  = IR_T_F32;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.f = v;
    return ir_push(f, inst);
}

u32 ir_const_i32(IrFunc* f, i32 v) {
    IrInst inst = {0};
    inst.op    = IR_CONST_I32;
    inst.type  = IR_T_I32;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.i = v;
    return ir_push(f, inst);
}

u32 ir_const_bool(IrFunc* f, b32 v) {
    IrInst inst = {0};
    inst.op    = IR_CONST_BOOL;
    inst.type  = IR_T_BOOL;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.i = v ? 1 : 0;
    return ir_push(f, inst);
}

u32 ir_uniform(IrFunc* f, IrType t, u32 byte_offset) {
    IrInst inst = {0};
    if (t == IR_T_F32) {
        inst.op = IR_UNIFORM_F32;
    } else if (t == IR_T_I32) {
        inst.op = IR_UNIFORM_I32;
    } else {
        PANIC("ir_uniform: invalid type %u", (u32)t);
    }
    inst.type  = (u8)t;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.u = byte_offset;
    return ir_push(f, inst);
}

u32 ir_varying(IrFunc* f, u32 index) {
    if (index >= SHADE_MAX_VARYINGS) {
        PANIC("ir_varying: index %u >= SHADE_MAX_VARYINGS", index);
    }
    IrInst inst = {0};
    inst.op    = IR_VARYING;
    inst.type  = IR_T_F32;
    inst.flags = IR_FLAG_VARYING;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.u = index;
    return ir_push(f, inst);
}

u32 ir_frag(IrFunc* f, b32 y) {
    IrInst inst = {0};
    inst.op    = y ? IR_FRAG_Y : IR_FRAG_X;
    inst.type  = IR_T_F32;
    inst.flags = IR_FLAG_VARYING;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    return ir_push(f, inst);
}

u32 ir_op1(IrFunc* f, IrOp op, u32 a) {
    const IrInst* ia = ir_get(f, a);
    IrType exp_in = IR_T_VOID;
    IrType res_t  = IR_T_VOID;

    if (op == IR_DERIV_X || op == IR_DERIV_Y) {
        if (ia->type != IR_T_F32) {
            PANIC("ir_op1: deriv operand must be F32, got %u", (u32)ia->type);
        }
        if (!(ia->flags & IR_FLAG_VARYING)) {
            return ir_const_f32(f, 0.0f);   /* uniform'un türevi 0 */
        }
    }

    switch (op) {
    case IR_DERIV_X:
    case IR_DERIV_Y:
    case IR_FNEG:
    case IR_FABS:
    case IR_FSQRT:
    case IR_FFLOOR:
    case IR_FCEIL:
    case IR_FTRUNC:
    case IR_FNEAREST:
        exp_in = IR_T_F32;
        res_t  = IR_T_F32;
        break;
    case IR_BNOT:
        exp_in = IR_T_BOOL;
        res_t  = IR_T_BOOL;
        break;
    case IR_F2I:
    case IR_BITCAST_F2I:
        exp_in = IR_T_F32;
        res_t  = IR_T_I32;
        break;
    case IR_I2F:
    case IR_BITCAST_I2F:
        exp_in = IR_T_I32;
        res_t  = IR_T_F32;
        break;
    default:
        PANIC("ir_op1: invalid unary op %u", (u32)op);
    }

    if ((IrType)ia->type != exp_in) {
        PANIC("ir_op1: type mismatch for op %u: expected %u, got %u",
              (u32)op, (u32)exp_in, (u32)ia->type);
    }

    IrInst inst = {0};
    inst.op    = (u16)op;
    inst.type  = (u8)res_t;
    inst.flags = ia->flags & IR_FLAG_VARYING;
    inst.a     = a;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    return ir_push(f, inst);
}

u32 ir_op2(IrFunc* f, IrOp op, u32 a, u32 b) {
    const IrInst* ia = ir_get(f, a);
    const IrInst* ib = ir_get(f, b);
    IrType exp_in = IR_T_VOID;
    IrType res_t  = IR_T_VOID;

    switch (op) {
    case IR_FADD:
    case IR_FSUB:
    case IR_FMUL:
    case IR_FDIV:
    case IR_FMIN:
    case IR_FMAX:
        exp_in = IR_T_F32;
        res_t  = IR_T_F32;
        break;
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
        exp_in = IR_T_I32;
        res_t  = IR_T_I32;
        break;
    case IR_FEQ:
    case IR_FNE:
    case IR_FLT:
    case IR_FLE:
    case IR_FGT:
    case IR_FGE:
        exp_in = IR_T_F32;
        res_t  = IR_T_BOOL;
        break;
    case IR_IEQ:
    case IR_INE:
    case IR_ILT_S:
    case IR_ILE_S:
    case IR_IGT_S:
    case IR_IGE_S:
        exp_in = IR_T_I32;
        res_t  = IR_T_BOOL;
        break;
    case IR_BAND:
    case IR_BOR:
        exp_in = IR_T_BOOL;
        res_t  = IR_T_BOOL;
        break;
    default:
        PANIC("ir_op2: invalid binary op %u", (u32)op);
    }

    if ((IrType)ia->type != exp_in || (IrType)ib->type != exp_in) {
        PANIC("ir_op2: type mismatch for op %u: expected %u, got (%u, %u)",
              (u32)op, (u32)exp_in, (u32)ia->type, (u32)ib->type);
    }

    IrInst inst = {0};
    inst.op    = (u16)op;
    inst.type  = (u8)res_t;
    inst.flags = (ia->flags | ib->flags) & IR_FLAG_VARYING;
    inst.a     = a;
    inst.b     = b;
    inst.c     = IR_NONE;
    return ir_push(f, inst);
}

u32 ir_select(IrFunc* f, u32 cond, u32 t, u32 e) {
    const IrInst* ic = ir_get(f, cond);
    const IrInst* it = ir_get(f, t);
    const IrInst* ie = ir_get(f, e);

    if (ic->type != IR_T_BOOL) {
        PANIC("ir_select: cond must be BOOL, got %u", (u32)ic->type);
    }
    if (it->type != ie->type || it->type == IR_T_VOID) {
        PANIC("ir_select: branch type mismatch (%u vs %u)", (u32)it->type, (u32)ie->type);
    }

    IrInst inst = {0};
    inst.op    = IR_SELECT;
    inst.type  = it->type;
    inst.flags = (ic->flags | it->flags | ie->flags) & IR_FLAG_VARYING;
    inst.a     = cond;
    inst.b     = t;
    inst.c     = e;
    return ir_push(f, inst);
}

u32 ir_tex_info(IrFunc* f, IrOp op, u32 slot) {
    if (op != IR_TEX_W && op != IR_TEX_H && op != IR_TEX_FLAGS && op != IR_TEX_LEVELS) {
        PANIC("ir_tex_info: invalid op %u", (u32)op);
    }
    if (slot >= SHADE_MAX_TEXTURES) {
        PANIC("ir_tex_info: slot %u >= SHADE_MAX_TEXTURES", slot);
    }
    IrInst inst = {0};
    inst.op    = (u16)op;
    inst.type  = IR_T_I32;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.u = slot;
    return ir_push(f, inst);
}

u32 ir_texel_load(IrFunc* f, u32 slot, u32 index) {
    if (slot >= SHADE_MAX_TEXTURES) {
        PANIC("ir_texel_load: slot %u >= SHADE_MAX_TEXTURES", slot);
    }
    const IrInst* ii = ir_get(f, index);
    if (ii->type != IR_T_I32) {
        PANIC("ir_texel_load: index must be I32, got %u", (u32)ii->type);
    }
    IrInst inst = {0};
    inst.op    = IR_TEXEL_LOAD;
    inst.type  = IR_T_I32;
    inst.flags = ii->flags & IR_FLAG_VARYING;
    inst.a     = index;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.u = slot;
    return ir_push(f, inst);
}

void ir_if_begin(IrFunc* f, u32 cond) {
    const IrInst* ic = ir_get(f, cond);
    if (ic->type != IR_T_BOOL) {
        PANIC("ir_if_begin: cond must be BOOL, got %u", (u32)ic->type);
    }
    IrInst inst = {0};
    inst.op    = IR_IF_BEGIN;
    inst.type  = IR_T_VOID;
    inst.flags = ic->flags & IR_FLAG_VARYING;
    inst.a     = cond;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    ir_push(f, inst);
}

void ir_else(IrFunc* f) {
    IrInst inst = {0};
    inst.op    = IR_ELSE;
    inst.type  = IR_T_VOID;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    ir_push(f, inst);
}

void ir_if_end(IrFunc* f) {
    IrInst inst = {0};
    inst.op    = IR_IF_END;
    inst.type  = IR_T_VOID;
    inst.flags = 0;
    inst.a     = IR_NONE;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    ir_push(f, inst);
}

void ir_discard(IrFunc* f, u32 mask) {
    const IrInst* im = ir_get(f, mask);
    if (im->type != IR_T_BOOL) {
        PANIC("ir_discard: mask must be BOOL, got %u", (u32)im->type);
    }
    IrInst inst = {0};
    inst.op    = IR_DISCARD;
    inst.type  = IR_T_VOID;
    inst.flags = im->flags & IR_FLAG_VARYING;
    inst.a     = mask;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    ir_push(f, inst);
}

void ir_output(IrFunc* f, u32 channel, u32 value) {
    if (channel >= 4) {
        PANIC("ir_output: invalid channel %u", channel);
    }
    const IrInst* iv = ir_get(f, value);
    if (iv->type != IR_T_F32) {
        PANIC("ir_output: value must be F32, got %u", (u32)iv->type);
    }
    IrInst inst = {0};
    inst.op    = IR_OUTPUT;
    inst.type  = IR_T_VOID;
    inst.flags = iv->flags & IR_FLAG_VARYING;
    inst.a     = value;
    inst.b     = IR_NONE;
    inst.c     = IR_NONE;
    inst.imm.u = channel;
    ir_push(f, inst);
}

/* Yardımcı: tek bir (tx, ty) koordinat çiftini wrap veya clamp ile sarıp texel yükler ve RGBA açar */
static void ir_unpack_texel(IrFunc* f, u32 slot, u32 tx, u32 ty, u32 w_i, u32 w_m1, u32 h_m1,
                            u32 base, u32 zero_i, u32 c255_i, u32 s8_i, u32 s16_i, u32 s24_i,
                            u32 inv255_f, u32 flags, u32 out_rgba[4]) {
    u32 wx, wy;
    if (flags & TEX_FLAG_CLAMP) {
        wx = ir_op2(f, IR_IMIN_S, ir_op2(f, IR_IMAX_S, tx, zero_i), w_m1);
        wy = ir_op2(f, IR_IMIN_S, ir_op2(f, IR_IMAX_S, ty, zero_i), h_m1);
    } else {
        wx = ir_op2(f, IR_IAND, tx, w_m1);
        wy = ir_op2(f, IR_IAND, ty, h_m1);
    }
    u32 idx = ir_op2(f, IR_IADD, ir_op2(f, IR_IMUL, wy, w_i), wx);
    if (base != IR_NONE) idx = ir_op2(f, IR_IADD, base, idx);
    u32 px  = ir_texel_load(f, slot, idx);

    u32 r_i = ir_op2(f, IR_IAND, px, c255_i);
    u32 g_i = ir_op2(f, IR_IAND, ir_op2(f, IR_ISHR_U, px, s8_i), c255_i);
    u32 b_i = ir_op2(f, IR_IAND, ir_op2(f, IR_ISHR_U, px, s16_i), c255_i);
    u32 a_i = ir_op2(f, IR_ISHR_U, px, s24_i);

    out_rgba[0] = ir_op2(f, IR_FMUL, ir_op1(f, IR_I2F, r_i), inv255_f);
    out_rgba[1] = ir_op2(f, IR_FMUL, ir_op1(f, IR_I2F, g_i), inv255_f);
    out_rgba[2] = ir_op2(f, IR_FMUL, ir_op1(f, IR_I2F, b_i), inv255_f);
    out_rgba[3] = ir_op2(f, IR_FMUL, ir_op1(f, IR_I2F, a_i), inv255_f);
}

/* Ortak örnekleme gövdesi. level == IR_NONE: seviye 0 (eski yol, komut dizisi değişmedi).
 * level != IR_NONE: wk = w >> level, hk = h >> level, taban = (4wh - (4wh >> 2*level)) / 3. */
static void ir_emit_sample_impl(IrFunc* f, u32 slot, u32 u, u32 v, u32 level, u32 flags, u32 out_rgba[4]) {
    u32 w_i      = ir_tex_info(f, IR_TEX_W, slot);
    u32 h_i      = ir_tex_info(f, IR_TEX_H, slot);
    u32 base     = IR_NONE;
    if (level != IR_NONE) {
        u32 two_i  = ir_const_i32(f, 2);
        u32 wh4    = ir_op2(f, IR_ISHL, ir_op2(f, IR_IMUL, w_i, h_i), two_i);
        u32 tail   = ir_op2(f, IR_ISHR_U, wh4, ir_op2(f, IR_ISHL, level, ir_const_i32(f, 1)));
        /* 3'e tam bölme = 3'ün mod 2^32 tersiyle çarpma */
        base = ir_op2(f, IR_IMUL, ir_op2(f, IR_ISUB, wh4, tail), ir_const_i32(f, (i32)0xAAAAAAABu));
        w_i  = ir_op2(f, IR_ISHR_S, w_i, level);
        h_i  = ir_op2(f, IR_ISHR_S, h_i, level);
    }
    u32 w_f      = ir_op1(f, IR_I2F, w_i);
    u32 h_f      = ir_op1(f, IR_I2F, h_i);
    u32 one_i    = ir_const_i32(f, 1);
    u32 zero_i   = ir_const_i32(f, 0);
    u32 w_m1     = ir_op2(f, IR_ISUB, w_i, one_i);
    u32 h_m1     = ir_op2(f, IR_ISUB, h_i, one_i);
    u32 c255_i   = ir_const_i32(f, 255);
    u32 s8_i     = ir_const_i32(f, 8);
    u32 s16_i    = ir_const_i32(f, 16);
    u32 s24_i    = ir_const_i32(f, 24);
    u32 inv255_f = ir_const_f32(f, 1.0f / 255.0f);

    if (!(flags & TEX_FLAG_BILINEAR)) {
        u32 uf = ir_op1(f, IR_FFLOOR, ir_op2(f, IR_FMUL, u, w_f));
        u32 vf = ir_op1(f, IR_FFLOOR, ir_op2(f, IR_FMUL, v, h_f));
        u32 tx = ir_op1(f, IR_F2I, uf);
        u32 ty = ir_op1(f, IR_F2I, vf);
        ir_unpack_texel(f, slot, tx, ty, w_i, w_m1, h_m1, base,
                        zero_i, c255_i, s8_i, s16_i, s24_i, inv255_f, flags, out_rgba);
    } else {
        u32 half_f = ir_const_f32(f, 0.5f);
        u32 fu     = ir_op2(f, IR_FSUB, ir_op2(f, IR_FMUL, u, w_f), half_f);
        u32 fv     = ir_op2(f, IR_FSUB, ir_op2(f, IR_FMUL, v, h_f), half_f);
        u32 x0_f   = ir_op1(f, IR_FFLOOR, fu);
        u32 y0_f   = ir_op1(f, IR_FFLOOR, fv);
        u32 ax     = ir_op2(f, IR_FSUB, fu, x0_f);
        u32 ay     = ir_op2(f, IR_FSUB, fv, y0_f);

        u32 x0 = ir_op1(f, IR_F2I, x0_f);
        u32 y0 = ir_op1(f, IR_F2I, y0_f);
        u32 x1 = ir_op2(f, IR_IADD, x0, one_i);
        u32 y1 = ir_op2(f, IR_IADD, y0, one_i);

        u32 t00[4], t10[4], t01[4], t11[4];
        ir_unpack_texel(f, slot, x0, y0, w_i, w_m1, h_m1, base,
                        zero_i, c255_i, s8_i, s16_i, s24_i, inv255_f, flags, t00);
        ir_unpack_texel(f, slot, x1, y0, w_i, w_m1, h_m1, base,
                        zero_i, c255_i, s8_i, s16_i, s24_i, inv255_f, flags, t10);
        ir_unpack_texel(f, slot, x0, y1, w_i, w_m1, h_m1, base,
                        zero_i, c255_i, s8_i, s16_i, s24_i, inv255_f, flags, t01);
        ir_unpack_texel(f, slot, x1, y1, w_i, w_m1, h_m1, base,
                        zero_i, c255_i, s8_i, s16_i, s24_i, inv255_f, flags, t11);

        for (u32 c = 0; c < 4; c++) {
            u32 top = ir_op2(f, IR_FADD, t00[c], ir_op2(f, IR_FMUL, ir_op2(f, IR_FSUB, t10[c], t00[c]), ax));
            u32 bot = ir_op2(f, IR_FADD, t01[c], ir_op2(f, IR_FMUL, ir_op2(f, IR_FSUB, t11[c], t01[c]), ax));
            out_rgba[c] = ir_op2(f, IR_FADD, top, ir_op2(f, IR_FMUL, ir_op2(f, IR_FSUB, bot, top), ay));
        }
    }
}

/* Seviyeyi [0, levels-1] aralığına kırpar (levels 0 okunursa 1) */
static u32 ir_clamp_level(IrFunc* f, u32 slot, u32 level) {
    u32 one_i  = ir_const_i32(f, 1);
    u32 levels = ir_op2(f, IR_IMAX_S, ir_tex_info(f, IR_TEX_LEVELS, slot), one_i);
    u32 lvl    = ir_op2(f, IR_IMIN_S, level, ir_op2(f, IR_ISUB, levels, one_i));
    return ir_op2(f, IR_IMAX_S, lvl, ir_const_i32(f, 0));
}

u32 ir_emit_mip_level(IrFunc* f, u32 slot, u32 u, u32 v) {
    u32 w_f  = ir_op1(f, IR_I2F, ir_tex_info(f, IR_TEX_W, slot));
    u32 h_f  = ir_op1(f, IR_I2F, ir_tex_info(f, IR_TEX_H, slot));
    u32 dudx = ir_op2(f, IR_FMUL, ir_op1(f, IR_DERIV_X, u), w_f);
    u32 dvdx = ir_op2(f, IR_FMUL, ir_op1(f, IR_DERIV_X, v), h_f);
    u32 dudy = ir_op2(f, IR_FMUL, ir_op1(f, IR_DERIV_Y, u), w_f);
    u32 dvdy = ir_op2(f, IR_FMUL, ir_op1(f, IR_DERIV_Y, v), h_f);
    u32 px2  = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, dudx, dudx), ir_op2(f, IR_FMUL, dvdx, dvdx));
    u32 py2  = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, dudy, dudy), ir_op2(f, IR_FMUL, dvdy, dvdy));
    u32 rho2 = ir_op2(f, IR_FMAX, px2, py2);
    u32 bits = ir_op1(f, IR_BITCAST_F2I, rho2);
    u32 e    = ir_op2(f, IR_ISUB, ir_op2(f, IR_ISHR_U, bits, ir_const_i32(f, 23)), ir_const_i32(f, 127));
    u32 lvl  = ir_op2(f, IR_ISHR_S, e, ir_const_i32(f, 1));
    u32 ok   = ir_op2(f, IR_FGE, rho2, ir_const_f32(f, 1.0f));   /* NaN → yanlış → seviye 0 */
    lvl = ir_select(f, ok, lvl, ir_const_i32(f, 0));
    return ir_clamp_level(f, slot, lvl);
}

void ir_emit_sample(IrFunc* f, u32 slot, u32 u, u32 v, u32 flags, u32 out_rgba[4]) {
    if (flags & TEX_FLAG_MIPMAP) {
        u32 level = ir_emit_mip_level(f, slot, u, v);
        ir_emit_sample_impl(f, slot, u, v, level, flags, out_rgba);
    } else {
        ir_emit_sample_impl(f, slot, u, v, IR_NONE, flags, out_rgba);
    }
}

void ir_emit_sample_level(IrFunc* f, u32 slot, u32 u, u32 v, u32 level, u32 flags, u32 out_rgba[4]) {
    ir_emit_sample_impl(f, slot, u, v, ir_clamp_level(f, slot, level), flags & ~TEX_FLAG_MIPMAP, out_rgba);
}

static b32 verify_fail(char* err, u32 err_cap, const char* fmt, ...) {
    if (err && err_cap > 0) {
        VaList ap;
        VA_START(ap, fmt);
        fmt_vformat(err, err_cap, fmt, ap);
        VA_END(ap);
    }
    return FALSE;
}

static b32 check_operand(const IrFunc* f, u32 inst_idx, u32 op_id, IrType exp_t,
                         char* err, u32 err_cap) {
    if (op_id >= inst_idx) {
        return verify_fail(err, err_cap, "inst %%%u: operand %%%u not defined before use",
                           inst_idx, op_id);
    }
    const IrInst* def = &f->insts[op_id];
    if (def->op == IR_NOP || def->type == IR_T_VOID) {
        return verify_fail(err, err_cap, "inst %%%u: operand %%%u is void or NOP",
                           inst_idx, op_id);
    }
    if (exp_t != IR_T_VOID && (IrType)def->type != exp_t) {
        return verify_fail(err, err_cap, "inst %%%u: operand %%%u type %u != expected %u",
                           inst_idx, op_id, (u32)def->type, (u32)exp_t);
    }
    return TRUE;
}

b32 ir_verify(const IrFunc* f, char* err, u32 err_cap) {
    if (err && err_cap > 0) err[0] = '\0';
    if (!f || (!f->insts && f->count > 0)) {
        return verify_fail(err, err_cap, "null IrFunc");
    }

    b32 if_has_else[64];
    u32 if_depth = 0;
    u32 out_mask = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        /* OUTPUT sonlarında olmalı: bir OUTPUT gördükten sonra yalnızca OUTPUT veya NOP gelebilir */
        if (out_mask != 0 && op != IR_OUTPUT) {
            return verify_fail(err, err_cap, "inst %%%u: non-OUTPUT instruction after OUTPUT", i);
        }

        IrType exp_t   = IR_T_VOID;
        u8     exp_var = 0;

        switch (op) {
        case IR_CONST_F32:
            exp_t = IR_T_F32;
            break;
        case IR_CONST_I32:
            exp_t = IR_T_I32;
            break;
        case IR_CONST_BOOL:
            exp_t = IR_T_BOOL;
            break;
        case IR_UNIFORM_F32:
            exp_t = IR_T_F32;
            break;
        case IR_UNIFORM_I32:
            exp_t = IR_T_I32;
            break;
        case IR_VARYING:
            if (in->imm.u >= SHADE_MAX_VARYINGS) {
                return verify_fail(err, err_cap, "inst %%%u: varying index %u out of range", i, in->imm.u);
            }
            exp_t   = IR_T_F32;
            exp_var = IR_FLAG_VARYING;
            break;
        case IR_FRAG_X:
        case IR_FRAG_Y:
            exp_t   = IR_T_F32;
            exp_var = IR_FLAG_VARYING;
            break;

        case IR_FNEG:
        case IR_FABS:
        case IR_FSQRT:
        case IR_FFLOOR:
        case IR_FCEIL:
        case IR_FTRUNC:
        case IR_FNEAREST:
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            exp_t   = IR_T_F32;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_BNOT:
            if (!check_operand(f, i, in->a, IR_T_BOOL, err, err_cap)) return FALSE;
            exp_t   = IR_T_BOOL;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_F2I:
        case IR_BITCAST_F2I:
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            exp_t   = IR_T_I32;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_I2F:
        case IR_BITCAST_I2F:
            if (!check_operand(f, i, in->a, IR_T_I32, err, err_cap)) return FALSE;
            exp_t   = IR_T_F32;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_FADD:
        case IR_FSUB:
        case IR_FMUL:
        case IR_FDIV:
        case IR_FMIN:
        case IR_FMAX:
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_F32, err, err_cap)) return FALSE;
            exp_t   = IR_T_F32;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags) & IR_FLAG_VARYING;
            break;

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
            if (!check_operand(f, i, in->a, IR_T_I32, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_I32, err, err_cap)) return FALSE;
            exp_t   = IR_T_I32;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags) & IR_FLAG_VARYING;
            break;

        case IR_FEQ:
        case IR_FNE:
        case IR_FLT:
        case IR_FLE:
        case IR_FGT:
        case IR_FGE:
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_F32, err, err_cap)) return FALSE;
            exp_t   = IR_T_BOOL;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags) & IR_FLAG_VARYING;
            break;

        case IR_IEQ:
        case IR_INE:
        case IR_ILT_S:
        case IR_ILE_S:
        case IR_IGT_S:
        case IR_IGE_S:
            if (!check_operand(f, i, in->a, IR_T_I32, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_I32, err, err_cap)) return FALSE;
            exp_t   = IR_T_BOOL;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags) & IR_FLAG_VARYING;
            break;

        case IR_BAND:
        case IR_BOR:
            if (!check_operand(f, i, in->a, IR_T_BOOL, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_BOOL, err, err_cap)) return FALSE;
            exp_t   = IR_T_BOOL;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags) & IR_FLAG_VARYING;
            break;

        case IR_SELECT:
            if (!check_operand(f, i, in->a, IR_T_BOOL, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->b, IR_T_VOID, err, err_cap)) return FALSE;
            if (!check_operand(f, i, in->c, (IrType)f->insts[in->b].type, err, err_cap)) return FALSE;
            exp_t   = (IrType)f->insts[in->b].type;
            exp_var = (f->insts[in->a].flags | f->insts[in->b].flags | f->insts[in->c].flags) & IR_FLAG_VARYING;
            break;

        case IR_DERIV_X:
        case IR_DERIV_Y:
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            exp_t   = IR_T_F32;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_TEX_W:
        case IR_TEX_H:
        case IR_TEX_FLAGS:
        case IR_TEX_LEVELS:
            if (in->imm.u >= SHADE_MAX_TEXTURES) {
                return verify_fail(err, err_cap, "inst %%%u: texture slot %u out of range", i, in->imm.u);
            }
            exp_t = IR_T_I32;
            break;

        case IR_TEXEL_LOAD:
            if (in->imm.u >= SHADE_MAX_TEXTURES) {
                return verify_fail(err, err_cap, "inst %%%u: texture slot %u out of range", i, in->imm.u);
            }
            if (!check_operand(f, i, in->a, IR_T_I32, err, err_cap)) return FALSE;
            exp_t   = IR_T_I32;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_IF_BEGIN:
            if (!check_operand(f, i, in->a, IR_T_BOOL, err, err_cap)) return FALSE;
            if (if_depth >= ARRAY_LEN(if_has_else)) {
                return verify_fail(err, err_cap, "inst %%%u: IF nesting depth overflow", i);
            }
            if_has_else[if_depth++] = FALSE;
            exp_t   = IR_T_VOID;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_ELSE:
            if (if_depth == 0) {
                return verify_fail(err, err_cap, "inst %%%u: ELSE without matching IF_BEGIN", i);
            }
            if (if_has_else[if_depth - 1]) {
                return verify_fail(err, err_cap, "inst %%%u: duplicate ELSE in same IF block", i);
            }
            if_has_else[if_depth - 1] = TRUE;
            exp_t = IR_T_VOID;
            break;

        case IR_IF_END:
            if (if_depth == 0) {
                return verify_fail(err, err_cap, "inst %%%u: IF_END without matching IF_BEGIN", i);
            }
            if_depth--;
            exp_t = IR_T_VOID;
            break;

        case IR_DISCARD:
            if (!check_operand(f, i, in->a, IR_T_BOOL, err, err_cap)) return FALSE;
            exp_t   = IR_T_VOID;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        case IR_OUTPUT:
            if (if_depth != 0) {
                return verify_fail(err, err_cap, "inst %%%u: OUTPUT inside IF block", i);
            }
            if (in->imm.u >= 4) {
                return verify_fail(err, err_cap, "inst %%%u: invalid OUTPUT channel %u", i, in->imm.u);
            }
            if (out_mask & (1u << in->imm.u)) {
                return verify_fail(err, err_cap, "inst %%%u: duplicate OUTPUT channel %u", i, in->imm.u);
            }
            if (!check_operand(f, i, in->a, IR_T_F32, err, err_cap)) return FALSE;
            out_mask |= (1u << in->imm.u);
            exp_t   = IR_T_VOID;
            exp_var = f->insts[in->a].flags & IR_FLAG_VARYING;
            break;

        default:
            return verify_fail(err, err_cap, "inst %%%u: unknown opcode %u", i, (u32)op);
        }

        if ((IrType)in->type != exp_t) {
            return verify_fail(err, err_cap, "inst %%%u: result type %u != expected %u",
                               i, (u32)in->type, (u32)exp_t);
        }
        if (exp_t != IR_T_VOID && (in->flags & IR_FLAG_VARYING) != exp_var) {
            return verify_fail(err, err_cap, "inst %%%u: varying flag %u != expected %u",
                               i, (u32)(in->flags & IR_FLAG_VARYING), (u32)exp_var);
        }
    }

    if (if_depth != 0) {
        return verify_fail(err, err_cap, "unclosed IF_BEGIN (depth=%u)", if_depth);
    }
    if (out_mask != 0xFu) {
        return verify_fail(err, err_cap, "incomplete OUTPUT channels (mask=0x%x, expected 0xf)", out_mask);
    }
    return TRUE;
}

static const char* ir_op_name(IrOp op) {
    switch (op) {
    case IR_NOP:         return "nop";
    case IR_CONST_F32:   return "const.f32";
    case IR_CONST_I32:   return "const.i32";
    case IR_CONST_BOOL:  return "const.bool";
    case IR_UNIFORM_F32: return "uniform.f32";
    case IR_UNIFORM_I32: return "uniform.i32";
    case IR_VARYING:     return "varying";
    case IR_FRAG_X:      return "frag.x";
    case IR_FRAG_Y:      return "frag.y";
    case IR_FADD:        return "fadd";
    case IR_FSUB:        return "fsub";
    case IR_FMUL:        return "fmul";
    case IR_FDIV:        return "fdiv";
    case IR_FMIN:        return "fmin";
    case IR_FMAX:        return "fmax";
    case IR_FNEG:        return "fneg";
    case IR_FABS:        return "fabs";
    case IR_FSQRT:       return "fsqrt";
    case IR_FFLOOR:      return "ffloor";
    case IR_FCEIL:       return "fceil";
    case IR_FTRUNC:      return "ftrunc";
    case IR_FNEAREST:    return "fnearest";
    case IR_IADD:        return "iadd";
    case IR_ISUB:        return "isub";
    case IR_IMUL:        return "imul";
    case IR_IAND:        return "iand";
    case IR_IOR:         return "ior";
    case IR_IXOR:        return "ixor";
    case IR_ISHL:        return "ishl";
    case IR_ISHR_S:      return "ishr_s";
    case IR_ISHR_U:      return "ishr_u";
    case IR_IMIN_S:      return "imin_s";
    case IR_IMAX_S:      return "imax_s";
    case IR_FEQ:         return "feq";
    case IR_FNE:         return "fne";
    case IR_FLT:         return "flt";
    case IR_FLE:         return "fle";
    case IR_FGT:         return "fgt";
    case IR_FGE:         return "fge";
    case IR_IEQ:         return "ieq";
    case IR_INE:         return "ine";
    case IR_ILT_S:       return "ilt_s";
    case IR_ILE_S:       return "ile_s";
    case IR_IGT_S:       return "igt_s";
    case IR_IGE_S:       return "ige_s";
    case IR_BAND:        return "band";
    case IR_BOR:         return "bor";
    case IR_BNOT:        return "bnot";
    case IR_SELECT:      return "select";
    case IR_F2I:         return "f2i";
    case IR_I2F:         return "i2f";
    case IR_BITCAST_F2I: return "bitcast.f2i";
    case IR_BITCAST_I2F: return "bitcast.i2f";
    case IR_TEX_W:       return "tex.w";
    case IR_TEX_H:       return "tex.h";
    case IR_TEX_FLAGS:   return "tex.flags";
    case IR_TEX_LEVELS:  return "tex.levels";
    case IR_DERIV_X:     return "dfdx";
    case IR_DERIV_Y:     return "dfdy";
    case IR_TEXEL_LOAD:  return "texel_load";
    case IR_IF_BEGIN:    return "if";
    case IR_ELSE:        return "else";
    case IR_IF_END:      return "endif";
    case IR_DISCARD:     return "discard";
    case IR_OUTPUT:      return "output";
    default:             return "unknown";
    }
}

static void print_append(char* buf, u32 cap, u32* pos, const char* fmt, ...) {
    if (*pos + 1 >= cap) return;
    VaList ap;
    VA_START(ap, fmt);
    u32 n = fmt_vformat(buf + *pos, cap - *pos, fmt, ap);
    VA_END(ap);
    *pos += n;
}

u32 ir_print(const IrFunc* f, char* buf, u32 cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!f) return 0;

    u32 pos = 0;
    u32 indent = 0;

    for (u32 i = 0; i < f->count; i++) {
        const IrInst* in = &f->insts[i];
        IrOp op = (IrOp)in->op;
        if (op == IR_NOP) continue;

        if (op == IR_ELSE || op == IR_IF_END) {
            if (indent > 0) indent--;
        }

        for (u32 d = 0; d < indent; d++) {
            print_append(buf, cap, &pos, "  ");
        }

        const char* uv = (in->flags & IR_FLAG_VARYING) ? ".v" : ".u";
        const char* name = ir_op_name(op);

        switch (op) {
        case IR_CONST_F32:
            print_append(buf, cap, &pos, "%%%u = %s%s %f\n", i, name, uv, (f64)in->imm.f);
            break;
        case IR_CONST_I32:
            print_append(buf, cap, &pos, "%%%u = %s%s %d\n", i, name, uv, in->imm.i);
            break;
        case IR_CONST_BOOL:
            print_append(buf, cap, &pos, "%%%u = %s%s %s\n", i, name, uv, in->imm.i ? "true" : "false");
            break;
        case IR_UNIFORM_F32:
        case IR_UNIFORM_I32:
        case IR_VARYING:
        case IR_TEX_W:
        case IR_TEX_H:
        case IR_TEX_FLAGS:
        case IR_TEX_LEVELS:
            print_append(buf, cap, &pos, "%%%u = %s%s %u\n", i, name, uv, in->imm.u);
            break;
        case IR_FRAG_X:
        case IR_FRAG_Y:
            print_append(buf, cap, &pos, "%%%u = %s%s\n", i, name, uv);
            break;
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
        case IR_DERIV_X:
        case IR_DERIV_Y:
            print_append(buf, cap, &pos, "%%%u = %s%s %%%u\n", i, name, uv, in->a);
            break;
        case IR_TEXEL_LOAD:
            print_append(buf, cap, &pos, "%%%u = %s%s %u, %%%u\n", i, name, uv, in->imm.u, in->a);
            break;
        case IR_SELECT:
            print_append(buf, cap, &pos, "%%%u = %s%s %%%u, %%%u, %%%u\n", i, name, uv, in->a, in->b, in->c);
            break;
        case IR_IF_BEGIN:
            print_append(buf, cap, &pos, "%s%s %%%u\n", name, uv, in->a);
            indent++;
            break;
        case IR_ELSE:
            print_append(buf, cap, &pos, "%s\n", name);
            indent++;
            break;
        case IR_IF_END:
            print_append(buf, cap, &pos, "%s\n", name);
            break;
        case IR_DISCARD:
            print_append(buf, cap, &pos, "%s%s %%%u\n", name, uv, in->a);
            break;
        case IR_OUTPUT:
            print_append(buf, cap, &pos, "%s %u, %%%u\n", name, in->imm.u, in->a);
            break;
        default:
            /* İkili operatörler */
            print_append(buf, cap, &pos, "%%%u = %s%s %%%u, %%%u\n", i, name, uv, in->a, in->b);
            break;
        }
    }
    return pos;
}
