/* app/main.c — Entegrasyon demosu: 5 sahne, JIT vs C referans, canlı Matoki editörü,
 * F1 debug overlay (FPS grafiği, profiler tablosu, JIT istatistikleri) ve F12 PNG ekran görüntüsü.
 * Sahneler (Tab): 1) 10k Sprite, 2) Plasma (tam ekran), 3) Branchy, 4) Canlı Shader Editörü, 5) 3D,
 * 6) Platform oyunu, 7) Ocak, 8) Kuyu (retro FPS).
 */
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/embed.h"
#include "core/simd.h"
#include "plat/platform.h"
#include "plat/input.h"
#include "raster/framebuffer.h"
#include "raster/texture.h"
#include "raster/raster.h"
#include "raster/ref_shaders.h"
#include "gfx2d/batch2d.h"
#include "img/png.h"
#include "ui/font.h"
#include "ui/ui.h"
#include "ui/editor.h"
#include "app/shaders.h"
#include "app/scene3d.h"
#include "app/r3d.h"
#include "asset/json.h"
#include "gfx3d/pipeline.h"
#include "app/scene_platformer.h"
#include "app/scene_ocak.h"
#include "app/scene_kuyu.h"
#include "audio/audio.h"
#include "thread/thread.h"
#include "thread/job.h"
#include "masm/masm.h"
#include "jit/jit.h"
#include "bench/asm_sources.h"
#include "spec/spec.h"

#define FB_DEFAULT_W 640
#define FB_DEFAULT_H 360
#define MAX_SPRITES  100000
#define TEX_COUNT    3

/* Tuş kodları */
#define KEY_TAB   9
#define KEY_1     49
#define KEY_9     57
#define KEY_B     66
#define KEY_J     74
#define KEY_P     80
#define KEY_T     84
#define KEY_F1    112
#define KEY_F12   123

typedef enum {
    SCENE_SPRITES = 0,
    SCENE_PLASMA  = 1,
    SCENE_BRANCHY = 2,
    SCENE_EDITOR  = 3,
    SCENE_3D      = 4,
    SCENE_GAME    = 5,
    SCENE_OCAK    = 6,
    SCENE_KUYU    = 7,
    SCENE_COUNT   = 8
} DemoScene;

typedef struct {
    f32 x, y;
    f32 vx, vy;
    f32 rot;
    f32 vrot;
    f32 size;
} DemoSprite;

extern u8 __heap_base;

static Arena         g_perm_arena;
static Arena         g_frame_arena;
static Arena         g_asset_arena;    /* sürükle-bırak dosyaları + yüklenen model (her grupta sıfırlanır) */
static Framebuffer   g_fb;
static InputState    g_input;
static Rasterizer    g_raster;
static Batch2D       g_batch;
static UiContext     g_ui;
static ShaderManager g_shaders;
static CodeEditor    g_editor;

static Texture       g_textures[TEX_COUNT];
static DemoSprite*   g_sprites;
static u32           g_sprite_count;
static BlendMode     g_blend_mode;

static DemoScene     g_scene;
static b32           g_use_jit;
static b32           g_show_debug;
static b32           g_pointer_locked;   /* JS fare kilidi durumu (engine_set_pointer_lock) */
static u32           g_caps_mask;        /* Platform yetenek bit maskesi (engine_set_caps) */
static u32           g_caps_gpu_max_tex2d;
static u32           g_caps_gpu_max_buf_mb;

/* Canlı editör önizleme shader durumu */
static ManagedShader    g_live_shader;
static b32              g_has_live_shader;

static RasterWatEdgeBlockFn  g_wat_edge_fn;
static RasterWatBlendAlphaFn g_wat_blend_fn;
static u32                   g_wat_raster_mode;

/* Sahne başına ve mod (0=C Ref, 1=JIT) başına üstel hareketli ortalama (EMA) frame süresi (ms) */
static f32           g_mode_ms[SCENE_COUNT][2];
static f32           g_mode_shade_ms[SCENE_COUNT][2];
static f32           g_cur_frame_ms;

static u8            g_prev_keys[256];
static f64           g_fps_last_ms;
static u32           g_fps_frames;
static u64           g_accum_pixels;
static f32           g_Screenshot_flash;

static const u32 k_sprite_counts[9] = {
    1000, 2500, 5000, 10000, 20000, 35000, 50000, 75000, 100000
};

static const char* scene_name(DemoScene s) {
    switch (s) {
    case SCENE_SPRITES: return "1:10k Sprite";
    case SCENE_PLASMA:  return "2:Plasma";
    case SCENE_BRANCHY: return "3:Branchy";
    case SCENE_EDITOR:  return "4:Editor";
    case SCENE_3D:      return "5:3D";
    case SCENE_GAME:    return "6:Oyun";
    case SCENE_OCAK:    return "7:Ocak";
    case SCENE_KUYU:    return "8:Kuyu";
    default:            return "?";
    }
}

static const char* blend_mode_name(BlendMode m) {
    switch (m) {
    case BLEND_NONE:  return "NONE";
    case BLEND_ALPHA: return "ALPHA";
    case BLEND_ADD:   return "ADD";
    }
    return "?";
}

static b32 key_pressed(u32 code) {
    return g_input.keys[code] && !g_prev_keys[code];
}

