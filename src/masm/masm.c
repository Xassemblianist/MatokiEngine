/* masm/masm.c — WebAssembly metin biçimi (WAT alt kümesi) ayrıştırıcısı ve kodlayıcısı.
 * Düz (flat) ve katlanmış (folded) S-ifadelerini, $isim çözümlemesini, kontrol akışı
 * etiketlerini, bellek argümanlarını ve 128-bit SIMD komutlarını mbin ile ikiliye çevirir.
 * libc veya malloc kullanmaz; tüm ara yapılar ve çıktı Arena üzerinden ayrılır.
 */
#include "masm/masm.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"
#include "core/log.h"

#define MASM_MAX_TOKENS       16384
#define MASM_MAX_LOCALS       128
#define MASM_MAX_LABELS       64
#define MASM_MAX_DATA_BYTES   4096

/* ============================================================================
 * 1. Yardımcı Karakter, Dizge ve Sayı Fonksiyonları (libc yok)
 * ============================================================================ */

static void masm_memzero(void* dst, usize len) {
    u8* p = (u8*)dst;
    for (usize i = 0; i < len; i++) p[i] = 0;
}

static void masm_memcpy(void* dst, const void* src, usize len) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    for (usize i = 0; i < len; i++) d[i] = s[i];
}

static u32 masm_cstrlen(const char* s) {
    u32 n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static b32 masm_streq(const char* a, u32 alen, const char* b) {
    u32 blen = masm_cstrlen(b);
    if (alen != blen) return FALSE;
    for (u32 i = 0; i < alen; i++) {
        if (a[i] != b[i]) return FALSE;
    }
    return TRUE;
}

static b32 masm_streq_n(const char* a, u32 alen, const char* b, u32 blen) {
    if (alen != blen) return FALSE;
    for (u32 i = 0; i < alen; i++) {
        if (a[i] != b[i]) return FALSE;
    }
    return TRUE;
}

static b32 masm_starts_with(const char* s, u32 slen, const char* prefix) {
    u32 plen = masm_cstrlen(prefix);
    if (slen < plen) return FALSE;
    for (u32 i = 0; i < plen; i++) {
        if (s[i] != prefix[i]) return FALSE;
    }
    return TRUE;
}

static i32 masm_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static b32 masm_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static b32 masm_is_delim(char c) {
    return masm_is_space(c) || c == '(' || c == ')' || c == ';' || c == '"';
}

/* WAT tam sayı ayrıştırıcı (onluk ve 0x onaltılık, '_' ayırıcı destekli) */
static b32 masm_parse_u64_raw(const char* s, u32 len, b32* out_neg, u64* out_mag, b32* out_hex) {
    if (len == 0) return FALSE;
    u32 i = 0;
    b32 neg = FALSE;
    if (s[i] == '-') {
        neg = TRUE;
        i++;
    } else if (s[i] == '+') {
        i++;
    }
    if (i >= len) return FALSE;

    u32 base = 10;
    if (i + 2 <= len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= len) return FALSE;

    u64 val = 0;
    u32 digits = 0;
    b32 prev_underscore = TRUE;
    for (; i < len; i++) {
        char c = s[i];
        if (c == '_') {
            if (prev_underscore || i + 1 == len) return FALSE;
            prev_underscore = TRUE;
            continue;
        }
        prev_underscore = FALSE;
        i32 d = (base == 16) ? masm_hex_val(c) : ((c >= '0' && c <= '9') ? (c - '0') : -1);
        if (d < 0) return FALSE;
        if (val > (0xFFFFFFFFFFFFFFFFull - (u64)d) / base) return FALSE;
        val = val * base + (u64)d;
        digits++;
    }
    if (digits == 0) return FALSE;
    *out_neg = neg;
    *out_mag = val;
    if (out_hex) *out_hex = (base == 16);
    return TRUE;
}

static b32 masm_parse_u32(const char* s, u32 len, u32* out) {
    b32 neg = FALSE;
    u64 mag = 0;
    if (!masm_parse_u64_raw(s, len, &neg, &mag, NULL)) return FALSE;
    if (neg || mag > 0xFFFFFFFFull) return FALSE;
    *out = (u32)mag;
    return TRUE;
}

static b32 masm_parse_i32(const char* s, u32 len, i32* out) {
    b32 neg = FALSE;
    u64 mag = 0;
    if (!masm_parse_u64_raw(s, len, &neg, &mag, NULL)) return FALSE;
    if (neg) {
        if (mag > 0x80000000ull) return FALSE;
        *out = (i32)(0u - (u32)mag);
    } else {
        if (mag > 0xFFFFFFFFull) return FALSE;
        *out = (i32)(u32)mag;
    }
    return TRUE;
}

static b32 masm_parse_i64(const char* s, u32 len, i64* out) {
    b32 neg = FALSE;
    u64 mag = 0;
    if (!masm_parse_u64_raw(s, len, &neg, &mag, NULL)) return FALSE;
    if (neg) {
        if (mag > 0x8000000000000000ull) return FALSE;
        *out = (i64)(0ull - mag);
    } else {
        *out = (i64)mag;
    }
    return TRUE;
}

static f64 masm_pow10(i32 exp) {
    f64 res = 1.0;
    f64 base = 10.0;
    b32 neg = FALSE;
    if (exp < 0) {
        neg = TRUE;
        exp = -exp;
    }
    u32 e = (u32)exp;
    while (e > 0) {
        if (e & 1u) res *= base;
        base *= base;
        e >>= 1;
    }
    return neg ? (1.0 / res) : res;
}

static f64 masm_ldexp(f64 v, i32 exp) {
    f64 factor = 1.0;
    f64 base = 2.0;
    b32 neg = FALSE;
    if (exp < 0) {
        neg = TRUE;
        exp = -exp;
    }
    u32 e = (u32)exp;
    while (e > 0) {
        if (e & 1u) factor *= base;
        base *= base;
        e >>= 1;
    }
    return neg ? (v / factor) : (v * factor);
}

static b32 masm_parse_f64(const char* s, u32 len, f64* out) {
    if (len == 0) return FALSE;
    u32 i = 0;
    b32 neg = FALSE;
    if (s[i] == '-') {
        neg = TRUE;
        i++;
    } else if (s[i] == '+') {
        i++;
    }
    if (i >= len) return FALSE;

    const char* rem = s + i;
    u32 rem_len = len - i;

    if (masm_streq(rem, rem_len, "inf")) {
        union { u64 u; f64 f; } u;
        u.u = neg ? 0xFFF0000000000000ull : 0x7FF0000000000000ull;
        *out = u.f;
        return TRUE;
    }
    if (masm_streq(rem, rem_len, "nan")) {
        union { u64 u; f64 f; } u;
        u.u = neg ? 0xFFF8000000000000ull : 0x7FF8000000000000ull;
        *out = u.f;
        return TRUE;
    }
    if (masm_starts_with(rem, rem_len, "nan:0x")) {
        u64 payload = 0;
        b32 pneg = FALSE;
        if (!masm_parse_u64_raw(rem + 4, rem_len - 4, &pneg, &payload, NULL) || pneg || payload == 0) {
            return FALSE;
        }
        union { u64 u; f64 f; } u;
        u.u = (neg ? 0xFFF0000000000000ull : 0x7FF0000000000000ull) | (payload & 0x000FFFFFFFFFFFFFull);
        *out = u.f;
        return TRUE;
    }

    /* Onaltılık kayan nokta: 0x1.8p+2 */
    if (rem_len >= 2 && rem[0] == '0' && (rem[1] == 'x' || rem[1] == 'X')) {
        u32 p = 2;
        f64 int_part = 0.0;
        f64 frac_part = 0.0;
        f64 frac_scale = 1.0 / 16.0;
        u32 digits = 0;
        while (p < rem_len) {
            if (rem[p] == '_') { p++; continue; }
            i32 d = masm_hex_val(rem[p]);
            if (d < 0) break;
            int_part = int_part * 16.0 + (f64)d;
            digits++;
            p++;
        }
        if (p < rem_len && rem[p] == '.') {
            p++;
            while (p < rem_len) {
                if (rem[p] == '_') { p++; continue; }
                i32 d = masm_hex_val(rem[p]);
                if (d < 0) break;
                frac_part += (f64)d * frac_scale;
                frac_scale /= 16.0;
                digits++;
                p++;
            }
        }
        if (digits == 0) return FALSE;
        i32 exp2 = 0;
        if (p < rem_len && (rem[p] == 'p' || rem[p] == 'P')) {
            p++;
            b32 eneg = FALSE;
            if (p < rem_len && rem[p] == '-') { eneg = TRUE; p++; }
            else if (p < rem_len && rem[p] == '+') { p++; }
            if (p >= rem_len) return FALSE;
            u32 edigits = 0;
            while (p < rem_len) {
                char c = rem[p];
                if (c < '0' || c > '9') return FALSE;
                exp2 = exp2 * 10 + (c - '0');
                edigits++;
                p++;
            }
            if (edigits == 0) return FALSE;
            if (eneg) exp2 = -exp2;
        } else if (p != rem_len) {
            return FALSE;
        }
        f64 val = masm_ldexp(int_part + frac_part, exp2);
        if (neg) val = -val;
        *out = val;
        return TRUE;
    }

    /* Onluk kayan nokta */
    u32 p = 0;
    u64 mant = 0;
    i32 frac_exp = 0;
    u32 digits = 0;
    while (p < rem_len) {
        char c = rem[p];
        if (c == '_') { p++; continue; }
        if (c < '0' || c > '9') break;
        if (mant < 1000000000000000000ull) {
            mant = mant * 10ull + (u64)(c - '0');
        } else {
            frac_exp++;
        }
        digits++;
        p++;
    }
    if (p < rem_len && rem[p] == '.') {
        p++;
        while (p < rem_len) {
            char c = rem[p];
            if (c == '_') { p++; continue; }
            if (c < '0' || c > '9') break;
            if (mant < 1000000000000000000ull) {
                mant = mant * 10ull + (u64)(c - '0');
                frac_exp--;
            }
            digits++;
            p++;
        }
    }
    if (digits == 0) return FALSE;

    i32 exp10 = 0;
    if (p < rem_len && (rem[p] == 'e' || rem[p] == 'E')) {
        p++;
        b32 eneg = FALSE;
        if (p < rem_len && rem[p] == '-') { eneg = TRUE; p++; }
        else if (p < rem_len && rem[p] == '+') { p++; }
        if (p >= rem_len) return FALSE;
        u32 edigits = 0;
        while (p < rem_len) {
            char c = rem[p];
            if (c == '_') { p++; continue; }
            if (c < '0' || c > '9') return FALSE;
            exp10 = exp10 * 10 + (c - '0');
            edigits++;
            p++;
        }
        if (edigits == 0) return FALSE;
        if (eneg) exp10 = -exp10;
    }
    if (p != rem_len) return FALSE;

    f64 val = (f64)mant * masm_pow10(frac_exp + exp10);
    if (neg) val = -val;
    *out = val;
    return TRUE;
}

static b32 masm_parse_f32(const char* s, u32 len, f32* out) {
    if (len == 0) return FALSE;
    u32 i = 0;
    b32 neg = FALSE;
    if (s[i] == '-') { neg = TRUE; i++; }
    else if (s[i] == '+') { i++; }
    const char* rem = s + i;
    u32 rem_len = len - i;
    if (masm_starts_with(rem, rem_len, "nan:0x")) {
        u64 payload = 0;
        b32 pneg = FALSE;
        if (!masm_parse_u64_raw(rem + 4, rem_len - 4, &pneg, &payload, NULL) || pneg || payload == 0 || payload > 0x7FFFFFull) {
            return FALSE;
        }
        union { u32 u; f32 f; } u;
        u.u = (neg ? 0xFF800000u : 0x7F800000u) | (u32)(payload & 0x7FFFFFull);
        *out = u.f;
        return TRUE;
    }
    f64 d = 0.0;
    if (!masm_parse_f64(s, len, &d)) return FALSE;
    *out = (f32)d;
    return TRUE;
}

/* Çift tırnaklı dizge sabitini bayt dizisine çözer (\n, \t, \r, \\, \", \', \xx) */
static b32 masm_unescape_string(const char* str_tok, u32 tok_len, u8* out_buf, u32 cap, u32* out_len) {
    if (tok_len < 2 || str_tok[0] != '"' || str_tok[tok_len - 1] != '"') return FALSE;
    u32 n = 0;
    u32 i = 1;
    u32 end = tok_len - 1;
    while (i < end) {
        char c = str_tok[i++];
        if (c != '\\') {
            if (n >= cap) return FALSE;
            out_buf[n++] = (u8)c;
            continue;
        }
        if (i >= end) return FALSE;
        char esc = str_tok[i++];
        u8 val = 0;
        if (esc == 'n') val = '\n';
        else if (esc == 't') val = '\t';
        else if (esc == 'r') val = '\r';
        else if (esc == '\\') val = '\\';
        else if (esc == '"') val = '"';
        else if (esc == '\'') val = '\'';
        else {
            i32 h1 = masm_hex_val(esc);
            if (h1 < 0 || i >= end) return FALSE;
            i32 h2 = masm_hex_val(str_tok[i++]);
            if (h2 < 0) return FALSE;
            val = (u8)((h1 << 4) | h2);
        }
        if (n >= cap) return FALSE;
        out_buf[n++] = val;
    }
    *out_len = n;
    return TRUE;
}

/* ============================================================================
 * 2. Sözcük Çözümleyici (Tokenizer)
 * ============================================================================ */

typedef enum {
    TOK_EOF = 0,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_STRING,
    TOK_ATOM
} MasmTokKind;

typedef struct {
    u8          kind;
    const char* ptr;
    u32         len;
    u32         line;
    u32         col;
} MasmToken;

typedef struct {
    const char* src;
    u32         src_len;
    MasmToken*  toks;
    u32         tok_count;
    u32         pos;

    b32         has_err;
    u32         err_line;
    u32         err_col;
    char        err_msg[256];
} MasmLexer;

static void masm_error_at(MasmLexer* lx, u32 line, u32 col, const char* fmt, ...) {
    if (lx->has_err) return;
    lx->has_err = TRUE;
    lx->err_line = line;
    lx->err_col = col;
    u32 n = fmt_format(lx->err_msg, sizeof(lx->err_msg), "%u:%u: ", line, col);
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(lx->err_msg + n, (u32)sizeof(lx->err_msg) - n, fmt, ap);
    VA_END(ap);
}

static b32 masm_tokenize(MasmLexer* lx) {
    u32 i = 0;
    u32 line = 1;
    u32 col = 1;

    while (i < lx->src_len) {
        char c = lx->src[i];

        /* Boşlukları atla */
        if (masm_is_space(c)) {
            if (c == '\n') { line++; col = 1; }
            else { col++; }
            i++;
            continue;
        }

        /* Satır yorumu: ;; ... */
        if (c == ';' && i + 1 < lx->src_len && lx->src[i + 1] == ';') {
            i += 2;
            col += 2;
            while (i < lx->src_len && lx->src[i] != '\n') {
                i++;
                col++;
            }
            continue;
        }

        /* Blok yorumu: (; ... ;) (iç içe olabilir) */
        if (c == '(' && i + 1 < lx->src_len && lx->src[i + 1] == ';') {
            u32 start_line = line;
            u32 start_col = col;
            u32 depth = 1;
            i += 2;
            col += 2;
            while (i < lx->src_len && depth > 0) {
                if (lx->src[i] == '(' && i + 1 < lx->src_len && lx->src[i + 1] == ';') {
                    depth++;
                    i += 2;
                    col += 2;
                } else if (lx->src[i] == ';' && i + 1 < lx->src_len && lx->src[i + 1] == ')') {
                    depth--;
                    i += 2;
                    col += 2;
                } else {
                    if (lx->src[i] == '\n') { line++; col = 1; }
                    else { col++; }
                    i++;
                }
            }
            if (depth != 0) {
                masm_error_at(lx, start_line, start_col, "unterminated block comment");
                return FALSE;
            }
            continue;
        }

        if (lx->tok_count + 1 >= MASM_MAX_TOKENS) {
            masm_error_at(lx, line, col, "too many tokens in WAT source");
            return FALSE;
        }

        if (c == '(' || c == ')') {
            MasmToken* t = &lx->toks[lx->tok_count++];
            t->kind = (c == '(') ? TOK_LPAREN : TOK_RPAREN;
            t->ptr = lx->src + i;
            t->len = 1;
            t->line = line;
            t->col = col;
            i++;
            col++;
            continue;
        }

        if (c == '"') {
            u32 start = i;
            u32 start_line = line;
            u32 start_col = col;
            i++;
            col++;
            b32 closed = FALSE;
            while (i < lx->src_len) {
                char sc = lx->src[i];
                if (sc == '\n' || (u8)sc < 0x20u) {
                    masm_error_at(lx, line, col, "invalid character or newline in string literal");
                    return FALSE;
                }
                if (sc == '"') {
                    i++;
                    col++;
                    closed = TRUE;
                    break;
                }
                if (sc == '\\') {
                    i++;
                    col++;
                    if (i >= lx->src_len) break;
                    char esc = lx->src[i];
                    if (esc == 'n' || esc == 't' || esc == 'r' || esc == '\\' || esc == '"' || esc == '\'') {
                        i++;
                        col++;
                    } else if (masm_hex_val(esc) >= 0 && i + 1 < lx->src_len && masm_hex_val(lx->src[i + 1]) >= 0) {
                        i += 2;
                        col += 2;
                    } else {
                        masm_error_at(lx, line, col - 1, "invalid escape sequence in string literal");
                        return FALSE;
                    }
                } else {
                    i++;
                    col++;
                }
            }
            if (!closed) {
                masm_error_at(lx, start_line, start_col, "unterminated string literal");
                return FALSE;
            }
            MasmToken* t = &lx->toks[lx->tok_count++];
            t->kind = TOK_STRING;
            t->ptr = lx->src + start;
            t->len = i - start;
            t->line = start_line;
            t->col = start_col;
            continue;
        }

        if (c == ';') {
            masm_error_at(lx, line, col, "unexpected ';'");
            return FALSE;
        }

        /* Atom (anahtar sözcük, $isim, sayı, offset=...) */
        u32 start = i;
        u32 start_line = line;
        u32 start_col = col;
        while (i < lx->src_len && !masm_is_delim(lx->src[i])) {
            i++;
            col++;
        }
        MasmToken* t = &lx->toks[lx->tok_count++];
        t->kind = TOK_ATOM;
        t->ptr = lx->src + start;
        t->len = i - start;
        t->line = start_line;
        t->col = start_col;
    }

    MasmToken* eof = &lx->toks[lx->tok_count];
    eof->kind = TOK_EOF;
    eof->ptr = lx->src + lx->src_len;
    eof->len = 0;
    eof->line = line;
    eof->col = col;
    return TRUE;
}

/* ============================================================================
 * 3. Ayrıştırıcı Durumu ve Sembol Tabloları
 * ============================================================================ */

typedef struct {
    const char* ptr;
    u32         len;
} MasmSlice;

typedef struct {
    MasmSlice name;
    u8        params[MB_MAX_PARAMS];
    u32       param_count;
    u8        results[MB_MAX_RESULTS];
    u32       result_count;
    u32       line;
    u32       col;
} MasmTypeDecl;

typedef struct {
    b32       is_import;
    MasmSlice name;
    MasmSlice imp_mod;
    MasmSlice imp_field;
    b32       has_type_use;
    MasmToken type_use_tok;
    b32       has_explicit_sig;
    u8        params[MB_MAX_PARAMS];
    MasmSlice param_names[MB_MAX_PARAMS];
    u32       param_count;
    u8        results[MB_MAX_RESULTS];
    u32       result_count;
    u8        locals[MASM_MAX_LOCALS];
    MasmSlice local_names[MASM_MAX_LOCALS];
    u32       local_count;
    u32       type_idx;
    u32       body_start_tok;
    u32       body_end_tok;
    u32       line;
    u32       col;
} MasmFuncDecl;

typedef struct {
    b32       is_import;
    MasmSlice name;
    MasmSlice imp_mod;
    MasmSlice imp_field;
    u8        valtype;
    b32       mutable_;
    u8        init_type;
    i64       init_i64;
    f32       init_f32;
    f64       init_f64;
    u8        init_v128[16];
    u32       line;
    u32       col;
} MasmGlobalDecl;

typedef struct {
    MasmSlice name;
    u8        kind;
    MasmToken target_tok;
    b32       resolved;
    u32       index;
} MasmExportDecl;

typedef struct {
    i32       offset;
    MasmToken func_toks[MB_MAX_ELEM_FUNCS];
    u32       count;
} MasmElemDecl;

typedef struct {
    i32       offset;
    u8*       bytes;
    u32       len;
} MasmDataDecl;

typedef struct {
    u8  kind;
    u32 index;
} MasmImportEntry;

typedef struct {
    MasmLexer      lx;
    Arena*         arena;

    MasmTypeDecl   types[MB_MAX_TYPES];
    u32            type_count;

    MasmImportEntry import_order[MB_MAX_IMPORTS];
    u32             import_order_count;

    MasmFuncDecl   funcs[MB_MAX_FUNCS];
    u32            func_count;
    u32            imported_func_count;

    MasmGlobalDecl globals[MB_MAX_GLOBALS];
    u32            global_count;
    u32            imported_global_count;

    b32            has_table;
    b32            table_is_import;
    MasmSlice      table_name;
    MasmSlice      table_imp_mod;
    MasmSlice      table_imp_field;
    u32            table_min;
    u32            table_max;
    b32            table_has_max;

    b32            has_memory;
    b32            mem_is_import;
    MasmSlice      mem_name;
    MasmSlice      mem_imp_mod;
    MasmSlice      mem_imp_field;
    u32            mem_min;
    u32            mem_max;
    b32            mem_has_max;
    b32            mem_shared;

    MasmExportDecl exports[MB_MAX_EXPORTS];
    u32            export_count;

    MasmElemDecl   elems[MB_MAX_ELEMS];
    u32            elem_count;

    MasmDataDecl   datas[MB_MAX_DATAS];
    u32            data_count;

    b32            seen_non_import;
} MasmParser;

static const MasmToken* mp_peek(const MasmParser* p) {
    return &p->lx.toks[p->lx.pos];
}

static const MasmToken* mp_peek_n(const MasmParser* p, u32 offset) {
    u32 idx = p->lx.pos + offset;
    if (idx >= p->lx.tok_count) return &p->lx.toks[p->lx.tok_count];
    return &p->lx.toks[idx];
}

static const MasmToken* mp_next(MasmParser* p) {
    const MasmToken* t = &p->lx.toks[p->lx.pos];
    if (t->kind != TOK_EOF) p->lx.pos++;
    return t;
}

static b32 mp_is_atom(const MasmToken* t, const char* s) {
    return t->kind == TOK_ATOM && masm_streq(t->ptr, t->len, s);
}

static b32 mp_is_id(const MasmToken* t) {
    return t->kind == TOK_ATOM && t->len >= 2 && t->ptr[0] == '$';
}

static b32 mp_expect_lparen(MasmParser* p) {
    const MasmToken* t = mp_next(p);
    if (t->kind != TOK_LPAREN) {
        masm_error_at(&p->lx, t->line, t->col, "expected '('");
        return FALSE;
    }
    return TRUE;
}

static b32 mp_expect_rparen(MasmParser* p) {
    const MasmToken* t = mp_next(p);
    if (t->kind != TOK_RPAREN) {
        masm_error_at(&p->lx, t->line, t->col, "expected ')'");
        return FALSE;
    }
    return TRUE;
}

static b32 mp_parse_valtype(const MasmToken* t, u8* out_vt) {
    if (t->kind != TOK_ATOM) return FALSE;
    if (masm_streq(t->ptr, t->len, "i32"))  { *out_vt = MB_VAL_I32;  return TRUE; }
    if (masm_streq(t->ptr, t->len, "i64"))  { *out_vt = MB_VAL_I64;  return TRUE; }
    if (masm_streq(t->ptr, t->len, "f32"))  { *out_vt = MB_VAL_F32;  return TRUE; }
    if (masm_streq(t->ptr, t->len, "f64"))  { *out_vt = MB_VAL_F64;  return TRUE; }
    if (masm_streq(t->ptr, t->len, "v128")) { *out_vt = MB_VAL_V128; return TRUE; }
    return FALSE;
}

/* Açık '(' sonrasından eşleşen ')' sonrasına kadar olan token indeksini bulur */
static b32 mp_skip_sexpr(MasmParser* p) {
    u32 depth = 1;
    while (depth > 0) {
        const MasmToken* t = mp_next(p);
        if (t->kind == TOK_EOF) {
            masm_error_at(&p->lx, t->line, t->col, "unexpected EOF inside '(' expression");
            return FALSE;
        }
        if (t->kind == TOK_LPAREN) depth++;
        else if (t->kind == TOK_RPAREN) depth--;
    }
    return TRUE;
}

static MasmSlice mp_unescape_name_to_arena(MasmParser* p, const MasmToken* str_tok, b32* out_ok) {
    MasmSlice s = { NULL, 0 };
    u8 tmp[MB_MAX_NAME_LEN];
    u32 len = 0;
    if (!masm_unescape_string(str_tok->ptr, str_tok->len, tmp, sizeof(tmp) - 1, &len)) {
        masm_error_at(&p->lx, str_tok->line, str_tok->col, "invalid or too long string literal");
        *out_ok = FALSE;
        return s;
    }
    char* dst = ARENA_PUSH_ARRAY(p->arena, char, len + 1);
    masm_memcpy(dst, tmp, len);
    dst[len] = '\0';
    s.ptr = dst;
    s.len = len;
    *out_ok = TRUE;
    return s;
}

static b32 mp_add_inline_export(MasmParser* p, const MasmToken* str_tok, u8 kind, u32 index) {
    if (p->export_count >= MB_MAX_EXPORTS) {
        masm_error_at(&p->lx, str_tok->line, str_tok->col, "too many exports");
        return FALSE;
    }
    b32 ok = FALSE;
    MasmSlice name = mp_unescape_name_to_arena(p, str_tok, &ok);
    if (!ok) return FALSE;
    MasmExportDecl* ex = &p->exports[p->export_count++];
    ex->name = name;
    ex->kind = kind;
    ex->resolved = TRUE;
    ex->index = index;
    return TRUE;
}

static b32 mp_record_import(MasmParser* p, const MasmToken* err_tok, u8 kind, u32 index) {
    if (p->import_order_count >= MB_MAX_IMPORTS) {
        masm_error_at(&p->lx, err_tok->line, err_tok->col, "too many imports");
        return FALSE;
    }
    MasmImportEntry* e = &p->import_order[p->import_order_count++];
    e->kind = kind;
    e->index = index;
    return TRUE;
}

/* ============================================================================
 * 4. Modül Alanlarını (Section Bildirimlerini) Ayrıştırma
 * ============================================================================ */

/* (param ...) ve (result ...) listelerini okur */
static b32 mp_parse_func_sig(MasmParser* p,
                             u8* params, MasmSlice* param_names, u32* out_np,
                             u8* results, u32* out_nr,
                             b32 allow_param_names) {
    u32 np = 0;
    u32 nr = 0;
    b32 seen_result = FALSE;

    while (mp_peek(p)->kind == TOK_LPAREN) {
        const MasmToken* kw = mp_peek_n(p, 1);
        if (mp_is_atom(kw, "param")) {
            if (seen_result) {
                masm_error_at(&p->lx, kw->line, kw->col, "(param) cannot appear after (result)");
                return FALSE;
            }
            mp_next(p); /* '(' */
            mp_next(p); /* 'param' */
            if (allow_param_names && mp_is_id(mp_peek(p))) {
                const MasmToken* id_tok = mp_next(p);
                /* Aynı fonksiyonda mükerrer parametre adı kontrolü */
                for (u32 k = 0; k < np; k++) {
                    if (param_names && param_names[k].len > 0 &&
                        masm_streq_n(param_names[k].ptr, param_names[k].len, id_tok->ptr, id_tok->len)) {
                        masm_error_at(&p->lx, id_tok->line, id_tok->col, "duplicate parameter name");
                        return FALSE;
                    }
                }
                const MasmToken* vt_tok = mp_next(p);
                u8 vt = 0;
                if (!mp_parse_valtype(vt_tok, &vt)) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in named (param)");
                    return FALSE;
                }
                if (np >= MB_MAX_PARAMS) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "too many parameters");
                    return FALSE;
                }
                params[np] = vt;
                if (param_names) {
                    param_names[np].ptr = id_tok->ptr;
                    param_names[np].len = id_tok->len;
                }
                np++;
                if (!mp_expect_rparen(p)) return FALSE;
            } else {
                while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                    const MasmToken* vt_tok = mp_next(p);
                    u8 vt = 0;
                    if (!mp_parse_valtype(vt_tok, &vt)) {
                        masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in (param)");
                        return FALSE;
                    }
                    if (np >= MB_MAX_PARAMS) {
                        masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "too many parameters");
                        return FALSE;
                    }
                    params[np] = vt;
                    if (param_names) {
                        param_names[np].ptr = NULL;
                        param_names[np].len = 0;
                    }
                    np++;
                }
                if (!mp_expect_rparen(p)) return FALSE;
            }
        } else if (mp_is_atom(kw, "result")) {
            seen_result = TRUE;
            mp_next(p); /* '(' */
            mp_next(p); /* 'result' */
            while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                const MasmToken* vt_tok = mp_next(p);
                u8 vt = 0;
                if (!mp_parse_valtype(vt_tok, &vt)) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in (result)");
                    return FALSE;
                }
                if (nr >= MB_MAX_RESULTS) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "too many results");
                    return FALSE;
                }
                results[nr++] = vt;
            }
            if (!mp_expect_rparen(p)) return FALSE;
        } else {
            break;
        }
    }

    *out_np = np;
    *out_nr = nr;
    return TRUE;
}

