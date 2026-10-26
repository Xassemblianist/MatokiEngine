/* app/plat_art.c — Platform oyunu pixel-art üreteci. Her karo 16×16 piksel, el ile kodlanmış
 * şekil fonksiyonları + PCG32 benekleri ile çizilir. Alfa 0 = saydam (sprite.mtk discard eder).
 * Arka plan tepeleri tam sayı frekanslı sinüs toplamıdır → 256 px'te kusursuz döşenir.
 */
#include "app/plat_art.h"
#include "core/math.h"
#include "core/rng.h"
#include "raster/texture.h"
#include "raster/framebuffer.h"

#define T 16

static Texture* g_at;
static Rng      g_rng;

static void put(i32 ox, i32 oy, i32 x, i32 y, u32 c) {
    if (x < 0 || y < 0 || x >= T || y >= T) return;
    g_at->px[(u32)(oy + y) * g_at->w + (u32)(ox + x)] = c;
}

static void fill(i32 ox, i32 oy, i32 x0, i32 y0, i32 x1, i32 y1, u32 c) {
    for (i32 y = y0; y <= y1; y++)
        for (i32 x = x0; x <= x1; x++) put(ox, oy, x, y, c);
}

static void origin(u32 idx, i32* ox, i32* oy) {
    *ox = (i32)(idx % 16u) * T;
    *oy = (i32)(idx / 16u) * T;
}

static u32 shade(u32 c, i32 d) {
    i32 r = (i32)(c & 0xFF) + d, g = (i32)((c >> 8) & 0xFF) + d, b = (i32)((c >> 16) & 0xFF) + d;
    return FB_RGBA(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255), c >> 24);
}

static void speckle(i32 ox, i32 oy, i32 y0, u32 base, i32 amount) {
    for (i32 y = y0; y < T; y++)
        for (i32 x = 0; x < T; x++) {
            i32 d = (i32)(rng_u32(&g_rng) % 3u) - 1;
            put(ox, oy, x, y, shade(base, d * amount));
        }
}

static void tile_dirt(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    speckle(ox, oy, 0, FB_RGBA(122, 80, 48, 255), 10);
    for (u32 k = 0; k < 6; k++) {   /* küçük taşlar */
        i32 x = (i32)(rng_u32(&g_rng) % 14u) + 1, y = (i32)(rng_u32(&g_rng) % 14u) + 1;
        put(ox, oy, x, y, FB_RGBA(150, 112, 78, 255));
        put(ox, oy, x + 1, y, FB_RGBA(96, 62, 38, 255));
    }
}

static void tile_grass(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    tile_dirt(idx);
    for (i32 x = 0; x < T; x++) {
        i32 depth = 3 + (i32)((x * 7 + 3) % 5 == 0) + (i32)(x % 4 == 1);
        for (i32 y = 0; y < depth; y++) {
            u32 g = (y == 0) ? FB_RGBA(130, 214, 86, 255) : FB_RGBA(78, 170, 60, 255);
            put(ox, oy, x, y, g);
        }
        put(ox, oy, x, depth, FB_RGBA(52, 120, 44, 255));
    }
}

static void tile_brick(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    u32 mortar = FB_RGBA(196, 182, 160, 255);
    for (i32 y = 0; y < T; y++)
        for (i32 x = 0; x < T; x++) {
            i32 row = y / 4, off = (row & 1) ? 4 : 0;
            b32 m = (y % 4 == 3) || ((x + off) % 8 == 7);
            u32 c = m ? mortar : shade(FB_RGBA(176, 72, 52, 255), (i32)(rng_u32(&g_rng) % 13u) - 6);
            if (!m && (y % 4 == 0)) c = shade(c, 18);
            put(ox, oy, x, y, c);
        }
}

