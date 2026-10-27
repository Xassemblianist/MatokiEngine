/* matoki/lower.c — Matoki AST → skaler SSA IR indirme (lowering) implementasyonu.
 * Yardımcı fonksiyonları çağrı noktasında inline eder, sabit sınır döngüleri açar,
 * aşkın matematik fonksiyonlarını (sin/cos/exp2/log2/pow) core/math.c ile birebir
 * aynı minimax polinom katsayılarıyla IR komutlarına açar.
 */
#include "matoki/lower.h"
#include "core/log.h"

typedef struct {
    MtkType type;
    u32     comps;
    u32     id[16];
    u32     tex_slot;
    u32     tex_flags;
} LowerVal;

typedef struct {
    char     name[32];
    LowerVal val;
    u32      scope_depth;
} LowerVar;

typedef struct {
    u32 cond_id;
    b32 is_else;
} CondFrame;

typedef struct {
    const AstProgram* prog;
    const MtkLayout*  layout;
    Arena*            arena;
    IrFunc*           ir;

    LowerVar          vars[128];
    u32               var_count;
    u32               scope_depth;

    CondFrame         cond_stack[32];
    u32               cond_depth;

    /* Aktif fonksiyonun dönüş değeri (frag veya inline edilen yardımcı fonksiyon) */
    LowerVal          ret_val;
    b32               has_ret;
} LowerCtx;

static b32 str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_name(char dst[32], const char* src) {
    u32 i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static const AstDecl* find_top_decl(const AstProgram* prog, const char* name) {
    for (u32 i = 0; i < prog->decl_count; i++) {
        if (str_eq(prog->decls[i]->name, name)) return prog->decls[i];
    }
    return NULL;
}

static i32 find_var(const LowerCtx* ctx, const char* name) {
    for (i32 i = (i32)ctx->var_count - 1; i >= 0; i--) {
        if (str_eq(ctx->vars[i].name, name)) return i;
    }
    return -1;
}

/* ---------- Polinom açılımları (core/math.c ile birebir aynı katsayılar) ---------- */

static u32 emit_trig_scalar(IrFunc* f, u32 x, b32 is_cos) {
    if (is_cos) {
        u32 pio2 = ir_const_f32(f, 1.57079633f);
        const IrInst* ix = &f->insts[x];
        if (ix->op == IR_FADD && !(f->insts[ix->b].flags & IR_FLAG_VARYING)) {
            u32 lhs = ix->a, rhs = ix->b;
            x = ir_op2(f, IR_FADD, lhs, ir_op2(f, IR_FADD, rhs, pio2));
        } else if (ix->op == IR_FADD && !(f->insts[ix->a].flags & IR_FLAG_VARYING)) {
            u32 lhs = ix->a, rhs = ix->b;
            x = ir_op2(f, IR_FADD, ir_op2(f, IR_FADD, lhs, pio2), rhs);
        } else if (ix->op == IR_FSUB && !(f->insts[ix->b].flags & IR_FLAG_VARYING)) {
            u32 lhs = ix->a, rhs = ix->b;
            x = ir_op2(f, IR_FADD, lhs, ir_op2(f, IR_FSUB, pio2, rhs));
        } else {
            x = ir_op2(f, IR_FADD, x, pio2);
        }
    }

    u32 inv_pi = ir_const_f32(f, 0.31830987f);
    u32 half   = ir_const_f32(f, 0.5f);
    u32 kf     = ir_op1(f, IR_FFLOOR, ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, x, inv_pi), half));

    u32 pi1    = ir_const_f32(f, 3.140625f);
    u32 pi2    = ir_const_f32(f, 9.6765359e-04f);

    u32 r      = ir_op2(f, IR_FSUB, x, ir_op2(f, IR_FMUL, kf, pi1));
    r          = ir_op2(f, IR_FSUB, r, ir_op2(f, IR_FMUL, kf, pi2));
    u32 ri     = ir_op1(f, IR_BITCAST_F2I, r);
    u32 k      = ir_op1(f, IR_F2I, kf);
    u32 sign   = ir_op2(f, IR_ISHL, k, ir_const_i32(f, 31));
    r          = ir_op1(f, IR_BITCAST_I2F, ir_op2(f, IR_IXOR, ri, sign));

    u32 r2     = ir_op2(f, IR_FMUL, r, r);

    /* Tek 7. derece minimax sin(r) polinomu, r in [-pi/2, pi/2] */
    u32 p      = ir_const_f32(f, -1.95152959e-04f);
    p          = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r2), ir_const_f32(f, 8.33216076e-03f));
    p          = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r2), ir_const_f32(f, -1.66666546e-01f));
    return ir_op2(f, IR_FADD, r, ir_op2(f, IR_FMUL, r, ir_op2(f, IR_FMUL, r2, p)));
}