/* (type $t (func (param ...) (result ...))) */
static b32 mp_parse_type_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->type_count >= MB_MAX_TYPES) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many types");
        return FALSE;
    }
    MasmTypeDecl* td = &p->types[p->type_count++];
    masm_memzero(td, sizeof(MasmTypeDecl));
    td->line = kw_tok->line;
    td->col = kw_tok->col;

    if (mp_is_id(mp_peek(p))) {
        const MasmToken* id = mp_next(p);
        for (u32 i = 0; i + 1 < p->type_count; i++) {
            if (p->types[i].name.len > 0 &&
                masm_streq_n(p->types[i].name.ptr, p->types[i].name.len, id->ptr, id->len)) {
                masm_error_at(&p->lx, id->line, id->col, "duplicate type identifier");
                return FALSE;
            }
        }
        td->name.ptr = id->ptr;
        td->name.len = id->len;
    }

    if (!mp_expect_lparen(p)) return FALSE;
    const MasmToken* fkw = mp_next(p);
    if (!mp_is_atom(fkw, "func")) {
        masm_error_at(&p->lx, fkw->line, fkw->col, "expected 'func' in type declaration");
        return FALSE;
    }
    if (!mp_parse_func_sig(p, td->params, NULL, &td->param_count, td->results, &td->result_count, TRUE)) {
        return FALSE;
    }
    if (!mp_expect_rparen(p)) return FALSE; /* end (func ...) */
    if (!mp_expect_rparen(p)) return FALSE; /* end (type ...) */
    return TRUE;
}

static b32 mp_parse_limits(MasmParser* p, u32* out_min, u32* out_max, b32* out_has_max, b32* out_shared) {
    const MasmToken* t_min = mp_next(p);
    if (t_min->kind != TOK_ATOM || !masm_parse_u32(t_min->ptr, t_min->len, out_min)) {
        masm_error_at(&p->lx, t_min->line, t_min->col, "expected minimum limit integer");
        return FALSE;
    }
    *out_max = 0;
    *out_has_max = FALSE;
    if (out_shared) *out_shared = FALSE;

    if (mp_peek(p)->kind == TOK_ATOM && !mp_is_atom(mp_peek(p), "funcref") && !mp_is_atom(mp_peek(p), "shared")) {
        const MasmToken* t_max = mp_next(p);
        if (!masm_parse_u32(t_max->ptr, t_max->len, out_max)) {
            masm_error_at(&p->lx, t_max->line, t_max->col, "expected maximum limit integer");
            return FALSE;
        }
        if (*out_max < *out_min) {
            masm_error_at(&p->lx, t_max->line, t_max->col, "size minimum must not be greater than maximum");
            return FALSE;
        }
        *out_has_max = TRUE;
    }

    if (out_shared && mp_is_atom(mp_peek(p), "shared")) {
        const MasmToken* sh = mp_next(p);
        if (!*out_has_max) {
            masm_error_at(&p->lx, sh->line, sh->col, "shared memory must have a maximum size");
            return FALSE;
        }
        *out_shared = TRUE;
    }
    return TRUE;
}

