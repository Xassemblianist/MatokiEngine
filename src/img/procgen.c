/* img/procgen.c — 2'nin kuvveti boyutlu, tam döşenebilir (tileable) prosedürel doku üreticileri.
 * Tüm üreticiler img/noise.h içindeki periyodik (_tile) fonksiyonları ve trigonometrik
 * sarma kurallarını kullanarak kenar sürekliliğini (x=0 <-> x=w, y=0 <-> y=h) garanti eder.
 */
#include "img/procgen.h"
#include "img/noise.h"
#include "img/image.h"
#include "core/math.h"
#include "raster/framebuffer.h"

static inline u8 clamp_byte(f32 x) {
    i32 v = (i32)(x + 0.5f);
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static inline u32 color_lerp(u32 c0, u32 c1, f32 t) {
    t = m_clamp(t, 0.0f, 1.0f);
    f32 r0 = (f32)((c0 >>  0) & 0xFFu), r1 = (f32)((c1 >>  0) & 0xFFu);
    f32 g0 = (f32)((c0 >>  8) & 0xFFu), g1 = (f32)((c1 >>  8) & 0xFFu);
    f32 b0 = (f32)((c0 >> 16) & 0xFFu), b1 = (f32)((c1 >> 16) & 0xFFu);
    f32 a0 = (f32)((c0 >> 24) & 0xFFu), a1 = (f32)((c1 >> 24) & 0xFFu);
    return FB_RGBA(clamp_byte(m_lerp(r0, r1, t)),
                   clamp_byte(m_lerp(g0, g1, t)),
                   clamp_byte(m_lerp(b0, b1, t)),
                   clamp_byte(m_lerp(a0, a1, t)));
}

static inline u32 color_scale(u32 c, f32 s) {
    f32 r = (f32)((c >>  0) & 0xFFu) * s;
    f32 g = (f32)((c >>  8) & 0xFFu) * s;
    f32 b = (f32)((c >> 16) & 0xFFu) * s;
    u8  a = (u8)((c >> 24) & 0xFFu);
    return FB_RGBA(clamp_byte(r), clamp_byte(g), clamp_byte(b), a);
}

static inline f32 smoothstep(f32 e0, f32 e1, f32 x) {
    f32 t = m_clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void procgen_brick(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcBrickParams* p) {
    ProcBrickParams def = {
        .cols = 4, .rows = 8, .mortar_width = 0.08f,
        .brick_color = FB_RGBA(168, 62, 45, 255),
        .mortar_color = FB_RGBA(198, 192, 182, 255),
        .color_var = 0.22f
    };
    const ProcBrickParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 cols = MAX(cfg->cols, 1);
    i32 rows = MAX(cfg->rows & ~1, 2); /* döşenebilir şaşırtma için çift sıra */

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            /* Kenar düzensizliği için hafif döşenebilir gürültü */
            f32 n_warp = noise_perlin2_tile(u * 16.0f, v * 16.0f, 16, 16, seed + 11u) * 0.012f;
            f32 bu = (u + n_warp) * (f32)cols;
            f32 bv = (v + n_warp) * (f32)rows;

            i32 row_idx = (i32)m_floor(bv);
            i32 row_mod = ((row_idx % rows) + rows) % rows;
            if (row_mod & 1) {
                bu += 0.5f;
            }
            i32 col_idx = (i32)m_floor(bu);
            i32 col_mod = ((col_idx % cols) + cols) % cols;

            f32 fu = bu - m_floor(bu);
            f32 fv = bv - m_floor(bv);

            f32 mw_x = cfg->mortar_width * 0.5f;
            f32 mw_y = cfg->mortar_width;
            f32 dx = m_min(fu, 1.0f - fu);
            f32 dy = m_min(fv, 1.0f - fv);

            f32 mx = smoothstep(mw_x * 0.6f, mw_x * 1.4f, dx);
            f32 my = smoothstep(mw_y * 0.6f, mw_y * 1.4f, dy);
            f32 brick_mask = mx * my;

            u32 b_hash = noise_hash2(col_mod, row_mod, seed);
            f32 b_var = ((f32)(b_hash & 0xFFFFu) * (1.0f / 32768.0f) - 1.0f) * cfg->color_var;
            f32 surf_n = noise_fbm2_tile(u * 16.0f, v * 16.0f, 16, 16, 3, 2.0f, 0.5f, seed + 37u);

            u32 b_col = color_scale(cfg->brick_color, 1.0f + b_var + surf_n * 0.15f);
            u32 m_col = color_scale(cfg->mortar_color, 0.92f + surf_n * 0.12f);

            out->px[y * w + x] = color_lerp(m_col, b_col, brick_mask);
        }
    }
}

