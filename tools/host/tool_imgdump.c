/* tools/host/tool_imgdump.c — Prosedürel dokuları, 8x8/8x16 font tablosunu ve örnek UI ekranını
 * kendi sıfırdan PNG kodlayıcımızla (src/img/png.c) diske yazan native komut satırı aracı.
 * Kullanım: ./build/host/imgdump <out_dir> [--bench]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/profiler.h"
#include "core/log.h"
#include "plat/platform.h"
#include "raster/framebuffer.h"
#include "img/noise.h"
#include "img/procgen.h"
#include "img/image.h"
#include "img/png.h"
#include "ui/font.h"
#include "ui/ui.h"

static b32 save_png_file(const char* out_dir, const char* filename,
                         const u32* px, u32 w, u32 h, Arena* arena) {
    usize mark = arena_mark(arena);
    usize png_len = 0;
    u8* png_bytes = png_encode(px, w, h, &png_len, arena);
    if (!png_bytes) {
        fprintf(stderr, "HATA: PNG kodlanamadi: %s\n", filename);
        arena_pop_to(arena, mark);
        return FALSE;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", out_dir, filename);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "HATA: Dosya acilamadi: %s\n", path);
        arena_pop_to(arena, mark);
        return FALSE;
    }
    usize written = fwrite(png_bytes, 1, png_len, f);
    fclose(f);
    arena_pop_to(arena, mark);

    if (written != png_len) return FALSE;
    printf("  [OK] %-26s (%4ux%-4u -> %7zu bayt)\n", filename, w, h, png_len);
    return TRUE;
}

static void render_font_sheet(Framebuffer* fb) {
    fb_clear(fb, FB_RGBA(16, 20, 28, 255));
    fb_rect(fb, 0, 0, (i32)fb->w, 32, FB_RGBA(28, 38, 54, 255));
    font_draw_8x16(fb, 16, 8, "MATOKI BITMAP FONT TABLOSU (8x8 & 8x16 + TURKCE UTF-8)",
                   FB_RGBA(88, 214, 156, 255), 1);

    font_draw(fb, 16, 44, "1. 8x8 ASCII (32..126) - Olcek 2x:", FB_RGBA(180, 195, 220, 255), 1);
    for (u32 cp = 32; cp <= 126; cp++) {
        u32 idx = cp - 32;
        i32 col = (i32)(idx % 16);
        i32 row = (i32)(idx / 16);
        i32 gx = 20 + col * 36;
        i32 gy = 62 + row * 22;

        fb_rect(fb, gx - 2, gy - 2, 20, 20, FB_RGBA(24, 30, 42, 255));
        char s[2] = { (char)cp, 0 };
        font_draw(fb, gx, gy, s, FB_RGBA(240, 244, 252, 255), 2);
    }

    font_draw(fb, 16, 204, "2. Turkce Karakterler (8x8 @ 2x ve 8x16 @ 2x):",
              FB_RGBA(180, 195, 220, 255), 1);
    const char* tr_sample = "\xC3\xA7 \xC3\x87 \xC4\x9F \xC4\x9E \xC4\xB1 \xC4\xB0 "
                            "\xC3\xB6 \xC3\x96 \xC5\x9F \xC5\x9E \xC3\xBC \xC3\x9C";
    font_draw(fb, 20, 222, tr_sample, FB_RGBA(255, 216, 102, 255), 2);
    font_draw_8x16(fb, 20, 246, tr_sample, FB_RGBA(110, 220, 255, 255), 2);

    font_draw(fb, 16, 290, "3. Pangram & Ornek Metinler:", FB_RGBA(180, 195, 220, 255), 1);
    font_draw(fb, 20, 308,
              "Pijamali hasta yagiz sofore cabukucak guvendi.",
              FB_RGBA(220, 230, 245, 255), 1);
    font_draw_8x16(fb, 20, 324,
                   "Pijamal\xC4\xB1 hasta ya\xC4\x9F\xC4\xB1z \xC5\x9Fof\xC3\xB6re \xC3\xA7" "abucak g\xC3\xBCvendi!",
                   FB_RGBA(255, 255, 255, 255), 1);
    font_draw(fb, 20, 348,
              "SIYAH EKRANDA PARLAYAN P\xC4\xB0KSELLER: 0123456789 +-*/=()[]{}<>@#$%&",
              FB_RGBA(140, 230, 170, 255), 1);
}

