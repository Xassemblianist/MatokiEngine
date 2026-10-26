/* matoki/sema.c — Matoki anlamsal çözümleyici (sema) implementasyonu.
 * Üst seviye bildirimlerden MtkLayout tablosunu üretir, sabit ifadeleri değerlendirir,
 * çağrı çizgesinde özyinelemeyi yakalar ve tüm ifadelerin tip/tekdüzelik bilgisini çıkarır.
 */
#include "matoki/sema.h"
#include "core/log.h"
#include "core/math.h"
#include "raster/shade_abi.h"

typedef struct {
    char    name[32];
    MtkType type;
    b32     is_mutable;
    b32     is_varying;
    u32     scope_depth;
} LocalVar;

typedef struct {
    AstProgram* prog;
    const char* src;
    u32         src_len;
    const char* filename;
    Arena*      arena;
    MtkLayout*  layout;
    MtkDiag*    diag;

    /* Aktif fonksiyon ve kapsam durumu */
    AstDecl*    cur_fn;
    LocalVar    locals[128];
    u32         local_count;
    u32         scope_depth;
    b32         in_varying_cond;

    /* Çağrı çizgesi: fn_idx -> çağrılan fn_idx bit maskesi (<= 64 fonksiyon) */
    u64         call_edges[64];
    u32         call_site_line[64][64];
    u32         call_site_col[64][64];
} Sema;