void procgen_wood(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcWoodParams* p) {
    ProcWoodParams def = {
        .rings = 6.0f, .turb_strength = 0.45f, .freq = 4,
        .light_color = FB_RGBA(198, 142, 86, 255),
        .dark_color  = FB_RGBA(104, 62, 30, 255)
    };
    const ProcWoodParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 freq = MAX(cfg->freq, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 turb = noise_fbm2_tile(u * (f32)freq, v * (f32)freq,
                                       freq, freq, 4, 2.0f, 0.5f, seed);
            /* Boyuna lif izi (x yönünde sık, y yönünde uzun) */
            f32 fiber = noise_perlin2_tile(u * (f32)(freq * 8), v * (f32)freq,
                                           freq * 8, freq, seed + 101u) * 0.08f;

            f32 phase = (u * cfg->rings + turb * cfg->turb_strength) * M_TAU;
            f32 ring = 0.5f + 0.5f * m_sin(phase);
            ring = m_pow(ring, 1.6f) + fiber;
            ring = m_clamp(ring, 0.0f, 1.0f);

            out->px[y * w + x] = color_lerp(cfg->light_color, cfg->dark_color, ring);
        }
    }
}

void procgen_marble(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcMarbleParams* p) {
    ProcMarbleParams def = {
        .freq = 4, .vein_freq = 2.0f, .turb = 5.5f,
        .base_color = FB_RGBA(238, 236, 232, 255),
        .vein_color = FB_RGBA(55, 62, 74, 255)
    };
    const ProcMarbleParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 freq = MAX(cfg->freq, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 n = noise_fbm2_tile(u * (f32)freq, v * (f32)freq,
                                    freq, freq, 5, 2.0f, 0.5f, seed);
            f32 angle = (u + v) * cfg->vein_freq * M_TAU + n * cfg->turb;
            f32 vein = 0.5f + 0.5f * m_sin(angle);
            vein = m_pow(1.0f - vein, 3.0f);

            f32 sub = 0.5f + 0.5f * noise_perlin2_tile(u * (f32)(freq * 2), v * (f32)(freq * 2),
                                                       freq * 2, freq * 2, seed + 77u);
            f32 t = m_clamp(vein * 0.85f + sub * 0.15f, 0.0f, 1.0f);

            out->px[y * w + x] = color_lerp(cfg->base_color, cfg->vein_color, t);
        }
    }
}

void procgen_grass(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcGrassParams* p) {
    ProcGrassParams def = {
        .base_freq = 6, .blade_freq = 32,
        .dark_green   = FB_RGBA(34, 82, 26, 255),
        .bright_green = FB_RGBA(112, 178, 52, 255)
    };
    const ProcGrassParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 bf = MAX(cfg->base_freq, 1);
    i32 lf = MAX(cfg->blade_freq, 2);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 patch = 0.5f + 0.5f * noise_fbm2_tile(u * (f32)bf, v * (f32)bf,
                                                      bf, bf, 3, 2.0f, 0.5f, seed);
            f32 blades = 1.0f - noise_worley2_tile(u * (f32)lf, v * (f32)lf,
                                                   lf, lf, seed + 41u, NULL);
            f32 micro = 0.5f + 0.5f * noise_value2_tile(u * (f32)(lf * 2), v * (f32)(lf * 2),
                                                        lf * 2, lf * 2, seed + 89u);

            f32 t = m_clamp(patch * 0.45f + blades * 0.40f + micro * 0.15f, 0.0f, 1.0f);
            out->px[y * w + x] = color_lerp(cfg->dark_green, cfg->bright_green, t);
        }
    }
}