static b32 str_eq(const char* a, const char* b) {
    if (!a || !b) return FALSE;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

EXPORT(engine_set_wat_raster_mode) void engine_set_wat_raster_mode(u32 mode) {
    g_wat_raster_mode = mode;
    RasterWatEdgeBlockFn  ef = (mode & 1u) ? g_wat_edge_fn  : NULL;
    RasterWatBlendAlphaFn bf = (mode & 2u) ? g_wat_blend_fn : NULL;
    raster_set_wat_kernels(ef, bf);
}

static void init_wat_raster_kernels(void) {
    g_wat_edge_fn  = NULL;
    g_wat_blend_fn = NULL;

    usize mark = arena_mark(&g_frame_arena);
    const EmbeddedAsm* ea_edge = bench_find_asm("edge_block_8x8");
    if (ea_edge) {
        MasmResult res_edge = masm_assemble(&g_frame_arena, ea_edge->src, ea_edge->len);
        if (res_edge.ok) {
            g_wat_edge_fn = (RasterWatEdgeBlockFn)jit_load(res_edge.wasm, res_edge.len, "edge_block_8x8");
        }
    }
    arena_pop_to(&g_frame_arena, mark);

    const EmbeddedAsm* ea_blend = bench_find_asm("blend_alpha_rgba8");
    if (ea_blend) {
        MasmResult res_blend = masm_assemble(&g_frame_arena, ea_blend->src, ea_blend->len);
        if (res_blend.ok) {
            g_wat_blend_fn = (RasterWatBlendAlphaFn)jit_load(res_blend.wasm, res_blend.len, "blend_alpha_rgba8");
        }
    }
    arena_pop_to(&g_frame_arena, mark);

    /* Varsayılan olarak C-SIMD kullan; gerçek sahnede >%15 kazananlar aktif edilir */
    engine_set_wat_raster_mode(0);
}

static void init_procedural_textures(void) {
    /* Doku 0: 32x32 dama tahtası */
    tex_init(&g_textures[0], &g_perm_arena, 32, 32, 0);
    tex_gen_checker(&g_textures[0], 4, FB_RGBA(255, 90, 60, 230), FB_RGBA(40, 200, 255, 230));

    /* Doku 1: 32x32 4-köşe renk gradyanı */
    tex_init(&g_textures[1], &g_perm_arena, 32, 32, 0);
    tex_gen_gradient(&g_textures[1],
                     FB_RGBA(255, 230, 50, 220),
                     FB_RGBA(255, 50, 180, 220),
                     FB_RGBA(50, 255, 160, 220),
                     FB_RGBA(70, 90, 255, 220));

    /* Doku 2: 32x32 gürültü + dairesel alpha maskesi (discard testini de görselleştirir) */
    tex_init(&g_textures[2], &g_perm_arena, 32, 32, 0);
    tex_gen_noise(&g_textures[2], 0x9E3779B97F4A7C15ULL);
    for (u32 y = 0; y < 32; y++) {
        f32 dy = (f32)y - 15.5f;
        for (u32 x = 0; x < 32; x++) {
            f32 dx = (f32)x - 15.5f;
            if (dx * dx + dy * dy > 15.0f * 15.0f) {
                g_textures[2].px[y * 32 + x] &= 0x00FFFFFFu; /* alpha = 0 -> discard */
            }
        }
    }
}

/* Tam ekran veya verilen dikdörtgen (x0,y0)-(x1,y1) üzerinde UV=(0..1, 0..1) olan 2 üçgen çizer. */
static void draw_quad_uv(Rasterizer* r, f32 x0, f32 y0, f32 x1, f32 y1) {
    RasterVert v0 = { x0, y0, 0.0f, 1.0f, { 0.0f, 0.0f } };
    RasterVert v1 = { x1, y0, 0.0f, 1.0f, { 1.0f, 0.0f } };
    RasterVert v2 = { x1, y1, 0.0f, 1.0f, { 1.0f, 1.0f } };
    RasterVert v3 = { x0, y1, 0.0f, 1.0f, { 0.0f, 1.0f } };
    raster_triangle(r, v0, v1, v2);
    raster_triangle(r, v0, v2, v3);
}

/* Canlı editördeki kaynağı derler; başarılıysa g_live_shader'ı günceller, hatada önceki shader'ı korur. */
static void compile_editor_shader(void) {
    MtkDiag diag;
    ManagedShader candidate;
    b32 ok = shaders_compile_one(&candidate, "editor.mtk",
                                 g_editor.buf, g_editor.len,
                                 ref_shade_plasma, &g_frame_arena, &diag);
    if (ok) {
        g_live_shader     = candidate;
        g_has_live_shader = TRUE;
        editor_set_diag(&g_editor, &diag, candidate.compile_ms, candidate.wasm_bytes);
        LOG("Editor JIT derlendi: %.3f ms, %u B wasm, %u IR komut",
            candidate.compile_ms, candidate.wasm_bytes, candidate.ir_inst_count);
    } else {
        editor_set_diag(&g_editor, &diag, 0.0, 0);
        LOG("Editor derleme hatasi (%u hata):\n%s", diag.count, diag.text);
    }
}

/* Canlı shader'ın MtkLayout tablosunu okuyarak time, resolution, threshold, tint uniformlarını doldurur. */
static u8* build_auto_uniforms(const MtkLayout* layout, Arena* arena, f32 time_sec, f32 res_w, f32 res_h) {
    u32 ubytes = layout ? MAX(layout->uniform_bytes, 64u) : 64u;
    u8* ubuf = (u8*)arena_push(arena, ubytes, 16);
    if (!layout || layout->sym_count == 0) {
        *(f32*)ubuf = time_sec;
        return ubuf;
    }

    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* sym = &layout->syms[i];
        if (sym->kind != MTK_SYM_UNIFORM) continue;
        u32 off = sym->offset_or_index;
        if (off + 16u > ubytes) continue;
        f32* dst = (f32*)(ubuf + off);

        if (str_eq(sym->name, "time") && sym->type == MTK_TYPE_F32) {
            dst[0] = time_sec;
        } else if (str_eq(sym->name, "resolution") && sym->type == MTK_TYPE_VEC2) {
            dst[0] = res_w;
            dst[1] = res_h;
        } else if (str_eq(sym->name, "threshold") && sym->type == MTK_TYPE_F32) {
            dst[0] = 1.35f + 0.35f * m_sin(time_sec * 1.8f);
        } else if ((str_eq(sym->name, "tint") || str_eq(sym->name, "color")) && sym->type == MTK_TYPE_VEC4) {
            dst[0] = 1.0f;
            dst[1] = 1.0f;
            dst[2] = 1.0f;
            dst[3] = 1.0f;
        }
    }
    return ubuf;
}