static u32 emit_exp2_scalar(IrFunc* f, u32 x) {
    u32 lo   = ir_const_f32(f, -126.0f);
    u32 hi   = ir_const_f32(f, 127.9999f);
    u32 xc   = ir_op2(f, IR_FMIN, ir_op2(f, IR_FMAX, x, lo), hi);
    u32 kf   = ir_op1(f, IR_FFLOOR, ir_op2(f, IR_FADD, xc, ir_const_f32(f, 0.5f)));
    u32 k    = ir_op1(f, IR_F2I, kf);
    u32 r    = ir_op2(f, IR_FSUB, xc, kf);

    u32 p = ir_const_f32(f, 1.546145504e-04f);
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r), ir_const_f32(f, 1.339087728e-03f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r), ir_const_f32(f, 9.618056938e-03f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r), ir_const_f32(f, 5.550356954e-02f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r), ir_const_f32(f, 2.402265072e-01f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, r), ir_const_f32(f, 6.931471825e-01f));

    u32 mant  = ir_op2(f, IR_FADD, ir_const_f32(f, 1.0f), ir_op2(f, IR_FMUL, r, p));
    u32 exp_b = ir_op2(f, IR_ISHL, ir_op2(f, IR_IADD, k, ir_const_i32(f, 127)), ir_const_i32(f, 23));
    u32 scale = ir_op1(f, IR_BITCAST_I2F, exp_b);
    u32 res   = ir_op2(f, IR_FMUL, mant, scale);
    u32 under = ir_op2(f, IR_FLT, x, lo);
    return ir_select(f, under, ir_const_f32(f, 0.0f), res);
}

static u32 emit_log2_scalar(IrFunc* f, u32 x) {
    u32 bits = ir_op1(f, IR_BITCAST_F2I, x);
    u32 e0   = ir_op2(f, IR_ISUB,
                      ir_op2(f, IR_IAND, ir_op2(f, IR_ISHR_U, bits, ir_const_i32(f, 23)), ir_const_i32(f, 0xff)),
                      ir_const_i32(f, 127));
    u32 m0_b = ir_op2(f, IR_IOR,
                      ir_op2(f, IR_IAND, bits, ir_const_i32(f, 0x007fffff)),
                      ir_const_i32(f, 0x3f800000));
    u32 m0   = ir_op1(f, IR_BITCAST_I2F, m0_b);
    u32 gt   = ir_op2(f, IR_FGT, m0, ir_const_f32(f, 1.41421356237f));
    u32 m    = ir_select(f, gt, ir_op2(f, IR_FMUL, m0, ir_const_f32(f, 0.5f)), m0);
    u32 e    = ir_select(f, gt, ir_op2(f, IR_IADD, e0, ir_const_i32(f, 1)), e0);

    u32 one  = ir_const_f32(f, 1.0f);
    u32 t    = ir_op2(f, IR_FDIV, ir_op2(f, IR_FSUB, m, one), ir_op2(f, IR_FADD, m, one));
    u32 t2   = ir_op2(f, IR_FMUL, t, t);

    u32 p = ir_const_f32(f, 4.317310750e-01f);
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, t2), ir_const_f32(f, 5.767146349e-01f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, t2), ir_const_f32(f, 9.617988467e-01f));
    p = ir_op2(f, IR_FADD, ir_op2(f, IR_FMUL, p, t2), ir_const_f32(f, 2.885390043e+00f));

    u32 ef = ir_op1(f, IR_I2F, e);
    return ir_op2(f, IR_FADD, ef, ir_op2(f, IR_FMUL, t, p));
}

static u32 emit_pow_scalar(IrFunc* f, u32 x, u32 y) {
    u32 zero   = ir_const_f32(f, 0.0f);
    u32 one    = ir_const_f32(f, 1.0f);
    u32 safe_x = ir_op2(f, IR_FMAX, x, ir_const_f32(f, 1e-38f));
    u32 lg     = emit_log2_scalar(f, safe_x);
    u32 ex     = emit_exp2_scalar(f, ir_op2(f, IR_FMUL, y, lg));
    u32 x_le0  = ir_op2(f, IR_FLE, x, zero);
    u32 y_eq0  = ir_op2(f, IR_FEQ, y, zero);
    return ir_select(f, y_eq0, one, ir_select(f, x_le0, zero, ex));
}

/* ---------- İfade ve deyim indirme ---------- */

static LowerVal lower_expr(LowerCtx* ctx, const AstExpr* e);
static void     lower_stmt(LowerCtx* ctx, const AstStmt* st);

static LowerVal make_val(MtkType type) {
    LowerVal v;
    __builtin_memset(&v, 0, sizeof(v));
    v.type  = type;
    v.comps = mtk_type_components(type);
    return v;
}

