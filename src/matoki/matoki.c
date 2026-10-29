/* matoki/matoki.c — Matoki derleyicisi üst seviye sürücüsü ve tanılama yardımcıları.
 * Kaynak kodu parser -> sema -> lower hattından geçirerek IrFunc üretir (mtk_compile_ir)
 * veya ir_optimize + mtk_codegen ile doğrudan wasm ikili modülüne derler (mtk_compile_wasm).
 */
#include "matoki/matoki.h"
#include "matoki/parser.h"
#include "matoki/sema.h"
#include "matoki/lower.h"
#include "core/log.h"

void mtk_diag_init(MtkDiag* diag) {
    if (!diag) return;
    diag->text[0] = '\0';
    diag->len     = 0;
    diag->count   = 0;
}

static void diag_append_str(MtkDiag* diag, const char* s, u32 n) {
    for (u32 i = 0; i < n && diag->len + 1 < sizeof(diag->text); i++) {
        diag->text[diag->len++] = s[i];
    }
    diag->text[diag->len] = '\0';
}

static void diag_append_char(MtkDiag* diag, char c) {
    if (diag->len + 1 < sizeof(diag->text)) {
        diag->text[diag->len++] = c;
        diag->text[diag->len] = '\0';
    }
}

void mtk_diag_error(MtkDiag* diag, const char* filename, const char* src, u32 src_len,
                    u32 line, u32 col, const char* fmt, ...) {
    if (!diag || diag->count >= MTK_MAX_ERRORS) return;
    diag->count++;

    char msg[256];
    VaList ap;
    VA_START(ap, fmt);
    fmt_vformat(msg, sizeof(msg), fmt, ap);
    VA_END(ap);

    char header[512];
    u32 hlen = fmt_format(header, sizeof(header), "%s:%u:%u: hata: %s\n",
                          filename ? filename : "<shader>", line, col, msg);
    diag_append_str(diag, header, hlen);

    if (!src || src_len == 0 || line == 0) return;

    /* İlgili kaynak satırını bul */
    u32 cur_line   = 1;
    u32 line_start = 0;
    while (line_start < src_len && cur_line < line) {
        if (src[line_start] == '\n') cur_line++;
        line_start++;
    }

    if (cur_line == line && line_start <= src_len) {
        u32 line_end = line_start;
        while (line_end < src_len && src[line_end] != '\n' && src[line_end] != '\r') {
            line_end++;
        }
        diag_append_str(diag, "  ", 2);
        diag_append_str(diag, src + line_start, line_end - line_start);
        diag_append_char(diag, '\n');

        diag_append_str(diag, "  ", 2);
        u32 caret_offset = col > 0 ? col - 1 : 0;
        u32 line_len     = line_end - line_start;
        for (u32 i = 0; i < caret_offset; i++) {
            char ch = (i < line_len && src[line_start + i] == '\t') ? '\t' : ' ';
            diag_append_char(diag, ch);
        }
        diag_append_str(diag, "^\n", 2);
    }
}

/* ---------- Tip yardımcıları ---------- */

const char* mtk_type_name(MtkType t) {
    switch (t) {
    case MTK_TYPE_VOID:      return "void";
    case MTK_TYPE_F32:       return "f32";
    case MTK_TYPE_I32:       return "i32";
    case MTK_TYPE_U32:       return "u32";
    case MTK_TYPE_BOOL:      return "bool";
    case MTK_TYPE_VEC2:      return "vec2";
    case MTK_TYPE_VEC3:      return "vec3";
    case MTK_TYPE_VEC4:      return "vec4";
    case MTK_TYPE_IVEC2:     return "ivec2";
    case MTK_TYPE_IVEC3:     return "ivec3";
    case MTK_TYPE_IVEC4:     return "ivec4";
    case MTK_TYPE_MAT3:          return "mat3";
    case MTK_TYPE_MAT4:          return "mat4";
    case MTK_TYPE_TEXTURE2D:     return "texture2d";
    case MTK_TYPE_TEXTURE2D_F32: return "texture2d_f32";
    default:                     return "?";
    }
}

