/* matoki/parser.c — Matoki özyinelemeli iniş ve Pratt ifade çözümleyicisi.
 * Tüm AST düğümlerini verilen Arena üzerinde ayırır; hata kurtarma ile
 * ilk hatada durmadan bildirim ve deyim sınırlarında toparlanır.
 */
#include "matoki/parser.h"
#include "core/log.h"
#include "raster/shade_abi.h"

typedef struct {
    Lexer       lex;
    const char* src;
    u32         src_len;
    const char* filename;
    Arena*      arena;
    MtkDiag*    diag;
    Token       cur;
    Token       prev;
    b32         panic_mode;
} Parser;

static void copy_name(char dst[32], const char* src, u32 len) {
    u32 n = len < 31 ? len : 31;
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

static b32 str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void parser_error_at(Parser* p, const Token* t, const char* fmt, ...) {
    if (p->panic_mode) return;
    p->panic_mode = TRUE;
    if (!p->diag || p->diag->count >= MTK_MAX_ERRORS) return;

    char msg[256];
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(msg, sizeof(msg), fmt, ap);
    VA_END(ap);

    mtk_diag_error(p->diag, p->filename, p->src, p->src_len, t->line, t->col, "%s", msg);
}

static void parser_advance(Parser* p) {
    p->prev = p->cur;
    for (;;) {
        p->cur = lexer_next(&p->lex);
        if (p->cur.kind != TOK_ERROR) break;
        if (!p->panic_mode && p->diag && p->diag->count < MTK_MAX_ERRORS) {
            mtk_diag_error(p->diag, p->filename, p->src, p->src_len,
                           p->cur.line, p->cur.col, "%s",
                           p->cur.err_msg ? p->cur.err_msg : "sozcuk hatasi");
        }
        p->panic_mode = TRUE;
    }
}

static b32 parser_check(const Parser* p, TokenKind k) {
    return p->cur.kind == k;
}

static b32 parser_match(Parser* p, TokenKind k) {
    if (p->cur.kind != k) return FALSE;
    parser_advance(p);
    return TRUE;
}

static b32 parser_consume(Parser* p, TokenKind k, const char* err_msg) {
    if (p->cur.kind == k) {
        parser_advance(p);
        return TRUE;
    }
    parser_error_at(p, &p->cur, "%s (beklenen '%s')", err_msg, token_kind_name(k));
    return FALSE;
}

static void sync_stmt(Parser* p) {
    p->panic_mode = FALSE;
    while (p->cur.kind != TOK_EOF) {
        if (p->prev.kind == TOK_SEMI) return;
        switch (p->cur.kind) {
        case TOK_RBRACE:
        case TOK_LET:
        case TOK_VAR:
        case TOK_IF:
        case TOK_FOR:
        case TOK_RETURN:
        case TOK_DISCARD:
        case TOK_FN:
        case TOK_UNIFORM:
        case TOK_IN:
        case TOK_CONST:
            return;
        default:
            break;
        }
        parser_advance(p);
    }
}

static void sync_decl(Parser* p) {
    p->panic_mode = FALSE;
    while (p->cur.kind != TOK_EOF) {
        if (p->prev.kind == TOK_SEMI || p->prev.kind == TOK_RBRACE) return;
        switch (p->cur.kind) {
        case TOK_FN:
        case TOK_UNIFORM:
        case TOK_IN:
        case TOK_CONST:
            return;
        default:
            break;
        }
        parser_advance(p);
    }
}

static MtkType parse_type_name(Parser* p, u32* out_line, u32* out_col) {
    if (out_line) *out_line = p->cur.line;
    if (out_col)  *out_col  = p->cur.col;
    if (p->cur.kind != TOK_IDENT) {
        parser_error_at(p, &p->cur, "tip ismi bekleniyordu");
        return MTK_TYPE_VOID;
    }
    char name[32];
    copy_name(name, p->cur.start, p->cur.len);
    Token tok = p->cur;
    parser_advance(p);

    if (str_eq(name, "f32"))       return MTK_TYPE_F32;
    if (str_eq(name, "i32"))       return MTK_TYPE_I32;
    if (str_eq(name, "u32"))       return MTK_TYPE_U32;
    if (str_eq(name, "bool"))      return MTK_TYPE_BOOL;
    if (str_eq(name, "vec2"))      return MTK_TYPE_VEC2;
    if (str_eq(name, "vec3"))      return MTK_TYPE_VEC3;
    if (str_eq(name, "vec4"))      return MTK_TYPE_VEC4;
    if (str_eq(name, "rgba"))      return MTK_TYPE_VEC4;
    if (str_eq(name, "ivec2"))     return MTK_TYPE_IVEC2;
    if (str_eq(name, "ivec3"))     return MTK_TYPE_IVEC3;
    if (str_eq(name, "ivec4"))     return MTK_TYPE_IVEC4;
    if (str_eq(name, "mat3"))          return MTK_TYPE_MAT3;
    if (str_eq(name, "mat4"))          return MTK_TYPE_MAT4;
    if (str_eq(name, "texture2d"))     return MTK_TYPE_TEXTURE2D;
    if (str_eq(name, "texture2d_f32")) return MTK_TYPE_TEXTURE2D_F32;
    if (str_eq(name, "void"))          return MTK_TYPE_VOID;
    if (str_eq(name, "buffer")) {
        if (!parser_consume(p, TOK_LT, "'buffer' sonrasinda '<' bekleniyordu")) {
            return MTK_TYPE_VOID;
        }
        u32 el_line = 0, el_col = 0;
        Token el_tok = p->cur;
        MtkType elem = parse_type_name(p, &el_line, &el_col);
        parser_consume(p, TOK_GT, "buffer<T> tipinde '>' bekleniyordu");
        if (elem == MTK_TYPE_F32)  return MTK_TYPE_BUF_F32;
        if (elem == MTK_TYPE_U32)  return MTK_TYPE_BUF_U32;
        if (elem == MTK_TYPE_VEC4) return MTK_TYPE_BUF_VEC4;
        parser_error_at(p, &el_tok, "buffer eleman tipi 'f32', 'u32' veya 'vec4' olmali");
        return MTK_TYPE_VOID;
    }

    parser_error_at(p, &tok, "bilinmeyen tip '%s'", name);
    return MTK_TYPE_VOID;
}

/* ---------- Pratt ifade çözümleyicisi ---------- */

static AstExpr* parse_expr_prec(Parser* p, i32 min_prec);
static AstExpr* parse_expr(Parser* p);

static AstExpr* new_expr(Parser* p, AstExprKind kind, u32 line, u32 col) {
    AstExpr* e = ARENA_PUSH_STRUCT(p->arena, AstExpr);
    e->kind = kind;
    e->line = line;
    e->col  = col;
    e->type = MTK_TYPE_VOID;
    return e;
}

static i32 infix_precedence(TokenKind k) {
    switch (k) {
    case TOK_OR_OR:   return 1;
    case TOK_AND_AND: return 2;
    case TOK_PIPE:    return 3;
    case TOK_CARET:   return 4;
    case TOK_AMP:     return 5;
    case TOK_EQ:
    case TOK_NE:      return 6;
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:      return 7;
    case TOK_SHL:
    case TOK_SHR:     return 8;
    case TOK_PLUS:
    case TOK_MINUS:   return 9;
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT: return 10;
    default:          return 0;
    }
}

static AstExpr* parse_primary(Parser* p) {
    Token t = p->cur;

    if (parser_match(p, TOK_INT_LIT)) {
        AstExpr* e = new_expr(p, AST_EXPR_INT, t.line, t.col);
        e->as.i_val   = t.i_val;
        e->is_int_lit = TRUE;
        return e;
    }

    if (parser_match(p, TOK_FLOAT_LIT)) {
        AstExpr* e = new_expr(p, AST_EXPR_FLOAT, t.line, t.col);
        e->as.f_val = t.f_val;
        return e;
    }

    if (parser_match(p, TOK_TRUE)) {
        AstExpr* e = new_expr(p, AST_EXPR_BOOL, t.line, t.col);
        e->as.b_val = TRUE;
        return e;
    }

    if (parser_match(p, TOK_FALSE)) {
        AstExpr* e = new_expr(p, AST_EXPR_BOOL, t.line, t.col);
        e->as.b_val = FALSE;
        return e;
    }

    if (parser_match(p, TOK_IDENT)) {
        if (parser_match(p, TOK_LPAREN)) {
            AstExpr* e = new_expr(p, AST_EXPR_CALL, t.line, t.col);
            copy_name(e->as.call.callee, t.start, t.len);
            e->as.call.arg_count = 0;
            if (!parser_check(p, TOK_RPAREN)) {
                do {
                    AstExpr* arg = parse_expr(p);
                    if (e->as.call.arg_count < 8) {
                        e->as.call.args[e->as.call.arg_count++] = arg;
                    } else {
                        parser_error_at(p, &p->cur, "fonksiyon cagrisinda en fazla 8 arguman olabilir");
                    }
                } while (parser_match(p, TOK_COMMA));
            }
            parser_consume(p, TOK_RPAREN, "arguman listesinden sonra ')' bekleniyordu");
            return e;
        }
        AstExpr* e = new_expr(p, AST_EXPR_IDENT, t.line, t.col);
        copy_name(e->as.ident.name, t.start, t.len);
        return e;
    }

    if (parser_match(p, TOK_LPAREN)) {
        AstExpr* inner = parse_expr(p);
        parser_consume(p, TOK_RPAREN, "ifadeden sonra ')' bekleniyordu");
        return inner;
    }

    if (t.kind == TOK_MINUS || t.kind == TOK_PLUS || t.kind == TOK_BANG || t.kind == TOK_TILDE) {
        parser_advance(p);
        AstExpr* operand = parse_expr_prec(p, 11);
        AstExpr* e = new_expr(p, AST_EXPR_UNARY, t.line, t.col);
        e->as.unary.op      = t.kind;
        e->as.unary.operand = operand;
        return e;
    }

    parser_error_at(p, &t, "ifade bekleniyordu (bulunan '%s')", token_kind_name(t.kind));
    if (p->cur.kind != TOK_EOF && p->cur.kind != TOK_SEMI && p->cur.kind != TOK_RBRACE) {
        parser_advance(p);
    }
    return new_expr(p, AST_EXPR_INT, t.line, t.col);
}

static AstExpr* parse_postfix(Parser* p) {
    AstExpr* expr = parse_primary(p);
    for (;;) {
        if (parser_match(p, TOK_DOT)) {
            Token member = p->cur;
            if (!parser_consume(p, TOK_IDENT, "'.' sonrasinda bilesen/swizzle ismi bekleniyordu")) {
                break;
            }
            AstExpr* swz = new_expr(p, AST_EXPR_SWIZZLE, member.line, member.col);
            swz->as.swizzle.base = expr;
            copy_name(swz->as.swizzle.mask, member.start, member.len < 7 ? member.len : 7);
            swz->as.swizzle.len = member.len;
            expr = swz;
        } else if (parser_match(p, TOK_LBRACKET)) {
            Token lbr = p->prev;
            AstExpr* idx = parse_expr(p);
            parser_consume(p, TOK_RBRACKET, "indeks ifadesinden sonra ']' bekleniyordu");
            AstExpr* ie = new_expr(p, AST_EXPR_INDEX, lbr.line, lbr.col);
            ie->as.index.base  = expr;
            ie->as.index.index = idx;
            expr = ie;
        } else {
            break;
        }
    }
    return expr;
}

static AstExpr* parse_expr_prec(Parser* p, i32 min_prec) {
    AstExpr* left = parse_postfix(p);
    for (;;) {
        i32 prec = infix_precedence(p->cur.kind);
        if (prec < min_prec) break;
        Token op_tok = p->cur;
        parser_advance(p);
        AstExpr* right = parse_expr_prec(p, prec + 1);
        AstExpr* bin = new_expr(p, AST_EXPR_BINARY, op_tok.line, op_tok.col);
        bin->as.binary.op    = op_tok.kind;
        bin->as.binary.left  = left;
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static AstExpr* parse_expr(Parser* p) {
    return parse_expr_prec(p, 1);
}

/* ---------- Deyim çözümleyicisi ---------- */

static AstStmt* parse_stmt(Parser* p);
static AstStmt* parse_block(Parser* p);

static AstStmt* new_stmt(Parser* p, AstStmtKind kind, u32 line, u32 col) {
    AstStmt* s = ARENA_PUSH_STRUCT(p->arena, AstStmt);
    s->kind = kind;
    s->line = line;
    s->col  = col;
    return s;
}

static AstStmt* parse_var_decl(Parser* p, b32 is_mutable) {
    Token kw = p->prev;
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "degisken ismi bekleniyordu")) {
        sync_stmt(p);
        return new_stmt(p, AST_STMT_VAR_DECL, kw.line, kw.col);
    }

    AstStmt* s = new_stmt(p, AST_STMT_VAR_DECL, name_tok.line, name_tok.col);
    s->as.var_decl.is_mutable = is_mutable;
    copy_name(s->as.var_decl.name, name_tok.start, name_tok.len);
    s->as.var_decl.decl_type  = MTK_TYPE_VOID;

    if (parser_match(p, TOK_COLON)) {
        s->as.var_decl.decl_type = parse_type_name(p, &s->as.var_decl.type_line, &s->as.var_decl.type_col);
    }

    if (parser_consume(p, TOK_ASSIGN, "degisken taniminda '=' bekleniyordu")) {
        s->as.var_decl.init = parse_expr(p);
    }

    if (!parser_consume(p, TOK_SEMI, "degisken tanimindan sonra ';' bekleniyordu")) {
        sync_stmt(p);
    }
    return s;
}

static AstStmt* parse_if_stmt(Parser* p) {
    Token kw = p->prev;
    AstStmt* s = new_stmt(p, AST_STMT_IF, kw.line, kw.col);
    s->as.if_stmt.cond = parse_expr(p);
    if (!parser_consume(p, TOK_LBRACE, "if kosulundan sonra '{' bekleniyordu")) {
        sync_stmt(p);
        return s;
    }
    s->as.if_stmt.then_branch = parse_block(p);
    if (parser_match(p, TOK_ELSE)) {
        if (parser_match(p, TOK_IF)) {
            s->as.if_stmt.else_branch = parse_if_stmt(p);
        } else if (parser_consume(p, TOK_LBRACE, "else sonrasinda '{' veya 'if' bekleniyordu")) {
            s->as.if_stmt.else_branch = parse_block(p);
        } else {
            sync_stmt(p);
        }
    }
    return s;
}

static AstStmt* parse_for_stmt(Parser* p) {
    Token kw = p->prev;
    AstStmt* s = new_stmt(p, AST_STMT_FOR, kw.line, kw.col);
    Token var_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "for dongusunde sayac degiskeni bekleniyordu")) {
        sync_stmt(p);
        return s;
    }
    copy_name(s->as.for_stmt.var_name, var_tok.start, var_tok.len);
    if (!parser_consume(p, TOK_IN, "for sayacindan sonra 'in' bekleniyordu")) {
        sync_stmt(p);
        return s;
    }
    s->as.for_stmt.start_expr = parse_expr(p);
    if (!parser_consume(p, TOK_DOTDOT, "for araliginda '..' bekleniyordu")) {
        sync_stmt(p);
        return s;
    }
    s->as.for_stmt.end_expr = parse_expr(p);
    if (!parser_consume(p, TOK_LBRACE, "for basligindan sonra '{' bekleniyordu")) {
        sync_stmt(p);
        return s;
    }
    s->as.for_stmt.body = parse_block(p);
    return s;
}

