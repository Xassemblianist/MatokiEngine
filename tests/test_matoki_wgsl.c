/* tests/test_matoki_wgsl.c — Matoki → WGSL kod üretici (M15_A, D-108) birim testleri.
 * Kendi WGSL sözdizimi/kapsam/bağlama denetleyicisini, shaders/ altındaki 9 .mtk shader'ının
 * WGSL'e çevrilip build/wgsl/ altına yazılmasını, vertex shader üreticisini (mtk_wgsl_vert)
 * ve geçersiz/bilinmeyen IR komutlarının derleme hatası vermesini doğrular.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "core/arena.h"
#include "core/log.h"
#include "matoki/matoki.h"
#include "matoki/wgsl.h"
#include "matoki/ir.h"
#include "gfx3d/pipeline.h"

static u8 g_wgsl_arena_buf[MB(16)];
static char g_wgsl_out[65536];
static char g_wgsl_vert_out[16384];
static char g_wgsl_comb_out[81920];

static void ensure_wgsl_build_dir(void) {
    mkdir("build", 0755);
    mkdir("build/wgsl", 0755);
}

static char* read_text_file(const char* path, u32* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = (u32)rd;
    return buf;
}

static b32 write_text_file(const char* path, const char* text, u32 len) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;
    size_t wr = fwrite(text, 1, len, fp);
    fclose(fp);
    return wr == (size_t)len;
}

/* ==========================================================================
 * Küçük WGSL Sözdizimi, Kapsam ve Bağlama Denetleyicisi
 *  1. Parantez (), blok {}, köşeli parantez [] dengesi
 *  2. '--' veya '++' gibi kazara oluşan geçersiz belirteçlerin yokluğu
 *  3. @group(g) @binding(b) çiftlerinin tekilliği ve @location(l) tekilliği
 *  4. Bildirilen tüm struct, alan, global var, parametre ve let/var yerel
 *     değişkenlerin kapsam takibi; kullanılan her tanımlayıcının tanımlı olması
 * ========================================================================== */

typedef enum {
    WTOK_EOF = 0,
    WTOK_IDENT,
    WTOK_NUMBER,
    WTOK_SYM,
} WTokKind;

typedef struct {
    WTokKind kind;
    char     text[64];
    char     sym;
    u32      line;
} WTok;

typedef struct {
    const char* src;
    u32         pos;
    u32         len;
    u32         line;
} WLex;

static b32 is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static b32 is_ident_cont(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static b32 is_digit_ch(char c) {
    return c >= '0' && c <= '9';
}

static b32 wlex_next(WLex* lx, WTok* out, char* err, u32 err_cap) {
    memset(out, 0, sizeof(*out));
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
            continue;
        }
        if (c == '\n') {
            lx->line++;
            lx->pos++;
            continue;
        }
        /* Satır yorumu */
        if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        /* Blok yorumu */
        if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            b32 closed = FALSE;
            while (lx->pos + 1 < lx->len) {
                if (lx->src[lx->pos] == '\n') lx->line++;
                if (lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/') {
                    lx->pos += 2;
                    closed = TRUE;
                    break;
                }
                lx->pos++;
            }
            if (!closed) {
                fmt_format(err, err_cap, "satir %u: kapanmamis blok yorumu", lx->line);
                return FALSE;
            }
            continue;
        }
        break;
    }

    out->line = lx->line;
    if (lx->pos >= lx->len) {
        out->kind = WTOK_EOF;
        return TRUE;
    }

    char c = lx->src[lx->pos];

    /* '--' ve '++' kontrolü */
    if ((c == '-' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '-') ||
        (c == '+' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '+')) {
        fmt_format(err, err_cap, "satir %u: gecersiz '%c%c' belirteci", lx->line, c, c);
        return FALSE;
    }

    if (is_ident_start(c)) {
        out->kind = WTOK_IDENT;
        u32 n = 0;
        while (lx->pos < lx->len && is_ident_cont(lx->src[lx->pos])) {
            if (n + 1 < sizeof(out->text)) out->text[n++] = lx->src[lx->pos];
            lx->pos++;
        }
        out->text[n] = '\0';
        return TRUE;
    }

    if (is_digit_ch(c)) {
        out->kind = WTOK_NUMBER;
        u32 n = 0;
        while (lx->pos < lx->len) {
            char d = lx->src[lx->pos];
            if (is_ident_cont(d) || d == '.') {
                if (n + 1 < sizeof(out->text)) out->text[n++] = d;
                lx->pos++;
            } else if ((d == '+' || d == '-') && n > 0 &&
                       (out->text[n - 1] == 'e' || out->text[n - 1] == 'E')) {
                if (n + 1 < sizeof(out->text)) out->text[n++] = d;
                lx->pos++;
            } else {
                break;
            }
        }
        out->text[n] = '\0';
        return TRUE;
    }

    out->kind = WTOK_SYM;
    out->sym  = c;
    out->text[0] = c;
    out->text[1] = '\0';
    lx->pos++;
    return TRUE;
}

