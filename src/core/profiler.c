/* core/profiler.c — iç içe bölge zamanlayıcısı ve çoklu-thread güvenli tablo raporlayıcısı.
 * Dinamik bellek ayırmaz; thread başına sabit boyutlu bölge ve yığın dizileri kullanır.
 * Bölge eşlemesi önce işaretçi eşitliği, sonra içerik karşılaştırması ile yapılır;
 * frame sonunda tüm aktif thread'lerin çağrı sayıları ve duvar saati süreleri birleştirilir.
 */
#include "core/profiler.h"
#include "core/log.h"
#include "plat/platform.h"
#include "thread/thread.h"
#include "thread/atomic.h"

typedef struct {
    u32 zone_idx;
    f64 start_ms;
} ProfStackEntry;

typedef struct {
    f64 cur_ms;
    u32 cur_calls;
} ProfTlsZone;

static ProfZone       g_zones[PROF_MAX_ZONES];
static u32            g_zone_count = 0;
static u32            g_prof_lock  = 0;
static u32            g_frame_count = 0;

static ProfStackEntry g_tls_stack[THREAD_MAX_COUNT][PROF_MAX_STACK];
static u32            g_tls_stack_top[THREAD_MAX_COUNT];
static ProfTlsZone    g_tls_zones[THREAD_MAX_COUNT][PROF_MAX_ZONES];

static b32 str_eq(const char* a, const char* b) {
    if (a == b) return TRUE;
    if (!a || !b) return FALSE;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static u32 find_or_create_zone(const char* name) {
    u32 count = atomic_load_acquire_u32(&g_zone_count);
    for (u32 i = 0; i < count; i++) {
        if (str_eq(g_zones[i].name, name)) {
            return i;
        }
    }

    while (!atomic_cas_u32(&g_prof_lock, 0u, 1u)) {
        atomic_spin_pause();
    }

    count = atomic_load_relaxed_u32(&g_zone_count);
    for (u32 i = 0; i < count; i++) {
        if (str_eq(g_zones[i].name, name)) {
            atomic_store_release_u32(&g_prof_lock, 0u);
            return i;
        }
    }

    if (count >= PROF_MAX_ZONES) {
        atomic_store_release_u32(&g_prof_lock, 0u);
        return U32_MAX;
    }

    u32 idx = count;
    g_zones[idx].name = name;
    g_zones[idx].cur_ms = 0.0;
    g_zones[idx].cur_calls = 0;
    g_zones[idx].last_ms = 0.0;
    g_zones[idx].last_calls = 0;
    g_zones[idx].total_ms = 0.0;
    g_zones[idx].max_ms = 0.0;
    g_zones[idx].total_calls = 0;
    for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
        g_tls_zones[t][idx].cur_ms = 0.0;
        g_tls_zones[t][idx].cur_calls = 0;
    }

    atomic_store_release_u32(&g_zone_count, idx + 1u);
    atomic_store_release_u32(&g_prof_lock, 0u);
    return idx;
}

void prof_reset(void) {
    while (!atomic_cas_u32(&g_prof_lock, 0u, 1u)) {
        atomic_spin_pause();
    }
    atomic_store_relaxed_u32(&g_zone_count, 0u);
    g_frame_count = 0;
    for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
        g_tls_stack_top[t] = 0;
        for (u32 i = 0; i < PROF_MAX_ZONES; i++) {
            g_tls_zones[t][i].cur_ms = 0.0;
            g_tls_zones[t][i].cur_calls = 0;
        }
    }
    atomic_store_release_u32(&g_prof_lock, 0u);
}

void prof_begin(const char* name) {
    u32 tid = thread_id();
    if (tid >= THREAD_MAX_COUNT) return;
    u32 top = g_tls_stack_top[tid];
    ASSERT(top < PROF_MAX_STACK);
    u32 idx = find_or_create_zone(name);
    g_tls_stack[tid][top].zone_idx = idx;
    g_tls_stack[tid][top].start_ms = plat_time_ms();
    g_tls_stack_top[tid] = top + 1u;
}