/* (import "mod" "name" (func|memory|table|global ...)) */
static b32 mp_parse_import_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->seen_non_import) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "imports must occur before all non-import definitions");
        return FALSE;
    }
    const MasmToken* mod_tok = mp_next(p);
    if (mod_tok->kind != TOK_STRING) {
        masm_error_at(&p->lx, mod_tok->line, mod_tok->col, "expected module name string in import");
        return FALSE;
    }
    const MasmToken* fld_tok = mp_next(p);
    if (fld_tok->kind != TOK_STRING) {
        masm_error_at(&p->lx, fld_tok->line, fld_tok->col, "expected field name string in import");
        return FALSE;
    }
    b32 ok1 = FALSE, ok2 = FALSE;
    MasmSlice mod_s = mp_unescape_name_to_arena(p, mod_tok, &ok1);
    MasmSlice fld_s = mp_unescape_name_to_arena(p, fld_tok, &ok2);
    if (!ok1 || !ok2) return FALSE;

    if (!mp_expect_lparen(p)) return FALSE;
    const MasmToken* desc_kw = mp_next(p);
    if (mp_is_atom(desc_kw, "func")) {
        if (p->func_count >= MB_MAX_FUNCS) {
            masm_error_at(&p->lx, desc_kw->line, desc_kw->col, "too many functions");
            return FALSE;
        }
        u32 fidx = p->func_count++;
        if (!mp_record_import(p, desc_kw, MB_EXT_FUNC, fidx)) return FALSE;
        MasmFuncDecl* fd = &p->funcs[fidx];
        masm_memzero(fd, sizeof(MasmFuncDecl));
        fd->is_import = TRUE;
        fd->imp_mod = mod_s;
        fd->imp_field = fld_s;
        fd->line = desc_kw->line;
        fd->col = desc_kw->col;
        p->imported_func_count++;

        if (mp_is_id(mp_peek(p))) {
            const MasmToken* id = mp_next(p);
            fd->name.ptr = id->ptr;
            fd->name.len = id->len;
        }
        if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "type")) {
            mp_next(p); /* '(' */
            mp_next(p); /* 'type' */
            fd->has_type_use = TRUE;
            fd->type_use_tok = *mp_next(p);
            if (!mp_expect_rparen(p)) return FALSE;
        }
        if (mp_peek(p)->kind == TOK_LPAREN) {
            fd->has_explicit_sig = TRUE;
            if (!mp_parse_func_sig(p, fd->params, fd->param_names, &fd->param_count,
                                   fd->results, &fd->result_count, TRUE)) {
                return FALSE;
            }
        }
        if (!mp_expect_rparen(p)) return FALSE;
    } else if (mp_is_atom(desc_kw, "memory")) {
        if (p->has_memory) {
            masm_error_at(&p->lx, desc_kw->line, desc_kw->col, "only one memory is allowed");
            return FALSE;
        }
        if (!mp_record_import(p, desc_kw, MB_EXT_MEMORY, 0)) return FALSE;
        p->has_memory = TRUE;
        p->mem_is_import = TRUE;
        p->mem_imp_mod = mod_s;
        p->mem_imp_field = fld_s;
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* id = mp_next(p);
            p->mem_name.ptr = id->ptr;
            p->mem_name.len = id->len;
        }
        if (!mp_parse_limits(p, &p->mem_min, &p->mem_max, &p->mem_has_max, &p->mem_shared)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
    } else if (mp_is_atom(desc_kw, "table")) {
        if (p->has_table) {
            masm_error_at(&p->lx, desc_kw->line, desc_kw->col, "only one table is allowed");
            return FALSE;
        }
        if (!mp_record_import(p, desc_kw, MB_EXT_TABLE, 0)) return FALSE;
        p->has_table = TRUE;
        p->table_is_import = TRUE;
        p->table_imp_mod = mod_s;
        p->table_imp_field = fld_s;
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* id = mp_next(p);
            p->table_name.ptr = id->ptr;
            p->table_name.len = id->len;
        }
        if (!mp_parse_limits(p, &p->table_min, &p->table_max, &p->table_has_max, NULL)) return FALSE;
        const MasmToken* rt = mp_next(p);
        if (!mp_is_atom(rt, "funcref") && !mp_is_atom(rt, "anyfunc")) {
            masm_error_at(&p->lx, rt->line, rt->col, "expected 'funcref' in table import");
            return FALSE;
        }
        if (!mp_expect_rparen(p)) return FALSE;
    } else if (mp_is_atom(desc_kw, "global")) {
        if (p->global_count >= MB_MAX_GLOBALS) {
            masm_error_at(&p->lx, desc_kw->line, desc_kw->col, "too many globals");
            return FALSE;
        }
        u32 gidx = p->global_count++;
        if (!mp_record_import(p, desc_kw, MB_EXT_GLOBAL, gidx)) return FALSE;
        MasmGlobalDecl* gd = &p->globals[gidx];
        masm_memzero(gd, sizeof(MasmGlobalDecl));
        gd->is_import = TRUE;
        gd->imp_mod = mod_s;
        gd->imp_field = fld_s;
        gd->line = desc_kw->line;
        gd->col = desc_kw->col;
        p->imported_global_count++;

        if (mp_is_id(mp_peek(p))) {
            const MasmToken* id = mp_next(p);
            gd->name.ptr = id->ptr;
            gd->name.len = id->len;
        }
        if (mp_peek(p)->kind == TOK_LPAREN) {
            mp_next(p); /* '(' */
            const MasmToken* mut_kw = mp_next(p);
            if (!mp_is_atom(mut_kw, "mut")) {
                masm_error_at(&p->lx, mut_kw->line, mut_kw->col, "expected 'mut' in global type");
                return FALSE;
            }
            const MasmToken* vt_tok = mp_next(p);
            if (!mp_parse_valtype(vt_tok, &gd->valtype)) {
                masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in (mut ...)");
                return FALSE;
            }
            gd->mutable_ = TRUE;
            if (!mp_expect_rparen(p)) return FALSE;
        } else {
            const MasmToken* vt_tok = mp_next(p);
            if (!mp_parse_valtype(vt_tok, &gd->valtype)) {
                masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in global import");
                return FALSE;
            }
            gd->mutable_ = FALSE;
        }
        if (!mp_expect_rparen(p)) return FALSE;
    } else {
        masm_error_at(&p->lx, desc_kw->line, desc_kw->col, "unsupported import descriptor");
        return FALSE;
    }

    if (!mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* Sabit başlatma ifadesi ayrıştırıcı (global, data, elem için) */
static b32 mp_parse_const_expr(MasmParser* p, u8 expected_type, i64* out_i64, f32* out_f32, f64* out_f64) {
    b32 paren = FALSE;
    if (mp_peek(p)->kind == TOK_LPAREN) {
        paren = TRUE;
        mp_next(p);
        if (mp_is_atom(mp_peek(p), "offset")) {
            mp_next(p);
            if (mp_peek(p)->kind == TOK_LPAREN) {
                mp_next(p);
                if (!mp_parse_const_expr(p, expected_type, out_i64, out_f32, out_f64)) return FALSE;
                if (!mp_expect_rparen(p)) return FALSE;
                if (!mp_expect_rparen(p)) return FALSE;
                return TRUE;
            }
        }
    }

    const MasmToken* op = mp_next(p);
    if (expected_type == MB_VAL_I32 && mp_is_atom(op, "i32.const")) {
        const MasmToken* val_tok = mp_next(p);
        i32 v = 0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_i32(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid i32.const value");
            return FALSE;
        }
        *out_i64 = (i64)v;
    } else if (expected_type == MB_VAL_I64 && mp_is_atom(op, "i64.const")) {
        const MasmToken* val_tok = mp_next(p);
        i64 v = 0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_i64(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid i64.const value");
            return FALSE;
        }
        *out_i64 = v;
    } else if (expected_type == MB_VAL_F32 && mp_is_atom(op, "f32.const")) {
        const MasmToken* val_tok = mp_next(p);
        f32 v = 0.0f;
        if (val_tok->kind != TOK_ATOM || !masm_parse_f32(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid f32.const value");
            return FALSE;
        }
        *out_f32 = v;
    } else if (expected_type == MB_VAL_F64 && mp_is_atom(op, "f64.const")) {
        const MasmToken* val_tok = mp_next(p);
        f64 v = 0.0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_f64(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid f64.const value");
            return FALSE;
        }
        *out_f64 = v;
    } else {
        masm_error_at(&p->lx, op->line, op->col, "type mismatch or invalid constant expression");
        return FALSE;
    }

    if (paren && !mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* (func $isim (export "...") (import "m" "n") (type $t) (param ...) (result ...) (local ...) body...) */
static b32 mp_parse_func_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->func_count >= MB_MAX_FUNCS) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many functions");
        return FALSE;
    }
    u32 func_idx = p->func_count++;
    MasmFuncDecl* fd = &p->funcs[func_idx];
    masm_memzero(fd, sizeof(MasmFuncDecl));
    fd->line = kw_tok->line;
    fd->col = kw_tok->col;

    if (mp_is_id(mp_peek(p))) {
        const MasmToken* id = mp_next(p);
        for (u32 i = 0; i < func_idx; i++) {
            if (p->funcs[i].name.len > 0 &&
                masm_streq_n(p->funcs[i].name.ptr, p->funcs[i].name.len, id->ptr, id->len)) {
                masm_error_at(&p->lx, id->line, id->col, "duplicate function identifier");
                return FALSE;
            }
        }
        fd->name.ptr = id->ptr;
        fd->name.len = id->len;
    }

    /* Satır içi (export "...") */
    while (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "export")) {
        mp_next(p); /* '(' */
        mp_next(p); /* 'export' */
        const MasmToken* str_tok = mp_next(p);
        if (str_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, str_tok->line, str_tok->col, "expected export name string");
            return FALSE;
        }
        if (!mp_add_inline_export(p, str_tok, MB_EXT_FUNC, func_idx)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
    }

    /* Satır içi (import "mod" "field") */
    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "import")) {
        if (p->seen_non_import) {
            masm_error_at(&p->lx, mp_peek(p)->line, mp_peek(p)->col,
                          "imports must occur before all non-import definitions");
            return FALSE;
        }
        mp_next(p); /* '(' */
        mp_next(p); /* 'import' */
        const MasmToken* mod_tok = mp_next(p);
        const MasmToken* fld_tok = mp_next(p);
        if (mod_tok->kind != TOK_STRING || fld_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, mod_tok->line, mod_tok->col, "expected import module and field strings");
            return FALSE;
        }
        b32 ok1 = FALSE, ok2 = FALSE;
        fd->imp_mod = mp_unescape_name_to_arena(p, mod_tok, &ok1);
        fd->imp_field = mp_unescape_name_to_arena(p, fld_tok, &ok2);
        if (!ok1 || !ok2) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
        if (!mp_record_import(p, kw_tok, MB_EXT_FUNC, func_idx)) return FALSE;
        fd->is_import = TRUE;
        p->imported_func_count++;
    } else {
        p->seen_non_import = TRUE;
    }

    /* İsteğe bağlı (type $t | idx) */
    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "type")) {
        mp_next(p); /* '(' */
        mp_next(p); /* 'type' */
        fd->has_type_use = TRUE;
        fd->type_use_tok = *mp_next(p);
        if (!mp_expect_rparen(p)) return FALSE;
    }

    /* (param ...) ve (result ...) */
    if (mp_peek(p)->kind == TOK_LPAREN &&
        (mp_is_atom(mp_peek_n(p, 1), "param") || mp_is_atom(mp_peek_n(p, 1), "result"))) {
        fd->has_explicit_sig = TRUE;
        if (!mp_parse_func_sig(p, fd->params, fd->param_names, &fd->param_count,
                               fd->results, &fd->result_count, TRUE)) {
            return FALSE;
        }
    }

    if (fd->is_import) {
        if (!mp_expect_rparen(p)) return FALSE;
        return TRUE;
    }

    /* (local ...) bildirimleri */
    while (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "local")) {
        mp_next(p); /* '(' */
        mp_next(p); /* 'local' */
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* id_tok = mp_next(p);
            /* Parametre ve önceki local isimleriyle çakışma kontrolü */
            for (u32 k = 0; k < fd->param_count; k++) {
                if (fd->param_names[k].len > 0 &&
                    masm_streq_n(fd->param_names[k].ptr, fd->param_names[k].len, id_tok->ptr, id_tok->len)) {
                    masm_error_at(&p->lx, id_tok->line, id_tok->col, "duplicate local/param identifier");
                    return FALSE;
                }
            }
            for (u32 k = 0; k < fd->local_count; k++) {
                if (fd->local_names[k].len > 0 &&
                    masm_streq_n(fd->local_names[k].ptr, fd->local_names[k].len, id_tok->ptr, id_tok->len)) {
                    masm_error_at(&p->lx, id_tok->line, id_tok->col, "duplicate local identifier");
                    return FALSE;
                }
            }
            const MasmToken* vt_tok = mp_next(p);
            u8 vt = 0;
            if (!mp_parse_valtype(vt_tok, &vt)) {
                masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in named (local)");
                return FALSE;
            }
            if (fd->local_count >= MASM_MAX_LOCALS) {
                masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "too many locals");
                return FALSE;
            }
            fd->locals[fd->local_count] = vt;
            fd->local_names[fd->local_count].ptr = id_tok->ptr;
            fd->local_names[fd->local_count].len = id_tok->len;
            fd->local_count++;
            if (!mp_expect_rparen(p)) return FALSE;
        } else {
            while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                const MasmToken* vt_tok = mp_next(p);
                u8 vt = 0;
                if (!mp_parse_valtype(vt_tok, &vt)) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in (local)");
                    return FALSE;
                }
                if (fd->local_count >= MASM_MAX_LOCALS) {
                    masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "too many locals");
                    return FALSE;
                }
                fd->locals[fd->local_count] = vt;
                fd->local_names[fd->local_count].ptr = NULL;
                fd->local_names[fd->local_count].len = 0;
                fd->local_count++;
            }
            if (!mp_expect_rparen(p)) return FALSE;
        }
    }

    /* Gövde başlangıç ve bitiş token aralığını kaydet (Pass 2'de derlenecek) */
    fd->body_start_tok = p->lx.pos;
    if (!mp_skip_sexpr(p)) return FALSE;
    fd->body_end_tok = p->lx.pos - 1; /* Kapanış ')' token'ı */
    return TRUE;
}

/* (memory $m (export "...") min [max] [shared]) */
static b32 mp_parse_memory_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->has_memory) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "only one memory is allowed");
        return FALSE;
    }
    p->has_memory = TRUE;
    if (mp_is_id(mp_peek(p))) {
        const MasmToken* id = mp_next(p);
        p->mem_name.ptr = id->ptr;
        p->mem_name.len = id->len;
    }
    while (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "export")) {
        mp_next(p);
        mp_next(p);
        const MasmToken* str_tok = mp_next(p);
        if (str_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, str_tok->line, str_tok->col, "expected export name string");
            return FALSE;
        }
        if (!mp_add_inline_export(p, str_tok, MB_EXT_MEMORY, 0)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
    }
    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "import")) {
        if (p->seen_non_import) {
            masm_error_at(&p->lx, mp_peek(p)->line, mp_peek(p)->col, "imports must occur before definitions");
            return FALSE;
        }
        mp_next(p);
        mp_next(p);
        const MasmToken* mod_tok = mp_next(p);
        const MasmToken* fld_tok = mp_next(p);
        if (mod_tok->kind != TOK_STRING || fld_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, mod_tok->line, mod_tok->col, "expected import module and field strings");
            return FALSE;
        }
        b32 ok1 = FALSE, ok2 = FALSE;
        p->mem_imp_mod = mp_unescape_name_to_arena(p, mod_tok, &ok1);
        p->mem_imp_field = mp_unescape_name_to_arena(p, fld_tok, &ok2);
        if (!ok1 || !ok2) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
        if (!mp_record_import(p, kw_tok, MB_EXT_MEMORY, 0)) return FALSE;
        p->mem_is_import = TRUE;
    } else {
        p->seen_non_import = TRUE;
    }
    if (!mp_parse_limits(p, &p->mem_min, &p->mem_max, &p->mem_has_max, &p->mem_shared)) return FALSE;
    if (!mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* (table $t (export "...") min [max] funcref) */
static b32 mp_parse_table_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->has_table) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "only one table is allowed");
        return FALSE;
    }
    p->has_table = TRUE;
    if (mp_is_id(mp_peek(p))) {
        const MasmToken* id = mp_next(p);
        p->table_name.ptr = id->ptr;
        p->table_name.len = id->len;
    }
    while (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "export")) {
        mp_next(p);
        mp_next(p);
        const MasmToken* str_tok = mp_next(p);
        if (str_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, str_tok->line, str_tok->col, "expected export name string");
            return FALSE;
        }
        if (!mp_add_inline_export(p, str_tok, MB_EXT_TABLE, 0)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
    }
    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "import")) {
        if (p->seen_non_import) {
            masm_error_at(&p->lx, mp_peek(p)->line, mp_peek(p)->col, "imports must occur before definitions");
            return FALSE;
        }
        mp_next(p);
        mp_next(p);
        const MasmToken* mod_tok = mp_next(p);
        const MasmToken* fld_tok = mp_next(p);
        if (mod_tok->kind != TOK_STRING || fld_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, mod_tok->line, mod_tok->col, "expected import module and field strings");
            return FALSE;
        }
        b32 ok1 = FALSE, ok2 = FALSE;
        p->table_imp_mod = mp_unescape_name_to_arena(p, mod_tok, &ok1);
        p->table_imp_field = mp_unescape_name_to_arena(p, fld_tok, &ok2);
        if (!ok1 || !ok2) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
        if (!mp_record_import(p, kw_tok, MB_EXT_TABLE, 0)) return FALSE;
        p->table_is_import = TRUE;
    } else {
        p->seen_non_import = TRUE;
    }
    if (!mp_parse_limits(p, &p->table_min, &p->table_max, &p->table_has_max, NULL)) return FALSE;
    const MasmToken* rt = mp_next(p);
    if (!mp_is_atom(rt, "funcref") && !mp_is_atom(rt, "anyfunc")) {
        masm_error_at(&p->lx, rt->line, rt->col, "expected 'funcref' in table declaration");
        return FALSE;
    }
    if (!mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* (global $g (export "...") (mut T)|T (T.const val)) */
static b32 mp_parse_global_decl(MasmParser* p, const MasmToken* kw_tok) {
    if (p->global_count >= MB_MAX_GLOBALS) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many globals");
        return FALSE;
    }
    u32 g_idx = p->global_count++;
    MasmGlobalDecl* gd = &p->globals[g_idx];
    masm_memzero(gd, sizeof(MasmGlobalDecl));
    gd->line = kw_tok->line;
    gd->col = kw_tok->col;

    if (mp_is_id(mp_peek(p))) {
        const MasmToken* id = mp_next(p);
        for (u32 i = 0; i < g_idx; i++) {
            if (p->globals[i].name.len > 0 &&
                masm_streq_n(p->globals[i].name.ptr, p->globals[i].name.len, id->ptr, id->len)) {
                masm_error_at(&p->lx, id->line, id->col, "duplicate global identifier");
                return FALSE;
            }
        }
        gd->name.ptr = id->ptr;
        gd->name.len = id->len;
    }

    while (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "export")) {
        mp_next(p);
        mp_next(p);
        const MasmToken* str_tok = mp_next(p);
        if (str_tok->kind != TOK_STRING) {
            masm_error_at(&p->lx, str_tok->line, str_tok->col, "expected export name string");
            return FALSE;
        }
        if (!mp_add_inline_export(p, str_tok, MB_EXT_GLOBAL, g_idx)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
    }

    p->seen_non_import = TRUE;

    if (mp_peek(p)->kind == TOK_LPAREN) {
        mp_next(p); /* '(' */
        const MasmToken* mut_kw = mp_next(p);
        if (!mp_is_atom(mut_kw, "mut")) {
            masm_error_at(&p->lx, mut_kw->line, mut_kw->col, "expected 'mut' in global type");
            return FALSE;
        }
        const MasmToken* vt_tok = mp_next(p);
        if (!mp_parse_valtype(vt_tok, &gd->valtype)) {
            masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in (mut ...)");
            return FALSE;
        }
        gd->mutable_ = TRUE;
        if (!mp_expect_rparen(p)) return FALSE;
    } else {
        const MasmToken* vt_tok = mp_next(p);
        if (!mp_parse_valtype(vt_tok, &gd->valtype)) {
            masm_error_at(&p->lx, vt_tok->line, vt_tok->col, "expected value type in global declaration");
            return FALSE;
        }
        gd->mutable_ = FALSE;
    }

    gd->init_type = gd->valtype;
    if (!mp_parse_const_expr(p, gd->valtype, &gd->init_i64, &gd->init_f32, &gd->init_f64)) return FALSE;
    if (!mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* (export "name" (func|memory|table|global $id|idx)) */
static b32 mp_parse_export_decl(MasmParser* p, const MasmToken* kw_tok) {
    p->seen_non_import = TRUE;
    if (p->export_count >= MB_MAX_EXPORTS) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many exports");
        return FALSE;
    }
    const MasmToken* str_tok = mp_next(p);
    if (str_tok->kind != TOK_STRING) {
        masm_error_at(&p->lx, str_tok->line, str_tok->col, "expected export name string");
        return FALSE;
    }
    b32 ok = FALSE;
    MasmSlice name = mp_unescape_name_to_arena(p, str_tok, &ok);
    if (!ok) return FALSE;

    if (!mp_expect_lparen(p)) return FALSE;
    const MasmToken* kind_tok = mp_next(p);
    u8 kind = 0;
    if (mp_is_atom(kind_tok, "func")) kind = MB_EXT_FUNC;
    else if (mp_is_atom(kind_tok, "table")) kind = MB_EXT_TABLE;
    else if (mp_is_atom(kind_tok, "memory")) kind = MB_EXT_MEMORY;
    else if (mp_is_atom(kind_tok, "global")) kind = MB_EXT_GLOBAL;
    else {
        masm_error_at(&p->lx, kind_tok->line, kind_tok->col, "invalid export kind");
        return FALSE;
    }

    const MasmToken* target = mp_next(p);
    if (target->kind != TOK_ATOM) {
        masm_error_at(&p->lx, target->line, target->col, "expected export target index or identifier");
        return FALSE;
    }
    if (!mp_expect_rparen(p)) return FALSE;
    if (!mp_expect_rparen(p)) return FALSE;

    MasmExportDecl* ex = &p->exports[p->export_count++];
    ex->name = name;
    ex->kind = kind;
    ex->target_tok = *target;
    ex->resolved = FALSE;
    ex->index = 0;
    return TRUE;
}

/* (elem (i32.const offset) [func] $f0 $f1 ...) */
static b32 mp_parse_elem_decl(MasmParser* p, const MasmToken* kw_tok) {
    p->seen_non_import = TRUE;
    if (p->elem_count >= MB_MAX_ELEMS) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many elem segments");
        return FALSE;
    }
    MasmElemDecl* el = &p->elems[p->elem_count++];
    masm_memzero(el, sizeof(MasmElemDecl));

    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "table")) {
        mp_next(p);
        mp_next(p);
        const MasmToken* tid = mp_next(p);
        u32 tidx = 0;
        if (!masm_parse_u32(tid->ptr, tid->len, &tidx) || tidx != 0) {
            masm_error_at(&p->lx, tid->line, tid->col, "invalid table index in elem");
            return FALSE;
        }
        if (!mp_expect_rparen(p)) return FALSE;
    }

    i64 off64 = 0;
    f32 dummy32 = 0;
    f64 dummy64 = 0;
    if (!mp_parse_const_expr(p, MB_VAL_I32, &off64, &dummy32, &dummy64)) return FALSE;
    el->offset = (i32)off64;

    if (mp_is_atom(mp_peek(p), "func")) {
        mp_next(p);
    }

    while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
        const MasmToken* ft = mp_next(p);
        if (ft->kind != TOK_ATOM) {
            masm_error_at(&p->lx, ft->line, ft->col, "expected function index or identifier in elem");
            return FALSE;
        }
        if (el->count >= MB_MAX_ELEM_FUNCS) {
            masm_error_at(&p->lx, ft->line, ft->col, "too many functions in elem segment");
            return FALSE;
        }
        el->func_toks[el->count++] = *ft;
    }
    if (!mp_expect_rparen(p)) return FALSE;
    return TRUE;
}