static b32 is_wgsl_builtin_or_kw(const char* s) {
    static const char* const list[] = {
        /* Anahtar kelimeler */
        "struct", "var", "let", "const", "fn", "return", "if", "else", "for",
        "discard", "true", "false", "uniform", "_",
        /* Yerleşik tipler */
        "f32", "i32", "u32", "bool", "vec2", "vec3", "vec4",
        "mat3x3", "mat4x4", "array", "texture_2d", "sampler",
        /* Yerleşik fonksiyonlar */
        "abs", "min", "max", "clamp", "mix", "step", "smoothstep",
        "floor", "ceil", "trunc", "round", "fract", "sqrt", "inverseSqrt",
        "sin", "cos", "tan", "exp2", "log2", "pow",
        "dot", "cross", "length", "normalize",
        "dpdx", "dpdy", "fwidth", "select", "bitcast",
        "pack4x8unorm", "unpack4x8unorm",
        "textureDimensions", "textureNumLevels", "textureLoad",
        "textureSample", "textureSampleLevel",
    };
    for (u32 i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (strcmp(s, list[i]) == 0) return TRUE;
    }
    return FALSE;
}

static b32 is_wgsl_attr_name(const char* s) {
    return strcmp(s, "group") == 0 || strcmp(s, "binding") == 0 ||
           strcmp(s, "location") == 0 || strcmp(s, "builtin") == 0 ||
           strcmp(s, "fragment") == 0 || strcmp(s, "vertex") == 0;
}

static b32 is_valid_swizzle(const char* s) {
    usize n = strlen(s);
    if (n == 0 || n > 4) return FALSE;
    b32 xyzw = TRUE, rgba = TRUE;
    for (usize i = 0; i < n; i++) {
        char c = s[i];
        if (c != 'x' && c != 'y' && c != 'z' && c != 'w') xyzw = FALSE;
        if (c != 'r' && c != 'g' && c != 'b' && c != 'a') rgba = FALSE;
    }
    return xyzw || rgba;
}

typedef struct {
    char name[64];
    u32  scope_depth; /* 0 = modül kapsamı (struct, global var, fn), >=1 = fonksiyon içi */
} WSym;

typedef struct {
    WTok  toks[16384];
    u32   tok_count;
    WSym  syms[2048];
    u32   sym_count;
    char  fields[256][64];
    u32   field_count;
    struct { u32 group; u32 binding; } bindings[64];
    u32   binding_count;
} WValidator;

static WValidator g_wval;

static b32 wval_has_sym(const WValidator* v, const char* name) {
    for (u32 i = 0; i < v->sym_count; i++) {
        if (strcmp(v->syms[i].name, name) == 0) return TRUE;
    }
    return FALSE;
}

static b32 wval_has_sym_at_scope(const WValidator* v, const char* name, u32 scope) {
    for (u32 i = 0; i < v->sym_count; i++) {
        if (v->syms[i].scope_depth == scope && strcmp(v->syms[i].name, name) == 0) return TRUE;
    }
    return FALSE;
}

static b32 wval_add_sym(WValidator* v, const char* name, u32 scope, u32 line, char* err, u32 err_cap) {
    if (wval_has_sym_at_scope(v, name, scope)) {
        fmt_format(err, err_cap, "satir %u: '%s' ayni kapsamda (%u) mukerrer tanimlandi", line, name, scope);
        return FALSE;
    }
    if (v->sym_count >= 2048) {
        fmt_format(err, err_cap, "satir %u: sembol tablosu dolu", line);
        return FALSE;
    }
    strncpy(v->syms[v->sym_count].name, name, sizeof(v->syms[0].name) - 1);
    v->syms[v->sym_count].name[sizeof(v->syms[0].name) - 1] = '\0';
    v->syms[v->sym_count].scope_depth = scope;
    v->sym_count++;
    return TRUE;
}

static void wval_pop_scope(WValidator* v, u32 scope) {
    while (v->sym_count > 0 && v->syms[v->sym_count - 1].scope_depth >= scope) {
        v->sym_count--;
    }
}

static void wval_add_field(WValidator* v, const char* name) {
    for (u32 i = 0; i < v->field_count; i++) {
        if (strcmp(v->fields[i], name) == 0) return;
    }
    if (v->field_count < 256) {
        strncpy(v->fields[v->field_count], name, sizeof(v->fields[0]) - 1);
        v->fields[v->field_count][sizeof(v->fields[0]) - 1] = '\0';
        v->field_count++;
    }
}

static b32 wval_has_field(const WValidator* v, const char* name) {
    for (u32 i = 0; i < v->field_count; i++) {
        if (strcmp(v->fields[i], name) == 0) return TRUE;
    }
    return FALSE;
}

