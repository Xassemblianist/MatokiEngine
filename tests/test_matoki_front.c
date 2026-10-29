/* tests/test_matoki_front.c — Matoki ön uç (lexer, parser, sema, lower) testleri.
 * Sözcük akışı, 35+ hata durumu ve satır:sütun doğrulaması, yerleşim (MtkLayout),
 * IR doğrulama (ir_verify) ve ir_eval ile anlamsal doğruluk testlerini içerir.
 */
#include "test.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/math.h"
#include "matoki/lexer.h"
#include "matoki/parser.h"
#include "matoki/sema.h"
#include "matoki/lower.h"
#include "matoki/matoki.h"
#include "matoki/ir.h"
#include "matoki/codegen.h"
#include "raster/shade_abi.h"

static u8 g_test_arena_buf[MB(8)];

static u32 c_strlen(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static b32 str_contains(const char* hay, const char* needle) {
    u32 hlen = c_strlen(hay);
    u32 nlen = c_strlen(needle);
    if (nlen == 0) return TRUE;
    if (nlen > hlen) return FALSE;
    for (u32 i = 0; i + nlen <= hlen; i++) {
        b32 match = TRUE;
        for (u32 j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) { match = FALSE; break; }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

/* ---------- 1. Lexer Testleri ---------- */

TEST(matoki_lexer_tokens) {
    const char* src =
        "// satir yorumu\n"
        "/* blok\n"
        "   yorumu */ uniform tex: texture2d @bilinear @clamp;\n"
        "fn frag() -> rgba {\n"
        "    let a = 42 + 0x1F;\n"
        "    var b = 1.0 + .5 - 1e-3;\n"
        "    for i in 0..4 { b += f32(i); }\n"
        "    if a != 0 && true || !false { discard; }\n"
        "    return vec4(b);\n"
        "}\n";

    Lexer l;
    lexer_init(&l, src, c_strlen(src));

    Token t = lexer_next(&l);
    CHECK(t.kind == TOK_UNIFORM && t.line == 3 && t.col == 14);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* tex */
    t = lexer_next(&l); CHECK(t.kind == TOK_COLON);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* texture2d */
    t = lexer_next(&l); CHECK(t.kind == TOK_AT);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* bilinear */
    t = lexer_next(&l); CHECK(t.kind == TOK_AT);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* clamp */
    t = lexer_next(&l); CHECK(t.kind == TOK_SEMI);

    t = lexer_next(&l); CHECK(t.kind == TOK_FN && t.line == 4 && t.col == 1);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* frag */
    t = lexer_next(&l); CHECK(t.kind == TOK_LPAREN);
    t = lexer_next(&l); CHECK(t.kind == TOK_RPAREN);
    t = lexer_next(&l); CHECK(t.kind == TOK_ARROW);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT); /* rgba */
    t = lexer_next(&l); CHECK(t.kind == TOK_LBRACE);

    t = lexer_next(&l); CHECK(t.kind == TOK_LET);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT);
    t = lexer_next(&l); CHECK(t.kind == TOK_ASSIGN);
    t = lexer_next(&l); CHECK(t.kind == TOK_INT_LIT && t.i_val == 42);
    t = lexer_next(&l); CHECK(t.kind == TOK_PLUS);
    t = lexer_next(&l); CHECK(t.kind == TOK_INT_LIT && t.i_val == 0x1F);
    t = lexer_next(&l); CHECK(t.kind == TOK_SEMI);

    t = lexer_next(&l); CHECK(t.kind == TOK_VAR);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT);
    t = lexer_next(&l); CHECK(t.kind == TOK_ASSIGN);
    t = lexer_next(&l); CHECK(t.kind == TOK_FLOAT_LIT); CHECK_NEAR(t.f_val, 1.0f, 1e-6);
    t = lexer_next(&l); CHECK(t.kind == TOK_PLUS);
    t = lexer_next(&l); CHECK(t.kind == TOK_FLOAT_LIT); CHECK_NEAR(t.f_val, 0.5f, 1e-6);
    t = lexer_next(&l); CHECK(t.kind == TOK_MINUS);
    t = lexer_next(&l); CHECK(t.kind == TOK_FLOAT_LIT); CHECK_NEAR(t.f_val, 1e-3f, 1e-7);
    t = lexer_next(&l); CHECK(t.kind == TOK_SEMI);

    t = lexer_next(&l); CHECK(t.kind == TOK_FOR);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT);
    t = lexer_next(&l); CHECK(t.kind == TOK_IN);
    t = lexer_next(&l); CHECK(t.kind == TOK_INT_LIT && t.i_val == 0);
    t = lexer_next(&l); CHECK(t.kind == TOK_DOTDOT);
    t = lexer_next(&l); CHECK(t.kind == TOK_INT_LIT && t.i_val == 4);
    t = lexer_next(&l); CHECK(t.kind == TOK_LBRACE);
    t = lexer_next(&l); CHECK(t.kind == TOK_IDENT);
    t = lexer_next(&l); CHECK(t.kind == TOK_PLUS_ASSIGN);

    while (t.kind != TOK_EOF && t.kind != TOK_ERROR) {
        t = lexer_next(&l);
    }
    CHECK(t.kind == TOK_EOF);
}