/* (data (i32.const offset) "bytes" ...) */
static b32 mp_parse_data_decl(MasmParser* p, const MasmToken* kw_tok) {
    p->seen_non_import = TRUE;
    if (p->data_count >= MB_MAX_DATAS) {
        masm_error_at(&p->lx, kw_tok->line, kw_tok->col, "too many data segments");
        return FALSE;
    }
    MasmDataDecl* d = &p->datas[p->data_count++];
    masm_memzero(d, sizeof(MasmDataDecl));

    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "memory")) {
        mp_next(p);
        mp_next(p);
        const MasmToken* mid = mp_next(p);
        u32 midx = 0;
        if (!masm_parse_u32(mid->ptr, mid->len, &midx) || midx != 0) {
            masm_error_at(&p->lx, mid->line, mid->col, "invalid memory index in data");
            return FALSE;
        }
        if (!mp_expect_rparen(p)) return FALSE;
    }

    i64 off64 = 0;
    f32 dummy32 = 0;
    f64 dummy64 = 0;
    if (!mp_parse_const_expr(p, MB_VAL_I32, &off64, &dummy32, &dummy64)) return FALSE;
    d->offset = (i32)off64;

    u8 tmp_buf[MASM_MAX_DATA_BYTES];
    u32 total_len = 0;
    while (mp_peek(p)->kind == TOK_STRING) {
        const MasmToken* st = mp_next(p);
        u32 chunk_len = 0;
        if (!masm_unescape_string(st->ptr, st->len, tmp_buf + total_len,
                                  MASM_MAX_DATA_BYTES - total_len, &chunk_len)) {
            masm_error_at(&p->lx, st->line, st->col, "invalid or too large data string");
            return FALSE;
        }
        total_len += chunk_len;
    }

    if (!mp_expect_rparen(p)) return FALSE;

    if (total_len > 0) {
        d->bytes = ARENA_PUSH_ARRAY(p->arena, u8, total_len);
        masm_memcpy(d->bytes, tmp_buf, total_len);
    }
    d->len = total_len;
    return TRUE;
}

/* ============================================================================
 * 5. Sembol Çözümleme Yardımcıları
 * ============================================================================ */

static b32 mp_resolve_type(MasmParser* p, const MasmToken* tok, u32* out_idx) {
    if (mp_is_id(tok)) {
        for (u32 i = 0; i < p->type_count; i++) {
            if (p->types[i].name.len > 0 &&
                masm_streq_n(p->types[i].name.ptr, p->types[i].name.len, tok->ptr, tok->len)) {
                *out_idx = i;
                return TRUE;
            }
        }
        masm_error_at(&p->lx, tok->line, tok->col, "undefined type variable %.*s", (int)tok->len, tok->ptr);
        return FALSE;
    }
    u32 idx = 0;
    if (!masm_parse_u32(tok->ptr, tok->len, &idx) || idx >= p->type_count) {
        masm_error_at(&p->lx, tok->line, tok->col, "type index out of range");
        return FALSE;
    }
    *out_idx = idx;
    return TRUE;
}

static b32 mp_resolve_func(MasmParser* p, const MasmToken* tok, u32* out_idx) {
    if (mp_is_id(tok)) {
        for (u32 i = 0; i < p->func_count; i++) {
            if (p->funcs[i].name.len > 0 &&
                masm_streq_n(p->funcs[i].name.ptr, p->funcs[i].name.len, tok->ptr, tok->len)) {
                *out_idx = i;
                return TRUE;
            }
        }
        masm_error_at(&p->lx, tok->line, tok->col, "undefined function variable %.*s", (int)tok->len, tok->ptr);
        return FALSE;
    }
    u32 idx = 0;
    if (!masm_parse_u32(tok->ptr, tok->len, &idx) || idx >= p->func_count) {
        masm_error_at(&p->lx, tok->line, tok->col, "function variable out of range");
        return FALSE;
    }
    *out_idx = idx;
    return TRUE;
}

static b32 mp_resolve_global(MasmParser* p, const MasmToken* tok, u32* out_idx) {
    if (mp_is_id(tok)) {
        for (u32 i = 0; i < p->global_count; i++) {
            if (p->globals[i].name.len > 0 &&
                masm_streq_n(p->globals[i].name.ptr, p->globals[i].name.len, tok->ptr, tok->len)) {
                *out_idx = i;
                return TRUE;
            }
        }
        masm_error_at(&p->lx, tok->line, tok->col, "undefined global variable %.*s", (int)tok->len, tok->ptr);
        return FALSE;
    }
    u32 idx = 0;
    if (!masm_parse_u32(tok->ptr, tok->len, &idx) || idx >= p->global_count) {
        masm_error_at(&p->lx, tok->line, tok->col, "global variable out of range");
        return FALSE;
    }
    *out_idx = idx;
    return TRUE;
}

static b32 mp_resolve_local(MasmParser* p, const MasmFuncDecl* fd, const MasmToken* tok, u32* out_idx) {
    u32 total_locals = fd->param_count + fd->local_count;
    if (mp_is_id(tok)) {
        for (u32 i = 0; i < fd->param_count; i++) {
            if (fd->param_names[i].len > 0 &&
                masm_streq_n(fd->param_names[i].ptr, fd->param_names[i].len, tok->ptr, tok->len)) {
                *out_idx = i;
                return TRUE;
            }
        }
        for (u32 i = 0; i < fd->local_count; i++) {
            if (fd->local_names[i].len > 0 &&
                masm_streq_n(fd->local_names[i].ptr, fd->local_names[i].len, tok->ptr, tok->len)) {
                *out_idx = fd->param_count + i;
                return TRUE;
            }
        }
        masm_error_at(&p->lx, tok->line, tok->col, "undefined local variable %.*s", (int)tok->len, tok->ptr);
        return FALSE;
    }
    u32 idx = 0;
    if (!masm_parse_u32(tok->ptr, tok->len, &idx) || idx >= total_locals) {
        masm_error_at(&p->lx, tok->line, tok->col, "local variable out of range");
        return FALSE;
    }
    *out_idx = idx;
    return TRUE;
}

/* ============================================================================
 * 6. Komut (Instruction) Tablosu ve Fonksiyon Gövdesi Derleyicisi
 * ============================================================================ */

typedef enum {
    OPK_SIMPLE = 0,
    OPK_SIMD_SIMPLE,
    OPK_SAT_TRUNC,
    OPK_MEM,
    OPK_MEM_ZERO,
    OPK_MEM_BULK,
    OPK_SIMD_MEM,
    OPK_SIMD_MEM_LANE_2,
    OPK_SIMD_MEM_LANE_4,
    OPK_SIMD_MEM_LANE_8,
    OPK_SIMD_MEM_LANE_16,
    OPK_LANE_2,
    OPK_LANE_4,
    OPK_LANE_8,
    OPK_LANE_16
} MasmOpKind;

typedef struct {
    const char* name;
    u8          kind;
    u16         code;
    u8          def_align; /* log2(byte_align) */
} MasmOpInfo;

