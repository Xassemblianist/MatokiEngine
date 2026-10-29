/* matoki/lexer.c — Matoki sözcük çözümleyicisi (lexer) implementasyonu.
 * Kaynak metin üzerinde tek geçişte (veya kopya üzerinden peek ile) token üretir.
 * Sayı literalleri (onluk, 0x hex, .5, 1e-3) libc kullanmadan ayrıştırılır.
 */
#include "matoki/lexer.h"

static b32 is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static b32 is_digit(char c) {
    return c >= '0' && c <= '9';
}

static b32 is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static u32 hex_val(char c) {
    if (c >= '0' && c <= '9') return (u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
    return (u32)(c - 'A' + 10);
}

static char lex_cur(const Lexer* l) {
    return (l->pos < l->len) ? l->src[l->pos] : '\0';
}

static char lex_peek_at(const Lexer* l, u32 offset) {
    return (l->pos + offset < l->len) ? l->src[l->pos + offset] : '\0';
}

static char lex_advance(Lexer* l) {
    if (l->pos >= l->len) return '\0';
    char c = l->src[l->pos++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static b32 str_eq_len(const char* a, u32 a_len, const char* b) {
    u32 i = 0;
    while (i < a_len && b[i] != '\0') {
        if (a[i] != b[i]) return FALSE;
        i++;
    }
    return i == a_len && b[i] == '\0';
}

static TokenKind check_keyword(const char* s, u32 len) {
    if (str_eq_len(s, len, "fn"))      return TOK_FN;
    if (str_eq_len(s, len, "let"))     return TOK_LET;
    if (str_eq_len(s, len, "var"))     return TOK_VAR;
    if (str_eq_len(s, len, "if"))      return TOK_IF;
    if (str_eq_len(s, len, "else"))    return TOK_ELSE;
    if (str_eq_len(s, len, "for"))     return TOK_FOR;
    if (str_eq_len(s, len, "in"))      return TOK_IN;
    if (str_eq_len(s, len, "return"))  return TOK_RETURN;
    if (str_eq_len(s, len, "discard")) return TOK_DISCARD;
    if (str_eq_len(s, len, "uniform")) return TOK_UNIFORM;
    if (str_eq_len(s, len, "const"))   return TOK_CONST;
    if (str_eq_len(s, len, "true"))    return TOK_TRUE;
    if (str_eq_len(s, len, "false"))   return TOK_FALSE;
    return TOK_IDENT;
}

void lexer_init(Lexer* l, const char* src, u32 len) {
    l->src  = src;
    l->len  = len;
    l->pos  = 0;
    l->line = 1;
    l->col  = 1;
}

static Token make_token(TokenKind kind, const char* start, u32 len, u32 line, u32 col) {
    Token t;
    t.kind    = kind;
    t.start   = start;
    t.len     = len;
    t.line    = line;
    t.col     = col;
    t.i_val   = 0;
    t.f_val   = 0.0f;
    t.err_msg = NULL;
    return t;
}

static Token make_error(const char* start, u32 len, u32 line, u32 col, const char* msg) {
    Token t = make_token(TOK_ERROR, start, len, line, col);
    t.err_msg = msg;
    return t;
}

static Token lex_number(Lexer* l, u32 start_pos, u32 line, u32 col, b32 started_with_dot) {
    const char* start = l->src + start_pos;

    /* 0x / 0X onaltılık tamsayı */
    if (!started_with_dot && start[0] == '0' && (lex_cur(l) == 'x' || lex_cur(l) == 'X')) {
        lex_advance(l);
        if (!is_hex(lex_cur(l))) {
            while (is_alpha(lex_cur(l)) || is_digit(lex_cur(l))) lex_advance(l);
            return make_error(start, l->pos - start_pos, line, col, "gecersiz onaltilik sayi literali");
        }
        u64 val = 0;
        while (is_hex(lex_cur(l))) {
            val = (val << 4) | (u64)hex_val(lex_advance(l));
        }
        if (is_alpha(lex_cur(l))) {
            while (is_alpha(lex_cur(l)) || is_digit(lex_cur(l))) lex_advance(l);
            return make_error(start, l->pos - start_pos, line, col, "gecersiz onaltilik sayi literali");
        }
        Token t = make_token(TOK_INT_LIT, start, l->pos - start_pos, line, col);
        t.i_val = (i32)(u32)val;
        return t;
    }

    b32 is_float = started_with_dot;
    u64 int_part = 0;
    if (!started_with_dot) {
        int_part = (u64)(start[0] - '0');
        while (is_digit(lex_cur(l))) {
            int_part = int_part * 10u + (u64)(lex_advance(l) - '0');
        }
    }

    f64 frac_part = 0.0;
    f64 frac_div  = 1.0;

    if (!started_with_dot && lex_cur(l) == '.' && lex_peek_at(l, 1) != '.') {
        is_float = TRUE;
        lex_advance(l); /* '.' */
        while (is_digit(lex_cur(l))) {
            frac_part = frac_part * 10.0 + (f64)(lex_advance(l) - '0');
            frac_div *= 10.0;
        }
    } else if (started_with_dot) {
        while (is_digit(lex_cur(l))) {
            frac_part = frac_part * 10.0 + (f64)(lex_advance(l) - '0');
            frac_div *= 10.0;
        }
    }

    /* Bilimsel gösterim: e / E */
    i32 exp_val = 0;
    b32 exp_neg = FALSE;
    if (lex_cur(l) == 'e' || lex_cur(l) == 'E') {
        is_float = TRUE;
        lex_advance(l);
        if (lex_cur(l) == '+' || lex_cur(l) == '-') {
            exp_neg = (lex_advance(l) == '-');
        }
        if (!is_digit(lex_cur(l))) {
            while (is_alpha(lex_cur(l)) || is_digit(lex_cur(l))) lex_advance(l);
            return make_error(start, l->pos - start_pos, line, col, "gecersiz ustel sayi literali");
        }
        while (is_digit(lex_cur(l))) {
            exp_val = exp_val * 10 + (i32)(lex_advance(l) - '0');
        }
    }

    /* Tam sayı literallerinde isteğe bağlı 'u'/'U' soneki (ör. 0u, 255u) */
    if (!is_float && (lex_cur(l) == 'u' || lex_cur(l) == 'U') &&
        !is_alpha(lex_peek_at(l, 1)) && !is_digit(lex_peek_at(l, 1))) {
        lex_advance(l);
    }

    /* Sayının hemen ardından harf veya ikinci bir nokta gelirse hata */
    if (is_alpha(lex_cur(l)) || (is_float && lex_cur(l) == '.' && lex_peek_at(l, 1) != '.')) {
        while (is_alpha(lex_cur(l)) || is_digit(lex_cur(l)) || lex_cur(l) == '.') {
            if (lex_cur(l) == '.' && lex_peek_at(l, 1) == '.') break;
            lex_advance(l);
        }
        return make_error(start, l->pos - start_pos, line, col, "gecersiz sayi literali");
    }

    Token t = make_token(is_float ? TOK_FLOAT_LIT : TOK_INT_LIT,
                         start, l->pos - start_pos, line, col);
    if (is_float) {
        f64 v = (f64)int_part + (frac_part / frac_div);
        if (exp_val > 0) {
            f64 p10 = 1.0;
            for (i32 i = 0; i < exp_val && i < 308; i++) p10 *= 10.0;
            v = exp_neg ? (v / p10) : (v * p10);
        }
        t.f_val = (f32)v;
    } else {
        t.i_val = (i32)int_part;
        t.f_val = (f32)int_part;
    }
    return t;
}

Token lexer_next(Lexer* l) {
    for (;;) {
        /* Boşlukları atla */
        while (l->pos < l->len) {
            char c = lex_cur(l);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                lex_advance(l);
            } else {
                break;
            }
        }

        if (l->pos >= l->len) {
            return make_token(TOK_EOF, l->src + l->pos, 0, l->line, l->col);
        }

        /* Tek satır yorum: // ... */
        if (lex_cur(l) == '/' && lex_peek_at(l, 1) == '/') {
            while (l->pos < l->len && lex_cur(l) != '\n') {
                lex_advance(l);
            }
            continue;
        }

        /* Çok satır blok yorumu */
        if (lex_cur(l) == '/' && lex_peek_at(l, 1) == '*') {
            u32 start_pos = l->pos;
            u32 line = l->line;
            u32 col  = l->col;
            lex_advance(l);
            lex_advance(l);
            b32 closed = FALSE;
            while (l->pos < l->len) {
                if (lex_cur(l) == '*' && lex_peek_at(l, 1) == '/') {
                    lex_advance(l);
                    lex_advance(l);
                    closed = TRUE;
                    break;
                }
                lex_advance(l);
            }
            if (!closed) {
                return make_error(l->src + start_pos, l->pos - start_pos,
                                  line, col, "kapatilmamis blok yorumu");
            }
            continue;
        }

        break;
    }

    u32 start_pos = l->pos;
    u32 line      = l->line;
    u32 col       = l->col;
    const char* start = l->src + start_pos;
    char c = lex_advance(l);

    if (is_alpha(c)) {
        while (is_alpha(lex_cur(l)) || is_digit(lex_cur(l))) {
            lex_advance(l);
        }
        u32 len = l->pos - start_pos;
        TokenKind k = check_keyword(start, len);
        Token t = make_token(k, start, len, line, col);
        if (k == TOK_TRUE)  t.i_val = 1;
        if (k == TOK_FALSE) t.i_val = 0;
        return t;
    }

    if (is_digit(c)) {
        return lex_number(l, start_pos, line, col, FALSE);
    }

    if (c == '.' && is_digit(lex_cur(l))) {
        return lex_number(l, start_pos, line, col, TRUE);
    }

    switch (c) {
    case '(': return make_token(TOK_LPAREN,   start, 1, line, col);
    case ')': return make_token(TOK_RPAREN,   start, 1, line, col);
    case '{': return make_token(TOK_LBRACE,   start, 1, line, col);
    case '}': return make_token(TOK_RBRACE,   start, 1, line, col);
    case '[': return make_token(TOK_LBRACKET, start, 1, line, col);
    case ']': return make_token(TOK_RBRACKET, start, 1, line, col);
    case ',': return make_token(TOK_COMMA,    start, 1, line, col);
    case ':': return make_token(TOK_COLON,    start, 1, line, col);
    case ';': return make_token(TOK_SEMI,     start, 1, line, col);
    case '@': return make_token(TOK_AT,       start, 1, line, col);
    case '~': return make_token(TOK_TILDE,    start, 1, line, col);
    case '^': return make_token(TOK_CARET,    start, 1, line, col);
    case '%': return make_token(TOK_PERCENT,  start, 1, line, col);

    case '.':
        if (lex_cur(l) == '.') {
            lex_advance(l);
            return make_token(TOK_DOTDOT, start, 2, line, col);
        }
        return make_token(TOK_DOT, start, 1, line, col);

    case '+':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_PLUS_ASSIGN, start, 2, line, col);
        }
        return make_token(TOK_PLUS, start, 1, line, col);

    case '-':
        if (lex_cur(l) == '>') {
            lex_advance(l);
            return make_token(TOK_ARROW, start, 2, line, col);
        }
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_MINUS_ASSIGN, start, 2, line, col);
        }
        return make_token(TOK_MINUS, start, 1, line, col);

    case '*':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_STAR_ASSIGN, start, 2, line, col);
        }
        return make_token(TOK_STAR, start, 1, line, col);

    case '/':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_SLASH_ASSIGN, start, 2, line, col);
        }
        return make_token(TOK_SLASH, start, 1, line, col);

    case '=':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_EQ, start, 2, line, col);
        }
        return make_token(TOK_ASSIGN, start, 1, line, col);

    case '!':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_NE, start, 2, line, col);
        }
        return make_token(TOK_BANG, start, 1, line, col);

    case '<':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_LE, start, 2, line, col);
        }
        if (lex_cur(l) == '<') {
            lex_advance(l);
            return make_token(TOK_SHL, start, 2, line, col);
        }
        return make_token(TOK_LT, start, 1, line, col);

    case '>':
        if (lex_cur(l) == '=') {
            lex_advance(l);
            return make_token(TOK_GE, start, 2, line, col);
        }
        if (lex_cur(l) == '>') {
            lex_advance(l);
            return make_token(TOK_SHR, start, 2, line, col);
        }
        return make_token(TOK_GT, start, 1, line, col);

    case '&':
        if (lex_cur(l) == '&') {
            lex_advance(l);
            return make_token(TOK_AND_AND, start, 2, line, col);
        }
        return make_token(TOK_AMP, start, 1, line, col);

    case '|':
        if (lex_cur(l) == '|') {
            lex_advance(l);
            return make_token(TOK_OR_OR, start, 2, line, col);
        }
        return make_token(TOK_PIPE, start, 1, line, col);

    default:
        return make_error(start, 1, line, col, "beklenmeyen karakter");
    }
}