u32 mtk_type_components(MtkType t) {
    switch (t) {
    case MTK_TYPE_F32:
    case MTK_TYPE_I32:
    case MTK_TYPE_U32:
    case MTK_TYPE_BOOL:
    case MTK_TYPE_TEXTURE2D:
    case MTK_TYPE_TEXTURE2D_F32: return 1;
    case MTK_TYPE_VEC2:
    case MTK_TYPE_IVEC2:     return 2;
    case MTK_TYPE_VEC3:
    case MTK_TYPE_IVEC3:     return 3;
    case MTK_TYPE_VEC4:
    case MTK_TYPE_IVEC4:     return 4;
    case MTK_TYPE_MAT3:      return 9;
    case MTK_TYPE_MAT4:      return 16;
    default:                 return 0;
    }
}

b32 mtk_type_is_float(MtkType t) {
    return t == MTK_TYPE_F32 || t == MTK_TYPE_VEC2 || t == MTK_TYPE_VEC3 || t == MTK_TYPE_VEC4;
}

b32 mtk_type_is_int(MtkType t) {
    return t == MTK_TYPE_I32 || t == MTK_TYPE_U32 ||
           t == MTK_TYPE_IVEC2 || t == MTK_TYPE_IVEC3 || t == MTK_TYPE_IVEC4;
}

b32 mtk_type_is_vector(MtkType t) {
    return t == MTK_TYPE_VEC2 || t == MTK_TYPE_VEC3 || t == MTK_TYPE_VEC4 ||
           t == MTK_TYPE_IVEC2 || t == MTK_TYPE_IVEC3 || t == MTK_TYPE_IVEC4;
}

b32 mtk_type_is_matrix(MtkType t) {
    return t == MTK_TYPE_MAT3 || t == MTK_TYPE_MAT4;
}

b32 mtk_type_is_texture(MtkType t) {
    return t == MTK_TYPE_TEXTURE2D || t == MTK_TYPE_TEXTURE2D_F32;
}

MtkType mtk_type_scalar_base(MtkType t) {
    if (mtk_type_is_float(t) || mtk_type_is_matrix(t)) return MTK_TYPE_F32;
    if (t == MTK_TYPE_IVEC2 || t == MTK_TYPE_IVEC3 || t == MTK_TYPE_IVEC4) return MTK_TYPE_I32;
    return t;
}

MtkType mtk_type_make_vector(MtkType scalar_base, u32 comps) {
    if (comps <= 1) return scalar_base;
    if (scalar_base == MTK_TYPE_F32) {
        if (comps == 2) return MTK_TYPE_VEC2;
        if (comps == 3) return MTK_TYPE_VEC3;
        return MTK_TYPE_VEC4;
    }
    if (scalar_base == MTK_TYPE_I32 || scalar_base == MTK_TYPE_U32) {
        if (comps == 2) return MTK_TYPE_IVEC2;
        if (comps == 3) return MTK_TYPE_IVEC3;
        return MTK_TYPE_IVEC4;
    }
    return scalar_base;
}

/* ---------- Üst seviye derleme API'si ---------- */

b32 mtk_compile_ir(const char* src, u32 len, const char* filename, Arena* a,
                   IrFunc* out, MtkLayout* layout, MtkDiag* diag) {
    if (diag) mtk_diag_init(diag);

    MtkLayout local_layout;
    if (!layout) layout = &local_layout;
    __builtin_memset(layout, 0, sizeof(*layout));

    AstProgram prog;
    __builtin_memset(&prog, 0, sizeof(prog));

    if (!parser_parse(src, len, filename, a, &prog, diag)) {
        return FALSE;
    }

    if (!sema_check(&prog, src, len, filename, a, layout, diag)) {
        return FALSE;
    }

    if (!lower_ast(&prog, layout, a, out)) {
        return FALSE;
    }

    return TRUE;
}

u32 mtk_compile_wasm(const char* src, u32 len, const char* filename, const MtkCodegenOpts* o,
                     Arena* a, u8* out, u32 out_cap, MtkLayout* layout, MtkDiag* diag) {
    IrFunc ir;
    __builtin_memset(&ir, 0, sizeof(ir));

    if (!mtk_compile_ir(src, len, filename, a, &ir, layout, diag)) {
        return 0;
    }

    ir_optimize(&ir);

    MtkCodegenOpts default_opts = { FALSE, FALSE, MTK_FUSE_NONE };
    const MtkCodegenOpts* opts = o ? o : &default_opts;

    char err[256];
    err[0] = '\0';
    u32 bytes = mtk_codegen(&ir, opts, a, out, out_cap, err, sizeof(err));
    if (bytes == 0 && diag) {
        mtk_diag_error(diag, filename, src, len, 1, 1, "codegen hatasi: %s", err);
    }
    return bytes;
}