static const MasmOpInfo g_masm_ops[] = {
    /* Kontrol ve parametrik */
    { "unreachable",                     OPK_SIMPLE,           0x00u, 0 },
    { "nop",                             OPK_SIMPLE,           0x01u, 0 },
    { "return",                          OPK_SIMPLE,           0x0Fu, 0 },
    { "drop",                            OPK_SIMPLE,           0x1Au, 0 },
    { "select",                          OPK_SIMPLE,           0x1Bu, 0 },

    /* Bellek komutları */
    { "i32.load",                        OPK_MEM,              0x28u, 2 },
    { "i64.load",                        OPK_MEM,              0x29u, 3 },
    { "f32.load",                        OPK_MEM,              0x2Au, 2 },
    { "f64.load",                        OPK_MEM,              0x2Bu, 3 },
    { "i32.load8_s",                     OPK_MEM,              0x2Cu, 0 },
    { "i32.load8_u",                     OPK_MEM,              0x2Du, 0 },
    { "i32.load16_s",                    OPK_MEM,              0x2Eu, 1 },
    { "i32.load16_u",                    OPK_MEM,              0x2Fu, 1 },
    { "i64.load8_s",                     OPK_MEM,              0x30u, 0 },
    { "i64.load8_u",                     OPK_MEM,              0x31u, 0 },
    { "i64.load16_s",                    OPK_MEM,              0x32u, 1 },
    { "i64.load16_u",                    OPK_MEM,              0x33u, 1 },
    { "i64.load32_s",                    OPK_MEM,              0x34u, 2 },
    { "i64.load32_u",                    OPK_MEM,              0x35u, 2 },
    { "i32.store",                       OPK_MEM,              0x36u, 2 },
    { "i64.store",                       OPK_MEM,              0x37u, 3 },
    { "f32.store",                       OPK_MEM,              0x38u, 2 },
    { "f64.store",                       OPK_MEM,              0x39u, 3 },
    { "i32.store8",                      OPK_MEM,              0x3Au, 0 },
    { "i32.store16",                     OPK_MEM,              0x3Bu, 1 },
    { "i64.store8",                      OPK_MEM,              0x3Cu, 0 },
    { "i64.store16",                     OPK_MEM,              0x3Du, 1 },
    { "i64.store32",                     OPK_MEM,              0x3Eu, 2 },
    { "memory.size",                     OPK_MEM_ZERO,         0x3Fu, 0 },
    { "memory.grow",                     OPK_MEM_ZERO,         0x40u, 0 },
    { "memory.copy",                     OPK_MEM_BULK,         0x0Au, 0 },
    { "memory.fill",                     OPK_MEM_BULK,         0x0Bu, 0 },

    /* i32 karşılaştırma */
    { "i32.eqz",                         OPK_SIMPLE,           0x45u, 0 },
    { "i32.eq",                          OPK_SIMPLE,           0x46u, 0 },
    { "i32.ne",                          OPK_SIMPLE,           0x47u, 0 },
    { "i32.lt_s",                        OPK_SIMPLE,           0x48u, 0 },
    { "i32.lt_u",                        OPK_SIMPLE,           0x49u, 0 },
    { "i32.gt_s",                        OPK_SIMPLE,           0x4Au, 0 },
    { "i32.gt_u",                        OPK_SIMPLE,           0x4Bu, 0 },
    { "i32.le_s",                        OPK_SIMPLE,           0x4Cu, 0 },
    { "i32.le_u",                        OPK_SIMPLE,           0x4Du, 0 },
    { "i32.ge_s",                        OPK_SIMPLE,           0x4Eu, 0 },
    { "i32.ge_u",                        OPK_SIMPLE,           0x4Fu, 0 },

    /* i64 karşılaştırma */
    { "i64.eqz",                         OPK_SIMPLE,           0x50u, 0 },
    { "i64.eq",                          OPK_SIMPLE,           0x51u, 0 },
    { "i64.ne",                          OPK_SIMPLE,           0x52u, 0 },
    { "i64.lt_s",                        OPK_SIMPLE,           0x53u, 0 },
    { "i64.lt_u",                        OPK_SIMPLE,           0x54u, 0 },
    { "i64.gt_s",                        OPK_SIMPLE,           0x55u, 0 },
    { "i64.gt_u",                        OPK_SIMPLE,           0x56u, 0 },
    { "i64.le_s",                        OPK_SIMPLE,           0x57u, 0 },
    { "i64.le_u",                        OPK_SIMPLE,           0x58u, 0 },
    { "i64.ge_s",                        OPK_SIMPLE,           0x59u, 0 },
    { "i64.ge_u",                        OPK_SIMPLE,           0x5Au, 0 },

    /* f32 ve f64 karşılaştırma */
    { "f32.eq",                          OPK_SIMPLE,           0x5Bu, 0 },
    { "f32.ne",                          OPK_SIMPLE,           0x5Cu, 0 },
    { "f32.lt",                          OPK_SIMPLE,           0x5Du, 0 },
    { "f32.gt",                          OPK_SIMPLE,           0x5Eu, 0 },
    { "f32.le",                          OPK_SIMPLE,           0x5Fu, 0 },
    { "f32.ge",                          OPK_SIMPLE,           0x60u, 0 },
    { "f64.eq",                          OPK_SIMPLE,           0x61u, 0 },
    { "f64.ne",                          OPK_SIMPLE,           0x62u, 0 },
    { "f64.lt",                          OPK_SIMPLE,           0x63u, 0 },
    { "f64.gt",                          OPK_SIMPLE,           0x64u, 0 },
    { "f64.le",                          OPK_SIMPLE,           0x65u, 0 },
    { "f64.ge",                          OPK_SIMPLE,           0x66u, 0 },

    /* i32 aritmetik ve bit */
    { "i32.clz",                         OPK_SIMPLE,           0x67u, 0 },
    { "i32.ctz",                         OPK_SIMPLE,           0x68u, 0 },
    { "i32.popcnt",                      OPK_SIMPLE,           0x69u, 0 },
    { "i32.add",                         OPK_SIMPLE,           0x6Au, 0 },
    { "i32.sub",                         OPK_SIMPLE,           0x6Bu, 0 },
    { "i32.mul",                         OPK_SIMPLE,           0x6Cu, 0 },
    { "i32.div_s",                       OPK_SIMPLE,           0x6Du, 0 },
    { "i32.div_u",                       OPK_SIMPLE,           0x6Eu, 0 },
    { "i32.rem_s",                       OPK_SIMPLE,           0x6Fu, 0 },
    { "i32.rem_u",                       OPK_SIMPLE,           0x70u, 0 },
    { "i32.and",                         OPK_SIMPLE,           0x71u, 0 },
    { "i32.or",                          OPK_SIMPLE,           0x72u, 0 },
    { "i32.xor",                         OPK_SIMPLE,           0x73u, 0 },
    { "i32.shl",                         OPK_SIMPLE,           0x74u, 0 },
    { "i32.shr_s",                       OPK_SIMPLE,           0x75u, 0 },
    { "i32.shr_u",                       OPK_SIMPLE,           0x76u, 0 },
    { "i32.rotl",                        OPK_SIMPLE,           0x77u, 0 },
    { "i32.rotr",                        OPK_SIMPLE,           0x78u, 0 },

    /* i64 aritmetik ve bit */
    { "i64.clz",                         OPK_SIMPLE,           0x79u, 0 },
    { "i64.ctz",                         OPK_SIMPLE,           0x7Au, 0 },
    { "i64.popcnt",                      OPK_SIMPLE,           0x7Bu, 0 },
    { "i64.add",                         OPK_SIMPLE,           0x7Cu, 0 },
    { "i64.sub",                         OPK_SIMPLE,           0x7Du, 0 },
    { "i64.mul",                         OPK_SIMPLE,           0x7Eu, 0 },
    { "i64.div_s",                       OPK_SIMPLE,           0x7Fu, 0 },
    { "i64.div_u",                       OPK_SIMPLE,           0x80u, 0 },
    { "i64.rem_s",                       OPK_SIMPLE,           0x81u, 0 },
    { "i64.rem_u",                       OPK_SIMPLE,           0x82u, 0 },
    { "i64.and",                         OPK_SIMPLE,           0x83u, 0 },
    { "i64.or",                          OPK_SIMPLE,           0x84u, 0 },
    { "i64.xor",                         OPK_SIMPLE,           0x85u, 0 },
    { "i64.shl",                         OPK_SIMPLE,           0x86u, 0 },
    { "i64.shr_s",                       OPK_SIMPLE,           0x87u, 0 },
    { "i64.shr_u",                       OPK_SIMPLE,           0x88u, 0 },
    { "i64.rotl",                        OPK_SIMPLE,           0x89u, 0 },
    { "i64.rotr",                        OPK_SIMPLE,           0x8Au, 0 },

    /* f32 ve f64 aritmetik ve matematik */
    { "f32.abs",                         OPK_SIMPLE,           0x8Bu, 0 },
    { "f32.neg",                         OPK_SIMPLE,           0x8Cu, 0 },
    { "f32.ceil",                        OPK_SIMPLE,           0x8Du, 0 },
    { "f32.floor",                       OPK_SIMPLE,           0x8Eu, 0 },
    { "f32.trunc",                       OPK_SIMPLE,           0x8Fu, 0 },
    { "f32.nearest",                     OPK_SIMPLE,           0x90u, 0 },
    { "f32.sqrt",                        OPK_SIMPLE,           0x91u, 0 },
    { "f32.add",                         OPK_SIMPLE,           0x92u, 0 },
    { "f32.sub",                         OPK_SIMPLE,           0x93u, 0 },
    { "f32.mul",                         OPK_SIMPLE,           0x94u, 0 },
    { "f32.div",                         OPK_SIMPLE,           0x95u, 0 },
    { "f32.min",                         OPK_SIMPLE,           0x96u, 0 },
    { "f32.max",                         OPK_SIMPLE,           0x97u, 0 },
    { "f32.copysign",                    OPK_SIMPLE,           0x98u, 0 },
    { "f64.abs",                         OPK_SIMPLE,           0x99u, 0 },
    { "f64.neg",                         OPK_SIMPLE,           0x9Au, 0 },
    { "f64.ceil",                        OPK_SIMPLE,           0x9Bu, 0 },
    { "f64.floor",                       OPK_SIMPLE,           0x9Cu, 0 },
    { "f64.trunc",                       OPK_SIMPLE,           0x9Du, 0 },
    { "f64.nearest",                     OPK_SIMPLE,           0x9Eu, 0 },
    { "f64.sqrt",                        OPK_SIMPLE,           0x9Fu, 0 },
    { "f64.add",                         OPK_SIMPLE,           0xA0u, 0 },
    { "f64.sub",                         OPK_SIMPLE,           0xA1u, 0 },
    { "f64.mul",                         OPK_SIMPLE,           0xA2u, 0 },
    { "f64.div",                         OPK_SIMPLE,           0xA3u, 0 },
    { "f64.min",                         OPK_SIMPLE,           0xA4u, 0 },
    { "f64.max",                         OPK_SIMPLE,           0xA5u, 0 },
    { "f64.copysign",                    OPK_SIMPLE,           0xA6u, 0 },

    /* Dönüşümler ve işaret genişletme */
    { "i32.wrap_i64",                    OPK_SIMPLE,           0xA7u, 0 },
    { "i32.trunc_f32_s",                 OPK_SIMPLE,           0xA8u, 0 },
    { "i32.trunc_f32_u",                 OPK_SIMPLE,           0xA9u, 0 },
    { "i32.trunc_f64_s",                 OPK_SIMPLE,           0xAAu, 0 },
    { "i32.trunc_f64_u",                 OPK_SIMPLE,           0xABu, 0 },
    { "i64.extend_i32_s",                OPK_SIMPLE,           0xACu, 0 },
    { "i64.extend_i32_u",                OPK_SIMPLE,           0xADu, 0 },
    { "i64.trunc_f32_s",                 OPK_SIMPLE,           0xAEu, 0 },
    { "i64.trunc_f32_u",                 OPK_SIMPLE,           0xAFu, 0 },
    { "i64.trunc_f64_s",                 OPK_SIMPLE,           0xB0u, 0 },
    { "i64.trunc_f64_u",                 OPK_SIMPLE,           0xB1u, 0 },
    { "f32.convert_i32_s",               OPK_SIMPLE,           0xB2u, 0 },
    { "f32.convert_i32_u",               OPK_SIMPLE,           0xB3u, 0 },
    { "f32.convert_i64_s",               OPK_SIMPLE,           0xB4u, 0 },
    { "f32.convert_i64_u",               OPK_SIMPLE,           0xB5u, 0 },
    { "f32.demote_f64",                  OPK_SIMPLE,           0xB6u, 0 },
    { "f64.convert_i32_s",               OPK_SIMPLE,           0xB7u, 0 },
    { "f64.convert_i32_u",               OPK_SIMPLE,           0xB8u, 0 },
    { "f64.convert_i64_s",               OPK_SIMPLE,           0xB9u, 0 },
    { "f64.convert_i64_u",               OPK_SIMPLE,           0xBAu, 0 },
    { "f64.promote_f32",                 OPK_SIMPLE,           0xBBu, 0 },
    { "i32.reinterpret_f32",             OPK_SIMPLE,           0xBCu, 0 },
    { "i64.reinterpret_f64",             OPK_SIMPLE,           0xBDu, 0 },
    { "f32.reinterpret_i32",             OPK_SIMPLE,           0xBEu, 0 },
    { "f64.reinterpret_i64",             OPK_SIMPLE,           0xBFu, 0 },
    { "i32.extend8_s",                   OPK_SIMPLE,           0xC0u, 0 },
    { "i32.extend16_s",                  OPK_SIMPLE,           0xC1u, 0 },
    { "i64.extend8_s",                   OPK_SIMPLE,           0xC2u, 0 },
    { "i64.extend16_s",                  OPK_SIMPLE,           0xC3u, 0 },
    { "i64.extend32_s",                  OPK_SIMPLE,           0xC4u, 0 },

    { "i32.trunc_sat_f32_s",             OPK_SAT_TRUNC,        0x00u, 0 },
    { "i32.trunc_sat_f32_u",             OPK_SAT_TRUNC,        0x01u, 0 },
    { "i32.trunc_sat_f64_s",             OPK_SAT_TRUNC,        0x02u, 0 },
    { "i32.trunc_sat_f64_u",             OPK_SAT_TRUNC,        0x03u, 0 },
    { "i64.trunc_sat_f32_s",             OPK_SAT_TRUNC,        0x04u, 0 },
    { "i64.trunc_sat_f32_u",             OPK_SAT_TRUNC,        0x05u, 0 },
    { "i64.trunc_sat_f64_s",             OPK_SAT_TRUNC,        0x06u, 0 },
    { "i64.trunc_sat_f64_u",             OPK_SAT_TRUNC,        0x07u, 0 },

    /* SIMD bellek komutları (genişleten, splat, zero, lane dahil) */
    { "v128.load",                       OPK_SIMD_MEM,         0x00u, 4 },
    { "v128.load8x8_s",                  OPK_SIMD_MEM,         0x01u, 3 },
    { "v128.load8x8_u",                  OPK_SIMD_MEM,         0x02u, 3 },
    { "v128.load16x4_s",                 OPK_SIMD_MEM,         0x03u, 3 },
    { "v128.load16x4_u",                 OPK_SIMD_MEM,         0x04u, 3 },
    { "v128.load32x2_s",                 OPK_SIMD_MEM,         0x05u, 3 },
    { "v128.load32x2_u",                 OPK_SIMD_MEM,         0x06u, 3 },
    { "v128.load8_splat",                OPK_SIMD_MEM,         0x07u, 0 },
    { "v128.load16_splat",               OPK_SIMD_MEM,         0x08u, 1 },
    { "v128.load32_splat",               OPK_SIMD_MEM,         0x09u, 2 },
    { "v128.load64_splat",               OPK_SIMD_MEM,         0x0Au, 3 },
    { "v128.store",                      OPK_SIMD_MEM,         0x0Bu, 4 },
    { "v128.load8_lane",                 OPK_SIMD_MEM_LANE_16, 0x54u, 0 },
    { "v128.load16_lane",                OPK_SIMD_MEM_LANE_8,  0x55u, 1 },
    { "v128.load32_lane",                OPK_SIMD_MEM_LANE_4,  0x56u, 2 },
    { "v128.load64_lane",                OPK_SIMD_MEM_LANE_2,  0x57u, 3 },
    { "v128.store8_lane",                OPK_SIMD_MEM_LANE_16, 0x58u, 0 },
    { "v128.store16_lane",               OPK_SIMD_MEM_LANE_8,  0x59u, 1 },
    { "v128.store32_lane",               OPK_SIMD_MEM_LANE_4,  0x5Au, 2 },
    { "v128.store64_lane",               OPK_SIMD_MEM_LANE_2,  0x5Bu, 3 },
    { "v128.load32_zero",                OPK_SIMD_MEM,         0x5Cu, 2 },
    { "v128.load64_zero",                OPK_SIMD_MEM,         0x5Du, 3 },

    /* SIMD swizzle, splat ve lane erişimi */
    { "i8x16.swizzle",                   OPK_SIMD_SIMPLE,      0x0Eu, 0 },
    { "i8x16.splat",                     OPK_SIMD_SIMPLE,      0x0Fu, 0 },
    { "i16x8.splat",                     OPK_SIMD_SIMPLE,      0x10u, 0 },
    { "i32x4.splat",                     OPK_SIMD_SIMPLE,      0x11u, 0 },
    { "i64x2.splat",                     OPK_SIMD_SIMPLE,      0x12u, 0 },
    { "f32x4.splat",                     OPK_SIMD_SIMPLE,      0x13u, 0 },
    { "f64x2.splat",                     OPK_SIMD_SIMPLE,      0x14u, 0 },
    { "i8x16.extract_lane_s",            OPK_LANE_16,          0x15u, 0 },
    { "i8x16.extract_lane_u",            OPK_LANE_16,          0x16u, 0 },
    { "i8x16.replace_lane",              OPK_LANE_16,          0x17u, 0 },
    { "i16x8.extract_lane_s",            OPK_LANE_8,           0x18u, 0 },
    { "i16x8.extract_lane_u",            OPK_LANE_8,           0x19u, 0 },
    { "i16x8.replace_lane",              OPK_LANE_8,           0x1Au, 0 },
    { "i32x4.extract_lane",              OPK_LANE_4,           0x1Bu, 0 },
    { "i32x4.replace_lane",              OPK_LANE_4,           0x1Cu, 0 },
    { "i64x2.extract_lane",              OPK_LANE_2,           0x1Du, 0 },
    { "i64x2.replace_lane",              OPK_LANE_2,           0x1Eu, 0 },
    { "f32x4.extract_lane",              OPK_LANE_4,           0x1Fu, 0 },
    { "f32x4.replace_lane",              OPK_LANE_4,           0x20u, 0 },
    { "f64x2.extract_lane",              OPK_LANE_2,           0x21u, 0 },
    { "f64x2.replace_lane",              OPK_LANE_2,           0x22u, 0 },

    /* SIMD i8x16 karşılaştırma */
    { "i8x16.eq",                        OPK_SIMD_SIMPLE,      0x23u, 0 },
    { "i8x16.ne",                        OPK_SIMD_SIMPLE,      0x24u, 0 },
    { "i8x16.lt_s",                      OPK_SIMD_SIMPLE,      0x25u, 0 },
    { "i8x16.lt_u",                      OPK_SIMD_SIMPLE,      0x26u, 0 },
    { "i8x16.gt_s",                      OPK_SIMD_SIMPLE,      0x27u, 0 },
    { "i8x16.gt_u",                      OPK_SIMD_SIMPLE,      0x28u, 0 },
    { "i8x16.le_s",                      OPK_SIMD_SIMPLE,      0x29u, 0 },
    { "i8x16.le_u",                      OPK_SIMD_SIMPLE,      0x2Au, 0 },
    { "i8x16.ge_s",                      OPK_SIMD_SIMPLE,      0x2Bu, 0 },
    { "i8x16.ge_u",                      OPK_SIMD_SIMPLE,      0x2Cu, 0 },

    /* SIMD i16x8 karşılaştırma */
    { "i16x8.eq",                        OPK_SIMD_SIMPLE,      0x2Du, 0 },
    { "i16x8.ne",                        OPK_SIMD_SIMPLE,      0x2Eu, 0 },
    { "i16x8.lt_s",                      OPK_SIMD_SIMPLE,      0x2Fu, 0 },
    { "i16x8.lt_u",                      OPK_SIMD_SIMPLE,      0x30u, 0 },
    { "i16x8.gt_s",                      OPK_SIMD_SIMPLE,      0x31u, 0 },
    { "i16x8.gt_u",                      OPK_SIMD_SIMPLE,      0x32u, 0 },
    { "i16x8.le_s",                      OPK_SIMD_SIMPLE,      0x33u, 0 },
    { "i16x8.le_u",                      OPK_SIMD_SIMPLE,      0x34u, 0 },
    { "i16x8.ge_s",                      OPK_SIMD_SIMPLE,      0x35u, 0 },
    { "i16x8.ge_u",                      OPK_SIMD_SIMPLE,      0x36u, 0 },

    /* SIMD i32x4 karşılaştırma */
    { "i32x4.eq",                        OPK_SIMD_SIMPLE,      0x37u, 0 },
    { "i32x4.ne",                        OPK_SIMD_SIMPLE,      0x38u, 0 },
    { "i32x4.lt_s",                      OPK_SIMD_SIMPLE,      0x39u, 0 },
    { "i32x4.lt_u",                      OPK_SIMD_SIMPLE,      0x3Au, 0 },
    { "i32x4.gt_s",                      OPK_SIMD_SIMPLE,      0x3Bu, 0 },
    { "i32x4.gt_u",                      OPK_SIMD_SIMPLE,      0x3Cu, 0 },
    { "i32x4.le_s",                      OPK_SIMD_SIMPLE,      0x3Du, 0 },
    { "i32x4.le_u",                      OPK_SIMD_SIMPLE,      0x3Eu, 0 },
    { "i32x4.ge_s",                      OPK_SIMD_SIMPLE,      0x3Fu, 0 },
    { "i32x4.ge_u",                      OPK_SIMD_SIMPLE,      0x40u, 0 },

    /* SIMD f32x4 ve f64x2 karşılaştırma */
    { "f32x4.eq",                        OPK_SIMD_SIMPLE,      0x41u, 0 },
    { "f32x4.ne",                        OPK_SIMD_SIMPLE,      0x42u, 0 },
    { "f32x4.lt",                        OPK_SIMD_SIMPLE,      0x43u, 0 },
    { "f32x4.gt",                        OPK_SIMD_SIMPLE,      0x44u, 0 },
    { "f32x4.le",                        OPK_SIMD_SIMPLE,      0x45u, 0 },
    { "f32x4.ge",                        OPK_SIMD_SIMPLE,      0x46u, 0 },
    { "f64x2.eq",                        OPK_SIMD_SIMPLE,      0x47u, 0 },
    { "f64x2.ne",                        OPK_SIMD_SIMPLE,      0x48u, 0 },
    { "f64x2.lt",                        OPK_SIMD_SIMPLE,      0x49u, 0 },
    { "f64x2.gt",                        OPK_SIMD_SIMPLE,      0x4Au, 0 },
    { "f64x2.le",                        OPK_SIMD_SIMPLE,      0x4Bu, 0 },
    { "f64x2.ge",                        OPK_SIMD_SIMPLE,      0x4Cu, 0 },

    /* SIMD v128 bit ve maske */
    { "v128.not",                        OPK_SIMD_SIMPLE,      0x4Du, 0 },
    { "v128.and",                        OPK_SIMD_SIMPLE,      0x4Eu, 0 },
    { "v128.andnot",                     OPK_SIMD_SIMPLE,      0x4Fu, 0 },
    { "v128.or",                         OPK_SIMD_SIMPLE,      0x50u, 0 },
    { "v128.xor",                        OPK_SIMD_SIMPLE,      0x51u, 0 },
    { "v128.bitselect",                  OPK_SIMD_SIMPLE,      0x52u, 0 },
    { "v128.any_true",                   OPK_SIMD_SIMPLE,      0x53u, 0 },
    { "f32x4.demote_f64x2_zero",         OPK_SIMD_SIMPLE,      0x5Eu, 0 },
    { "f64x2.promote_low_f32x4",         OPK_SIMD_SIMPLE,      0x5Fu, 0 },

    /* SIMD i8x16 aritmetik, narrow ve f32x4/f64x2 yuvarlama */
    { "i8x16.abs",                       OPK_SIMD_SIMPLE,      0x60u, 0 },
    { "i8x16.neg",                       OPK_SIMD_SIMPLE,      0x61u, 0 },
    { "i8x16.popcnt",                    OPK_SIMD_SIMPLE,      0x62u, 0 },
    { "i8x16.all_true",                  OPK_SIMD_SIMPLE,      0x63u, 0 },
    { "i8x16.bitmask",                   OPK_SIMD_SIMPLE,      0x64u, 0 },
    { "i8x16.narrow_i16x8_s",            OPK_SIMD_SIMPLE,      0x65u, 0 },
    { "i8x16.narrow_i16x8_u",            OPK_SIMD_SIMPLE,      0x66u, 0 },
    { "f32x4.ceil",                      OPK_SIMD_SIMPLE,      0x67u, 0 },
    { "f32x4.floor",                     OPK_SIMD_SIMPLE,      0x68u, 0 },
    { "f32x4.trunc",                     OPK_SIMD_SIMPLE,      0x69u, 0 },
    { "f32x4.nearest",                   OPK_SIMD_SIMPLE,      0x6Au, 0 },
    { "i8x16.shl",                       OPK_SIMD_SIMPLE,      0x6Bu, 0 },
    { "i8x16.shr_s",                     OPK_SIMD_SIMPLE,      0x6Cu, 0 },
    { "i8x16.shr_u",                     OPK_SIMD_SIMPLE,      0x6Du, 0 },
    { "i8x16.add",                       OPK_SIMD_SIMPLE,      0x6Eu, 0 },
    { "i8x16.add_sat_s",                 OPK_SIMD_SIMPLE,      0x6Fu, 0 },
    { "i8x16.add_sat_u",                 OPK_SIMD_SIMPLE,      0x70u, 0 },
    { "i8x16.sub",                       OPK_SIMD_SIMPLE,      0x71u, 0 },
    { "i8x16.sub_sat_s",                 OPK_SIMD_SIMPLE,      0x72u, 0 },
    { "i8x16.sub_sat_u",                 OPK_SIMD_SIMPLE,      0x73u, 0 },
    { "f64x2.ceil",                      OPK_SIMD_SIMPLE,      0x74u, 0 },
    { "f64x2.floor",                     OPK_SIMD_SIMPLE,      0x75u, 0 },
    { "i8x16.min_s",                     OPK_SIMD_SIMPLE,      0x76u, 0 },
    { "i8x16.min_u",                     OPK_SIMD_SIMPLE,      0x77u, 0 },
    { "i8x16.max_s",                     OPK_SIMD_SIMPLE,      0x78u, 0 },
    { "i8x16.max_u",                     OPK_SIMD_SIMPLE,      0x79u, 0 },
    { "f64x2.trunc",                     OPK_SIMD_SIMPLE,      0x7Au, 0 },
    { "i8x16.avgr_u",                    OPK_SIMD_SIMPLE,      0x7Bu, 0 },
    { "i16x8.extadd_pairwise_i8x16_s",   OPK_SIMD_SIMPLE,      0x7Cu, 0 },
    { "i16x8.extadd_pairwise_i8x16_u",   OPK_SIMD_SIMPLE,      0x7Du, 0 },
    { "i32x4.extadd_pairwise_i16x8_s",   OPK_SIMD_SIMPLE,      0x7Eu, 0 },
    { "i32x4.extadd_pairwise_i16x8_u",   OPK_SIMD_SIMPLE,      0x7Fu, 0 },

    /* SIMD i16x8 aritmetik */
    { "i16x8.abs",                       OPK_SIMD_SIMPLE,      0x80u, 0 },
    { "i16x8.neg",                       OPK_SIMD_SIMPLE,      0x81u, 0 },
    { "i16x8.q15mulr_sat_s",             OPK_SIMD_SIMPLE,      0x82u, 0 },
    { "i16x8.all_true",                  OPK_SIMD_SIMPLE,      0x83u, 0 },
    { "i16x8.bitmask",                   OPK_SIMD_SIMPLE,      0x84u, 0 },
    { "i16x8.narrow_i32x4_s",            OPK_SIMD_SIMPLE,      0x85u, 0 },
    { "i16x8.narrow_i32x4_u",            OPK_SIMD_SIMPLE,      0x86u, 0 },
    { "i16x8.extend_low_i8x16_s",        OPK_SIMD_SIMPLE,      0x87u, 0 },
    { "i16x8.extend_high_i8x16_s",       OPK_SIMD_SIMPLE,      0x88u, 0 },
    { "i16x8.extend_low_i8x16_u",        OPK_SIMD_SIMPLE,      0x89u, 0 },
    { "i16x8.extend_high_i8x16_u",       OPK_SIMD_SIMPLE,      0x8Au, 0 },
    { "i16x8.shl",                       OPK_SIMD_SIMPLE,      0x8Bu, 0 },
    { "i16x8.shr_s",                     OPK_SIMD_SIMPLE,      0x8Cu, 0 },
    { "i16x8.shr_u",                     OPK_SIMD_SIMPLE,      0x8Du, 0 },
    { "i16x8.add",                       OPK_SIMD_SIMPLE,      0x8Eu, 0 },
    { "i16x8.add_sat_s",                 OPK_SIMD_SIMPLE,      0x8Fu, 0 },
    { "i16x8.add_sat_u",                 OPK_SIMD_SIMPLE,      0x90u, 0 },
    { "i16x8.sub",                       OPK_SIMD_SIMPLE,      0x91u, 0 },
    { "i16x8.sub_sat_s",                 OPK_SIMD_SIMPLE,      0x92u, 0 },
    { "i16x8.sub_sat_u",                 OPK_SIMD_SIMPLE,      0x93u, 0 },
    { "f64x2.nearest",                   OPK_SIMD_SIMPLE,      0x94u, 0 },
    { "i16x8.mul",                       OPK_SIMD_SIMPLE,      0x95u, 0 },
    { "i16x8.min_s",                     OPK_SIMD_SIMPLE,      0x96u, 0 },
    { "i16x8.min_u",                     OPK_SIMD_SIMPLE,      0x97u, 0 },
    { "i16x8.max_s",                     OPK_SIMD_SIMPLE,      0x98u, 0 },
    { "i16x8.max_u",                     OPK_SIMD_SIMPLE,      0x99u, 0 },
    { "i16x8.avgr_u",                    OPK_SIMD_SIMPLE,      0x9Bu, 0 },
    { "i16x8.extmul_low_i8x16_s",        OPK_SIMD_SIMPLE,      0x9Cu, 0 },
    { "i16x8.extmul_high_i8x16_s",       OPK_SIMD_SIMPLE,      0x9Du, 0 },
    { "i16x8.extmul_low_i8x16_u",        OPK_SIMD_SIMPLE,      0x9Eu, 0 },
    { "i16x8.extmul_high_i8x16_u",       OPK_SIMD_SIMPLE,      0x9Fu, 0 },

    /* SIMD i32x4 aritmetik */
    { "i32x4.abs",                       OPK_SIMD_SIMPLE,      0xA0u, 0 },
    { "i32x4.neg",                       OPK_SIMD_SIMPLE,      0xA1u, 0 },
    { "i32x4.all_true",                  OPK_SIMD_SIMPLE,      0xA3u, 0 },
    { "i32x4.bitmask",                   OPK_SIMD_SIMPLE,      0xA4u, 0 },
    { "i32x4.extend_low_i16x8_s",        OPK_SIMD_SIMPLE,      0xA7u, 0 },
    { "i32x4.extend_high_i16x8_s",       OPK_SIMD_SIMPLE,      0xA8u, 0 },
    { "i32x4.extend_low_i16x8_u",        OPK_SIMD_SIMPLE,      0xA9u, 0 },
    { "i32x4.extend_high_i16x8_u",       OPK_SIMD_SIMPLE,      0xAAu, 0 },
    { "i32x4.shl",                       OPK_SIMD_SIMPLE,      0xABu, 0 },
    { "i32x4.shr_s",                     OPK_SIMD_SIMPLE,      0xACu, 0 },
    { "i32x4.shr_u",                     OPK_SIMD_SIMPLE,      0xADu, 0 },
    { "i32x4.add",                       OPK_SIMD_SIMPLE,      0xAEu, 0 },
    { "i32x4.sub",                       OPK_SIMD_SIMPLE,      0xB1u, 0 },
    { "i32x4.mul",                       OPK_SIMD_SIMPLE,      0xB5u, 0 },
    { "i32x4.min_s",                     OPK_SIMD_SIMPLE,      0xB6u, 0 },
    { "i32x4.min_u",                     OPK_SIMD_SIMPLE,      0xB7u, 0 },
    { "i32x4.max_s",                     OPK_SIMD_SIMPLE,      0xB8u, 0 },
    { "i32x4.max_u",                     OPK_SIMD_SIMPLE,      0xB9u, 0 },
    { "i32x4.dot_i16x8_s",               OPK_SIMD_SIMPLE,      0xBAu, 0 },
    { "i32x4.extmul_low_i16x8_s",        OPK_SIMD_SIMPLE,      0xBCu, 0 },
    { "i32x4.extmul_high_i16x8_s",       OPK_SIMD_SIMPLE,      0xBDu, 0 },
    { "i32x4.extmul_low_i16x8_u",        OPK_SIMD_SIMPLE,      0xBEu, 0 },
    { "i32x4.extmul_high_i16x8_u",       OPK_SIMD_SIMPLE,      0xBFu, 0 },

    /* SIMD i64x2 aritmetik ve karşılaştırma */
    { "i64x2.abs",                       OPK_SIMD_SIMPLE,      0xC0u, 0 },
    { "i64x2.neg",                       OPK_SIMD_SIMPLE,      0xC1u, 0 },
    { "i64x2.all_true",                  OPK_SIMD_SIMPLE,      0xC3u, 0 },
    { "i64x2.bitmask",                   OPK_SIMD_SIMPLE,      0xC4u, 0 },
    { "i64x2.extend_low_i32x4_s",        OPK_SIMD_SIMPLE,      0xC7u, 0 },
    { "i64x2.extend_high_i32x4_s",       OPK_SIMD_SIMPLE,      0xC8u, 0 },
    { "i64x2.extend_low_i32x4_u",        OPK_SIMD_SIMPLE,      0xC9u, 0 },
    { "i64x2.extend_high_i32x4_u",       OPK_SIMD_SIMPLE,      0xCAu, 0 },
    { "i64x2.shl",                       OPK_SIMD_SIMPLE,      0xCBu, 0 },
    { "i64x2.shr_s",                     OPK_SIMD_SIMPLE,      0xCCu, 0 },
    { "i64x2.shr_u",                     OPK_SIMD_SIMPLE,      0xCDu, 0 },
    { "i64x2.add",                       OPK_SIMD_SIMPLE,      0xCEu, 0 },
    { "i64x2.sub",                       OPK_SIMD_SIMPLE,      0xD1u, 0 },
    { "i64x2.mul",                       OPK_SIMD_SIMPLE,      0xD5u, 0 },
    { "i64x2.eq",                        OPK_SIMD_SIMPLE,      0xD6u, 0 },
    { "i64x2.ne",                        OPK_SIMD_SIMPLE,      0xD7u, 0 },
    { "i64x2.lt_s",                      OPK_SIMD_SIMPLE,      0xD8u, 0 },
    { "i64x2.gt_s",                      OPK_SIMD_SIMPLE,      0xD9u, 0 },
    { "i64x2.le_s",                      OPK_SIMD_SIMPLE,      0xDAu, 0 },
    { "i64x2.ge_s",                      OPK_SIMD_SIMPLE,      0xDBu, 0 },
    { "i64x2.extmul_low_i32x4_s",        OPK_SIMD_SIMPLE,      0xDCu, 0 },
    { "i64x2.extmul_high_i32x4_s",       OPK_SIMD_SIMPLE,      0xDDu, 0 },
    { "i64x2.extmul_low_i32x4_u",        OPK_SIMD_SIMPLE,      0xDEu, 0 },
    { "i64x2.extmul_high_i32x4_u",       OPK_SIMD_SIMPLE,      0xDFu, 0 },

    /* SIMD f32x4 ve f64x2 aritmetik ve dönüşümler */
    { "f32x4.abs",                       OPK_SIMD_SIMPLE,      0xE0u, 0 },
    { "f32x4.neg",                       OPK_SIMD_SIMPLE,      0xE1u, 0 },
    { "f32x4.sqrt",                      OPK_SIMD_SIMPLE,      0xE3u, 0 },
    { "f32x4.add",                       OPK_SIMD_SIMPLE,      0xE4u, 0 },
    { "f32x4.sub",                       OPK_SIMD_SIMPLE,      0xE5u, 0 },
    { "f32x4.mul",                       OPK_SIMD_SIMPLE,      0xE6u, 0 },
    { "f32x4.div",                       OPK_SIMD_SIMPLE,      0xE7u, 0 },
    { "f32x4.min",                       OPK_SIMD_SIMPLE,      0xE8u, 0 },
    { "f32x4.max",                       OPK_SIMD_SIMPLE,      0xE9u, 0 },
    { "f32x4.pmin",                      OPK_SIMD_SIMPLE,      0xEAu, 0 },
    { "f32x4.pmax",                      OPK_SIMD_SIMPLE,      0xEBu, 0 },
    { "f64x2.abs",                       OPK_SIMD_SIMPLE,      0xECu, 0 },
    { "f64x2.neg",                       OPK_SIMD_SIMPLE,      0xEDu, 0 },
    { "f64x2.sqrt",                      OPK_SIMD_SIMPLE,      0xEFu, 0 },
    { "f64x2.add",                       OPK_SIMD_SIMPLE,      0xF0u, 0 },
    { "f64x2.sub",                       OPK_SIMD_SIMPLE,      0xF1u, 0 },
    { "f64x2.mul",                       OPK_SIMD_SIMPLE,      0xF2u, 0 },
    { "f64x2.div",                       OPK_SIMD_SIMPLE,      0xF3u, 0 },
    { "f64x2.min",                       OPK_SIMD_SIMPLE,      0xF4u, 0 },
    { "f64x2.max",                       OPK_SIMD_SIMPLE,      0xF5u, 0 },
    { "f64x2.pmin",                      OPK_SIMD_SIMPLE,      0xF6u, 0 },
    { "f64x2.pmax",                      OPK_SIMD_SIMPLE,      0xF7u, 0 },
    { "i32x4.trunc_sat_f32x4_s",         OPK_SIMD_SIMPLE,      0xF8u, 0 },
    { "i32x4.trunc_sat_f32x4_u",         OPK_SIMD_SIMPLE,      0xF9u, 0 },
    { "f32x4.convert_i32x4_s",           OPK_SIMD_SIMPLE,      0xFAu, 0 },
    { "f32x4.convert_i32x4_u",           OPK_SIMD_SIMPLE,      0xFBu, 0 },
    { "i32x4.trunc_sat_f64x2_s_zero",    OPK_SIMD_SIMPLE,      0xFCu, 0 },
    { "i32x4.trunc_sat_f64x2_u_zero",    OPK_SIMD_SIMPLE,      0xFDu, 0 },
    { "f64x2.convert_low_i32x4_s",       OPK_SIMD_SIMPLE,      0xFEu, 0 },
    { "f64x2.convert_low_i32x4_u",       OPK_SIMD_SIMPLE,      0xFFu, 0 },
};