static b32 wgsl_validate_syntax(const char* wgsl, char* err, u32 err_cap) {
    if (err && err_cap > 0) err[0] = '\0';
    if (!wgsl || wgsl[0] == '\0') {
        fmt_format(err, err_cap, "bos WGSL metni");
        return FALSE;
    }

    WValidator* v = &g_wval;
    memset(v, 0, sizeof(*v));

    /* 1. Parantez / blok / köşeli parantez dengesi ve sözcükleme */
    WLex lx = { wgsl, 0, (u32)strlen(wgsl), 1 };
    char brk_stk[256];
    u32  brk_line[256];
    u32  brk_depth = 0;

    while (1) {
        WTok t;
        if (!wlex_next(&lx, &t, err, err_cap)) return FALSE;
        if (t.kind == WTOK_EOF) break;

        if (t.kind == WTOK_SYM) {
            char c = t.sym;
            if (c == '(' || c == '{' || c == '[') {
                if (brk_depth >= sizeof(brk_stk)) {
                    fmt_format(err, err_cap, "satir %u: parantez yigini tasti", t.line);
                    return FALSE;
                }
                brk_stk[brk_depth]  = c;
                brk_line[brk_depth] = t.line;
                brk_depth++;
            } else if (c == ')' || c == '}' || c == ']') {
                char exp = (c == ')') ? '(' : (c == '}') ? '{' : '[';
                if (brk_depth == 0 || brk_stk[brk_depth - 1] != exp) {
                    fmt_format(err, err_cap, "satir %u: eslesmeyen '%c' kapanisi", t.line, c);
                    return FALSE;
                }
                brk_depth--;
            }
        }

        if (v->tok_count >= 16384) {
            fmt_format(err, err_cap, "satir %u: cok fazla belirtec", t.line);
            return FALSE;
        }
        v->toks[v->tok_count++] = t;
    }

    if (brk_depth != 0) {
        fmt_format(err, err_cap, "satir %u: kapanmamis '%c' parantezi",
                   brk_line[brk_depth - 1], brk_stk[brk_depth - 1]);
        return FALSE;
    }

    /* 2. @group/@binding tekilliği, bildirimler ve kapsam denetimi */
    u32 scope = 0;
    b32 has_entry_fn = FALSE;
    b32 pending_group = FALSE;
    u32 last_group = 0;

    u32 i = 0;
    while (i < v->tok_count) {
        const WTok* t = &v->toks[i];

        /* @ öznitelikleri */
        if (t->kind == WTOK_SYM && t->sym == '@') {
            if (i + 1 >= v->tok_count || v->toks[i + 1].kind != WTOK_IDENT) {
                fmt_format(err, err_cap, "satir %u: '@' sonrasinda oznitelik bekleniyordu", t->line);
                return FALSE;
            }
            const char* aname = v->toks[i + 1].text;
            if (!is_wgsl_attr_name(aname)) {
                fmt_format(err, err_cap, "satir %u: bilinmeyen oznitelik '@%s'", t->line, aname);
                return FALSE;
            }
            if (strcmp(aname, "fragment") == 0 || strcmp(aname, "vertex") == 0) {
                has_entry_fn = TRUE;
                i += 2;
                continue;
            }
            /* @group(N), @binding(N), @location(N), @builtin(position) */
            if (i + 4 >= v->tok_count ||
                v->toks[i + 2].kind != WTOK_SYM || v->toks[i + 2].sym != '(' ||
                v->toks[i + 4].kind != WTOK_SYM || v->toks[i + 4].sym != ')') {
                fmt_format(err, err_cap, "satir %u: '@%s(...)' bicimi gecersiz", t->line, aname);
                return FALSE;
            }
            if (strcmp(aname, "builtin") == 0) {
                if (v->toks[i + 3].kind != WTOK_IDENT || strcmp(v->toks[i + 3].text, "position") != 0) {
                    fmt_format(err, err_cap, "satir %u: bilinmeyen @builtin(%s)", t->line, v->toks[i + 3].text);
                    return FALSE;
                }
            } else {
                if (v->toks[i + 3].kind != WTOK_NUMBER) {
                    fmt_format(err, err_cap, "satir %u: '@%s' tamsayi arguman bekler", t->line, aname);
                    return FALSE;
                }
                u32 val = (u32)strtoul(v->toks[i + 3].text, NULL, 10);
                if (strcmp(aname, "group") == 0) {
                    last_group = val;
                    pending_group = TRUE;
                } else if (strcmp(aname, "binding") == 0) {
                    if (!pending_group) {
                        fmt_format(err, err_cap, "satir %u: @group olmadan @binding(%u)", t->line, val);
                        return FALSE;
                    }
                    for (u32 b = 0; b < v->binding_count; b++) {
                        if (v->bindings[b].group == last_group && v->bindings[b].binding == val) {
                            fmt_format(err, err_cap,
                                       "satir %u: mukerrer @group(%u) @binding(%u)",
                                       t->line, last_group, val);
                            return FALSE;
                        }
                    }
                    v->bindings[v->binding_count].group   = last_group;
                    v->bindings[v->binding_count].binding = val;
                    v->binding_count++;
                    pending_group = FALSE;
                }
            }
            i += 5;
            continue;
        }

        /* struct Name { ... }; */
        if (t->kind == WTOK_IDENT && strcmp(t->text, "struct") == 0 && scope == 0) {
            if (i + 2 >= v->tok_count || v->toks[i + 1].kind != WTOK_IDENT ||
                v->toks[i + 2].kind != WTOK_SYM || v->toks[i + 2].sym != '{') {
                fmt_format(err, err_cap, "satir %u: gecersiz struct bildirimi", t->line);
                return FALSE;
            }
            if (!wval_add_sym(v, v->toks[i + 1].text, 0, t->line, err, err_cap)) return FALSE;
            i += 3;
            u32 loc_mask = 0;
            while (i < v->tok_count && !(v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '}')) {
                if (v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '@') {
                    const char* an = v->toks[i + 1].text;
                    if (strcmp(an, "location") == 0) {
                        u32 loc = (u32)strtoul(v->toks[i + 3].text, NULL, 10);
                        if (loc < 32 && (loc_mask & (1u << loc))) {
                            fmt_format(err, err_cap, "satir %u: struct icinde mukerrer @location(%u)",
                                       v->toks[i].line, loc);
                            return FALSE;
                        }
                        if (loc < 32) loc_mask |= (1u << loc);
                    }
                    i += 5;
                    continue;
                }
                if (v->toks[i].kind == WTOK_IDENT && i + 1 < v->tok_count &&
                    v->toks[i + 1].kind == WTOK_SYM && v->toks[i + 1].sym == ':') {
                    wval_add_field(v, v->toks[i].text);
                    i += 2;
                    u32 angle_depth = 0;
                    while (i < v->tok_count &&
                           !(v->toks[i].kind == WTOK_SYM &&
                             ((v->toks[i].sym == ',' && angle_depth == 0) || v->toks[i].sym == '}'))) {
                        if (v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '<') {
                            angle_depth++;
                        } else if (v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '>' && angle_depth > 0) {
                            angle_depth--;
                        } else if (v->toks[i].kind == WTOK_IDENT &&
                            !is_wgsl_builtin_or_kw(v->toks[i].text) &&
                            !wval_has_sym(v, v->toks[i].text)) {
                            fmt_format(err, err_cap, "satir %u: tanimsiz struct alan tipi '%s'",
                                       v->toks[i].line, v->toks[i].text);
                            return FALSE;
                        }
                        i++;
                    }
                    if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ',') i++;
                } else {
                    fmt_format(err, err_cap, "satir %u: struct icinde beklenmeyen '%s'",
                               v->toks[i].line, v->toks[i].text);
                    return FALSE;
                }
            }
            if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '}') i++;
            if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ';') i++;
            continue;
        }

        /* Modül seviyesi var<uniform> u: Type; veya var t: Type; */
        if (t->kind == WTOK_IDENT && strcmp(t->text, "var") == 0 && scope == 0) {
            i++;
            if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '<') {
                if (i + 2 >= v->tok_count || v->toks[i + 2].kind != WTOK_SYM || v->toks[i + 2].sym != '>') {
                    fmt_format(err, err_cap, "satir %u: gecersiz var<...> adres uzayi", t->line);
                    return FALSE;
                }
                i += 3;
            }
            if (i + 2 >= v->tok_count || v->toks[i].kind != WTOK_IDENT ||
                v->toks[i + 1].kind != WTOK_SYM || v->toks[i + 1].sym != ':') {
                fmt_format(err, err_cap, "satir %u: gecersiz modul var bildirimi", t->line);
                return FALSE;
            }
            if (!wval_add_sym(v, v->toks[i].text, 0, t->line, err, err_cap)) return FALSE;
            i += 2;
            while (i < v->tok_count && !(v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ';')) {
                if (v->toks[i].kind == WTOK_IDENT &&
                    !is_wgsl_builtin_or_kw(v->toks[i].text) &&
                    !wval_has_sym(v, v->toks[i].text)) {
                    fmt_format(err, err_cap, "satir %u: tanimsiz modul var tipi '%s'",
                               v->toks[i].line, v->toks[i].text);
                    return FALSE;
                }
                i++;
            }
            if (i < v->tok_count) i++;
            continue;
        }

        /* fn name(param: Type, ...) -> ... { */
        if (t->kind == WTOK_IDENT && strcmp(t->text, "fn") == 0 && scope == 0) {
            if (i + 2 >= v->tok_count || v->toks[i + 1].kind != WTOK_IDENT ||
                v->toks[i + 2].kind != WTOK_SYM || v->toks[i + 2].sym != '(') {
                fmt_format(err, err_cap, "satir %u: gecersiz fn basligi", t->line);
                return FALSE;
            }
            if (!wval_add_sym(v, v->toks[i + 1].text, 0, t->line, err, err_cap)) return FALSE;
            i += 3;
            /* Parametreleri kapsam 1'e kaydet */
            while (i < v->tok_count && !(v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ')')) {
                if (v->toks[i].kind == WTOK_IDENT && i + 2 < v->tok_count &&
                    v->toks[i + 1].kind == WTOK_SYM && v->toks[i + 1].sym == ':' &&
                    v->toks[i + 2].kind == WTOK_IDENT) {
                    const char* pname = v->toks[i].text;
                    const char* ptype = v->toks[i + 2].text;
                    if (!is_wgsl_builtin_or_kw(ptype) && !wval_has_sym(v, ptype)) {
                        fmt_format(err, err_cap, "satir %u: tanimsiz parametre tipi '%s'",
                                   v->toks[i + 2].line, ptype);
                        return FALSE;
                    }
                    if (!wval_add_sym(v, pname, 1, v->toks[i].line, err, err_cap)) return FALSE;
                    i += 3;
                    if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ',') i++;
                } else {
                    fmt_format(err, err_cap, "satir %u: gecersiz fn parametresi", v->toks[i].line);
                    return FALSE;
                }
            }
            /* Gövde '{' işaretine kadar dönüş tipini doğrula */
            while (i < v->tok_count && !(v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '{')) {
                if (v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '@') {
                    i += 5;
                    continue;
                }
                if (v->toks[i].kind == WTOK_IDENT &&
                    !is_wgsl_builtin_or_kw(v->toks[i].text) &&
                    !wval_has_sym(v, v->toks[i].text)) {
                    fmt_format(err, err_cap, "satir %u: tanimsiz fn donus tipi '%s'",
                               v->toks[i].line, v->toks[i].text);
                    return FALSE;
                }
                i++;
            }
            if (i < v->tok_count && v->toks[i].kind == WTOK_SYM && v->toks[i].sym == '{') {
                scope = 1;
                i++;
            }
            continue;
        }

        /* Fonksiyon içi blok açılış / kapanış */
        if (t->kind == WTOK_SYM && t->sym == '{') {
            scope++;
            i++;
            continue;
        }
        if (t->kind == WTOK_SYM && t->sym == '}') {
            wval_pop_scope(v, scope);
            if (scope > 0) scope--;
            i++;
            continue;
        }

        /* Fonksiyon içi let / var bildirimi */
        if (scope > 0 && t->kind == WTOK_IDENT &&
            (strcmp(t->text, "let") == 0 || strcmp(t->text, "var") == 0)) {
            if (i + 1 >= v->tok_count || v->toks[i + 1].kind != WTOK_IDENT) {
                fmt_format(err, err_cap, "satir %u: '%s' sonrasinda degisken adi bekleniyordu",
                           t->line, t->text);
                return FALSE;
            }
            const char* vname = v->toks[i + 1].text;
            u32 vline = v->toks[i + 1].line;
            i += 2;
            /* ';' işaretine kadar olan tip ve sağ-taraf ifadesindeki tüm tanımlayıcıları doğrula
             * (değişkenin kendisi henüz kapsama eklenmedi -> 'let x = x;' yakalanır!) */
            while (i < v->tok_count && !(v->toks[i].kind == WTOK_SYM && v->toks[i].sym == ';')) {
                if (v->toks[i].kind == WTOK_IDENT) {
                    b32 after_dot = (i > 0 && v->toks[i - 1].kind == WTOK_SYM && v->toks[i - 1].sym == '.');
                    if (after_dot) {
                        if (!wval_has_field(v, v->toks[i].text) && !is_valid_swizzle(v->toks[i].text)) {
                            fmt_format(err, err_cap, "satir %u: bilinmeyen alan/swizzle '.%s'",
                                       v->toks[i].line, v->toks[i].text);
                            return FALSE;
                        }
                    } else if (!is_wgsl_builtin_or_kw(v->toks[i].text) && !wval_has_sym(v, v->toks[i].text)) {
                        fmt_format(err, err_cap, "satir %u: tanimsiz tanimlayici '%s'",
                                   v->toks[i].line, v->toks[i].text);
                        return FALSE;
                    }
                }
                i++;
            }
            if (!wval_add_sym(v, vname, scope, vline, err, err_cap)) return FALSE;
            if (i < v->tok_count) i++; /* ';' */
            continue;
        }

        /* Genel ifade / deyim tanımlayıcı denetimi */
        if (t->kind == WTOK_IDENT) {
            b32 after_dot = (i > 0 && v->toks[i - 1].kind == WTOK_SYM && v->toks[i - 1].sym == '.');
            if (after_dot) {
                if (!wval_has_field(v, t->text) && !is_valid_swizzle(t->text)) {
                    fmt_format(err, err_cap, "satir %u: bilinmeyen alan/swizzle '.%s'", t->line, t->text);
                    return FALSE;
                }
            } else if (!is_wgsl_builtin_or_kw(t->text) && !wval_has_sym(v, t->text)) {
                fmt_format(err, err_cap, "satir %u: tanimsiz tanimlayici '%s'", t->line, t->text);
                return FALSE;
            }
        }
        i++;
    }

    if (!has_entry_fn) {
        fmt_format(err, err_cap, "@fragment veya @vertex giris noktasi bulunamadi");
        return FALSE;
    }

    return TRUE;
}