static void tile_stone(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    speckle(ox, oy, 0, FB_RGBA(128, 132, 142, 255), 7);
    for (i32 i = 0; i < T; i++) {
        put(ox, oy, i, 0, FB_RGBA(186, 190, 200, 255));
        put(ox, oy, 0, i, FB_RGBA(170, 174, 184, 255));
        put(ox, oy, i, T - 1, FB_RGBA(72, 74, 84, 255));
        put(ox, oy, T - 1, i, FB_RGBA(86, 88, 98, 255));
    }
}

static void tile_platform(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    for (i32 x = 0; x < T; x++) {
        put(ox, oy, x, 0, FB_RGBA(214, 160, 96, 255));
        for (i32 y = 1; y < 5; y++) put(ox, oy, x, y, shade(FB_RGBA(168, 112, 62, 255), (x % 5 == 0) ? -30 : 0));
        put(ox, oy, x, 5, FB_RGBA(98, 62, 34, 255));
    }
    fill(ox, oy, 2, 6, 3, 8, FB_RGBA(98, 62, 34, 255));
    fill(ox, oy, 12, 6, 13, 8, FB_RGBA(98, 62, 34, 255));
}

static void tile_spikes(u32 idx) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    for (i32 k = 0; k < 4; k++) {
        for (i32 y = 0; y < 8; y++) {
            i32 half = y / 2;
            for (i32 x = 2 - half; x <= 1 + half; x++) {
                u32 c = (x <= 1) ? FB_RGBA(220, 224, 232, 255) : FB_RGBA(140, 144, 156, 255);
                put(ox, oy, k * 4 + x, 8 + y, c);
            }
        }
    }
}

static void tile_flag(u32 idx, b32 top) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    fill(ox, oy, 7, 0, 8, T - 1, FB_RGBA(230, 230, 236, 255));
    put(ox, oy, 8, 0, FB_RGBA(150, 150, 160, 255));
    if (top) {
        fill(ox, oy, 6, 0, 9, 1, FB_RGBA(250, 210, 60, 255));
        for (i32 y = 2; y < 10; y++) {
            i32 w = (y < 6) ? (y - 1) : (10 - y);
            for (i32 x = 0; x < w * 2; x++) put(ox, oy, 9 + x, y, (y & 1) ? FB_RGBA(232, 56, 64, 255) : FB_RGBA(250, 92, 92, 255));
        }
    }
}

static void coin(u32 idx, i32 half_w) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    for (i32 y = 2; y < 14; y++) {
        f32 dy = ((f32)y - 7.5f) / 6.0f;
        i32 hw = (i32)((f32)half_w * m_sqrt(m_max(0.0f, 1.0f - dy * dy)) + 0.5f);
        for (i32 x = 8 - hw; x < 8 + hw; x++) {
            u32 c = FB_RGBA(246, 196, 48, 255);
            if (x == 8 - hw || y == 2) c = FB_RGBA(255, 240, 150, 255);
            if (x == 8 + hw - 1 || y == 13) c = FB_RGBA(184, 128, 24, 255);
            put(ox, oy, x, y, c);
        }
    }
    if (half_w >= 4) fill(ox, oy, 7, 5, 8, 10, FB_RGBA(214, 152, 30, 255));
}

