/* tests/wasm/phys2_bench.c — phys2 adım süresi ölçümü (wasm simd128 + gerçek thread; test_phys2_wasm.mjs bench).
 * bench_rigid: kutu yığını / piramit (oturmuş adım medyanı, dar faz ve çözücü ayrı), bench_cloth: n×n kumaş.
 * Sahneler SamurayBalgami/Matoki'nin phys_bench.c'sinden uyarlandı (izinle). */
#include "core/types.h"
#include "core/arena.h"
#include "core/log.h"
#include "plat/platform.h"
#include "thread/thread.h"
#include "phys2/phys.h"
#include "phys2/soft.h"

static _Alignas(64) u8 g_mem[MB(400)];
static Arena g_arena;
static f64 g_res[16];
static f64 g_times[1024];

static u32 true_count(u32 L) { return L * (L + 1) * (2 * L + 1) / 6; }

static void sort_f64(f64* v, u32 n) {
    for (u32 i = 1; i < n; i++) { f64 x = v[i]; u32 j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
}
static f64 median(f64* v, u32 n) { sort_f64(v, n); return v[n / 2]; }

static void build_scene(Phys2World* w, i32 scene, i32 n) {
    Phys2BodyDesc g = phys2_desc_plane(vec3(0, 1, 0), 0.0f);
    phys2_add_body(w, &g);
    Phys2Rng r = phys2_rng_make(12345);
    if (scene == 0) {   /* düşen kutu yığını (ızgara, hafif döndürülmüş) */
        u32 side = 1;
        while (side * side * side < (u32)n) side++;
        u32 k = 0;
        for (u32 y = 0; y < side && k < (u32)n; y++)
            for (u32 z = 0; z < side && k < (u32)n; z++)
                for (u32 x = 0; x < side && k < (u32)n; x++, k++) {
                    Vec3 p = vec3(((f32)x - (f32)side * 0.5f) * 1.3f + phys2_rng_range(&r, -0.1f, 0.1f), 0.7f + (f32)y * 1.3f,
                                  ((f32)z - (f32)side * 0.5f) * 1.3f + phys2_rng_range(&r, -0.1f, 0.1f));
                    Phys2BodyDesc b = phys2_desc_box(p, vec3(0.5f, 0.5f, 0.5f), 1.0f);
                    b.rot = phys2_quat_from_axis_angle(vec3(phys2_rng_range(&r, -1, 1), 1.0f, phys2_rng_range(&r, -1, 1)), phys2_rng_range(&r, -0.4f, 0.4f));
                    phys2_add_body(w, &b);
                }
    } else {            /* 3B basamaklı piramit */
        u32 levels = 1;
        while (true_count(levels + 1) <= (u32)n) levels++;
        for (u32 l = 0; l < levels; l++) {
            u32 cnt = levels - l;
            for (u32 iz = 0; iz < cnt; iz++)
                for (u32 ix = 0; ix < cnt; ix++) {
                    f32 x = ((f32)ix - (f32)(cnt - 1) * 0.5f) * 1.02f, z = ((f32)iz - (f32)(cnt - 1) * 0.5f) * 1.02f;
                    Phys2BodyDesc b = phys2_desc_box(vec3(x, 0.5f + (f32)l, z), vec3(0.5f, 0.5f, 0.5f), 1.0f);
                    phys2_add_body(w, &b);
                }
        }
    }
}

/* scene 0 = yığın (n kutu), 1 = piramit; mt: 0 = 1T, 1 = çok thread (iş sistemindeki worker'lar). Sonuçlar bench_get ile:
 * 0 etkin evre ort. ms (ilk 'warm' adım), 1 oturmuş medyan ms (sonraki 'steps'), 2 dar faz medyan, 3 çözücü medyan, 4 manifold, 5 çift, 6 hash */
EXPORT(bench_rigid) f64 bench_rigid(i32 scene, i32 n, i32 mt, i32 warm, i32 steps, i32 simd) {
    arena_init(&g_arena, g_mem, sizeof(g_mem));
    Phys2World* w = ARENA_PUSH_STRUCT(&g_arena, Phys2World);
    phys2_world_init(w, &g_arena, (u32)n + 64, (u32)n * 12 + 64);
    w->threads = mt ? 0u : 1u;
    w->simd = simd != 0;
    build_scene(w, scene, n);
    f64 act = 0;
    for (i32 s = 0; s < warm; s++) { phys2_step(w, 1.0f / 60.0f); act += w->stats.step_ms; }
    f64 col[512], sol[512];
    for (i32 s = 0; s < steps; s++) {
        phys2_step(w, 1.0f / 60.0f);
        g_times[s] = w->stats.step_ms;
        col[s] = w->stats.collide_ms;
        sol[s] = w->stats.solve_ms;
    }
    g_res[0] = act / (warm ? warm : 1);
    g_res[1] = median(g_times, (u32)steps);
    g_res[2] = median(col, (u32)steps);
    g_res[3] = median(sol, (u32)steps);
    g_res[4] = w->stats.manifolds;
    g_res[5] = w->stats.pairs;
    g_res[6] = phys2_state_hash(w);
    return g_res[1];
}

/* kumaş n×n, colored: 0 dizi sırası, 1 renk sırası; mt; selfc: öz-çarpışma */
EXPORT(bench_cloth) f64 bench_cloth(i32 n, i32 colored, i32 mt, i32 steps, i32 selfc) {
    arena_init(&g_arena, g_mem, sizeof(g_mem));
    Phys2SoftBody* c = phys2_soft_cloth(&g_arena, vec3(0, 10, 0), vec3(8, 0, 0), vec3(0, 0, 8), (u32)n, (u32)n, 4.0f, 0.0f, 1e-4f, 0.02f);
    for (i32 i = 0; i < n; i++) phys2_soft_pin(c, (u32)i);
    c->self_collide = selfc != 0;
    c->tri_collide = FALSE;
    phys2_soft_set_colored(&g_arena, c, colored != 0);
    Phys2SoftWorld* sw = ARENA_PUSH_STRUCT(&g_arena, Phys2SoftWorld);
    phys2_soft_world_init(sw, &g_arena, 2);
    sw->threads = mt ? 0u : 1u;
    phys2_soft_world_add(sw, c);
    for (i32 s = 0; s < 4; s++) phys2_soft_world_step(sw, NULL, 1.0f / 60.0f);
    for (i32 s = 0; s < steps; s++) { phys2_soft_world_step(sw, NULL, 1.0f / 60.0f); g_times[s] = sw->step_ms; }
    g_res[0] = median(g_times, (u32)steps);
    g_res[6] = phys2_soft_hash(sw);
    return g_res[0];
}

EXPORT(bench_get) f64 bench_get(i32 k) { return g_res[k]; }