static b32 str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_name(char dst[32], const char* src) {
    u32 i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void sema_error(Sema* s, u32 line, u32 col, const char* fmt, ...) {
    if (!s->diag || s->diag->count >= MTK_MAX_ERRORS) return;
    char msg[256];
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(msg, sizeof(msg), fmt, ap);
    VA_END(ap);
    mtk_diag_error(s->diag, s->filename, s->src, s->src_len, line, col, "%s", msg);
}

static i32 find_fn_index(const AstProgram* prog, const char* name) {
    for (u32 i = 0; i < prog->decl_count; i++) {
        AstDecl* d = prog->decls[i];
        if (d->kind == AST_DECL_FN && str_eq(d->name, name)) {
            return (i32)i;
        }
    }
    return -1;
}

static AstDecl* find_top_decl(const AstProgram* prog, const char* name, u32 max_idx) {
    u32 lim = max_idx < prog->decl_count ? max_idx : prog->decl_count;
    for (u32 i = 0; i < lim; i++) {
        AstDecl* d = prog->decls[i];
        if (str_eq(d->name, name)) return d;
    }
    return NULL;
}

static i32 find_local(const Sema* s, const char* name) {
    for (i32 i = (i32)s->local_count - 1; i >= 0; i--) {
        if (str_eq(s->locals[i].name, name)) return i;
    }
    return -1;
}

/* ---------- Derleme zamanı sabit ifadesi değerlendirme ---------- */

static b32 eval_const_scalar(Sema* s, AstExpr* e, f32* out_f, i32* out_i, MtkType* out_t) {
    if (!e) return FALSE;
    switch (e->kind) {
    case AST_EXPR_INT:
        *out_i = e->as.i_val;
        *out_f = (f32)e->as.i_val;
        *out_t = MTK_TYPE_I32;
        e->type = MTK_TYPE_I32;
        return TRUE;
    case AST_EXPR_FLOAT:
        *out_f = e->as.f_val;
        *out_i = (i32)e->as.f_val;
        *out_t = MTK_TYPE_F32;
        e->type = MTK_TYPE_F32;
        return TRUE;
    case AST_EXPR_BOOL:
        *out_i = e->as.b_val ? 1 : 0;
        *out_f = e->as.b_val ? 1.0f : 0.0f;
        *out_t = MTK_TYPE_BOOL;
        e->type = MTK_TYPE_BOOL;
        return TRUE;
    case AST_EXPR_IDENT: {
        AstDecl* d = find_top_decl(s->prog, e->as.ident.name, s->prog->decl_count);
        if (d && d->kind == AST_DECL_CONST && mtk_type_components(d->type) == 1) {
            *out_f = d->as.const_decl.f_vals[0];
            *out_i = d->as.const_decl.i_vals[0];
            *out_t = d->type;
            e->type = d->type;
            return TRUE;
        }
        return FALSE;
    }
    case AST_EXPR_UNARY: {
        f32 vf = 0.0f; i32 vi = 0; MtkType vt = MTK_TYPE_VOID;
        if (!eval_const_scalar(s, e->as.unary.operand, &vf, &vi, &vt)) return FALSE;
        if (e->as.unary.op == TOK_MINUS) {
            *out_f = -vf;
            *out_i = -vi;
            *out_t = vt;
            e->type = vt;
            e->is_int_lit = e->as.unary.operand->is_int_lit;
            return TRUE;
        }
        if (e->as.unary.op == TOK_PLUS) {
            *out_f = vf;
            *out_i = vi;
            *out_t = vt;
            e->type = vt;
            e->is_int_lit = e->as.unary.operand->is_int_lit;
            return TRUE;
        }
        if (e->as.unary.op == TOK_BANG && vt == MTK_TYPE_BOOL) {
            *out_i = !vi;
            *out_f = *out_i ? 1.0f : 0.0f;
            *out_t = MTK_TYPE_BOOL;
            e->type = MTK_TYPE_BOOL;
            return TRUE;
        }
        return FALSE;
    }
    case AST_EXPR_BINARY: {
        f32 lf = 0.0f, rf = 0.0f;
        i32 li = 0, ri = 0;
        MtkType lt = MTK_TYPE_VOID, rt = MTK_TYPE_VOID;
        if (!eval_const_scalar(s, e->as.binary.left, &lf, &li, &lt)) return FALSE;
        if (!eval_const_scalar(s, e->as.binary.right, &rf, &ri, &rt)) return FALSE;
        if (lt != rt) return FALSE;
        *out_t = lt;
        e->type = lt;
        e->is_int_lit = e->as.binary.left->is_int_lit && e->as.binary.right->is_int_lit;
        switch (e->as.binary.op) {
        case TOK_PLUS:  *out_f = lf + rf; *out_i = li + ri; return TRUE;
        case TOK_MINUS: *out_f = lf - rf; *out_i = li - ri; return TRUE;
        case TOK_STAR:  *out_f = lf * rf; *out_i = li * ri; return TRUE;
        case TOK_SLASH:
            if (lt == MTK_TYPE_F32) { *out_f = lf / rf; return TRUE; }
            return FALSE;
        default:
            return FALSE;
        }
    }
    default:
        return FALSE;
    }
}

/* ---------- İfade tip ve tekdüzelik denetimi ---------- */

static MtkType check_expr(Sema* s, AstExpr* e);

static b32 unify_int_lit(AstExpr* a, AstExpr* b) {
    if (a->type == b->type) return TRUE;
    if (a->is_int_lit && (b->type == MTK_TYPE_I32 || b->type == MTK_TYPE_U32)) {
        a->type = b->type;
        return TRUE;
    }
    if (b->is_int_lit && (a->type == MTK_TYPE_I32 || a->type == MTK_TYPE_U32)) {
        b->type = a->type;
        return TRUE;
    }
    return FALSE;
}

static b32 is_type_ctor_name(const char* name, MtkType* out_type) {
    if (str_eq(name, "f32"))   { *out_type = MTK_TYPE_F32;   return TRUE; }
    if (str_eq(name, "i32"))   { *out_type = MTK_TYPE_I32;   return TRUE; }
    if (str_eq(name, "u32"))   { *out_type = MTK_TYPE_U32;   return TRUE; }
    if (str_eq(name, "bool"))  { *out_type = MTK_TYPE_BOOL;  return TRUE; }
    if (str_eq(name, "vec2"))  { *out_type = MTK_TYPE_VEC2;  return TRUE; }
    if (str_eq(name, "vec3"))  { *out_type = MTK_TYPE_VEC3;  return TRUE; }
    if (str_eq(name, "vec4"))  { *out_type = MTK_TYPE_VEC4;  return TRUE; }
    if (str_eq(name, "rgba"))  { *out_type = MTK_TYPE_VEC4;  return TRUE; }
    if (str_eq(name, "ivec2")) { *out_type = MTK_TYPE_IVEC2; return TRUE; }
    if (str_eq(name, "ivec3")) { *out_type = MTK_TYPE_IVEC3; return TRUE; }
    if (str_eq(name, "ivec4")) { *out_type = MTK_TYPE_IVEC4; return TRUE; }
    return FALSE;
}

static MtkType check_type_ctor(Sema* s, AstExpr* e, MtkType target) {
    u32 argc = e->as.call.arg_count;
    b32 varying = FALSE;
    for (u32 i = 0; i < argc; i++) {
        check_expr(s, e->as.call.args[i]);
        if (e->as.call.args[i]->is_varying) varying = TRUE;
    }
    e->is_varying = varying;
    e->type = target;

    /* Skaler dönüşüm: f32(x), i32(x), u32(x), bool(x) */
    if (mtk_type_components(target) == 1) {
        if (argc != 1) {
            sema_error(s, e->line, e->col, "'%s' kurucusu tam olarak 1 arguman bekler", mtk_type_name(target));
            return target;
        }
        MtkType src_t = e->as.call.args[0]->type;
        if (mtk_type_components(src_t) != 1 || src_t == MTK_TYPE_TEXTURE2D) {
            sema_error(s, e->line, e->col, "'%s' tipinden '%s' tipine donusum gecersiz",
                       mtk_type_name(src_t), mtk_type_name(target));
        }
        return target;
    }

    /* Vektör kurucusu: vec2/vec3/vec4/ivec2/ivec3/ivec4 */
    u32 target_comps = mtk_type_components(target);
    MtkType target_base = mtk_type_scalar_base(target);

    if (argc == 1) {
        MtkType a0 = e->as.call.args[0]->type;
        if (e->as.call.args[0]->is_int_lit && target_base == MTK_TYPE_I32) {
            a0 = MTK_TYPE_I32;
            e->as.call.args[0]->type = MTK_TYPE_I32;
        }
        /* Skaler splat: vec3(1.0) veya aynı boyutta vektör dönüşümü: vec2(ivec2_val) */
        if (a0 == target_base) return target;
        if (mtk_type_components(a0) == target_comps &&
            (mtk_type_is_float(a0) || mtk_type_is_int(a0))) {
            return target;
        }
        sema_error(s, e->line, e->col, "'%s' kurucusuna gecersiz tek arguman '%s'",
                   mtk_type_name(target), mtk_type_name(a0));
        return target;
    }

    u32 total_comps = 0;
    for (u32 i = 0; i < argc; i++) {
        AstExpr* arg = e->as.call.args[i];
        if (arg->is_int_lit && target_base == MTK_TYPE_I32) {
            arg->type = MTK_TYPE_I32;
        }
        MtkType abase = mtk_type_scalar_base(arg->type);
        if (abase != target_base && !(target_base == MTK_TYPE_I32 && abase == MTK_TYPE_U32)) {
            sema_error(s, arg->line, arg->col,
                       "'%s' kurucusunda arguman %u tipi '%s' (beklenen '%s' bileseni)",
                       mtk_type_name(target), i + 1, mtk_type_name(arg->type), mtk_type_name(target_base));
            return target;
        }
        total_comps += mtk_type_components(arg->type);
    }

    if (total_comps != target_comps) {
        sema_error(s, e->line, e->col,
                   "'%s' kurucusu %u bilesen bekler, %u bilesen verildi",
                   mtk_type_name(target), target_comps, total_comps);
    }
    return target;
}

static MtkType check_builtin_call(Sema* s, AstExpr* e, b32* out_handled) {
    const char* name = e->as.call.callee;
    u32 argc = e->as.call.arg_count;
    *out_handled = TRUE;

    /* sample(tex, uv) -> vec4 */
    if (str_eq(name, "sample")) {
        for (u32 i = 0; i < argc; i++) check_expr(s, e->as.call.args[i]);
        if (argc != 2) {
            sema_error(s, e->line, e->col, "'sample' 2 arguman bekler: (texture2d, vec2)");
            e->type = MTK_TYPE_VEC4;
            return e->type;
        }
        if (e->as.call.args[0]->type != MTK_TYPE_TEXTURE2D) {
            sema_error(s, e->as.call.args[0]->line, e->as.call.args[0]->col,
                       "'sample' ilk argumani 'texture2d' olmalidir (bulunan '%s')",
                       mtk_type_name(e->as.call.args[0]->type));
        }
        if (e->as.call.args[1]->type != MTK_TYPE_VEC2) {
            sema_error(s, e->as.call.args[1]->line, e->as.call.args[1]->col,
                       "'sample' ikinci argumani 'vec2' olmalidir (bulunan '%s')",
                       mtk_type_name(e->as.call.args[1]->type));
        }
        e->is_varying = e->as.call.args[1]->is_varying;
        e->type = MTK_TYPE_VEC4;
        return e->type;
    }

    /* tex_size(tex) -> ivec2 */
    if (str_eq(name, "tex_size")) {
        for (u32 i = 0; i < argc; i++) check_expr(s, e->as.call.args[i]);
        if (argc != 1 || e->as.call.args[0]->type != MTK_TYPE_TEXTURE2D) {
            sema_error(s, e->line, e->col, "'tex_size' tek bir 'texture2d' argumani bekler");
        }
        e->is_varying = FALSE;
        e->type = MTK_TYPE_IVEC2;
        return e->type;
    }

    /* Tek argümanlı f32/vec* yerleşikleri:
     * abs, floor, ceil, fract, sqrt, rsqrt, sin, cos, exp2, log2, length, normalize */
    b32 is_unary_math = str_eq(name, "abs") || str_eq(name, "floor") || str_eq(name, "ceil") ||
                        str_eq(name, "fract") || str_eq(name, "sqrt") || str_eq(name, "rsqrt") ||
                        str_eq(name, "sin") || str_eq(name, "cos") || str_eq(name, "exp2") ||
                        str_eq(name, "log2") || str_eq(name, "length") || str_eq(name, "normalize");
    if (is_unary_math) {
        for (u32 i = 0; i < argc; i++) check_expr(s, e->as.call.args[i]);
        if (argc != 1) {
            sema_error(s, e->line, e->col, "'%s' 1 arguman bekler, %u verildi", name, argc);
            e->type = MTK_TYPE_F32;
            return e->type;
        }
        MtkType t0 = e->as.call.args[0]->type;
        e->is_varying = e->as.call.args[0]->is_varying;
        if (str_eq(name, "abs") && (mtk_type_is_float(t0) || mtk_type_is_int(t0))) {
            e->type = t0;
            return e->type;
        }
        if (!mtk_type_is_float(t0)) {
            sema_error(s, e->as.call.args[0]->line, e->as.call.args[0]->col,
                       "'%s' f32 veya vec2/vec3/vec4 bekler (bulunan '%s')", name, mtk_type_name(t0));
            e->type = MTK_TYPE_F32;
            return e->type;
        }
        e->type = str_eq(name, "length") ? MTK_TYPE_F32 : t0;
        return e->type;
    }

    /* İki argümanlı yerleşikler: min, max, pow, step, dot, cross */
    b32 is_binary_math = str_eq(name, "min") || str_eq(name, "max") || str_eq(name, "pow") ||
                         str_eq(name, "step") || str_eq(name, "dot") || str_eq(name, "cross");
    if (is_binary_math) {
        for (u32 i = 0; i < argc; i++) check_expr(s, e->as.call.args[i]);
        if (argc != 2) {
            sema_error(s, e->line, e->col, "'%s' 2 arguman bekler, %u verildi", name, argc);
            e->type = MTK_TYPE_F32;
            return e->type;
        }
        AstExpr* a = e->as.call.args[0];
        AstExpr* b = e->as.call.args[1];
        unify_int_lit(a, b);
        e->is_varying = a->is_varying || b->is_varying;

        if (str_eq(name, "cross")) {
            if (a->type != MTK_TYPE_VEC3 || b->type != MTK_TYPE_VEC3) {
                sema_error(s, e->line, e->col, "'cross' (vec3, vec3) bekler");
            }
            e->type = MTK_TYPE_VEC3;
            return e->type;
        }

        if (str_eq(name, "dot")) {
            if (!mtk_type_is_float(a->type) || a->type != b->type) {
                sema_error(s, e->line, e->col, "'dot' ayni tipte iki f32/vec arguman bekler");
            }
            e->type = MTK_TYPE_F32;
            return e->type;
        }

        if (str_eq(name, "step")) {
            if (!mtk_type_is_float(b->type) || (a->type != b->type && a->type != MTK_TYPE_F32)) {
                sema_error(s, e->line, e->col, "'step' (T, T) veya (f32, T) bekler");
            }
            e->type = b->type;
            return e->type;
        }

        if (str_eq(name, "min") || str_eq(name, "max")) {
            b32 ok_float = mtk_type_is_float(a->type) && (b->type == a->type || b->type == MTK_TYPE_F32);
            b32 ok_int   = mtk_type_is_int(a->type) && (b->type == a->type || b->type == mtk_type_scalar_base(a->type));
            if (!ok_float && !ok_int) {
                sema_error(s, e->line, e->col, "'%s' uyumsuz arguman tipleri ('%s', '%s')",
                           name, mtk_type_name(a->type), mtk_type_name(b->type));
            }
            e->type = a->type;
            return e->type;
        }

        /* pow(x, y) */
        if (!mtk_type_is_float(a->type) || (b->type != a->type && b->type != MTK_TYPE_F32)) {
            sema_error(s, e->line, e->col, "'pow' (T, T) veya (T, f32) bekler");
        }
        e->type = a->type;
        return e->type;
    }

    /* Üç argümanlı yerleşikler: clamp, mix, smoothstep */
    if (str_eq(name, "clamp") || str_eq(name, "mix") || str_eq(name, "smoothstep")) {
        for (u32 i = 0; i < argc; i++) check_expr(s, e->as.call.args[i]);
        if (argc != 3) {
            sema_error(s, e->line, e->col, "'%s' 3 arguman bekler, %u verildi", name, argc);
            e->type = MTK_TYPE_F32;
            return e->type;
        }
        AstExpr* a = e->as.call.args[0];
        AstExpr* b = e->as.call.args[1];
        AstExpr* c = e->as.call.args[2];
        unify_int_lit(a, b);
        unify_int_lit(a, c);
        unify_int_lit(b, c);
        e->is_varying = a->is_varying || b->is_varying || c->is_varying;

        if (str_eq(name, "clamp")) {
            MtkType base = mtk_type_scalar_base(a->type);
            b32 ok_type = mtk_type_is_float(a->type) || mtk_type_is_int(a->type);
            b32 ok_args = (b->type == a->type && c->type == a->type) ||
                          (b->type == base && c->type == base);
            if (!ok_type || !ok_args) {
                sema_error(s, e->line, e->col, "'clamp' uyumsuz arguman tipleri ('%s', '%s', '%s')",
                           mtk_type_name(a->type), mtk_type_name(b->type), mtk_type_name(c->type));
            }
            e->type = a->type;
            return e->type;
        }

        if (str_eq(name, "mix")) {
            if (!mtk_type_is_float(a->type) || b->type != a->type ||
                (c->type != a->type && c->type != MTK_TYPE_F32)) {
                sema_error(s, e->line, e->col, "'mix' (T, T, T) veya (T, T, f32) bekler");
            }
            e->type = a->type;
            return e->type;
        }

        /* smoothstep(e0, e1, x) */
        if (!mtk_type_is_float(c->type) || a->type != b->type ||
            (a->type != c->type && a->type != MTK_TYPE_F32)) {
            sema_error(s, e->line, e->col, "'smoothstep' (T, T, T) veya (f32, f32, T) bekler");
        }
        e->type = c->type;
        return e->type;
    }

    *out_handled = FALSE;
    return MTK_TYPE_VOID;
}

static b32 parse_swizzle_mask(const char* mask, u32 len, u32 base_comps, u8 out_idx[4],
                              const char** out_err) {
    if (len == 0 || len > 4) {
        *out_err = "swizzle uzunlugu 1..4 arasinda olmalidir";
        return FALSE;
    }
    u32 set_id = 0; /* 1: xyzw, 2: rgba, 3: stpq */
    for (u32 i = 0; i < len; i++) {
        char c = mask[i];
        u32 cur_set = 0;
        u8  idx = 0;
        if      (c == 'x') { cur_set = 1; idx = 0; }
        else if (c == 'y') { cur_set = 1; idx = 1; }
        else if (c == 'z') { cur_set = 1; idx = 2; }
        else if (c == 'w') { cur_set = 1; idx = 3; }
        else if (c == 'r') { cur_set = 2; idx = 0; }
        else if (c == 'g') { cur_set = 2; idx = 1; }
        else if (c == 'b') { cur_set = 2; idx = 2; }
        else if (c == 'a') { cur_set = 2; idx = 3; }
        else if (c == 's') { cur_set = 3; idx = 0; }
        else if (c == 't') { cur_set = 3; idx = 1; }
        else if (c == 'p') { cur_set = 3; idx = 2; }
        else if (c == 'q') { cur_set = 3; idx = 3; }
        else {
            *out_err = "gecersiz swizzle karakteri";
            return FALSE;
        }
        if (set_id == 0) set_id = cur_set;
        else if (set_id != cur_set) {
            *out_err = "swizzle bilesen kumeleri (xyzw/rgba) karistirilamaz";
            return FALSE;
        }
        if (idx >= base_comps) {
            *out_err = "swizzle bileseni vektor boyutunu asiyor";
            return FALSE;
        }
        out_idx[i] = idx;
    }
    return TRUE;
}

static MtkType check_expr(Sema* s, AstExpr* e) {
    if (!e) return MTK_TYPE_VOID;

    switch (e->kind) {
    case AST_EXPR_INT:
        e->type = MTK_TYPE_I32;
        e->is_varying = FALSE;
        return e->type;

    case AST_EXPR_FLOAT:
        e->type = MTK_TYPE_F32;
        e->is_varying = FALSE;
        return e->type;

    case AST_EXPR_BOOL:
        e->type = MTK_TYPE_BOOL;
        e->is_varying = FALSE;
        return e->type;

    case AST_EXPR_IDENT: {
        const char* name = e->as.ident.name;
        i32 loc = find_local(s, name);
        if (loc >= 0) {
            e->type       = s->locals[loc].type;
            e->is_varying = s->locals[loc].is_varying;
            return e->type;
        }
        AstDecl* d = find_top_decl(s->prog, name, s->prog->decl_count);
        if (d && d->kind != AST_DECL_FN) {
            e->type       = d->type;
            e->is_varying = (d->kind == AST_DECL_IN);
            return e->type;
        }
        /* Yerleşik piksel koordinatı değişkenleri */
        if (str_eq(name, "frag_coord")) {
            e->type       = MTK_TYPE_VEC2;
            e->is_varying = TRUE;
            return e->type;
        }
        if (str_eq(name, "frag_x") || str_eq(name, "frag_y")) {
            e->type       = MTK_TYPE_F32;
            e->is_varying = TRUE;
            return e->type;
        }
        sema_error(s, e->line, e->col, "tanimlanmamis isim '%s'", name);
        e->type = MTK_TYPE_F32;
        return e->type;
    }

    case AST_EXPR_UNARY: {
        MtkType ot = check_expr(s, e->as.unary.operand);
        e->is_varying = e->as.unary.operand->is_varying;
        TokenKind op = e->as.unary.op;
        if (op == TOK_MINUS || op == TOK_PLUS) {
            if (!mtk_type_is_float(ot) && !mtk_type_is_int(ot)) {
                sema_error(s, e->line, e->col, "tekli '%s' operatoru '%s' tipine uygulanamaz",
                           token_kind_name(op), mtk_type_name(ot));
            }
            e->is_int_lit = e->as.unary.operand->is_int_lit;
            e->type = ot;
            return e->type;
        }
        if (op == TOK_BANG) {
            if (ot != MTK_TYPE_BOOL) {
                sema_error(s, e->line, e->col, "'!' operatoru 'bool' bekler (bulunan '%s')", mtk_type_name(ot));
            }
            e->type = MTK_TYPE_BOOL;
            return e->type;
        }
        if (op == TOK_TILDE) {
            if (!mtk_type_is_int(ot)) {
                sema_error(s, e->line, e->col, "'~' operatoru tamsayi bekler (bulunan '%s')", mtk_type_name(ot));
            }
            e->type = ot;
            return e->type;
        }
        e->type = ot;
        return e->type;
    }

    case AST_EXPR_BINARY: {
        AstExpr* l = e->as.binary.left;
        AstExpr* r = e->as.binary.right;
        MtkType lt = check_expr(s, l);
        MtkType rt = check_expr(s, r);
        if (unify_int_lit(l, r)) {
            lt = l->type;
            rt = r->type;
        }
        e->is_varying = l->is_varying || r->is_varying;
        TokenKind op = e->as.binary.op;

        /* Mantıksal && ve || */
        if (op == TOK_AND_AND || op == TOK_OR_OR) {
            if (lt != MTK_TYPE_BOOL || rt != MTK_TYPE_BOOL) {
                sema_error(s, e->line, e->col, "'%s' operatoru 'bool' operandlar bekler ('%s', '%s')",
                           token_kind_name(op), mtk_type_name(lt), mtk_type_name(rt));
            }
            e->type = MTK_TYPE_BOOL;
            return e->type;
        }

        /* Karşılaştırmalar: ==, !=, <, <=, >, >= */
        if (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
            b32 scalar_ok = (mtk_type_components(lt) == 1) && (lt == rt);
            if (!scalar_ok || lt == MTK_TYPE_TEXTURE2D ||
                ((op != TOK_EQ && op != TOK_NE) && lt == MTK_TYPE_BOOL)) {
                sema_error(s, e->line, e->col, "'%s' karsilastirmasi uyumsuz tipler arasinda ('%s', '%s')",
                           token_kind_name(op), mtk_type_name(lt), mtk_type_name(rt));
            }
            e->type = MTK_TYPE_BOOL;
            return e->type;
        }

        /* Bit düzeyinde operatörler: &, |, ^, <<, >> */
        if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET || op == TOK_SHL || op == TOK_SHR) {
            if (!mtk_type_is_int(lt) || !mtk_type_is_int(rt) ||
                (lt != rt && mtk_type_components(rt) != 1)) {
                sema_error(s, e->line, e->col, "'%s' bit operatoru tamsayi operandlar bekler ('%s', '%s')",
                           token_kind_name(op), mtk_type_name(lt), mtk_type_name(rt));
            }
            e->type = lt;
            return e->type;
        }

        /* Aritmetik: +, -, *, / */
        if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR || op == TOK_SLASH) {
            /* Matris çarpımı: mat3 * vec3, mat4 * vec4, mat3 * mat3, mat4 * mat4 */
            if (op == TOK_STAR && lt == MTK_TYPE_MAT3 && rt == MTK_TYPE_VEC3) {
                e->type = MTK_TYPE_VEC3;
                return e->type;
            }
            if (op == TOK_STAR && lt == MTK_TYPE_MAT4 && rt == MTK_TYPE_VEC4) {
                e->type = MTK_TYPE_VEC4;
                return e->type;
            }
            if (op == TOK_STAR && ((lt == MTK_TYPE_MAT3 && rt == MTK_TYPE_MAT3) ||
                                   (lt == MTK_TYPE_MAT4 && rt == MTK_TYPE_MAT4))) {
                e->type = lt;
                return e->type;
            }

            /* Tamsayı bölme IR'da desteklenmez */
            if (op == TOK_SLASH && mtk_type_is_int(lt)) {
                sema_error(s, e->line, e->col, "tamsayi bolme '/' desteklenmiyor; kayan nokta veya '>>' kullanin");
                e->type = lt;
                return e->type;
            }

            /* Aynı tipte iki skaler/vektör */
            if ((mtk_type_is_float(lt) || mtk_type_is_int(lt)) && lt == rt) {
                e->is_int_lit = l->is_int_lit && r->is_int_lit;
                e->type = lt;
                return e->type;
            }

            /* Vektör ⊕ Skaler veya Skaler ⊕ Vektör (aynı taban tipinde) */
            if (mtk_type_is_vector(lt) && mtk_type_components(rt) == 1) {
                if (r->is_int_lit && mtk_type_is_int(lt)) {
                    r->type = mtk_type_scalar_base(lt);
                    rt = r->type;
                }
                if (mtk_type_scalar_base(lt) == rt) {
                    e->type = lt;
                    return e->type;
                }
            }
            if (mtk_type_is_vector(rt) && mtk_type_components(lt) == 1) {
                if (l->is_int_lit && mtk_type_is_int(rt)) {
                    l->type = mtk_type_scalar_base(rt);
                    lt = l->type;
                }
                if (mtk_type_scalar_base(rt) == lt) {
                    e->type = rt;
                    return e->type;
                }
            }

            sema_error(s, e->line, e->col, "'%s' aritmetik operatoru uyumsuz tipler arasinda ('%s', '%s')",
                       token_kind_name(op), mtk_type_name(lt), mtk_type_name(rt));
            e->type = lt != MTK_TYPE_VOID ? lt : rt;
            return e->type;
        }

        sema_error(s, e->line, e->col, "desteklenmeyen ikili operator '%s'", token_kind_name(op));
        e->type = lt;
        return e->type;
    }

    case AST_EXPR_SWIZZLE: {
        MtkType bt = check_expr(s, e->as.swizzle.base);
        e->is_varying = e->as.swizzle.base->is_varying;
        if (!mtk_type_is_vector(bt)) {
            sema_error(s, e->line, e->col, "'%s' tipi uzerinde swizzle ('.%s') kullanilamaz",
                       mtk_type_name(bt), e->as.swizzle.mask);
            e->type = MTK_TYPE_F32;
            return e->type;
        }
        const char* err = NULL;
        if (!parse_swizzle_mask(e->as.swizzle.mask, e->as.swizzle.len,
                                mtk_type_components(bt), e->as.swizzle.indices, &err)) {
            sema_error(s, e->line, e->col, "%s ('.%s')", err, e->as.swizzle.mask);
            e->type = mtk_type_scalar_base(bt);
            return e->type;
        }
        e->type = mtk_type_make_vector(mtk_type_scalar_base(bt), e->as.swizzle.len);
        return e->type;
    }

    case AST_EXPR_CALL: {
        MtkType ctor_type = MTK_TYPE_VOID;
        if (is_type_ctor_name(e->as.call.callee, &ctor_type)) {
            return check_type_ctor(s, e, ctor_type);
        }

        b32 handled = FALSE;
        MtkType bt = check_builtin_call(s, e, &handled);
        if (handled) return bt;

        /* Kullanıcı tanımlı fonksiyon çağrısı */
        i32 callee_idx = find_fn_index(s->prog, e->as.call.callee);
        if (callee_idx < 0) {
            for (u32 i = 0; i < e->as.call.arg_count; i++) check_expr(s, e->as.call.args[i]);
            sema_error(s, e->line, e->col, "bilinmeyen fonksiyon '%s'", e->as.call.callee);
            e->type = MTK_TYPE_F32;
            return e->type;
        }

        AstDecl* fn = s->prog->decls[callee_idx];
        if (s->cur_fn) {
            i32 caller_idx = find_fn_index(s->prog, s->cur_fn->name);
            if (caller_idx >= 0 && caller_idx < 64 && callee_idx < 64) {
                s->call_edges[caller_idx] |= (1ull << (u32)callee_idx);
                s->call_site_line[caller_idx][callee_idx] = e->line;
                s->call_site_col[caller_idx][callee_idx]  = e->col;
            }
        }

        b32 varying = FALSE;
        for (u32 i = 0; i < e->as.call.arg_count; i++) {
            check_expr(s, e->as.call.args[i]);
            if (e->as.call.args[i]->is_varying) varying = TRUE;
        }
        if (e->as.call.arg_count != fn->as.fn_decl.param_count) {
            sema_error(s, e->line, e->col, "'%s' fonksiyonu %u arguman bekler, %u verildi",
                       fn->name, fn->as.fn_decl.param_count, e->as.call.arg_count);
        } else {
            for (u32 i = 0; i < e->as.call.arg_count; i++) {
                AstExpr* arg = e->as.call.args[i];
                MtkType pt = fn->as.fn_decl.params[i].type;
                if (arg->is_int_lit && (pt == MTK_TYPE_I32 || pt == MTK_TYPE_U32)) {
                    arg->type = pt;
                }
                if (arg->type != pt) {
                    sema_error(s, arg->line, arg->col,
                               "'%s' fonksiyonunun %u. argumani '%s' bekler (bulunan '%s')",
                               fn->name, i + 1, mtk_type_name(pt), mtk_type_name(arg->type));
                }
            }
        }
        e->is_varying = varying;
        e->type = fn->type;
        return e->type;
    }
    }
    return MTK_TYPE_VOID;
}