EXPORT(engine_init) void engine_init(u32 seed) {
    u8* heap = &__heap_base;
    arena_init(&g_perm_arena, heap, MB(64));
    arena_init(&g_frame_arena, heap + MB(64), MB(128));
    arena_init(&g_asset_arena, heap + MB(192), MB(48));

    fb_init(&g_fb, &g_perm_arena, FB_DEFAULT_W, FB_DEFAULT_H);
    __builtin_memset(&g_input, 0, sizeof(g_input));
    __builtin_memset(g_prev_keys, 0, sizeof(g_prev_keys));
    __builtin_memset(g_mode_ms, 0, sizeof(g_mode_ms));
    __builtin_memset(g_mode_shade_ms, 0, sizeof(g_mode_shade_ms));

    ui_init(&g_ui, &g_fb);
    init_procedural_textures();

    /* Gömülü tüm Matoki shader'larını ve WAT çekirdeklerini derle ve JIT'e yükle */
    shaders_init(&g_shaders, &g_frame_arena);
    init_wat_raster_kernels();
    scene3d_init(&g_perm_arena, &g_frame_arena, &g_fb);
    scene_plat_init(&g_perm_arena);
    scene_ocak_init(&g_perm_arena, &g_frame_arena);
    scene_kuyu_init(&g_perm_arena, &g_frame_arena);

    /* Editörü plasma.mtk kaynağıyla ilklendir (+ resolution uniform'u de hazır olsun) */
    const char* default_ed_src =
        "// Canli Matoki Shader (Ctrl+Enter ile derle)\n"
        "uniform time: f32;\n"
        "uniform resolution: vec2;\n"
        "in uv: vec2;\n\n"
        "const PI: f32 = 3.14159265;\n\n"
        "fn wave(p: vec2, t: f32) -> f32 {\n"
        "    let v1 = sin(p.x * 10.0 + t);\n"
        "    let v2 = cos(p.y * 10.0 - t * 0.5);\n"
        "    let v3 = sin((p.x + p.y) * 7.0 + t * 0.7);\n"
        "    let cx = p.x - 0.5;\n"
        "    let cy = p.y - 0.5;\n"
        "    let d  = sqrt(cx * cx + cy * cy);\n"
        "    let v4 = cos(d * 14.0 - t * 1.3);\n"
        "    return (v1 + v2 + v3 + v4) * 0.25;\n"
        "}\n\n"
        "fn frag() -> rgba {\n"
        "    let aspect = resolution.x / max(resolution.y, 1.0);\n"
        "    let p = vec2(uv.x * aspect, uv.y);\n"
        "    let w = wave(p, time);\n"
        "    let r = sin(w * PI) * 0.5 + 0.5;\n"
        "    let g = cos(w * PI + 1.0) * 0.5 + 0.5;\n"
        "    let b = sin(w * PI + 2.0) * 0.5 + 0.5;\n"
        "    return vec4(r, g, b, 1.0);\n"
        "}\n";

    u32 ed_len = 0;
    while (default_ed_src[ed_len]) ed_len++;
    editor_init(&g_editor, default_ed_src, ed_len);
    compile_editor_shader();

    g_sprites      = ARENA_PUSH_ARRAY(&g_perm_arena, DemoSprite, MAX_SPRITES);
    g_sprite_count = 10000;
    g_blend_mode   = BLEND_NONE;
    g_scene        = SCENE_SPRITES;
    g_use_jit      = TRUE;
    g_show_debug   = FALSE;
    g_fps_last_ms  = 0.0;
    g_fps_frames   = 0;
    g_accum_pixels = 0;

    Rng rng;
    rng_seed(&rng, seed != 0 ? (u64)seed : 0x12345678ULL);
    for (u32 i = 0; i < MAX_SPRITES; i++) {
        g_sprites[i].x    = 12.0f + rng_f32(&rng) * (f32)(FB_DEFAULT_W - 24);
        g_sprites[i].y    = 26.0f + rng_f32(&rng) * (f32)(FB_DEFAULT_H - 38);
        g_sprites[i].vx   = (rng_f32(&rng) * 2.0f - 1.0f) * 120.0f;
        g_sprites[i].vy   = (rng_f32(&rng) * 2.0f - 1.0f) * 120.0f;
        g_sprites[i].rot  = rng_f32(&rng) * M_TAU;
        g_sprites[i].vrot = (rng_f32(&rng) * 2.0f - 1.0f) * 3.5f;
        g_sprites[i].size = 4.0f + rng_f32(&rng) * 4.0f;
    }

    prof_reset();
    LOG("MATOKI M5_4 hazir: Tab=Sahne, J=JIT/C-Ref, F1=Debug, F12=PNG, Ctrl+Enter=Derle");
}

static void render_scene_sprites(f32 step_dt) {
    f32 max_x = (f32)g_fb.w - 8.0f;
    f32 max_y = (f32)g_fb.h - 8.0f;
    u32 count = g_sprite_count;

    for (u32 i = 0; i < count; i++) {
        DemoSprite* s = &g_sprites[i];
        s->x   += s->vx * step_dt;
        s->y   += s->vy * step_dt;
        s->rot += s->vrot * step_dt;
        if (s->rot > M_TAU)  s->rot -= M_TAU;
        if (s->rot < -M_TAU) s->rot += M_TAU;

        if (s->x < 8.0f)   { s->x = 8.0f;   s->vx = -s->vx; }
        if (s->x > max_x)  { s->x = max_x;  s->vx = -s->vx; }
        if (s->y < 24.0f)  { s->y = 24.0f;  s->vy = -s->vy; }
        if (s->y > max_y)  { s->y = max_y;  s->vy = -s->vy; }
    }

    const ManagedShader* sh = shaders_find(&g_shaders, "sprite.mtk");
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, g_use_jit, g_blend_mode, &fused);

    raster_begin(&g_raster, &g_fb, &g_frame_arena);
    b2d_begin(&g_batch, &g_raster, mat3_identity());
    b2d_set_shader_ex(&g_batch, fn, fused);
    b2d_set_blend(&g_batch, g_blend_mode);

    Rect full_uv = rect(0.0f, 0.0f, 1.0f, 1.0f);
    Vec4 white   = vec4(1.0f, 1.0f, 1.0f, 1.0f);

    for (u32 t = 0; t < TEX_COUNT; t++) {
        u32 start = (count * t) / TEX_COUNT;
        u32 end   = (count * (t + 1u)) / TEX_COUNT;
        const Texture* tex = &g_textures[t];
        for (u32 i = start; i < end; i++) {
            const DemoSprite* s = &g_sprites[i];
            b2d_sprite(&g_batch, tex, full_uv,
                       vec2(s->x, s->y), vec2(s->size, s->size), s->rot, white);
        }
    }

    b2d_end(&g_batch);
}

static void render_scene_plasma(f32 time_sec) {
    const ManagedShader* sh = shaders_find(&g_shaders, "plasma.mtk");
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, g_use_jit, BLEND_NONE, &fused);

    f32* u_time = (f32*)arena_push(&g_frame_arena, 16, 16);
    *u_time = time_sec;

    raster_begin(&g_raster, &g_fb, &g_frame_arena);
    raster_set_shader_ex(&g_raster, fn, u_time, NULL, 2, BLEND_NONE, fused);
    draw_quad_uv(&g_raster, 0.0f, 20.0f, (f32)g_fb.w, (f32)g_fb.h);
    raster_flush(&g_raster);
}