static u32 emit_scalar_binop(IrFunc* f, TokenKind op, MtkType base_type, u32 a, u32 b) {
    b32 is_flt  = (base_type == MTK_TYPE_F32);
    b32 is_uint = (base_type == MTK_TYPE_U32);

    switch (op) {
    case TOK_PLUS:
    case TOK_PLUS_ASSIGN:  return ir_op2(f, is_flt ? IR_FADD : IR_IADD, a, b);
    case TOK_MINUS:
    case TOK_MINUS_ASSIGN: return ir_op2(f, is_flt ? IR_FSUB : IR_ISUB, a, b);
    case TOK_STAR:
    case TOK_STAR_ASSIGN:  return ir_op2(f, is_flt ? IR_FMUL : IR_IMUL, a, b);
    case TOK_SLASH:
    case TOK_SLASH_ASSIGN: return ir_op2(f, IR_FDIV, a, b);

    case TOK_AMP:          return ir_op2(f, IR_IAND, a, b);
    case TOK_PIPE:         return ir_op2(f, IR_IOR,  a, b);
    case TOK_CARET:        return ir_op2(f, IR_IXOR, a, b);
    case TOK_SHL:          return ir_op2(f, IR_ISHL, a, b);
    case TOK_SHR:          return ir_op2(f, is_uint ? IR_ISHR_U : IR_ISHR_S, a, b);

    case TOK_AND_AND:      return ir_op2(f, IR_BAND, a, b);
    case TOK_OR_OR:        return ir_op2(f, IR_BOR,  a, b);

    case TOK_EQ:
        if (base_type == MTK_TYPE_BOOL) {
            return ir_op1(f, IR_BNOT, ir_op2(f, IR_BOR,
                          ir_op2(f, IR_BAND, a, ir_op1(f, IR_BNOT, b)),
                          ir_op2(f, IR_BAND, ir_op1(f, IR_BNOT, a), b)));
        }
        return ir_op2(f, is_flt ? IR_FEQ : IR_IEQ, a, b);
    case TOK_NE:
        if (base_type == MTK_TYPE_BOOL) {
            return ir_op2(f, IR_BOR,
                          ir_op2(f, IR_BAND, a, ir_op1(f, IR_BNOT, b)),
                          ir_op2(f, IR_BAND, ir_op1(f, IR_BNOT, a), b));
        }
        return ir_op2(f, is_flt ? IR_FNE : IR_INE, a, b);
    case TOK_LT: return ir_op2(f, is_flt ? IR_FLT : IR_ILT_S, a, b);
    case TOK_LE: return ir_op2(f, is_flt ? IR_FLE : IR_ILE_S, a, b);
    case TOK_GT: return ir_op2(f, is_flt ? IR_FGT : IR_IGT_S, a, b);
    case TOK_GE: return ir_op2(f, is_flt ? IR_FGE : IR_IGE_S, a, b);
    default:
        PANIC("desteklenmeyen operator: %d", (int)op);
    }
}

static LowerVal lower_type_ctor(LowerCtx* ctx, const AstExpr* e) {
    IrFunc* f = ctx->ir;
    MtkType target = e->type;
    LowerVal res = make_val(target);
    u32 argc = e->as.call.arg_count;

    LowerVal args[8];
    for (u32 i = 0; i < argc; i++) {
        args[i] = lower_expr(ctx, e->as.call.args[i]);
    }

    /* Skaler dönüşüm */
    if (res.comps == 1) {
        MtkType st = args[0].type;
        u32 a = args[0].id[0];
        if (target == MTK_TYPE_F32) {
            if (st == MTK_TYPE_F32) res.id[0] = a;
            else if (st == MTK_TYPE_I32 || st == MTK_TYPE_U32) res.id[0] = ir_op1(f, IR_I2F, a);
            else if (st == MTK_TYPE_BOOL) res.id[0] = ir_select(f, a, ir_const_f32(f, 1.0f), ir_const_f32(f, 0.0f));
        } else if (target == MTK_TYPE_I32 || target == MTK_TYPE_U32) {
            if (st == MTK_TYPE_I32 || st == MTK_TYPE_U32) res.id[0] = a;
            else if (st == MTK_TYPE_F32) res.id[0] = ir_op1(f, IR_F2I, a);
            else if (st == MTK_TYPE_BOOL) res.id[0] = ir_select(f, a, ir_const_i32(f, 1), ir_const_i32(f, 0));
        } else if (target == MTK_TYPE_BOOL) {
            if (st == MTK_TYPE_BOOL) res.id[0] = a;
            else if (st == MTK_TYPE_F32) res.id[0] = ir_op2(f, IR_FNE, a, ir_const_f32(f, 0.0f));
            else res.id[0] = ir_op2(f, IR_INE, a, ir_const_i32(f, 0));
        }
        return res;
    }

    /* Tek argümanlı vektör kurucusu: splat veya tip dönüşümü */
    if (argc == 1) {
        if (args[0].comps == 1) {
            for (u32 c = 0; c < res.comps; c++) res.id[c] = args[0].id[0];
            return res;
        }
        MtkType dst_base = mtk_type_scalar_base(target);
        MtkType src_base = mtk_type_scalar_base(args[0].type);
        for (u32 c = 0; c < res.comps; c++) {
            u32 v = args[0].id[c];
            if (dst_base == MTK_TYPE_F32 && src_base != MTK_TYPE_F32) v = ir_op1(f, IR_I2F, v);
            else if (dst_base != MTK_TYPE_F32 && src_base == MTK_TYPE_F32) v = ir_op1(f, IR_F2I, v);
            res.id[c] = v;
        }
        return res;
    }

    /* Çok argümanlı vektör birleştirme */
    u32 out_c = 0;
    for (u32 i = 0; i < argc; i++) {
        for (u32 c = 0; c < args[i].comps && out_c < res.comps; c++) {
            res.id[out_c++] = args[i].id[c];
        }
    }
    return res;
}

