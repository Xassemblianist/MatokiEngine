/* thread/thread.c — Thread yığınları, TLS kurulumu ve JIT çoğaltma el sıkışması.
 * Wasm32'de her worker için paylaşımlı bellekte ayrı 1 MB yığın ve 256 bayt TLS bloğu
 * barındırır. Host derlemesinde tek thread (tid=0, count=1) olarak çalışır.
 */
#include "thread/thread.h"
#include "thread/atomic.h"
#include "thread/job.h"
#include "core/log.h"

/* Worker yığınları: tid = 1..THREAD_MAX_WORKERS için 1'er MB (toplam 8 MB).
 * Ana thread (tid = 0) wasm-ld'nin --stack-first ile ayırdığı ilk 1 MB yığını kullanır.
 */
static _Alignas(16) u8 g_worker_stacks[THREAD_MAX_WORKERS][THREAD_STACK_SIZE];

#if defined(__wasm_atomics__)
/* Her thread için 16-bayt hizalı TLS alanı (tid = 0..THREAD_MAX_WORKERS). */
static _Alignas(16) u8 g_thread_tls[THREAD_MAX_COUNT][THREAD_TLS_SIZE];
#endif

/* Paylaşımlı durum değişkenleri */
static u32 g_target_workers     = 0;
static u32 g_has_target_workers = 0;
static u32 g_ready_workers      = 0;

/* JIT çoğaltma onay sayaçları: worker idx'inci fonksiyonu kendi tablosuna koyduğunda
 * Atomics.add ile g_jit_ack[idx] değerini 1 artırır.
 */
static _Alignas(16) u32 g_jit_ack[JIT_MAX_FUNCS];

#if defined(__wasm_atomics__)
extern void __wasm_init_tls(void* tls_block);
static _Thread_local u32 tls_thread_id = 0;
#else
static u32 tls_thread_id = 0;
#endif

u32 thread_id(void) {
    return tls_thread_id;
}

u32 thread_spawned_worker_count(void) {
    return atomic_load_acquire_u32(&g_ready_workers);
}

u32 thread_worker_count(void) {
    if (atomic_load_acquire_u32(&g_has_target_workers)) {
        return atomic_load_acquire_u32(&g_target_workers);
    }
    return atomic_load_acquire_u32(&g_ready_workers);
}

u32 thread_count(void) {
    return 1u + thread_worker_count();
}

EXPORT(engine_thread_stack_top) u32 engine_thread_stack_top(u32 tid) {
    if (tid == 0 || tid > THREAD_MAX_WORKERS) {
        return 0;
    }
    uptr top = (uptr)&g_worker_stacks[tid - 1u][THREAD_STACK_SIZE];
    return (u32)(top & ~(uptr)15u);
}

/* Yalnızca __stack_pointer ve __tls_base yazmaçlarını değiştiren çıplak yardımcı.
 * engine_thread_init içinde varargs (LOG) veya adreslenen yerel değişken bulunmadığından
 * clang yığın çerçevesi (prologue/epilogue) üretmez ve yeni __stack_pointer korunur.
 */
EXPORT(engine_thread_init) void engine_thread_init(u32 tid, u32 stack_top) {
    if (tid > THREAD_MAX_WORKERS) {
        return;
    }

    if (tid == 0) {
#if defined(__wasm_atomics__)
        u32 tls_ptr = (u32)(uptr)&g_thread_tls[0][0];
        __wasm_init_tls((void*)(uptr)tls_ptr);
#endif
        tls_thread_id = 0;
        return;
    }

    if (stack_top == 0) {
        stack_top = engine_thread_stack_top(tid);
    }
    stack_top &= ~15u;

#if defined(__wasm_atomics__)
    u32 tls_ptr = (u32)(uptr)&g_thread_tls[tid][0];
    __wasm_init_tls((void*)(uptr)tls_ptr);
    tls_thread_id = tid;
    atomic_add_u32(&g_ready_workers, 1u);
    __asm__ volatile(
        ".globaltype __stack_pointer, i32\n"
        "local.get %0\n"
        "global.set __stack_pointer\n"
        :
        : "r"(stack_top));
#else
    UNUSED(stack_top);
    tls_thread_id = tid;
    atomic_add_u32(&g_ready_workers, 1u);
#endif
}

EXPORT(engine_set_worker_count) void engine_set_worker_count(u32 num_workers) {
    if (num_workers > THREAD_MAX_WORKERS) {
        num_workers = THREAD_MAX_WORKERS;
    }
    atomic_store_release_u32(&g_target_workers, num_workers);
    atomic_store_release_u32(&g_has_target_workers, 1u);
}

EXPORT(engine_reset_ready_workers) void engine_reset_ready_workers(void) {
    atomic_store_release_u32(&g_ready_workers, 0u);
}

EXPORT(engine_jit_ack_buf) u32* engine_jit_ack_buf(void) {
    return g_jit_ack;
}

b32 thread_wait_jit_ack(u32 idx) {
    if (idx >= JIT_MAX_FUNCS) {
        LOG("[thread] thread_wait_jit_ack: idx=%u out of range (max=%u)", idx, JIT_MAX_FUNCS);
        return FALSE;
    }
    /* Havuzda başlatılmış tüm worker'ların (aktif T=1 olsa bile sonradan T=8'e geçildiğinde
     * tablosunun güncel olması için) ack vermesini bekle. */
    u32 workers = thread_spawned_worker_count();
    if (workers == 0) {
        workers = thread_worker_count();
    }
    if (workers == 0) {
        return TRUE;
    }

    /* Uyuyan worker'ları futex'ten anında uyandır ki 2 ms timeout beklemeden JS postMessage'ı işlesinler */
    job_wake_for_sync();

    f64 t0 = plat_time_ms();
    const f64 timeout_ms = 2000.0;
    u32 spins = 0;
    while (atomic_load_acquire_u32(&g_jit_ack[idx]) < workers) {
        atomic_spin_pause();
        spins++;
        if ((spins & 1023u) == 0) {
            job_wake_for_sync();
            f64 elapsed = plat_time_ms() - t0;
            if (elapsed >= timeout_ms) {
                u32 got = atomic_load_acquire_u32(&g_jit_ack[idx]);
                LOG("[thread] JIT ack timeout idx=%u (beklenen=%u, gelen=%u, %.1f ms)",
                    idx, workers, got, elapsed);
                return FALSE;
            }
        }
    }
    return TRUE;
}