/* ---------- 1. WGSL Denetleyicinin Kendi Testleri ---------- */

TEST(matoki_wgsl_validator_self_test) {
    char err[256];

    const char* valid_src =
        "struct FragIn {\n"
        "    @builtin(position) frag_pos: vec4<f32>,\n"
        "};\n"
        "@fragment\n"
        "fn frag(fin: FragIn) -> @location(0) vec4<f32> {\n"
        "    let a: f32 = fin.frag_pos.x;\n"
        "    return vec4<f32>(a, 0.0, 0.0, 1.0);\n"
        "}\n";
    CHECK(wgsl_validate_syntax(valid_src, err, sizeof(err)));

    /* Dengesiz parantez */
    const char* bad_paren =
        "@fragment\n"
        "fn frag() -> @location(0) vec4<f32> {\n"
        "    return vec4<f32>(1.0, 0.0, 0.0, 1.0;\n"
        "}\n";
    CHECK(!wgsl_validate_syntax(bad_paren, err, sizeof(err)));

    /* Tanımsız tanımlayıcı */
    const char* bad_ident =
        "@fragment\n"
        "fn frag() -> @location(0) vec4<f32> {\n"
        "    return vec4<f32>(t999, 0.0, 0.0, 1.0);\n"
        "}\n";
    CHECK(!wgsl_validate_syntax(bad_ident, err, sizeof(err)));
    CHECK(strstr(err, "t999") != NULL);

    /* Blok kapsamı dışına sızan let değişkeni */
    const char* bad_scope =
        "@fragment\n"
        "fn frag() -> @location(0) vec4<f32> {\n"
        "    if (true) {\n"
        "        let inner_v: f32 = 1.0;\n"
        "    }\n"
        "    return vec4<f32>(inner_v, 0.0, 0.0, 1.0);\n"
        "}\n";
    CHECK(!wgsl_validate_syntax(bad_scope, err, sizeof(err)));
    CHECK(strstr(err, "inner_v") != NULL);

    /* Mükerrer @group/@binding */
    const char* bad_binding =
        "@group(0) @binding(1) var t0: texture_2d<f32>;\n"
        "@group(0) @binding(1) var s0: sampler;\n"
        "@fragment\n"
        "fn frag() -> @location(0) vec4<f32> {\n"
        "    return vec4<f32>(1.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    CHECK(!wgsl_validate_syntax(bad_binding, err, sizeof(err)));
    CHECK(strstr(err, "mukerrer") != NULL);
}

