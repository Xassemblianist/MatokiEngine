/* matoki/lexer.h — Matoki sözcük çözümleyicisi (lexer).
 * Kaynak metni satır/sütun konumlu Token akışına dönüştürür.
 * Satır ve blok yorumlarını atlar; tamsayı, hex, ondalık/bilimsel kayan nokta
 * literallerini ve anahtar kelimeleri ayrıştırır; geçersiz girdide TOK_ERROR üretir.
 */
#ifndef MATOKI_LEXER_H
#define MATOKI_LEXER_H

#include "core/types.h"

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    /* Kimlik ve literaller */
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,

    /* Anahtar kelimeler */
    TOK_FN,
    TOK_LET,
    TOK_VAR,
    TOK_IF,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_RETURN,
    TOK_DISCARD,
    TOK_UNIFORM,
    TOK_CONST,
    TOK_TRUE,
    TOK_FALSE,

    /* Noktalama ve ayraçlar */
    TOK_LPAREN,        /* ( */
    TOK_RPAREN,        /* ) */
    TOK_LBRACE,        /* { */
    TOK_RBRACE,        /* } */
    TOK_LBRACKET,      /* [ */
    TOK_RBRACKET,      /* ] */
    TOK_COMMA,         /* , */
    TOK_COLON,         /* : */
    TOK_SEMI,          /* ; */
    TOK_DOT,           /* . */
    TOK_DOTDOT,        /* .. */
    TOK_ARROW,         /* -> */
    TOK_AT,            /* @ */

    /* Operatörler */
    TOK_PLUS,          /* + */
    TOK_MINUS,         /* - */
    TOK_STAR,          /* * */
    TOK_SLASH,         /* / */
    TOK_PERCENT,       /* % */
    TOK_ASSIGN,        /* = */
    TOK_PLUS_ASSIGN,   /* += */
    TOK_MINUS_ASSIGN,  /* -= */
    TOK_STAR_ASSIGN,   /* *= */
    TOK_SLASH_ASSIGN,  /* /= */

    TOK_EQ,            /* == */
    TOK_NE,            /* != */
    TOK_LT,            /* < */
    TOK_LE,            /* <= */
    TOK_GT,            /* > */
    TOK_GE,            /* >= */

    TOK_AND_AND,       /* && */
    TOK_OR_OR,         /* || */
    TOK_BANG,          /* ! */

    TOK_AMP,           /* & */
    TOK_PIPE,          /* | */
    TOK_CARET,         /* ^ */
    TOK_TILDE,         /* ~ */
    TOK_SHL,           /* << */
    TOK_SHR,           /* >> */

    TOK_COUNT
} TokenKind;

typedef struct {
    TokenKind   kind;
    const char* start;
    u32         len;
    u32         line;
    u32         col;
    i32         i_val;
    f32         f_val;
    const char* err_msg; /* TOK_ERROR için kısa açıklama */
} Token;

typedef struct {
    const char* src;
    u32         len;
    u32         pos;
    u32         line;
    u32         col;
} Lexer;

void        lexer_init(Lexer* l, const char* src, u32 len);
Token       lexer_next(Lexer* l);
Token       lexer_peek(const Lexer* l);
const char* token_kind_name(TokenKind k);

#endif