static void render_scene_branchy(f32 time_sec) {
    /* Arka plana dama deseni çiz ki discard edilen pikseller net görünsün */
    for (u32 y = 20; y < g_fb.h; y++) {
        u32* row = g_fb.px + (usize)y * g_fb.w;
        for (u32 x = 0; x < g_fb.w; x++) {
            u32 chk = ((x >> 4) ^ (y >> 4)) & 1u;
            row[x] = chk ? FB_RGBA(32, 36, 48, 255) : FB_RGBA(20, 23, 30, 255);
        }
    }

    const ManagedShader* sh = shaders_find(&g_shaders, "branchy.mtk");
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, g_use_jit, g_blend_mode, &fused);

    f32* u_thresh = (f32*)arena_push(&g_frame_arena, 16, 16);
    *u_thresh = 1.35f + 0.35f * m_sin(time_sec * 1.8f);

    raster_begin(&g_raster, &g_fb, &g_frame_arena);
    raster_set_shader_ex(&g_raster, fn, u_thresh, NULL, 2, g_blend_mode, fused);
    draw_quad_uv(&g_raster, 24.0f, 32.0f, (f32)g_fb.w - 24.0f, (f32)g_fb.h - 16.0f);
    raster_flush(&g_raster);
}

static void render_scene_editor(f32 time_sec, f32 step_dt) {
    /* Bölünmüş ekran: sol yarı (0..336) kod editörü, sağ yarı (340..636) canlı shader önizlemesi */
    i32 split_x = 336;
    i32 prev_x0 = split_x + 4;
    i32 prev_y0 = 22;
    i32 prev_w  = (i32)g_fb.w - prev_x0 - 4;
    i32 prev_h  = (i32)g_fb.h - prev_y0 - 4;

    if (g_has_live_shader) {
        b32 fused = FALSE;
        ShadeFn fn = shaders_get_fn(&g_live_shader, g_use_jit, BLEND_NONE, &fused);
        u8* ubuf = build_auto_uniforms(&g_live_shader.layout, &g_frame_arena,
                                       time_sec, (f32)prev_w, (f32)prev_h);
        u32 vcount = MAX(g_live_shader.layout.varying_count, 2u);

        raster_begin(&g_raster, &g_fb, &g_frame_arena);
        raster_set_shader_ex(&g_raster, fn, ubuf, &g_textures[0], vcount, BLEND_NONE, fused);
        draw_quad_uv(&g_raster, (f32)prev_x0, (f32)prev_y0,
                     (f32)(prev_x0 + prev_w), (f32)(prev_y0 + prev_h));
        raster_flush(&g_raster);
    }

    /* Önizleme çerçevesi ve bilgi başlığı */
    ui_draw_outline(&g_ui, prev_x0, prev_y0, prev_w, prev_h, FB_RGBA(70, 86, 112, 255));
    ui_draw_rect(&g_ui, prev_x0 + 1, prev_y0 + 1, prev_w - 2, 14, FB_RGBA(16, 20, 28, 210));
    char ptitle[80];
    fmt_format(ptitle, sizeof(ptitle), "CANLI ONIZLEME (%dx%d) [%s]",
               prev_w, prev_h, g_use_jit ? "JIT" : "C-Ref");
    ui_draw_text(&g_ui, prev_x0 + 6, prev_y0 + 4, ptitle, FB_RGBA(220, 232, 248, 255), 1);

    /* Sol yarı: metin editörü */
    if (editor_update_and_draw(&g_editor, &g_ui, &g_input, step_dt,
                               4, 22, split_x - 4, (i32)g_fb.h - 26)) {
        compile_editor_shader();
    }
}

static void draw_top_bar(void) {
    ui_draw_rect(&g_ui, 0, 0, (i32)g_fb.w, 19, FB_RGBA(20, 25, 36, 245));
    ui_draw_rect(&g_ui, 0, 19, (i32)g_fb.w, 1, FB_RGBA(64, 82, 110, 255));

    /* Sahne sekmeleri (tıklanabilir veya Tab ile geçişli) */
    i32 tx = 4;
    for (u32 s = 0; s < SCENE_COUNT; s++) {
        const char* sname = scene_name((DemoScene)s);
        i32 tw = font_measure_width(sname, 1) + 10;
        b32 active = ((u32)g_scene == s);
        b32 hover  = (g_input.mouse_x >= tx && g_input.mouse_x < tx + tw &&
                      g_input.mouse_y >= 2  && g_input.mouse_y < 17);
        u32 bg = active ? FB_RGBA(56, 132, 98, 255)
                        : (hover ? FB_RGBA(48, 62, 86, 255) : FB_RGBA(32, 40, 56, 255));
        ui_draw_rect(&g_ui, tx, 2, tw, 15, bg);
        ui_draw_outline(&g_ui, tx, 2, tw, 15, FB_RGBA(75, 95, 125, 255));
        ui_draw_text(&g_ui, tx + 5, 6, sname, FB_RGBA(238, 244, 252, 255), 1);
        if (hover && g_ui.mouse_pressed) {
            g_scene = (DemoScene)s;
        }
        tx += tw + 3;
    }

    /* Mod ve JIT vs C-Ref ms karşılaştırma göstergesi */
    f32 ref_ms = g_mode_ms[g_scene][0];
    f32 jit_ms = g_mode_ms[g_scene][1];
    f32 ref_sh = g_mode_shade_ms[g_scene][0];
    f32 jit_sh = g_mode_shade_ms[g_scene][1];

    char info[128];
    fmt_format(info, sizeof(info),
               "[T]%uT [J]%s | JIT %.1f / C %.1fms (Sh %.1f/%.1f) | [F1]Dbg [F12]PNG",
               thread_count(), g_use_jit ? "JIT" : "C-REF",
               (f64)jit_ms, (f64)ref_ms, (f64)jit_sh, (f64)ref_sh);
    i32 iw = font_measure_width(info, 1);
    ui_draw_text(&g_ui, MAX((i32)g_fb.w - iw - 4, tx + 4), 6,
                 info, g_use_jit ? FB_RGBA(110, 235, 165, 255) : FB_RGBA(245, 190, 90, 255), 1);
}