/* ---------- 2. shaders/ Altındaki 9 Shader'ın Çevrimi ve build/wgsl/ Çıktısı ---------- */

TEST(matoki_wgsl_all_9_shaders) {
    static const struct {
        const char* name;
        const char* mtk_path;
        const char* wgsl_path;
        const char* must_contain;
    } shaders[9] = {
        { "branchy",   "shaders/branchy.mtk",   "build/wgsl/branchy.wgsl",   "discard;" },
        { "dungeon3d", "shaders/dungeon3d.mtk", "build/wgsl/dungeon3d.wgsl", "textureNumLevels(" },
        { "lit3d",     "shaders/lit3d.mtk",     "build/wgsl/lit3d.wgsl",     "dpdx(" },
        { "pbr3d",     "shaders/pbr3d.mtk",     "build/wgsl/pbr3d.wgsl",     "tex_1" },
        { "plasma",    "shaders/plasma.mtk",    "build/wgsl/plasma.wgsl",    "sqrt(" },
        { "solid",     "shaders/solid.mtk",     "build/wgsl/solid.wgsl",     "mtk_u.data[0]" },
        { "sprite",    "shaders/sprite.mtk",    "build/wgsl/sprite.wgsl",    "discard;" },
        { "unlit3d",   "shaders/unlit3d.mtk",   "build/wgsl/unlit3d.wgsl",   "abs(" },
        { "vcolor",    "shaders/vcolor.mtk",    "build/wgsl/vcolor.wgsl",    "fin.v_v_color" },
    };

    ensure_wgsl_build_dir();

    Arena arena;
    arena_init(&arena, g_wgsl_arena_buf, sizeof(g_wgsl_arena_buf));

    for (u32 i = 0; i < 9; i++) {
        u32 src_len = 0;
        char* src = read_text_file(shaders[i].mtk_path, &src_len);
        CHECK(src != NULL && src_len > 0);
        if (!src) continue;

        arena_reset(&arena);
        MtkLayout layout;
        MtkDiag diag;
        u32 wlen = mtk_compile_wgsl(src, src_len, shaders[i].mtk_path, NULL, &arena,
                                    g_wgsl_out, sizeof(g_wgsl_out), &layout, &diag);
        if (wlen == 0) {
            LOG("WGSL derleme hatasi (%s):\n%s", shaders[i].mtk_path, diag.text);
        }
        CHECK(wlen > 0);
        CHECK(diag.count == 0);
        CHECK(strlen(g_wgsl_out) == wlen);

        char val_err[256];
        b32 valid = wgsl_validate_syntax(g_wgsl_out, val_err, sizeof(val_err));
        if (!valid) {
            LOG("WGSL dogrulama hatasi (%s): %s\n%s", shaders[i].name, val_err, g_wgsl_out);
        }
        CHECK(valid);
        CHECK(strstr(g_wgsl_out, shaders[i].must_contain) != NULL);

        CHECK(write_text_file(shaders[i].wgsl_path, g_wgsl_out, wlen));
        free(src);
    }
}

