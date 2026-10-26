/* tests/test_img.c — Görüntü işleme, prosedürel gürültü/doku, bitmap font, UI ve PNG testleri.
 * PNG/DEFLATE akışlarımızı Python zlib ile çift yönlü (compress <-> decompress) çapraz doğrular.
 * Tüm gürültü fonksiyonlarının determinizmini, değer aralıklarını ve döşenebilirliğini sınar.
 */
#include "test.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/math.h"
#include "core/profiler.h"
#include "raster/framebuffer.h"
#include "img/noise.h"
#include "img/procgen.h"
#include "img/image.h"
#include "img/png.h"
#include "ui/font.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 g_test_buf[16 * 1024 * 1024];

TEST(noise_determinism_and_range) {
    for (i32 i = 0; i < 200; i++) {
        f32 x = (f32)i * 0.37f - 12.4f;
        f32 y = (f32)i * 0.71f + 3.1f;
        f32 z = (f32)i * 0.19f - 5.0f;

        f32 v2a = noise_value2(x, y, 42u);
        f32 v2b = noise_value2(x, y, 42u);
        CHECK(v2a == v2b);
        CHECK(v2a >= 0.0f && v2a <= 1.0f);

        f32 v3a = noise_value3(x, y, z, 42u);
        CHECK(v3a == noise_value3(x, y, z, 42u));
        CHECK(v3a >= 0.0f && v3a <= 1.0f);

        f32 p2 = noise_perlin2(x, y, 99u);
        CHECK(p2 == noise_perlin2(x, y, 99u));
        CHECK(p2 >= -1.0f && p2 <= 1.0f);

        f32 p3 = noise_perlin3(x, y, z, 99u);
        CHECK(p3 == noise_perlin3(x, y, z, 99u));
        CHECK(p3 >= -1.0f && p3 <= 1.0f);

        f32 s2 = noise_simplex2(x, y, 123u);
        CHECK(s2 == noise_simplex2(x, y, 123u));
        CHECK(s2 >= -1.0f && s2 <= 1.0f);

        f32 f2 = 0.0f;
        f32 w1 = noise_worley2(x, y, 777u, &f2);
        CHECK(w1 == noise_worley2(x, y, 777u, NULL));
        CHECK(w1 >= 0.0f && w1 <= 1.0f);
        CHECK(f2 >= w1 && f2 <= 1.0f);

        f32 fbm = noise_fbm2(x, y, 4, 2.0f, 0.5f, 555u);
        CHECK(fbm >= -1.0f && fbm <= 1.0f);

        f32 wrp = noise_warp2(x, y, 0.5f, 3, 888u);
        CHECK(wrp >= -1.0f && wrp <= 1.0f);
    }
}

TEST(noise_tileability) {
    i32 px = 8, py = 8, pz = 4;
    for (i32 i = 0; i < 50; i++) {
        f32 x = (f32)i * 0.23f;
        f32 y = (f32)i * 0.41f;
        f32 z = (f32)i * 0.17f;

        /* Value 2D tile */
        CHECK_NEAR(noise_value2_tile(x, y, px, py, 10u),
                   noise_value2_tile(x + (f32)px, y, px, py, 10u), 1e-5);
        CHECK_NEAR(noise_value2_tile(x, y, px, py, 10u),
                   noise_value2_tile(x, y + (f32)py, px, py, 10u), 1e-5);

        /* Perlin 2D tile */
        CHECK_NEAR(noise_perlin2_tile(x, y, px, py, 20u),
                   noise_perlin2_tile(x + (f32)px, y + (f32)py, px, py, 20u), 1e-5);

        /* Perlin 3D tile */
        CHECK_NEAR(noise_perlin3_tile(x, y, z, px, py, pz, 30u),
                   noise_perlin3_tile(x + (f32)px, y + (f32)py, z + (f32)pz, px, py, pz, 30u), 1e-5);

        /* Simplex 2D tile */
        CHECK_NEAR(noise_simplex2_tile(x, y, (f32)px, (f32)py, 40u),
                   noise_simplex2_tile(x + (f32)px, y + (f32)py, (f32)px, (f32)py, 40u), 1e-4);

        /* Worley 2D tile */
        CHECK_NEAR(noise_worley2_tile(x, y, px, py, 50u, NULL),
                   noise_worley2_tile(x + (f32)px, y + (f32)py, px, py, 50u, NULL), 1e-5);

        /* fBm & Warp 2D tile */
        CHECK_NEAR(noise_fbm2_tile(x, y, px, py, 4, 2.0f, 0.5f, 60u),
                   noise_fbm2_tile(x + (f32)px, y + (f32)py, px, py, 4, 2.0f, 0.5f, 60u), 1e-5);
        CHECK_NEAR(noise_warp2_tile(x, y, px, py, 0.4f, 3, 70u),
                   noise_warp2_tile(x + (f32)px, y + (f32)py, px, py, 0.4f, 3, 70u), 1e-5);
    }
}