static LowerVal lower_builtin_call(LowerCtx* ctx, const AstExpr* e, b32* out_handled) {
    IrFunc* f = ctx->ir;
    const char* name = e->as.call.callee;
    u32 argc = e->as.call.arg_count;
    *out_handled = TRUE;

    if (str_eq(name, "sample")) {
        LowerVal tex = lower_expr(ctx, e->as.call.args[0]);
        LowerVal uv  = lower_expr(ctx, e->as.call.args[1]);
        LowerVal res = make_val(MTK_TYPE_VEC4);
        ir_emit_sample(f, tex.tex_slot, uv.id[0], uv.id[1], tex.tex_flags, res.id);
        return res;
    }

    if (str_eq(name, "sample_level")) {
        LowerVal tex = lower_expr(ctx, e->as.call.args[0]);
        LowerVal uv  = lower_expr(ctx, e->as.call.args[1]);
        LowerVal lv  = lower_expr(ctx, e->as.call.args[2]);
        LowerVal res = make_val(MTK_TYPE_VEC4);
        ir_emit_sample_level(f, tex.tex_slot, uv.id[0], uv.id[1], lv.id[0], tex.tex_flags, res.id);
        return res;
    }

    if (str_eq(name, "tex_size")) {
        LowerVal tex = lower_expr(ctx, e->as.call.args[0]);
        LowerVal res = make_val(MTK_TYPE_IVEC2);
        res.id[0] = ir_tex_info(f, IR_TEX_W, tex.tex_slot);
        res.id[1] = ir_tex_info(f, IR_TEX_H, tex.tex_slot);
        return res;
    }

    LowerVal a[3];
    for (u32 i = 0; i < argc && i < 3; i++) {
        a[i] = lower_expr(ctx, e->as.call.args[i]);
    }

    LowerVal res = make_val(e->type);

    if (str_eq(name, "abs")) {
        b32 is_flt = mtk_type_is_float(a[0].type);
        for (u32 c = 0; c < res.comps; c++) {
            if (is_flt) {
                res.id[c] = ir_op1(f, IR_FABS, a[0].id[c]);
            } else {
                u32 z   = ir_const_i32(f, 0);
                u32 neg = ir_op2(f, IR_ILT_S, a[0].id[c], z);
                res.id[c] = ir_select(f, neg, ir_op2(f, IR_ISUB, z, a[0].id[c]), a[0].id[c]);
            }
        }
        return res;
    }
    if (str_eq(name, "floor")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = ir_op1(f, IR_FFLOOR, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "dFdx") || str_eq(name, "dFdy")) {
        IrOp dop = str_eq(name, "dFdx") ? IR_DERIV_X : IR_DERIV_Y;
        for (u32 c = 0; c < res.comps; c++) res.id[c] = ir_op1(f, dop, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "fwidth")) {
        for (u32 c = 0; c < res.comps; c++) {
            u32 dx = ir_op1(f, IR_FABS, ir_op1(f, IR_DERIV_X, a[0].id[c]));
            u32 dy = ir_op1(f, IR_FABS, ir_op1(f, IR_DERIV_Y, a[0].id[c]));
            res.id[c] = ir_op2(f, IR_FADD, dx, dy);
        }
        return res;
    }
    if (str_eq(name, "ceil")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = ir_op1(f, IR_FCEIL, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "fract")) {
        for (u32 c = 0; c < res.comps; c++) {
            u32 fl = ir_op1(f, IR_FFLOOR, a[0].id[c]);
            res.id[c] = ir_op2(f, IR_FSUB, a[0].id[c], fl);
        }
        return res;
    }
    if (str_eq(name, "sqrt")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = ir_op1(f, IR_FSQRT, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "rsqrt")) {
        u32 one = ir_const_f32(f, 1.0f);
        for (u32 c = 0; c < res.comps; c++) {
            res.id[c] = ir_op2(f, IR_FDIV, one, ir_op1(f, IR_FSQRT, a[0].id[c]));
        }
        return res;
    }
    if (str_eq(name, "sin")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = emit_trig_scalar(f, a[0].id[c], FALSE);
        return res;
    }
    if (str_eq(name, "cos")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = emit_trig_scalar(f, a[0].id[c], TRUE);
        return res;
    }
    if (str_eq(name, "exp2")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = emit_exp2_scalar(f, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "log2")) {
        for (u32 c = 0; c < res.comps; c++) res.id[c] = emit_log2_scalar(f, a[0].id[c]);
        return res;
    }
    if (str_eq(name, "pow")) {
        for (u32 c = 0; c < res.comps; c++) {
            u32 yc = a[1].comps == 1 ? a[1].id[0] : a[1].id[c];
            res.id[c] = emit_pow_scalar(f, a[0].id[c], yc);
        }
        return res;
    }
    if (str_eq(name, "min") || str_eq(name, "max")) {
        b32 is_min = str_eq(name, "min");
        b32 is_flt = mtk_type_is_float(res.type);
        IrOp op    = is_flt ? (is_min ? IR_FMIN : IR_FMAX) : (is_min ? IR_IMIN_S : IR_IMAX_S);
        for (u32 c = 0; c < res.comps; c++) {
            u32 bc = a[1].comps == 1 ? a[1].id[0] : a[1].id[c];
            res.id[c] = ir_op2(f, op, a[0].id[c], bc);
        }
        return res;
    }
    if (str_eq(name, "clamp")) {
        b32 is_flt = mtk_type_is_float(res.type);
        IrOp op_min = is_flt ? IR_FMIN : IR_IMIN_S;
        IrOp op_max = is_flt ? IR_FMAX : IR_IMAX_S;
        for (u32 c = 0; c < res.comps; c++) {
            u32 lo = a[1].comps == 1 ? a[1].id[0] : a[1].id[c];
            u32 hi = a[2].comps == 1 ? a[2].id[0] : a[2].id[c];
            res.id[c] = ir_op2(f, op_min, ir_op2(f, op_max, a[0].id[c], lo), hi);
        }
        return res;
    }
    if (str_eq(name, "mix")) {
        for (u32 c = 0; c < res.comps; c++) {
            u32 tc   = a[2].comps == 1 ? a[2].id[0] : a[2].id[c];
            u32 diff = ir_op2(f, IR_FSUB, a[1].id[c], a[0].id[c]);
            res.id[c] = ir_op2(f, IR_FADD, a[0].id[c], ir_op2(f, IR_FMUL, diff, tc));
        }
        return res;
    }
    if (str_eq(name, "step")) {
        u32 zero = ir_const_f32(f, 0.0f);
        u32 one  = ir_const_f32(f, 1.0f);
        for (u32 c = 0; c < res.comps; c++) {
            u32 edge = a[0].comps == 1 ? a[0].id[0] : a[0].id[c];
            u32 lt   = ir_op2(f, IR_FLT, a[1].id[c], edge);
            res.id[c] = ir_select(f, lt, zero, one);
        }
        return res;
    }
    if (str_eq(name, "smoothstep")) {
        u32 zero  = ir_const_f32(f, 0.0f);
        u32 one   = ir_const_f32(f, 1.0f);
        u32 two   = ir_const_f32(f, 2.0f);
        u32 three = ir_const_f32(f, 3.0f);
        for (u32 c = 0; c < res.comps; c++) {
            u32 e0  = a[0].comps == 1 ? a[0].id[0] : a[0].id[c];
            u32 e1  = a[1].comps == 1 ? a[1].id[0] : a[1].id[c];
            u32 x   = a[2].id[c];
            u32 raw = ir_op2(f, IR_FDIV, ir_op2(f, IR_FSUB, x, e0), ir_op2(f, IR_FSUB, e1, e0));
            u32 t   = ir_op2(f, IR_FMIN, ir_op2(f, IR_FMAX, raw, zero), one);
            u32 t2  = ir_op2(f, IR_FMUL, t, t);
            u32 h   = ir_op2(f, IR_FSUB, three, ir_op2(f, IR_FMUL, two, t));
            res.id[c] = ir_op2(f, IR_FMUL, t2, h);
        }
        return res;
    }
    if (str_eq(name, "dot")) {
        u32 sum = ir_op2(f, IR_FMUL, a[0].id[0], a[1].id[0]);
        for (u32 c = 1; c < a[0].comps; c++) {
            sum = ir_op2(f, IR_FADD, sum, ir_op2(f, IR_FMUL, a[0].id[c], a[1].id[c]));
        }
        res.id[0] = sum;
        return res;
    }
    if (str_eq(name, "cross")) {
        res.id[0] = ir_op2(f, IR_FSUB, ir_op2(f, IR_FMUL, a[0].id[1], a[1].id[2]),
                                       ir_op2(f, IR_FMUL, a[0].id[2], a[1].id[1]));
        res.id[1] = ir_op2(f, IR_FSUB, ir_op2(f, IR_FMUL, a[0].id[2], a[1].id[0]),
                                       ir_op2(f, IR_FMUL, a[0].id[0], a[1].id[2]));
        res.id[2] = ir_op2(f, IR_FSUB, ir_op2(f, IR_FMUL, a[0].id[0], a[1].id[1]),
                                       ir_op2(f, IR_FMUL, a[0].id[1], a[1].id[0]));
        return res;
    }
    if (str_eq(name, "length")) {
        u32 d = ir_op2(f, IR_FMUL, a[0].id[0], a[0].id[0]);
        for (u32 c = 1; c < a[0].comps; c++) {
            d = ir_op2(f, IR_FADD, d, ir_op2(f, IR_FMUL, a[0].id[c], a[0].id[c]));
        }
        res.id[0] = ir_op1(f, IR_FSQRT, d);
        return res;
    }
    if (str_eq(name, "normalize")) {
        u32 d = ir_op2(f, IR_FMUL, a[0].id[0], a[0].id[0]);
        for (u32 c = 1; c < a[0].comps; c++) {
            d = ir_op2(f, IR_FADD, d, ir_op2(f, IR_FMUL, a[0].id[c], a[0].id[c]));
        }
        u32 inv_len = ir_op2(f, IR_FDIV, ir_const_f32(f, 1.0f), ir_op1(f, IR_FSQRT, d));
        for (u32 c = 0; c < res.comps; c++) {
            res.id[c] = ir_op2(f, IR_FMUL, a[0].id[c], inv_len);
        }
        return res;
    }

    *out_handled = FALSE;
    return res;
}