/* ---------- 2. Hata Durumları ve Satır:Sütun Doğrulaması (35+ durum) ---------- */

typedef struct {
    const char* name;
    const char* src;
    u32         line;
    u32         col;
} ErrorCase;

static void check_error_case(const ErrorCase* ec) {
    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    b32 ok = mtk_compile_ir(ec->src, c_strlen(ec->src), ec->name, &arena, &ir, &layout, &diag);
    CHECK(!ok);
    CHECK(diag.count >= 1);

    char expected_loc[128];
    fmt_format(expected_loc, sizeof(expected_loc), "%s:%u:%u: hata:", ec->name, ec->line, ec->col);
    if (!str_contains(diag.text, expected_loc)) {
        LOG("HATA BEKLENTISI UYUSMADI (%s):\n  Beklenen: %s\n  Gelen:\n%s",
            ec->name, expected_loc, diag.text);
    }
    CHECK(str_contains(diag.text, expected_loc));
}

TEST(matoki_error_cases_and_locations) {
    static const ErrorCase cases[] = {
        /* 1. Geçersiz karakter */
        { "err01.mtk", "fn frag() -> rgba {\n    let a = $1.0;\n    return vec4(a);\n}\n", 2, 13 },
        /* 2. Geçersiz hex literali */
        { "err02.mtk", "fn frag() -> rgba {\n    let a = 0xZZ;\n    return vec4(1.0);\n}\n", 2, 13 },
        /* 3. Geçersiz üstel sayı literali */
        { "err03.mtk", "fn frag() -> rgba {\n    let a = 1.5e+;\n    return vec4(1.0);\n}\n", 2, 13 },
        /* 4. Sayı sonuna bitişik harf */
        { "err04.mtk", "fn frag() -> rgba {\n    let a = 42abc;\n    return vec4(1.0);\n}\n", 2, 13 },
        /* 5. Kapatılmamış blok yorumu */
        { "err05.mtk", "fn frag() -> rgba {\n    return vec4(1.0);\n}\n  /* acik yorum", 4, 3 },
        /* 6. Üst seviyede geçersiz bildirim */
        { "err06.mtk", "let g = 1.0;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 1 },
        /* 7. Uniform bildiriminde ':' eksik */
        { "err07.mtk", "uniform u vec4;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 11 },
        /* 8. Bilinmeyen tip ismi */
        { "err08.mtk", "uniform u: float4;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 12 },
        /* 9. texture2d olmayan uniform üzerinde @bilinear */
        { "err09.mtk", "uniform u: vec4 @bilinear;\nfn frag() -> rgba { return u; }\n", 1, 18 },
        /* 10. Bilinmeyen örnekleme niteliği */
        { "err10.mtk", "uniform t: texture2d @anisotropic;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 23 },
        /* 11. 'in' bildiriminde ';' eksik */
        { "err11.mtk", "in uv: vec2\nfn frag() -> rgba { return vec4(1.0); }\n", 2, 1 },
        /* 12. Değişken tanımında '=' eksik */
        { "err12.mtk", "fn frag() -> rgba {\n    let x: f32 1.0;\n    return vec4(x);\n}\n", 2, 16 },
        /* 13. Deyim sonunda ';' eksik */
        { "err13.mtk", "fn frag() -> rgba {\n    let x = 1.0\n    return vec4(x);\n}\n", 3, 5 },
        /* 14. if koşulundan sonra '{' eksik */
        { "err14.mtk", "fn frag() -> rgba {\n    if true return vec4(1.0);\n}\n", 2, 13 },
        /* 15. Fonksiyon çağrısında ')' eksik */
        { "err15.mtk", "fn frag() -> rgba {\n    let x = sin(1.0;\n    return vec4(x);\n}\n", 2, 20 },
        /* 16. for döngüsünde '..' eksik */
        { "err16.mtk", "fn frag() -> rgba {\n    for i in 0, 4 {}\n    return vec4(1.0);\n}\n", 2, 15 },
        /* 17. '.' sonrasında bileşen ismi eksik */
        { "err17.mtk", "fn frag() -> rgba {\n    let v = vec2(1.0, 2.0);\n    let x = v.;\n    return vec4(x);\n}\n", 3, 15 },
        /* 18. Mükerrer üst seviye bildirim */
        { "err18.mtk", "uniform c: vec4;\nuniform c: f32;\nfn frag() -> rgba { return vec4(1.0); }\n", 2, 9 },
        /* 19. Varying girdi tipi texture2d olamaz */
        { "err19.mtk", "in bad_in: texture2d;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 12 },
        /* 20. Varying girdi tipi i32 olamaz */
        { "err20.mtk", "in bad_in: i32;\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 12 },
        /* 21. const bildiriminde varying ifade */
        { "err21.mtk", "in uv: vec2;\nconst C: f32 = uv.x;\nfn frag() -> rgba { return vec4(1.0); }\n", 2, 7 },
        /* 22. Giriş noktası 'frag' eksik */
        { "err22.mtk", "fn helper(x: f32) -> f32 { return x; }\n", 1, 1 },
        /* 23. 'frag' parametre alamaz */
        { "err23.mtk", "fn frag(x: f32) -> rgba { return vec4(x); }\n", 1, 4 },
        /* 24. 'frag' dönüş tipi rgba olmalıdır */
        { "err24.mtk", "fn frag() -> f32 { return 1.0; }\n", 1, 14 },
        /* 25. Tanımlanmamış değişken ismi */
        { "err25.mtk", "fn frag() -> rgba {\n    let x = olmayan;\n    return vec4(x);\n}\n", 2, 13 },
        /* 26. Bilinmeyen fonksiyon çağrısı */
        { "err26.mtk", "fn frag() -> rgba {\n    let x = foo_bar(1.0);\n    return vec4(x);\n}\n", 2, 13 },
        /* 27. Örtük dönüşüm yok: f32 değişkene tamsayı literali atanamaz */
        { "err27.mtk", "fn frag() -> rgba {\n    let x: f32 = 1;\n    return vec4(x);\n}\n", 2, 9 },
        /* 28. İkili operatörde tip uyuşmazlığı (f32 + i32) */
        { "err28.mtk", "fn frag() -> rgba {\n    let x = 1.0 + 2;\n    return vec4(x);\n}\n", 2, 17 },
        /* 29. if koşulu bool olmalıdır */
        { "err29.mtk", "fn frag() -> rgba {\n    if 1.0 {}\n    return vec4(1.0);\n}\n", 2, 8 },
        /* 30. Skaler tip üzerinde swizzle yapılamaz */
        { "err30.mtk", "fn frag() -> rgba {\n    let x = 1.0;\n    let y = x.xy;\n    return vec4(1.0);\n}\n", 3, 15 },
        /* 31. Swizzle bileşeni vektör boyutunu aşıyor (vec2 üzerinde .z) */
        { "err31.mtk", "in uv: vec2;\nfn frag() -> rgba {\n    let z = uv.z;\n    return vec4(z);\n}\n", 3, 16 },
        /* 32. Karışık swizzle kümeleri (.xr) */
        { "err32.mtk", "in c: vec4;\nfn frag() -> rgba {\n    let v = c.xr;\n    return vec4(1.0);\n}\n", 3, 15 },
        /* 33. 4 bileşenden uzun swizzle (.xyzwx) */
        { "err33.mtk", "in c: vec4;\nfn frag() -> rgba {\n    let v = c.xyzwx;\n    return vec4(1.0);\n}\n", 3, 15 },
        /* 34. 'let' ile tanımlanan değişkene atama yapılamaz */
        { "err34.mtk", "fn frag() -> rgba {\n    let x = 1.0;\n    x = 2.0;\n    return vec4(x);\n}\n", 3, 7 },
        /* 35. Swizzle sol tarafında tekrarlı bileşen (v.xx = ...) */
        { "err35.mtk", "fn frag() -> rgba {\n    var v = vec4(1.0);\n    v.xx = vec2(2.0, 3.0);\n    return v;\n}\n", 3, 7 },
        /* 36. Yerleşik fonksiyonda yanlış argüman sayısı */
        { "err36.mtk", "fn frag() -> rgba {\n    let x = clamp(1.0, 0.0);\n    return vec4(x);\n}\n", 2, 13 },
        /* 37. sample() ilk argümanı texture2d olmalı */
        { "err37.mtk", "in uv: vec2;\nfn frag() -> rgba {\n    return sample(uv, uv);\n}\n", 3, 19 },
        /* 38. Doğrudan özyineleme (recursion) tespiti */
        { "err38.mtk", "fn loop_fn(x: f32) -> f32 {\n    return loop_fn(x);\n}\nfn frag() -> rgba {\n    return vec4(loop_fn(1.0));\n}\n", 2, 12 },
        /* 39. Dolaylı (karşılıklı) özyineleme tespiti */
        { "err39.mtk", "fn fa(x: f32) -> f32 { return fb(x); }\nfn fb(x: f32) -> f32 { return fa(x); }\nfn frag() -> rgba { return vec4(fa(1.0)); }\n", 2, 31 },
        /* 40. Değer döndürmeyen fonksiyon */
        { "err40.mtk", "fn broken(x: f32) -> f32 {\n    let y = x + 1.0;\n}\nfn frag() -> rgba { return vec4(1.0); }\n", 1, 4 },
        /* 41. for döngüsü sınırı sabit değil */
        { "err41.mtk", "uniform n: i32;\nfn frag() -> rgba {\n    for i in 0..n {}\n    return vec4(1.0);\n}\n", 3, 5 },
    };

    for (u32 i = 0; i < ARRAY_LEN(cases); i++) {
        check_error_case(&cases[i]);
    }

    /* Çoklu hata kurtarma testi: ilk hatada durmamalı */
    const char* multi_err =
        "fn frag() -> rgba {\n"
        "    let a: f32 = 1;\n"
        "    let b = bilinmeyen;\n"
        "    let c = 1.0 + 2;\n"
        "    return vec4(1.0);\n"
        "}\n";
    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;
    b32 ok = mtk_compile_ir(multi_err, c_strlen(multi_err), "multi.mtk", &arena, &ir, &layout, &diag);
    CHECK(!ok);
    CHECK(diag.count == 3);
}

/* ---------- 3. IR Doğrulama ve Yerleşim Testleri ---------- */

TEST(matoki_layout_and_ir_verify) {
    const char* src =
        "uniform u_time: f32;\n"
        "uniform u_tint: vec4;\n"
        "uniform u_tex0: texture2d;\n"
        "uniform u_mat:  mat4;\n"
        "uniform u_tex1: texture2d @bilinear @clamp;\n"
        "in v_uv: vec2;\n"
        "in v_col: vec3;\n"
        "fn frag() -> rgba {\n"
        "    let s0 = sample(u_tex0, v_uv);\n"
        "    let s1 = sample(u_tex1, v_uv);\n"
        "    let p  = u_mat * vec4(v_col, u_time);\n"
        "    return (s0 + s1 + p) * u_tint;\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    b32 ok = mtk_compile_ir(src, c_strlen(src), "layout_test.mtk", &arena, &ir, &layout, &diag);
    CHECK(ok);
    CHECK(diag.count == 0);

    /* u_time (0..16), u_tint (16..32), u_mat (32..96) -> toplam 96 byte */
    CHECK(layout.uniform_bytes == 96);
    /* v_uv (2) + v_col (3) = 5 skaler varying */
    CHECK(layout.varying_count == 5);
    /* u_tex0 (slot 0), u_tex1 (slot 1) = 2 doku */
    CHECK(layout.texture_count == 2);
    CHECK(layout.sym_count == 7);

    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));
    ir_optimize(&ir);
    CHECK(ir_verify(&ir, err, sizeof(err)));
}

/* ---------- 4. Anlamsal Doğruluk Testleri (ir_eval ile) ---------- */

TEST(matoki_eval_helpers_loops_and_math) {
    const char* src =
        "uniform u_scale: f32;\n"
        "in v_uv: vec2;\n"
        "const STEPS: i32 = 4;\n"
        "\n"
        "fn boost(v: vec2, s: f32) -> vec2 {\n"
        "    return normalize(v) * s;\n"
        "}\n"
        "\n"
        "fn frag() -> rgba {\n"
        "    var acc = 0.0;\n"
        "    for i in 0..STEPS {\n"
        "        acc += f32(i) * 0.1;\n"
        "    }\n"
        "    let b = boost(v_uv, u_scale);\n"
        "    let m = mix(0.2, 0.8, smoothstep(0.0, 1.0, 0.5));\n"
        "    let p = pow(2.0, 3.0) * 0.1;\n"
        "    return vec4(b.x + acc, b.y, m, p);\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(src, c_strlen(src), "math_eval.mtk", &arena, &ir, &layout, &diag));
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    QuadHeader q = { 0xFu, 0, 0 };
    f32 varyings[2 * 4] = {
        3.0f, 3.0f, 3.0f, 3.0f, /* v_uv.x */
        4.0f, 4.0f, 4.0f, 4.0f, /* v_uv.y */
    };
    _Alignas(16) u8 uniforms[16];
    __builtin_memset(uniforms, 0, sizeof(uniforms));
    f32 u_scale = 5.0f;
    __builtin_memcpy(uniforms, &u_scale, sizeof(f32));

    f32 out_rgba[4 * 4];
    __builtin_memset(out_rgba, 0, sizeof(out_rgba));

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = 1;
    batch.varying_count = 2;
    batch.quads         = &q;
    batch.varyings      = varyings;
    batch.uniforms      = uniforms;
    batch.out_rgba      = out_rgba;

    ir_eval(&ir, &batch);

    /* normalize(3,4) * 5 = (3, 4); acc = (0 + 1 + 2 + 3)*0.1 = 0.6 -> r = 3.6, g = 4.0 */
    /* smoothstep(0,1,0.5) = 0.5 -> mix(0.2,0.8,0.5) = 0.5; pow(2,3)*0.1 = 0.8 */
    for (u32 lane = 0; lane < 4; lane++) {
        CHECK_NEAR(out_rgba[0 * 4 + lane], 3.6f, 1e-5);
        CHECK_NEAR(out_rgba[1 * 4 + lane], 4.0f, 1e-5);
        CHECK_NEAR(out_rgba[2 * 4 + lane], 0.5f, 1e-5);
        CHECK_NEAR(out_rgba[3 * 4 + lane], 0.8f, 1e-4);
    }
}

TEST(matoki_eval_sprite_and_branchy) {
    /* 1. branchy.mtk: iç içe varying if/else ve discard */
    const char* branchy_src =
        "uniform threshold: f32;\n"
        "in uv: vec2;\n"
        "fn frag() -> rgba {\n"
        "    var col = vec4(0.1, 0.2, 0.3, 1.0);\n"
        "    if uv.x > 0.5 {\n"
        "        if uv.y > 0.5 {\n"
        "            if uv.x + uv.y > threshold {\n"
        "                discard;\n"
        "            }\n"
        "            col = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "        } else {\n"
        "            col.g = uv.x;\n"
        "        }\n"
        "    } else {\n"
        "        if uv.y > 0.5 {\n"
        "            col.b = uv.y;\n"
        "        } else {\n"
        "            col = vec4(uv.x, uv.y, 0.5, 1.0);\n"
        "        }\n"
        "    }\n"
        "    return col;\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(branchy_src, c_strlen(branchy_src), "branchy.mtk", &arena, &ir, &layout, &diag));
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    /* 4 lane için farklı dallar:
     * lane 0: uv=(0.2, 0.3) -> sol-alt: col=(0.2, 0.3, 0.5, 1.0)
     * lane 1: uv=(0.2, 0.8) -> sol-üst: col=(0.1, 0.2, 0.8, 1.0)
     * lane 2: uv=(0.7, 0.3) -> sağ-alt: col=(0.1, 0.7, 0.3, 1.0)
     * lane 3: uv=(0.9, 0.9) -> sağ-üst ve 1.8 > 1.5 -> DISCARD! */
    QuadHeader q = { 0xFu, 0, 0 };
    f32 varyings[2 * 4] = {
        0.2f, 0.2f, 0.7f, 0.9f, /* uv.x */
        0.3f, 0.8f, 0.3f, 0.9f, /* uv.y */
    };
    _Alignas(16) u8 uniforms[16];
    __builtin_memset(uniforms, 0, sizeof(uniforms));
    f32 thresh = 1.5f;
    __builtin_memcpy(uniforms, &thresh, sizeof(f32));

    f32 out_rgba[4 * 4];
    __builtin_memset(out_rgba, 0, sizeof(out_rgba));

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = 1;
    batch.varying_count = 2;
    batch.quads         = &q;
    batch.varyings      = varyings;
    batch.uniforms      = uniforms;
    batch.out_rgba      = out_rgba;

    ir_eval(&ir, &batch);

    /* Lane 3 discard edildiği için maske 0x7 olmalı */
    CHECK(q.mask == 0x7u);
    /* Lane 0: (0.2, 0.3, 0.5, 1.0) */
    CHECK_NEAR(out_rgba[0 * 4 + 0], 0.2f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 0], 0.3f, 1e-5);
    CHECK_NEAR(out_rgba[2 * 4 + 0], 0.5f, 1e-5);
    /* Lane 1: (0.1, 0.2, 0.8, 1.0) */
    CHECK_NEAR(out_rgba[0 * 4 + 1], 0.1f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 1], 0.2f, 1e-5);
    CHECK_NEAR(out_rgba[2 * 4 + 1], 0.8f, 1e-5);
    /* Lane 2: (0.1, 0.7, 0.3, 1.0) */
    CHECK_NEAR(out_rgba[0 * 4 + 2], 0.1f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 2], 0.7f, 1e-5);
    CHECK_NEAR(out_rgba[2 * 4 + 2], 0.3f, 1e-5);
}

TEST(matoki_eval_texture_sampling_and_discard) {
    /* sprite.mtk: sample(tex, uv) + if c.a < 0.01 { discard; } + return c * tint */
    const char* sprite_src =
        "uniform tint: vec4;\n"
        "uniform tex:  texture2d;\n"
        "in uv: vec2;\n"
        "fn frag() -> rgba {\n"
        "    let c = sample(tex, uv);\n"
        "    if c.a < 0.01 {\n"
        "        discard;\n"
        "    }\n"
        "    return c * tint;\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(sprite_src, c_strlen(sprite_src), "sprite.mtk", &arena, &ir, &layout, &diag));
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    /* 2x2 RGBA8 doku (0xAABBGGRR):
     * (0,0) = tam kırmızı (255,0,0,255)
     * (1,0) = tam yeşil   (0,255,0,255)
     * (0,1) = saydam      (0,0,0,0) -> discard edilmeli!
     * (1,1) = tam beyaz   (255,255,255,255) */
    u32 tex_px[4] = {
        0xFF0000FFu,
        0xFF00FF00u,
        0x00000000u,
        0xFFFFFFFFu,
    };
    Texture tex = { tex_px, 2, 2, 1, 1, 1, 0, 0 };

    QuadHeader q = { 0xFu, 0, 0 };
    f32 varyings[2 * 4] = {
        0.25f, 0.75f, 0.25f, 0.75f, /* u */
        0.25f, 0.25f, 0.75f, 0.75f, /* v */
    };
    _Alignas(16) f32 tint[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
    f32 out_rgba[4 * 4];
    __builtin_memset(out_rgba, 0, sizeof(out_rgba));

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = 1;
    batch.varying_count = 2;
    batch.quads         = &q;
    batch.varyings      = varyings;
    batch.uniforms      = (const u8*)tint;
    batch.out_rgba      = out_rgba;
    batch.textures      = &tex;

    ir_eval(&ir, &batch);

    /* Lane 2 (0,1) saydam olduğu için discard edilmeli -> mask = 0b1011 = 0xB */
    CHECK(q.mask == 0xBu);
    /* Lane 0 (0,0): (1,0,0,1) * (0.5,0.5,1,1) = (0.5, 0, 0, 1) */
    CHECK_NEAR(out_rgba[0 * 4 + 0], 0.5f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 0], 0.0f, 1e-5);
    CHECK_NEAR(out_rgba[3 * 4 + 0], 1.0f, 1e-5);
    /* Lane 1 (1,0): (0,1,0,1) * (0.5,0.5,1,1) = (0, 0.5, 0, 1) */
    CHECK_NEAR(out_rgba[0 * 4 + 1], 0.0f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 1], 0.5f, 1e-5);
    CHECK_NEAR(out_rgba[3 * 4 + 1], 1.0f, 1e-5);
    /* Lane 3 (1,1): (1,1,1,1) * (0.5,0.5,1,1) = (0.5, 0.5, 1, 1) */
    CHECK_NEAR(out_rgba[0 * 4 + 3], 0.5f, 1e-5);
    CHECK_NEAR(out_rgba[1 * 4 + 3], 0.5f, 1e-5);
    CHECK_NEAR(out_rgba[2 * 4 + 3], 1.0f, 1e-5);
    CHECK_NEAR(out_rgba[3 * 4 + 3], 1.0f, 1e-5);
}