TEST(procgen_and_image_ops) {
    Arena a;
    arena_init(&a, g_test_buf, sizeof(g_test_buf));

    Texture t_brick, t_wood, t_marble, t_grass, t_stone, t_metal, t_lava, t_caustic, t_cloud;
    procgen_brick(&t_brick, &a, 64, 64, 1u, NULL);
    procgen_wood(&t_wood, &a, 64, 64, 2u, NULL);
    procgen_marble(&t_marble, &a, 64, 64, 3u, NULL);
    procgen_grass(&t_grass, &a, 64, 64, 4u, NULL);
    procgen_stone(&t_stone, &a, 64, 64, 5u, NULL);
    procgen_metal(&t_metal, &a, 64, 64, 6u, NULL);
    procgen_lava(&t_lava, &a, 64, 64, 7u, NULL);
    procgen_caustic(&t_caustic, &a, 64, 64, 8u, NULL);
    procgen_cloud(&t_cloud, &a, 64, 64, 9u, NULL);

    /* Determinizm: aynı seed ile üretilen doku birebir aynı olmalı */
    Texture t_brick2;
    procgen_brick(&t_brick2, &a, 64, 64, 1u, NULL);
    for (u32 i = 0; i < 64 * 64; i++) {
        CHECK(t_brick.px[i] == t_brick2.px[i]);
    }

    /* Mipmap zinciri: 64x64 -> 7 seviye (64, 32, 16, 8, 4, 2, 1) */
    Texture mips[8];
    u32 lvls = img_mipmap_build(&t_brick, mips, 8, &a);
    CHECK(lvls == 7u);
    CHECK(mips[6].w == 1u && mips[6].h == 1u);

    /* Bulanıklık, yeniden boyutlandırma, normal map, renk düzeltme */
    img_blur_box(t_wood.px, 64, 64, 2, &a);
    img_blur_gaussian(t_marble.px, 64, 64, 3, 1.5f, &a);

    u32* resized = ARENA_PUSH_ARRAY(&a, u32, 32 * 32);
    img_resize_bilinear(t_grass.px, 64, 64, resized, 32, 32);
    img_resize_box(t_stone.px, 64, 64, resized, 32, 32);

    u32* norm = ARENA_PUSH_ARRAY(&a, u32, 64 * 64);
    img_normal_from_height(t_brick.px, norm, 64, 64, 3.0f, TRUE);
    /* Normal map Z kanalı pozitif (B > 127) olmalı */
    CHECK(((norm[0] >> 16) & 0xFFu) >= 128u);

    img_color_grade(t_lava.px, 64, 64, 0.05f, 1.1f, 1.2f, 1.0f);
    img_premultiply_alpha(t_cloud.px, 64, 64);
}