static AstStmt* parse_block(Parser* p) {
    Token lbrace = p->prev;
    AstStmt* temp[256];
    u32 count = 0;

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstStmt* st = parse_stmt(p);
        if (st && count < 256) {
            temp[count++] = st;
        }
        if (p->panic_mode) {
            sync_stmt(p);
        }
    }
    parser_consume(p, TOK_RBRACE, "blok sonunda '}' bekleniyordu");

    AstStmt* blk = new_stmt(p, AST_STMT_BLOCK, lbrace.line, lbrace.col);
    blk->as.block.count = count;
    if (count > 0) {
        blk->as.block.stmts = ARENA_PUSH_ARRAY(p->arena, AstStmt*, count);
        for (u32 i = 0; i < count; i++) blk->as.block.stmts[i] = temp[i];
    }
    return blk;
}

static b32 is_assign_op(TokenKind k) {
    return k == TOK_ASSIGN || k == TOK_PLUS_ASSIGN || k == TOK_MINUS_ASSIGN ||
           k == TOK_STAR_ASSIGN || k == TOK_SLASH_ASSIGN;
}

static AstStmt* parse_stmt(Parser* p) {
    if (parser_match(p, TOK_LBRACE))  return parse_block(p);
    if (parser_match(p, TOK_LET))     return parse_var_decl(p, FALSE);
    if (parser_match(p, TOK_VAR))     return parse_var_decl(p, TRUE);
    if (parser_match(p, TOK_IF))      return parse_if_stmt(p);
    if (parser_match(p, TOK_FOR))     return parse_for_stmt(p);

    if (parser_match(p, TOK_DISCARD)) {
        Token kw = p->prev;
        AstStmt* s = new_stmt(p, AST_STMT_DISCARD, kw.line, kw.col);
        if (!parser_consume(p, TOK_SEMI, "'discard' sonrasinda ';' bekleniyordu")) {
            sync_stmt(p);
        }
        return s;
    }

    if (parser_match(p, TOK_RETURN)) {
        Token kw = p->prev;
        AstStmt* s = new_stmt(p, AST_STMT_RETURN, kw.line, kw.col);
        if (!parser_check(p, TOK_SEMI)) {
            s->as.ret.value = parse_expr(p);
        }
        if (!parser_consume(p, TOK_SEMI, "'return' deyiminden sonra ';' bekleniyordu")) {
            sync_stmt(p);
        }
        return s;
    }

    AstExpr* expr = parse_expr(p);
    if (is_assign_op(p->cur.kind)) {
        Token op_tok = p->cur;
        parser_advance(p);
        AstExpr* val = parse_expr(p);
        AstStmt* s = new_stmt(p, AST_STMT_ASSIGN, op_tok.line, op_tok.col);
        s->as.assign.op     = op_tok.kind;
        s->as.assign.target = expr;
        s->as.assign.value  = val;
        if (!parser_consume(p, TOK_SEMI, "atama deyiminden sonra ';' bekleniyordu")) {
            sync_stmt(p);
        }
        return s;
    }

    if (!parser_consume(p, TOK_SEMI, "deyimden sonra ';' bekleniyordu")) {
        sync_stmt(p);
    }
    AstStmt* s = new_stmt(p, AST_STMT_EXPR, expr->line, expr->col);
    s->as.expr_stmt.expr = expr;
    return s;
}