static void draw_debug_overlay(void) {
    i32 pw = 316;
    i32 ph = 334;
    i32 px = (i32)g_fb.w - pw - 6;
    i32 py = 22;

    ui_panel_begin(&g_ui, "MATOKI DEBUG OVERLAY (F1)", px, py, pw, ph);

    f32 fps = (g_cur_frame_ms > 0.05f) ? (1000.0f / g_cur_frame_ms) : 0.0f;
    char line[96];
    fmt_format(line, sizeof(line), "FPS: %.1f (%.2f ms) | %uT | Mod: %s | Blend: %s",
               (f64)fps, (f64)g_cur_frame_ms, thread_count(),
               g_use_jit ? "Matoki JIT" : "C Referans",
               blend_mode_name(g_blend_mode));
    ui_label(&g_ui, line);

    fmt_format(line, sizeof(line), "Caps:%s%s%s%s%s%s%s[%s/%s] tex:%u",
               (g_caps_mask & CAP_SAB)          ? "SAB "  : "",
               (g_caps_mask & CAP_SIMD128)      ? "SIMD " : "",
               (g_caps_mask & CAP_RELAXED_SIMD) ? "RLX "  : "",
               (g_caps_mask & CAP_THREADS)      ? "THR "  : "",
               (g_caps_mask & CAP_OFFSCREEN)    ? "OFFS " : "",
               (g_caps_mask & CAP_WAIT_ASYNC)   ? "WA "   : "",
               (g_caps_mask & CAP_WEBGPU)       ? "GPU "  : "",
               SIMD_RELAXED ? "rlx" : "std",
               (g_caps_mask & CAP_WORKER_RENDER) ? "wrk" : "main",
               g_caps_gpu_max_tex2d);
    ui_label(&g_ui, line);

    ui_frame_graph(&g_ui, "Kare Suresi", 33.3f);
    ui_separator(&g_ui);
    ui_profiler_table(&g_ui);
    ui_separator(&g_ui);

    /* JIT Shader Tablosu (wasm boyutu, IR komut sayısı, derleme ms) */
    ui_draw_text(&g_ui, g_ui.cursor_x, g_ui.cursor_y,
                 "SHADER          WASM(B)  IR   DERLE(ms)", FB_RGBA(150, 165, 188, 255), 1);
    g_ui.cursor_y += 11;
    for (u32 i = 0; i < g_shaders.count; i++) {
        const ManagedShader* sh = &g_shaders.items[i];
        fmt_format(line, sizeof(line), "%-14s %6u  %4u   %6.3f",
                   sh->name, sh->wasm_bytes, sh->ir_inst_count, sh->compile_ms);
        ui_draw_text(&g_ui, g_ui.cursor_x, g_ui.cursor_y, line, FB_RGBA(225, 235, 248, 255), 1);
        g_ui.cursor_y += 10;
    }
    if (g_has_live_shader) {
        fmt_format(line, sizeof(line), "%-14s %6u  %4u   %6.3f",
                   g_live_shader.name, g_live_shader.wasm_bytes,
                   g_live_shader.ir_inst_count, g_live_shader.compile_ms);
        ui_draw_text(&g_ui, g_ui.cursor_x, g_ui.cursor_y, line, FB_RGBA(110, 235, 165, 255), 1);
        g_ui.cursor_y += 10;
    }

    ui_panel_end(&g_ui);
}

static void capture_screenshot_png(void) {
    usize mark = arena_mark(&g_frame_arena);
    f64 t0 = plat_time_ms();
    usize png_len = 0;
    u8* png_bytes = png_encode(g_fb.px, g_fb.w, g_fb.h, &png_len, &g_frame_arena);
    f64 t1 = plat_time_ms();

    if (png_bytes != NULL && png_len > 0) {
        const char* fname = "matoki.png";
        plat_download(fname, 10, png_bytes, (u32)png_len);
        LOG("Ekran goruntusu kaydedildi: matoki.png (%u byte, %.2f ms)", (u32)png_len, t1 - t0);
        g_Screenshot_flash = 0.25f;
    } else {
        LOG("Ekran goruntusu PNG kodlama hatasi!");
    }
    arena_pop_to(&g_frame_arena, mark);
}