TEST(font_glyphs_and_utf8) {
    /* Tüm ASCII 32..126 glifleri tanımlı ve boşluk (32) dışındaki tüm glifler en az 1 dolu piksele sahip olmalı */
    for (u32 cp = 32; cp <= 126; cp++) {
        CHECK(font_has_glyph(cp));
        const u8* g8  = font_glyph_8x8(cp);
        const u8* g16 = font_glyph_8x16(cp);
        CHECK(g8 != NULL && g16 != NULL);
        if (cp != 32) {
            u32 sum8 = 0, sum16 = 0;
            for (i32 r = 0; r < 8;  r++) sum8  += g8[r];
            for (i32 r = 0; r < 16; r++) sum16 += g16[r];
            CHECK(sum8 > 0 && sum16 > 0);
        }
    }

    /* 12 Türkçe karakterin tamamı tanımlı ve dolu olmalı */
    const u32 tr_cps[12] = {
        FONT_CP_C_CEDILLA_LOWER, FONT_CP_C_CEDILLA_UPPER,
        FONT_CP_G_BREVE_LOWER,   FONT_CP_G_BREVE_UPPER,
        FONT_CP_I_DOTLESS_LOWER, FONT_CP_I_DOTTED_UPPER,
        FONT_CP_O_UMLAUT_LOWER,  FONT_CP_O_UMLAUT_UPPER,
        FONT_CP_S_CEDILLA_LOWER, FONT_CP_S_CEDILLA_UPPER,
        FONT_CP_U_UMLAUT_LOWER,  FONT_CP_U_UMLAUT_UPPER
    };
    for (u32 i = 0; i < 12; i++) {
        CHECK(font_has_glyph(tr_cps[i]));
        const u8* g8  = font_glyph_8x8(tr_cps[i]);
        const u8* g16 = font_glyph_8x16(tr_cps[i]);
        u32 sum8 = 0, sum16 = 0;
        for (i32 r = 0; r < 8;  r++) sum8  += g8[r];
        for (i32 r = 0; r < 16; r++) sum16 += g16[r];
        CHECK(sum8 > 0 && sum16 > 0);
    }

    /* UTF-8 Türkçe dizi çözme */
    const char* tr_str = "\xC3\xA7\xC3\x87\xC4\x9F\xC4\x9E\xC4\xB1\xC4\xB0"
                         "\xC3\xB6\xC3\x96\xC5\x9F\xC5\x9E\xC3\xBC\xC3\x9C";
    const char* p = tr_str;
    for (u32 i = 0; i < 12; i++) {
        u32 cp = font_utf8_next(&p);
        CHECK(cp == tr_cps[i]);
    }
    CHECK(font_utf8_next(&p) == 0u);

    /* UTF-8 kenar durumları: yalnız devam baytı, kesik dizi, overlong 2-bayt, surrogate */
    const char* bad1 = "\x80" "A";
    p = bad1;
    CHECK(font_utf8_next(&p) == 0xFFFDu);
    CHECK(font_utf8_next(&p) == 'A');

    const char* bad_overlong = "\xC0\xAF" "B";
    p = bad_overlong;
    CHECK(font_utf8_next(&p) == 0xFFFDu);
    /* İkinci bayt da devam baytı olduğundan 0xFFFD döner, sonra 'B' gelir */
    CHECK(font_utf8_next(&p) == 0xFFFDu);
    CHECK(font_utf8_next(&p) == 'B');

    const char* bad_trunc = "\xE2\x82";
    p = bad_trunc;
    CHECK(font_utf8_next(&p) == 0xFFFDu);
    CHECK(font_utf8_next(&p) == 0u);

    /* Ölçüm ve çizim testi */
    CHECK(font_measure_width("MATOKI", 2) == 6 * 16);
    CHECK(font_measure_height("A\nB", 2) == 2 * 16);

    Arena a;
    arena_init(&a, g_test_buf, sizeof(g_test_buf));
    Framebuffer fb;
    fb_init(&fb, &a, 128, 64);
    fb_clear(&fb, 0);
    font_draw(&fb, 4, 4, "Şğüıİöç", FB_RGBA(255, 255, 255, 255), 1);

    UiContext ui;
    ui_init(&ui, &fb);
    InputState inp;
    memset(&inp, 0, sizeof(inp));
    ui_begin(&ui, &inp);
    ui_panel_begin(&ui, "Test", 2, 2, 100, 50);
    ui_label(&ui, "Merhaba");
    b32 flag = TRUE;
    ui_checkbox(&ui, "Secim", &flag);
    ui_panel_end(&ui);
    ui_end(&ui);
}