static const MasmOpInfo* masm_find_op(const char* name, u32 len) {
    for (usize i = 0; i < ARRAY_LEN(g_masm_ops); i++) {
        if (masm_streq(name, len, g_masm_ops[i].name)) {
            return &g_masm_ops[i];
        }
    }
    return NULL;
}

typedef struct {
    MasmSlice       labels[MASM_MAX_LABELS];
    u8              block_kinds[MASM_MAX_LABELS]; /* 0x02=block, 0x03=loop, 0x04=if */
    u32             label_depth;
    MbModule*       mod;
    MbFunc*         mf;
    const MasmFuncDecl* fd;
} MasmFuncCtx;

static b32 mp_push_label(MasmParser* p, MasmFuncCtx* ctx, MasmSlice name, u8 kind, const MasmToken* err_tok) {
    if (ctx->label_depth >= MASM_MAX_LABELS) {
        masm_error_at(&p->lx, err_tok->line, err_tok->col, "control block nesting too deep");
        return FALSE;
    }
    ctx->labels[ctx->label_depth] = name;
    ctx->block_kinds[ctx->label_depth] = kind;
    ctx->label_depth++;
    return TRUE;
}

static b32 mp_pop_label(MasmParser* p, MasmFuncCtx* ctx, const MasmToken* end_tok, MasmSlice opt_label) {
    if (ctx->label_depth == 0) {
        masm_error_at(&p->lx, end_tok->line, end_tok->col, "unexpected 'end' (block depth underflow)");
        return FALSE;
    }
    ctx->label_depth--;
    MasmSlice expected = ctx->labels[ctx->label_depth];
    if (opt_label.len > 0) {
        if (expected.len == 0 || !masm_streq_n(expected.ptr, expected.len, opt_label.ptr, opt_label.len)) {
            masm_error_at(&p->lx, end_tok->line, end_tok->col, "mismatching label on 'end'");
            return FALSE;
        }
    }
    return TRUE;
}

static b32 mp_resolve_branch_depth(MasmParser* p, const MasmFuncCtx* ctx, const MasmToken* tok, u32* out_depth) {
    if (mp_is_id(tok)) {
        for (i32 i = (i32)ctx->label_depth - 1; i >= 0; i--) {
            MasmSlice lbl = ctx->labels[i];
            if (lbl.len > 0 && masm_streq_n(lbl.ptr, lbl.len, tok->ptr, tok->len)) {
                *out_depth = (ctx->label_depth - 1u) - (u32)i;
                return TRUE;
            }
        }
        masm_error_at(&p->lx, tok->line, tok->col, "undefined label %.*s", (int)tok->len, tok->ptr);
        return FALSE;
    }
    u32 d = 0;
    if (!masm_parse_u32(tok->ptr, tok->len, &d) || d > ctx->label_depth) {
        masm_error_at(&p->lx, tok->line, tok->col, "branch depth out of range");
        return FALSE;
    }
    *out_depth = d;
    return TRUE;
}

static b32 mp_is_label_idx_tok(const MasmToken* t) {
    u32 dummy = 0;
    return t->kind == TOK_ATOM && (mp_is_id(t) || masm_parse_u32(t->ptr, t->len, &dummy));
}

static b32 mp_parse_blocktype(MasmParser* p, MasmFuncCtx* ctx, const MasmToken* op_tok,
                              b32* out_is_type_idx, u8* out_bt, u32* out_type_idx) {
    b32 has_type_use = FALSE;
    MasmToken type_tok;
    masm_memzero(&type_tok, sizeof(type_tok));
    if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "type")) {
        mp_next(p); /* '(' */
        mp_next(p); /* 'type' */
        has_type_use = TRUE;
        type_tok = *mp_next(p);
        if (!mp_expect_rparen(p)) return FALSE;
    }

    u8 params[MB_MAX_PARAMS];
    u32 np = 0;
    u8 results[MB_MAX_RESULTS];
    u32 nr = 0;
    b32 has_sig = FALSE;
    if (mp_peek(p)->kind == TOK_LPAREN &&
        (mp_is_atom(mp_peek_n(p, 1), "param") || mp_is_atom(mp_peek_n(p, 1), "result"))) {
        has_sig = TRUE;
        if (!mp_parse_func_sig(p, params, NULL, &np, results, &nr, FALSE)) return FALSE;
    }

    u32 tidx = 0;
    if (has_type_use) {
        if (!mp_resolve_type(p, &type_tok, &tidx)) return FALSE;
        const MbFuncType* t = &ctx->mod->types[tidx];
        if (has_sig) {
            if (t->param_count != np || t->result_count != nr) {
                masm_error_at(&p->lx, op_tok->line, op_tok->col, "block signature does not match (type)");
                return FALSE;
            }
            for (u32 i = 0; i < np; i++) {
                if (t->params[i] != params[i]) {
                    masm_error_at(&p->lx, op_tok->line, op_tok->col, "block param mismatch with (type)");
                    return FALSE;
                }
            }
            for (u32 i = 0; i < nr; i++) {
                if (t->results[i] != results[i]) {
                    masm_error_at(&p->lx, op_tok->line, op_tok->col, "block result mismatch with (type)");
                    return FALSE;
                }
            }
        } else {
            np = t->param_count;
            for (u32 i = 0; i < np; i++) params[i] = t->params[i];
            nr = t->result_count;
            for (u32 i = 0; i < nr; i++) results[i] = t->results[i];
        }
    }

    if (np == 0 && nr == 0) {
        *out_is_type_idx = FALSE;
        *out_bt = MB_BLOCK_VOID;
        *out_type_idx = 0;
        return TRUE;
    }
    if (np == 0 && nr == 1) {
        *out_is_type_idx = FALSE;
        *out_bt = results[0];
        *out_type_idx = 0;
        return TRUE;
    }
    if (!has_type_use) {
        tidx = mb_type(ctx->mod, params, np, results, nr);
    }
    *out_is_type_idx = TRUE;
    *out_bt = 0;
    *out_type_idx = tidx;
    return TRUE;
}