EXPORT(engine_frame) void engine_frame(f64 now_ms, f32 dt) {
    f64 frame_t0 = plat_time_ms();
    arena_reset(&g_frame_arena);

    f32 step_dt = (dt >= 0.0f && dt < 0.1f) ? dt : (1.0f / 60.0f);
    f32 time_sec = (f32)(now_ms * 0.001);

    /* Tab: sahne değiştir (Editör sahnesinde odak editördeyken Tab girinti ekler;
     * ancak Ctrl+Tab veya editör dışına tıklanmışsa veya üst sekmelerden sahne değişir;
     * ayrıca editör dışındaki sahnelerde doğrudan Tab sahne değiştirir) */
    if (key_pressed(KEY_TAB)) {
        if (g_scene != SCENE_EDITOR || !g_editor.focused || g_input.keys[17]) {
            g_scene = (DemoScene)(((u32)g_scene + 1u) % SCENE_COUNT);
            /* Tab tuşunun editöre sızmasını önle */
            g_editor.prev_keys[KEY_TAB] = 1;
            prof_reset();
            LOG("Sahne: %s", scene_name(g_scene));
        }
    }

    /* F1: Debug overlay aç/kapat */
    if (key_pressed(KEY_F1)) {
        g_show_debug = !g_show_debug;
    }

    /* Editör odakta değilken tek tuş kısayolları (T, J, B, P, 1..9) */
    b32 editor_typing = (g_scene == SCENE_EDITOR && g_editor.focused);
    if (!editor_typing) {
        if (key_pressed(KEY_T)) {
            u32 cur_t = thread_count();
            u32 next_t = (cur_t < 2u) ? 2u : (cur_t < 4u) ? 4u : (cur_t < 8u) ? 8u : 1u;
            u32 max_t = 1u + thread_spawned_worker_count();
            if (max_t > 1u && next_t > max_t) {
                next_t = 1u;
            }
            engine_set_worker_count(next_t - 1u);
            prof_reset();
            LOG("Thread sayisi: %u (%u worker)", thread_count(), thread_count() - 1u);
        }
        if (key_pressed(KEY_J)) {
            g_use_jit = !g_use_jit;
            prof_reset();
            LOG("Shader modu: %s", g_use_jit ? "Matoki JIT" : "C Referans");
        }
        if (key_pressed(KEY_B)) {
            g_blend_mode = (BlendMode)(((u32)g_blend_mode + 1u) % 3u);
            prof_reset();
            LOG("Blend modu: %s", blend_mode_name(g_blend_mode));
        }
        for (u32 i = 0; i < 9; i++) {
            if (key_pressed(KEY_1 + i) || key_pressed(97 + i)) {
                g_sprite_count = k_sprite_counts[i];
                prof_reset();
                LOG("Sprite sayisi: %u", g_sprite_count);
            }
        }
    }

    b32 want_report     = (!editor_typing && key_pressed(KEY_P));
    b32 want_screenshot = key_pressed(KEY_F12);

    /* Oyun sahnesinden çıkınca müziği durdur */
    static DemoScene s_prev_scene = SCENE_SPRITES;
    if (s_prev_scene == SCENE_GAME && g_scene != SCENE_GAME) scene_plat_leave();
    if (s_prev_scene == SCENE_OCAK && g_scene != SCENE_OCAK) scene_ocak_leave();
    if (s_prev_scene == SCENE_KUYU && g_scene != SCENE_KUYU) scene_kuyu_leave();
    s_prev_scene = g_scene;

    fb_clear(&g_fb, FB_RGBA(18, 20, 28, 255));
    ui_begin(&g_ui, &g_input);

    switch (g_scene) {
    case SCENE_SPRITES:
        render_scene_sprites(step_dt);
        break;
    case SCENE_PLASMA:
        render_scene_plasma(time_sec);
        break;
    case SCENE_BRANCHY:
        render_scene_branchy(time_sec);
        break;
    case SCENE_GAME:
        scene_plat_frame(&g_raster, &g_fb, &g_frame_arena, &g_shaders, &g_input, step_dt, g_use_jit);
        scene_plat_draw_hud(&g_ui, &g_fb);
        break;
    case SCENE_OCAK:
        scene_ocak_frame(&g_raster, &g_fb, &g_frame_arena, &g_shaders, &g_input, step_dt, g_use_jit,
                         g_input.mouse_y >= 20 && !g_show_debug);
        scene_ocak_draw_hud(&g_ui, &g_fb);
        break;
    case SCENE_KUYU:
        scene_kuyu_frame(&g_raster, &g_fb, &g_frame_arena, &g_shaders, &g_input, step_dt, g_use_jit,
                         g_pointer_locked);
        scene_kuyu_draw_hud(&g_ui, &g_fb);
        break;
    case SCENE_3D:
        scene3d_frame(&g_raster, &g_fb, &g_frame_arena, &g_shaders, &g_input, step_dt, time_sec,
                      g_use_jit, g_input.mouse_y >= 20 && !g_show_debug);
        scene3d_draw_hud(&g_ui, &g_fb);
        break;
    case SCENE_EDITOR:
    default:
        render_scene_editor(time_sec, step_dt);
        break;
    }

    draw_top_bar();

    if (g_show_debug) {
        draw_debug_overlay();
    }

    if (g_Screenshot_flash > 0.0f) {
        u32 a = (u32)m_clamp(g_Screenshot_flash * 400.0f, 0.0f, 180.0f);
        ui_draw_rect(&g_ui, 0, 0, (i32)g_fb.w, (i32)g_fb.h, FB_RGBA(255, 255, 255, a));
        g_Screenshot_flash -= step_dt;
    }

    ui_end(&g_ui);
    prof_frame_end();

    /* F12 ekran görüntüsü (tüm sahne + UI çizildikten sonra yakala) */
    if (want_screenshot) {
        capture_screenshot_png();
    }

    /* Ses: halka tamponu ~70 ms ileriye kadar doldur (ses başlamadıysa işlem yok) */
    audio_pump(0.07f);

    /* Girdi durumunu gelecek kare için sıfırla */
    __builtin_memcpy(g_prev_keys, g_input.keys, sizeof(g_prev_keys));
    g_input.mouse_dx   = 0;
    g_input.mouse_dy   = 0;
    g_input.wheel      = 0;
    g_input.char_count = 0;

    f64 frame_t1 = plat_time_ms();
    g_cur_frame_ms = (f32)(frame_t1 - frame_t0);
    ui_record_frame_time(&g_ui, g_cur_frame_ms);

    /* Aktif sahne ve mod için EMA güncelle */
    u32 mode_idx = g_use_jit ? 1u : 0u;
    if (g_mode_ms[g_scene][mode_idx] <= 0.001f) {
        g_mode_ms[g_scene][mode_idx] = g_cur_frame_ms;
    } else {
        g_mode_ms[g_scene][mode_idx] = g_mode_ms[g_scene][mode_idx] * 0.9f + g_cur_frame_ms * 0.1f;
    }

    /* "shade" bölgesi süresini oku */
    for (u32 z = 0; z < prof_zone_count(); z++) {
        const ProfZone* pz = prof_zone_get(z);
        if (pz && str_eq(pz->name, "shade")) {
            f32 sms = (f32)pz->last_ms;
            if (g_mode_shade_ms[g_scene][mode_idx] <= 0.001f) {
                g_mode_shade_ms[g_scene][mode_idx] = sms;
            } else {
                g_mode_shade_ms[g_scene][mode_idx] = g_mode_shade_ms[g_scene][mode_idx] * 0.9f + sms * 0.1f;
            }
            break;
        }
    }

    g_accum_pixels += g_raster.stat_pixels;

    if (want_report) {
        prof_report();
    }

    if (g_fps_last_ms == 0.0) {
        g_fps_last_ms = now_ms;
    }
    g_fps_frames++;
    f64 elapsed = now_ms - g_fps_last_ms;
    if (elapsed >= 1000.0) {
        f64 fps    = ((f64)g_fps_frames * 1000.0) / elapsed;
        f64 ms_frm = elapsed / (f64)g_fps_frames;
        f64 mpix_s = ((f64)g_accum_pixels / (elapsed * 1000.0));
        LOG("FPS: %.1f | %.2f ms/frame | %.1f Mpiksel/s | %uT | sahne=%s mod=%s",
            fps, ms_frm, mpix_s, thread_count(), scene_name(g_scene), g_use_jit ? "JIT" : "C-Ref");
        g_fps_frames   = 0;
        g_fps_last_ms  = now_ms;
        g_accum_pixels = 0;
    }
}