TEST(png_roundtrip_dimensions_1_to_300) {
    Arena a;
    arena_init(&a, g_test_buf, sizeof(g_test_buf));
    Rng rng;
    rng_seed(&rng, 0xCAFEBABEULL);

    /* 1..300 arasındaki uç, asal ve rastgele boyutlarda kodla -> çöz birebir doğrulama */
    const u32 dims[][2] = {
        {1, 1}, {1, 300}, {300, 1}, {2, 3}, {7, 13}, {16, 16},
        {31, 47}, {64, 64}, {100, 75}, {127, 129}, {255, 199}, {300, 300}
    };

    for (u32 d = 0; d < ARRAY_LEN(dims); d++) {
        usize mark = arena_mark(&a);
        u32 w = dims[d][0];
        u32 h = dims[d][1];
        u32* src = ARENA_PUSH_ARRAY(&a, u32, (usize)w * (usize)h);

        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                /* Hem gradyan/desen (filtre + LZ77 için) hem rastgele gürültü */
                if ((x + y) & 1u) {
                    src[y * w + x] = FB_RGBA((x * 7u) & 0xFFu, (y * 11u) & 0xFFu, ((x ^ y) * 5u) & 0xFFu, 255u);
                } else {
                    src[y * w + x] = rng_u32(&rng);
                }
            }
        }

        usize png_len = 0;
        u8* png = png_encode(src, w, h, &png_len, &a);
        CHECK(png != NULL && png_len > 33);

        u32 dw = 0, dh = 0;
        u32* dec = png_decode(png, png_len, &dw, &dh, &a);
        CHECK(dec != NULL);
        CHECK(dw == w && dh == h);

        b32 match = (memcmp(src, dec, (usize)w * (usize)h * sizeof(u32)) == 0);
        CHECK(match);

        arena_pop_to(&a, mark);
    }
}