/* Oyuncu: leg = bacak pozu (-2..2), bob = gövde kayması, arms_up, hurt */
static void player(u32 idx, i32 leg, i32 bob, b32 arms_up, b32 hurt) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    u32 skin  = hurt ? FB_RGBA(255, 150, 150, 255) : FB_RGBA(250, 206, 164, 255);
    u32 hair  = FB_RGBA(70, 44, 30, 255);
    u32 shirt = hurt ? FB_RGBA(220, 70, 70, 255) : FB_RGBA(66, 120, 220, 255);
    u32 pants = FB_RGBA(46, 50, 86, 255);
    u32 shoe  = FB_RGBA(40, 30, 26, 255);
    i32 b = bob;
    /* kafa */
    fill(ox, oy, 5, 1 + b, 10, 6 + b, skin);
    fill(ox, oy, 5, 1 + b, 10, 2 + b, hair);
    put(ox, oy, 4, 3 + b, hair);
    put(ox, oy, 9, 4 + b, hurt ? FB_RGBA(120, 0, 0, 255) : FB_RGBA(20, 20, 30, 255));
    put(ox, oy, 7, 4 + b, hurt ? FB_RGBA(120, 0, 0, 255) : FB_RGBA(20, 20, 30, 255));
    /* gövde */
    fill(ox, oy, 5, 7 + b, 10, 11, shirt);
    put(ox, oy, 5, 7 + b, shade(shirt, 30));
    /* kollar */
    if (arms_up) {
        fill(ox, oy, 3, 4 + b, 4, 7 + b, skin);
        fill(ox, oy, 11, 4 + b, 12, 7 + b, skin);
    } else {
        fill(ox, oy, 4, 8 + b, 4, 10 + b, skin);
        fill(ox, oy, 11, 8 + b, 11, 10 + b, skin);
    }
    /* bacaklar */
    fill(ox, oy, 5, 12, 10, 12, pants);
    i32 l0 = 6 - leg, l1 = 9 + leg;
    fill(ox, oy, l0, 13, l0 + 1, 14, pants);
    fill(ox, oy, l1 - 1, 13, l1, 14, pants);
    fill(ox, oy, l0 - 1, 15, l0 + 1, 15, shoe);
    fill(ox, oy, l1 - 1, 15, l1 + 1, 15, shoe);
}