static b32 is_ctor_callee(const char* name) {
    return str_eq(name, "f32") || str_eq(name, "i32") || str_eq(name, "u32") ||
           str_eq(name, "bool") || str_eq(name, "vec2") || str_eq(name, "vec3") ||
           str_eq(name, "vec4") || str_eq(name, "rgba") || str_eq(name, "ivec2") ||
           str_eq(name, "ivec3") || str_eq(name, "ivec4");
}

static LowerVal lower_expr(LowerCtx* ctx, const AstExpr* e) {
    IrFunc* f = ctx->ir;
    LowerVal res = make_val(e->type);

    switch (e->kind) {
    case AST_EXPR_INT:
        res.id[0] = ir_const_i32(f, e->as.i_val);
        return res;

    case AST_EXPR_FLOAT:
        res.id[0] = ir_const_f32(f, e->as.f_val);
        return res;

    case AST_EXPR_BOOL:
        res.id[0] = ir_const_bool(f, e->as.b_val);
        return res;

    case AST_EXPR_IDENT: {
        const char* name = e->as.ident.name;
        i32 vi = find_var(ctx, name);
        if (vi >= 0) {
            return ctx->vars[vi].val;
        }
        const AstDecl* d = find_top_decl(ctx->prog, name);
        if (d) {
            if (d->kind == AST_DECL_UNIFORM) {
                if (d->type == MTK_TYPE_TEXTURE2D) {
                    res.tex_slot  = d->as.uniform_decl.offset_or_slot;
                    res.tex_flags = d->as.uniform_decl.tex_flags;
                    return res;
                }
                IrType it = mtk_type_is_float(d->type) || mtk_type_is_matrix(d->type) ? IR_T_F32 : IR_T_I32;
                u32 base_off = d->as.uniform_decl.offset_or_slot;
                for (u32 c = 0; c < res.comps; c++) {
                    u32 u = ir_uniform(f, it, base_off + c * 4u);
                    if (d->type == MTK_TYPE_BOOL) {
                        u = ir_op2(f, IR_INE, u, ir_const_i32(f, 0));
                    }
                    res.id[c] = u;
                }
                return res;
            }
            if (d->kind == AST_DECL_IN) {
                u32 base_idx = d->as.in_decl.varying_index;
                for (u32 c = 0; c < res.comps; c++) {
                    res.id[c] = ir_varying(f, base_idx + c);
                }
                return res;
            }
            if (d->kind == AST_DECL_CONST) {
                return lower_expr(ctx, d->as.const_decl.value);
            }
        }
        if (str_eq(name, "frag_coord")) {
            res.id[0] = ir_frag(f, FALSE);
            res.id[1] = ir_frag(f, TRUE);
            return res;
        }
        if (str_eq(name, "frag_x")) {
            res.id[0] = ir_frag(f, FALSE);
            return res;
        }
        if (str_eq(name, "frag_y")) {
            res.id[0] = ir_frag(f, TRUE);
            return res;
        }
        PANIC("cozumlernememis kimlik: %s", name);
    }

    case AST_EXPR_UNARY: {
        LowerVal opnd = lower_expr(ctx, e->as.unary.operand);
        TokenKind op  = e->as.unary.op;
        if (op == TOK_PLUS) return opnd;
        if (op == TOK_MINUS) {
            b32 is_flt = mtk_type_is_float(e->type);
            for (u32 c = 0; c < res.comps; c++) {
                if (is_flt) {
                    res.id[c] = ir_op1(f, IR_FNEG, opnd.id[c]);
                } else {
                    res.id[c] = ir_op2(f, IR_ISUB, ir_const_i32(f, 0), opnd.id[c]);
                }
            }
            return res;
        }
        if (op == TOK_BANG) {
            res.id[0] = ir_op1(f, IR_BNOT, opnd.id[0]);
            return res;
        }
        if (op == TOK_TILDE) {
            for (u32 c = 0; c < res.comps; c++) {
                res.id[c] = ir_op2(f, IR_IXOR, opnd.id[c], ir_const_i32(f, -1));
            }
            return res;
        }
        return opnd;
    }

    case AST_EXPR_BINARY: {
        LowerVal l = lower_expr(ctx, e->as.binary.left);
        LowerVal r = lower_expr(ctx, e->as.binary.right);
        TokenKind op = e->as.binary.op;

        /* Matris * Vektör (sütun-öncelikli: m[col * N + row]) */
        if (op == TOK_STAR && (l.type == MTK_TYPE_MAT3 || l.type == MTK_TYPE_MAT4) &&
            mtk_type_is_vector(r.type)) {
            u32 n = (l.type == MTK_TYPE_MAT3) ? 3u : 4u;
            for (u32 row = 0; row < n; row++) {
                u32 acc = ir_op2(f, IR_FMUL, l.id[0 * n + row], r.id[0]);
                for (u32 col = 1; col < n; col++) {
                    acc = ir_op2(f, IR_FADD, acc, ir_op2(f, IR_FMUL, l.id[col * n + row], r.id[col]));
                }
                res.id[row] = acc;
            }
            return res;
        }

        /* Matris * Matris */
        if (op == TOK_STAR && (l.type == MTK_TYPE_MAT3 || l.type == MTK_TYPE_MAT4) && l.type == r.type) {
            u32 n = (l.type == MTK_TYPE_MAT3) ? 3u : 4u;
            for (u32 col = 0; col < n; col++) {
                for (u32 row = 0; row < n; row++) {
                    u32 acc = ir_op2(f, IR_FMUL, l.id[0 * n + row], r.id[col * n + 0]);
                    for (u32 k = 1; k < n; k++) {
                        acc = ir_op2(f, IR_FADD, acc, ir_op2(f, IR_FMUL, l.id[k * n + row], r.id[col * n + k]));
                    }
                    res.id[col * n + row] = acc;
                }
            }
            return res;
        }

        MtkType base = mtk_type_scalar_base(l.type);
        for (u32 c = 0; c < res.comps; c++) {
            u32 lc = l.comps == 1 ? l.id[0] : l.id[c];
            u32 rc = r.comps == 1 ? r.id[0] : r.id[c];
            res.id[c] = emit_scalar_binop(f, op, base, lc, rc);
        }
        return res;
    }

    case AST_EXPR_SWIZZLE: {
        LowerVal base = lower_expr(ctx, e->as.swizzle.base);
        for (u32 i = 0; i < e->as.swizzle.len; i++) {
            res.id[i] = base.id[e->as.swizzle.indices[i]];
        }
        return res;
    }

    case AST_EXPR_CALL: {
        if (is_ctor_callee(e->as.call.callee)) {
            return lower_type_ctor(ctx, e);
        }
        b32 handled = FALSE;
        LowerVal bv = lower_builtin_call(ctx, e, &handled);
        if (handled) return bv;

        /* Yardımcı fonksiyon inline etme */
        const AstDecl* fn = find_top_decl(ctx->prog, e->as.call.callee);
        ASSERT(fn && fn->kind == AST_DECL_FN);

        LowerVal arg_vals[8];
        for (u32 i = 0; i < e->as.call.arg_count; i++) {
            arg_vals[i] = lower_expr(ctx, e->as.call.args[i]);
        }

        /* Mevcut yerel değişken ve dönüş bağlamını kaydet */
        u32      saved_var_count = ctx->var_count;
        u32      saved_scope     = ctx->scope_depth;
        LowerVal saved_ret_val   = ctx->ret_val;
        b32      saved_has_ret   = ctx->has_ret;

        ctx->scope_depth++;
        ctx->has_ret = FALSE;
        ctx->ret_val = make_val(fn->type);

        for (u32 i = 0; i < fn->as.fn_decl.param_count; i++) {
            LowerVar* lv = &ctx->vars[ctx->var_count++];
            copy_name(lv->name, fn->as.fn_decl.params[i].name);
            lv->val         = arg_vals[i];
            lv->scope_depth = ctx->scope_depth;
        }

        lower_stmt(ctx, fn->as.fn_decl.body);
        LowerVal fn_result = ctx->ret_val;

        ctx->var_count   = saved_var_count;
        ctx->scope_depth = saved_scope;
        ctx->ret_val     = saved_ret_val;
        ctx->has_ret     = saved_has_ret;
        return fn_result;
    }
    }
    return res;
}