static b32 mp_parse_memarg(MasmParser* p, u32 def_align_log2, u32* out_align_log2, u32* out_offset) {
    u32 offset = 0;
    u32 align_log2 = def_align_log2;

    if (mp_peek(p)->kind == TOK_ATOM && masm_starts_with(mp_peek(p)->ptr, mp_peek(p)->len, "offset=")) {
        const MasmToken* t = mp_next(p);
        if (!masm_parse_u32(t->ptr + 7, t->len - 7, &offset)) {
            masm_error_at(&p->lx, t->line, t->col, "invalid memory offset");
            return FALSE;
        }
    }
    if (mp_peek(p)->kind == TOK_ATOM && masm_starts_with(mp_peek(p)->ptr, mp_peek(p)->len, "align=")) {
        const MasmToken* t = mp_next(p);
        u32 align_bytes = 0;
        if (!masm_parse_u32(t->ptr + 6, t->len - 6, &align_bytes) || !IS_POW2(align_bytes)) {
            masm_error_at(&p->lx, t->line, t->col, "alignment must be a power of two");
            return FALSE;
        }
        u32 lg = 0;
        while ((1u << lg) < align_bytes) lg++;
        if (lg > def_align_log2) {
            masm_error_at(&p->lx, t->line, t->col, "alignment must not be larger than natural alignment");
            return FALSE;
        }
        align_log2 = lg;
    }
    if (mp_peek(p)->kind == TOK_ATOM && masm_starts_with(mp_peek(p)->ptr, mp_peek(p)->len, "offset=")) {
        const MasmToken* t = mp_next(p);
        masm_error_at(&p->lx, t->line, t->col, "offset= must precede align=");
        return FALSE;
    }

    *out_align_log2 = align_log2;
    *out_offset = offset;
    return TRUE;
}

static b32 mp_compile_instr(MasmParser* p, MasmFuncCtx* ctx);

static b32 mp_compile_operands(MasmParser* p, MasmFuncCtx* ctx) {
    while (mp_peek(p)->kind == TOK_LPAREN) {
        if (!mp_compile_instr(p, ctx)) return FALSE;
    }
    return TRUE;
}

static b32 mp_emit_single_op(MasmParser* p, MasmFuncCtx* ctx, const MasmToken* op_tok, b32 is_folded) {
    MbFunc* mf = ctx->mf;

    /* 1. Sabitler */
    if (mp_is_atom(op_tok, "i32.const")) {
        const MasmToken* val_tok = mp_next(p);
        i32 v = 0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_i32(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid i32.const literal");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_i32_const(mf, v);
        return TRUE;
    }
    if (mp_is_atom(op_tok, "i64.const")) {
        const MasmToken* val_tok = mp_next(p);
        i64 v = 0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_i64(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid i64.const literal");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_i64_const(mf, v);
        return TRUE;
    }
    if (mp_is_atom(op_tok, "f32.const")) {
        const MasmToken* val_tok = mp_next(p);
        f32 v = 0.0f;
        if (val_tok->kind != TOK_ATOM || !masm_parse_f32(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid f32.const literal");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_f32_const(mf, v);
        return TRUE;
    }
    if (mp_is_atom(op_tok, "f64.const")) {
        const MasmToken* val_tok = mp_next(p);
        f64 v = 0.0;
        if (val_tok->kind != TOK_ATOM || !masm_parse_f64(val_tok->ptr, val_tok->len, &v)) {
            masm_error_at(&p->lx, val_tok->line, val_tok->col, "invalid f64.const literal");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_f64_const(mf, v);
        return TRUE;
    }

    /* 2. SIMD v128.const ve i8x16.shuffle */
    if (mp_is_atom(op_tok, "v128.const")) {
        const MasmToken* shape = mp_next(p);
        if (mp_is_atom(shape, "f32x4")) {
            f32 lanes[4];
            for (u32 i = 0; i < 4; i++) {
                const MasmToken* lt = mp_next(p);
                if (lt->kind != TOK_ATOM || !masm_parse_f32(lt->ptr, lt->len, &lanes[i])) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid f32x4 lane constant");
                    return FALSE;
                }
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_f32x4_const(mf, lanes[0], lanes[1], lanes[2], lanes[3]);
            return TRUE;
        } else if (mp_is_atom(shape, "i32x4")) {
            i32 lanes[4];
            for (u32 i = 0; i < 4; i++) {
                const MasmToken* lt = mp_next(p);
                if (lt->kind != TOK_ATOM || !masm_parse_i32(lt->ptr, lt->len, &lanes[i])) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid i32x4 lane constant");
                    return FALSE;
                }
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_i32x4_const(mf, lanes[0], lanes[1], lanes[2], lanes[3]);
            return TRUE;
        } else if (mp_is_atom(shape, "i8x16")) {
            u8 bytes[16];
            for (u32 i = 0; i < 16; i++) {
                const MasmToken* lt = mp_next(p);
                i32 v = 0;
                if (lt->kind != TOK_ATOM || !masm_parse_i32(lt->ptr, lt->len, &v) || v < -128 || v > 255) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid i8x16 lane constant");
                    return FALSE;
                }
                bytes[i] = (u8)(v & 0xFF);
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_v128_const(mf, bytes);
            return TRUE;
        } else if (mp_is_atom(shape, "i16x8")) {
            u8 bytes[16];
            for (u32 i = 0; i < 8; i++) {
                const MasmToken* lt = mp_next(p);
                i32 v = 0;
                if (lt->kind != TOK_ATOM || !masm_parse_i32(lt->ptr, lt->len, &v) || v < -32768 || v > 65535) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid i16x8 lane constant");
                    return FALSE;
                }
                u16 u = (u16)(v & 0xFFFF);
                bytes[i * 2 + 0] = (u8)(u & 0xFFu);
                bytes[i * 2 + 1] = (u8)((u >> 8) & 0xFFu);
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_v128_const(mf, bytes);
            return TRUE;
        } else if (mp_is_atom(shape, "i64x2")) {
            u8 bytes[16];
            for (u32 i = 0; i < 2; i++) {
                const MasmToken* lt = mp_next(p);
                i64 v = 0;
                if (lt->kind != TOK_ATOM || !masm_parse_i64(lt->ptr, lt->len, &v)) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid i64x2 lane constant");
                    return FALSE;
                }
                u64 u = (u64)v;
                for (u32 b = 0; b < 8; b++) bytes[i * 8 + b] = (u8)((u >> (b * 8)) & 0xFFu);
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_v128_const(mf, bytes);
            return TRUE;
        } else if (mp_is_atom(shape, "f64x2")) {
            u8 bytes[16];
            for (u32 i = 0; i < 2; i++) {
                const MasmToken* lt = mp_next(p);
                f64 v = 0.0;
                if (lt->kind != TOK_ATOM || !masm_parse_f64(lt->ptr, lt->len, &v)) {
                    masm_error_at(&p->lx, lt->line, lt->col, "invalid f64x2 lane constant");
                    return FALSE;
                }
                union { f64 f; u64 u; } conv;
                conv.f = v;
                for (u32 b = 0; b < 8; b++) bytes[i * 8 + b] = (u8)((conv.u >> (b * 8)) & 0xFFu);
            }
            if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
            mb_v128_const(mf, bytes);
            return TRUE;
        } else {
            masm_error_at(&p->lx, shape->line, shape->col, "unsupported v128.const shape");
            return FALSE;
        }
    }

    if (mp_is_atom(op_tok, "i8x16.shuffle")) {
        u8 lanes[16];
        for (u32 i = 0; i < 16; i++) {
            const MasmToken* lt = mp_next(p);
            u32 lv = 0;
            if (lt->kind != TOK_ATOM || !masm_parse_u32(lt->ptr, lt->len, &lv) || lv >= 32u) {
                masm_error_at(&p->lx, lt->line, lt->col, "shuffle lane index must be in 0..31");
                return FALSE;
            }
            lanes[i] = (u8)lv;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_i8x16_shuffle(mf, lanes);
        return TRUE;
    }

    /* 3. Değişken erişimi (local / global) */
    if (mp_is_atom(op_tok, "local.get") || mp_is_atom(op_tok, "local.set") || mp_is_atom(op_tok, "local.tee")) {
        const MasmToken* id_tok = mp_next(p);
        u32 idx = 0;
        if (id_tok->kind != TOK_ATOM || !mp_resolve_local(p, ctx->fd, id_tok, &idx)) return FALSE;
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        if (mp_is_atom(op_tok, "local.get")) mb_local_get(mf, idx);
        else if (mp_is_atom(op_tok, "local.set")) mb_local_set(mf, idx);
        else mb_local_tee(mf, idx);
        return TRUE;
    }

    if (mp_is_atom(op_tok, "global.get") || mp_is_atom(op_tok, "global.set")) {
        const MasmToken* id_tok = mp_next(p);
        u32 idx = 0;
        if (id_tok->kind != TOK_ATOM || !mp_resolve_global(p, id_tok, &idx)) return FALSE;
        if (mp_is_atom(op_tok, "global.set") && !p->globals[idx].mutable_) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "can't global.set on immutable global");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        if (mp_is_atom(op_tok, "global.get")) mb_global_get(mf, idx);
        else mb_global_set(mf, idx);
        return TRUE;
    }

    /* 4. Dallanma ve Çağrı */
    if (mp_is_atom(op_tok, "br") || mp_is_atom(op_tok, "br_if")) {
        const MasmToken* lbl_tok = mp_next(p);
        u32 depth = 0;
        if (lbl_tok->kind != TOK_ATOM || !mp_resolve_branch_depth(p, ctx, lbl_tok, &depth)) return FALSE;
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        if (mp_is_atom(op_tok, "br")) mb_br(mf, depth);
        else mb_br_if(mf, depth);
        return TRUE;
    }

    if (mp_is_atom(op_tok, "br_table")) {
        u32 depths[128];
        u32 total = 0;
        while (mp_is_label_idx_tok(mp_peek(p))) {
            const MasmToken* lbl_tok = mp_next(p);
            if (total >= ARRAY_LEN(depths)) {
                masm_error_at(&p->lx, lbl_tok->line, lbl_tok->col, "too many labels in br_table");
                return FALSE;
            }
            if (!mp_resolve_branch_depth(p, ctx, lbl_tok, &depths[total])) return FALSE;
            total++;
        }
        if (total == 0) {
            const MasmToken* err_t = mp_peek(p);
            masm_error_at(&p->lx, err_t->line, err_t->col, "br_table requires at least one default label");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_br_table(mf, depths, total - 1u, depths[total - 1u]);
        return TRUE;
    }

    if (mp_is_atom(op_tok, "call")) {
        const MasmToken* fn_tok = mp_next(p);
        u32 fn_idx = 0;
        if (fn_tok->kind != TOK_ATOM || !mp_resolve_func(p, fn_tok, &fn_idx)) return FALSE;
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_call(mf, fn_idx);
        return TRUE;
    }

    if (mp_is_atom(op_tok, "call_indirect")) {
        if (!p->has_table) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "call_indirect requires a table");
            return FALSE;
        }
        b32 has_type_use = FALSE;
        MasmToken type_tok;
        masm_memzero(&type_tok, sizeof(type_tok));
        if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "type")) {
            mp_next(p); /* '(' */
            mp_next(p); /* 'type' */
            has_type_use = TRUE;
            type_tok = *mp_next(p);
            if (!mp_expect_rparen(p)) return FALSE;
        }
        u8 params[MB_MAX_PARAMS];
        u32 np = 0;
        u8 results[MB_MAX_RESULTS];
        u32 nr = 0;
        b32 has_sig = FALSE;
        if (mp_peek(p)->kind == TOK_LPAREN &&
            (mp_is_atom(mp_peek_n(p, 1), "param") || mp_is_atom(mp_peek_n(p, 1), "result"))) {
            has_sig = TRUE;
            if (!mp_parse_func_sig(p, params, NULL, &np, results, &nr, FALSE)) return FALSE;
        }
        u32 type_idx = 0;
        if (has_type_use) {
            if (!mp_resolve_type(p, &type_tok, &type_idx)) return FALSE;
            if (has_sig) {
                const MbFuncType* t = &ctx->mod->types[type_idx];
                if (t->param_count != np || t->result_count != nr) {
                    masm_error_at(&p->lx, op_tok->line, op_tok->col, "type mismatch in call_indirect");
                    return FALSE;
                }
                for (u32 i = 0; i < np; i++) {
                    if (t->params[i] != params[i]) {
                        masm_error_at(&p->lx, op_tok->line, op_tok->col, "type mismatch in call_indirect");
                        return FALSE;
                    }
                }
                for (u32 i = 0; i < nr; i++) {
                    if (t->results[i] != results[i]) {
                        masm_error_at(&p->lx, op_tok->line, op_tok->col, "type mismatch in call_indirect");
                        return FALSE;
                    }
                }
            }
        } else {
            type_idx = mb_type(ctx->mod, params, np, results, nr);
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_call_indirect(mf, type_idx);
        return TRUE;
    }

    /* 5. Kontrol Blokları: block, loop, if, else, end */
    if (mp_is_atom(op_tok, "block") || mp_is_atom(op_tok, "loop")) {
        u8 kind = mp_is_atom(op_tok, "block") ? 0x02u : 0x03u;
        MasmSlice lbl = { NULL, 0 };
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* lt = mp_next(p);
            lbl.ptr = lt->ptr;
            lbl.len = lt->len;
        }
        b32 is_type_idx = FALSE;
        u8 bt = MB_BLOCK_VOID;
        u32 bt_tidx = 0;
        if (!mp_parse_blocktype(p, ctx, op_tok, &is_type_idx, &bt, &bt_tidx)) return FALSE;
        if (!mp_push_label(p, ctx, lbl, kind, op_tok)) return FALSE;
        if (kind == 0x02u) {
            if (is_type_idx) mb_block_type(mf, bt_tidx);
            else mb_block(mf, bt);
        } else {
            if (is_type_idx) mb_loop_type(mf, bt_tidx);
            else mb_loop(mf, bt);
        }

        if (is_folded) {
            while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                if (!mp_compile_instr(p, ctx)) return FALSE;
            }
            MasmSlice empty_lbl = { NULL, 0 };
            if (!mp_pop_label(p, ctx, op_tok, empty_lbl)) return FALSE;
            mb_end(mf);
        }
        return TRUE;
    }

    if (mp_is_atom(op_tok, "if")) {
        MasmSlice lbl = { NULL, 0 };
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* lt = mp_next(p);
            lbl.ptr = lt->ptr;
            lbl.len = lt->len;
        }
        b32 is_type_idx = FALSE;
        u8 bt = MB_BLOCK_VOID;
        u32 bt_tidx = 0;
        if (!mp_parse_blocktype(p, ctx, op_tok, &is_type_idx, &bt, &bt_tidx)) return FALSE;

        if (is_folded) {
            /* Katlanmış if: (if $l (result T) <koşul>* (then ...) [(else ...)]) */
            while (mp_peek(p)->kind == TOK_LPAREN && !mp_is_atom(mp_peek_n(p, 1), "then")) {
                if (!mp_compile_instr(p, ctx)) return FALSE;
            }
            if (mp_peek(p)->kind != TOK_LPAREN || !mp_is_atom(mp_peek_n(p, 1), "then")) {
                masm_error_at(&p->lx, mp_peek(p)->line, mp_peek(p)->col, "expected (then ...) in folded if");
                return FALSE;
            }
            if (!mp_push_label(p, ctx, lbl, 0x04u, op_tok)) return FALSE;
            if (is_type_idx) mb_if_type(mf, bt_tidx);
            else mb_if(mf, bt);

            mp_next(p); /* '(' */
            mp_next(p); /* 'then' */
            while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                if (!mp_compile_instr(p, ctx)) return FALSE;
            }
            if (!mp_expect_rparen(p)) return FALSE;

            if (mp_peek(p)->kind == TOK_LPAREN && mp_is_atom(mp_peek_n(p, 1), "else")) {
                mp_next(p); /* '(' */
                mp_next(p); /* 'else' */
                mb_else(mf);
                while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
                    if (!mp_compile_instr(p, ctx)) return FALSE;
                }
                if (!mp_expect_rparen(p)) return FALSE;
            }

            MasmSlice empty_lbl = { NULL, 0 };
            if (!mp_pop_label(p, ctx, op_tok, empty_lbl)) return FALSE;
            mb_end(mf);
        } else {
            if (!mp_push_label(p, ctx, lbl, 0x04u, op_tok)) return FALSE;
            if (is_type_idx) mb_if_type(mf, bt_tidx);
            else mb_if(mf, bt);
        }
        return TRUE;
    }

    if (mp_is_atom(op_tok, "else")) {
        if (is_folded) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "unexpected folded 'else'");
            return FALSE;
        }
        if (ctx->label_depth == 0 || ctx->block_kinds[ctx->label_depth - 1] != 0x04u) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "'else' outside of 'if' block");
            return FALSE;
        }
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* lt = mp_next(p);
            MasmSlice expected = ctx->labels[ctx->label_depth - 1];
            if (expected.len == 0 || !masm_streq_n(expected.ptr, expected.len, lt->ptr, lt->len)) {
                masm_error_at(&p->lx, lt->line, lt->col, "mismatching label on 'else'");
                return FALSE;
            }
        }
        mb_else(mf);
        return TRUE;
    }

    if (mp_is_atom(op_tok, "end")) {
        if (is_folded) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "unexpected '(end)'");
            return FALSE;
        }
        MasmSlice lbl = { NULL, 0 };
        if (mp_is_id(mp_peek(p))) {
            const MasmToken* lt = mp_next(p);
            lbl.ptr = lt->ptr;
            lbl.len = lt->len;
        }
        if (!mp_pop_label(p, ctx, op_tok, lbl)) return FALSE;
        mb_end(mf);
        return TRUE;
    }

    /* 6. Tablo tabanlı standart ve SIMD komutlar */
    const MasmOpInfo* info = masm_find_op(op_tok->ptr, op_tok->len);
    if (!info) {
        masm_error_at(&p->lx, op_tok->line, op_tok->col, "unknown instruction '%.*s'",
                      (int)op_tok->len, op_tok->ptr);
        return FALSE;
    }

    switch (info->kind) {
    case OPK_SIMPLE:
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_op(mf, (u8)info->code);
        return TRUE;

    case OPK_SIMD_SIMPLE:
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_simd_op(mf, (u32)info->code);
        return TRUE;

    case OPK_SAT_TRUNC:
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_op(mf, 0xFCu);
        mb_uleb(&mf->code, (u64)info->code);
        return TRUE;

    case OPK_MEM: {
        if (!p->has_memory) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "memory instruction requires a memory");
            return FALSE;
        }
        u32 align_lg = 0, offset = 0;
        if (!mp_parse_memarg(p, info->def_align, &align_lg, &offset)) return FALSE;
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_memop(mf, (u8)info->code, align_lg, offset);
        return TRUE;
    }

    case OPK_MEM_ZERO: {
        if (!p->has_memory) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "memory instruction requires a memory");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_op(mf, (u8)info->code);
        mb_u8(&mf->code, 0x00u);
        return TRUE;
    }

    case OPK_MEM_BULK: {
        if (!p->has_memory) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "memory instruction requires a memory");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        if (info->code == 0x0Au) mb_memory_copy(mf);
        else mb_memory_fill(mf);
        return TRUE;
    }

    case OPK_SIMD_MEM: {
        if (!p->has_memory) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "memory instruction requires a memory");
            return FALSE;
        }
        u32 align_lg = 0, offset = 0;
        if (!mp_parse_memarg(p, info->def_align, &align_lg, &offset)) return FALSE;
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_simd_op(mf, (u32)info->code);
        mb_uleb(&mf->code, (u64)align_lg);
        mb_uleb(&mf->code, (u64)offset);
        return TRUE;
    }

    case OPK_SIMD_MEM_LANE_2:
    case OPK_SIMD_MEM_LANE_4:
    case OPK_SIMD_MEM_LANE_8:
    case OPK_SIMD_MEM_LANE_16: {
        if (!p->has_memory) {
            masm_error_at(&p->lx, op_tok->line, op_tok->col, "memory instruction requires a memory");
            return FALSE;
        }
        u32 max_lanes = (info->kind == OPK_SIMD_MEM_LANE_2) ? 2u :
                        (info->kind == OPK_SIMD_MEM_LANE_4) ? 4u :
                        (info->kind == OPK_SIMD_MEM_LANE_8) ? 8u : 16u;
        u32 align_lg = 0, offset = 0;
        if (!mp_parse_memarg(p, info->def_align, &align_lg, &offset)) return FALSE;
        const MasmToken* lt = mp_next(p);
        u32 lane = 0;
        if (lt->kind != TOK_ATOM || !masm_parse_u32(lt->ptr, lt->len, &lane) || lane >= max_lanes) {
            masm_error_at(&p->lx, lt->line, lt->col, "SIMD lane index out of range");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_simd_mem_lane(mf, (u32)info->code, align_lg, offset, (u8)lane);
        return TRUE;
    }

    case OPK_LANE_2:
    case OPK_LANE_4:
    case OPK_LANE_8:
    case OPK_LANE_16: {
        u32 max_lanes = (info->kind == OPK_LANE_2) ? 2u :
                        (info->kind == OPK_LANE_4) ? 4u :
                        (info->kind == OPK_LANE_8) ? 8u : 16u;
        const MasmToken* lt = mp_next(p);
        u32 lane = 0;
        if (lt->kind != TOK_ATOM || !masm_parse_u32(lt->ptr, lt->len, &lane) || lane >= max_lanes) {
            masm_error_at(&p->lx, lt->line, lt->col, "SIMD lane index out of range");
            return FALSE;
        }
        if (is_folded && !mp_compile_operands(p, ctx)) return FALSE;
        mb_simd_op(mf, (u32)info->code);
        mb_u8(&mf->code, (u8)lane);
        return TRUE;
    }
    }

    return FALSE;
}