static void render_ui_demo(Framebuffer* fb, Arena* arena) {
    usize mark = arena_mark(arena);

    /* Arka plan olarak döşenmiş taş + su kostiği karışımı çiz */
    Texture bg_stone, bg_caustic;
    procgen_stone(&bg_stone, arena, 128, 128, 42u, NULL);
    procgen_caustic(&bg_caustic, arena, 128, 128, 77u, NULL);

    for (u32 y = 0; y < fb->h; y++) {
        for (u32 x = 0; x < fb->w; x++) {
            u32 cs = bg_stone.px[(y & 127u) * 128u + (x & 127u)];
            u32 cc = bg_caustic.px[(y & 127u) * 128u + (x & 127u)];
            u32 r = (((cs & 0xFFu) * 3u + (cc & 0xFFu)) >> 2u);
            u32 g = ((((cs >> 8) & 0xFFu) * 3u + ((cc >> 8) & 0xFFu)) >> 2u);
            u32 b = ((((cs >> 16) & 0xFFu) * 3u + ((cc >> 16) & 0xFFu)) >> 2u);
            fb->px[y * fb->w + x] = FB_RGBA(r / 2u, g / 2u, b / 2u, 255u);
        }
    }

    /* Örnek profiler verisi oluştur */
    prof_reset();
    prof_begin("raster_tiles");
    prof_begin("shade_quads");
    prof_end();
    prof_end();
    prof_begin("procgen_tex");
    prof_end();
    prof_begin("ui_overlay");
    prof_end();
    prof_frame_end();

    UiContext ui;
    ui_init(&ui, fb);

    /* 120 karelik örnek frame süresi geçmişi doldur */
    for (u32 i = 0; i < 120; i++) {
        f32 ms = 11.5f + 3.2f * m_sin((f32)i * 0.18f) + ((i % 29 == 0) ? 7.5f : 0.0f);
        ui_record_frame_time(&ui, ms);
    }

    InputState inp;
    memset(&inp, 0, sizeof(inp));
    inp.mouse_x = 120;
    inp.mouse_y = 148;

    b32 wireframe = FALSE;
    b32 tile_grid = TRUE;
    f32 exposure  = 1.25f;
    i32 threads   = 4;

    ui_begin(&ui, &inp);

    ui_panel_begin(&ui, "MATOKI MOTOR KONTROLU", 16, 16, 280, 328);
    ui_label(&ui, "Surum: M7_0 (Freestanding Wasm32)");
    ui_separator(&ui);
    ui_checkbox(&ui, "Tel Kafes (Wireframe)", &wireframe);
    ui_checkbox(&ui, "Karo Izgarasi (64x64)", &tile_grid);
    ui_slider_f32(&ui, "Pozlama (EV)", &exposure, 0.2f, 3.0f);
    ui_slider_i32(&ui, "Worker Sayisi", &threads, 1, 16);
    ui_button(&ui, "Shader JIT Yeniden Derle");
    ui_separator(&ui);
    ui_frame_graph(&ui, "Kare Suresi", 33.3f);
    ui_panel_end(&ui);

    ui_panel_begin(&ui, "PROFILER & BELLEK", 312, 16, 312, 210);
    ui_profiler_table(&ui);
    ui_separator(&ui);
    ui_label(&ui, "Arena: 14.2 MB / 256.0 MB (%5.5)");
    ui_label(&ui, "Durum: 60 FPS Kararli (SIMD128 Aktif)");
    ui_panel_end(&ui);

    ui_end(&ui);
    arena_pop_to(arena, mark);
}

static f64 now_sec(void) {
    return plat_time_ms() * 1e-3;
}

static void run_benchmark(const char* out_dir, Arena* arena) {
    usize mark = arena_mark(arena);
    u32 w = 512, h = 512;
    usize raw_bytes = (usize)w * (usize)h * 4u;

    Texture tex;
    procgen_marble(&tex, arena, w, h, 1337u, NULL);

    usize png_len = 0;
    f64 t0 = now_sec();
    i32 enc_iters = 10;
    u8* png_data = NULL;
    for (i32 i = 0; i < enc_iters; i++) {
        usize m2 = arena_mark(arena);
        png_data = png_encode(tex.px, w, h, &png_len, arena);
        if (i + 1 < enc_iters) arena_pop_to(arena, m2);
    }
    f64 t1 = now_sec();
    f64 enc_mb_s = ((f64)raw_bytes * enc_iters / (1024.0 * 1024.0)) / (t1 - t0);

    u32 dw = 0, dh = 0;
    f64 t2 = now_sec();
    i32 dec_iters = 25;
    for (i32 i = 0; i < dec_iters; i++) {
        usize m2 = arena_mark(arena);
        u32* dec = png_decode(png_data, png_len, &dw, &dh, arena);
        (void)dec;
        arena_pop_to(arena, m2);
    }
    f64 t3 = now_sec();
    f64 dec_mb_s = ((f64)raw_bytes * dec_iters / (1024.0 * 1024.0)) / (t3 - t2);

    char raw_path[512];
    snprintf(raw_path, sizeof(raw_path), "%s/_bench_512.raw", out_dir);
    FILE* f = fopen(raw_path, "wb");
    if (f) {
        fwrite(tex.px, 1, raw_bytes, f);
        fclose(f);
    }

    printf("\n=== 512x512 PROSEDUREL DOKU PNG BENCHMARK ===\n");
    printf("  Ham RGBA8 Boyutu    : %zu bayt (1.00 MB)\n", raw_bytes);
    printf("  MATOKI PNG Boyutu   : %zu bayt (oran: %.2f%%, %.2fx)\n",
           png_len, 100.0 * (f64)png_len / (f64)raw_bytes, (f64)raw_bytes / (f64)png_len);
    printf("  Kodlama Hizi (Enc)  : %.2f MB/s\n", enc_mb_s);
    printf("  Cozme Hizi   (Dec)  : %.2f MB/s\n", dec_mb_s);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "python3 -c \""
             "import zlib; "
             "d = open('%s','rb').read(); "
             "c = zlib.compress(d, 6); "
             "print('  Python zlib(lvl 6)  : %%d bayt (oran: %%.2f%%%%, %%.2fx)' %% (len(c), 100.0*len(c)/len(d), len(d)/len(c)))"
             "\"", raw_path);
    fflush(stdout);
    int rc = system(cmd);
    (void)rc;
    remove(raw_path);

    arena_pop_to(arena, mark);
}