/* ---------- Üst seviye bildirim çözümleyicisi ---------- */

static AstDecl* new_decl(Parser* p, AstDeclKind kind, u32 line, u32 col) {
    AstDecl* d = ARENA_PUSH_STRUCT(p->arena, AstDecl);
    d->kind = kind;
    d->line = line;
    d->col  = col;
    return d;
}

static AstDecl* parse_uniform_decl(Parser* p) {
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "'uniform' sonrasinda isim bekleniyordu")) {
        sync_decl(p);
        return NULL;
    }
    AstDecl* d = new_decl(p, AST_DECL_UNIFORM, name_tok.line, name_tok.col);
    copy_name(d->name, name_tok.start, name_tok.len);

    if (!parser_consume(p, TOK_COLON, "uniform isminden sonra ':' bekleniyordu")) {
        sync_decl(p);
        return d;
    }
    d->type = parse_type_name(p, &d->type_line, &d->type_col);
    if (mtk_type_is_buffer(d->type)) {
        d->kind = AST_DECL_BUFFER;
    } else if (d->type == MTK_TYPE_TEXTURE2D_F32) {
        d->as.uniform_decl.tex_flags |= TEX_FLAG_F32;
    }

    /* Opsiyonel @bilinear, @clamp, @nearest, @wrap, @mipmap nitelikleri */
    while (parser_match(p, TOK_AT)) {
        Token attr_tok = p->cur;
        if (!parser_consume(p, TOK_IDENT, "'@' sonrasinda nitelik ismi bekleniyordu")) {
            break;
        }
        char attr[32];
        copy_name(attr, attr_tok.start, attr_tok.len);
        if (!mtk_type_is_texture(d->type)) {
            parser_error_at(p, &attr_tok, "ornekleme niteligi '@%s' yalnizca texture2d tipinde kullanilabilir", attr);
        } else if (str_eq(attr, "bilinear")) {
            d->as.uniform_decl.tex_flags |= TEX_FLAG_BILINEAR;
        } else if (str_eq(attr, "clamp")) {
            d->as.uniform_decl.tex_flags |= TEX_FLAG_CLAMP;
        } else if (str_eq(attr, "nearest")) {
            d->as.uniform_decl.tex_flags &= ~TEX_FLAG_BILINEAR;
        } else if (str_eq(attr, "wrap")) {
            d->as.uniform_decl.tex_flags &= ~TEX_FLAG_CLAMP;
        } else if (str_eq(attr, "mipmap")) {
            if (d->type == MTK_TYPE_TEXTURE2D_F32) {
                parser_error_at(p, &attr_tok, "'@mipmap' niteligi texture2d_f32 tipinde desteklenmez");
            } else {
                d->as.uniform_decl.tex_flags |= TEX_FLAG_MIPMAP;
            }
        } else {
            parser_error_at(p, &attr_tok, "bilinmeyen ornekleme niteligi '@%s'", attr);
        }
    }

    if (!parser_consume(p, TOK_SEMI, "uniform bildiriminden sonra ';' bekleniyordu")) {
        sync_decl(p);
    }
    return d;
}