static void render_depth_test_scene(void) {
    fb_clear_depth(&g_fb, 1.0f);
    const ManagedShader* sh = shaders_find(&g_shaders, "plasma.mtk");
    b32 fused = FALSE;
    ShadeFn fn = shaders_get_fn(sh, g_use_jit, BLEND_NONE, &fused);

    f32* u_time = (f32*)arena_push(&g_frame_arena, 16, 16);
    *u_time = 2.0f;

    raster_begin(&g_raster, &g_fb, &g_frame_arena);
    raster_set_depth(&g_raster, TRUE, TRUE, DEPTH_LESS);
    raster_set_shader_ex(&g_raster, fn, u_time, NULL, 2, BLEND_NONE, fused);

    /* Birbiriyle kesişen farklı Z derinlikli üçgenler */
    for (u32 i = 0; i < 64; i++) {
        f32 fi = (f32)i;
        f32 cx = 80.0f + (f32)((i * 73u) % 480u);
        f32 cy = 60.0f + (f32)((i * 53u) % 240u);
        f32 z0 = 0.2f + 0.6f * m_sin(fi * 0.37f);
        f32 z1 = 0.2f + 0.6f * m_cos(fi * 0.51f);
        f32 z2 = 0.2f + 0.6f * m_sin(fi * 0.79f + 1.0f);
        RasterVert v0 = { cx - 45.0f, cy - 35.0f, z0, 1.0f, { 0.0f, 0.0f } };
        RasterVert v1 = { cx + 50.0f, cy - 20.0f, z1, 1.0f, { 1.0f, 0.0f } };
        RasterVert v2 = { cx + 10.0f, cy + 48.0f, z2, 1.0f, { 0.5f, 1.0f } };
        raster_triangle(&g_raster, v0, v1, v2);
    }
    raster_flush(&g_raster);
}

/* 1 thread vs N thread tüm sahnelerde (10k Sprite, Alpha Sprite, Plasma, Branchy, Editor, 3D Depth)
 * bit-bit özdeş framebuffer doğrulaması. 0 = tüm sahneler 100% bit-bit aynı. */
EXPORT(engine_raster_parallel_selftest) i32 engine_raster_parallel_selftest(void) {
    u32 saved_workers = thread_count() - 1u;
    DemoScene saved_scene = g_scene;
    u32 saved_sprites = g_sprite_count;
    BlendMode saved_blend = g_blend_mode;
    b32 saved_jit = g_use_jit;

    usize px_count = (usize)g_fb.w * (usize)g_fb.h;
    usize perm_mark = arena_mark(&g_perm_arena);
    u32* ref_px = ARENA_PUSH_ARRAY(&g_perm_arena, u32, px_count);

    g_sprite_count = 10000;
    g_use_jit = TRUE;

    for (u32 tc = 0; tc < 6; tc++) {
        /* 1) 1 thread ile referans kareyi çiz */
        engine_set_worker_count(0);
        arena_reset(&g_frame_arena);
        fb_clear(&g_fb, FB_RGBA(18, 20, 28, 255));
        ui_begin(&g_ui, &g_input);

        if (tc == 0) {
            g_blend_mode = BLEND_NONE;
            render_scene_sprites(0.0f);
        } else if (tc == 1) {
            g_blend_mode = BLEND_ALPHA;
            render_scene_sprites(0.0f);
        } else if (tc == 2) {
            render_scene_plasma(1.25f);
        } else if (tc == 3) {
            g_blend_mode = BLEND_ALPHA;
            render_scene_branchy(1.25f);
        } else if (tc == 4) {
            render_scene_editor(1.25f, 0.0f);
        } else {
            render_depth_test_scene();
        }
        ui_end(&g_ui);
        prof_frame_end();

        __builtin_memcpy(ref_px, g_fb.px, px_count * sizeof(u32));

        /* 2) Aktif worker havuzuyla aynı sahneyi çiz ve bit-bit karşılaştır */
        u32 test_workers = saved_workers > 0 ? saved_workers : thread_spawned_worker_count();
        engine_set_worker_count(test_workers);
        arena_reset(&g_frame_arena);
        fb_clear(&g_fb, FB_RGBA(18, 20, 28, 255));
        ui_begin(&g_ui, &g_input);

        if (tc == 0) {
            g_blend_mode = BLEND_NONE;
            render_scene_sprites(0.0f);
        } else if (tc == 1) {
            g_blend_mode = BLEND_ALPHA;
            render_scene_sprites(0.0f);
        } else if (tc == 2) {
            render_scene_plasma(1.25f);
        } else if (tc == 3) {
            g_blend_mode = BLEND_ALPHA;
            render_scene_branchy(1.25f);
        } else if (tc == 4) {
            render_scene_editor(1.25f, 0.0f);
        } else {
            render_depth_test_scene();
        }
        ui_end(&g_ui);
        prof_frame_end();

        for (usize i = 0; i < px_count; i++) {
            if (g_fb.px[i] != ref_px[i]) {
                arena_pop_to(&g_perm_arena, perm_mark);
                g_scene = saved_scene;
                g_sprite_count = saved_sprites;
                g_blend_mode = saved_blend;
                g_use_jit = saved_jit;
                engine_set_worker_count(saved_workers);
                return -100 - (i32)tc;
            }
        }
    }

    arena_pop_to(&g_perm_arena, perm_mark);
    g_scene = saved_scene;
    g_sprite_count = saved_sprites;
    g_blend_mode = saved_blend;
    g_use_jit = saved_jit;
    engine_set_worker_count(saved_workers);
    return 0;
}

EXPORT(engine_set_scene) void engine_set_scene(u32 scene, u32 sprite_count, u32 blend_mode, b32 use_jit) {
    if (scene < SCENE_COUNT) g_scene = (DemoScene)scene;
    if (sprite_count > 0 && sprite_count <= MAX_SPRITES) g_sprite_count = sprite_count;
    if (blend_mode < 3u) g_blend_mode = (BlendMode)blend_mode;
    g_use_jit = use_jit ? TRUE : FALSE;
}

EXPORT(engine_set_parallel_dynamic) void engine_set_parallel_dynamic(b32 dynamic) {
    raster_set_parallel_dynamic(dynamic);
}

EXPORT(engine_compile_editor_now) f64 engine_compile_editor_now(void) {
    f64 t0 = plat_time_ms();
    compile_editor_shader();
    return plat_time_ms() - t0;
}

/* ---------- Sürükle-bırak dosya yükleme (web/filedrop.js) ----------
 * Sıra: engine_file_begin → her dosya için engine_file_alloc(ad) + engine_file_alloc(veri) + engine_file_add
 * → engine_file_end (modeli yükler, 3D sahnesine geçer). Kareler arasında çağrılır (frame arena geçici bellek). */
#define MAX_DROP_FILES 32
static DroppedFile g_drop[MAX_DROP_FILES];
static u32         g_drop_count;

EXPORT(engine_file_begin) void engine_file_begin(void) {
    arena_reset(&g_asset_arena);
    g_drop_count = 0;
}