TEST(png_python_zlib_cross_validation) {
    Arena a;
    arena_init(&a, g_test_buf, sizeof(g_test_buf));
    Rng rng;
    rng_seed(&rng, 0x12345678ULL);

    /* 1) Bizim DEFLATE ve ZLIB çıktımızı Python'un zlib.decompress'i açabilmeli */
    usize data_len = 16384;
    u8* orig = ARENA_PUSH_ARRAY(&a, u8, data_len);
    for (usize i = 0; i < data_len; i++) {
        orig[i] = (i % 7 < 4) ? (u8)(i & 0x3Fu) : (u8)(rng_u32(&rng) & 0xFFu);
    }

    usize z_len = 0;
    u8* z_stream = png_zlib_compress(orig, data_len, &z_len, &a);
    CHECK(z_stream != NULL && z_len > 6);

    const char* tmp_orig = "/tmp/matoki_test_orig.bin";
    const char* tmp_zlib = "/tmp/matoki_test_zlib.bin";
    FILE* fo = fopen(tmp_orig, "wb");
    FILE* fz = fopen(tmp_zlib, "wb");
    CHECK(fo != NULL && fz != NULL);
    fwrite(orig, 1, data_len, fo);
    fwrite(z_stream, 1, z_len, fz);
    fclose(fo);
    fclose(fz);

    int rc = system("python3 -c \""
                    "import zlib, sys; "
                    "orig = open('/tmp/matoki_test_orig.bin','rb').read(); "
                    "comp = open('/tmp/matoki_test_zlib.bin','rb').read(); "
                    "dec = zlib.decompress(comp); "
                    "sys.exit(0 if dec == orig else 1)\"");
    CHECK(rc == 0);

    /* 2) Python'un zlib.compress(level 0..9) ile ürettiği tüm akışları bizim inflate açabilmeli */
    for (int lvl = 0; lvl <= 9; lvl++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "python3 -c \""
                 "import zlib; "
                 "orig = open('/tmp/matoki_test_orig.bin','rb').read(); "
                 "open('/tmp/matoki_test_py.z','wb').write(zlib.compress(orig, %d))\"",
                 lvl);
        CHECK(system(cmd) == 0);

        FILE* fp = fopen("/tmp/matoki_test_py.z", "rb");
        CHECK(fp != NULL);
        fseek(fp, 0, SEEK_END);
        usize py_len = (usize)ftell(fp);
        fseek(fp, 0, SEEK_SET);

        usize m = arena_mark(&a);
        u8* py_buf = ARENA_PUSH_ARRAY(&a, u8, py_len);
        CHECK(fread(py_buf, 1, py_len, fp) == py_len);
        fclose(fp);

        usize dec_len = 0;
        u8* dec = png_zlib_decompress(py_buf, py_len, &dec_len, &a);
        CHECK(dec != NULL);
        CHECK(dec_len == data_len);
        CHECK(memcmp(dec, orig, data_len) == 0);

        arena_pop_to(&a, m);
    }

    /* 3) Kodladığımız PNG dosyasını Python üzerinden IHDR/IDAT/IEND ve zlib ile doğrula */
    Texture tex;
    procgen_wood(&tex, &a, 128, 128, 77u, NULL);
    usize png_len = 0;
    u8* png_bytes = png_encode(tex.px, 128, 128, &png_len, &a);
    CHECK(png_bytes != NULL);

    FILE* fpng = fopen("/tmp/matoki_test.png", "wb");
    CHECK(fpng != NULL);
    fwrite(png_bytes, 1, png_len, fpng);
    fclose(fpng);

    int rc_png = system(
        "python3 -c \""
        "import struct, zlib\n"
        "d = open('/tmp/matoki_test.png','rb').read()\n"
        "assert d[:8] == b'\\x89PNG\\r\\n\\x1a\\n'\n"
        "pos = 8\n"
        "idat = b''\n"
        "while pos < len(d):\n"
        "  l = struct.unpack('>I', d[pos:pos+4])[0]\n"
        "  t = d[pos+4:pos+8]\n"
        "  c = d[pos+8:pos+8+l]\n"
        "  crc = struct.unpack('>I', d[pos+8+l:pos+12+l])[0]\n"
        "  assert (zlib.crc32(t + c) & 0xffffffff) == crc\n"
        "  if t == b'IDAT': idat += c\n"
        "  pos += 12 + l\n"
        "raw = zlib.decompress(idat)\n"
        "assert len(raw) == 128 * (1 + 128 * 4)\n"
        "\"");
    CHECK(rc_png == 0);

    /* 4) Python ile üretilen RGB (color_type=2), Gri (0) ve Paletli (3) PNG'leri bizim çözücü açabilmeli */
    int rc_types = system(
        "python3 -c \""
        "import struct, zlib\n"
        "def mk_png(w, h, ct, raw, plte=None):\n"
        "  def ch(t, d):\n"
        "    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d)&0xffffffff)\n"
        "  out = b'\\x89PNG\\r\\n\\x1a\\n'\n"
        "  out += ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, ct, 0, 0, 0))\n"
        "  if plte: out += ch(b'PLTE', plte)\n"
        "  out += ch(b'IDAT', zlib.compress(raw))\n"
        "  out += ch(b'IEND', b'')\n"
        "  return out\n"
        "# Gri (ct=0) 4x4: her piksel x*10+y\n"
        "raw0 = b''.join(b'\\x00' + bytes([x*10+y for x in range(4)]) for y in range(4))\n"
        "open('/tmp/matoki_ct0.png','wb').write(mk_png(4, 4, 0, raw0))\n"
        "# RGB (ct=2) 4x4\n"
        "raw2 = b''.join(b'\\x00' + b''.join(bytes([x*20, y*30, 99]) for x in range(4)) for y in range(4))\n"
        "open('/tmp/matoki_ct2.png','wb').write(mk_png(4, 4, 2, raw2))\n"
        "# Paletli (ct=3) 4x4\n"
        "plte = bytes([255,0,0, 0,255,0, 0,0,255, 255,255,0])\n"
        "raw3 = b''.join(b'\\x00' + bytes([(x+y)&3 for x in range(4)]) for y in range(4))\n"
        "open('/tmp/matoki_ct3.png','wb').write(mk_png(4, 4, 3, raw3, plte))\n"
        "\"");
    CHECK(rc_types == 0);

    const char* ct_files[3] = { "/tmp/matoki_ct0.png", "/tmp/matoki_ct2.png", "/tmp/matoki_ct3.png" };
    for (i32 k = 0; k < 3; k++) {
        FILE* f = fopen(ct_files[k], "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END);
        usize flen = (usize)ftell(f);
        fseek(f, 0, SEEK_SET);
        u8* fbuf = ARENA_PUSH_ARRAY(&a, u8, flen);
        CHECK(fread(fbuf, 1, flen, f) == flen);
        fclose(f);

        u32 cw = 0, ch = 0;
        u32* cpx = png_decode(fbuf, flen, &cw, &ch, &a);
        CHECK(cpx != NULL && cw == 4u && ch == 4u);
        if (k == 0) CHECK(cpx[0] == FB_RGBA(0, 0, 0, 255) && cpx[1] == FB_RGBA(10, 10, 10, 255));
        if (k == 1) CHECK(cpx[1] == FB_RGBA(20, 0, 99, 255));
        if (k == 2) CHECK(cpx[0] == FB_RGBA(255, 0, 0, 255) && cpx[1] == FB_RGBA(0, 255, 0, 255));
        remove(ct_files[k]);
    }

    remove(tmp_orig);
    remove(tmp_zlib);
    remove("/tmp/matoki_test_py.z");
    remove("/tmp/matoki_test.png");
}