static AstDecl* parse_buffer_decl(Parser* p) {
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "'buffer' sonrasinda isim bekleniyordu")) {
        sync_decl(p);
        return NULL;
    }
    AstDecl* d = new_decl(p, AST_DECL_BUFFER, name_tok.line, name_tok.col);
    copy_name(d->name, name_tok.start, name_tok.len);

    if (!parser_consume(p, TOK_COLON, "buffer isminden sonra ':' bekleniyordu")) {
        sync_decl(p);
        return d;
    }
    Token t_tok = p->cur;
    MtkType t = parse_type_name(p, &d->type_line, &d->type_col);
    if (t == MTK_TYPE_F32)       t = MTK_TYPE_BUF_F32;
    else if (t == MTK_TYPE_U32)  t = MTK_TYPE_BUF_U32;
    else if (t == MTK_TYPE_VEC4) t = MTK_TYPE_BUF_VEC4;
    else if (!mtk_type_is_buffer(t) && t != MTK_TYPE_VOID) {
        parser_error_at(p, &t_tok, "buffer tipi 'buffer<f32>', 'buffer<u32>' veya 'buffer<vec4>' olmali");
    }
    d->type = t;

    if (!parser_consume(p, TOK_SEMI, "'buffer' bildiriminden sonra ';' bekleniyordu")) {
        sync_decl(p);
    }
    return d;
}