int main(int argc, char** argv) {
    const char* out_dir = (argc >= 2) ? argv[1] : "out";
    b32 do_bench = (argc >= 3 && strcmp(argv[2], "--bench") == 0);

    mkdir(out_dir, 0755);

    usize mem_size = MB(64);
    void* mem = malloc(mem_size);
    if (!mem) return 1;
    Arena arena;
    arena_init(&arena, mem, mem_size);

    printf("MATOKI imgdump -> %s/\n", out_dir);

    u32 tex_w = 256, tex_h = 256;
    Texture tex;

    procgen_brick(&tex, &arena, tex_w, tex_h, 101u, NULL);
    save_png_file(out_dir, "proc_brick.png", tex.px, tex.w, tex.h, &arena);

    /* Tuğla yüksekliğinden normal map örneği de kaydet */
    Texture norm_tex;
    img_tex_init(&norm_tex, &arena, tex_w, tex_h, 0);
    img_normal_from_height(tex.px, norm_tex.px, tex_w, tex_h, 4.0f, TRUE);
    save_png_file(out_dir, "proc_brick_normal.png", norm_tex.px, norm_tex.w, norm_tex.h, &arena);

    procgen_wood(&tex, &arena, tex_w, tex_h, 202u, NULL);
    save_png_file(out_dir, "proc_wood.png", tex.px, tex.w, tex.h, &arena);

    procgen_marble(&tex, &arena, tex_w, tex_h, 303u, NULL);
    save_png_file(out_dir, "proc_marble.png", tex.px, tex.w, tex.h, &arena);

    procgen_grass(&tex, &arena, tex_w, tex_h, 404u, NULL);
    save_png_file(out_dir, "proc_grass.png", tex.px, tex.w, tex.h, &arena);

    procgen_stone(&tex, &arena, tex_w, tex_h, 505u, NULL);
    save_png_file(out_dir, "proc_stone.png", tex.px, tex.w, tex.h, &arena);

    procgen_metal(&tex, &arena, tex_w, tex_h, 606u, NULL);
    save_png_file(out_dir, "proc_metal.png", tex.px, tex.w, tex.h, &arena);

    procgen_lava(&tex, &arena, tex_w, tex_h, 707u, NULL);
    save_png_file(out_dir, "proc_lava.png", tex.px, tex.w, tex.h, &arena);

    procgen_caustic(&tex, &arena, tex_w, tex_h, 808u, NULL);
    save_png_file(out_dir, "proc_caustic.png", tex.px, tex.w, tex.h, &arena);

    procgen_cloud(&tex, &arena, tex_w, tex_h, 909u, NULL);
    save_png_file(out_dir, "proc_cloud.png", tex.px, tex.w, tex.h, &arena);

    Framebuffer fb;
    fb_init(&fb, &arena, 640, 368);
    render_font_sheet(&fb);
    save_png_file(out_dir, "font_sheet.png", fb.px, fb.w, fb.h, &arena);

    Framebuffer ui_fb;
    fb_init(&ui_fb, &arena, 640, 360);
    render_ui_demo(&ui_fb, &arena);
    save_png_file(out_dir, "ui_demo.png", ui_fb.px, ui_fb.w, ui_fb.h, &arena);

    if (do_bench) {
        run_benchmark(out_dir, &arena);
    }

    free(mem);
    return 0;
}