void procgen_stone(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcStoneParams* p) {
    ProcStoneParams def = {
        .cells = 6, .crack_sharpness = 10.0f,
        .stone_color = FB_RGBA(132, 136, 140, 255),
        .crack_color = FB_RGBA(42, 44, 48, 255)
    };
    const ProcStoneParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 cells = MAX(cfg->cells, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 wx = noise_perlin2_tile(u * (f32)(cells * 2), v * (f32)(cells * 2),
                                        cells * 2, cells * 2, seed + 13u) * 0.06f;
            f32 wy = noise_perlin2_tile(u * (f32)(cells * 2), v * (f32)(cells * 2),
                                        cells * 2, cells * 2, seed + 29u) * 0.06f;

            f32 f2 = 0.0f;
            f32 f1 = noise_worley2_tile((u + wx) * (f32)cells, (v + wy) * (f32)cells,
                                        cells, cells, seed, &f2);
            f32 edge = m_clamp((f2 - f1) * cfg->crack_sharpness, 0.0f, 1.0f);

            f32 bump = 0.5f + 0.5f * noise_fbm2_tile(u * 12.0f, v * 12.0f,
                                                     12, 12, 4, 2.0f, 0.5f, seed + 61u);
            u32 scol = color_scale(cfg->stone_color, 0.80f + 0.35f * bump);
            out->px[y * w + x] = color_lerp(cfg->crack_color, scol, edge);
        }
    }
}

void procgen_metal(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcMetalParams* p) {
    ProcMetalParams def = {
        .grid_x = 2, .grid_y = 2, .bevel_width = 0.06f, .brush_strength = 0.12f,
        .base_color = FB_RGBA(156, 166, 178, 255)
    };
    const ProcMetalParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 gx = MAX(cfg->grid_x, 1);
    i32 gy = MAX(cfg->grid_y, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 pu = u * (f32)gx;
            f32 pv = v * (f32)gy;
            f32 fu = pu - m_floor(pu);
            f32 fv = pv - m_floor(pv);

            /* Yatay fırçalanmış metal izi */
            f32 brush = noise_perlin2_tile(u * 4.0f, v * 64.0f, 4, 64, seed) * cfg->brush_strength;

            /* Panel pah/derz (bevel) */
            f32 bw = cfg->bevel_width;
            f32 shade = 1.0f + brush;
            if (fu < bw || fv < bw) {
                shade *= 1.18f; /* üst/sol parlama */
            }
            if (fu > 1.0f - bw || fv > 1.0f - bw) {
                shade *= 0.68f; /* alt/sağ gölge */
            }
            if (fu < bw * 0.35f || fu > 1.0f - bw * 0.35f ||
                fv < bw * 0.35f || fv > 1.0f - bw * 0.35f) {
                shade *= 0.45f; /* derz oluğu */
            }

            /* Köşe perçinleri (rivets) */
            f32 r_off = bw * 1.8f;
            f32 rx = (fu < 0.5f) ? (fu - r_off) : (fu - (1.0f - r_off));
            f32 ry = (fv < 0.5f) ? (fv - r_off) : (fv - (1.0f - r_off));
            f32 r_dist = m_sqrt(rx * rx + ry * ry);
            if (r_dist < bw * 0.55f) {
                shade = (rx + ry < 0.0f) ? 1.25f : 0.60f;
            }

            out->px[y * w + x] = color_scale(cfg->base_color, shade);
        }
    }
}