static AstDecl* parse_in_decl(Parser* p) {
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "'in' sonrasinda isim bekleniyordu")) {
        sync_decl(p);
        return NULL;
    }
    AstDecl* d = new_decl(p, AST_DECL_IN, name_tok.line, name_tok.col);
    copy_name(d->name, name_tok.start, name_tok.len);

    if (!parser_consume(p, TOK_COLON, "varying girdi isminden sonra ':' bekleniyordu")) {
        sync_decl(p);
        return d;
    }
    d->type = parse_type_name(p, &d->type_line, &d->type_col);
    if (!parser_consume(p, TOK_SEMI, "'in' bildiriminden sonra ';' bekleniyordu")) {
        sync_decl(p);
    }
    return d;
}

static AstDecl* parse_const_decl(Parser* p) {
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "'const' sonrasinda isim bekleniyordu")) {
        sync_decl(p);
        return NULL;
    }
    AstDecl* d = new_decl(p, AST_DECL_CONST, name_tok.line, name_tok.col);
    copy_name(d->name, name_tok.start, name_tok.len);
    d->type = MTK_TYPE_VOID;

    if (parser_match(p, TOK_COLON)) {
        d->type = parse_type_name(p, &d->type_line, &d->type_col);
    }
    if (!parser_consume(p, TOK_ASSIGN, "'const' bildiriminde '=' bekleniyordu")) {
        sync_decl(p);
        return d;
    }
    d->as.const_decl.value = parse_expr(p);
    if (!parser_consume(p, TOK_SEMI, "'const' bildiriminden sonra ';' bekleniyordu")) {
        sync_decl(p);
    }
    return d;
}

