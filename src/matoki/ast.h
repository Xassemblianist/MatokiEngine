/* matoki/ast.h — Matoki soyut sözdizimi ağacı (AST) düğüm tanımları.
 * Tüm düğümler Arena içinde ayrılır; ifade, deyim ve üst seviye bildirimleri temsil eder.
 * Sema aşaması ifade düğümlerinin tip (MtkType) ve tekdüzelik (is_varying) alanlarını doldurur.
 */
#ifndef MATOKI_AST_H
#define MATOKI_AST_H

#include "core/types.h"
#include "matoki/lexer.h"
#include "matoki/matoki.h"

typedef struct AstExpr AstExpr;
typedef struct AstStmt AstStmt;
typedef struct AstDecl AstDecl;

typedef enum {
    AST_EXPR_INT = 0,
    AST_EXPR_FLOAT,
    AST_EXPR_BOOL,
    AST_EXPR_IDENT,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_SWIZZLE,
    AST_EXPR_CALL,
} AstExprKind;

struct AstExpr {
    AstExprKind kind;
    u32         line;
    u32         col;
    MtkType     type;        /* sema doldurur */
    b32         is_varying;  /* sema doldurur */
    b32         is_int_lit;  /* bağlamdan i32/u32 alabilen tamsayı literali */

    union {
        i32 i_val;
        f32 f_val;
        b32 b_val;
        struct {
            char name[32];
        } ident;
        struct {
            TokenKind op;
            AstExpr*  operand;
        } unary;
        struct {
            TokenKind op;
            AstExpr*  left;
            AstExpr*  right;
        } binary;
        struct {
            AstExpr* base;
            char     mask[8];
            u32      len;
            u8       indices[4]; /* sema doldurur: 0..3 */
        } swizzle;
        struct {
            char     callee[32];
            AstExpr* args[8];
            u32      arg_count;
        } call;
    } as;
};

typedef enum {
    AST_STMT_BLOCK = 0,
    AST_STMT_VAR_DECL,
    AST_STMT_ASSIGN,
    AST_STMT_IF,
    AST_STMT_FOR,
    AST_STMT_RETURN,
    AST_STMT_DISCARD,
    AST_STMT_EXPR,
} AstStmtKind;

struct AstStmt {
    AstStmtKind kind;
    u32         line;
    u32         col;

    union {
        struct {
            AstStmt** stmts;
            u32       count;
        } block;
        struct {
            b32      is_mutable; /* TRUE = var, FALSE = let */
            char     name[32];
            MtkType  decl_type;  /* belirtilmemişse MTK_TYPE_VOID */
            u32      type_line;
            u32      type_col;
            AstExpr* init;
        } var_decl;
        struct {
            TokenKind op;        /* TOK_ASSIGN, TOK_PLUS_ASSIGN, vb. */
            AstExpr*  target;
            AstExpr*  value;
        } assign;
        struct {
            AstExpr* cond;
            AstStmt* then_branch;
            AstStmt* else_branch;
        } if_stmt;
        struct {
            char     var_name[32];
            AstExpr* start_expr;
            AstExpr* end_expr;
            AstStmt* body;
            i32      const_start; /* sema doldurur */
            i32      const_end;   /* sema doldurur */
        } for_stmt;
        struct {
            AstExpr* value;      /* NULL olabilir */
        } ret;
        struct {
            AstExpr* expr;
        } expr_stmt;
    } as;
};

typedef struct {
    char    name[32];
    MtkType type;
    u32     line;
    u32     col;
} AstParam;

typedef enum {
    AST_DECL_UNIFORM = 0,
    AST_DECL_IN,
    AST_DECL_CONST,
    AST_DECL_FN,
} AstDeclKind;

struct AstDecl {
    AstDeclKind kind;
    u32         line;
    u32         col;
    char        name[32];
    MtkType     type;
    u32         type_line;
    u32         type_col;

    union {
        struct {
            u32 tex_flags;      /* TEX_FLAG_* (@bilinear, @clamp) */
            u32 offset_or_slot; /* sema doldurur */
        } uniform_decl;
        struct {
            u32 varying_index;  /* sema doldurur */
        } in_decl;
        struct {
            AstExpr* value;
            f32      f_vals[4]; /* sema sabit değerlendirmesi */
            i32      i_vals[4];
        } const_decl;
        struct {
            AstParam params[8];
            u32      param_count;
            AstStmt* body;
        } fn_decl;
    } as;
};

typedef struct {
    AstDecl** decls;
    u32       decl_count;
} AstProgram;

#endif