/* ---------- 3. Vertex Shader Üretici (mtk_wgsl_vert) ve Birleşik Boru Hattı ---------- */

TEST(matoki_wgsl_vertex_and_pipeline) {
    ensure_wgsl_build_dir();
    Arena arena;
    arena_init(&arena, g_wgsl_arena_buf, sizeof(g_wgsl_arena_buf));

    /* pbr3d.mtk varying dizilimi: uv, nrm, depth, wpos, spos */
    static const u8 pbr_attrs[5] = {
        MTK_WGSL_ATTR_UV,
        MTK_WGSL_ATTR_NORMAL,
        MTK_WGSL_ATTR_VIEW_DEPTH,
        MTK_WGSL_ATTR_WORLD_POS,
        MTK_WGSL_ATTR_SHADOW_POS,
    };

    char err[256];
    u32 vlen = mtk_wgsl_vert(pbr_attrs, 5, g_wgsl_vert_out, sizeof(g_wgsl_vert_out), err, sizeof(err));
    CHECK(vlen > 0);
    CHECK(wgsl_validate_syntax(g_wgsl_vert_out, err, sizeof(err)));
    CHECK(strstr(g_wgsl_vert_out, "@vertex") != NULL);
    CHECK(strstr(g_wgsl_vert_out, "vs_u.shadow_mat") != NULL);
    CHECK(write_text_file("build/wgsl/pbr3d_vs.wgsl", g_wgsl_vert_out, vlen));

    /* pbr3d vertex + fragment aynı WGSL modülünde birleştirildiğinde de @group/@binding tekil olmalı */
    u32 pbr_src_len = 0;
    char* pbr_src = read_text_file("shaders/pbr3d.mtk", &pbr_src_len);
    CHECK(pbr_src != NULL);
    if (pbr_src) {
        MtkLayout layout;
        MtkDiag diag;
        u32 flen = mtk_compile_wgsl(pbr_src, pbr_src_len, "shaders/pbr3d.mtk", NULL, &arena,
                                    g_wgsl_out, sizeof(g_wgsl_out), &layout, &diag);
        CHECK(flen > 0);
        snprintf(g_wgsl_comb_out, sizeof(g_wgsl_comb_out), "%s\n%s", g_wgsl_vert_out, g_wgsl_out);
        CHECK(wgsl_validate_syntax(g_wgsl_comb_out, err, sizeof(err)));
        free(pbr_src);
    }

    /* Tüm 6 G3dAttr özniteliğini aynı anda içeren vertex shader */
    static const u8 all_attrs[6] = {
        MTK_WGSL_ATTR_UV,
        MTK_WGSL_ATTR_NORMAL,
        MTK_WGSL_ATTR_VIEW_DEPTH,
        MTK_WGSL_ATTR_COLOR,
        MTK_WGSL_ATTR_WORLD_POS,
        MTK_WGSL_ATTR_SHADOW_POS,
    };
    vlen = mtk_wgsl_vert(all_attrs, 6, g_wgsl_vert_out, sizeof(g_wgsl_vert_out), err, sizeof(err));
    CHECK(vlen > 0);
    CHECK(wgsl_validate_syntax(g_wgsl_vert_out, err, sizeof(err)));
    CHECK(strstr(g_wgsl_vert_out, "vin.color") != NULL);

    /* 0 öznitelikli (yalnız konum) vertex shader */
    vlen = mtk_wgsl_vert(NULL, 0, g_wgsl_vert_out, sizeof(g_wgsl_vert_out), err, sizeof(err));
    CHECK(vlen > 0);
    CHECK(wgsl_validate_syntax(g_wgsl_vert_out, err, sizeof(err)));
}