static AstDecl* parse_fn_decl(Parser* p) {
    Token name_tok = p->cur;
    if (!parser_consume(p, TOK_IDENT, "'fn' sonrasinda fonksiyon ismi bekleniyordu")) {
        sync_decl(p);
        return NULL;
    }
    AstDecl* d = new_decl(p, AST_DECL_FN, name_tok.line, name_tok.col);
    copy_name(d->name, name_tok.start, name_tok.len);
    d->type = MTK_TYPE_VOID;
    d->type_line = name_tok.line;
    d->type_col  = name_tok.col;

    if (!parser_consume(p, TOK_LPAREN, "fonksiyon isminden sonra '(' bekleniyordu")) {
        sync_decl(p);
        return d;
    }

    if (!parser_check(p, TOK_RPAREN)) {
        do {
            Token p_tok = p->cur;
            if (!parser_consume(p, TOK_IDENT, "parametre ismi bekleniyordu")) {
                break;
            }
            if (!parser_consume(p, TOK_COLON, "parametre isminden sonra ':' bekleniyordu")) {
                break;
            }
            u32 t_line = 0, t_col = 0;
            MtkType pt = parse_type_name(p, &t_line, &t_col);
            if (d->as.fn_decl.param_count < 8) {
                AstParam* prm = &d->as.fn_decl.params[d->as.fn_decl.param_count++];
                copy_name(prm->name, p_tok.start, p_tok.len);
                prm->type = pt;
                prm->line = p_tok.line;
                prm->col  = p_tok.col;
            } else {
                parser_error_at(p, &p_tok, "fonksiyon en fazla 8 parametre alabilir");
            }
        } while (parser_match(p, TOK_COMMA));
    }

    if (!parser_consume(p, TOK_RPAREN, "parametre listesinden sonra ')' bekleniyordu")) {
        sync_decl(p);
        return d;
    }

    if (parser_match(p, TOK_ARROW)) {
        d->type = parse_type_name(p, &d->type_line, &d->type_col);
    }

    if (!parser_consume(p, TOK_LBRACE, "fonksiyon basligindan sonra '{' bekleniyordu")) {
        sync_decl(p);
        return d;
    }
    d->as.fn_decl.body = parse_block(p);
    return d;
}