static void slime(u32 idx, i32 squash, b32 flat) {
    i32 ox, oy;
    origin(idx, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    u32 body = FB_RGBA(120, 210, 90, 255);
    if (flat) {
        fill(ox, oy, 1, 12, 14, 15, body);
        fill(ox, oy, 2, 12, 13, 12, shade(body, 40));
        return;
    }
    i32 top = 5 + squash;
    for (i32 y = top; y < 16; y++) {
        f32 t = (f32)(y - top) / (f32)(16 - top);
        i32 hw = (i32)(3.0f + 4.5f * m_sqrt(t) + (f32)squash * 0.5f);
        for (i32 x = 8 - hw; x < 8 + hw; x++) put(ox, oy, x, y, (y == top) ? shade(body, 40) : body);
    }
    fill(ox, oy, 5, top + 3, 6, top + 4, FB_RGBA(255, 255, 255, 255));
    fill(ox, oy, 9, top + 3, 10, top + 4, FB_RGBA(255, 255, 255, 255));
    put(ox, oy, 6, top + 4, FB_RGBA(20, 30, 20, 255));
    put(ox, oy, 10, top + 4, FB_RGBA(20, 30, 20, 255));
}

static void decor(void) {
    i32 ox, oy;
    origin(ART_BUSH, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    for (i32 y = 6; y < 16; y++)
        for (i32 x = 0; x < 16; x++) {
            f32 dx = ((f32)x - 7.5f) / 8.0f, dy = ((f32)y - 15.0f) / 9.0f;
            if (dx * dx + dy * dy < 1.0f) put(ox, oy, x, y, shade(FB_RGBA(60, 150, 70, 255), (i32)(rng_u32(&g_rng) % 21u) - 10));
        }
    origin(ART_FLOWER, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    fill(ox, oy, 7, 9, 7, 15, FB_RGBA(60, 140, 60, 255));
    fill(ox, oy, 6, 6, 8, 8, FB_RGBA(250, 120, 170, 255));
    put(ox, oy, 7, 7, FB_RGBA(255, 230, 80, 255));
    origin(ART_TUFT, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, 0);
    for (i32 k = 0; k < 5; k++) fill(ox, oy, 3 + k * 2, 11 + (k & 1) * 2, 3 + k * 2, 15, FB_RGBA(90, 180, 70, 255));
    origin(ART_WHITE, &ox, &oy);
    fill(ox, oy, 0, 0, T - 1, T - 1, FB_RGBA(255, 255, 255, 255));
}

static void hills(Texture* t, Arena* perm, u32 top_col, u32 bot_col, f32 seed) {
    tex_init(t, perm, 256, 64, 0);
    for (u32 x = 0; x < 256; x++) {
        f32 u = (f32)x / 256.0f * M_TAU;
        f32 h = 0.45f + 0.18f * m_sin(u * 2.0f + seed) + 0.10f * m_sin(u * 5.0f + seed * 2.3f) +
                0.05f * m_sin(u * 11.0f + seed * 0.7f);
        i32 hy = (i32)(64.0f * (1.0f - h));
        for (u32 y = 0; y < 64; y++) {
            u32 c = 0;
            if ((i32)y >= hy) {
                f32 k = (f32)((i32)y - hy) / 24.0f;
                k = m_min(k, 1.0f);
                u32 r = (u32)m_lerp((f32)(top_col & 0xFF), (f32)(bot_col & 0xFF), k);
                u32 g = (u32)m_lerp((f32)((top_col >> 8) & 0xFF), (f32)((bot_col >> 8) & 0xFF), k);
                u32 b = (u32)m_lerp((f32)((top_col >> 16) & 0xFF), (f32)((bot_col >> 16) & 0xFF), k);
                c = FB_RGBA(r, g, b, 255);
            }
            t->px[y * 256 + x] = c;
        }
    }
}

void plat_art_build(PlatArt* art, Arena* perm, u64 seed) {
    rng_seed(&g_rng, seed);
    tex_init(&art->atlas, perm, 256, 256, 0);
    g_at = &art->atlas;
    for (u32 i = 0; i < 256u * 256u; i++) g_at->px[i] = 0;

    tile_grass(ART_GRASS);
    tile_dirt(ART_DIRT);
    tile_brick(ART_BRICK);
    tile_stone(ART_STONE);
    tile_platform(ART_PLATFORM);
    tile_spikes(ART_SPIKES);
    tile_flag(ART_FLAG_POLE, FALSE);
    tile_flag(ART_FLAG_TOP, TRUE);
    static const i32 coin_w[4] = { 6, 4, 1, 4 };
    for (u32 k = 0; k < 4; k++) coin(ART_COIN0 + k, coin_w[k]);
    player(ART_PLAYER_IDLE0, 0, 0, FALSE, FALSE);
    player(ART_PLAYER_IDLE0 + 1, 0, 1, FALSE, FALSE);
    static const i32 run_leg[4] = { 2, 0, -2, 0 };
    static const i32 run_bob[4] = { 0, 1, 0, 1 };
    for (u32 k = 0; k < 4; k++) player(ART_PLAYER_RUN0 + k, run_leg[k], run_bob[k], FALSE, FALSE);
    player(ART_PLAYER_JUMP, 1, 0, TRUE, FALSE);
    player(ART_PLAYER_FALL, -1, 0, TRUE, FALSE);
    player(ART_PLAYER_HURT, 0, 0, TRUE, TRUE);
    slime(ART_SLIME0, 0, FALSE);
    slime(ART_SLIME0 + 1, 2, FALSE);
    slime(ART_SLIME_FLAT, 0, TRUE);
    decor();

    tex_init(&art->sky, perm, 4, 64, 0);
    for (u32 y = 0; y < 64; y++) {
        f32 k = (f32)y / 63.0f;
        u32 c = FB_RGBA((u32)m_lerp(70.0f, 176.0f, k), (u32)m_lerp(120.0f, 216.0f, k), (u32)m_lerp(210.0f, 242.0f, k), 255);
        for (u32 x = 0; x < 4; x++) art->sky.px[y * 4 + x] = c;
    }
    hills(&art->hills_far, perm, FB_RGBA(150, 176, 214, 255), FB_RGBA(126, 150, 196, 255), 1.3f);
    hills(&art->hills_near, perm, FB_RGBA(96, 170, 110, 255), FB_RGBA(60, 120, 80, 255), 4.1f);
}