void prof_end(void) {
    f64 end_ms = plat_time_ms();
    u32 tid = thread_id();
    if (tid >= THREAD_MAX_COUNT) return;
    u32 top = g_tls_stack_top[tid];
    if (top == 0u) return;
    top--;
    g_tls_stack_top[tid] = top;
    u32 idx = g_tls_stack[tid][top].zone_idx;
    if (idx < atomic_load_acquire_u32(&g_zone_count)) {
        f64 dt = end_ms - g_tls_stack[tid][top].start_ms;
        if (dt < 0.0) dt = 0.0;
        g_tls_zones[tid][idx].cur_ms += dt;
        g_tls_zones[tid][idx].cur_calls += 1u;
        if (tid == 0u) {
            g_zones[idx].cur_ms = g_tls_zones[0][idx].cur_ms;
            g_zones[idx].cur_calls = g_tls_zones[0][idx].cur_calls;
        }
    }
}

void prof_frame_end(void) {
    /* Ana thread'de açık kalan bölgeleri kapat */
    while (g_tls_stack_top[0] > 0) {
        prof_end();
    }
    for (u32 t = 1; t < THREAD_MAX_COUNT; t++) {
        g_tls_stack_top[t] = 0;
    }

    g_frame_count++;
    u32 count = atomic_load_acquire_u32(&g_zone_count);
    for (u32 i = 0; i < count; i++) {
        ProfZone* z = &g_zones[i];
        f64 sum_ms = 0.0;
        u32 sum_calls = 0;
        u32 active_t = 0;
        for (u32 t = 0; t < THREAD_MAX_COUNT; t++) {
            u32 c = g_tls_zones[t][i].cur_calls;
            f64 m = g_tls_zones[t][i].cur_ms;
            g_tls_zones[t][i].cur_calls = 0;
            g_tls_zones[t][i].cur_ms = 0.0;
            if (c > 0u) {
                sum_calls += c;
                sum_ms += m;
                active_t++;
            }
        }
        /* Paralel bölgelerde (active_t > 1) katılan thread başına ortalama duvar saati süresi */
        f64 eff_ms = (active_t > 1u) ? (sum_ms / (f64)active_t) : sum_ms;
        z->last_ms = eff_ms;
        z->last_calls = sum_calls;
        z->total_ms += eff_ms;
        z->total_calls += sum_calls;
        if (eff_ms > z->max_ms) {
            z->max_ms = eff_ms;
        }
        z->cur_ms = 0.0;
        z->cur_calls = 0;
    }
}

void prof_report(void) {
    u32 frames = g_frame_count > 0 ? g_frame_count : 1;
    u32 count = atomic_load_acquire_u32(&g_zone_count);
    LOG("%-24s %10s %10s %10s %8s", "Bolge", "Son(ms)", "Ort(ms)", "Max(ms)", "Cagri");
    LOG("%-24s %10s %10s %10s %8s",
        "------------------------", "----------", "----------", "----------", "--------");
    for (u32 i = 0; i < count; i++) {
        const ProfZone* z = &g_zones[i];
        f64 last = (g_frame_count > 0) ? z->last_ms : z->cur_ms;
        u32 calls = (g_frame_count > 0) ? z->last_calls : z->cur_calls;
        f64 avg = (g_frame_count > 0) ? (z->total_ms / (f64)frames) : z->cur_ms;
        f64 maxv = (g_frame_count > 0) ? z->max_ms : z->cur_ms;
        LOG("%-24s %10.3f %10.3f %10.3f %8u", z->name, last, avg, maxv, calls);
    }
}

u32 prof_zone_count(void) {
    return atomic_load_acquire_u32(&g_zone_count);
}

const ProfZone* prof_zone_get(u32 idx) {
    if (idx >= atomic_load_acquire_u32(&g_zone_count)) return NULL;
    return &g_zones[idx];
}

u32 prof_frame_count(void) {
    return g_frame_count;
}