EXPORT(engine_file_alloc) u8* engine_file_alloc(u32 len) {
    return (u8*)arena_push(&g_asset_arena, (usize)len + 1u, 16);
}

EXPORT(engine_file_add) void engine_file_add(const char* name, u32 name_len, const u8* data, u32 len) {
    if (g_drop_count >= MAX_DROP_FILES) return;
    ((char*)name)[name_len] = 0;   /* engine_file_alloc len+1 ayırır */
    g_drop[g_drop_count].name = name;
    g_drop[g_drop_count].data = data;
    g_drop[g_drop_count].len  = len;
    g_drop_count++;
}

EXPORT(engine_file_end) b32 engine_file_end(void) {
    arena_reset(&g_frame_arena);
    b32 ok = scene3d_load_files(g_drop, g_drop_count, &g_asset_arena, &g_frame_arena);
    arena_reset(&g_frame_arena);
    g_scene = SCENE_3D;
    return ok;
}

/* Benchmark: verilen metni JSON olarak ayrıştırır (frame arena), süreyi ms döner; hata = -1 */
EXPORT(engine_bench_json) f64 engine_bench_json(const char* text, u32 len) {
    arena_reset(&g_frame_arena);
    JsonValue v;
    JsonError e;
    f64 t0 = plat_time_ms();
    b32 ok = json_parse(text, len, &g_frame_arena, &v, &e);
    f64 t1 = plat_time_ms();
    arena_reset(&g_frame_arena);
    return ok ? t1 - t0 : -1.0;
}

/* Benchmark betikleri için: 3D vertex aşaması optimizasyon anahtarları (G3D_OPT_*) */
EXPORT(engine_g3d_opt) void engine_g3d_opt(u32 mask) {
    g3d_set_opt(mask);
}

/* Benchmark betikleri için: profiler bölge tablosunu sıfırla / konsola yaz */
EXPORT(engine_prof_reset) void engine_prof_reset(void) {
    prof_reset();
}

EXPORT(engine_prof_report) void engine_prof_report(void) {
    prof_report();
}

/* Ölçüm: JIT/C-ref seçimi ve 3D sahne aydınlatma modu (konsoldan) */
EXPORT(engine_set_jit) void engine_set_jit(b32 on) {
    g_use_jit = on;
}

/* Ölçüm: 3D ön ucun paralel (ertelenmiş) yolu açık/kapalı */
EXPORT(engine_set_g3d_parallel) void engine_set_g3d_parallel(b32 on) {
    r3d_set_parallel(on);
}

EXPORT(engine_s3d_bench) void engine_s3d_bench(b32 pbr, f32 tod) {
    scene3d_bench_set(pbr, tod);
}

EXPORT(engine_ocak_debug) const f32* engine_ocak_debug(void) {
    return scene_ocak_debug();
}

EXPORT(engine_ocak_yaw) void engine_ocak_yaw(f32 yaw) {
    scene_ocak_debug_set_yaw(yaw);
}

EXPORT(engine_last_frame_ms) f32 engine_last_frame_ms(void) {
    return g_cur_frame_ms;
}

EXPORT(engine_input_buf) u8* engine_input_buf(void) {
    return (u8*)&g_input;
}

EXPORT(engine_framebuffer) u32* engine_framebuffer(void) {
    return g_fb.px;
}

EXPORT(engine_fb_width) u32 engine_fb_width(void) {
    return g_fb.w;
}

EXPORT(engine_fb_height) u32 engine_fb_height(void) {
    return g_fb.h;
}

/* Fare kilidi sözleşmesi (ARCHITECTURE §2): JS tıklamada bunu sorar, kilit değişince bildirir */
EXPORT(engine_wants_pointer_lock) u32 engine_wants_pointer_lock(void) {
    return (g_scene == SCENE_KUYU && !g_show_debug) ? 1u : 0u;
}

EXPORT(engine_set_pointer_lock) void engine_set_pointer_lock(u32 locked) {
    g_pointer_locked = locked != 0u;
}

EXPORT(engine_kuyu_debug) const f32* engine_kuyu_debug(void) {
    return scene_kuyu_debug();
}

EXPORT(engine_kuyu_cmd) void engine_kuyu_cmd(u32 cmd, f32 a, f32 b, f32 c) {
    scene_kuyu_debug_cmd(cmd, a, b, c);
}

/* Platform yetenek bildirimi (ARCHITECTURE §2, D-109): web/caps.js sonuçlarını alır */
EXPORT(engine_set_caps) void engine_set_caps(u32 caps_mask, u32 gpu_max_tex2d, u32 gpu_max_buf_mb) {
#if SIMD_RELAXED
    caps_mask |= CAP_RELAXED_BUILD;
#endif
    g_caps_mask           = caps_mask;
    g_caps_gpu_max_tex2d  = gpu_max_tex2d;
    g_caps_gpu_max_buf_mb = gpu_max_buf_mb;
    LOG("Platform caps: mask=0x%04x (%s, render=%s) gpu_tex2d=%u gpu_buf=%uMB",
        g_caps_mask,
        SIMD_RELAXED ? "engine_relaxed.wasm" : "engine.wasm",
        (g_caps_mask & CAP_WORKER_RENDER) ? "worker" : "main",
        g_caps_gpu_max_tex2d, g_caps_gpu_max_buf_mb);

/* F0_B (ARCH_V2 H1): Çalışma anı özelleştirme (arketip başına JIT ECS döngüsü) benchmark dışa aktarımları */
static void ensure_spec_arena(void) {
    if (g_frame_arena.base == NULL) {
        u8* heap = &__heap_base;
        arena_init(&g_frame_arena, heap + MB(64), MB(128));
    }
}

EXPORT(engine_spec_verify) i32 engine_spec_verify(void) {
    ensure_spec_arena();
    arena_reset(&g_frame_arena);
    i32 rc = spec_bench_verify(&g_frame_arena);
    arena_reset(&g_frame_arena);
    return rc;
}

EXPORT(engine_spec_bench_run) i32 engine_spec_bench_run(u32 entity_count) {
    ensure_spec_arena();
    arena_reset(&g_frame_arena);
    i32 rc = spec_bench_run(&g_frame_arena, entity_count);
    arena_reset(&g_frame_arena);
    return rc;
}

EXPORT(engine_spec_report_ptr) const char* engine_spec_report_ptr(void) {
    return spec_bench_report_ptr();
}

EXPORT(engine_spec_report_len) u32 engine_spec_report_len(void) {
    return spec_bench_report_len();
}