static b32 mp_compile_instr(MasmParser* p, MasmFuncCtx* ctx) {
    const MasmToken* t = mp_peek(p);
    if (t->kind == TOK_LPAREN) {
        mp_next(p); /* '(' */
        const MasmToken* op = mp_next(p);
        if (op->kind != TOK_ATOM) {
            masm_error_at(&p->lx, op->line, op->col, "expected instruction name after '('");
            return FALSE;
        }
        if (!mp_emit_single_op(p, ctx, op, TRUE)) return FALSE;
        if (!mp_expect_rparen(p)) return FALSE;
        return TRUE;
    } else if (t->kind == TOK_ATOM) {
        const MasmToken* op = mp_next(p);
        return mp_emit_single_op(p, ctx, op, FALSE);
    } else {
        masm_error_at(&p->lx, t->line, t->col, "unexpected token in function body");
        return FALSE;
    }
}

/* ============================================================================
 * 7. Ana Birleştirici (masm_assemble)
 * ============================================================================ */

MasmResult masm_assemble(Arena* arena, const char* src, u32 src_len) {
    ASSERT(arena != NULL);
    MasmResult res;
    masm_memzero(&res, sizeof(res));

    usize mark = arena_mark(arena);
    MasmParser* p = ARENA_PUSH_STRUCT(arena, MasmParser);
    p->arena = arena;
    p->lx.src = src;
    p->lx.src_len = src_len;
    p->lx.toks = ARENA_PUSH_ARRAY(arena, MasmToken, MASM_MAX_TOKENS);

    if (!masm_tokenize(&p->lx)) {
        goto fail;
    }

    /* En dışta (module ...) bekliyoruz */
    if (!mp_expect_lparen(p)) goto fail;
    const MasmToken* mod_kw = mp_next(p);
    if (!mp_is_atom(mod_kw, "module")) {
        masm_error_at(&p->lx, mod_kw->line, mod_kw->col, "expected 'module'");
        goto fail;
    }
    if (mp_is_id(mp_peek(p))) {
        mp_next(p); /* isteğe bağlı modül adı ($m) */
    }

    /* Pass 1: Tüm üst seviye bildirimleri ayrıştır */
    while (mp_peek(p)->kind != TOK_RPAREN && mp_peek(p)->kind != TOK_EOF) {
        if (!mp_expect_lparen(p)) goto fail;
        const MasmToken* kw = mp_next(p);
        if (mp_is_atom(kw, "type")) {
            if (!mp_parse_type_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "import")) {
            if (!mp_parse_import_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "func")) {
            if (!mp_parse_func_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "memory")) {
            if (!mp_parse_memory_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "table")) {
            if (!mp_parse_table_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "global")) {
            if (!mp_parse_global_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "export")) {
            if (!mp_parse_export_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "elem")) {
            if (!mp_parse_elem_decl(p, kw)) goto fail;
        } else if (mp_is_atom(kw, "data")) {
            if (!mp_parse_data_decl(p, kw)) goto fail;
        } else {
            masm_error_at(&p->lx, kw->line, kw->col, "unexpected module field '%.*s'",
                          (int)kw->len, kw->ptr);
            goto fail;
        }
    }

    if (!mp_expect_rparen(p)) goto fail;
    if (mp_peek(p)->kind != TOK_EOF) {
        const MasmToken* extra = mp_peek(p);
        masm_error_at(&p->lx, extra->line, extra->col, "unexpected token after module end");
        goto fail;
    }

    /* Pass 2: MbModule oluştur ve tüm bölümleri sırayla doldur */
    MbModule* mod = ARENA_PUSH_STRUCT(arena, MbModule);
    u32 work_cap = 65536;
    u8* work_mem = ARENA_PUSH_ARRAY(arena, u8, work_cap);
    mb_module_init(mod, work_mem, work_cap);

    /* 2.1 Açıkça tanımlanmış (type ...) bildirimlerini koru */
    for (u32 i = 0; i < p->type_count; i++) {
        MasmTypeDecl* td = &p->types[i];
        mb_type_add(mod, td->params, td->param_count, td->results, td->result_count);
    }

    /* 2.2 Fonksiyon imzalarını çözümle / ekle */
    for (u32 i = 0; i < p->func_count; i++) {
        MasmFuncDecl* fd = &p->funcs[i];
        if (fd->has_type_use) {
            u32 tidx = 0;
            if (!mp_resolve_type(p, &fd->type_use_tok, &tidx)) goto fail;
            fd->type_idx = tidx;
            const MasmTypeDecl* td = &p->types[tidx];
            if (fd->has_explicit_sig) {
                if (td->param_count != fd->param_count || td->result_count != fd->result_count) {
                    masm_error_at(&p->lx, fd->line, fd->col, "function signature does not match (type)");
                    goto fail;
                }
                for (u32 j = 0; j < fd->param_count; j++) {
                    if (td->params[j] != fd->params[j]) {
                        masm_error_at(&p->lx, fd->line, fd->col, "function parameter type mismatch with (type)");
                        goto fail;
                    }
                }
                for (u32 j = 0; j < fd->result_count; j++) {
                    if (td->results[j] != fd->results[j]) {
                        masm_error_at(&p->lx, fd->line, fd->col, "function result type mismatch with (type)");
                        goto fail;
                    }
                }
            } else {
                fd->param_count = td->param_count;
                for (u32 j = 0; j < td->param_count; j++) fd->params[j] = td->params[j];
                fd->result_count = td->result_count;
                for (u32 j = 0; j < td->result_count; j++) fd->results[j] = td->results[j];
            }
        } else {
            fd->type_idx = mb_type(mod, fd->params, fd->param_count, fd->results, fd->result_count);
        }
    }

    /* 2.3 Import'ları kaynak dosyadaki bildirim sırasıyla ekle */
    for (u32 i = 0; i < p->import_order_count; i++) {
        const MasmImportEntry* ie = &p->import_order[i];
        if (ie->kind == MB_EXT_FUNC) {
            const MasmFuncDecl* fd = &p->funcs[ie->index];
            mb_import_func(mod, fd->imp_mod.ptr, fd->imp_field.ptr, fd->type_idx);
        } else if (ie->kind == MB_EXT_TABLE) {
            mb_import_table_limits(mod, p->table_imp_mod.ptr, p->table_imp_field.ptr,
                                   p->table_min, p->table_max, p->table_has_max);
        } else if (ie->kind == MB_EXT_MEMORY) {
            mb_import_memory(mod, p->mem_imp_mod.ptr, p->mem_imp_field.ptr,
                             p->mem_min, p->mem_max, p->mem_shared);
        } else if (ie->kind == MB_EXT_GLOBAL) {
            const MasmGlobalDecl* gd = &p->globals[ie->index];
            mb_import_global(mod, gd->imp_mod.ptr, gd->imp_field.ptr, gd->valtype, gd->mutable_);
        }
    }

    /* 2.4 Dahili Table, Memory, Global tanımları */
    if (p->has_table && !p->table_is_import) {
        mb_table(mod, p->table_min, p->table_max, p->table_has_max);
    }
    if (p->has_memory && !p->mem_is_import) {
        mb_memory(mod, p->mem_min, p->mem_max, p->mem_shared);
    }
    for (u32 i = 0; i < p->global_count; i++) {
        MasmGlobalDecl* gd = &p->globals[i];
        if (!gd->is_import) {
            if (gd->valtype == MB_VAL_I32) mb_global_i32(mod, gd->mutable_, (i32)gd->init_i64);
            else if (gd->valtype == MB_VAL_I64) mb_global_i64(mod, gd->mutable_, gd->init_i64);
            else if (gd->valtype == MB_VAL_F32) mb_global_f32(mod, gd->mutable_, gd->init_f32);
            else if (gd->valtype == MB_VAL_F64) mb_global_f64(mod, gd->mutable_, gd->init_f64);
            else if (gd->valtype == MB_VAL_V128) mb_global_v128(mod, gd->mutable_, gd->init_v128);
        }
    }

    /* 2.5 Fonksiyon gövdelerini derle */
    for (u32 i = 0; i < p->func_count; i++) {
        MasmFuncDecl* fd = &p->funcs[i];
        if (fd->is_import) continue;

        MbFunc* mf = mb_func_begin(mod, fd->type_idx);
        for (u32 l = 0; l < fd->local_count; l++) {
            mb_local(mf, fd->locals[l], 1);
        }

        MasmFuncCtx ctx;
        masm_memzero(&ctx, sizeof(ctx));
        ctx.mod = mod;
        ctx.mf = mf;
        ctx.fd = fd;

        p->lx.pos = fd->body_start_tok;
        while (p->lx.pos < fd->body_end_tok) {
            if (!mp_compile_instr(p, &ctx)) goto fail;
        }
        if (ctx.label_depth != 0) {
            const MasmToken* end_tok = &p->lx.toks[fd->body_end_tok];
            masm_error_at(&p->lx, end_tok->line, end_tok->col, "unclosed control block at end of function");
            goto fail;
        }
        mb_func_end(mf);
    }

    /* 2.6 Export'ları çözümle ve ekle */
    for (u32 i = 0; i < p->export_count; i++) {
        MasmExportDecl* ex = &p->exports[i];
        if (!ex->resolved) {
            if (ex->kind == MB_EXT_FUNC) {
                if (!mp_resolve_func(p, &ex->target_tok, &ex->index)) goto fail;
            } else if (ex->kind == MB_EXT_GLOBAL) {
                if (!mp_resolve_global(p, &ex->target_tok, &ex->index)) goto fail;
            } else if (ex->kind == MB_EXT_MEMORY) {
                if (!p->has_memory) {
                    masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "unknown memory");
                    goto fail;
                }
                if (mp_is_id(&ex->target_tok)) {
                    if (p->mem_name.len == 0 ||
                        !masm_streq_n(p->mem_name.ptr, p->mem_name.len, ex->target_tok.ptr, ex->target_tok.len)) {
                        masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "undefined memory identifier");
                        goto fail;
                    }
                } else {
                    u32 midx = 0;
                    if (!masm_parse_u32(ex->target_tok.ptr, ex->target_tok.len, &midx) || midx != 0) {
                        masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "memory index out of range");
                        goto fail;
                    }
                }
                ex->index = 0;
            } else if (ex->kind == MB_EXT_TABLE) {
                if (!p->has_table) {
                    masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "unknown table");
                    goto fail;
                }
                if (mp_is_id(&ex->target_tok)) {
                    if (p->table_name.len == 0 ||
                        !masm_streq_n(p->table_name.ptr, p->table_name.len, ex->target_tok.ptr, ex->target_tok.len)) {
                        masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "undefined table identifier");
                        goto fail;
                    }
                } else {
                    u32 tidx = 0;
                    if (!masm_parse_u32(ex->target_tok.ptr, ex->target_tok.len, &tidx) || tidx != 0) {
                        masm_error_at(&p->lx, ex->target_tok.line, ex->target_tok.col, "table index out of range");
                        goto fail;
                    }
                }
                ex->index = 0;
            }
            ex->resolved = TRUE;
        }
        /* Mükerrer export adı kontrolü */
        for (u32 j = 0; j < i; j++) {
            if (masm_streq_n(p->exports[j].name.ptr, p->exports[j].name.len, ex->name.ptr, ex->name.len)) {
                masm_error_at(&p->lx, 1, 1, "duplicate export name '%s'", ex->name.ptr);
                goto fail;
            }
        }
        mb_export(mod, ex->name.ptr, ex->kind, ex->index);
    }

    /* 2.7 Element segmentlerini çözümle ve ekle */
    for (u32 i = 0; i < p->elem_count; i++) {
        MasmElemDecl* el = &p->elems[i];
        if (!p->has_table) {
            masm_error_at(&p->lx, 1, 1, "elem segment requires a table");
            goto fail;
        }
        u32 indices[MB_MAX_ELEM_FUNCS];
        for (u32 j = 0; j < el->count; j++) {
            if (!mp_resolve_func(p, &el->func_toks[j], &indices[j])) goto fail;
        }
        mb_elem(mod, el->offset, indices, el->count);
    }

    /* 2.8 Data segmentlerini ekle */
    for (u32 i = 0; i < p->data_count; i++) {
        if (!p->has_memory) {
            masm_error_at(&p->lx, 1, 1, "data segment requires a memory");
            goto fail;
        }
        MasmDataDecl* d = &p->datas[i];
        mb_data(mod, d->offset, d->bytes, d->len);
    }

    /* 2.9 İkili modülü serileştir ve sadece çıktı baytlarını arena'da tut */
    u32 max_out = 65536;
    u8* tmp_out = ARENA_PUSH_ARRAY(arena, u8, max_out);
    MbBuf out_buf;
    mb_buf_init(&out_buf, tmp_out, max_out);
    u32 wasm_len = mb_module_finish(mod, &out_buf);

    /* Ara tamponları geri sarıp son wasm dizisini arena'nın tepesine kopyala */
    arena_pop_to(arena, mark);
    u8* allocated = ARENA_PUSH_ARRAY(arena, u8, wasm_len);
    for (u32 i = 0; i < wasm_len; i++) {
        allocated[i] = out_buf.p[i];
    }

    res.ok = TRUE;
    res.wasm = allocated;
    res.len = wasm_len;
    return res;

fail:
    res.ok = FALSE;
    res.line = p->lx.err_line;
    res.col = p->lx.err_col;
    for (usize i = 0; i < sizeof(res.msg); i++) {
        res.msg[i] = p->lx.err_msg[i];
    }
    arena_pop_to(arena, mark);
    return res;
}

MasmResult masm_assemble_cstr(Arena* arena, const char* src) {
    return masm_assemble(arena, src, masm_cstrlen(src));
}