Token lexer_peek(const Lexer* l) {
    Lexer copy = *l;
    return lexer_next(&copy);
}

const char* token_kind_name(TokenKind k) {
    switch (k) {
    case TOK_EOF:          return "EOF";
    case TOK_ERROR:        return "ERROR";
    case TOK_IDENT:        return "IDENT";
    case TOK_INT_LIT:      return "INT_LIT";
    case TOK_FLOAT_LIT:    return "FLOAT_LIT";
    case TOK_FN:           return "fn";
    case TOK_LET:          return "let";
    case TOK_VAR:          return "var";
    case TOK_IF:           return "if";
    case TOK_ELSE:         return "else";
    case TOK_FOR:          return "for";
    case TOK_IN:           return "in";
    case TOK_RETURN:       return "return";
    case TOK_DISCARD:      return "discard";
    case TOK_UNIFORM:      return "uniform";
    case TOK_CONST:        return "const";
    case TOK_TRUE:         return "true";
    case TOK_FALSE:        return "false";
    case TOK_LPAREN:       return "(";
    case TOK_RPAREN:       return ")";
    case TOK_LBRACE:       return "{";
    case TOK_RBRACE:       return "}";
    case TOK_LBRACKET:     return "[";
    case TOK_RBRACKET:     return "]";
    case TOK_COMMA:        return ",";
    case TOK_COLON:        return ":";
    case TOK_SEMI:         return ";";
    case TOK_DOT:          return ".";
    case TOK_DOTDOT:       return "..";
    case TOK_ARROW:        return "->";
    case TOK_AT:           return "@";
    case TOK_PLUS:         return "+";
    case TOK_MINUS:        return "-";
    case TOK_STAR:         return "*";
    case TOK_SLASH:        return "/";
    case TOK_PERCENT:      return "%";
    case TOK_ASSIGN:       return "=";
    case TOK_PLUS_ASSIGN:  return "+=";
    case TOK_MINUS_ASSIGN: return "-=";
    case TOK_STAR_ASSIGN:  return "*=";
    case TOK_SLASH_ASSIGN: return "/=";
    case TOK_EQ:           return "==";
    case TOK_NE:           return "!=";
    case TOK_LT:           return "<";
    case TOK_LE:           return "<=";
    case TOK_GT:           return ">";
    case TOK_GE:           return ">=";
    case TOK_AND_AND:      return "&&";
    case TOK_OR_OR:        return "||";
    case TOK_BANG:         return "!";
    case TOK_AMP:          return "&";
    case TOK_PIPE:         return "|";
    case TOK_CARET:        return "^";
    case TOK_TILDE:        return "~";
    case TOK_SHL:          return "<<";
    case TOK_SHR:          return ">>";
    default:               return "?";
    }
}