TEST(matoki_eval_plasma_matches_c_math) {
    /* shaders/plasma.mtk ile birebir aynı kaynak */
    const char* plasma_src =
        "uniform time: f32;\n"
        "in uv: vec2;\n"
        "\n"
        "const PI: f32 = 3.14159265;\n"
        "\n"
        "fn wave(p: vec2, t: f32) -> f32 {\n"
        "    let v1 = sin(p.x * 10.0 + t);\n"
        "    let v2 = cos(p.y * 10.0 - t * 0.5);\n"
        "    let v3 = sin((p.x + p.y) * 7.0 + t * 0.7);\n"
        "    let cx = p.x - 0.5;\n"
        "    let cy = p.y - 0.5;\n"
        "    let d  = sqrt(cx * cx + cy * cy);\n"
        "    let v4 = cos(d * 14.0 - t * 1.3);\n"
        "    return (v1 + v2 + v3 + v4) * 0.25;\n"
        "}\n"
        "\n"
        "fn frag() -> rgba {\n"
        "    let w = wave(uv, time);\n"
        "    let r = sin(w * PI) * 0.5 + 0.5;\n"
        "    let g = cos(w * PI + 1.0) * 0.5 + 0.5;\n"
        "    let b = sin(w * PI + 2.0) * 0.5 + 0.5;\n"
        "    return vec4(r, g, b, 1.0);\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(plasma_src, c_strlen(plasma_src), "plasma.mtk", &arena, &ir, &layout, &diag));
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    /* Optimizasyon sonrası da aynı sonucu vermeli */
    ir_optimize(&ir);
    CHECK(ir_verify(&ir, err, sizeof(err)));

    QuadHeader q = { 0xFu, 0, 0 };
    f32 u_vals[4] = { 0.15f, 0.45f, 0.75f, 0.92f };
    f32 v_vals[4] = { 0.25f, 0.60f, 0.10f, 0.85f };
    f32 varyings[2 * 4] = {
        u_vals[0], u_vals[1], u_vals[2], u_vals[3],
        v_vals[0], v_vals[1], v_vals[2], v_vals[3],
    };

    _Alignas(16) u8 uniforms[16];
    __builtin_memset(uniforms, 0, sizeof(uniforms));
    f32 time_val = 1.234f;
    __builtin_memcpy(uniforms, &time_val, sizeof(f32));

    f32 out_rgba[4 * 4];
    __builtin_memset(out_rgba, 0, sizeof(out_rgba));

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = 1;
    batch.varying_count = 2;
    batch.quads         = &q;
    batch.varyings      = varyings;
    batch.uniforms      = uniforms;
    batch.out_rgba      = out_rgba;

    ir_eval(&ir, &batch);

    const f32 pi = 3.14159265f;
    for (u32 lane = 0; lane < 4; lane++) {
        f32 px = u_vals[lane];
        f32 py = v_vals[lane];
        f32 v1 = m_sin(px * 10.0f + time_val);
        f32 v2 = m_cos(py * 10.0f - time_val * 0.5f);
        f32 v3 = m_sin((px + py) * 7.0f + time_val * 0.7f);
        f32 cx = px - 0.5f;
        f32 cy = py - 0.5f;
        f32 d  = m_sqrt(cx * cx + cy * cy);
        f32 v4 = m_cos(d * 14.0f - time_val * 1.3f);
        f32 w  = (v1 + v2 + v3 + v4) * 0.25f;

        f32 ref_r = m_sin(w * pi) * 0.5f + 0.5f;
        f32 ref_g = m_cos(w * pi + 1.0f) * 0.5f + 0.5f;
        f32 ref_b = m_sin(w * pi + 2.0f) * 0.5f + 0.5f;

        /* 7. derece tek-polinom minimax sin/cos yaklaşımı için beklenen hata sınırı <= 2e-4 */
        CHECK_NEAR(out_rgba[0 * 4 + lane], ref_r, 2e-4);
        CHECK_NEAR(out_rgba[1 * 4 + lane], ref_g, 2e-4);
        CHECK_NEAR(out_rgba[2 * 4 + lane], ref_b, 2e-4);
        CHECK_NEAR(out_rgba[3 * 4 + lane], 1.0f,  1e-5);
    }

    /* Tek-polinom sin/cos açılımında kadran seçimi için IR_SELECT kullanılmamalı */
    for (u32 i = 0; i < ir.count; i++) {
        CHECK(ir.insts[i].op != IR_SELECT);
    }
}