/* ---------- Deyim denetimi ---------- */

static b32 check_stmt(Sema* s, AstStmt* st);

static b32 check_block(Sema* s, AstStmt* blk) {
    u32 saved_count = s->local_count;
    s->scope_depth++;
    b32 has_return = FALSE;
    for (u32 i = 0; i < blk->as.block.count; i++) {
        if (check_stmt(s, blk->as.block.stmts[i])) {
            has_return = TRUE;
        }
    }
    s->scope_depth--;
    s->local_count = saved_count;
    return has_return;
}

static b32 check_stmt(Sema* s, AstStmt* st) {
    if (!st) return FALSE;

    switch (st->kind) {
    case AST_STMT_BLOCK:
        return check_block(s, st);

    case AST_STMT_VAR_DECL: {
        const char* name = st->as.var_decl.name;
        /* Aynı kapsamda mükerrer isim kontrolü */
        for (i32 i = (i32)s->local_count - 1; i >= 0; i--) {
            if (s->locals[i].scope_depth < s->scope_depth) break;
            if (str_eq(s->locals[i].name, name)) {
                sema_error(s, st->line, st->col, "'%s' degiskeni bu kapsamda zaten tanimli", name);
                break;
            }
        }
        MtkType init_t = check_expr(s, st->as.var_decl.init);
        MtkType decl_t = st->as.var_decl.decl_type;
        if (decl_t != MTK_TYPE_VOID) {
            if (decl_t == MTK_TYPE_TEXTURE2D || mtk_type_is_matrix(decl_t)) {
                sema_error(s, st->as.var_decl.type_line, st->as.var_decl.type_col,
                           "yerel degisken tipi '%s' olamaz", mtk_type_name(decl_t));
            }
            if (st->as.var_decl.init && st->as.var_decl.init->is_int_lit &&
                (decl_t == MTK_TYPE_I32 || decl_t == MTK_TYPE_U32)) {
                st->as.var_decl.init->type = decl_t;
                init_t = decl_t;
            }
            if (init_t != decl_t) {
                sema_error(s, st->line, st->col,
                           "'%s' degiskeni '%s' olarak bildirildi ancak '%s' ile ilklendirildi",
                           name, mtk_type_name(decl_t), mtk_type_name(init_t));
            }
        } else {
            if (init_t == MTK_TYPE_VOID || init_t == MTK_TYPE_TEXTURE2D || mtk_type_is_matrix(init_t)) {
                sema_error(s, st->line, st->col, "'%s' degiskeni gecersiz '%s' tipiyle ilklendirilemez",
                           name, mtk_type_name(init_t));
                decl_t = MTK_TYPE_F32;
            } else {
                decl_t = init_t;
            }
            st->as.var_decl.decl_type = decl_t;
        }

        if (s->local_count < 128) {
            LocalVar* lv = &s->locals[s->local_count++];
            copy_name(lv->name, name);
            lv->type        = decl_t;
            lv->is_mutable  = st->as.var_decl.is_mutable;
            lv->is_varying  = (st->as.var_decl.init && st->as.var_decl.init->is_varying) || s->in_varying_cond;
            lv->scope_depth = s->scope_depth;
        }
        return FALSE;
    }

    case AST_STMT_ASSIGN: {
        AstExpr* target = st->as.assign.target;
        AstExpr* val    = st->as.assign.value;
        MtkType lt = check_expr(s, target);
        MtkType rt = check_expr(s, val);

        AstExpr* base_ident = target;
        if (target->kind == AST_EXPR_SWIZZLE) {
            base_ident = target->as.swizzle.base;
            /* Swizzle yazma maskesinde tekrarlı bileşen kontrolü (ör. v.xx = ...) */
            for (u32 i = 0; i < target->as.swizzle.len; i++) {
                for (u32 j = i + 1; j < target->as.swizzle.len; j++) {
                    if (target->as.swizzle.indices[i] == target->as.swizzle.indices[j]) {
                        sema_error(s, target->line, target->col,
                                   "atama sol tarafindaki swizzle ('.%s') tekrarli bilesen iceremez",
                                   target->as.swizzle.mask);
                    }
                }
            }
        }

        if (!base_ident || base_ident->kind != AST_EXPR_IDENT) {
            sema_error(s, st->line, st->col, "atama sol tarafi gecerli bir degisken degil");
            return FALSE;
        }

        i32 loc = find_local(s, base_ident->as.ident.name);
        if (loc < 0) {
            AstDecl* d = find_top_decl(s->prog, base_ident->as.ident.name, s->prog->decl_count);
            if (d) {
                sema_error(s, st->line, st->col, "salt-okunur '%s' bildirimine atama yapilamaz", d->name);
            }
            return FALSE;
        }
        if (!s->locals[loc].is_mutable) {
            sema_error(s, st->line, st->col, "'let' veya parametre olarak tanimlanan '%s' degistirilemez",
                       s->locals[loc].name);
        }

        if (val->is_int_lit && (lt == MTK_TYPE_I32 || lt == MTK_TYPE_U32)) {
            val->type = lt;
            rt = lt;
        }

        TokenKind op = st->as.assign.op;
        if (op == TOK_ASSIGN) {
            if (lt != rt) {
                sema_error(s, st->line, st->col, "'%s' tipindeki hedefe '%s' atanamaz",
                           mtk_type_name(lt), mtk_type_name(rt));
            }
        } else {
            /* +=, -=, *=, /= */
            b32 vec_scalar = mtk_type_is_vector(lt) && mtk_type_scalar_base(lt) == rt;
            if (lt != rt && !vec_scalar) {
                sema_error(s, st->line, st->col, "'%s' birlesik atamasi uyumsuz tipler arasinda ('%s', '%s')",
                           token_kind_name(op), mtk_type_name(lt), mtk_type_name(rt));
            }
        }

        if (val->is_varying || s->in_varying_cond) {
            s->locals[loc].is_varying = TRUE;
        }
        return FALSE;
    }

    case AST_STMT_IF: {
        MtkType ct = check_expr(s, st->as.if_stmt.cond);
        if (ct != MTK_TYPE_BOOL) {
            sema_error(s, st->as.if_stmt.cond->line, st->as.if_stmt.cond->col,
                       "if kosulu 'bool' tipinde olmalidir (bulunan '%s')", mtk_type_name(ct));
        }
        b32 prev_var = s->in_varying_cond;
        if (st->as.if_stmt.cond->is_varying) s->in_varying_cond = TRUE;
        b32 r_then = check_stmt(s, st->as.if_stmt.then_branch);
        b32 r_else = st->as.if_stmt.else_branch ? check_stmt(s, st->as.if_stmt.else_branch) : FALSE;
        s->in_varying_cond = prev_var;
        return r_then && r_else;
    }

    case AST_STMT_FOR: {
        f32 sf = 0.0f, ef = 0.0f;
        i32 si = 0, ei = 0;
        MtkType st_t = MTK_TYPE_VOID, end_t = MTK_TYPE_VOID;
        check_expr(s, st->as.for_stmt.start_expr);
        check_expr(s, st->as.for_stmt.end_expr);
        b32 ok_s = eval_const_scalar(s, st->as.for_stmt.start_expr, &sf, &si, &st_t);
        b32 ok_e = eval_const_scalar(s, st->as.for_stmt.end_expr, &ef, &ei, &end_t);
        if (!ok_s || !ok_e || (st_t != MTK_TYPE_I32 && st_t != MTK_TYPE_U32) ||
            (end_t != MTK_TYPE_I32 && end_t != MTK_TYPE_U32)) {
            sema_error(s, st->line, st->col, "for dongusu sinirlari derleme zamani tamsayi sabiti olmalidir");
            si = 0; ei = 0;
        }
        st->as.for_stmt.const_start = si;
        st->as.for_stmt.const_end   = ei;

        u32 saved = s->local_count;
        s->scope_depth++;
        if (s->local_count < 128) {
            LocalVar* lv = &s->locals[s->local_count++];
            copy_name(lv->name, st->as.for_stmt.var_name);
            lv->type        = MTK_TYPE_I32;
            lv->is_mutable  = FALSE;
            lv->is_varying  = FALSE;
            lv->scope_depth = s->scope_depth;
        }
        check_stmt(s, st->as.for_stmt.body);
        s->scope_depth--;
        s->local_count = saved;
        return FALSE;
    }

    case AST_STMT_DISCARD:
        return FALSE;

    case AST_STMT_RETURN: {
        MtkType fn_ret = s->cur_fn ? s->cur_fn->type : MTK_TYPE_VOID;
        if (st->as.ret.value) {
            MtkType vt = check_expr(s, st->as.ret.value);
            if (st->as.ret.value->is_int_lit && (fn_ret == MTK_TYPE_I32 || fn_ret == MTK_TYPE_U32)) {
                st->as.ret.value->type = fn_ret;
                vt = fn_ret;
            }
            if (vt != fn_ret) {
                sema_error(s, st->line, st->col,
                           "'%s' fonksiyonu '%s' dondurmeli, ancak '%s' donduruldu",
                           s->cur_fn ? s->cur_fn->name : "?", mtk_type_name(fn_ret), mtk_type_name(vt));
            }
        } else if (fn_ret != MTK_TYPE_VOID) {
            sema_error(s, st->line, st->col, "'%s' fonksiyonu '%s' tipinde deger dondurmelidir",
                       s->cur_fn ? s->cur_fn->name : "?", mtk_type_name(fn_ret));
        }
        return TRUE;
    }

    case AST_STMT_EXPR:
        check_expr(s, st->as.expr_stmt.expr);
        return FALSE;
    }
    return FALSE;
}