/* ---------- 4. Tüm Yerleşikler, Türevler, Örnekleme ve IR Komutları ---------- */

TEST(matoki_wgsl_all_builtins_and_ir_ops) {
    Arena arena;
    arena_init(&arena, g_wgsl_arena_buf, sizeof(g_wgsl_arena_buf));

    /* Türevler, sample_level, tex_size, frag_coord, matrisler, bit işlemleri ve trigonometri */
    const char* rich_mtk =
        "uniform m: mat4;\n"
        "uniform count: i32;\n"
        "uniform t0: texture2d @bilinear @clamp;\n"
        "in uv: vec2;\n"
        "fn helper(p: vec2) -> f32 {\n"
        "    let dx = dFdx(p.x);\n"
        "    let dy = dFdy(p.y);\n"
        "    let fw = fwidth(p.x + p.y);\n"
        "    return dx + dy + fw;\n"
        "}\n"
        "fn frag() -> rgba {\n"
        "    let sz = tex_size(t0);\n"
        "    let c0 = sample_level(t0, uv, count & 3);\n"
        "    let fc = frag_coord;\n"
        "    var acc = helper(uv) + fc.x * 0.001 + fc.y * 0.001;\n"
        "    if uv.x > 0.25 {\n"
        "        acc += dFdx(uv.y) + pow(max(uv.x, 0.01), 2.2) + exp2(-1.5) + log2(1.5);\n"
        "    } else {\n"
        "        acc += ceil(uv.x) + fract(uv.y) + step(0.5, uv.x) + smoothstep(0.0, 1.0, uv.y);\n"
        "    }\n"
        "    let v4 = m * vec4(uv.x, uv.y, acc, 1.0);\n"
        "    let cr = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
        "    let ln = length(cr) + f32((count << 2) ^ (count >> 1) ^ f32_to_i: 0);\n"
        "    return vec4(c0.rgb + v4.xyz * 0.01 * ln, c0.a);\n"
        "}\n";
    /* Yukarıdaki sözdiziminde f32_to_i yerine geçerli i32(acc) kullanalım */
    const char* valid_rich_mtk =
        "uniform m: mat4;\n"
        "uniform count: i32;\n"
        "uniform t0: texture2d @bilinear @clamp;\n"
        "in uv: vec2;\n"
        "fn helper(p: vec2) -> f32 {\n"
        "    let dx = dFdx(p.x);\n"
        "    let dy = dFdy(p.y);\n"
        "    let fw = fwidth(p.x + p.y);\n"
        "    return dx + dy + fw;\n"
        "}\n"
        "fn frag() -> rgba {\n"
        "    let sz = tex_size(t0);\n"
        "    let c0 = sample_level(t0, uv, count & 3);\n"
        "    let fc = frag_coord;\n"
        "    var acc = helper(uv) + fc.x * 0.001 + fc.y * 0.001 + f32(sz.x);\n"
        "    if uv.x > 0.25 {\n"
        "        acc += dFdx(uv.y) + pow(max(uv.x, 0.01), 2.2) + exp2(-1.5) + log2(1.5);\n"
        "    } else {\n"
        "        acc += ceil(uv.x) + fract(uv.y) + step(0.5, uv.x) + smoothstep(0.0, 1.0, uv.y);\n"
        "    }\n"
        "    let v4 = m * vec4(uv.x, uv.y, acc, 1.0);\n"
        "    let cr = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
        "    let ln = length(cr) + f32((count << 2) ^ (count >> 1) ^ (~count) | i32(acc));\n"
        "    return vec4(c0.rgb + v4.xyz * 0.01 * ln, c0.a);\n"
        "}\n";
    (void)rich_mtk;

    MtkLayout layout;
    MtkDiag diag;
    u32 wlen = mtk_compile_wgsl(valid_rich_mtk, (u32)strlen(valid_rich_mtk), "rich.mtk", NULL,
                                &arena, g_wgsl_out, sizeof(g_wgsl_out), &layout, &diag);
    CHECK(wlen > 0);
    CHECK(diag.count == 0);

    char err[256];
    CHECK(wgsl_validate_syntax(g_wgsl_out, err, sizeof(err)));
    CHECK(strstr(g_wgsl_out, "dpdx(") != NULL);
    CHECK(strstr(g_wgsl_out, "dpdy(") != NULL);
    CHECK(strstr(g_wgsl_out, "fin.frag_pos.x") != NULL);
    CHECK(strstr(g_wgsl_out, "fin.frag_pos.y") != NULL);

    /* Doğrudan IrFunc üzerinde IR_FTRUNC, IR_FNEAREST, IR_TEX_FLAGS, I32_MIN ve select sırası testi */
    arena_reset(&arena);
    IrFunc f;
    ir_init(&f, &arena, 128);
    f.uniform_bytes = 16;
    f.varying_count = 1;
    f.texture_count = 1;

    u32 v0    = ir_varying(&f, 0);
    u32 tr    = ir_op1(&f, IR_FTRUNC, v0);
    u32 nr    = ir_op1(&f, IR_FNEAREST, v0);
    u32 tf    = ir_tex_info(&f, IR_TEX_FLAGS, 0);
    u32 imin  = ir_const_i32(&f, I32_MIN);
    u32 raw_l = ir_texel_load(&f, 0, ir_op2(&f, IR_IADD, tf, imin));
    u32 rf    = ir_op1(&f, IR_I2F, raw_l);
    u32 cond  = ir_op2(&f, IR_FGT, v0, ir_const_f32(&f, 0.0f));
    u32 sel   = ir_select(&f, cond, tr, nr);
    u32 c_nz  = ir_const_f32(&f, -0.0f);
    u32 c_one = ir_const_f32(&f, 1.0f);
    ir_output(&f, 0, sel);
    ir_output(&f, 1, rf);
    ir_output(&f, 2, c_nz);
    ir_output(&f, 3, c_one);

    wlen = mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err));
    CHECK(wlen > 0);
    CHECK(wgsl_validate_syntax(g_wgsl_out, err, sizeof(err)));
    CHECK(strstr(g_wgsl_out, "trunc(") != NULL);
    CHECK(strstr(g_wgsl_out, "round(") != NULL);
    /* select(if_false=t2(nr), if_true=t1(tr), cond=t9) */
    CHECK(strstr(g_wgsl_out, "select(t2, t1, t9)") != NULL);
}