static u32 build_active_mask(LowerCtx* ctx) {
    IrFunc* f = ctx->ir;
    if (ctx->cond_depth == 0) {
        return ir_const_bool(f, TRUE);
    }
    u32 mask = IR_NONE;
    for (u32 i = 0; i < ctx->cond_depth; i++) {
        u32 c = ctx->cond_stack[i].cond_id;
        if (ctx->cond_stack[i].is_else) {
            c = ir_op1(f, IR_BNOT, c);
        }
        mask = (mask == IR_NONE) ? c : ir_op2(f, IR_BAND, mask, c);
    }
    return mask;
}

static void lower_stmt(LowerCtx* ctx, const AstStmt* st) {
    if (!st) return;
    IrFunc* f = ctx->ir;

    switch (st->kind) {
    case AST_STMT_BLOCK: {
        u32 saved_count = ctx->var_count;
        ctx->scope_depth++;
        for (u32 i = 0; i < st->as.block.count; i++) {
            lower_stmt(ctx, st->as.block.stmts[i]);
        }
        ctx->scope_depth--;
        ctx->var_count = saved_count;
        break;
    }

    case AST_STMT_VAR_DECL: {
        LowerVal init_val = lower_expr(ctx, st->as.var_decl.init);
        LowerVar* lv = &ctx->vars[ctx->var_count++];
        copy_name(lv->name, st->as.var_decl.name);
        lv->val         = init_val;
        lv->scope_depth = ctx->scope_depth;
        break;
    }

    case AST_STMT_ASSIGN: {
        LowerVal rhs = lower_expr(ctx, st->as.assign.value);
        const AstExpr* target = st->as.assign.target;
        TokenKind op = st->as.assign.op;

        if (target->kind == AST_EXPR_IDENT) {
            i32 vi = find_var(ctx, target->as.ident.name);
            ASSERT(vi >= 0);
            LowerVal* dst = &ctx->vars[vi].val;
            if (op == TOK_ASSIGN) {
                *dst = rhs;
            } else {
                MtkType base = mtk_type_scalar_base(dst->type);
                for (u32 c = 0; c < dst->comps; c++) {
                    u32 rc = rhs.comps == 1 ? rhs.id[0] : rhs.id[c];
                    dst->id[c] = emit_scalar_binop(f, op, base, dst->id[c], rc);
                }
            }
        } else if (target->kind == AST_EXPR_SWIZZLE) {
            const AstExpr* base_ident = target->as.swizzle.base;
            i32 vi = find_var(ctx, base_ident->as.ident.name);
            ASSERT(vi >= 0);
            LowerVal* dst = &ctx->vars[vi].val;
            MtkType base = mtk_type_scalar_base(dst->type);
            for (u32 i = 0; i < target->as.swizzle.len; i++) {
                u32 idx = target->as.swizzle.indices[i];
                u32 rc  = rhs.comps == 1 ? rhs.id[0] : rhs.id[i];
                if (op == TOK_ASSIGN) {
                    dst->id[idx] = rc;
                } else {
                    dst->id[idx] = emit_scalar_binop(f, op, base, dst->id[idx], rc);
                }
            }
        }
        break;
    }

    case AST_STMT_IF: {
        LowerVal cond_val = lower_expr(ctx, st->as.if_stmt.cond);
        u32 cond_id = cond_val.id[0];

        /* IF öncesi değişken ve dönüş durumu anlık görüntüsü */
        u32      pre_count = ctx->var_count;
        LowerVar pre_vars[128];
        for (u32 i = 0; i < pre_count; i++) pre_vars[i] = ctx->vars[i];
        LowerVal pre_ret     = ctx->ret_val;
        b32      pre_has_ret = ctx->has_ret;

        ir_if_begin(f, cond_id);
        ctx->cond_stack[ctx->cond_depth].cond_id = cond_id;
        ctx->cond_stack[ctx->cond_depth].is_else = FALSE;
        ctx->cond_depth++;

        lower_stmt(ctx, st->as.if_stmt.then_branch);

        ctx->cond_depth--;
        LowerVar then_vars[128];
        for (u32 i = 0; i < pre_count; i++) then_vars[i] = ctx->vars[i];
        LowerVal then_ret     = ctx->ret_val;
        b32      then_has_ret = ctx->has_ret;

        LowerVar else_vars[128];
        LowerVal else_ret     = pre_ret;
        b32      else_has_ret = pre_has_ret;

        if (st->as.if_stmt.else_branch) {
            for (u32 i = 0; i < pre_count; i++) ctx->vars[i] = pre_vars[i];
            ctx->var_count = pre_count;
            ctx->ret_val   = pre_ret;
            ctx->has_ret   = pre_has_ret;

            ir_else(f);
            ctx->cond_stack[ctx->cond_depth].cond_id = cond_id;
            ctx->cond_stack[ctx->cond_depth].is_else = TRUE;
            ctx->cond_depth++;

            lower_stmt(ctx, st->as.if_stmt.else_branch);

            ctx->cond_depth--;
            for (u32 i = 0; i < pre_count; i++) else_vars[i] = ctx->vars[i];
            else_ret     = ctx->ret_val;
            else_has_ret = ctx->has_ret;
        } else {
            for (u32 i = 0; i < pre_count; i++) else_vars[i] = pre_vars[i];
        }

        ir_if_end(f);

        /* Dallarda değişen değişkenleri IR_SELECT ile birleştir */
        ctx->var_count = pre_count;
        for (u32 i = 0; i < pre_count; i++) {
            ctx->vars[i] = pre_vars[i];
            for (u32 c = 0; c < pre_vars[i].val.comps; c++) {
                u32 tid = then_vars[i].val.id[c];
                u32 eid = else_vars[i].val.id[c];
                if (tid != eid) {
                    ctx->vars[i].val.id[c] = ir_select(f, cond_id, tid, eid);
                } else {
                    ctx->vars[i].val.id[c] = tid;
                }
            }
        }

        /* Dallarda return kullanıldıysa dönüş değerini birleştir */
        if (then_has_ret && else_has_ret) {
            ctx->has_ret = TRUE;
            ctx->ret_val = then_ret;
            for (u32 c = 0; c < then_ret.comps; c++) {
                if (then_ret.id[c] != else_ret.id[c]) {
                    ctx->ret_val.id[c] = ir_select(f, cond_id, then_ret.id[c], else_ret.id[c]);
                }
            }
        } else if (then_has_ret) {
            ctx->has_ret = TRUE;
            ctx->ret_val = then_ret;
        } else if (else_has_ret) {
            ctx->has_ret = TRUE;
            ctx->ret_val = else_ret;
        }
        break;
    }

    case AST_STMT_FOR: {
        i32 start_v = st->as.for_stmt.const_start;
        i32 end_v   = st->as.for_stmt.const_end;
        u32 saved_count = ctx->var_count;
        ctx->scope_depth++;

        u32 loop_var_idx = ctx->var_count++;
        LowerVar* lv = &ctx->vars[loop_var_idx];
        copy_name(lv->name, st->as.for_stmt.var_name);
        lv->val         = make_val(MTK_TYPE_I32);
        lv->scope_depth = ctx->scope_depth;

        for (i32 it = start_v; it < end_v; it++) {
            ctx->vars[loop_var_idx].val.id[0] = ir_const_i32(f, it);
            lower_stmt(ctx, st->as.for_stmt.body);
        }

        ctx->scope_depth--;
        ctx->var_count = saved_count;
        break;
    }

    case AST_STMT_DISCARD: {
        u32 mask = build_active_mask(ctx);
        ir_discard(f, mask);
        break;
    }

    case AST_STMT_RETURN: {
        if (st->as.ret.value) {
            LowerVal rv = lower_expr(ctx, st->as.ret.value);
            ctx->ret_val = rv;
            ctx->has_ret = TRUE;
        }
        break;
    }

    case AST_STMT_EXPR:
        lower_expr(ctx, st->as.expr_stmt.expr);
        break;
    }
}

b32 lower_ast(const AstProgram* prog, const MtkLayout* layout, Arena* arena, IrFunc* out_ir) {
    ir_init(out_ir, arena, 4096);
    out_ir->uniform_bytes = layout ? layout->uniform_bytes : 0;
    out_ir->varying_count = layout ? layout->varying_count : 0;
    out_ir->texture_count = layout ? layout->texture_count : 0;

    const AstDecl* frag = find_top_decl(prog, "frag");
    ASSERT(frag && frag->kind == AST_DECL_FN);

    LowerCtx ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    ctx.prog        = prog;
    ctx.layout      = layout;
    ctx.arena       = arena;
    ctx.ir          = out_ir;
    ctx.scope_depth = 1;
    ctx.ret_val     = make_val(MTK_TYPE_VEC4);

    lower_stmt(&ctx, frag->as.fn_decl.body);
    ASSERT(ctx.has_ret);

    for (u32 c = 0; c < 4; c++) {
        ir_output(out_ir, c, ctx.ret_val.id[c]);
    }
    return TRUE;
}