b32 parser_parse(const char* src, u32 len, const char* filename,
                 Arena* arena, AstProgram* out_prog, MtkDiag* diag) {
    Parser p;
    __builtin_memset(&p, 0, sizeof(p));
    lexer_init(&p.lex, src, len);
    p.src      = src;
    p.src_len  = len;
    p.filename = filename ? filename : "<shader>";
    p.arena    = arena;
    p.diag     = diag;

    parser_advance(&p);

    AstDecl* temp[128];
    u32 count = 0;

    while (!parser_check(&p, TOK_EOF)) {
        if (p.diag && p.diag->count >= MTK_MAX_ERRORS) break;
        AstDecl* d = NULL;
        if (parser_match(&p, TOK_UNIFORM)) {
            d = parse_uniform_decl(&p);
        } else if (p.cur.kind == TOK_IDENT && p.cur.len == 6 &&
                   p.cur.start[0] == 'b' && p.cur.start[1] == 'u' && p.cur.start[2] == 'f' &&
                   p.cur.start[3] == 'f' && p.cur.start[4] == 'e' && p.cur.start[5] == 'r') {
            parser_advance(&p);
            d = parse_buffer_decl(&p);
        } else if (parser_match(&p, TOK_IN)) {
            d = parse_in_decl(&p);
        } else if (parser_match(&p, TOK_CONST)) {
            d = parse_const_decl(&p);
        } else if (parser_match(&p, TOK_FN)) {
            d = parse_fn_decl(&p);
        } else {
            parser_error_at(&p, &p.cur, "ust seviyede 'uniform', 'in', 'const' veya 'fn' bekleniyordu");
            parser_advance(&p);
            sync_decl(&p);
        }
        if (d && count < 128) {
            temp[count++] = d;
        }
        if (p.panic_mode) {
            sync_decl(&p);
        }
    }

    out_prog->decl_count = count;
    if (count > 0) {
        out_prog->decls = ARENA_PUSH_ARRAY(arena, AstDecl*, count);
        for (u32 i = 0; i < count; i++) out_prog->decls[i] = temp[i];
    } else {
        out_prog->decls = NULL;
    }

    return (!diag || diag->count == 0);
}