void procgen_lava(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcLavaParams* p) {
    ProcLavaParams def = {
        .cells = 5, .glow_width = 0.28f,
        .crust_color = FB_RGBA(32, 26, 28, 255),
        .lava_color  = FB_RGBA(255, 110, 15, 255)
    };
    const ProcLavaParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 cells = MAX(cfg->cells, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 wx = noise_fbm2_tile(u * (f32)cells, v * (f32)cells, cells, cells, 3, 2.0f, 0.5f, seed + 7u) * 0.25f;
            f32 wy = noise_fbm2_tile(u * (f32)cells, v * (f32)cells, cells, cells, 3, 2.0f, 0.5f, seed + 19u) * 0.25f;

            f32 f2 = 0.0f;
            f32 f1 = noise_worley2_tile((u + wx) * (f32)cells, (v + wy) * (f32)cells,
                                        cells, cells, seed, &f2);
            f32 crack = 1.0f - smoothstep(0.0f, cfg->glow_width, f2 - f1);

            u32 core_yellow = FB_RGBA(255, 240, 120, 255);
            u32 c_crust = color_scale(cfg->crust_color, 0.7f + 0.5f * f1);
            u32 c_mid   = color_lerp(c_crust, cfg->lava_color, smoothstep(0.0f, 0.65f, crack));
            out->px[y * w + x] = color_lerp(c_mid, core_yellow, smoothstep(0.65f, 1.0f, crack));
        }
    }
}

void procgen_caustic(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcCausticParams* p) {
    ProcCausticParams def = {
        .cells = 5, .warp = 0.35f, .sharpness = 3.2f,
        .deep_color      = FB_RGBA(14, 78, 138, 255),
        .highlight_color = FB_RGBA(185, 248, 255, 255)
    };
    const ProcCausticParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 cells = MAX(cfg->cells, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 wx = noise_perlin2_tile(u * (f32)cells, v * (f32)cells, cells, cells, seed + 3u) * cfg->warp;
            f32 wy = noise_perlin2_tile(u * (f32)cells, v * (f32)cells, cells, cells, seed + 9u) * cfg->warp;

            f32 w1 = noise_worley2_tile(u * (f32)cells + wx, v * (f32)cells + wy, cells, cells, seed + 21u, NULL);
            f32 w2 = noise_worley2_tile(u * (f32)cells - wy, v * (f32)cells + wx, cells, cells, seed + 43u, NULL);

            f32 c = m_pow(m_clamp((w1 + w2) * 0.65f, 0.0f, 1.0f), cfg->sharpness);
            out->px[y * w + x] = color_lerp(cfg->deep_color, cfg->highlight_color, c);
        }
    }
}

void procgen_cloud(Texture* out, Arena* arena, u32 w, u32 h, u32 seed, const ProcCloudParams* p) {
    ProcCloudParams def = {
        .freq = 4, .coverage = 0.52f, .softness = 0.38f,
        .sky_color   = FB_RGBA(78, 146, 224, 255),
        .cloud_color = FB_RGBA(248, 250, 255, 255)
    };
    const ProcCloudParams* cfg = p ? p : &def;
    img_tex_init(out, arena, w, h, TEX_FLAG_BILINEAR);

    i32 freq = MAX(cfg->freq, 1);

    for (u32 y = 0; y < h; y++) {
        f32 v = (f32)y / (f32)h;
        for (u32 x = 0; x < w; x++) {
            f32 u = (f32)x / (f32)w;

            f32 n = 0.5f + 0.5f * noise_warp2_tile(u * (f32)freq, v * (f32)freq,
                                                   freq, freq, 0.6f, 5, seed);
            f32 lo = (1.0f - cfg->coverage) - cfg->softness * 0.5f;
            f32 hi = (1.0f - cfg->coverage) + cfg->softness * 0.5f;
            f32 density = smoothstep(lo, hi, n);

            out->px[y * w + x] = color_lerp(cfg->sky_color, cfg->cloud_color, density);
        }
    }
}