/* ---------- 5. Bilinmeyen / Desteklenmeyen IR Komutları -> Derleme Hatası ---------- */

TEST(matoki_wgsl_rejects_invalid_and_unknown_ir) {
    Arena arena;
    arena_init(&arena, g_wgsl_arena_buf, sizeof(g_wgsl_arena_buf));
    char err[256];

    /* 1. Bilinmeyen opcode (IR_OP_COUNT ve 999) */
    IrFunc f;
    ir_init(&f, &arena, 32);
    u32 c0 = ir_const_f32(&f, 1.0f);
    u32 bad_op = ir_op1(&f, IR_FABS, c0);
    ir_output(&f, 0, bad_op);
    ir_output(&f, 1, c0);
    ir_output(&f, 2, c0);
    ir_output(&f, 3, c0);

    f.insts[bad_op].op = (u16)IR_OP_COUNT;
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);
    CHECK(err[0] != '\0');

    f.insts[bad_op].op = 999u;
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);
    CHECK(err[0] != '\0');

    /* 2. Hizasız uniform ofseti (imm.u = 2) */
    ir_init(&f, &arena, 32);
    f.uniform_bytes = 16;
    u32 u0 = ir_uniform(&f, IR_T_F32, 2);
    ir_output(&f, 0, u0);
    ir_output(&f, 1, u0);
    ir_output(&f, 2, u0);
    ir_output(&f, 3, u0);
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);
    CHECK(strstr(err, "uniform") != NULL);

    /* 3. Sınır dışı uniform ofseti (imm.u = 32, uniform_bytes = 16) */
    f.insts[u0].imm.u = 32;
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);

    /* 4. Sınır dışı varying indeksi */
    ir_init(&f, &arena, 32);
    f.varying_count = 2;
    u32 v0 = ir_varying(&f, 5);
    ir_output(&f, 0, v0);
    ir_output(&f, 1, v0);
    ir_output(&f, 2, v0);
    ir_output(&f, 3, v0);
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);

    /* 5. Sınır dışı doku slotu */
    ir_init(&f, &arena, 32);
    f.texture_count = 1;
    u32 tw = ir_tex_info(&f, IR_TEX_W, 3);
    u32 tf = ir_op1(&f, IR_I2F, tw);
    ir_output(&f, 0, tf);
    ir_output(&f, 1, tf);
    ir_output(&f, 2, tf);
    ir_output(&f, 3, tf);
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, g_wgsl_out, sizeof(g_wgsl_out), err, sizeof(err)) == 0);

    /* 6. Yetersiz tampon kapasitesi */
    ir_init(&f, &arena, 32);
    c0 = ir_const_f32(&f, 1.0f);
    ir_output(&f, 0, c0);
    ir_output(&f, 1, c0);
    ir_output(&f, 2, c0);
    ir_output(&f, 3, c0);
    char tiny[32];
    CHECK(mtk_wgsl_frag(&f, NULL, NULL, tiny, sizeof(tiny), err, sizeof(err)) == 0);
    CHECK(tiny[0] == '\0');

    /* 7. Vertex shader geçersiz öznitelik */
    u8 bad_attr = 99;
    CHECK(mtk_wgsl_vert(&bad_attr, 1, g_wgsl_vert_out, sizeof(g_wgsl_vert_out), err, sizeof(err)) == 0);
    CHECK(mtk_wgsl_vert(&bad_attr, 7, g_wgsl_vert_out, sizeof(g_wgsl_vert_out), err, sizeof(err)) == 0);
}