TEST(matoki_compile_wasm_all_shaders) {
    const char* src =
        "uniform tint: vec4;\n"
        "in v_color: vec4;\n"
        "fn frag() -> rgba {\n"
        "    return v_color * tint;\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    u8 wasm_buf[16384];
    MtkLayout layout;
    MtkDiag diag;

    u32 wasm_len = mtk_compile_wasm(src, c_strlen(src), "vcolor.mtk", NULL, &arena,
                                    wasm_buf, sizeof(wasm_buf), &layout, &diag);
    CHECK(wasm_len > 8);
    CHECK(diag.count == 0);
    CHECK(wasm_buf[0] == 0x00 && wasm_buf[1] == 0x61 &&
          wasm_buf[2] == 0x73 && wasm_buf[3] == 0x6D);
}

TEST(matoki_trig_single_poly_accuracy) {
    const char* src =
        "uniform phase: f32;\n"
        "in x: f32;\n"
        "fn frag() -> rgba {\n"
        "    return vec4(sin(x), cos(x), cos(x - phase), sin(x + phase));\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(src, c_strlen(src), "trig.mtk", &arena, &ir, &layout, &diag));
    ir_optimize(&ir);

    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));
    for (u32 i = 0; i < ir.count; i++) {
        CHECK(ir.insts[i].op != IR_SELECT);
    }

    enum { NQ = 64 };
    QuadHeader quads[NQ];
    f32 varyings[NQ * 4];
    f32 out_rgba[4 * NQ * 4];
    for (u32 q = 0; q < NQ; q++) {
        quads[q].mask = 0xFu;
        quads[q].x    = (u16)(q * 2);
        quads[q].y    = 0;
        for (u32 l = 0; l < 4; l++) {
            u32 idx = q * 4 + l;
            f32 t = (f32)idx / (f32)(NQ * 4 - 1);
            varyings[idx] = (t * 2.0f - 1.0f) * 31.4159265f; /* [-10*pi, 10*pi] */
        }
    }

    _Alignas(16) u8 uniforms[16];
    __builtin_memset(uniforms, 0, sizeof(uniforms));
    f32 phase = 0.78539816f;
    __builtin_memcpy(uniforms, &phase, sizeof(f32));

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = NQ;
    batch.varying_count = 1;
    batch.quads         = quads;
    batch.varyings      = varyings;
    batch.uniforms      = uniforms;
    batch.out_rgba      = out_rgba;

    ir_eval(&ir, &batch);

    for (u32 q = 0; q < NQ; q++) {
        for (u32 l = 0; l < 4; l++) {
            u32 idx = q * 4 + l;
            f32 x = varyings[idx];
            CHECK_NEAR(out_rgba[(0 * NQ + q) * 4 + l], m_sin(x),         2e-4);
            CHECK_NEAR(out_rgba[(1 * NQ + q) * 4 + l], m_cos(x),         2e-4);
            CHECK_NEAR(out_rgba[(2 * NQ + q) * 4 + l], m_cos(x - phase), 2e-4);
            CHECK_NEAR(out_rgba[(3 * NQ + q) * 4 + l], m_sin(x + phase), 2e-4);
        }
    }
}