/* ---------- Özyineleme tespiti (DFS 3-renk) ---------- */

static void detect_recursion_dfs(Sema* s, u32 u, u8 state[64]) {
    state[u] = 1; /* visiting */
    u64 mask = s->call_edges[u];
    for (u32 v = 0; v < s->prog->decl_count && v < 64; v++) {
        if (!(mask & (1ull << v))) continue;
        if (state[v] == 1) {
            sema_error(s, s->call_site_line[u][v], s->call_site_col[u][v],
                       "ozyinelemeli fonksiyon cagrisi yasaktir ('%s' -> '%s')",
                       s->prog->decls[u]->name, s->prog->decls[v]->name);
        } else if (state[v] == 0) {
            detect_recursion_dfs(s, v, state);
        }
    }
    state[u] = 2; /* visited */
}

b32 sema_check(AstProgram* prog, const char* src, u32 src_len, const char* filename,
               Arena* arena, MtkLayout* out_layout, MtkDiag* diag) {
    Sema s;
    __builtin_memset(&s, 0, sizeof(s));
    s.prog     = prog;
    s.src      = src;
    s.src_len  = src_len;
    s.filename = filename ? filename : "<shader>";
    s.arena    = arena;
    s.layout   = out_layout;
    s.diag     = diag;

    if (out_layout) {
        __builtin_memset(out_layout, 0, sizeof(*out_layout));
    }

    /* 1. Üst seviye bildirimleri ve yerleşimi doğrula */
    AstDecl* frag_fn = NULL;
    for (u32 i = 0; i < prog->decl_count; i++) {
        AstDecl* d = prog->decls[i];

        /* Mükerrer üst seviye isim kontrolü */
        AstDecl* prev = find_top_decl(prog, d->name, i);
        if (prev) {
            sema_error(&s, d->line, d->col, "'%s' ismi daha once satir %u'de tanimlandi",
                       d->name, prev->line);
        }

        if (d->kind == AST_DECL_UNIFORM) {
            if (d->type == MTK_TYPE_VOID) {
                sema_error(&s, d->line, d->col, "uniform '%s' gecersiz tipe sahip", d->name);
                continue;
            }
            if (d->type == MTK_TYPE_TEXTURE2D) {
                u32 slot = out_layout ? out_layout->texture_count : 0;
                if (slot >= SHADE_MAX_TEXTURES) {
                    sema_error(&s, d->line, d->col, "en fazla %u texture2d tanimlanabilir", SHADE_MAX_TEXTURES);
                }
                d->as.uniform_decl.offset_or_slot = slot;
                if (out_layout) {
                    out_layout->texture_count++;
                    if (out_layout->sym_count < 64) {
                        MtkSymbol* sym = &out_layout->syms[out_layout->sym_count++];
                        copy_name(sym->name, d->name);
                        sym->kind            = MTK_SYM_TEXTURE;
                        sym->type            = d->type;
                        sym->offset_or_index = slot;
                        sym->components      = 1;
                    }
                }
            } else {
                u32 comps = mtk_type_components(d->type);
                u32 cur_off = out_layout ? ALIGN_UP(out_layout->uniform_bytes, 16u) : 0;
                u32 byte_sz = ALIGN_UP(comps * 4u, 16u);
                d->as.uniform_decl.offset_or_slot = cur_off;
                if (out_layout) {
                    out_layout->uniform_bytes = cur_off + byte_sz;
                    if (out_layout->sym_count < 64) {
                        MtkSymbol* sym = &out_layout->syms[out_layout->sym_count++];
                        copy_name(sym->name, d->name);
                        sym->kind            = MTK_SYM_UNIFORM;
                        sym->type            = d->type;
                        sym->offset_or_index = cur_off;
                        sym->components      = comps;
                    }
                }
            }
        } else if (d->kind == AST_DECL_IN) {
            if (!mtk_type_is_float(d->type)) {
                sema_error(&s, d->type_line, d->type_col,
                           "varying girdi '%s' tipi f32 veya vec2/vec3/vec4 olmalidir (bulunan '%s')",
                           d->name, mtk_type_name(d->type));
            }
            u32 comps = mtk_type_components(d->type);
            u32 idx   = out_layout ? out_layout->varying_count : 0;
            d->as.in_decl.varying_index = idx;
            if (idx + comps > SHADE_MAX_VARYINGS) {
                sema_error(&s, d->line, d->col, "toplam skaler varying sayisi %u sinirini asiyor",
                           SHADE_MAX_VARYINGS);
            }
            if (out_layout) {
                out_layout->varying_count += comps;
                if (out_layout->sym_count < 64) {
                    MtkSymbol* sym = &out_layout->syms[out_layout->sym_count++];
                    copy_name(sym->name, d->name);
                    sym->kind            = MTK_SYM_VARYING;
                    sym->type            = d->type;
                    sym->offset_or_index = idx;
                    sym->components      = comps;
                }
            }
        } else if (d->kind == AST_DECL_CONST) {
            AstExpr* val = d->as.const_decl.value;
            MtkType vt = check_expr(&s, val);
            if (d->type != MTK_TYPE_VOID) {
                if (val && val->is_int_lit && (d->type == MTK_TYPE_I32 || d->type == MTK_TYPE_U32)) {
                    val->type = d->type;
                    vt = d->type;
                }
                if (vt != d->type) {
                    sema_error(&s, d->line, d->col,
                               "const '%s' tipi '%s' ancak degeri '%s'",
                               d->name, mtk_type_name(d->type), mtk_type_name(vt));
                }
            } else {
                d->type = vt;
            }
            if (val && val->is_varying) {
                sema_error(&s, d->line, d->col, "const '%s' varying ifade iceremez", d->name);
            }
            f32 cf = 0.0f; i32 ci = 0; MtkType ct = MTK_TYPE_VOID;
            if (mtk_type_components(d->type) == 1) {
                if (!eval_const_scalar(&s, val, &cf, &ci, &ct)) {
                    sema_error(&s, d->line, d->col, "const '%s' derleme zamani sabiti olmalidir", d->name);
                }
                d->as.const_decl.f_vals[0] = cf;
                d->as.const_decl.i_vals[0] = ci;
            }
        } else if (d->kind == AST_DECL_FN) {
            if (str_eq(d->name, "frag")) {
                frag_fn = d;
            }
        }
    }

    /* 2. Fonksiyon gövdelerini kontrol et */
    for (u32 i = 0; i < prog->decl_count; i++) {
        AstDecl* d = prog->decls[i];
        if (d->kind != AST_DECL_FN) continue;

        s.cur_fn          = d;
        s.local_count     = 0;
        s.scope_depth     = 1;
        s.in_varying_cond = FALSE;

        if (d->type == MTK_TYPE_TEXTURE2D || mtk_type_is_matrix(d->type)) {
            sema_error(&s, d->type_line, d->type_col,
                       "fonksiyon donus tipi '%s' olamaz", mtk_type_name(d->type));
        }

        for (u32 p = 0; p < d->as.fn_decl.param_count; p++) {
            AstParam* prm = &d->as.fn_decl.params[p];
            if (prm->type == MTK_TYPE_VOID || prm->type == MTK_TYPE_TEXTURE2D ||
                mtk_type_is_matrix(prm->type)) {
                sema_error(&s, prm->line, prm->col,
                           "parametre '%s' gecersiz '%s' tipinde olamaz",
                           prm->name, mtk_type_name(prm->type));
            }
            for (u32 q = 0; q < p; q++) {
                if (str_eq(d->as.fn_decl.params[q].name, prm->name)) {
                    sema_error(&s, prm->line, prm->col, "mükerrer parametre ismi '%s'", prm->name);
                }
            }
            LocalVar* lv = &s.locals[s.local_count++];
            copy_name(lv->name, prm->name);
            lv->type        = prm->type;
            lv->is_mutable  = FALSE;
            lv->is_varying  = TRUE;
            lv->scope_depth = 1;
        }

        b32 has_ret = check_stmt(&s, d->as.fn_decl.body);
        if (d->type != MTK_TYPE_VOID && !has_ret) {
            sema_error(&s, d->line, d->col,
                       "'%s' fonksiyonu tum kontrol yollarinda '%s' dondurmelidir",
                       d->name, mtk_type_name(d->type));
        }
    }

    /* 3. Özyineleme kontrolü */
    u8 visit_state[64];
    __builtin_memset(visit_state, 0, sizeof(visit_state));
    for (u32 i = 0; i < prog->decl_count && i < 64; i++) {
        if (prog->decls[i]->kind == AST_DECL_FN && visit_state[i] == 0) {
            detect_recursion_dfs(&s, i, visit_state);
        }
    }

    /* 4. Giriş noktası 'fn frag() -> rgba' kontrolü */
    if (!frag_fn) {
        sema_error(&s, 1, 1, "giris noktasi 'fn frag() -> rgba' bulunamadi");
    } else {
        if (frag_fn->as.fn_decl.param_count != 0) {
            sema_error(&s, frag_fn->line, frag_fn->col,
                       "'frag' giris noktasi parametre alamaz");
        }
        if (frag_fn->type != MTK_TYPE_VEC4) {
            sema_error(&s, frag_fn->type_line, frag_fn->type_col,
                       "'frag' giris noktasi 'rgba' (vec4) dondurmelidir (bulunan '%s')",
                       mtk_type_name(frag_fn->type));
        }
    }

    return (!diag || diag->count == 0);
}