TEST(matoki_texture2d_f32_and_sample_depth) {
    const char* src =
        "uniform sh_near: texture2d_f32 @clamp;\n"
        "uniform sh_bili: texture2d_f32 @bilinear @clamp;\n"
        "in uv: vec2;\n"
        "fn frag() -> rgba {\n"
        "    let d0 = sample_depth(sh_near, uv);\n"
        "    let d1 = sample_depth(sh_bili, uv);\n"
        "    let sz = tex_size(sh_near);\n"
        "    return vec4(d0, d1, f32(sz.x), f32(sz.y));\n"
        "}\n";

    Arena arena;
    arena_init(&arena, g_test_arena_buf, sizeof(g_test_arena_buf));
    IrFunc ir;
    MtkLayout layout;
    MtkDiag diag;

    CHECK(mtk_compile_ir(src, c_strlen(src), "shadow_f32.mtk", &arena, &ir, &layout, &diag));
    CHECK(diag.count == 0);
    CHECK(layout.texture_count == 2);
    CHECK(layout.syms[0].kind == MTK_SYM_TEXTURE);
    CHECK(layout.syms[0].type == MTK_TYPE_TEXTURE2D_F32);
    CHECK(layout.syms[1].kind == MTK_SYM_TEXTURE);
    CHECK(layout.syms[1].type == MTK_TYPE_TEXTURE2D_F32);

    ir_optimize(&ir);
    char err[256];
    CHECK(ir_verify(&ir, err, sizeof(err)));

    /* 4x4 f32 derinlik dokusu: d(x, y) = 0.1 * x + 0.2 * y + 0.05 */
    f32 depth_buf[16];
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 4; x++) {
            depth_buf[y * 4 + x] = 0.1f * (f32)x + 0.2f * (f32)y + 0.05f;
        }
    }

    Texture texs[2];
    __builtin_memset(texs, 0, sizeof(texs));
    texs[0].px = (u32*)depth_buf;
    texs[0].w = 4; texs[0].h = 4;
    texs[0].w_mask = 3; texs[0].h_mask = 3; texs[0].log2_w = 2;
    texs[0].flags = TEX_FLAG_CLAMP | TEX_FLAG_F32;
    texs[0].levels = 1;

    texs[1] = texs[0];
    texs[1].flags = TEX_FLAG_CLAMP | TEX_FLAG_BILINEAR | TEX_FLAG_F32;

    QuadHeader q = { 0xFu, 0, 0 };
    /* Lane 0: (0.125, 0.125) -> texel (0,0) = 0.05
     * Lane 1: (0.5, 0.5)     -> nearest texel (2,2) = 0.65; bilinear merkez (1..2, 1..2) ort = 0.50
     * Lane 2: (-0.5, 2.0)    -> clamp texel (0,3) = 0.65
     * Lane 3: (0.875, 0.375) -> texel (3,1) = 0.55 */
    f32 varyings[8] = {
        0.125f, 0.5f, -0.5f, 0.875f,
        0.125f, 0.5f,  2.0f, 0.375f
    };
    f32 out_rgba[16];

    ShadeBatch batch;
    __builtin_memset(&batch, 0, sizeof(batch));
    batch.count         = 1;
    batch.varying_count = 2;
    batch.quads         = &q;
    batch.varyings      = varyings;
    batch.out_rgba      = out_rgba;
    batch.textures      = texs;

    ir_eval(&ir, &batch);

    /* Nearest + clamp sonuçları (kanal 0) */
    CHECK_NEAR(out_rgba[0 * 4 + 0], 0.05f, 1e-6);
    CHECK_NEAR(out_rgba[0 * 4 + 1], 0.65f, 1e-6);
    CHECK_NEAR(out_rgba[0 * 4 + 2], 0.65f, 1e-6);
    CHECK_NEAR(out_rgba[0 * 4 + 3], 0.55f, 1e-6);

    /* Bilinear + clamp sonuçları (kanal 1) */
    CHECK_NEAR(out_rgba[1 * 4 + 0], 0.05f, 1e-6);
    CHECK_NEAR(out_rgba[1 * 4 + 1], 0.50f, 1e-6);
    CHECK_NEAR(out_rgba[1 * 4 + 2], 0.65f, 1e-6);
    CHECK_NEAR(out_rgba[1 * 4 + 3], 0.55f, 1e-6);

    /* tex_size(sh_near) = (4, 4) */
    CHECK_NEAR(out_rgba[2 * 4 + 0], 4.0f, 1e-6);
    CHECK_NEAR(out_rgba[3 * 4 + 0], 4.0f, 1e-6);

    /* Negatif sema testleri: sample/sample_depth tip uyuşmazlığı ve @mipmap yasağı */
    const char* bad1 = "uniform s: texture2d_f32;\nin uv: vec2;\nfn frag() -> rgba { return sample(s, uv); }\n";
    const char* bad2 = "uniform t: texture2d;\nin uv: vec2;\nfn frag() -> rgba { return vec4(sample_depth(t, uv)); }\n";
    const char* bad3 = "uniform s: texture2d_f32 @mipmap;\nfn frag() -> rgba { return vec4(1.0); }\n";
    CHECK(!mtk_compile_ir(bad1, c_strlen(bad1), "b1.mtk", &arena, &ir, &layout, &diag) && diag.count > 0);
    CHECK(!mtk_compile_ir(bad2, c_strlen(bad2), "b2.mtk", &arena, &ir, &layout, &diag) && diag.count > 0);
    CHECK(!mtk_compile_ir(bad3, c_strlen(bad3), "b3.mtk", &arena, &ir, &layout, &diag) && diag.count > 0);
}



